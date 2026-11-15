/* SPDX-FileCopyrightText: 2026 SimpleCAD Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edcad
 *
 * Feature dialogs: a new or edited feature is previewed live while its inputs are changed in
 * the sidebar ("Feature" tab) or with viewport gizmos. OK keeps the changes as one undo step,
 * Cancel (or undo) reverts them. See #BKE_cad_dialog_begin.
 */

#include <fmt/format.h>

#include "DNA_cad_types.h"
#include "DNA_object_types.h"
#include "DNA_screen_types.h"
#include "DNA_view3d_types.h"

#include "BLI_listbase_iterator.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"

#include "BKE_cad.hh"
#include "BKE_context.hh"
#include "BKE_library.hh"
#include "BKE_main.hh"
#include "BKE_screen.hh"

#include "DEG_depsgraph.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_enum_types.hh"

#include "UI_interface_c.hh"
#include "UI_resources.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "ED_cad.hh"
#include "ED_gizmo_library.hh"
#include "ED_screen.hh"
#include "ED_undo.hh"

#include "cad_intern.hh"
#include "cad_sketch.hh"

namespace blender::ed::cad {

/* -------------------------------------------------------------------- */
/** \name Utilities
 * \{ */

static bool dialog_poll(bContext *C)
{
  const CADPart *part = context_part(C);
  if (part == nullptr || BKE_cad_dialog_feature(part) == nullptr) {
    CTX_wm_operator_poll_msg_set(C, "No feature dialog is open");
    return false;
  }
  return ID_IS_EDITABLE(&part->id);
}

/** Shows the "Feature" tab of the sidebar in the context's 3D view, or the largest one. */
static void dialog_sidebar_show(bContext *C)
{
  ScrArea *area = CTX_wm_area(C);
  if (area == nullptr || area->spacetype != SPACE_VIEW3D) {
    area = largest_view3d(C);
  }
  ARegion *region = area ? BKE_area_find_region_type(area, RGN_TYPE_UI) : nullptr;
  if (region == nullptr) {
    return;
  }
  if (region->flag & RGN_FLAG_HIDDEN) {
    region->flag &= ~RGN_FLAG_HIDDEN;
    ED_region_visibility_change_update(C, area, region);
  }
  ui::panel_category_active_set(region, "Feature");
  ED_region_tag_redraw(region);
}

/** Closes the dialog of `part`; confirming makes an undo step. */
static void dialog_close(bContext *C, CADPart *part, const bool apply)
{
  const CADFeature *feature = BKE_cad_dialog_feature(part);
  std::string undo_name;
  if (feature) {
    undo_name = (part->flag & CAD_PART_DIALOG_NEW) ?
                    BKE_cad_feature_type_name(eCADFeatureType(feature->type)) :
                    fmt::format("Edit {}", feature->name);
  }
  BKE_cad_dialog_end(part, apply);
  /* Cancel restores old values; parameters may have changed meanwhile. */
  BKE_cad_expressions_update(part);
  part_changed(C, part);
  if (apply && feature) {
    ED_undo_push(C, undo_name.c_str());
  }
}

bool dialog_cancel_for_undo(bContext *C)
{
  Main *bmain = CTX_data_main(C);
  bool cancelled = false;
  for (CADPart &part : bmain->cad_parts) {
    if (BKE_cad_dialog_feature(&part)) {
      dialog_close(C, &part, false);
      cancelled = true;
    }
  }
  return cancelled;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name New Feature
 * \{ */

static bool feature_new_poll(bContext *C)
{
  if (!part_editable_poll(C)) {
    return false;
  }
  const Object *ob = CTX_data_active_object(C);
  if (ob && ob->type == OB_CAD && !ELEM(ob->mode, OB_MODE_OBJECT, OB_MODE_EDIT)) {
    CTX_wm_operator_poll_msg_set(C, "Not available in this mode");
    return false;
  }
  return true;
}

static wmOperatorStatus feature_new_exec(bContext *C, wmOperator *op)
{
  /* From sketch mode: finish the sketch first, like confirming it. */
  Object *ob = CTX_data_active_object(C);
  if (ob && ob->type == OB_CAD && ob->mode == OB_MODE_EDIT) {
    WM_operator_name_call(
        C, "CAD_OT_sketch_finish", wm::OpCallContext::ExecDefault, nullptr, nullptr);
  }

  CADPart *part = context_part(C);
  if (BKE_cad_dialog_feature(part)) {
    dialog_close(C, part, true);
  }
  const eCADFeatureType type = eCADFeatureType(RNA_enum_get(op->ptr, "type"));
  const CADFeature *prev_active = BKE_cad_feature_active(part);
  CADFeature *feature = BKE_cad_feature_add(part, type);
  /* Before moving the marker to the new feature, so Cancel can restore it. */
  BKE_cad_dialog_begin(part, feature, true);
  feature_insert_at_rollback(part, feature);
  feature_init_defaults(part, feature, prev_active);
  part_changed(C, part);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus feature_new_invoke(bContext *C, wmOperator *op, const wmEvent * /*event*/)
{
  const wmOperatorStatus ret = feature_new_exec(C, op);
  dialog_sidebar_show(C);
  const CADFeature *feature = BKE_cad_dialog_feature(context_part(C));
  if (feature && ELEM(feature->type, CAD_FEATURE_FILLET, CAD_FEATURE_CHAMFER) && feature->prev) {
    /* Pick the edges right away. */
    WM_operator_name_call(
        C, "CAD_OT_references_pick", wm::OpCallContext::InvokeDefault, nullptr, nullptr);
  }
  return ret;
}

static const EnumPropertyItem *feature_new_type_itemf(bContext * /*C*/,
                                                      PointerRNA * /*ptr*/,
                                                      PropertyRNA * /*prop*/,
                                                      bool *r_free)
{
  EnumPropertyItem *items = nullptr;
  int items_num = 0;
  for (const EnumPropertyItem *item = rna_enum_cad_feature_type_items; item->identifier; item++) {
    /* Sketches are made in sketch mode. */
    if (item->value != CAD_FEATURE_SKETCH) {
      RNA_enum_item_add(&items, &items_num, item);
    }
  }
  RNA_enum_item_end(&items, &items_num);
  *r_free = true;
  return items;
}

void CAD_OT_feature_new(wmOperatorType *ot)
{
  ot->name = "New Feature";
  ot->description = "Add a feature and set it up in a dialog with a live preview";
  ot->idname = "CAD_OT_feature_new";

  ot->invoke = feature_new_invoke;
  ot->exec = feature_new_exec;
  ot->poll = feature_new_poll;

  /* No undo step: confirming the dialog makes it. */
  ot->flag = 0;

  ot->prop = RNA_def_enum(
      ot->srna, "type", rna_enum_cad_feature_type_items, CAD_FEATURE_EXTRUDE, "Type", "");
  RNA_def_enum_funcs(ot->prop, feature_new_type_itemf);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Edit Feature
 * \{ */

static CADFeature *edit_feature_from_op(bContext *C, wmOperator *op)
{
  CADPart *part = context_part(C);
  const int uid = RNA_int_get(op->ptr, "feature_uid");
  return uid >= 0 ? BKE_cad_feature_find(part, uid) : BKE_cad_feature_active(part);
}

static wmOperatorStatus feature_edit_exec(bContext *C, wmOperator *op)
{
  CADPart *part = context_part(C);
  CADFeature *feature = edit_feature_from_op(C, op);
  if (feature == nullptr) {
    return OPERATOR_CANCELLED;
  }
  if (feature->type == CAD_FEATURE_SKETCH) {
    /* Sketches are edited in sketch mode. */
    wmOperatorType *ot = WM_operatortype_find("CAD_OT_sketch_edit", false);
    PointerRNA ptr = WM_operator_properties_create_ptr(ot);
    RNA_int_set(&ptr, "feature_uid", feature->uid);
    WM_operator_name_call_ptr(C, ot, wm::OpCallContext::ExecDefault, &ptr, nullptr);
    WM_operator_properties_free(&ptr);
    return OPERATOR_FINISHED;
  }
  if (BKE_cad_dialog_feature(part) == feature) {
    return OPERATOR_FINISHED;
  }
  if (BKE_cad_dialog_feature(part)) {
    dialog_close(C, part, true);
  }
  BKE_cad_dialog_begin(part, feature, false);
  part_changed(C, part);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus feature_edit_invoke(bContext *C,
                                            wmOperator *op,
                                            const wmEvent * /*event*/)
{
  const wmOperatorStatus ret = feature_edit_exec(C, op);
  if (BKE_cad_dialog_feature(context_part(C))) {
    dialog_sidebar_show(C);
  }
  return ret;
}

void CAD_OT_feature_edit(wmOperatorType *ot)
{
  ot->name = "Edit Feature";
  ot->description =
      "Change a feature in a dialog with a live preview (sketches open in sketch mode)";
  ot->idname = "CAD_OT_feature_edit";

  ot->invoke = feature_edit_invoke;
  ot->exec = feature_edit_exec;
  ot->poll = part_editable_poll;

  ot->flag = 0;

  PropertyRNA *prop = RNA_def_int(ot->srna,
                                  "feature_uid",
                                  -1,
                                  -1,
                                  INT_MAX,
                                  "Feature",
                                  "Unique id of the feature (-1: the active feature)",
                                  -1,
                                  INT_MAX);
  RNA_def_property_flag(prop, PropertyFlag(PROP_HIDDEN | PROP_SKIP_SAVE));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name OK / Cancel
 * \{ */

static wmOperatorStatus dialog_ok_exec(bContext *C, wmOperator * /*op*/)
{
  dialog_close(C, context_part(C), true);
  return OPERATOR_FINISHED;
}

void CAD_OT_dialog_ok(wmOperatorType *ot)
{
  ot->name = "OK";
  ot->description = "Keep the changes of the feature dialog";
  ot->idname = "CAD_OT_dialog_ok";

  ot->exec = dialog_ok_exec;
  ot->poll = dialog_poll;

  /* The undo step is pushed by #dialog_close, named after the feature. */
  ot->flag = 0;
}

static wmOperatorStatus dialog_cancel_exec(bContext *C, wmOperator * /*op*/)
{
  dialog_close(C, context_part(C), false);
  return OPERATOR_FINISHED;
}

void CAD_OT_dialog_cancel(wmOperatorType *ot)
{
  ot->name = "Cancel";
  ot->description = "Discard the changes of the feature dialog (a new feature is removed)";
  ot->idname = "CAD_OT_dialog_cancel";

  ot->exec = dialog_cancel_exec;
  ot->poll = dialog_poll;

  ot->flag = 0;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Extrude Gizmo
 *
 * Arrows on the profile drag the extrusion distances.
 * \{ */

struct ExtrudeArrowTarget {
  CADPart *part = nullptr;
  int feature_uid = -1;
  const char *key = "distance";
  /** Gizmo offset (world units) per input unit. */
  float scale = 1.0f;
};

struct ExtrudeGizmoGroup {
  wmGizmo *arrows[2] = {nullptr, nullptr};
  ExtrudeArrowTarget targets[2];
};

/** The extrude feature in a dialog of the active object, with its sketch. */
static CADFeature *extrude_dialog_feature(const bContext *C,
                                          Object **r_ob,
                                          const CADSketch **r_sketch)
{
  Object *ob = CTX_data_active_object(C);
  if (ob == nullptr || ob->type != OB_CAD || ob->data == nullptr || ob->mode != OB_MODE_OBJECT) {
    return nullptr;
  }
  CADPart *part = id_cast<CADPart *>(ob->data);
  CADFeature *feature = BKE_cad_dialog_feature(part);
  if (feature == nullptr || feature->type != CAD_FEATURE_EXTRUDE ||
      (feature->flag & CAD_FEATURE_SUPPRESSED))
  {
    return nullptr;
  }
  const CADFeature *sketch = BKE_cad_feature_find(
      part, BKE_cad_feature_param_get_int(feature, "sketch", -1));
  if (sketch == nullptr || sketch->sketch == nullptr || sketch->sketch->curves_num == 0) {
    return nullptr;
  }
  if (r_ob) {
    *r_ob = ob;
  }
  if (r_sketch) {
    *r_sketch = sketch->sketch;
  }
  return feature;
}

static bool extrude_gizmo_poll(const bContext *C, wmGizmoGroupType * /*gzgt*/)
{
  const View3D *v3d = CTX_wm_view3d(C);
  if (v3d && (v3d->gizmo_flag & V3D_GIZMO_HIDE)) {
    return false;
  }
  return extrude_dialog_feature(C, nullptr, nullptr) != nullptr;
}

static void extrude_arrow_value_get(const wmGizmo * /*gz*/, wmGizmoProperty *gz_prop, void *value)
{
  const ExtrudeArrowTarget &target = *static_cast<const ExtrudeArrowTarget *>(
      gz_prop->custom_func.user_data);
  const CADFeature *feature = BKE_cad_feature_find(target.part, target.feature_uid);
  const double v = feature ? BKE_cad_feature_param_get(feature, target.key, 0.0) : 0.0;
  *static_cast<float *>(value) = float(v) * target.scale;
}

static void extrude_arrow_value_set(const wmGizmo * /*gz*/,
                                    wmGizmoProperty *gz_prop,
                                    const void *value)
{
  const ExtrudeArrowTarget &target = *static_cast<const ExtrudeArrowTarget *>(
      gz_prop->custom_func.user_data);
  CADFeature *feature = BKE_cad_feature_find(target.part, target.feature_uid);
  if (feature == nullptr || target.scale == 0.0f) {
    return;
  }
  float v = *static_cast<const float *>(value) / target.scale;
  if (STREQ(target.key, "distance2")) {
    v = std::max(v, 0.0f);
  }
  BKE_cad_feature_input_value_set(feature, target.key, v);
  DEG_id_tag_update(&target.part->id, ID_RECALC_GEOMETRY);
  WM_main_add_notifier(NC_GEOM | ND_DATA, &target.part->id);
}

static void extrude_gizmo_free(void *customdata)
{
  MEM_delete(static_cast<ExtrudeGizmoGroup *>(customdata));
}

static void extrude_gizmo_setup(const bContext * /*C*/, wmGizmoGroup *gzgroup)
{
  ExtrudeGizmoGroup *data = MEM_new<ExtrudeGizmoGroup>(__func__);
  for (wmGizmo *&gz : data->arrows) {
    gz = WM_gizmo_new("GIZMO_GT_arrow_3d", gzgroup, nullptr);
    RNA_enum_set(gz->ptr, "draw_style", ED_GIZMO_ARROW_STYLE_NORMAL);
    RNA_enum_set(gz->ptr, "draw_options", ED_GIZMO_ARROW_DRAW_FLAG_STEM);
    ui::theme::get_color_3fv(TH_GIZMO_PRIMARY, gz->color);
    ui::theme::get_color_3fv(TH_GIZMO_HI, gz->color_hi);
    gz->line_width = 2.0f;
  }
  gzgroup->customdata = data;
  gzgroup->customdata_free = extrude_gizmo_free;
}

static void extrude_gizmo_refresh(const bContext *C, wmGizmoGroup *gzgroup)
{
  ExtrudeGizmoGroup *data = static_cast<ExtrudeGizmoGroup *>(gzgroup->customdata);
  Object *ob = nullptr;
  const CADSketch *sketch = nullptr;
  const CADFeature *feature = extrude_dialog_feature(C, &ob, &sketch);
  if (feature == nullptr) {
    for (wmGizmo *gz : data->arrows) {
      WM_gizmo_set_flag(gz, WM_GIZMO_HIDDEN, true);
    }
    return;
  }

  /* The arrows start in the middle of the profile. */
  float2 min(FLT_MAX), max(-FLT_MAX);
  for (int i = 0; i < sketch->curves_num; i++) {
    if (sketch->curves[i].flag & CAD_SKETCH_CONSTRUCTION) {
      continue;
    }
    for (const double2 &co : sketch_curve_polyline(sketch, i, 16)) {
      min = math::min(min, float2(co));
      max = math::max(max, float2(co));
    }
  }
  if (min.x > max.x) {
    min = max = float2(0.0f);
  }
  const SketchFrame frame = sketch_frame(ob, sketch);
  const float2 center_2d = (min + max) * 0.5f;
  const float3 center = math::transform_point(frame.to_world, float3(center_2d, 0.0f));
  const float3 normal = math::normalize(frame.normal_world);
  /* Distances are in part space: scale them like the object does. */
  const float3 normal_part = math::transform_direction(frame.to_sketch, normal);
  const float scale = math::length(normal_part) > 1e-12f ? 1.0f / math::length(normal_part) :
                                                           1.0f;

  const int extent = BKE_cad_feature_param_get_int(feature, "extent", 0);
  CADPart *part = id_cast<CADPart *>(ob->data);
  for (const int side : {0, 1}) {
    wmGizmo *gz = data->arrows[side];
    /* Two Sides: the second arrow drags the distance against the normal. */
    if (side == 1 && extent != 2) {
      WM_gizmo_set_flag(gz, WM_GIZMO_HIDDEN, true);
      continue;
    }
    WM_gizmo_set_flag(gz, WM_GIZMO_HIDDEN, false);
    ExtrudeArrowTarget &target = data->targets[side];
    target.part = part;
    target.feature_uid = feature->uid;
    target.key = side == 0 ? "distance" : "distance2";
    /* Symmetric: half the distance to each side. */
    target.scale = (extent == 1 ? 0.5f : 1.0f) * scale;

    WM_gizmo_set_matrix_location(gz, center);
    WM_gizmo_set_matrix_rotation_from_z_axis(gz, side == 0 ? normal : -normal);

    wmGizmoPropertyFnParams params{};
    params.value_get_fn = extrude_arrow_value_get;
    params.value_set_fn = extrude_arrow_value_set;
    params.user_data = &target;
    WM_gizmo_target_property_def_func(gz, "offset", &params);
  }
}

static void VIEW3D_GGT_cad_extrude(wmGizmoGroupType *gzgt)
{
  gzgt->name = "CAD Extrude";
  gzgt->idname = "VIEW3D_GGT_cad_extrude";

  gzgt->flag |= WM_GIZMOGROUPTYPE_PERSISTENT | WM_GIZMOGROUPTYPE_3D;

  gzgt->poll = extrude_gizmo_poll;
  gzgt->setup = extrude_gizmo_setup;
  gzgt->setup_keymap = WM_gizmogroup_setup_keymap_generic_maybe_drag;
  gzgt->refresh = extrude_gizmo_refresh;
}

void dialog_gizmos_register()
{
  wmGizmoMapType_Params params{SPACE_VIEW3D, RGN_TYPE_WINDOW};
  wmGizmoMapType *gzmap_type = WM_gizmomaptype_ensure(&params);
  WM_gizmogrouptype_append_and_link(gzmap_type, VIEW3D_GGT_cad_extrude);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Keymap
 * \{ */

static bool part_keymap_poll(bContext *C)
{
  const Object *ob = CTX_data_active_object(C);
  return ob && ob->type == OB_CAD && ob->mode == OB_MODE_OBJECT;
}

void part_keymap(wmKeyConfig *keyconf)
{
  wmKeyMap *keymap = WM_keymap_ensure(keyconf, "CAD Part", SPACE_EMPTY, RGN_TYPE_WINDOW);
  keymap->poll = part_keymap_poll;
}

/** \} */

}  // namespace blender::ed::cad
