/* SPDX-FileCopyrightText: 2026 SimpleCAD Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup cad
 *
 * Sketch solving with PlaneGCS.
 *
 * Every sketch point owns two solver parameters; curves reference the points they use, so
 * shared points connect curves without extra constraints. Circles own a radius parameter, arcs
 * a radius and two angles tied to their end points by "arc rules". Dimension values and fixed
 * geometry are parameters that are not declared unknown, so the solver treats them as
 * constants. Constraint tags are the constraint index + 1, which maps diagnosis results back to
 * constraint uids; drag targets use PlaneGCS' temporary (soft) constraints.
 */

#include <cmath>
#include <deque>
#include <mutex>
#include <numbers>

#include "BLI_map.hh"
#include "BLI_math_vector.hh"
#include "BLI_set.hh"

#include "CAD_sketch_solver.hh"

#include "GCS.h"

namespace blender::cad::sketch {

namespace {

class PlaneGCSSketch {
 public:
  GCS::System system;
  /** Parameter storage; a deque keeps addresses stable while growing. */
  std::deque<double> params;
  std::vector<double *> unknowns;

  Map<int, GCS::Point> points;
  Map<int, GCS::Line> lines;
  Map<int, GCS::Circle> circles;
  Map<int, GCS::Arc> arcs;
  /** Arc radius parameters: arcs are drawn from their points, so radii are not written back. */
  Map<int, double *> circle_radius;

  double *param(const double value, const bool unknown)
  {
    params.push_back(value);
    double *p = &params.back();
    if (unknown) {
      unknowns.push_back(p);
    }
    return p;
  }

  GCS::Point make_point(const double2 co, const bool unknown)
  {
    GCS::Point p;
    p.x = param(co.x, unknown);
    p.y = param(co.y, unknown);
    return p;
  }

  void add_frame()
  {
    const GCS::Point origin = make_point({0.0, 0.0}, false);
    points.add(ORIGIN_UID, origin);
    GCS::Line x_axis;
    x_axis.p1 = origin;
    x_axis.p2 = make_point({1.0, 0.0}, false);
    lines.add(X_AXIS_UID, x_axis);
    GCS::Line y_axis;
    y_axis.p1 = origin;
    y_axis.p2 = make_point({0.0, 1.0}, false);
    lines.add(Y_AXIS_UID, y_axis);
  }

  const GCS::Point *point(const int uid) const
  {
    return points.lookup_ptr(uid);
  }
  GCS::Line *line(const int uid)
  {
    return lines.lookup_ptr(uid);
  }
  /** Circle or arc (an arc is a circle for radius, tangency, ...). */
  GCS::Circle *round(const int uid)
  {
    if (GCS::Circle *c = circles.lookup_ptr(uid)) {
      return c;
    }
    return arcs.lookup_ptr(uid);
  }
  GCS::Arc *arc(const int uid)
  {
    return arcs.lookup_ptr(uid);
  }
};

/** True when `p` is left of the directed line (PlaneGCS' "ccw" side). */
static bool point_left_of_line(const GCS::Point &p, const GCS::Line &l)
{
  const double dx = *l.p2.x - *l.p1.x;
  const double dy = *l.p2.y - *l.p1.y;
  return dx * (*p.y - *l.p1.y) - dy * (*p.x - *l.p1.x) > 0.0;
}

static double angle_of(const double2 v)
{
  return std::atan2(v.y, v.x);
}

/** Adds one constraint. Returns false when its entities do not fit its type. */
static bool add_constraint(PlaneGCSSketch &s, const Constraint &c, const int tag)
{
  const std::array<int, 4> &e = c.entities;
  GCS::System &sys = s.system;
  auto value_param = [&](const double value) { return s.param(value, false); };

  switch (c.type) {
    case ConstraintType::Coincident:
    case ConstraintType::PointOnCurve: {
      const GCS::Point *p0 = s.point(e[0]);
      if (!p0) {
        return false;
      }
      GCS::Point a = *p0;
      if (const GCS::Point *p1 = s.point(e[1]); p1 && c.type == ConstraintType::Coincident) {
        GCS::Point b = *p1;
        sys.addConstraintP2PCoincident(a, b, tag);
        return true;
      }
      if (GCS::Line *l = s.line(e[1])) {
        sys.addConstraintPointOnLine(a, *l, tag);
        return true;
      }
      if (GCS::Arc *arc = s.arc(e[1])) {
        sys.addConstraintPointOnArc(a, *arc, tag);
        return true;
      }
      if (GCS::Circle *circle = s.round(e[1])) {
        sys.addConstraintPointOnCircle(a, *circle, tag);
        return true;
      }
      return false;
    }
    case ConstraintType::Horizontal:
    case ConstraintType::Vertical: {
      const bool horizontal = c.type == ConstraintType::Horizontal;
      if (GCS::Line *l = s.line(e[0])) {
        horizontal ? sys.addConstraintHorizontal(*l, tag) : sys.addConstraintVertical(*l, tag);
        return true;
      }
      const GCS::Point *p0 = s.point(e[0]), *p1 = s.point(e[1]);
      if (p0 && p1) {
        GCS::Point a = *p0, b = *p1;
        horizontal ? sys.addConstraintHorizontal(a, b, tag) :
                     sys.addConstraintVertical(a, b, tag);
        return true;
      }
      return false;
    }
    case ConstraintType::Parallel:
    case ConstraintType::Perpendicular: {
      GCS::Line *l0 = s.line(e[0]), *l1 = s.line(e[1]);
      if (!l0 || !l1) {
        return false;
      }
      c.type == ConstraintType::Parallel ? sys.addConstraintParallel(*l0, *l1, tag) :
                                           sys.addConstraintPerpendicular(*l0, *l1, tag);
      return true;
    }
    case ConstraintType::Tangent: {
      GCS::Line *l0 = s.line(e[0]), *l1 = s.line(e[1]);
      GCS::Line *line = l0 ? l0 : l1;
      const int round_uid = l0 ? e[1] : e[0];
      if (line) {
        if (GCS::Arc *arc = s.arc(round_uid)) {
          sys.addConstraintTangent(*line, *arc, point_left_of_line(arc->center, *line), tag);
          return true;
        }
        if (GCS::Circle *circle = s.round(round_uid)) {
          sys.addConstraintTangent(
              *line, *circle, point_left_of_line(circle->center, *line), tag);
          return true;
        }
        return false;
      }
      GCS::Arc *a0 = s.arc(e[0]), *a1 = s.arc(e[1]);
      if (a0 && a1) {
        sys.addConstraintTangent(*a0, *a1, tag);
        return true;
      }
      GCS::Circle *c0 = s.circles.lookup_ptr(e[0]), *c1 = s.circles.lookup_ptr(e[1]);
      if (c0 && c1) {
        sys.addConstraintTangent(*c0, *c1, tag);
        return true;
      }
      if ((c0 || c1) && (a0 || a1)) {
        sys.addConstraintTangent(c0 ? *c0 : *c1, a0 ? *a0 : *a1, tag);
        return true;
      }
      return false;
    }
    case ConstraintType::Equal: {
      GCS::Line *l0 = s.line(e[0]), *l1 = s.line(e[1]);
      if (l0 && l1) {
        sys.addConstraintEqualLength(*l0, *l1, tag);
        return true;
      }
      GCS::Arc *a0 = s.arc(e[0]), *a1 = s.arc(e[1]);
      GCS::Circle *c0 = s.circles.lookup_ptr(e[0]), *c1 = s.circles.lookup_ptr(e[1]);
      if (a0 && a1) {
        sys.addConstraintEqualRadius(*a0, *a1, tag);
      }
      else if (c0 && c1) {
        sys.addConstraintEqualRadius(*c0, *c1, tag);
      }
      else if ((c0 || c1) && (a0 || a1)) {
        sys.addConstraintEqualRadius(c0 ? *c0 : *c1, a0 ? *a0 : *a1, tag);
      }
      else {
        return false;
      }
      return true;
    }
    case ConstraintType::Fix:
      /* Handled when declaring unknowns. */
      return s.point(e[0]) != nullptr;
    case ConstraintType::Distance: {
      const GCS::Point *p0 = s.point(e[0]), *p1 = s.point(e[1]);
      if (p0 && p1) {
        GCS::Point a = *p0, b = *p1;
        sys.addConstraintP2PDistance(a, b, value_param(c.value), tag);
        return true;
      }
      if (GCS::Line *l = s.line(e[0]); l && e[1] == -1) {
        sys.addConstraintP2PDistance(l->p1, l->p2, value_param(c.value), tag);
        return true;
      }
      if (p0) {
        if (GCS::Line *l = s.line(e[1])) {
          GCS::Point a = *p0;
          sys.addConstraintP2LDistance(
              a, *l, value_param(c.value), point_left_of_line(a, *l), tag);
          return true;
        }
      }
      return false;
    }
    case ConstraintType::DistanceX:
    case ConstraintType::DistanceY: {
      const bool x = c.type == ConstraintType::DistanceX;
      const GCS::Point *p0 = s.point(e[0]), *p1 = s.point(e[1]);
      if (const GCS::Line *l = s.line(e[0]); l && !p1) {
        p0 = &l->p1;
        p1 = &l->p2;
      }
      if (p0 && p1) {
        /* Offset of the second point from the first. */
        sys.addConstraintDifference(
            x ? p0->x : p0->y, x ? p1->x : p1->y, value_param(c.value), tag);
        return true;
      }
      if (p0) {
        GCS::Point a = *p0;
        x ? sys.addConstraintCoordinateX(a, value_param(c.value), tag) :
            sys.addConstraintCoordinateY(a, value_param(c.value), tag);
        return true;
      }
      return false;
    }
    case ConstraintType::Radius:
    case ConstraintType::Diameter: {
      const bool radius = c.type == ConstraintType::Radius;
      if (GCS::Arc *arc = s.arc(e[0])) {
        radius ? sys.addConstraintArcRadius(*arc, value_param(c.value), tag) :
                 sys.addConstraintArcDiameter(*arc, value_param(c.value), tag);
        return true;
      }
      if (GCS::Circle *circle = s.round(e[0])) {
        radius ? sys.addConstraintCircleRadius(*circle, value_param(c.value), tag) :
                 sys.addConstraintCircleDiameter(*circle, value_param(c.value), tag);
        return true;
      }
      return false;
    }
    case ConstraintType::Angle: {
      GCS::Line *l0 = s.line(e[0]), *l1 = s.line(e[1]);
      if (l0 && l1) {
        sys.addConstraintL2LAngle(*l0, *l1, value_param(c.value), tag);
        return true;
      }
      if (l0) {
        sys.addConstraintP2PAngle(l0->p1, l0->p2, value_param(c.value), tag);
        return true;
      }
      return false;
    }
  }
  return false;
}

class PlaneGCSSolver : public Solver {
 public:
  const char *name() const override
  {
    return "PlaneGCS";
  }

  SolveResult solve(Sketch &sketch, const Span<DragTarget> drag) const override
  {
    /* Sketches are small and solve in milliseconds; serializing avoids depending on PlaneGCS
     * being free of shared state. */
    static std::mutex mutex;
    std::lock_guard lock(mutex);

    SolveResult result;
    PlaneGCSSketch s;
    s.add_frame();

    /* Fixed points: by flag or by a Fix constraint. */
    Set<int> fixed;
    for (const Point &p : sketch.points) {
      if (p.fixed) {
        fixed.add(p.uid);
      }
    }
    for (const Constraint &c : sketch.constraints) {
      if (c.type == ConstraintType::Fix) {
        fixed.add(c.entities[0]);
      }
    }

    for (const Point &p : sketch.points) {
      s.points.add(p.uid, s.make_point(p.co, !fixed.contains(p.uid)));
    }

    for (const Curve &curve : sketch.curves) {
      switch (curve.type) {
        case CurveType::Line: {
          const GCS::Point *a = s.point(curve.start), *b = s.point(curve.end);
          if (!a || !b) {
            continue;
          }
          GCS::Line line;
          line.p1 = *a;
          line.p2 = *b;
          s.lines.add(curve.uid, line);
          break;
        }
        case CurveType::Circle: {
          const GCS::Point *center = s.point(curve.center);
          if (!center) {
            continue;
          }
          GCS::Circle circle;
          circle.center = *center;
          circle.rad = s.param(curve.radius, true);
          s.circles.add(curve.uid, circle);
          s.circle_radius.add(curve.uid, circle.rad);
          break;
        }
        case CurveType::Arc: {
          const GCS::Point *center = s.point(curve.center), *a = s.point(curve.start),
                           *b = s.point(curve.end);
          if (!center || !a || !b) {
            continue;
          }
          /* PlaneGCS arcs run counter-clockwise from start to end. */
          const GCS::Point &start = curve.ccw ? *a : *b;
          const GCS::Point &end = curve.ccw ? *b : *a;
          const double2 c(*center->x, *center->y);
          const double2 ps(*start.x, *start.y), pe(*end.x, *end.y);
          double a0 = angle_of(ps - c);
          double a1 = angle_of(pe - c);
          if (a1 <= a0) {
            a1 += 2.0 * std::numbers::pi;
          }
          GCS::Arc arc;
          arc.center = *center;
          arc.start = start;
          arc.end = end;
          arc.rad = s.param(math::distance(ps, c), true);
          arc.startAngle = s.param(a0, true);
          arc.endAngle = s.param(a1, true);
          s.arcs.add(curve.uid, arc);
          s.system.addConstraintArcRules(s.arcs.lookup(curve.uid));
          break;
        }
      }
    }

    for (const int i : sketch.constraints.index_range()) {
      if (!add_constraint(s, sketch.constraints[i], i + 1)) {
        result.malformed.append(sketch.constraints[i].uid);
      }
    }

    /* Drag targets: soft constraints, satisfied as closely as the others allow. */
    for (const DragTarget &target : drag) {
      const GCS::Point *p = s.point(target.point);
      if (!p || fixed.contains(target.point)) {
        continue;
      }
      GCS::Point a = *p;
      GCS::Point goal = s.make_point(target.co, false);
      s.system.addConstraintP2PCoincident(a, goal, GCS::DefaultTemporaryConstraint);
    }

    if (s.unknowns.empty()) {
      result.dof = 0;
      return result;
    }

    s.system.declareUnknowns(s.unknowns);
    s.system.initSolution(GCS::DogLeg);
    result.dof = std::max(0, s.system.dofsNumber());

    GCS::VEC_I conflicting, redundant;
    s.system.getConflicting(conflicting);
    s.system.getRedundant(redundant);
    auto tags_to_uids = [&](const GCS::VEC_I &tags, Vector<int> &r_uids) {
      for (const int tag : tags) {
        if (tag >= 1 && tag <= sketch.constraints.size()) {
          r_uids.append_non_duplicates(sketch.constraints[tag - 1].uid);
        }
      }
    };
    tags_to_uids(conflicting, result.conflicting);
    tags_to_uids(redundant, result.redundant);
    if (!result.conflicting.is_empty()) {
      result.status = SolveStatus::Conflicting;
      return result;
    }

    bool solved = false;
    for (const GCS::Algorithm algorithm : {GCS::DogLeg, GCS::LevenbergMarquardt, GCS::BFGS}) {
      const GCS::SolveStatus status = s.system.solve(algorithm);
      if (status == GCS::SolveStatus::Success || status == GCS::SolveStatus::Converged) {
        solved = true;
        break;
      }
      s.system.undoSolution();
    }
    if (!solved) {
      result.status = SolveStatus::Failed;
      return result;
    }
    s.system.applySolution();

    for (Point &p : sketch.points) {
      const GCS::Point &gp = s.points.lookup(p.uid);
      p.co = double2(*gp.x, *gp.y);
    }
    for (Curve &curve : sketch.curves) {
      if (double *const *rad = s.circle_radius.lookup_ptr(curve.uid)) {
        curve.radius = **rad;
      }
    }

    /* Diagnose again on the solved geometry: dependencies that only show once constraints hold
     * (two horizontals make a parallel redundant) are invisible at a rough initial guess.
     * Diagnosis may reset the parameters, so this comes after reading the solution. */
    s.system.diagnose(GCS::DogLeg);
    result.dof = std::max(0, s.system.dofsNumber());
    redundant.clear();
    s.system.getRedundant(redundant);
    result.redundant.clear();
    tags_to_uids(redundant, result.redundant);

    /* What can still move: parameters the constraints do not determine. */
    GCS::VEC_pD dependent_params;
    s.system.getDependentParams(dependent_params);
    Set<const double *> dependent;
    for (const double *param : dependent_params) {
      dependent.add(param);
    }
    auto point_free = [&](const GCS::Point &p) {
      return dependent.contains(p.x) || dependent.contains(p.y);
    };
    for (const Point &p : sketch.points) {
      if (point_free(s.points.lookup(p.uid))) {
        result.free_points.append(p.uid);
      }
    }
    for (const Curve &curve : sketch.curves) {
      bool free = false;
      if (const GCS::Line *line = s.lines.lookup_ptr(curve.uid)) {
        free = point_free(line->p1) || point_free(line->p2);
      }
      else if (const GCS::Circle *circle = s.circles.lookup_ptr(curve.uid)) {
        free = point_free(circle->center) || dependent.contains(circle->rad);
      }
      else if (const GCS::Arc *arc = s.arcs.lookup_ptr(curve.uid)) {
        free = point_free(arc->center) || point_free(arc->start) || point_free(arc->end) ||
               dependent.contains(arc->rad) || dependent.contains(arc->startAngle) ||
               dependent.contains(arc->endAngle);
      }
      if (free) {
        result.free_curves.append(curve.uid);
      }
    }
    return result;
  }
};

}  // namespace

std::unique_ptr<Solver> solver_planegcs()
{
  return std::make_unique<PlaneGCSSolver>();
}

const Solver &solver_default()
{
  static const std::unique_ptr<Solver> solver = solver_planegcs();
  return *solver;
}

}  // namespace blender::cad::sketch
