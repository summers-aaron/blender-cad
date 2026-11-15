/* SPDX-FileCopyrightText: 2026 SimpleCAD Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * Parametric CAD part data-block (#CADPart): feature timeline management and evaluation.
 */

#include <memory>
#include <string>

#include "BLI_index_range.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"
#include "BLI_string_ref.hh"
#include "BLI_vector.hh"

#include "DNA_cad_types.h"

namespace blender {

struct Depsgraph;
struct IDProperty;
struct Main;
struct Mesh;
struct Object;
struct Scene;

namespace cad {
struct EvalResult;
}

namespace bke {

/** Evaluation status of one feature, for the UI (History strip, browser). */
struct CADFeatureStatus {
  int uid = -1;
  /** Matches `cad::FeatureStatus`. */
  int status = 0;
  std::string message;
};

/** One solid of the tessellated part, for picking and highlighting B-rep faces and edges. */
struct CADBrepBody {
  /** Vertices and (triangle) faces of the body in #CADBrepMesh::mesh. */
  IndexRange verts;
  IndexRange faces;
  /** Per B-rep edge: the two faces it joins (-1 when absent; both equal on a periodic seam). */
  Vector<int2> edge_faces;
  /** Per B-rep edge: mesh vertices along it (indices into the whole mesh). */
  Vector<Vector<int>> edge_polylines;

  bool edge_is_seam(const int edge) const
  {
    return edge_faces[edge][0] >= 0 && edge_faces[edge][0] == edge_faces[edge][1];
  }
};

/** The evaluated part before Blender modifiers: the exact B-rep, tessellated. */
struct CADBrepMesh {
  /** Owned copy (arrays are implicitly shared with the evaluated geometry). Object space,
   * Blender units. Face attributes `cad_face` / `cad_body`, edge attribute `cad_edge`. */
  Mesh *mesh = nullptr;
  Vector<CADBrepBody> bodies;

  CADBrepMesh() = default;
  CADBrepMesh(const CADBrepMesh &) = delete;
  CADBrepMesh &operator=(const CADBrepMesh &) = delete;
  ~CADBrepMesh();
};

struct CADPartRuntime {
  /** Last evaluation result (set on the evaluated copy, read through the original's
   * #CADPartRuntime::eval_status copy for the UI). */
  std::shared_ptr<const cad::EvalResult> eval;
  /** Per-feature status of the last evaluation, copied back to the original data-block so the
   * UI can show errors without the depsgraph. */
  Vector<CADFeatureStatus> eval_status;
  /** Tessellated result before mesh modifiers (evaluated copy only). */
  std::shared_ptr<const CADBrepMesh> brep_mesh;

  MEM_CXX_CLASS_ALLOC_FUNCS("CADPartRuntime");
};

/** Result of evaluating an expression for a part (see #BKE_cad_expression_eval). */
struct CADExprEval {
  bool ok = false;
  /** Blender units for lengths, radians for angles. */
  double value = 0.0;
  std::string error;
};

/** Attribute names of the mesh a part evaluates to. */
inline constexpr StringRef CAD_ATTR_FACE = "cad_face";
inline constexpr StringRef CAD_ATTR_EDGE = "cad_edge";
inline constexpr StringRef CAD_ATTR_BODY = "cad_body";

}  // namespace bke

CADPart *BKE_cad_part_add(Main *bmain, const char *name);

/* -------------------------------------------------------------------- */
/** \name Features
 * \{ */

/** Appends a feature with default parameters and a unique name ("Extrude2", ...). */
CADFeature *BKE_cad_feature_add(CADPart *part, eCADFeatureType type);
void BKE_cad_feature_remove(CADPart *part, CADFeature *feature);
/** Moves a feature `offset` places in the timeline (negative: earlier). Returns false when it
 * cannot move (already first / last). */
bool BKE_cad_feature_move(CADPart *part, CADFeature *feature, int offset);
CADFeature *BKE_cad_feature_find(const CADPart *part, int uid);
CADFeature *BKE_cad_feature_active(const CADPart *part);
const char *BKE_cad_feature_type_name(eCADFeatureType type);

double BKE_cad_feature_param_get(const CADFeature *feature, StringRef key, double fallback);
int BKE_cad_feature_param_get_int(const CADFeature *feature, StringRef key, int fallback);
float3 BKE_cad_feature_param_get_float3(const CADFeature *feature,
                                        StringRef key,
                                        const float3 &fallback);
void BKE_cad_feature_param_set(CADFeature *feature, StringRef key, double value);
void BKE_cad_feature_param_set_int(CADFeature *feature, StringRef key, int value);

/**
 * Inputs are the numeric (float) parameters of a feature: they can be typed as expressions.
 * The unit comes from the parameter's subtype (distance, angle).
 */
bool BKE_cad_feature_param_is_input(const IDProperty *prop);
eCADParamUnit BKE_cad_feature_param_unit(const IDProperty *prop);
/** The expression bound to an input, or null. */
CADParamExpr *BKE_cad_feature_expression_find(const CADFeature *feature, StringRef key);
/**
 * Sets an input from text ("12 mm", "width / 2"). Text that uses part parameters is kept as the
 * input's expression and evaluated again when they change; other text only sets the value.
 * Returns false and changes nothing when the text cannot be evaluated.
 */
bool BKE_cad_feature_input_set(
    CADPart *part, CADFeature *feature, StringRef key, StringRef text, std::string *r_error);
/** Sets an input's value, removing an expression bound to it. */
void BKE_cad_feature_input_value_set(CADFeature *feature, StringRef key, double value);
/** The input as text: its expression, or the value with units ("12.5 mm"). */
std::string BKE_cad_feature_input_text(const CADPart *part,
                                       const CADFeature *feature,
                                       StringRef key);

/** Adds an edge / face reference. */
void BKE_cad_feature_ref_add(CADFeature *feature, StringRef name, const double3 &point);
void BKE_cad_feature_refs_clear(CADFeature *feature);
void BKE_cad_feature_ref_remove(CADFeature *feature, int index);
/** Index of the reference with this name, or -1. */
int BKE_cad_feature_ref_find(const CADFeature *feature, StringRef name);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Sketches
 * \{ */

CADSketchPoint *BKE_cad_sketch_point_find(const CADSketch *sketch, int uid);
int BKE_cad_sketch_point_add(CADPart *part, CADSketch *sketch, const double2 &co);
int BKE_cad_sketch_line_add(CADPart *part, CADSketch *sketch, int p0, int p1);
int BKE_cad_sketch_circle_add(CADPart *part, CADSketch *sketch, int center, double radius);
int BKE_cad_sketch_arc_add(
    CADPart *part, CADSketch *sketch, int center, int start, int end, bool ccw);
int BKE_cad_sketch_constraint_add(CADPart *part,
                                  CADSketch *sketch,
                                  eCADConstraintType type,
                                  Span<int> entities,
                                  double value);

/** Pull a sketch point towards a position while solving (dragging). */
struct CADSketchDrag {
  int point = 0;
  double2 co = {0.0, 0.0};
};

/**
 * Solves the sketch's constraints: moves points and changes radii so all constraints hold,
 * staying close to the current geometry. Updates #CADSketch.dof, #CADSketch.solve_status and the
 * conflict flags of the constraints. Geometry is left unchanged when constraints conflict or
 * the solver fails; returns false then.
 */
bool BKE_cad_sketch_solve(const CADPart *part, CADSketch *sketch, Span<CADSketchDrag> drag = {});

/** The feature that owns `sketch`, or null. */
CADFeature *BKE_cad_sketch_feature(const CADPart *part, const CADSketch *sketch);

/** Deep copy / free of a sketch (for sketch mode undo). */
CADSketch *BKE_cad_sketch_copy(const CADSketch *sketch);
void BKE_cad_sketch_free(CADSketch *sketch);

CADSketchCurve *BKE_cad_sketch_curve_find(const CADSketch *sketch, int uid);
CADSketchConstraint *BKE_cad_sketch_constraint_find(const CADSketch *sketch, int uid);
/** True when any curve uses the point. */
bool BKE_cad_sketch_point_is_used(const CADSketch *sketch, int point_uid);

/**
 * Removes selected curves, points and constraints, the curves using removed points, points that
 * only the removed curves used, and constraints that refer to removed geometry.
 * Returns true when anything was removed.
 */
bool BKE_cad_sketch_remove_selected(CADSketch *sketch);
/** Removes elements by uid (points, curves or constraints), with the same cleanup. */
bool BKE_cad_sketch_remove(CADSketch *sketch, Span<int> uids);

/** Selection of all points, curves and constraints. */
bool BKE_cad_sketch_select_all(CADSketch *sketch, bool select);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Feature Dialog
 *
 * A feature dialog edits one feature (new or existing) with a live preview. Its changes are not
 * undo steps of their own: confirming the dialog makes one undo step, cancelling restores the
 * feature (or removes a new one). Editing an existing feature rolls the timeline back to it.
 * \{ */

/** The feature in the open dialog, or null. */
CADFeature *BKE_cad_dialog_feature(const CADPart *part);
/** Opens the dialog; there must be no open dialog. */
void BKE_cad_dialog_begin(CADPart *part, CADFeature *feature, bool is_new);
/** Closes the dialog, keeping the changes (`apply`) or reverting them. */
void BKE_cad_dialog_end(CADPart *part, bool apply);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Parameters and Expressions
 *
 * Expressions such as "width / 2 + 3 mm" drive feature inputs, sketch dimensions and other
 * user parameters. Bare numbers are millimeters (lengths) or degrees (angles).
 * \{ */

/** Evaluates an expression with the part's user parameters. */
bke::CADExprEval BKE_cad_expression_eval(const CADPart *part,
                                         StringRef expression,
                                         eCADParamUnit unit);
/** Whether the expression uses any of the part's user parameters. */
bool BKE_cad_expression_uses_params(const CADPart *part, StringRef expression);
/** A value (Blender units / radians) as expression text in document units: "12.5 mm". */
std::string BKE_cad_value_format(const CADPart *part, double value, eCADParamUnit unit);

/** Sets a sketch dimension from text, see #BKE_cad_feature_input_set. Does not solve. */
bool BKE_cad_constraint_input_set(CADPart *part,
                                  CADSketchConstraint *constraint,
                                  StringRef text,
                                  std::string *r_error);
std::string BKE_cad_constraint_input_text(const CADPart *part,
                                          const CADSketchConstraint *constraint);

int BKE_cad_param_find(const CADPart *part, StringRef name);
/** Identifiers that are not functions, constants or units. */
bool BKE_cad_param_name_is_valid(StringRef name);
/** Adds a parameter with a unique name; `expression` may be empty. */
CADUserParam *BKE_cad_param_add(CADPart *part,
                                StringRef name,
                                eCADParamUnit unit,
                                StringRef expression);
void BKE_cad_param_remove(CADPart *part, int index);
/** Renames a parameter and the expressions that use it. False for invalid names. */
bool BKE_cad_param_rename(CADPart *part, int index, StringRef name);
/** Sets a parameter's expression; false for invalid text or cyclic dependencies. */
bool BKE_cad_param_expression_set(CADPart *part,
                                  int index,
                                  StringRef text,
                                  std::string *r_error);
std::string BKE_cad_param_text(const CADPart *part, int index);

/**
 * Evaluates all expressions of the part in dependency order and stores their values. Sketches
 * with changed dimensions are solved again. Expressions that fail keep their last value and get
 * an error flag. Returns true when any value changed.
 */
bool BKE_cad_expressions_update(CADPart *part);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Evaluation
 * \{ */

/** Depsgraph callback: evaluates the timeline into a mesh on the object's geometry set. */
void BKE_cad_data_update(Depsgraph *depsgraph, Scene *scene, Object *object);

/**
 * Persistent name of a face or edge of the evaluated result (see #bke::CADBrepMesh), e.g.
 * "F3:top" or "F3:front|F3:top". Empty when out of range or not evaluated.
 */
std::string BKE_cad_topology_name(const CADPart *part_eval, int body, bool is_edge, int index);

/** Status of the last evaluation for `uid` (on the original data-block), or null. */
const bke::CADFeatureStatus *BKE_cad_feature_status(const CADPart *part, int uid);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Exchange
 * \{ */

/** Whether this build can write STEP files (needs the OpenCASCADE kernel). */
bool BKE_cad_step_export_supported();

/**
 * Writes the solids of evaluated CAD part objects to a STEP file, in millimeters.
 * Parts on kernels without STEP support are re-evaluated with OpenCASCADE.
 * Blender modifiers are not included (the B-rep is exported, not the mesh).
 *
 * \param objects_eval: Evaluated objects; non-CAD objects are skipped.
 * \param apply_transform: Place each part with its object's world transform.
 * \return Number of exported bodies, or -1 on failure (with `r_message` set).
 */
int BKE_cad_export_step(Span<const Object *> objects_eval,
                        StringRefNull filepath,
                        bool apply_transform,
                        std::string &r_message);

/** \} */

}  // namespace blender
