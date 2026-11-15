/* SPDX-FileCopyrightText: 2026 SimpleCAD Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edcad
 *
 * Picking B-rep faces and edges of CAD parts in the 3D viewport, mapped to persistent names,
 * and the operators that use it to set feature references (e.g. the edges of a fillet).
 *
 * Picking works on the tessellated B-rep before Blender modifiers
 * (#bke::CADBrepMesh): a ray cast finds the face, and the closest B-rep edge on that face's
 * boundary, measured on screen, is the edge.
 */

#include "DNA_cad_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_screen_types.h"
#include "DNA_view3d_types.h"

#include "BLI_kdopbvh.hh"
#include "BLI_listbase_iterator.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "BLI_string.hh"
#include "BLI_vector.hh"

#include "BKE_attribute.hh"
#include "BKE_bvhutils.hh"
#include "BKE_cad.hh"
#include "BKE_context.hh"
#include "BKE_mesh.hh"
#include "BKE_library.hh"
#include "BKE_report.hh"
#include "BKE_screen.hh"

#include "BLT_translation.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"

#include "ED_screen.hh"
#include "ED_space_api.hh"
#include "ED_view3d.hh"

#include "GPU_immediate.hh"
#include "GPU_matrix.hh"
#include "GPU_state.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "UI_resources.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "cad_intern.hh"

namespace blender::ed::cad {

/* -------------------------------------------------------------------- */
/** \name Picking
 * \{ */

static const bke::CADBrepMesh *brep_mesh_get(const Object *ob_eval)
{
  if (ob_eval == nullptr || ob_eval->type != OB_CAD || ob_eval->data == nullptr) {
    return nullptr;
  }
  const CADPart *part = id_cast<const CADPart *>(ob_eval->data);
  if (part->runtime == nullptr || !part->runtime->brep_mesh || !part->runtime->brep_mesh->mesh) {
    return nullptr;
  }
  return part->runtime->brep_mesh.get();
}

/** Distance from `p` to segment `a`-`b` in 2D, and the segment parameter of the closest point. */
static float dist_to_segment_2d(const float2 &p, const float2 &a, const float2 &b, float &r_t)
{
  const float2 ab = b - a;
  const float len_sq = math::length_squared(ab);
  r_t = len_sq > 0.0f ? std::clamp(math::dot(p - a, ab) / len_sq, 0.0f, 1.0f) : 0.0f;
  return math::distance(p, a + ab * r_t);
}

std::optional<TopoPick> pick_topology(const ARegion *region,
                                      const View3D *v3d,
                                      Depsgraph *depsgraph,
                                      const Object *ob_eval,
                                      const float2 &mval,
                                      const PickType type)
{
  const bke::CADBrepMesh *brep = brep_mesh_get(ob_eval);
  if (brep == nullptr) {
    return std::nullopt;
  }
  const RegionView3D *rv3d = static_cast<const RegionView3D *>(region->regiondata);
  const Mesh &mesh = *brep->mesh;
  const Span<float3> positions = mesh.vert_positions();

  /* Ray in object space. */
  float3 ray_start, ray_dir;
  if (!ED_view3d_win_to_ray_clipped(
          depsgraph, region, v3d, mval, ray_start, ray_dir, true))
  {
    return std::nullopt;
  }
  const float4x4 &world_to_object = ob_eval->world_to_object();
  ray_start = math::transform_point(world_to_object, ray_start);
  ray_dir = math::normalize(math::transform_direction(world_to_object, ray_dir));

  const bke::AttributeAccessor attributes = mesh.attributes();
  const VArraySpan<int> cad_face = *attributes.lookup<int>(bke::CAD_ATTR_FACE,
                                                           bke::AttrDomain::Face);
  const VArraySpan<int> cad_body = *attributes.lookup<int>(bke::CAD_ATTR_BODY,
                                                           bke::AttrDomain::Face);
  if (cad_face.is_empty() || cad_body.is_empty()) {
    return std::nullopt;
  }

  TopoPick pick;
  bke::BVHTreeFromMesh tree = mesh.bvh_corner_tris();
  if (tree.tree) {
    BVHTreeRayHit hit;
    hit.index = -1;
    hit.dist = BVH_RAYCAST_DIST_MAX;
    BLI_bvhtree_ray_cast(
        tree.tree, ray_start, ray_dir, 0.0f, &hit, tree.raycast_callback, &tree);
    if (hit.index >= 0) {
      const int face = mesh.corner_tri_faces()[hit.index];
      pick.body = cad_body[face];
      pick.face = cad_face[face] - 1;
      pick.location = float3(hit.co);
    }
  }

  if (type == PickType::Face) {
    if (pick.face < 0) {
      return std::nullopt;
    }
    return pick;
  }

  /* Closest edge on screen: edges of the hit face, or of any body when the ray misses (the
   * cursor is just outside a silhouette edge). */
  const float4x4 project = ED_view3d_ob_project_mat_get(rv3d, ob_eval);
  const float threshold = 12.0f * UI_SCALE_FAC;
  float best_dist = threshold;
  for (const int b : brep->bodies.index_range()) {
    if (pick.body >= 0 && b != pick.body) {
      continue;
    }
    const bke::CADBrepBody &body = brep->bodies[b];
    for (const int e : body.edge_polylines.index_range()) {
      if (body.edge_is_seam(e)) {
        continue;
      }
      if (pick.face >= 0 && body.edge_faces[e][0] != pick.face &&
          body.edge_faces[e][1] != pick.face)
      {
        continue;
      }
      const Span<int> line = body.edge_polylines[e];
      for (int k = 0; k + 1 < line.size(); k++) {
        const float3 &a = positions[line[k]];
        const float3 &b3 = positions[line[k + 1]];
        const float2 sa = ED_view3d_project_float_v2_m4(region, a, project);
        const float2 sb = ED_view3d_project_float_v2_m4(region, b3, project);
        float t;
        const float dist = dist_to_segment_2d(mval, sa, sb, t);
        if (dist < best_dist) {
          best_dist = dist;
          pick.body = b;
          pick.edge = e;
          pick.location = math::interpolate(a, b3, t);
        }
      }
    }
  }
  if (pick.edge < 0) {
    return std::nullopt;
  }
  return pick;
}

std::optional<TopoPick> pick_topology(bContext *C,
                                      const Object *ob,
                                      const int2 &mval,
                                      const PickType type)
{
  ARegion *region = CTX_wm_region(C);
  View3D *v3d = CTX_wm_view3d(C);
  if (region == nullptr || v3d == nullptr || region->regiontype != RGN_TYPE_WINDOW) {
    return std::nullopt;
  }
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  const Object *ob_eval = DEG_get_evaluated(depsgraph, const_cast<Object *>(ob));
  std::optional<TopoPick> pick = pick_topology(
      region, v3d, depsgraph, ob_eval, float2(mval), type);
  if (pick) {
    pick->name = BKE_cad_topology_name(id_cast<const CADPart *>(ob_eval->data),
                                       pick->body,
                                       type == PickType::Edge,
                                       type == PickType::Edge ? pick->edge : pick->face);
    if (pick->name.empty()) {
      return std::nullopt;
    }
  }
  return pick;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Highlight Drawing
 * \{ */

static void draw_polyline(const uint pos, const Span<float3> positions, const Span<int> line)
{
  if (line.size() < 2) {
    return;
  }
  immBegin(GPU_PRIM_LINES, uint(line.size() - 1) * 2);
  for (int k = 0; k + 1 < line.size(); k++) {
    immVertex3fv(pos, positions[line[k]]);
    immVertex3fv(pos, positions[line[k + 1]]);
  }
  immEnd();
}

void draw_edge_highlights(const ARegion *region,
                          const Object *ob_eval,
                          const Span<std::string> selected_names,
                          const TopoPick *hover)
{
  const bke::CADBrepMesh *brep = brep_mesh_get(ob_eval);
  if (brep == nullptr) {
    return;
  }
  const CADPart *part = id_cast<const CADPart *>(ob_eval->data);
  const Span<float3> positions = brep->mesh->vert_positions();
  const RegionView3D *rv3d = static_cast<const RegionView3D *>(region->regiondata);

  GPU_matrix_push();
  GPU_matrix_push_projection();
  GPU_matrix_mul(ob_eval->object_to_world().ptr());
  /* Draw on top of the surface it lies on. */
  ED_view3d_polygon_offset(rv3d, 1.0f);
  GPU_depth_test(GPU_DEPTH_LESS_EQUAL);
  GPU_blend(GPU_BLEND_ALPHA);

  const uint pos = GPU_vertformat_attr_add(
      immVertexFormat(), "pos", gpu::VertAttrType::SFLOAT_32_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_POLYLINE_UNIFORM_COLOR);
  float viewport[4];
  GPU_viewport_size_get_f(viewport);
  immUniform2fv("viewportSize", &viewport[2]);

  /* Selected edges, resolved by name in the current result. */
  float select_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  ui::theme::get_color_3fv(TH_EDGE_SELECT, select_color);
  immUniformColor4fv(select_color);
  immUniform1f("lineWidth", 4.0f * U.pixelsize);
  for (const int b : brep->bodies.index_range()) {
    const bke::CADBrepBody &body = brep->bodies[b];
    for (const int e : body.edge_polylines.index_range()) {
      if (selected_names.is_empty()) {
        break;
      }
      const std::string name = BKE_cad_topology_name(part, b, true, e);
      if (selected_names.contains(name)) {
        draw_polyline(pos, positions, body.edge_polylines[e]);
      }
    }
  }

  if (hover && hover->edge >= 0 && hover->body < brep->bodies.size()) {
    const bke::CADBrepBody &body = brep->bodies[hover->body];
    if (hover->edge < body.edge_polylines.size()) {
      const float hover_color[4] = {0.35f, 0.78f, 1.0f, 1.0f};
      immUniformColor4fv(hover_color);
      immUniform1f("lineWidth", 5.0f * U.pixelsize);
      draw_polyline(pos, positions, body.edge_polylines[hover->edge]);
    }
  }

  immUnbindProgram();
  GPU_blend(GPU_BLEND_NONE);
  GPU_depth_test(GPU_DEPTH_NONE);
  GPU_matrix_pop_projection();
  GPU_matrix_pop();
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Toggle Reference Operator
 * \{ */

static CADFeature *edge_ref_feature(bContext *C, Object **r_ob)
{
  Object *ob = CTX_data_active_object(C);
  if (ob == nullptr || ob->type != OB_CAD || ob->data == nullptr) {
    return nullptr;
  }
  CADPart *part = id_cast<CADPart *>(ob->data);
  CADFeature *feature = BKE_cad_feature_active(part);
  if (feature == nullptr ||
      !ELEM(feature->type, CAD_FEATURE_FILLET, CAD_FEATURE_CHAMFER))
  {
    return nullptr;
  }
  if (r_ob) {
    *r_ob = ob;
  }
  return feature;
}

/** The largest 3D view of the screen, for operators started from another editor. */
static ScrArea *largest_view3d(bContext *C)
{
  bScreen *screen = CTX_wm_screen(C);
  if (screen == nullptr) {
    return nullptr;
  }
  ScrArea *best = nullptr;
  int best_size = 0;
  for (ScrArea &area : screen->areabase) {
    const int size = area.winx * area.winy;
    if (area.spacetype == SPACE_VIEW3D && size > best_size) {
      best = &area;
      best_size = size;
    }
  }
  return best;
}

/** Makes a 3D view region the context region. Returns false when the screen has none. */
static bool view3d_context_ensure(bContext *C)
{
  if (CTX_wm_region_view3d(C)) {
    return true;
  }
  ScrArea *area = largest_view3d(C);
  ARegion *region = area ? BKE_area_find_region_type(area, RGN_TYPE_WINDOW) : nullptr;
  if (region == nullptr) {
    return false;
  }
  CTX_wm_area_set(C, area);
  CTX_wm_region_set(C, region);
  return true;
}

static bool edge_ref_poll_view3d(bContext *C, const bool any_area)
{
  if (any_area ? (CTX_wm_region_view3d(C) == nullptr && largest_view3d(C) == nullptr) :
                 !ED_operator_region_view3d_active(C))
  {
    return false;
  }
  Object *ob = nullptr;
  if (edge_ref_feature(C, &ob) == nullptr) {
    CTX_wm_operator_poll_msg_set(C, "The active feature does not take edges");
    return false;
  }
  if (!ID_IS_EDITABLE(ob->data)) {
    CTX_wm_operator_poll_msg_set(C, "CAD part is not editable");
    return false;
  }
  return true;
}

static bool edge_ref_poll(bContext *C)
{
  return edge_ref_poll_view3d(C, false);
}

static bool edge_ref_poll_any_area(bContext *C)
{
  return edge_ref_poll_view3d(C, true);
}

/** Adds the picked edge to the feature, or removes it when already there. Returns true if the
 * references changed. */
static bool toggle_edge_ref(Object *ob, CADFeature *feature, const TopoPick &pick)
{
  const int index = BKE_cad_feature_ref_find(feature, pick.name);
  if (index >= 0) {
    BKE_cad_feature_ref_remove(feature, index);
  }
  else {
    BKE_cad_feature_ref_add(feature, pick.name, double3(pick.location));
  }
  DEG_id_tag_update(static_cast<ID *>(ob->data), ID_RECALC_GEOMETRY);
  WM_main_add_notifier(NC_GEOM | ND_DATA, ob->data);
  return true;
}

static wmOperatorStatus reference_toggle_exec(bContext *C, wmOperator *op)
{
  Object *ob = nullptr;
  CADFeature *feature = edge_ref_feature(C, &ob);
  int2 mval;
  RNA_int_get_array(op->ptr, "location", mval);
  const std::optional<TopoPick> pick = pick_topology(C, ob, mval, PickType::Edge);
  if (!pick) {
    BKE_report(op->reports, RPT_WARNING, "No edge under the cursor");
    return OPERATOR_CANCELLED;
  }
  toggle_edge_ref(ob, feature, *pick);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus reference_toggle_invoke(bContext *C,
                                                wmOperator *op,
                                                const wmEvent *event)
{
  RNA_int_set_array(op->ptr, "location", event->mval);
  return reference_toggle_exec(C, op);
}

void CAD_OT_reference_toggle(wmOperatorType *ot)
{
  ot->name = "Toggle Edge Reference";
  ot->description = "Add the edge under the cursor to the active feature, or remove it";
  ot->idname = "CAD_OT_reference_toggle";

  ot->invoke = reference_toggle_invoke;
  ot->exec = reference_toggle_exec;
  ot->poll = edge_ref_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_int_array(ot->srna,
                    "location",
                    2,
                    nullptr,
                    INT_MIN,
                    INT_MAX,
                    "Location",
                    "Cursor location in region coordinates",
                    INT_MIN,
                    INT_MAX);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Pick References (Modal)
 * \{ */

struct PickRefsData {
  Object *ob = nullptr;
  int feature_uid = -1;
  int rollback_uid_orig = -1;
  Vector<std::pair<std::string, double3>> refs_orig;
  std::optional<TopoPick> hover;
  ARegion *region = nullptr;
  void *draw_handle = nullptr;
};

static CADFeature *pick_refs_feature(const PickRefsData &data)
{
  return BKE_cad_feature_find(id_cast<CADPart *>(data.ob->data), data.feature_uid);
}

static void pick_refs_draw(const bContext *C, ARegion *region, void *arg)
{
  const PickRefsData &data = *static_cast<const PickRefsData *>(arg);
  const CADFeature *feature = pick_refs_feature(data);
  if (feature == nullptr) {
    return;
  }
  Vector<std::string> names;
  for (int i = 0; i < feature->refs_num; i++) {
    if (feature->refs[i].name) {
      names.append(feature->refs[i].name);
    }
  }
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  const Object *ob_eval = DEG_get_evaluated(depsgraph, data.ob);
  draw_edge_highlights(region, ob_eval, names, data.hover ? &*data.hover : nullptr);
}

static void pick_refs_status(bContext *C)
{
  WorkspaceStatus status(C);
  status.item(IFACE_("Add/Remove Edge"), ICON_MOUSE_LMB);
  status.item(IFACE_("Done"), ICON_EVENT_RETURN, ICON_MOUSE_RMB);
  status.item(IFACE_("Cancel"), ICON_EVENT_ESC);
}

static void pick_refs_exit(bContext *C, wmOperator *op, const bool cancel)
{
  PickRefsData *data = static_cast<PickRefsData *>(op->customdata);
  CADPart *part = id_cast<CADPart *>(data->ob->data);
  if (cancel) {
    if (CADFeature *feature = pick_refs_feature(*data)) {
      BKE_cad_feature_refs_clear(feature);
      for (const auto &[name, point] : data->refs_orig) {
        BKE_cad_feature_ref_add(feature, name, point);
      }
    }
  }
  part->rollback_uid = data->rollback_uid_orig;
  DEG_id_tag_update(&part->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, part);

  ED_region_draw_cb_exit(data->region->runtime->type, data->draw_handle);
  ED_region_tag_redraw(data->region);
  ED_workspace_status_text(C, nullptr);
  MEM_delete(data);
  op->customdata = nullptr;
}

static wmOperatorStatus pick_refs_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  Object *ob = nullptr;
  CADFeature *feature = edge_ref_feature(C, &ob);
  CADPart *part = id_cast<CADPart *>(ob->data);
  if (feature->prev == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "Nothing before this feature to pick edges on");
    return OPERATOR_CANCELLED;
  }

  /* Started from a button in another editor: run in the 3D view, without an initial hover. */
  const bool in_view3d = CTX_wm_region_view3d(C) != nullptr;
  if (!view3d_context_ensure(C)) {
    return OPERATOR_CANCELLED;
  }

  PickRefsData *data = MEM_new<PickRefsData>(__func__);
  data->ob = ob;
  data->feature_uid = feature->uid;
  data->rollback_uid_orig = part->rollback_uid;
  for (int i = 0; i < feature->refs_num; i++) {
    const CADTopoRef &ref = feature->refs[i];
    data->refs_orig.append(
        {ref.name ? ref.name : "", double3(ref.point[0], ref.point[1], ref.point[2])});
  }
  data->region = CTX_wm_region(C);
  op->customdata = data;

  /* Show the model the feature applies to, so its edges can be picked. */
  part->rollback_uid = feature->prev->uid;
  DEG_id_tag_update(&part->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, part);

  data->draw_handle = ED_region_draw_cb_activate(
      data->region->runtime->type, pick_refs_draw, data, REGION_DRAW_POST_VIEW);
  if (in_view3d) {
    data->hover = pick_topology(C, ob, event->mval, PickType::Edge);
  }
  pick_refs_status(C);
  ED_region_tag_redraw(data->region);

  WM_event_add_modal_handler(C, op);
  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus pick_refs_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  PickRefsData *data = static_cast<PickRefsData *>(op->customdata);
  CADFeature *feature = pick_refs_feature(*data);
  if (feature == nullptr) {
    pick_refs_exit(C, op, false);
    return OPERATOR_CANCELLED;
  }

  switch (event->type) {
    case MOUSEMOVE: {
      std::optional<TopoPick> hover = pick_topology(C, data->ob, event->mval, PickType::Edge);
      const bool changed = hover.has_value() != data->hover.has_value() ||
                           (hover && hover->name != data->hover->name);
      data->hover = std::move(hover);
      if (changed) {
        ED_region_tag_redraw(data->region);
      }
      return OPERATOR_RUNNING_MODAL;
    }
    case LEFTMOUSE:
      if (event->val == KM_PRESS) {
        data->hover = pick_topology(C, data->ob, event->mval, PickType::Edge);
        if (data->hover) {
          toggle_edge_ref(data->ob, feature, *data->hover);
          ED_region_tag_redraw(data->region);
        }
      }
      return OPERATOR_RUNNING_MODAL;
    case EVT_RETKEY:
    case EVT_PADENTER:
    case EVT_SPACEKEY:
    case RIGHTMOUSE:
      if (event->val == KM_PRESS) {
        pick_refs_exit(C, op, false);
        return OPERATOR_FINISHED;
      }
      return OPERATOR_RUNNING_MODAL;
    case EVT_ESCKEY:
      if (event->val == KM_PRESS) {
        pick_refs_exit(C, op, true);
        return OPERATOR_CANCELLED;
      }
      return OPERATOR_RUNNING_MODAL;
    default:
      break;
  }
  /* Let the view be navigated while picking. */
  if (ISMOUSE_WHEEL(event->type) || ISNDOF(event->type) ||
      ELEM(event->type, MIDDLEMOUSE, MOUSEPAN, MOUSEZOOM, MOUSEROTATE, MOUSESMARTZOOM) ||
      (event->type >= EVT_PAD0 && event->type <= EVT_PADPERIOD))
  {
    return OPERATOR_PASS_THROUGH;
  }
  return OPERATOR_RUNNING_MODAL;
}

static void pick_refs_cancel(bContext *C, wmOperator *op)
{
  pick_refs_exit(C, op, true);
}

void CAD_OT_references_pick(wmOperatorType *ot)
{
  ot->name = "Select Edges";
  ot->description =
      "Pick the edges the active feature applies to, on the model before that feature";
  ot->idname = "CAD_OT_references_pick";

  ot->invoke = pick_refs_invoke;
  ot->modal = pick_refs_modal;
  ot->cancel = pick_refs_cancel;
  ot->poll = edge_ref_poll_any_area;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_BLOCKING;
}

/** \} */

}  // namespace blender::ed::cad
