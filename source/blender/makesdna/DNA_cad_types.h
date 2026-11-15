/* SPDX-FileCopyrightText: 2026 SimpleCAD Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup DNA
 *
 * Parametric CAD part: a feature timeline (sketches, extrusions, fillets, ...) evaluated by a
 * B-rep kernel into solid bodies, which Blender sees as a mesh.
 *
 * Lengths are stored in Blender units (so RNA shows them in scene units); the evaluator converts
 * them to millimeters with #CADPart::mm_per_unit. Feature parameters live in an ID property group
 * per feature so they can be animated and driven like Geometry Nodes inputs.
 */

#pragma once

#include "DNA_ID.h"
#include "DNA_listBase.h"

namespace blender {

namespace bke {
struct CADPartRuntime;
}  // namespace bke

struct AnimData;
struct IDProperty;
struct Material;

/* -------------------------------------------------------------------- */
/** \name Enums (stored in files: never reorder or reuse values)
 * \{ */

/** #CADPart::kernel, matches `cad::kernel::KernelId`. */
enum eCADKernel : int {
  CAD_KERNEL_OCCT = 0,
  CAD_KERNEL_SMLIB = 1,
};

/** #CADFeature::type, matches `cad::FeatureType`. */
enum eCADFeatureType : short {
  CAD_FEATURE_SKETCH = 0,
  CAD_FEATURE_BOX = 1,
  CAD_FEATURE_EXTRUDE = 2,
  CAD_FEATURE_FILLET = 3,
  CAD_FEATURE_CHAMFER = 4,
};

/** #CADFeature::flag */
enum eCADFeatureFlag : short {
  CAD_FEATURE_SUPPRESSED = 1 << 0,
  /** UI: expanded in lists. */
  CAD_FEATURE_EXPANDED = 1 << 1,
};

/** #CADSketchCurve::type, matches `cad::SketchCurveType`. */
enum eCADSketchCurveType : char {
  CAD_SKETCH_LINE = 0,
  CAD_SKETCH_ARC = 1,
  CAD_SKETCH_CIRCLE = 2,
};

/** #CADSketchCurve::flag, #CADSketchPoint::flag */
enum eCADSketchElementFlag : char {
  CAD_SKETCH_SELECT = 1 << 0,
  CAD_SKETCH_CONSTRUCTION = 1 << 1,
  CAD_SKETCH_FIXED = 1 << 2,
};

/** #CADSketchConstraint::type */
enum eCADConstraintType : short {
  CAD_CONSTRAINT_COINCIDENT = 0,
  CAD_CONSTRAINT_HORIZONTAL = 1,
  CAD_CONSTRAINT_VERTICAL = 2,
  CAD_CONSTRAINT_PARALLEL = 3,
  CAD_CONSTRAINT_PERPENDICULAR = 4,
  CAD_CONSTRAINT_TANGENT = 5,
  CAD_CONSTRAINT_EQUAL = 6,
  CAD_CONSTRAINT_POINT_ON_CURVE = 7,
  CAD_CONSTRAINT_FIX = 8,
  /* Dimensions (have a value). */
  CAD_CONSTRAINT_DISTANCE = 32,
  CAD_CONSTRAINT_DISTANCE_X = 33,
  CAD_CONSTRAINT_DISTANCE_Y = 34,
  CAD_CONSTRAINT_RADIUS = 35,
  CAD_CONSTRAINT_DIAMETER = 36,
  CAD_CONSTRAINT_ANGLE = 37,
};

/** #CADTopoRef::flag */
enum eCADTopoRefFlag : int {
  /** The last evaluation could not find the element by name. */
  CAD_REF_LOST = 1 << 0,
};

/** #CADUserParam::unit */
enum eCADParamUnit : int {
  CAD_UNIT_NONE = 0,
  CAD_UNIT_LENGTH = 1,
  CAD_UNIT_ANGLE = 2,
};

/** #CADPart.flag */
enum eCADPartFlag : int {
  /** Do not draw the B-rep edges in the viewport. */
  CAD_PART_HIDE_EDGES = 1 << 0,
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Structs
 * \{ */

/** Persistent reference to a face or edge of an earlier result. */
struct CADTopoRef {
  /** Persistent name (see `cad::Body`). */
  char *name = nullptr;
  void *_pad_ptr = nullptr;
  /** Picked point, used to re-find the element when the name no longer exists. */
  double point[3] = {0.0, 0.0, 0.0};
  int flag = 0;
  int _pad = 0;
};

struct CADSketchPoint {
  /** Plane coordinates. */
  double co[2] = {0.0, 0.0};
  int uid = 0;
  char flag = 0;
  char _pad[3] = {};
};

struct CADSketchCurve {
  int uid = 0;
  /** #eCADSketchCurveType. */
  char type = CAD_SKETCH_LINE;
  /** #eCADSketchElementFlag. */
  char flag = 0;
  /** Arcs: direction from start to end. */
  char ccw = 1;
  char _pad0 = 0;
  /** Point uids: start, end, center (-1 when unused). */
  int points[3] = {-1, -1, -1};
  int _pad1 = 0;
  double radius = 0.0;
};

struct CADSketchConstraint {
  int uid = 0;
  /** #eCADConstraintType. */
  short type = 0;
  short flag = 0;
  /** Uids of the constrained points or curves, -1 when unused. */
  int entities[4] = {-1, -1, -1, -1};
  /** Dimension value (Blender units or radians). */
  double value = 0.0;
  /** Optional expression driving the value. */
  char *expression = nullptr;
  void *_pad_ptr = nullptr;
  /** Where the dimension label sits, relative to its default position (plane units). */
  float label_offset[2] = {0.0f, 0.0f};
};

struct CADSketch {
  /** Sketch plane frame (Blender units, part space). */
  double origin[3] = {0.0, 0.0, 0.0};
  double x_dir[3] = {1.0, 0.0, 0.0};
  double normal[3] = {0.0, 0.0, 1.0};
  CADSketchPoint *points = nullptr;
  CADSketchCurve *curves = nullptr;
  CADSketchConstraint *constraints = nullptr;
  void *_pad_ptr = nullptr;
  int points_num = 0;
  int curves_num = 0;
  int constraints_num = 0;
  int _pad = 0;
};

struct CADFeature {
  struct CADFeature *next = nullptr, *prev = nullptr;
  char name[64] = "";
  /** Unique within the part, never reused. */
  int uid = 0;
  /** #eCADFeatureType. */
  short type = 0;
  /** #eCADFeatureFlag. */
  short flag = CAD_FEATURE_EXPANDED;
  /** Numeric and enum inputs as (system) ID properties: animatable and drivable. */
  IDProperty *params = nullptr;
  /** Face / edge references (fillet edges, ...). */
  CADTopoRef *refs = nullptr;
  int refs_num = 0;
  int _pad = 0;
  /** Only for #CAD_FEATURE_SKETCH. */
  CADSketch *sketch = nullptr;
};

/** A named part parameter usable in expressions. */
struct CADUserParam {
  char name[64] = "";
  /** Resolved value (Blender units / radians). */
  double value = 0.0;
  /** #eCADParamUnit. */
  int unit = CAD_UNIT_LENGTH;
  int flag = 0;
  char *expression = nullptr;
  void *_pad_ptr = nullptr;
};

struct CADPart {
#ifdef __cplusplus
  /** See #ID_Type comment for why this is here. */
  static constexpr ID_Type id_type = ID_CAD;
#endif

  ID id;
  /** Animation data (must be immediately after id). */
  struct AnimData *adt = nullptr;

  /** The feature timeline, in evaluation order. */
  ListBaseT<CADFeature> features = {nullptr, nullptr};

  CADUserParam *user_params = nullptr;
  void *_pad_ptr = nullptr;
  int user_params_num = 0;

  /** #eCADKernel. */
  int kernel = CAD_KERNEL_OCCT;
  /** Next uid for features and sketch elements. */
  int next_uid = 1;
  /** Evaluate up to and including this feature; -1 for the whole timeline. */
  int rollback_uid = -1;
  int active_feature_uid = -1;
  /** Sketch being edited in sketch mode, -1 when none. */
  int edit_sketch_uid = -1;
  int flag = 0;
  /** Fork-local format version for versioning without upstream file subversions. */
  int format_version = 1;

  /** Millimeters per Blender unit, fixed when the part is created. */
  double mm_per_unit = 1000.0;
  /** Tessellation tolerances: chordal deviation (mm) and angle (radians). */
  float linear_deflection = 0.02f;
  float angular_deflection = 0.35f;

  struct Material **mat = nullptr;
  short totcol = 0;
  char _pad[6] = {};

  bke::CADPartRuntime *runtime = nullptr;
};

/** \} */

}  // namespace blender
