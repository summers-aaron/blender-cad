/* SPDX-FileCopyrightText: 2026 SimpleCAD Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup cad
 *
 * 2D geometric constraint solving for sketches, behind a solver-neutral interface.
 *
 * The sketch is described by points, curves that refer to points by uid (so curves sharing a
 * point are connected), and constraints that refer to points and curves by uid. Solving moves
 * points and changes radii so that all constraints hold, staying as close as possible to the
 * current geometry. The default backend is FreeCAD's PlaneGCS.
 */

#pragma once

#include <array>
#include <memory>

#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

namespace blender::cad::sketch {

/** Entity uids that refer to the sketch frame instead of sketch geometry. */
inline constexpr int ORIGIN_UID = -2;
inline constexpr int X_AXIS_UID = -3;
inline constexpr int Y_AXIS_UID = -4;

/** Values match #eCADConstraintType (stored in files). */
enum class ConstraintType : int16_t {
  /** Two points; or a point and a curve (point on curve). */
  Coincident = 0,
  /** A line; or two points. */
  Horizontal = 1,
  Vertical = 2,
  /** Two lines. */
  Parallel = 3,
  Perpendicular = 4,
  /** A line and a circle/arc, or two circles/arcs. */
  Tangent = 5,
  /** Two lines (equal length) or two circles/arcs (equal radius). */
  Equal = 6,
  /** A point and a curve. */
  PointOnCurve = 7,
  /** A point that does not move. */
  Fix = 8,

  /* Dimensions (have a value). */
  /** Two points; a line (its length); or a point and a line. */
  Distance = 32,
  /** Signed horizontal / vertical offset: two points, a line, or a point (from the origin). */
  DistanceX = 33,
  DistanceY = 34,
  /** A circle or arc. */
  Radius = 35,
  Diameter = 36,
  /** Two lines (angle from the first to the second), or one line (from the X axis). */
  Angle = 37,
};

inline bool constraint_is_dimension(const ConstraintType type)
{
  return int(type) >= int(ConstraintType::Distance);
}

struct Point {
  int uid = 0;
  double2 co = {0.0, 0.0};
  bool fixed = false;
};

enum class CurveType : int8_t {
  Line = 0,
  Arc = 1,
  Circle = 2,
};

struct Curve {
  int uid = 0;
  CurveType type = CurveType::Line;
  /** Point uids: line/arc ends, arc/circle center (-1 when unused). */
  int start = -1;
  int end = -1;
  int center = -1;
  /** Circles only (arcs take the radius from their points). */
  double radius = 0.0;
  /** Arcs: direction from start to end. */
  bool ccw = true;
};

struct Constraint {
  int uid = 0;
  ConstraintType type = ConstraintType::Coincident;
  std::array<int, 4> entities = {-1, -1, -1, -1};
  /** Dimension value: length, or angle in radians. */
  double value = 0.0;
};

struct Sketch {
  Vector<Point> points;
  Vector<Curve> curves;
  Vector<Constraint> constraints;
};

/** Pull a point towards a position while solving (dragging); other constraints still hold. */
struct DragTarget {
  int point = 0;
  double2 co = {0.0, 0.0};
};

enum class SolveStatus : int8_t {
  /** All constraints are satisfied. */
  Ok,
  /** Some constraints contradict each other; geometry is unchanged. */
  Conflicting,
  /** The solver did not converge; geometry is unchanged. */
  Failed,
};

struct SolveResult {
  SolveStatus status = SolveStatus::Ok;
  /** Remaining degrees of freedom (0: fully constrained). */
  int dof = 0;
  /** Uids of constraints that contradict others. */
  Vector<int> conflicting;
  /** Uids of constraints already implied by others (harmless, but worth removing). */
  Vector<int> redundant;
  /** Uids of constraints that refer to missing or wrong kinds of entities (ignored). */
  Vector<int> malformed;
  /** Uids of points and curves that can still move (not fully constrained); set on success. */
  Vector<int> free_points;
  Vector<int> free_curves;

  bool ok() const
  {
    return status == SolveStatus::Ok;
  }
};

class Solver {
 public:
  virtual ~Solver() = default;
  virtual const char *name() const = 0;
  /**
   * Solves `sketch` in place: point coordinates and circle radii are updated on success and left
   * unchanged otherwise.
   */
  virtual SolveResult solve(Sketch &sketch, Span<DragTarget> drag = {}) const = 0;
};

/** PlaneGCS (FreeCAD). */
std::unique_ptr<Solver> solver_planegcs();

/** The solver sketches use. Lives for the whole process; thread-safe. */
const Solver &solver_default();

}  // namespace blender::cad::sketch
