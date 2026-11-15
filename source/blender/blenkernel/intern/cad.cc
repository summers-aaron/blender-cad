/* SPDX-FileCopyrightText: 2026 SimpleCAD Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * #CADPart data-block: ID type callbacks and timeline editing.
 */

#include <algorithm>
#include <cmath>
#include <cstring>

#include "MEM_guardedalloc.h"

#include "DNA_cad_types.h"
#include "DNA_material_types.h"

#include "BLI_listbase.hh"
#include "BLI_listbase_iterator.hh"
#include "BLI_string.hh"
#include "BLI_string_utf8.hh"
#include "BLI_string_utils.hh"
#include "BLI_utildefines.hh"

#include "BKE_anim_data.hh"
#include "BKE_cad.hh"
#include "BKE_idprop.hh"
#include "BKE_idtype.hh"
#include "BKE_lib_id.hh"
#include "BKE_lib_query.hh"

#include "BLT_translation.hh"

#include "BLO_read_write.hh"

#include "RNA_types.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Deep Copy / Free Helpers
 * \{ */

template<typename T> static T *dup_array(const T *src, const int num)
{
  if (!src || num <= 0) {
    return nullptr;
  }
  T *dst = MEM_new_array<T>(size_t(num), __func__);
  std::copy_n(src, num, dst);
  return dst;
}

template<typename T> static void array_append(T *&array, int &num, const T &value)
{
  T *grown = MEM_new_array<T>(size_t(num + 1), __func__);
  if (array) {
    std::copy_n(array, num, grown);
    MEM_delete(array);
  }
  grown[num] = value;
  array = grown;
  num++;
}

static char *dup_string(const char *s)
{
  return s ? BLI_strdup(s) : nullptr;
}

static CADSketch *sketch_copy(const CADSketch *src)
{
  CADSketch *dst = MEM_dupalloc(src);
  dst->points = dup_array(src->points, src->points_num);
  dst->curves = dup_array(src->curves, src->curves_num);
  dst->constraints = dup_array(src->constraints, src->constraints_num);
  for (int i = 0; i < dst->constraints_num; i++) {
    dst->constraints[i].expression = dup_string(src->constraints[i].expression);
  }
  return dst;
}

static void sketch_free(CADSketch *sketch)
{
  if (!sketch) {
    return;
  }
  for (int i = 0; i < sketch->constraints_num; i++) {
    MEM_SAFE_DELETE(sketch->constraints[i].expression);
  }
  MEM_SAFE_DELETE(sketch->points);
  MEM_SAFE_DELETE(sketch->curves);
  MEM_SAFE_DELETE(sketch->constraints);
  MEM_delete(sketch);
}

static CADFeature *feature_copy(const CADFeature *src, const int flag)
{
  CADFeature *dst = MEM_dupalloc(src);
  dst->next = dst->prev = nullptr;
  dst->params = src->params ? IDP_CopyProperty_ex(src->params, flag) : nullptr;
  dst->refs = dup_array(src->refs, src->refs_num);
  for (int i = 0; i < dst->refs_num; i++) {
    dst->refs[i].name = dup_string(src->refs[i].name);
  }
  dst->sketch = src->sketch ? sketch_copy(src->sketch) : nullptr;
  return dst;
}

static void feature_free(CADFeature *feature)
{
  if (feature->params) {
    IDP_FreeProperty(feature->params);
  }
  for (int i = 0; i < feature->refs_num; i++) {
    MEM_SAFE_DELETE(feature->refs[i].name);
  }
  MEM_SAFE_DELETE(feature->refs);
  sketch_free(feature->sketch);
  MEM_delete(feature);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name ID Type
 * \{ */

static void cad_part_init_data(ID *id)
{
  CADPart *part = id_cast<CADPart *>(id);
  INIT_DEFAULT_STRUCT_AFTER(part, id);
  part->runtime = MEM_new<bke::CADPartRuntime>(__func__);
}

static void cad_part_copy_data(Main * /*bmain*/,
                               std::optional<Library *> /*owner_library*/,
                               ID *id_dst,
                               const ID *id_src,
                               const int flag)
{
  CADPart *dst = id_cast<CADPart *>(id_dst);
  const CADPart *src = id_cast<const CADPart *>(id_src);

  BLI_listbase_clear(&dst->features);
  for (const CADFeature &feature_item : src->features) {
    const CADFeature *feature = &feature_item;
    BLI_addtail(&dst->features, feature_copy(feature, flag));
  }
  dst->user_params = dup_array(src->user_params, src->user_params_num);
  for (int i = 0; i < dst->user_params_num; i++) {
    dst->user_params[i].expression = dup_string(src->user_params[i].expression);
  }
  dst->mat = MEM_dupalloc(src->mat);

  dst->runtime = MEM_new<bke::CADPartRuntime>(__func__);
  /* Evaluated copies share the last result: cheap, and the UI keeps showing it. */
  dst->runtime->eval = src->runtime->eval;
  dst->runtime->eval_status = src->runtime->eval_status;
}

static void cad_part_free_data(ID *id)
{
  CADPart *part = id_cast<CADPart *>(id);
  BKE_animdata_free(&part->id, false);
  for (CADFeature &feature_item : part->features.items_mutable()) {
    CADFeature *feature = &feature_item;
    feature_free(feature);
  }
  BLI_listbase_clear(&part->features);
  for (int i = 0; i < part->user_params_num; i++) {
    MEM_SAFE_DELETE(part->user_params[i].expression);
  }
  MEM_SAFE_DELETE(part->user_params);
  part->user_params_num = 0;
  MEM_SAFE_DELETE(part->mat);
  MEM_delete(part->runtime);
  part->runtime = nullptr;
}

static void cad_part_foreach_id(ID *id, LibraryForeachIDData *data)
{
  CADPart *part = id_cast<CADPart *>(id);
  for (int i = 0; i < part->totcol; i++) {
    BKE_LIB_FOREACHID_PROCESS_IDSUPER(data, part->mat[i], IDWALK_CB_USER);
  }
}

static void cad_part_blend_write(BlendWriter *writer, ID *id, const void *id_address)
{
  CADPart *part = id_cast<CADPart *>(id);

  writer->write_id_struct(id_address, part);
  BKE_id_blend_write(writer, &part->id);

  writer->write_struct_list(&part->features);
  for (CADFeature &feature_item : part->features) {
    CADFeature *feature = &feature_item;
    if (feature->params) {
      IDP_BlendWrite(writer, feature->params);
    }
    if (feature->refs) {
      writer->write_struct_array(feature->refs_num, feature->refs);
      for (int i = 0; i < feature->refs_num; i++) {
        writer->write_string(feature->refs[i].name);
      }
    }
    if (CADSketch *sketch = feature->sketch) {
      writer->write_struct(sketch);
      writer->write_struct_array(sketch->points_num, sketch->points);
      writer->write_struct_array(sketch->curves_num, sketch->curves);
      writer->write_struct_array(sketch->constraints_num, sketch->constraints);
      for (int i = 0; i < sketch->constraints_num; i++) {
        writer->write_string(sketch->constraints[i].expression);
      }
    }
  }
  if (part->user_params) {
    writer->write_struct_array(part->user_params_num, part->user_params);
    for (int i = 0; i < part->user_params_num; i++) {
      writer->write_string(part->user_params[i].expression);
    }
  }
  writer->write_pointer_array(part->totcol, part->mat);
}

static void cad_part_blend_read_data(BlendDataReader *reader, ID *id)
{
  CADPart *part = id_cast<CADPart *>(id);

  BLO_read_struct_list(reader, CADFeature, &part->features);
  for (CADFeature &feature_item : part->features) {
    CADFeature *feature = &feature_item;
    BLO_read_struct(reader, IDProperty, &feature->params);
    IDP_BlendDataRead(reader, &feature->params);
    BLO_read_array_and_validate_size(reader, &feature->refs, &feature->refs_num);
    for (int i = 0; i < feature->refs_num; i++) {
      BLO_read_string(reader, &feature->refs[i].name);
    }
    BLO_read_struct(reader, CADSketch, &feature->sketch);
    if (CADSketch *sketch = feature->sketch) {
      BLO_read_array_and_validate_size(reader, &sketch->points, &sketch->points_num);
      BLO_read_array_and_validate_size(reader, &sketch->curves, &sketch->curves_num);
      BLO_read_array_and_validate_size(reader, &sketch->constraints, &sketch->constraints_num);
      for (int i = 0; i < sketch->constraints_num; i++) {
        BLO_read_string(reader, &sketch->constraints[i].expression);
      }
    }
  }
  BLO_read_array_and_validate_size(reader, &part->user_params, &part->user_params_num);
  for (int i = 0; i < part->user_params_num; i++) {
    BLO_read_string(reader, &part->user_params[i].expression);
  }
  BLO_read_pointer_array_and_validate_size(reader, &part->mat, &part->totcol);

  part->runtime = MEM_new<bke::CADPartRuntime>(__func__);
}

IDTypeInfo IDType_ID_CAD = {
    .id_code = CADPart::id_type,
    .id_filter = FILTER_ID_CAD,
    .dependencies_id_types = FILTER_ID_MA,
    .main_listbase_index = INDEX_ID_CAD,
    .struct_size = sizeof(CADPart),
    .name = "CADPart",
    .name_plural = N_("cad_parts"),
    .translation_context = BLT_I18NCONTEXT_ID_CADPART,
    .flags = IDTYPE_FLAGS_APPEND_IS_REUSABLE,
    .asset_type_info = nullptr,

    .init_data = cad_part_init_data,
    .copy_data = cad_part_copy_data,
    .free_data = cad_part_free_data,
    .make_local = nullptr,
    .foreach_id = cad_part_foreach_id,
    .foreach_cache = nullptr,
    .foreach_path = nullptr,
    .foreach_working_space_color = nullptr,
    .foreach_asset_weak_reference = nullptr,
    .owner_pointer_get = nullptr,

    .blend_write = cad_part_blend_write,
    .blend_read_data = cad_part_blend_read_data,
    .blend_read_after_liblink = nullptr,

    .blend_read_undo_preserve = nullptr,

    .lib_override_apply_post = nullptr,
};

CADPart *BKE_cad_part_add(Main *bmain, const char *name)
{
  return BKE_id_new<CADPart>(bmain, name);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Parameters
 * \{ */

static IDProperty *param_find(const CADFeature *feature, const StringRef key)
{
  return feature->params ? IDP_GetPropertyFromGroup(feature->params, key) : nullptr;
}

double BKE_cad_feature_param_get(const CADFeature *feature,
                                 const StringRef key,
                                 const double fallback)
{
  const IDProperty *prop = param_find(feature, key);
  if (!prop) {
    return fallback;
  }
  switch (prop->type) {
    case IDP_DOUBLE:
      return IDP_double_get(prop);
    case IDP_FLOAT:
      return IDP_float_get(prop);
    case IDP_INT:
      return IDP_int_get(prop);
    default:
      return fallback;
  }
}

int BKE_cad_feature_param_get_int(const CADFeature *feature, const StringRef key, int fallback)
{
  const IDProperty *prop = param_find(feature, key);
  if (prop && prop->type == IDP_INT) {
    return IDP_int_get(prop);
  }
  return prop ? int(BKE_cad_feature_param_get(feature, key, fallback)) : fallback;
}

float3 BKE_cad_feature_param_get_float3(const CADFeature *feature,
                                        const StringRef key,
                                        const float3 &fallback)
{
  const IDProperty *prop = param_find(feature, key);
  if (!prop || prop->type != IDP_ARRAY || prop->len != 3) {
    return fallback;
  }
  if (prop->subtype == IDP_DOUBLE) {
    const double *v = IDP_array_double_get(prop);
    return float3(float(v[0]), float(v[1]), float(v[2]));
  }
  if (prop->subtype == IDP_FLOAT) {
    const float *v = IDP_array_float_get(prop);
    return float3(v[0], v[1], v[2]);
  }
  return fallback;
}

static IDProperty *params_ensure(CADFeature *feature)
{
  if (!feature->params) {
    feature->params = bke::idprop::create_group("params").release();
  }
  return feature->params;
}

void BKE_cad_feature_param_set(CADFeature *feature, const StringRef key, const double value)
{
  IDProperty *prop = param_find(feature, key);
  if (prop && prop->type == IDP_DOUBLE) {
    IDP_double_set(prop, value);
    return;
  }
  if (prop) {
    IDP_FreeFromGroup(feature->params, prop);
  }
  IDP_AddToGroup(params_ensure(feature), bke::idprop::create(key, value).release());
}

void BKE_cad_feature_param_set_int(CADFeature *feature, const StringRef key, const int value)
{
  IDProperty *prop = param_find(feature, key);
  if (prop && prop->type == IDP_INT) {
    IDP_int_set(prop, value);
    return;
  }
  if (prop) {
    IDP_FreeFromGroup(feature->params, prop);
  }
  IDP_AddToGroup(params_ensure(feature), bke::idprop::create(key, value).release());
}

/** A length parameter, shown in scene units. */
static void add_length(CADFeature *feature,
                       const StringRef key,
                       const double value,
                       const double soft_min,
                       const char *description)
{
  IDProperty *prop = bke::idprop::create(key, value).release();
  IDPropertyUIDataFloat *ui = reinterpret_cast<IDPropertyUIDataFloat *>(IDP_ui_data_ensure(prop));
  ui->base.rna_subtype = PROP_DISTANCE;
  ui->base.description = BLI_strdup(description);
  ui->soft_min = soft_min;
  ui->soft_max = 1e4;
  ui->min = -1e9;
  ui->max = 1e9;
  ui->step = 1.0f;
  ui->precision = 4;
  ui->default_value = value;
  IDP_AddToGroup(params_ensure(feature), prop);
}

static void add_length3(CADFeature *feature,
                        const StringRef key,
                        const double3 &value,
                        const char *description)
{
  const double values[3] = {value.x, value.y, value.z};
  IDProperty *prop = bke::idprop::create(key, Span<double>(values, 3)).release();
  IDPropertyUIDataFloat *ui = reinterpret_cast<IDPropertyUIDataFloat *>(IDP_ui_data_ensure(prop));
  ui->base.rna_subtype = PROP_XYZ_LENGTH;
  ui->base.description = BLI_strdup(description);
  ui->soft_min = -1e4;
  ui->soft_max = 1e4;
  ui->min = -1e9;
  ui->max = 1e9;
  ui->step = 1.0f;
  ui->precision = 4;
  IDP_AddToGroup(params_ensure(feature), prop);
}

struct EnumItem {
  int value;
  const char *identifier;
  const char *name;
};

static void add_enum(CADFeature *feature,
                     const StringRef key,
                     const int value,
                     const Span<EnumItem> items,
                     const char *description)
{
  IDProperty *prop = bke::idprop::create(key, value).release();
  IDPropertyUIDataInt *ui = reinterpret_cast<IDPropertyUIDataInt *>(IDP_ui_data_ensure(prop));
  ui->base.description = BLI_strdup(description);
  ui->default_value = value;
  ui->min = ui->soft_min = items.first().value;
  ui->max = ui->soft_max = items.last().value;
  ui->enum_items_num = int(items.size());
  ui->enum_items = MEM_new_array<IDPropertyUIDataEnumItem>(items.size(), __func__);
  for (const int i : items.index_range()) {
    ui->enum_items[i].value = items[i].value;
    ui->enum_items[i].identifier = BLI_strdup(items[i].identifier);
    ui->enum_items[i].name = BLI_strdup(items[i].name);
    ui->enum_items[i].description = nullptr;
    ui->enum_items[i].icon = 0;
  }
  IDP_AddToGroup(params_ensure(feature), prop);
}

static void add_int(CADFeature *feature, const StringRef key, const int value)
{
  IDP_AddToGroup(params_ensure(feature), bke::idprop::create(key, value).release());
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Features
 * \{ */

const char *BKE_cad_feature_type_name(const eCADFeatureType type)
{
  switch (type) {
    case CAD_FEATURE_SKETCH:
      return "Sketch";
    case CAD_FEATURE_BOX:
      return "Box";
    case CAD_FEATURE_EXTRUDE:
      return "Extrude";
    case CAD_FEATURE_FILLET:
      return "Fillet";
    case CAD_FEATURE_CHAMFER:
      return "Chamfer";
  }
  return "Feature";
}

static int next_uid(CADPart *part)
{
  return part->next_uid++;
}

static void feature_unique_name(CADPart *part, CADFeature *feature)
{
  const char *base = BKE_cad_feature_type_name(eCADFeatureType(feature->type));
  for (int n = 1;; n++) {
    char name[64];
    SNPRINTF(name, "%s%d", base, n);
    bool used = false;
    for (const CADFeature &other_item : part->features) {
    const CADFeature *other = &other_item;
      if (other != feature && STREQ(other->name, name)) {
        used = true;
        break;
      }
    }
    if (!used) {
      STRNCPY_UTF8(feature->name, name);
      return;
    }
  }
}

CADFeature *BKE_cad_feature_add(CADPart *part, const eCADFeatureType type)
{
  CADFeature *feature = MEM_new<CADFeature>(__func__);
  feature->type = type;
  feature->uid = next_uid(part);
  /* Defaults are given in millimeters and stored in Blender units. */
  const double mm = 1.0 / part->mm_per_unit;
  switch (type) {
    case CAD_FEATURE_SKETCH:
      feature->sketch = MEM_new<CADSketch>(__func__);
      break;
    case CAD_FEATURE_BOX:
      add_length3(feature, "min", double3(0.0), "Corner of the box with the smallest coordinates");
      add_length3(feature, "size", double3(20.0 * mm), "Size of the box along X, Y and Z");
      break;
    case CAD_FEATURE_EXTRUDE: {
      add_int(feature, "sketch", -1);
      add_length(feature, "distance", 10.0 * mm, -1e4, "Extrusion distance");
      add_length(feature, "distance2", 0.0, 0.0, "Distance of the second side (Two Sides)");
      static const EnumItem extents[] = {
          {0, "ONE_SIDE", "One Side"}, {1, "SYMMETRIC", "Symmetric"}, {2, "TWO_SIDES", "Two Sides"}};
      add_enum(feature, "extent", 0, extents, "How the distance is applied");
      static const EnumItem operations[] = {
          {0, "NEW_BODY", "New Body"}, {1, "JOIN", "Join"}, {2, "CUT", "Cut"}, {3, "INTERSECT", "Intersect"}};
      add_enum(feature, "operation", 0, operations, "How the result combines with the body");
      break;
    }
    case CAD_FEATURE_FILLET:
      add_length(feature, "radius", 1.0 * mm, 0.0, "Fillet radius");
      break;
    case CAD_FEATURE_CHAMFER:
      add_length(feature, "distance", 1.0 * mm, 0.0, "Chamfer distance");
      break;
  }
  feature_unique_name(part, feature);
  BLI_addtail(&part->features, feature);
  part->active_feature_uid = feature->uid;
  return feature;
}

void BKE_cad_feature_remove(CADPart *part, CADFeature *feature)
{
  BLI_remlink(&part->features, feature);
  if (part->active_feature_uid == feature->uid) {
    part->active_feature_uid = -1;
  }
  if (part->rollback_uid == feature->uid) {
    part->rollback_uid = -1;
  }
  feature_free(feature);
}

bool BKE_cad_feature_move(CADPart *part, CADFeature *feature, const int offset)
{
  return BLI_listbase_link_move(&part->features, feature, offset);
}

CADFeature *BKE_cad_feature_find(const CADPart *part, const int uid)
{
  for (CADFeature &feature_item : part->features) {
    CADFeature *feature = &feature_item;
    if (feature->uid == uid) {
      return feature;
    }
  }
  return nullptr;
}

CADFeature *BKE_cad_feature_active(const CADPart *part)
{
  return BKE_cad_feature_find(part, part->active_feature_uid);
}

void BKE_cad_feature_ref_add(CADFeature *feature, const StringRef name, const double3 &point)
{
  CADTopoRef ref;
  ref.name = BLI_strdupn(name.data(), size_t(name.size()));
  ref.point[0] = point.x;
  ref.point[1] = point.y;
  ref.point[2] = point.z;
  array_append(feature->refs, feature->refs_num, ref);
}

void BKE_cad_feature_refs_clear(CADFeature *feature)
{
  for (int i = 0; i < feature->refs_num; i++) {
    MEM_SAFE_DELETE(feature->refs[i].name);
  }
  MEM_SAFE_DELETE(feature->refs);
  feature->refs_num = 0;
}

void BKE_cad_feature_ref_remove(CADFeature *feature, const int index)
{
  if (index < 0 || index >= feature->refs_num) {
    return;
  }
  MEM_SAFE_DELETE(feature->refs[index].name);
  for (int i = index; i + 1 < feature->refs_num; i++) {
    feature->refs[i] = feature->refs[i + 1];
  }
  feature->refs_num--;
  if (feature->refs_num == 0) {
    MEM_SAFE_DELETE(feature->refs);
  }
}

int BKE_cad_feature_ref_find(const CADFeature *feature, const StringRef name)
{
  for (int i = 0; i < feature->refs_num; i++) {
    if (feature->refs[i].name && name == feature->refs[i].name) {
      return i;
    }
  }
  return -1;
}

const bke::CADFeatureStatus *BKE_cad_feature_status(const CADPart *part, const int uid)
{
  if (!part->runtime) {
    return nullptr;
  }
  for (const bke::CADFeatureStatus &status : part->runtime->eval_status) {
    if (status.uid == uid) {
      return &status;
    }
  }
  return nullptr;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Sketches
 * \{ */

CADSketchPoint *BKE_cad_sketch_point_find(const CADSketch *sketch, const int uid)
{
  for (int i = 0; i < sketch->points_num; i++) {
    if (sketch->points[i].uid == uid) {
      return &sketch->points[i];
    }
  }
  return nullptr;
}

int BKE_cad_sketch_point_add(CADPart *part, CADSketch *sketch, const double2 &co)
{
  CADSketchPoint point;
  point.uid = next_uid(part);
  point.co[0] = co.x;
  point.co[1] = co.y;
  array_append(sketch->points, sketch->points_num, point);
  return point.uid;
}

static int curve_add(CADPart *part, CADSketch *sketch, CADSketchCurve curve)
{
  curve.uid = next_uid(part);
  array_append(sketch->curves, sketch->curves_num, curve);
  return curve.uid;
}

int BKE_cad_sketch_line_add(CADPart *part, CADSketch *sketch, const int p0, const int p1)
{
  CADSketchCurve curve;
  curve.type = CAD_SKETCH_LINE;
  curve.points[0] = p0;
  curve.points[1] = p1;
  return curve_add(part, sketch, curve);
}

int BKE_cad_sketch_circle_add(CADPart *part,
                              CADSketch *sketch,
                              const int center,
                              const double radius)
{
  CADSketchCurve curve;
  curve.type = CAD_SKETCH_CIRCLE;
  curve.points[2] = center;
  curve.radius = radius;
  return curve_add(part, sketch, curve);
}

int BKE_cad_sketch_arc_add(CADPart *part,
                           CADSketch *sketch,
                           const int center,
                           const int start,
                           const int end,
                           const bool ccw)
{
  CADSketchCurve curve;
  curve.type = CAD_SKETCH_ARC;
  curve.points[0] = start;
  curve.points[1] = end;
  curve.points[2] = center;
  curve.ccw = ccw ? 1 : 0;
  const CADSketchPoint *c = BKE_cad_sketch_point_find(sketch, center);
  const CADSketchPoint *s = BKE_cad_sketch_point_find(sketch, start);
  if (c && s) {
    curve.radius = std::hypot(s->co[0] - c->co[0], s->co[1] - c->co[1]);
  }
  return curve_add(part, sketch, curve);
}

int BKE_cad_sketch_constraint_add(CADPart *part,
                                  CADSketch *sketch,
                                  const eCADConstraintType type,
                                  const Span<int> entities,
                                  const double value)
{
  CADSketchConstraint constraint;
  constraint.uid = next_uid(part);
  constraint.type = type;
  for (int i = 0; i < std::min<int>(4, int(entities.size())); i++) {
    constraint.entities[i] = entities[i];
  }
  constraint.value = value;
  array_append(sketch->constraints, sketch->constraints_num, constraint);
  return constraint.uid;
}

/** \} */

}  // namespace blender
