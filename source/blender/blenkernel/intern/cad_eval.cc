/* SPDX-FileCopyrightText: 2026 SimpleCAD Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Depsgraph evaluation of #CADPart objects:
 * DNA -> `cad::Document` (Blender units to millimeters) -> `cad::evaluate` (cached, incremental)
 * -> tessellation -> a welded mesh with B-rep attributes -> the object's mesh modifiers.
 *
 * Mesh attributes:
 * - `cad_face` (face, int): B-rep face index + 1 within its body.
 * - `cad_body` (face, int): body index.
 * - `cad_edge` (edge, int): B-rep edge index + 1 for mesh edges on a B-rep edge, 0 inside faces.
 * - `sharp_edge` (edge, bool): B-rep edges that are a crease (not tangent-continuous), so curved
 *   faces shade smooth and corners stay crisp.
 */

#include "MEM_guardedalloc.h"

#include "DNA_cad_types.h"
#include "DNA_mesh_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BLI_bit_vector.hh"
#include "BLI_listbase_iterator.hh"
#include "BLI_map.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector.hh"
#include "BLI_ordered_edge.hh"
#include "BLI_string.hh"

#include "BKE_attribute.hh"
#include "BKE_cad.hh"
#include "BKE_geometry_set.hh"
#include "BKE_idprop.hh"
#include "BKE_lib_id.hh"
#include "BKE_mesh.h"
#include "BKE_mesh.hh"
#include "BKE_modifier.hh"
#include "BKE_object.hh"
#include "BKE_object_types.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"

#ifdef WITH_CAD
#  include "CAD_eval.hh"
#endif

namespace blender {

#ifdef WITH_CAD

/* -------------------------------------------------------------------- */
/** \name DNA to Document
 * \{ */

static double3 param_double3(const CADFeature &feature, const StringRef key, const double3 fallback)
{
  const IDProperty *prop = feature.params ? IDP_GetPropertyFromGroup(feature.params, key) :
                                            nullptr;
  if (prop && prop->type == IDP_ARRAY && prop->len == 3 && prop->subtype == IDP_DOUBLE) {
    const double *v = IDP_array_double_get(prop);
    return {v[0], v[1], v[2]};
  }
  return double3(BKE_cad_feature_param_get_float3(
      &feature, key, float3(float(fallback.x), float(fallback.y), float(fallback.z))));
}

static Vector<int64_t> param_int_array(const CADFeature &feature, const StringRef key)
{
  Vector<int64_t> values;
  const IDProperty *prop = feature.params ? IDP_GetPropertyFromGroup(feature.params, key) :
                                            nullptr;
  if (prop && prop->type == IDP_ARRAY && prop->subtype == IDP_INT) {
    const int *v = IDP_array_int_get(prop);
    for (int i = 0; i < prop->len; i++) {
      values.append(v[i]);
    }
  }
  return values;
}

static Vector<cad::TopoRef> refs_to_mm(const CADFeature &feature, const double mm)
{
  Vector<cad::TopoRef> refs;
  for (int i = 0; i < feature.refs_num; i++) {
    const CADTopoRef &ref = feature.refs[i];
    refs.append({ref.name ? ref.name : "",
                 double3(ref.point[0], ref.point[1], ref.point[2]) * mm});
  }
  return refs;
}

static cad::SketchData sketch_to_mm(const CADSketch &sketch, const double mm)
{
  cad::SketchData data;
  data.plane.origin = {sketch.origin[0] * mm, sketch.origin[1] * mm, sketch.origin[2] * mm};
  data.plane.x_dir = {sketch.x_dir[0], sketch.x_dir[1], sketch.x_dir[2]};
  data.plane.normal = {sketch.normal[0], sketch.normal[1], sketch.normal[2]};
  auto point = [&](const int uid) -> std::optional<double2> {
    if (const CADSketchPoint *p = BKE_cad_sketch_point_find(&sketch, uid)) {
      return double2(p->co[0], p->co[1]) * mm;
    }
    return std::nullopt;
  };
  for (int i = 0; i < sketch.curves_num; i++) {
    const CADSketchCurve &c = sketch.curves[i];
    cad::SketchCurve curve;
    curve.uid = c.uid;
    curve.construction = (c.flag & CAD_SKETCH_CONSTRUCTION) != 0;
    curve.ccw = c.ccw != 0;
    switch (c.type) {
      case CAD_SKETCH_LINE: {
        const std::optional<double2> a = point(c.points[0]), b = point(c.points[1]);
        if (!a || !b) {
          continue;
        }
        curve.type = cad::SketchCurveType::Line;
        curve.start = *a;
        curve.end = *b;
        break;
      }
      case CAD_SKETCH_CIRCLE: {
        const std::optional<double2> center = point(c.points[2]);
        if (!center) {
          continue;
        }
        curve.type = cad::SketchCurveType::Circle;
        curve.center = *center;
        curve.radius = c.radius * mm;
        break;
      }
      case CAD_SKETCH_ARC: {
        const std::optional<double2> center = point(c.points[2]), a = point(c.points[0]),
                                     b = point(c.points[1]);
        if (!center || !a || !b) {
          continue;
        }
        curve.type = cad::SketchCurveType::Arc;
        curve.center = *center;
        curve.start = *a;
        curve.end = *b;
        curve.radius = math::distance(*a, *center);
        break;
      }
      default:
        continue;
    }
    data.curves.append(curve);
  }
  return data;
}

static cad::Document document_from_dna(const CADPart &part)
{
  const double mm = part.mm_per_unit;
  cad::Document doc;
  doc.kernel = cad::kernel::KernelId(part.kernel);
  doc.rollback_uid = part.rollback_uid;
  for (const CADFeature &f_item : part.features) {
    const CADFeature *f = &f_item;
    cad::Feature feature;
    feature.uid = f->uid;
    feature.name = f->name;
    feature.suppressed = (f->flag & CAD_FEATURE_SUPPRESSED) != 0;
    switch (eCADFeatureType(f->type)) {
      case CAD_FEATURE_SKETCH:
        feature.data = f->sketch ? sketch_to_mm(*f->sketch, mm) : cad::SketchData();
        break;
      case CAD_FEATURE_BOX: {
        cad::BoxData data;
        data.min = param_double3(*f, "min", double3(0.0)) * mm;
        data.size = param_double3(*f, "size", double3(0.02)) * mm;
        feature.data = data;
        break;
      }
      case CAD_FEATURE_EXTRUDE: {
        cad::ExtrudeData data;
        data.sketch_uid = BKE_cad_feature_param_get_int(f, "sketch", -1);
        data.regions = param_int_array(*f, "regions");
        data.extent = cad::kernel::ExtentMode(BKE_cad_feature_param_get_int(f, "extent", 0));
        data.distance = BKE_cad_feature_param_get(f, "distance", 0.01) * mm;
        data.distance2 = BKE_cad_feature_param_get(f, "distance2", 0.0) * mm;
        data.operation = cad::BodyOperation(BKE_cad_feature_param_get_int(f, "operation", 0));
        feature.data = data;
        break;
      }
      case CAD_FEATURE_FILLET: {
        cad::FilletData data;
        data.radius = BKE_cad_feature_param_get(f, "radius", 0.001) * mm;
        data.edges = refs_to_mm(*f, mm);
        feature.data = data;
        break;
      }
      case CAD_FEATURE_CHAMFER: {
        cad::ChamferData data;
        data.distance = BKE_cad_feature_param_get(f, "distance", 0.001) * mm;
        data.edges = refs_to_mm(*f, mm);
        feature.data = data;
        break;
      }
      default:
        continue;
    }
    doc.features.append(std::move(feature));
  }
  return doc;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Tessellation to Mesh
 * \{ */

static Mesh *mesh_from_state(const cad::ModelState &state,
                             const CADPart &part,
                             bke::CADBrepMesh &r_brep)
{
  const cad::kernel::Kernel *kernel = cad::kernel::kernel_get(cad::kernel::KernelId(part.kernel));
  const float to_units = float(1.0 / part.mm_per_unit);

  struct BodyMesh {
    cad::kernel::Tessellation tess;
    int vert_offset = 0;
  };
  Vector<BodyMesh> bodies;
  int verts_num = 0, tris_num = 0;
  for (const cad::Body &body : state.bodies) {
    bodies.append({});
    BodyMesh &bm = bodies.last();
    if (kernel) {
      cad::kernel::TessellationParams params;
      params.linear_deflection = std::max(1e-4, double(part.linear_deflection));
      params.angular_deflection = std::max(0.05, double(part.angular_deflection));
      kernel->tessellate(body.shape, params, bm.tess);
    }
    bm.vert_offset = verts_num;
    verts_num += int(bm.tess.positions.size());
    tris_num += int(bm.tess.triangles.size());
  }

  Mesh *mesh = BKE_mesh_new_nomain(verts_num, 0, tris_num, tris_num * 3);
  MutableSpan<float3> positions = mesh->vert_positions_for_write();
  MutableSpan<int> face_offsets = mesh->face_offsets_for_write();
  MutableSpan<int> corner_verts = mesh->corner_verts_for_write();
  for (const int i : face_offsets.index_range()) {
    face_offsets[i] = i * 3;
  }

  bke::MutableAttributeAccessor attributes = mesh->attributes_for_write();
  bke::SpanAttributeWriter<int> cad_face = attributes.lookup_or_add_for_write_only_span<int>(
      bke::CAD_ATTR_FACE, bke::AttrDomain::Face);
  bke::SpanAttributeWriter<int> cad_body = attributes.lookup_or_add_for_write_only_span<int>(
      bke::CAD_ATTR_BODY, bke::AttrDomain::Face);

  /* B-rep edge segments, keyed by mesh edge, for the edge attributes below. */
  struct Segment {
    int edge;
    bool smooth;
    /** Periodic seam (e.g. on a cylinder): an edge in the B-rep, but not drawn. */
    bool seam;
  };
  Map<OrderedEdge, Segment> brep_segments;

  int tri = 0;
  for (const int b : bodies.index_range()) {
    const BodyMesh &bm = bodies[b];
    const cad::kernel::Tessellation &t = bm.tess;
    r_brep.bodies.append_as();
    bke::CADBrepBody &brep_body = r_brep.bodies.last();
    brep_body.verts = IndexRange(bm.vert_offset, int(t.positions.size()));
    brep_body.faces = IndexRange(tri, int(t.triangles.size()));
    for (const int v : IndexRange(t.positions.size())) {
      positions[bm.vert_offset + v] = float3(t.positions[v][0], t.positions[v][1], t.positions[v][2]) *
                                      to_units;
    }
    for (const int i : IndexRange(t.triangles.size())) {
      for (int k = 0; k < 3; k++) {
        corner_verts[tri * 3 + k] = bm.vert_offset + t.triangles[i][k];
      }
      cad_face.span[tri] = t.triangle_faces[i] + 1;
      cad_body.span[tri] = b;
      tri++;
    }
    const cad::kernel::Topology *topology = state.bodies[b].topology.get();
    for (const int e : IndexRange(t.edge_polylines.size())) {
      const std::vector<int> &line = t.edge_polylines[e];
      const bool smooth = e < int(t.edge_smooth.size()) && t.edge_smooth[e];
      bool seam = false;
      int2 edge_faces(-1, -1);
      if (topology && e < int(topology->edges.size())) {
        const std::array<int, 2> &faces = topology->edges[e].faces;
        seam = faces[0] >= 0 && faces[0] == faces[1];
        edge_faces = int2(faces[0], faces[1]);
      }
      brep_body.edge_faces.append(edge_faces);
      brep_body.edge_polylines.append_as();
      Vector<int> &polyline = brep_body.edge_polylines.last();
      for (const int v : line) {
        polyline.append(bm.vert_offset + v);
      }
      for (size_t k = 0; k + 1 < line.size(); k++) {
        brep_segments.add(OrderedEdge(bm.vert_offset + line[k], bm.vert_offset + line[k + 1]),
                          {e + 1, smooth, seam});
      }
    }
  }
  cad_face.finish();
  cad_body.finish();

  bke::mesh_calc_edges(*mesh, false, false);

  const Span<int2> edges = mesh->edges();
  bke::SpanAttributeWriter<int> cad_edge = attributes.lookup_or_add_for_write_only_span<int>(
      bke::CAD_ATTR_EDGE, bke::AttrDomain::Edge);
  bke::SpanAttributeWriter<bool> sharp_edge = attributes.lookup_or_add_for_write_only_span<bool>(
      "sharp_edge", bke::AttrDomain::Edge);
  for (const int i : edges.index_range()) {
    const Segment *segment = brep_segments.lookup_ptr(OrderedEdge(edges[i][0], edges[i][1]));
    cad_edge.span[i] = segment ? segment->edge : 0;
    sharp_edge.span[i] = segment && !segment->smooth;
  }
  cad_edge.finish();
  sharp_edge.finish();

  mesh->tag_positions_changed();
  mesh->tag_topology_changed();

  /* Only B-rep edges are wire edges: the viewport wireframe (and the CAD edge overlay) skip the
   * edges inside faces. Array and Mirror modifiers keep this, others drop it. */
  BitVector<> &display_edges = mesh->runtime->subsurf_optimal_display_edges;
  display_edges.resize(edges.size(), false);
  for (const int i : edges.index_range()) {
    const Segment *segment = brep_segments.lookup_ptr(OrderedEdge(edges[i][0], edges[i][1]));
    display_edges[i].set(segment && !segment->seam);
  }
  return mesh;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Modifiers
 * \{ */

static void cad_evaluate_modifiers(Depsgraph *depsgraph,
                                   Scene *scene,
                                   Object *object,
                                   bke::GeometrySet &geometry_set)
{
  const bool use_render = (DEG_get_mode(depsgraph) == DAG_EVAL_RENDER);
  const ModifierMode required_mode = use_render ? eModifierMode_Render : eModifierMode_Realtime;
  const ModifierApplyFlag apply_flag = use_render ? MOD_APPLY_RENDER : MOD_APPLY_USECACHE;
  const ModifierEvalContext mectx = {depsgraph, object, apply_flag};

  BKE_modifiers_clear_errors(object);
  VirtualModifierData virtual_modifier_data;
  ModifierData *md = BKE_modifiers_get_virtual_modifierlist(object, &virtual_modifier_data);
  for (; md; md = md->next) {
    const ModifierTypeInfo *mti = BKE_modifier_get_info(ModifierType(md->type));
    if (!BKE_modifier_is_enabled(scene, md, required_mode)) {
      continue;
    }
    bke::ScopedModifierTimer modifier_timer{*md};
    if (md->type == eModifierType_Nodes) {
      mti->modify_geometry_set(md, &mectx, &geometry_set);
      continue;
    }
    if (!geometry_set.has_mesh()) {
      continue;
    }
    Mesh *mesh = geometry_set.get_mesh_for_write();
    if (mti->type == ModifierTypeType::OnlyDeform) {
      mti->deform_verts(md, &mectx, mesh, mesh->vert_positions_for_write());
      mesh->tag_positions_changed();
    }
    else if (mti->modify_mesh) {
      Mesh *result = mti->modify_mesh(md, &mectx, mesh);
      if (result != mesh) {
        geometry_set.replace_mesh(result);
      }
    }
  }
}

/** \} */

#endif /* WITH_CAD */

void BKE_cad_data_update(Depsgraph *depsgraph, Scene *scene, Object *object)
{
  BKE_object_free_derived_caches(object);
  CADPart *part = id_cast<CADPart *>(object->data);
  bke::GeometrySet geometry_set;

#ifdef WITH_CAD
  const cad::Document doc = document_from_dna(*part);
  auto result = std::make_shared<cad::EvalResult>(cad::evaluate(doc));

  Vector<bke::CADFeatureStatus> status;
  for (const cad::FeatureResult &fr : result->features) {
    status.append({fr.uid, int(fr.status), fr.message});
  }
  std::shared_ptr<bke::CADBrepMesh> brep;
  if (result->final_state) {
    brep = std::make_shared<bke::CADBrepMesh>();
    Mesh *mesh = mesh_from_state(*result->final_state, *part, *brep);
    /* Cheap: arrays are shared until the modifiers below write to them. */
    brep->mesh = BKE_mesh_copy_for_eval(*mesh);
    geometry_set.replace_mesh(mesh);
  }
  part->runtime->eval = result;
  part->runtime->brep_mesh = std::move(brep);
  part->runtime->eval_status = status;
  /* Share the status with the original so the UI can show it. */
  if (DEG_is_active(depsgraph)) {
    CADPart *part_orig = DEG_get_original(part);
    if (part_orig != part && part_orig->runtime) {
      part_orig->runtime->eval = result;
      part_orig->runtime->eval_status = std::move(status);
    }
  }

  cad_evaluate_modifiers(depsgraph, scene, object, geometry_set);
#else
  UNUSED_VARS(depsgraph, scene);
#endif

  if (Mesh *mesh = geometry_set.get_mesh_for_write()) {
    STRNCPY(mesh->id.name, part->id.name);
    *(reinterpret_cast<short *>(mesh->id.name)) = ID_ME;
  }
  BKE_object_eval_assign_data(object, &part->id, false);
  object->runtime->geometry_set_eval = new bke::GeometrySet(std::move(geometry_set));
}

namespace bke {
CADBrepMesh::~CADBrepMesh()
{
  if (this->mesh) {
    BKE_id_free(nullptr, this->mesh);
  }
}
}  // namespace bke

std::string BKE_cad_topology_name(const CADPart *part_eval,
                                  const int body,
                                  const bool is_edge,
                                  const int index)
{
#ifdef WITH_CAD
  if (!part_eval || !part_eval->runtime || !part_eval->runtime->eval ||
      !part_eval->runtime->eval->final_state)
  {
    return {};
  }
  const Vector<cad::Body> &bodies = part_eval->runtime->eval->final_state->bodies;
  if (body < 0 || body >= bodies.size()) {
    return {};
  }
  const Vector<std::string> &names = is_edge ? bodies[body].edge_names : bodies[body].face_names;
  return (index >= 0 && index < names.size()) ? names[index] : std::string();
#else
  UNUSED_VARS(part_eval, body, is_edge, index);
  return {};
#endif
}

/* -------------------------------------------------------------------- */
/** \name Exchange
 * \{ */

bool BKE_cad_step_export_supported()
{
#ifdef WITH_CAD
  const cad::kernel::Kernel *occt = cad::kernel::kernel_get(cad::kernel::KernelId::OCCT);
  return occt && occt->capabilities().step_export;
#else
  return false;
#endif
}

int BKE_cad_export_step(const Span<const Object *> objects_eval,
                        const StringRefNull filepath,
                        const bool apply_transform,
                        std::string &r_message)
{
#ifdef WITH_CAD
  using namespace cad::kernel;
  const Kernel *occt = kernel_get(KernelId::OCCT);
  if (!occt || !occt->capabilities().step_export) {
    r_message = "STEP export needs the OpenCASCADE kernel, which is not part of this build";
    return -1;
  }

  std::vector<Shape> shapes;
  std::vector<std::string> names;
  for (const Object *object : objects_eval) {
    if (object->type != OB_CAD || object->data == nullptr) {
      continue;
    }
    const CADPart &part = *id_cast<const CADPart *>(object->data);
    const char *object_name = object->id.name + 2;

    /* The STEP writer needs OpenCASCADE shapes. The document is kernel-neutral, so parts on
     * another kernel are evaluated once more with OpenCASCADE (cached like any evaluation). */
    std::shared_ptr<const cad::EvalResult> result = part.runtime ? part.runtime->eval : nullptr;
    if (!result || KernelId(part.kernel) != KernelId::OCCT) {
      cad::Document doc = document_from_dna(part);
      doc.kernel = KernelId::OCCT;
      result = std::make_shared<cad::EvalResult>(cad::evaluate(doc));
    }
    if (!result->final_state || result->final_state->bodies.is_empty()) {
      continue;
    }

    Transform transform;
    if (apply_transform) {
      const float4x4 &m = object->object_to_world();
      for (int c = 0; c < 4; c++) {
        for (int r = 0; r < 4; r++) {
          transform.matrix[c * 4 + r] = double(m[c][r]);
        }
      }
      /* Translation is in Blender units, the shapes are in millimeters. */
      for (int r = 0; r < 3; r++) {
        transform.matrix[12 + r] *= part.mm_per_unit;
      }
    }

    const Vector<cad::Body> &bodies = result->final_state->bodies;
    for (const cad::Body &body : bodies) {
      Shape shape = body.shape;
      if (apply_transform) {
        OpResult moved = occt->transform(shape, transform);
        if (!moved.ok()) {
          r_message = std::string("Cannot place \"") + object_name + "\": " + moved.message +
                      " (non-uniform scale is not supported)";
          return -1;
        }
        shape = moved.shape;
      }
      shapes.push_back(shape);
      names.push_back(bodies.size() == 1 ? std::string(object_name) :
                                           std::string(object_name) + "." + body.name);
    }
  }

  if (shapes.empty()) {
    r_message = "No CAD part solids to export";
    return -1;
  }
  if (occt->export_step(shapes, names, filepath, &r_message) != Status::Ok) {
    if (r_message.empty()) {
      r_message = "STEP export failed";
    }
    return -1;
  }
  return int(shapes.size());
#else
  UNUSED_VARS(objects_eval, filepath, apply_transform);
  r_message = "This build has no CAD support";
  return -1;
#endif
}

/** \} */

}  // namespace blender
