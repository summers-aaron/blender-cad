/* SPDX-FileCopyrightText: 2026 SimpleCAD Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <numbers>

#include "testing/testing.h"

#include "BLI_math_vector.hh"

#include "CAD_sketch_solver.hh"

namespace blender::cad::sketch::tests {

static constexpr double EPS = 1e-6;

static const Point &point(const Sketch &s, const int uid)
{
  for (const Point &p : s.points) {
    if (p.uid == uid) {
      return p;
    }
  }
  BLI_assert_unreachable();
  return s.points[0];
}

static int constraint(Sketch &s, const ConstraintType type, std::array<int, 4> entities, double value = 0.0)
{
  Constraint c;
  c.uid = 1000 + int(s.constraints.size());
  c.type = type;
  c.entities = entities;
  c.value = value;
  s.constraints.append(c);
  return c.uid;
}

/** Points 1..4 counter-clockwise from the bottom left, lines 11..14 (bottom, right, top, left).
 * Deliberately not quite rectangular. */
static Sketch rough_rectangle()
{
  Sketch s;
  s.points = {{1, {0.3, -0.2}}, {2, {28.0, 1.0}}, {3, {31.0, 22.0}}, {4, {-1.0, 19.0}}};
  for (int i = 0; i < 4; i++) {
    Curve line;
    line.uid = 11 + i;
    line.start = 1 + i;
    line.end = 1 + (i + 1) % 4;
    s.curves.append(line);
  }
  return s;
}

static void constrain_rectangle_shape(Sketch &s)
{
  constraint(s, ConstraintType::Horizontal, {11, -1});
  constraint(s, ConstraintType::Horizontal, {13, -1});
  constraint(s, ConstraintType::Vertical, {12, -1});
  constraint(s, ConstraintType::Vertical, {14, -1});
}

TEST(cad_sketch_solver, free_geometry_dof)
{
  Sketch s = rough_rectangle();
  const SolveResult r = solver_default().solve(s);
  EXPECT_TRUE(r.ok());
  EXPECT_EQ(r.dof, 8);
}

TEST(cad_sketch_solver, fully_constrained_rectangle)
{
  Sketch s = rough_rectangle();
  constrain_rectangle_shape(s);
  constraint(s, ConstraintType::Coincident, {1, ORIGIN_UID});
  const int width = constraint(s, ConstraintType::Distance, {11, -1}, 30.0);
  constraint(s, ConstraintType::Distance, {14, -1}, 20.0);

  SolveResult r = solver_default().solve(s);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r.dof, 0);
  EXPECT_TRUE(r.conflicting.is_empty());
  EXPECT_TRUE(r.redundant.is_empty());
  EXPECT_NEAR(point(s, 1).co.x, 0.0, EPS);
  EXPECT_NEAR(point(s, 1).co.y, 0.0, EPS);
  EXPECT_NEAR(point(s, 3).co.x, 30.0, EPS);
  EXPECT_NEAR(point(s, 3).co.y, 20.0, EPS);

  /* Change a dimension: the geometry follows. */
  for (Constraint &c : s.constraints) {
    if (c.uid == width) {
      c.value = 45.0;
    }
  }
  r = solver_default().solve(s);
  ASSERT_TRUE(r.ok());
  EXPECT_NEAR(point(s, 2).co.x, 45.0, EPS);
  EXPECT_NEAR(point(s, 3).co.x, 45.0, EPS);
  EXPECT_NEAR(point(s, 3).co.y, 20.0, EPS);
}

TEST(cad_sketch_solver, conflicting_constraints_keep_geometry)
{
  Sketch s = rough_rectangle();
  constrain_rectangle_shape(s);
  constraint(s, ConstraintType::Coincident, {1, ORIGIN_UID});
  constraint(s, ConstraintType::Distance, {11, -1}, 30.0);
  /* Bottom is 30 long and also horizontal from (0,0): a 40 distance between its ends
   * contradicts. */
  const int bad = constraint(s, ConstraintType::Distance, {1, 2}, 40.0);
  const Sketch before = s;

  const SolveResult r = solver_default().solve(s);
  EXPECT_EQ(r.status, SolveStatus::Conflicting);
  EXPECT_TRUE(r.conflicting.contains(bad));
  EXPECT_EQ(point(s, 3).co, point(before, 3).co);
}

TEST(cad_sketch_solver, redundant_constraint_reported)
{
  Sketch s = rough_rectangle();
  constrain_rectangle_shape(s);
  /* Parallel is implied by both lines being horizontal. */
  const int parallel = constraint(s, ConstraintType::Parallel, {11, 13});
  const SolveResult r = solver_default().solve(s);
  EXPECT_TRUE(r.ok());
  /* PlaneGCS may blame any of the dependent constraints. */
  EXPECT_FALSE(r.redundant.is_empty());
  UNUSED_VARS(parallel);
  /* The redundant constraint does not remove freedom: 8 - 4 = 4. */
  EXPECT_EQ(r.dof, 4);
}

TEST(cad_sketch_solver, redundancy_does_not_drop_solution)
{
  /* Redundancy elsewhere in the sketch must not undo the solution (PlaneGCS' redundancy
   * diagnosis restores the initial parameters). */
  Sketch s = rough_rectangle();
  constrain_rectangle_shape(s);
  constraint(s, ConstraintType::Parallel, {11, 13});
  s.points.append({20, {25.0, 10.0}});
  Curve circle;
  circle.uid = 21;
  circle.type = CurveType::Circle;
  circle.center = 20;
  circle.radius = 3.0;
  s.curves.append(circle);
  constraint(s, ConstraintType::Radius, {21, -1}, 5.0);
  const SolveResult r = solver_default().solve(s);
  ASSERT_TRUE(r.ok());
  EXPECT_FALSE(r.redundant.is_empty());
  EXPECT_NEAR(s.curves.last().radius, 5.0, EPS);
  EXPECT_NEAR(point(s, 2).co.y, point(s, 1).co.y, EPS);
}

TEST(cad_sketch_solver, circle_radius_and_tangent_line)
{
  Sketch s;
  s.points = {{1, {0.5, 0.2}}, {2, {-20.0, 9.0}}, {3, {20.0, 11.0}}};
  Curve circle;
  circle.uid = 10;
  circle.type = CurveType::Circle;
  circle.center = 1;
  circle.radius = 4.0;
  s.curves.append(circle);
  Curve line;
  line.uid = 11;
  line.start = 2;
  line.end = 3;
  s.curves.append(line);

  constraint(s, ConstraintType::Coincident, {1, ORIGIN_UID});
  constraint(s, ConstraintType::Radius, {10, -1}, 5.0);
  constraint(s, ConstraintType::Horizontal, {11, -1});
  constraint(s, ConstraintType::Tangent, {11, 10});

  const SolveResult r = solver_default().solve(s);
  ASSERT_TRUE(r.ok());
  EXPECT_NEAR(s.curves[0].radius, 5.0, EPS);
  /* The line was above the circle and stays above: y = +5. */
  EXPECT_NEAR(point(s, 2).co.y, 5.0, EPS);
  EXPECT_NEAR(point(s, 3).co.y, 5.0, EPS);
  /* Line ends still free along the line. */
  EXPECT_EQ(r.dof, 2);
}

TEST(cad_sketch_solver, arc_keeps_ends_on_radius)
{
  Sketch s;
  s.points = {{1, {0.0, 0.0}, true}, {2, {10.0, 0.5}}, {3, {0.3, 9.0}}};
  Curve arc;
  arc.uid = 10;
  arc.type = CurveType::Arc;
  arc.center = 1;
  arc.start = 2;
  arc.end = 3;
  arc.ccw = true;
  s.curves.append(arc);
  constraint(s, ConstraintType::Radius, {10, -1}, 12.0);
  constraint(s, ConstraintType::Horizontal, {1, 2});
  constraint(s, ConstraintType::Vertical, {1, 3});

  const SolveResult r = solver_default().solve(s);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r.dof, 0);
  EXPECT_NEAR(point(s, 2).co.x, 12.0, EPS);
  EXPECT_NEAR(point(s, 2).co.y, 0.0, EPS);
  EXPECT_NEAR(point(s, 3).co.x, 0.0, EPS);
  EXPECT_NEAR(point(s, 3).co.y, 12.0, EPS);
}

TEST(cad_sketch_solver, clockwise_arc)
{
  Sketch s;
  /* Clockwise from (0,10) to (10,0) around the origin: the short quarter. */
  s.points = {{1, {0.0, 0.0}, true}, {2, {0.0, 10.0}}, {3, {10.0, 0.2}}};
  Curve arc;
  arc.uid = 10;
  arc.type = CurveType::Arc;
  arc.center = 1;
  arc.start = 2;
  arc.end = 3;
  arc.ccw = false;
  s.curves.append(arc);
  constraint(s, ConstraintType::Radius, {10, -1}, 10.0);
  constraint(s, ConstraintType::Horizontal, {1, 3});
  constraint(s, ConstraintType::Vertical, {1, 2});
  const SolveResult r = solver_default().solve(s);
  ASSERT_TRUE(r.ok());
  EXPECT_NEAR(point(s, 3).co.x, 10.0, EPS);
  EXPECT_NEAR(point(s, 3).co.y, 0.0, EPS);
  EXPECT_NEAR(point(s, 2).co.y, 10.0, EPS);
}

TEST(cad_sketch_solver, angle_and_equal)
{
  Sketch s;
  s.points = {{1, {0.0, 0.0}, true}, {2, {10.0, 1.0}}, {3, {1.0, 7.0}}};
  Curve a, b;
  a.uid = 11;
  a.start = 1;
  a.end = 2;
  b.uid = 12;
  b.start = 1;
  b.end = 3;
  s.curves.append(a);
  s.curves.append(b);
  constraint(s, ConstraintType::Horizontal, {11, -1});
  constraint(s, ConstraintType::Distance, {11, -1}, 10.0);
  constraint(s, ConstraintType::Equal, {11, 12});
  constraint(s, ConstraintType::Angle, {11, 12}, std::numbers::pi / 3.0);
  const SolveResult r = solver_default().solve(s);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r.dof, 0);
  EXPECT_NEAR(point(s, 3).co.x, 5.0, EPS);
  EXPECT_NEAR(point(s, 3).co.y, 10.0 * std::sin(std::numbers::pi / 3.0), EPS);
}

TEST(cad_sketch_solver, drag_keeps_constraints)
{
  Sketch s = rough_rectangle();
  constrain_rectangle_shape(s);
  constraint(s, ConstraintType::Coincident, {1, ORIGIN_UID});
  ASSERT_TRUE(solver_default().solve(s).ok());

  /* Drag the top right corner: the rectangle resizes and stays a rectangle at the origin. */
  const DragTarget target = {3, {40.0, 25.0}};
  const SolveResult r = solver_default().solve(s, {&target, 1});
  ASSERT_TRUE(r.ok());
  EXPECT_NEAR(point(s, 3).co.x, 40.0, 1e-4);
  EXPECT_NEAR(point(s, 3).co.y, 25.0, 1e-4);
  EXPECT_NEAR(point(s, 2).co.y, 0.0, 1e-6);
  EXPECT_NEAR(point(s, 4).co.x, 0.0, 1e-6);
  EXPECT_NEAR(point(s, 1).co.x, 0.0, 1e-6);
}

TEST(cad_sketch_solver, fixed_point_and_distance_xy)
{
  Sketch s;
  s.points = {{1, {1.0, 2.0}}, {2, {7.0, 9.0}}};
  constraint(s, ConstraintType::Fix, {1, -1});
  constraint(s, ConstraintType::DistanceX, {1, 2}, 10.0);
  constraint(s, ConstraintType::DistanceY, {1, 2}, -3.0);
  const SolveResult r = solver_default().solve(s);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r.dof, 0);
  EXPECT_NEAR(point(s, 1).co.x, 1.0, EPS);
  EXPECT_NEAR(point(s, 2).co.x, 11.0, EPS);
  EXPECT_NEAR(point(s, 2).co.y, -1.0, EPS);
}

TEST(cad_sketch_solver, malformed_constraint_ignored)
{
  Sketch s = rough_rectangle();
  /* Parallel needs two lines, not points. */
  const int bad = constraint(s, ConstraintType::Parallel, {1, 2});
  const int missing = constraint(s, ConstraintType::Horizontal, {99, -1});
  const SolveResult r = solver_default().solve(s);
  EXPECT_TRUE(r.ok());
  EXPECT_TRUE(r.malformed.contains(bad));
  EXPECT_TRUE(r.malformed.contains(missing));
}

TEST(cad_sketch_solver, free_and_constrained_elements)
{
  /* The rectangle is pinned and sized; a circle next to it only has its radius. */
  Sketch s = rough_rectangle();
  constrain_rectangle_shape(s);
  constraint(s, ConstraintType::Coincident, {1, ORIGIN_UID});
  constraint(s, ConstraintType::Distance, {11, -1}, 30.0);
  constraint(s, ConstraintType::Distance, {14, -1}, 20.0);
  s.points.append({5, {50.0, 10.0}});
  Curve circle;
  circle.uid = 15;
  circle.type = CurveType::Circle;
  circle.center = 5;
  circle.radius = 4.0;
  s.curves.append(circle);
  constraint(s, ConstraintType::Radius, {15, -1}, 5.0);

  SolveResult r = solver_default().solve(s);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r.dof, 2);
  EXPECT_EQ(r.free_points.as_span(), Span<int>({5}));
  EXPECT_EQ(r.free_curves.as_span(), Span<int>({15}));

  /* Placing the center leaves nothing free. */
  constraint(s, ConstraintType::DistanceX, {ORIGIN_UID, 5}, 50.0);
  constraint(s, ConstraintType::DistanceY, {ORIGIN_UID, 5}, 10.0);
  r = solver_default().solve(s);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r.dof, 0);
  EXPECT_TRUE(r.free_points.is_empty());
  EXPECT_TRUE(r.free_curves.is_empty());

  /* A partly constrained line: one end pinned, horizontal, free length. */
  Sketch l;
  l.points = {{1, {0.0, 0.0}}, {2, {10.0, 1.0}}};
  Curve line;
  line.uid = 11;
  line.start = 1;
  line.end = 2;
  l.curves.append(line);
  constraint(l, ConstraintType::Coincident, {1, ORIGIN_UID});
  constraint(l, ConstraintType::Horizontal, {11, -1});
  r = solver_default().solve(l);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r.dof, 1);
  EXPECT_EQ(r.free_points.as_span(), Span<int>({2}));
  EXPECT_EQ(r.free_curves.as_span(), Span<int>({11}));
}

}  // namespace blender::cad::sketch::tests
