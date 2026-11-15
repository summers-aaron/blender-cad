/* SPDX-FileCopyrightText: 2026 SimpleCAD Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup RNA
 *
 * Python / UI access to parametric CAD parts. Feature parameters are ID properties of the
 * feature (`part.features["Extrude1"]["distance"]`), so they can be animated and driven.
 */

#include <climits>

#include "DNA_cad_types.h"

#include "RNA_define.hh"
#include "RNA_enum_types.hh"

#include "rna_internal.hh"

#include "UI_resources.hh"

namespace blender {

static const EnumPropertyItem rna_enum_cad_kernel_items[] = {
    {CAD_KERNEL_OCCT, "OCCT", 0, "OpenCASCADE", "Open CASCADE Technology B-rep kernel"},
    {CAD_KERNEL_SMLIB, "SMLIB", 0, "SMLib", "NVIDIA usd-brep SMLib B-rep kernel"},
    {0, nullptr, 0, nullptr, nullptr},
};

const EnumPropertyItem rna_enum_cad_feature_type_items[] = {
    {CAD_FEATURE_SKETCH, "SKETCH", ICON_GREASEPENCIL, "Sketch", "2D profile on a plane"},
    {CAD_FEATURE_BOX, "BOX", ICON_MESH_CUBE, "Box", "Axis aligned box primitive"},
    {CAD_FEATURE_EXTRUDE, "EXTRUDE", ICON_MOD_SOLIDIFY, "Extrude", "Extrude sketch regions"},
    {CAD_FEATURE_FILLET, "FILLET", ICON_MOD_BEVEL, "Fillet", "Round edges"},
    {CAD_FEATURE_CHAMFER, "CHAMFER", ICON_MOD_BEVEL, "Chamfer", "Bevel edges flat"},
    {0, nullptr, 0, nullptr, nullptr},
};

static const EnumPropertyItem rna_enum_cad_feature_status_items[] = {
    {0, "NONE", 0, "None", "Not evaluated yet"},
    {1, "OK", 0, "OK", ""},
    {2, "WARNING", ICON_ERROR, "Warning", ""},
    {3, "ERROR", ICON_CANCEL, "Error", ""},
    {4, "SUPPRESSED", 0, "Suppressed", ""},
    {5, "ROLLED_BACK", 0, "Rolled Back", "After the rollback marker"},
    {0, nullptr, 0, nullptr, nullptr},
};

static const EnumPropertyItem rna_enum_cad_sketch_curve_type_items[] = {
    {CAD_SKETCH_LINE, "LINE", 0, "Line", ""},
    {CAD_SKETCH_ARC, "ARC", 0, "Arc", ""},
    {CAD_SKETCH_CIRCLE, "CIRCLE", 0, "Circle", ""},
    {0, nullptr, 0, nullptr, nullptr},
};

static const EnumPropertyItem rna_enum_cad_constraint_type_items[] = {
    {CAD_CONSTRAINT_COINCIDENT, "COINCIDENT", 0, "Coincident", ""},
    {CAD_CONSTRAINT_HORIZONTAL, "HORIZONTAL", 0, "Horizontal", ""},
    {CAD_CONSTRAINT_VERTICAL, "VERTICAL", 0, "Vertical", ""},
    {CAD_CONSTRAINT_PARALLEL, "PARALLEL", 0, "Parallel", ""},
    {CAD_CONSTRAINT_PERPENDICULAR, "PERPENDICULAR", 0, "Perpendicular", ""},
    {CAD_CONSTRAINT_TANGENT, "TANGENT", 0, "Tangent", ""},
    {CAD_CONSTRAINT_EQUAL, "EQUAL", 0, "Equal", ""},
    {CAD_CONSTRAINT_POINT_ON_CURVE, "POINT_ON_CURVE", 0, "Point on Curve", ""},
    {CAD_CONSTRAINT_FIX, "FIX", 0, "Fix", ""},
    {CAD_CONSTRAINT_DISTANCE, "DISTANCE", 0, "Distance", ""},
    {CAD_CONSTRAINT_DISTANCE_X, "DISTANCE_X", 0, "Horizontal Distance", ""},
    {CAD_CONSTRAINT_DISTANCE_Y, "DISTANCE_Y", 0, "Vertical Distance", ""},
    {CAD_CONSTRAINT_RADIUS, "RADIUS", 0, "Radius", ""},
    {CAD_CONSTRAINT_DIAMETER, "DIAMETER", 0, "Diameter", ""},
    {CAD_CONSTRAINT_ANGLE, "ANGLE", 0, "Angle", ""},
    {0, nullptr, 0, nullptr, nullptr},
};

}  // namespace blender

#ifdef RNA_RUNTIME

#  include <fmt/format.h>

#  include "BLI_listbase.hh"
#  include "BLI_listbase_iterator.hh"
#  include "BLI_string.hh"
#  include "BLI_string_utf8.hh"
#  include "BLI_string_utils.hh"

#  include "BKE_cad.hh"
#  include "BKE_idprop.hh"
#  include "BKE_report.hh"

#  include "DEG_depsgraph.hh"

#  include "WM_api.hh"
#  include "WM_types.hh"

namespace blender {

static CADPart *rna_cad_part(const PointerRNA *ptr)
{
  return id_cast<CADPart *>(ptr->owner_id);
}

static void rna_cad_tag(ID *id)
{
  DEG_id_tag_update(id, ID_RECALC_GEOMETRY);
  WM_main_add_notifier(NC_GEOM | ND_DATA, id);
}

static void rna_CADPart_update_data(Main * /*bmain*/, Scene * /*scene*/, PointerRNA *ptr)
{
  rna_cad_tag(ptr->owner_id);
}

static void rna_CADPart_update_ui(Main * /*bmain*/, Scene * /*scene*/, PointerRNA *ptr)
{
  WM_main_add_notifier(NC_GEOM | ND_DATA, ptr->owner_id);
}

/** The feature owning `sketch`, for paths. */
static const CADFeature *rna_cad_sketch_feature(const CADPart *part, const CADSketch *sketch)
{
  for (const CADFeature &feature_item : part->features) {
    const CADFeature *feature = &feature_item;
    if (feature->sketch == sketch) {
      return feature;
    }
  }
  return nullptr;
}

/** The sketch whose element array contains `elem`. */
template<typename T>
static const CADFeature *rna_cad_element_feature(const CADPart *part,
                                                 const T *elem,
                                                 T *CADSketch::*array,
                                                 int CADSketch::*num,
                                                 int *r_index)
{
  for (const CADFeature &feature_item : part->features) {
    const CADFeature *feature = &feature_item;
    const CADSketch *sketch = feature->sketch;
    if (sketch && sketch->*array && elem >= sketch->*array &&
        elem < sketch->*array + sketch->*num)
    {
      *r_index = int(elem - sketch->*array);
      return feature;
    }
  }
  return nullptr;
}

static std::string rna_cad_feature_path_prefix(const CADFeature *feature)
{
  return fmt::format("features[\"{}\"]", BLI_str_escape(feature->name));
}

/* -------------------------------------------------------------------- */
/** \name CADPart
 * \{ */

static PointerRNA rna_CADPart_feature_pointer(PointerRNA *ptr, const int uid)
{
  CADPart *part = rna_cad_part(ptr);
  CADFeature *feature = BKE_cad_feature_find(part, uid);
  return RNA_pointer_create_with_parent(*ptr, RNA_CADFeature, feature);
}

static bool rna_CADPart_feature_poll(PointerRNA *ptr, const PointerRNA value)
{
  return value.owner_id == ptr->owner_id;
}

static PointerRNA rna_CADPart_active_feature_get(PointerRNA *ptr)
{
  return rna_CADPart_feature_pointer(ptr, rna_cad_part(ptr)->active_feature_uid);
}

static void rna_CADPart_active_feature_set(PointerRNA *ptr,
                                           PointerRNA value,
                                           ReportList * /*reports*/)
{
  const CADFeature *feature = value.data_as<CADFeature>();
  rna_cad_part(ptr)->active_feature_uid = feature ? feature->uid : -1;
}

static PointerRNA rna_CADPart_rollback_feature_get(PointerRNA *ptr)
{
  return rna_CADPart_feature_pointer(ptr, rna_cad_part(ptr)->rollback_uid);
}

static void rna_CADPart_rollback_feature_set(PointerRNA *ptr,
                                             PointerRNA value,
                                             ReportList * /*reports*/)
{
  const CADFeature *feature = value.data_as<CADFeature>();
  rna_cad_part(ptr)->rollback_uid = feature ? feature->uid : -1;
}

static int rna_CADPart_active_feature_index_get(PointerRNA *ptr)
{
  const CADPart *part = rna_cad_part(ptr);
  int index = 0;
  for (const CADFeature &feature_item : part->features) {
    const CADFeature *feature = &feature_item;
    if (feature->uid == part->active_feature_uid) {
      return index;
    }
    index++;
  }
  return -1;
}

static void rna_CADPart_active_feature_index_set(PointerRNA *ptr, int value)
{
  CADPart *part = rna_cad_part(ptr);
  const CADFeature *feature = static_cast<const CADFeature *>(
      BLI_findlink(&part->features, value));
  part->active_feature_uid = feature ? feature->uid : -1;
}

static void rna_CADPart_active_feature_index_range(
    PointerRNA *ptr, int *min, int *max, int * /*softmin*/, int * /*softmax*/)
{
  *min = -1;
  *max = std::max(0, BLI_listbase_count(&rna_cad_part(ptr)->features) - 1);
}

static CADFeature *rna_CADFeatures_new(CADPart *part, int type, const char *name)
{
  CADFeature *feature = BKE_cad_feature_add(part, eCADFeatureType(type));
  if (name && name[0]) {
    STRNCPY_UTF8(feature->name, name);
    BLI_uniquename(&part->features,
                   feature,
                   BKE_cad_feature_type_name(eCADFeatureType(type)),
                   '.',
                   offsetof(CADFeature, name),
                   sizeof(feature->name));
  }
  rna_cad_tag(&part->id);
  return feature;
}

static void rna_CADFeatures_remove(CADPart *part, ReportList *reports, PointerRNA *feature_ptr)
{
  CADFeature *feature = feature_ptr->data_as<CADFeature>();
  if (BLI_findindex(&part->features, feature) == -1) {
    BKE_reportf(reports, RPT_ERROR, "Feature '%s' not found in part", feature->name);
    return;
  }
  BKE_cad_feature_remove(part, feature);
  feature_ptr->invalidate();
  rna_cad_tag(&part->id);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name CADFeature
 * \{ */

static std::optional<std::string> rna_CADFeature_path(const PointerRNA *ptr)
{
  return rna_cad_feature_path_prefix(ptr->data_as<CADFeature>());
}

static IDProperty **rna_CADFeature_idprops(PointerRNA *ptr)
{
  return &ptr->data_as<CADFeature>()->params;
}

static void rna_CADFeature_name_set(PointerRNA *ptr, const char *value)
{
  CADPart *part = rna_cad_part(ptr);
  CADFeature *feature = ptr->data_as<CADFeature>();
  STRNCPY_UTF8(feature->name, value);
  BLI_uniquename(&part->features,
                 feature,
                 BKE_cad_feature_type_name(eCADFeatureType(feature->type)),
                 '.',
                 offsetof(CADFeature, name),
                 sizeof(feature->name));
}

static PointerRNA rna_CADFeature_sketch_get(PointerRNA *ptr)
{
  CADFeature *feature = ptr->data_as<CADFeature>();
  return RNA_pointer_create_with_parent(*ptr, RNA_CADSketch, feature->sketch);
}

static int rna_CADFeature_status_get(PointerRNA *ptr)
{
  const CADFeature *feature = ptr->data_as<CADFeature>();
  const bke::CADFeatureStatus *status = BKE_cad_feature_status(rna_cad_part(ptr), feature->uid);
  return status ? status->status + 1 : 0;
}

static void rna_CADFeature_status_message_get(PointerRNA *ptr, char *value)
{
  const CADFeature *feature = ptr->data_as<CADFeature>();
  const bke::CADFeatureStatus *status = BKE_cad_feature_status(rna_cad_part(ptr), feature->uid);
  strcpy(value, status ? status->message.c_str() : "");
}

static int rna_CADFeature_status_message_length(PointerRNA *ptr)
{
  const CADFeature *feature = ptr->data_as<CADFeature>();
  const bke::CADFeatureStatus *status = BKE_cad_feature_status(rna_cad_part(ptr), feature->uid);
  return status ? int(status->message.size()) : 0;
}

static bool rna_CADFeature_is_active_get(PointerRNA *ptr)
{
  return rna_cad_part(ptr)->active_feature_uid == ptr->data_as<CADFeature>()->uid;
}

static void rna_CADFeature_refs_begin(CollectionPropertyIterator *iter, PointerRNA *ptr)
{
  CADFeature *feature = ptr->data_as<CADFeature>();
  rna_iterator_array_begin(
      iter, ptr, feature->refs, sizeof(CADTopoRef), feature->refs_num, false, nullptr);
}

static int rna_CADFeature_refs_length(PointerRNA *ptr)
{
  return ptr->data_as<CADFeature>()->refs_num;
}

static void rna_CADFeature_ref_add(ID *id,
                                   CADFeature *feature,
                                   const char *name,
                                   const float point[3])
{
  BKE_cad_feature_ref_add(feature, name, double3(point[0], point[1], point[2]));
  rna_cad_tag(id);
}

static void rna_CADFeature_refs_clear(ID *id, CADFeature *feature)
{
  BKE_cad_feature_refs_clear(feature);
  rna_cad_tag(id);
}

static void rna_CADFeature_regions_set(ID *id, CADFeature *feature, const int *regions, int num)
{
  IDProperty *&params = feature->params;
  if (!params) {
    params = bke::idprop::create_group("params").release();
  }
  if (IDProperty *old = IDP_GetPropertyFromGroup(params, "regions")) {
    IDP_FreeFromGroup(params, old);
  }
  if (num > 0) {
    IDP_AddToGroup(params, bke::idprop::create("regions", Span<int>(regions, num)).release());
  }
  rna_cad_tag(id);
}

static void rna_CADTopoRef_name_get(PointerRNA *ptr, char *value)
{
  const CADTopoRef *ref = ptr->data_as<CADTopoRef>();
  strcpy(value, ref->name ? ref->name : "");
}

static int rna_CADTopoRef_name_length(PointerRNA *ptr)
{
  const CADTopoRef *ref = ptr->data_as<CADTopoRef>();
  return ref->name ? int(strlen(ref->name)) : 0;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name CADSketch
 * \{ */

static std::optional<std::string> rna_CADSketch_path(const PointerRNA *ptr)
{
  const CADFeature *feature = rna_cad_sketch_feature(rna_cad_part(ptr),
                                                     ptr->data_as<CADSketch>());
  if (!feature) {
    return std::nullopt;
  }
  return rna_cad_feature_path_prefix(feature) + ".sketch";
}

template<typename T>
static std::optional<std::string> rna_cad_element_path(const PointerRNA *ptr,
                                                       T *CADSketch::*array,
                                                       int CADSketch::*num,
                                                       const char *collection)
{
  int index = 0;
  const CADFeature *feature = rna_cad_element_feature(
      rna_cad_part(ptr), ptr->data_as<T>(), array, num, &index);
  if (!feature) {
    return std::nullopt;
  }
  return fmt::format("{}.sketch.{}[{}]", rna_cad_feature_path_prefix(feature), collection, index);
}

static std::optional<std::string> rna_CADSketchPoint_path(const PointerRNA *ptr)
{
  return rna_cad_element_path(ptr, &CADSketch::points, &CADSketch::points_num, "points");
}

static std::optional<std::string> rna_CADSketchCurve_path(const PointerRNA *ptr)
{
  return rna_cad_element_path(ptr, &CADSketch::curves, &CADSketch::curves_num, "curves");
}

static std::optional<std::string> rna_CADSketchConstraint_path(const PointerRNA *ptr)
{
  return rna_cad_element_path(
      ptr, &CADSketch::constraints, &CADSketch::constraints_num, "constraints");
}

static bool rna_cad_points_exist(const CADSketch *sketch,
                                 ReportList *reports,
                                 const std::initializer_list<int> uids)
{
  for (const int uid : uids) {
    if (!BKE_cad_sketch_point_find(sketch, uid)) {
      BKE_reportf(reports, RPT_ERROR, "No sketch point with uid %d", uid);
      return false;
    }
  }
  return true;
}

static int rna_CADSketch_point_add(ID *id, CADSketch *sketch, const float co[2])
{
  const int uid = BKE_cad_sketch_point_add(id_cast<CADPart *>(id), sketch, double2(co[0], co[1]));
  rna_cad_tag(id);
  return uid;
}

static int rna_CADSketch_line_add(ID *id, CADSketch *sketch, ReportList *reports, int p0, int p1)
{
  if (!rna_cad_points_exist(sketch, reports, {p0, p1})) {
    return -1;
  }
  const int uid = BKE_cad_sketch_line_add(id_cast<CADPart *>(id), sketch, p0, p1);
  rna_cad_tag(id);
  return uid;
}

static int rna_CADSketch_circle_add(
    ID *id, CADSketch *sketch, ReportList *reports, int center, float radius)
{
  if (!rna_cad_points_exist(sketch, reports, {center})) {
    return -1;
  }
  const int uid = BKE_cad_sketch_circle_add(id_cast<CADPart *>(id), sketch, center, radius);
  rna_cad_tag(id);
  return uid;
}

static int rna_CADSketch_arc_add(ID *id,
                                 CADSketch *sketch,
                                 ReportList *reports,
                                 int center,
                                 int start,
                                 int end,
                                 bool ccw)
{
  if (!rna_cad_points_exist(sketch, reports, {center, start, end})) {
    return -1;
  }
  const int uid = BKE_cad_sketch_arc_add(id_cast<CADPart *>(id), sketch, center, start, end, ccw);
  rna_cad_tag(id);
  return uid;
}

static int rna_CADSketch_constraint_add(
    ID *id, CADSketch *sketch, int type, const int *entities, int entities_num, float value)
{
  const int uid = BKE_cad_sketch_constraint_add(id_cast<CADPart *>(id),
                                                sketch,
                                                eCADConstraintType(type),
                                                Span<int>(entities, entities_num),
                                                value);
  rna_cad_tag(id);
  return uid;
}

/** \} */

}  // namespace blender

#else

namespace blender {

static void rna_def_cad_topo_ref(BlenderRNA *brna)
{
  StructRNA *srna = RNA_def_struct(brna, "CADTopoRef", nullptr);
  RNA_def_struct_ui_text(srna, "CAD Reference", "Persistent reference to a face or edge");

  PropertyRNA *prop = RNA_def_property(srna, "name", PROP_STRING, PROP_NONE);
  RNA_def_property_string_funcs(
      prop, "rna_CADTopoRef_name_get", "rna_CADTopoRef_name_length", nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Name", "Persistent name of the referenced element");

  prop = RNA_def_property(srna, "point", PROP_FLOAT, PROP_TRANSLATION);
  RNA_def_property_float_sdna(prop, nullptr, "point");
  RNA_def_property_array(prop, 3);
  RNA_def_property_ui_text(prop, "Point", "Picked point, used when the name is not found");
  RNA_def_property_update(prop, 0, "rna_CADPart_update_data");

  prop = RNA_def_property(srna, "is_lost", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", CAD_REF_LOST);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Lost", "The last evaluation could not find the element");
}

static void rna_def_cad_sketch_elements(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  srna = RNA_def_struct(brna, "CADSketchPoint", nullptr);
  RNA_def_struct_ui_text(srna, "Sketch Point", "");
  RNA_def_struct_path_func(srna, "rna_CADSketchPoint_path");

  prop = RNA_def_property(srna, "co", PROP_FLOAT, PROP_XYZ_LENGTH);
  RNA_def_property_float_sdna(prop, nullptr, "co");
  RNA_def_property_array(prop, 2);
  RNA_def_property_ui_text(prop, "Location", "Position on the sketch plane");
  RNA_def_property_update(prop, 0, "rna_CADPart_update_data");

  prop = RNA_def_property(srna, "uid", PROP_INT, PROP_NONE);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "UID", "");

  prop = RNA_def_property(srna, "select", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", CAD_SKETCH_SELECT);
  RNA_def_property_ui_text(prop, "Select", "");
  RNA_def_property_update(prop, 0, "rna_CADPart_update_ui");

  prop = RNA_def_property(srna, "fixed", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", CAD_SKETCH_FIXED);
  RNA_def_property_ui_text(prop, "Fixed", "");
  RNA_def_property_update(prop, 0, "rna_CADPart_update_data");

  srna = RNA_def_struct(brna, "CADSketchCurve", nullptr);
  RNA_def_struct_ui_text(srna, "Sketch Curve", "");
  RNA_def_struct_path_func(srna, "rna_CADSketchCurve_path");

  prop = RNA_def_property(srna, "uid", PROP_INT, PROP_NONE);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "UID", "");

  prop = RNA_def_property(srna, "type", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, rna_enum_cad_sketch_curve_type_items);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Type", "");

  prop = RNA_def_property(srna, "points", PROP_INT, PROP_NONE);
  RNA_def_property_int_sdna(prop, nullptr, "points");
  RNA_def_property_array(prop, 3);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Points", "Start, end and center point uids (-1 if unused)");

  prop = RNA_def_property(srna, "radius", PROP_FLOAT, PROP_DISTANCE);
  RNA_def_property_range(prop, 0.0, FLT_MAX);
  RNA_def_property_ui_text(prop, "Radius", "Circle radius");
  RNA_def_property_update(prop, 0, "rna_CADPart_update_data");

  prop = RNA_def_property(srna, "use_ccw", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "ccw", 1);
  RNA_def_property_ui_text(prop, "Counter-Clockwise", "Arc direction from start to end");
  RNA_def_property_update(prop, 0, "rna_CADPart_update_data");

  prop = RNA_def_property(srna, "construction", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", CAD_SKETCH_CONSTRUCTION);
  RNA_def_property_ui_text(prop, "Construction", "Reference geometry, not part of profiles");
  RNA_def_property_update(prop, 0, "rna_CADPart_update_data");

  prop = RNA_def_property(srna, "select", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", CAD_SKETCH_SELECT);
  RNA_def_property_ui_text(prop, "Select", "");
  RNA_def_property_update(prop, 0, "rna_CADPart_update_ui");

  srna = RNA_def_struct(brna, "CADSketchConstraint", nullptr);
  RNA_def_struct_ui_text(srna, "Sketch Constraint", "Geometric constraint or dimension");
  RNA_def_struct_path_func(srna, "rna_CADSketchConstraint_path");

  prop = RNA_def_property(srna, "uid", PROP_INT, PROP_NONE);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "UID", "");

  prop = RNA_def_property(srna, "type", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, rna_enum_cad_constraint_type_items);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Type", "");

  prop = RNA_def_property(srna, "entities", PROP_INT, PROP_NONE);
  RNA_def_property_array(prop, 4);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Entities", "Constrained point or curve uids (-1 if unused)");

  prop = RNA_def_property(srna, "value", PROP_FLOAT, PROP_NONE);
  RNA_def_property_ui_text(prop, "Value", "Dimension value (scene length or radians)");
  RNA_def_property_update(prop, 0, "rna_CADPart_update_data");
}

static void rna_def_cad_sketch(BlenderRNA *brna)
{
  StructRNA *srna = RNA_def_struct(brna, "CADSketch", nullptr);
  RNA_def_struct_ui_text(srna, "CAD Sketch", "Constrained 2D geometry on a plane");
  RNA_def_struct_path_func(srna, "rna_CADSketch_path");

  PropertyRNA *prop = RNA_def_property(srna, "origin", PROP_FLOAT, PROP_TRANSLATION);
  RNA_def_property_array(prop, 3);
  RNA_def_property_ui_text(prop, "Origin", "Sketch plane origin");
  RNA_def_property_update(prop, 0, "rna_CADPart_update_data");

  prop = RNA_def_property(srna, "x_axis", PROP_FLOAT, PROP_DIRECTION);
  RNA_def_property_float_sdna(prop, nullptr, "x_dir");
  RNA_def_property_array(prop, 3);
  RNA_def_property_ui_text(prop, "X Axis", "Direction of the sketch X axis");
  RNA_def_property_update(prop, 0, "rna_CADPart_update_data");

  prop = RNA_def_property(srna, "normal", PROP_FLOAT, PROP_DIRECTION);
  RNA_def_property_array(prop, 3);
  RNA_def_property_ui_text(prop, "Normal", "Sketch plane normal");
  RNA_def_property_update(prop, 0, "rna_CADPart_update_data");

  prop = RNA_def_property(srna, "points", PROP_COLLECTION, PROP_NONE);
  RNA_def_property_collection_sdna(prop, nullptr, "points", "points_num");
  RNA_def_property_struct_type(prop, "CADSketchPoint");
  RNA_def_property_ui_text(prop, "Points", "");

  prop = RNA_def_property(srna, "curves", PROP_COLLECTION, PROP_NONE);
  RNA_def_property_collection_sdna(prop, nullptr, "curves", "curves_num");
  RNA_def_property_struct_type(prop, "CADSketchCurve");
  RNA_def_property_ui_text(prop, "Curves", "");

  prop = RNA_def_property(srna, "constraints", PROP_COLLECTION, PROP_NONE);
  RNA_def_property_collection_sdna(prop, nullptr, "constraints", "constraints_num");
  RNA_def_property_struct_type(prop, "CADSketchConstraint");
  RNA_def_property_ui_text(prop, "Constraints", "");

  FunctionRNA *func;
  PropertyRNA *parm;

  func = RNA_def_function(srna, "point_add", "rna_CADSketch_point_add");
  RNA_def_function_flag(func, FUNC_USE_SELF_ID);
  RNA_def_function_ui_description(func, "Add a point, returns its uid");
  parm = RNA_def_float_vector(
      func, "co", 2, nullptr, -FLT_MAX, FLT_MAX, "Location", "", -FLT_MAX, FLT_MAX);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  RNA_def_function_return(func, RNA_def_int(func, "uid", 0, INT_MIN, INT_MAX, "", "", 0, 0));

  func = RNA_def_function(srna, "line_add", "rna_CADSketch_line_add");
  RNA_def_function_flag(func, FUNC_USE_SELF_ID | FUNC_USE_REPORTS);
  RNA_def_function_ui_description(func, "Add a line between two points, returns its uid");
  parm = RNA_def_int(func, "start", 0, INT_MIN, INT_MAX, "Start", "Point uid", 0, 0);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_int(func, "end", 0, INT_MIN, INT_MAX, "End", "Point uid", 0, 0);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  RNA_def_function_return(func, RNA_def_int(func, "uid", 0, INT_MIN, INT_MAX, "", "", 0, 0));

  func = RNA_def_function(srna, "circle_add", "rna_CADSketch_circle_add");
  RNA_def_function_flag(func, FUNC_USE_SELF_ID | FUNC_USE_REPORTS);
  RNA_def_function_ui_description(func, "Add a circle, returns its uid");
  parm = RNA_def_int(func, "center", 0, INT_MIN, INT_MAX, "Center", "Point uid", 0, 0);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_float(func, "radius", 1.0f, 0.0f, FLT_MAX, "Radius", "", 0.0f, FLT_MAX);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  RNA_def_function_return(func, RNA_def_int(func, "uid", 0, INT_MIN, INT_MAX, "", "", 0, 0));

  func = RNA_def_function(srna, "arc_add", "rna_CADSketch_arc_add");
  RNA_def_function_flag(func, FUNC_USE_SELF_ID | FUNC_USE_REPORTS);
  RNA_def_function_ui_description(func, "Add a circular arc, returns its uid");
  parm = RNA_def_int(func, "center", 0, INT_MIN, INT_MAX, "Center", "Point uid", 0, 0);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_int(func, "start", 0, INT_MIN, INT_MAX, "Start", "Point uid", 0, 0);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_int(func, "end", 0, INT_MIN, INT_MAX, "End", "Point uid", 0, 0);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  RNA_def_boolean(func, "ccw", true, "Counter-Clockwise", "");
  RNA_def_function_return(func, RNA_def_int(func, "uid", 0, INT_MIN, INT_MAX, "", "", 0, 0));

  func = RNA_def_function(srna, "constraint_add", "rna_CADSketch_constraint_add");
  RNA_def_function_flag(func, FUNC_USE_SELF_ID);
  RNA_def_function_ui_description(func, "Add a constraint or dimension, returns its uid");
  parm = RNA_def_enum(func, "type", rna_enum_cad_constraint_type_items, 0, "Type", "");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_int_array(
      func, "entities", 1, nullptr, INT_MIN, INT_MAX, "Entities", "Uids", INT_MIN, INT_MAX);
  RNA_def_property_flag(parm, PROP_DYNAMIC);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  RNA_def_float(func, "value", 0.0f, -FLT_MAX, FLT_MAX, "Value", "", -FLT_MAX, FLT_MAX);
  RNA_def_function_return(func, RNA_def_int(func, "uid", 0, INT_MIN, INT_MAX, "", "", 0, 0));
}

static void rna_def_cad_feature(BlenderRNA *brna)
{
  StructRNA *srna = RNA_def_struct(brna, "CADFeature", nullptr);
  RNA_def_struct_ui_text(srna,
                         "CAD Feature",
                         "Step of a CAD part's history; parameters are ID properties "
                         "(feature[\"distance\"]) so they can be animated and driven");
  RNA_def_struct_path_func(srna, "rna_CADFeature_path");
  RNA_def_struct_idprops_func(srna, "rna_CADFeature_idprops");

  PropertyRNA *prop = RNA_def_property(srna, "name", PROP_STRING, PROP_NONE);
  RNA_def_property_string_funcs(prop, nullptr, nullptr, "rna_CADFeature_name_set");
  RNA_def_property_ui_text(prop, "Name", "");
  RNA_def_struct_name_property(srna, prop);
  RNA_def_property_update(prop, 0, "rna_CADPart_update_ui");

  prop = RNA_def_property(srna, "type", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, rna_enum_cad_feature_type_items);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Type", "");

  prop = RNA_def_property(srna, "uid", PROP_INT, PROP_NONE);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "UID", "Unique identifier within the part");

  prop = RNA_def_property(srna, "suppress", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", CAD_FEATURE_SUPPRESSED);
  RNA_def_property_ui_text(prop, "Suppress", "Skip this feature when evaluating");
  RNA_def_property_ui_icon(prop, ICON_HIDE_OFF, 1);
  RNA_def_property_update(prop, 0, "rna_CADPart_update_data");

  prop = RNA_def_property(srna, "show_expanded", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", CAD_FEATURE_EXPANDED);
  RNA_def_property_flag(prop, PROP_NO_DEG_UPDATE);
  RNA_def_property_ui_text(prop, "Expanded", "");
  RNA_def_property_update(prop, 0, "rna_CADPart_update_ui");

  prop = RNA_def_property(srna, "is_active", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(prop, "rna_CADFeature_is_active_get", nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Active", "");

  prop = RNA_def_property(srna, "sketch", PROP_POINTER, PROP_NONE);
  RNA_def_property_struct_type(prop, "CADSketch");
  RNA_def_property_pointer_funcs(prop, "rna_CADFeature_sketch_get", nullptr, nullptr, nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Sketch", "Sketch geometry (sketch features only)");

  prop = RNA_def_property(srna, "status", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, rna_enum_cad_feature_status_items);
  RNA_def_property_enum_funcs(prop, "rna_CADFeature_status_get", nullptr, nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Status", "Result of the last evaluation");

  prop = RNA_def_property(srna, "status_message", PROP_STRING, PROP_NONE);
  RNA_def_property_string_funcs(
      prop, "rna_CADFeature_status_message_get", "rna_CADFeature_status_message_length", nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Status Message", "Error or warning of the last evaluation");

  prop = RNA_def_property(srna, "references", PROP_COLLECTION, PROP_NONE);
  RNA_def_property_struct_type(prop, "CADTopoRef");
  RNA_def_property_collection_funcs(prop,
                                    "rna_CADFeature_refs_begin",
                                    "rna_iterator_array_next",
                                    "rna_iterator_array_end",
                                    "rna_iterator_array_get",
                                    "rna_CADFeature_refs_length",
                                    nullptr,
                                    nullptr,
                                    nullptr);
  RNA_def_property_ui_text(prop, "References", "Faces or edges this feature works on");

  FunctionRNA *func;
  PropertyRNA *parm;

  func = RNA_def_function(srna, "reference_add", "rna_CADFeature_ref_add");
  RNA_def_function_flag(func, FUNC_USE_SELF_ID);
  RNA_def_function_ui_description(func, "Reference a face or edge by persistent name");
  parm = RNA_def_string(func, "name", nullptr, 0, "Name", "Persistent element name");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  RNA_def_float_vector(func,
                       "point",
                       3,
                       nullptr,
                       -FLT_MAX,
                       FLT_MAX,
                       "Point",
                       "Point on the element (part space)",
                       -FLT_MAX,
                       FLT_MAX);

  func = RNA_def_function(srna, "references_clear", "rna_CADFeature_refs_clear");
  RNA_def_function_flag(func, FUNC_USE_SELF_ID);
  RNA_def_function_ui_description(func, "Remove all references");

  func = RNA_def_function(srna, "regions_set", "rna_CADFeature_regions_set");
  RNA_def_function_flag(func, FUNC_USE_SELF_ID);
  RNA_def_function_ui_description(
      func, "Set the sketch regions an Extrude uses (region keys, empty for all)");
  parm = RNA_def_int_array(
      func, "regions", 1, nullptr, INT_MIN, INT_MAX, "Regions", "", INT_MIN, INT_MAX);
  RNA_def_property_flag(parm, PROP_DYNAMIC);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
}

static void rna_def_cad_features(BlenderRNA *brna, PropertyRNA *cprop)
{
  RNA_def_property_srna(cprop, "CADFeatures");
  StructRNA *srna = RNA_def_struct(brna, "CADFeatures", nullptr);
  RNA_def_struct_sdna(srna, "CADPart");
  RNA_def_struct_ui_text(srna, "CAD Features", "Feature history of a CAD part");

  FunctionRNA *func = RNA_def_function(srna, "new", "rna_CADFeatures_new");
  RNA_def_function_ui_description(func, "Append a feature with default parameters");
  PropertyRNA *parm = RNA_def_enum(
      func, "type", rna_enum_cad_feature_type_items, CAD_FEATURE_BOX, "Type", "");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  RNA_def_string(func, "name", nullptr, 64, "Name", "Name (generated when empty)");
  parm = RNA_def_pointer(func, "feature", "CADFeature", "", "New feature");
  RNA_def_function_return(func, parm);

  func = RNA_def_function(srna, "remove", "rna_CADFeatures_remove");
  RNA_def_function_flag(func, FUNC_USE_REPORTS);
  RNA_def_function_ui_description(func, "Remove a feature");
  parm = RNA_def_pointer(func, "feature", "CADFeature", "", "Feature to remove");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED | PARM_RNAPTR);
  RNA_def_parameter_clear_flags(parm, PROP_THICK_WRAP, ParameterFlag(0));

  PropertyRNA *prop = RNA_def_property(srna, "active", PROP_POINTER, PROP_NONE);
  RNA_def_property_struct_type(prop, "CADFeature");
  RNA_def_property_pointer_funcs(prop,
                                 "rna_CADPart_active_feature_get",
                                 "rna_CADPart_active_feature_set",
                                 nullptr,
                                 "rna_CADPart_feature_poll");
  RNA_def_property_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Active Feature", "");
  RNA_def_property_update(prop, 0, "rna_CADPart_update_ui");

  prop = RNA_def_property(srna, "active_index", PROP_INT, PROP_UNSIGNED);
  RNA_def_property_int_funcs(prop,
                             "rna_CADPart_active_feature_index_get",
                             "rna_CADPart_active_feature_index_set",
                             "rna_CADPart_active_feature_index_range");
  RNA_def_property_flag(prop, PROP_NO_DEG_UPDATE);
  RNA_def_property_ui_text(prop, "Active Feature Index", "");
  RNA_def_property_update(prop, 0, "rna_CADPart_update_ui");
}

static void rna_def_cad_part(BlenderRNA *brna)
{
  StructRNA *srna = RNA_def_struct(brna, "CADPart", "ID");
  RNA_def_struct_ui_text(srna, "CAD Part", "Parametric B-rep part built from a feature history");
  RNA_def_struct_ui_icon(srna, ICON_MESH_CUBE);

  PropertyRNA *prop = RNA_def_property(srna, "features", PROP_COLLECTION, PROP_NONE);
  RNA_def_property_collection_sdna(prop, nullptr, "features", nullptr);
  RNA_def_property_struct_type(prop, "CADFeature");
  RNA_def_property_ui_text(prop, "Features", "Feature history, in evaluation order");
  rna_def_cad_features(brna, prop);

  prop = RNA_def_property(srna, "kernel", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, rna_enum_cad_kernel_items);
  RNA_def_property_ui_text(prop, "Kernel", "B-rep modeling kernel that evaluates this part");
  RNA_def_property_update(prop, 0, "rna_CADPart_update_data");

  prop = RNA_def_property(srna, "rollback_feature", PROP_POINTER, PROP_NONE);
  RNA_def_property_struct_type(prop, "CADFeature");
  RNA_def_property_pointer_funcs(prop,
                                 "rna_CADPart_rollback_feature_get",
                                 "rna_CADPart_rollback_feature_set",
                                 nullptr,
                                 "rna_CADPart_feature_poll");
  RNA_def_property_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(
      prop, "Rollback", "Evaluate up to and including this feature (None: all features)");
  RNA_def_property_update(prop, 0, "rna_CADPart_update_data");

  prop = RNA_def_property(srna, "mm_per_unit", PROP_FLOAT, PROP_NONE);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(
      prop, "Millimeters per Unit", "Model scale, fixed when the part is created");

  prop = RNA_def_property(srna, "show_edges", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_negative_sdna(prop, nullptr, "flag", CAD_PART_HIDE_EDGES);
  RNA_def_property_ui_text(prop, "Show Edges", "Draw the edges of the solid in the viewport");
  RNA_def_property_update(prop, 0, "rna_CADPart_update_ui");

  prop = RNA_def_property(srna, "chord_tolerance", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, nullptr, "linear_deflection");
  RNA_def_property_range(prop, 1e-4, 100.0);
  RNA_def_property_ui_range(prop, 0.001, 1.0, 0.1, 3);
  RNA_def_property_ui_text(
      prop, "Chord Tolerance", "Maximum distance between mesh and exact surface, in millimeters");
  RNA_def_property_update(prop, 0, "rna_CADPart_update_data");

  prop = RNA_def_property(srna, "angle_tolerance", PROP_FLOAT, PROP_ANGLE);
  RNA_def_property_float_sdna(prop, nullptr, "angular_deflection");
  RNA_def_property_range(prop, 0.0174533f, 1.0471976f);
  RNA_def_property_ui_text(
      prop, "Angle Tolerance", "Maximum angle between neighboring mesh normals on curved faces");
  RNA_def_property_update(prop, 0, "rna_CADPart_update_data");

  /* materials */
  prop = RNA_def_property(srna, "materials", PROP_COLLECTION, PROP_NONE);
  RNA_def_property_collection_sdna(prop, nullptr, "mat", "totcol");
  RNA_def_property_struct_type(prop, "Material");
  RNA_def_property_ui_text(prop, "Materials", "");
  RNA_def_property_srna(prop, "IDMaterials"); /* see rna_ID.cc */
  RNA_def_property_collection_funcs(prop,
                                    nullptr,
                                    nullptr,
                                    nullptr,
                                    nullptr,
                                    nullptr,
                                    nullptr,
                                    nullptr,
                                    "rna_IDMaterials_assign_int");

  rna_def_animdata_common(srna);
}

void RNA_def_cad(BlenderRNA *brna)
{
  rna_def_cad_topo_ref(brna);
  rna_def_cad_sketch_elements(brna);
  rna_def_cad_sketch(brna);
  rna_def_cad_feature(brna);
  rna_def_cad_part(brna);
}

}  // namespace blender

#endif
