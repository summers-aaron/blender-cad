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
  /** Fully constrained: the constraints leave it no freedom to move (set by the solver). */
  CAD_SKETCH_CONSTRAINED = 1 << 3,
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

/** #CADParamExpr::flag, #CADUserParam::flag */
enum eCADExprFlag : int {
  /** The expression could not be evaluated; the value is the last good one. */
  CAD_EXPR_ERROR = 1 << 0,
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

/** Entity uids in #CADSketchConstraint.entities that refer to the sketch frame. */
enum eCADSketchFrameUid : int {
  CAD_SKETCH_ORIGIN = -2,
  CAD_SKETCH_X_AXIS = -3,
  CAD_SKETCH_Y_AXIS = -4,
};

/** #CADSketchConstraint.flag (set by the solver). */
enum eCADConstraintFlag : short {
  /** Contradicts other constraints. */
  CAD_CONSTRAINT_CONFLICTING = 1 << 0,
  /** Implied by other constraints. */
  CAD_CONSTRAINT_REDUNDANT = 1 << 1,
  /** Refers to missing entities or entities of the wrong kind; ignored. */
  CAD_CONSTRAINT_MALFORMED = 1 << 2,
  /** #CADSketchConstraint.expression could not be evaluated. */
  CAD_CONSTRAINT_EXPR_ERROR = 1 << 3,
  /** Selected in sketch mode (dimension labels, constraint glyphs). */
  CAD_CONSTRAINT_SELECT = 1 << 8,
};

/** #CADSketch.solve_status */
enum eCADSketchSolveStatus : char {
  CAD_SKETCH_SOLVE_OK = 0,
  CAD_SKETCH_SOLVE_CONFLICTING = 1,
  CAD_SKETCH_SOLVE_FAILED = 2,
};

/** #CADPart.flag */
enum eCADPartFlag : int {
  /** Do not draw the B-rep edges in the viewport. */
  CAD_PART_HIDE_EDGES = 1 << 0,
  /**
   * A sketch is being edited (sketch mode). Stored so that re-entering the mode after an undo
   * step or file load keeps the state saved when the mode was entered.
   */
  CAD_PART_SKETCH_EDITING = 1 << 1,
  /** The feature in the dialog (#CADPart.dialog_uid) was created by it: Cancel removes it. */
  CAD_PART_DIALOG_NEW = 1 << 2,
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
  /** Expression driving the value when it uses part parameters, else null. */
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
  /** Degrees of freedom left after the last solve (-1: not solved yet). */
  short dof = -1;
  /** #eCADSketchSolveStatus of the last solve. */
  char solve_status = CAD_SKETCH_SOLVE_OK;
  char _pad = 0;
};

/** Expression bound to a numeric feature parameter (an input). */
struct CADParamExpr {
  /** Parameter name in #CADFeature.params. */
  char key[64] = "";
  char *expression = nullptr;
  void *_pad_ptr = nullptr;
  /** #eCADExprFlag. */
  int flag = 0;
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
  /** Inputs driven by expressions that use part parameters. */
  CADParamExpr *exprs = nullptr;
  int exprs_num = 0;
  int _pad1 = 0;
};

/** A named part parameter usable in expressions. */
struct CADUserParam {
  char name[64] = "";
  /** Resolved value (Blender units / radians). */
  double value = 0.0;
  /** #eCADParamUnit. */
  int unit = CAD_UNIT_LENGTH;
  /** #eCADExprFlag. */
  int flag = 0;
  /** What the user entered ("20 mm", "width / 2"), null when only the value is set. */
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
  /**
   * The feature open in a feature dialog as it was before the dialog changed it (null when the
   * dialog made the feature). Cancel restores it.
   */
  CADFeature *dialog_backup = nullptr;
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
  /** #rollback_uid before sketch mode moved the marker to the edited sketch. */
  int edit_prev_rollback_uid = -1;
  int flag = 0;
  /** Fork-local format version for versioning without upstream file subversions. */
  int format_version = 1;
  /**
   * Feature edited in a feature dialog, -1 when none. Its changes are previewed live and
   * become one undo step when the dialog is confirmed.
   */
  int dialog_uid = -1;
  /** #rollback_uid before the dialog opened. */
  int dialog_prev_rollback_uid = -1;
  /** Active item in the parameters list. */
  int active_param_index = 0;

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
