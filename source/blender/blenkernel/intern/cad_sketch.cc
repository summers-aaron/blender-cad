/* SPDX-FileCopyrightText: 2026 SimpleCAD Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Sketch constraint solving for #CADSketch: DNA (Blender units) to the solver-neutral
 * `cad::sketch::Sketch` (millimeters) and back.
 */

#include <climits>

#include "DNA_cad_types.h"

#include "BLI_listbase_iterator.hh"
#include "BLI_math_vector.hh"

#include "BKE_cad.hh"

#ifdef WITH_CAD
#  include "CAD_sketch_solver.hh"
#endif

namespace blender {

CADFeature *BKE_cad_sketch_feature(const CADPart *part, const CADSketch *sketch)
{
  for (CADFeature &feature : const_cast<CADPart *>(part)->features) {
    if (feature.sketch == sketch) {
      return &feature;
    }
  }
  return nullptr;
}

#ifdef WITH_CAD

static bool constraint_is_length(const eCADConstraintType type)
{
  return ELEM(type,
              CAD_CONSTRAINT_DISTANCE,
              CAD_CONSTRAINT_DISTANCE_X,
              CAD_CONSTRAINT_DISTANCE_Y,
              CAD_CONSTRAINT_RADIUS,
              CAD_CONSTRAINT_DIAMETER);
}

bool BKE_cad_sketch_solve(const CADPart *part, CADSketch *sketch, const Span<CADSketchDrag> drag)
{
  namespace sk = cad::sketch;
  /* Millimeters keep the solver's absolute tolerances meaningful for typical part sizes. */
  const double mm = part->mm_per_unit;

  sk::Sketch s;
  for (int i = 0; i < sketch->points_num; i++) {
    const CADSketchPoint &p = sketch->points[i];
    s.points.append({p.uid, double2(p.co[0], p.co[1]) * mm, (p.flag & CAD_SKETCH_FIXED) != 0});
  }
  for (int i = 0; i < sketch->curves_num; i++) {
    const CADSketchCurve &c = sketch->curves[i];
    sk::Curve curve;
    curve.uid = c.uid;
    curve.type = sk::CurveType(c.type);
    curve.start = c.points[0];
    curve.end = c.points[1];
    curve.center = c.points[2];
    curve.radius = c.radius * mm;
    curve.ccw = c.ccw != 0;
    s.curves.append(curve);
  }
  for (int i = 0; i < sketch->constraints_num; i++) {
    const CADSketchConstraint &c = sketch->constraints[i];
    sk::Constraint constraint;
    constraint.uid = c.uid;
    constraint.type = sk::ConstraintType(c.type);
    std::copy_n(c.entities, 4, constraint.entities.begin());
    constraint.value = constraint_is_length(eCADConstraintType(c.type)) ? c.value * mm : c.value;
    s.constraints.append(constraint);
  }
  Vector<sk::DragTarget> targets;
  for (const CADSketchDrag &d : drag) {
    targets.append({d.point, d.co * mm});
  }

  const sk::SolveResult result = sk::solver_default().solve(s, targets);

  for (int i = 0; i < sketch->constraints_num; i++) {
    CADSketchConstraint &c = sketch->constraints[i];
    c.flag &= ~(CAD_CONSTRAINT_CONFLICTING | CAD_CONSTRAINT_REDUNDANT | CAD_CONSTRAINT_MALFORMED);
    SET_FLAG_FROM_TEST(c.flag, result.conflicting.contains(c.uid), CAD_CONSTRAINT_CONFLICTING);
    SET_FLAG_FROM_TEST(c.flag, result.redundant.contains(c.uid), CAD_CONSTRAINT_REDUNDANT);
    SET_FLAG_FROM_TEST(c.flag, result.malformed.contains(c.uid), CAD_CONSTRAINT_MALFORMED);
  }
  sketch->dof = short(std::min(result.dof, int(SHRT_MAX)));
  switch (result.status) {
    case sk::SolveStatus::Ok:
      sketch->solve_status = CAD_SKETCH_SOLVE_OK;
      break;
    case sk::SolveStatus::Conflicting:
      sketch->solve_status = CAD_SKETCH_SOLVE_CONFLICTING;
      break;
    case sk::SolveStatus::Failed:
      sketch->solve_status = CAD_SKETCH_SOLVE_FAILED;
      break;
  }
  if (!result.ok()) {
    return false;
  }

  for (int i = 0; i < sketch->points_num; i++) {
    CADSketchPoint &p = sketch->points[i];
    SET_FLAG_FROM_TEST(p.flag, !result.free_points.contains(p.uid), CAD_SKETCH_CONSTRAINED);
  }
  for (int i = 0; i < sketch->curves_num; i++) {
    CADSketchCurve &c = sketch->curves[i];
    SET_FLAG_FROM_TEST(c.flag, !result.free_curves.contains(c.uid), CAD_SKETCH_CONSTRAINED);
  }

  for (int i = 0; i < sketch->points_num; i++) {
    const double2 co = s.points[i].co / mm;
    sketch->points[i].co[0] = co.x;
    sketch->points[i].co[1] = co.y;
  }
  for (int i = 0; i < sketch->curves_num; i++) {
    CADSketchCurve &c = sketch->curves[i];
    if (c.type == CAD_SKETCH_CIRCLE) {
      c.radius = s.curves[i].radius / mm;
    }
    else if (c.type == CAD_SKETCH_ARC) {
      const CADSketchPoint *center = BKE_cad_sketch_point_find(sketch, c.points[2]);
      const CADSketchPoint *start = BKE_cad_sketch_point_find(sketch, c.points[0]);
      if (center && start) {
        c.radius = math::distance(double2(center->co[0], center->co[1]),
                                  double2(start->co[0], start->co[1]));
      }
    }
  }
  return true;
}

#else

bool BKE_cad_sketch_solve(const CADPart * /*part*/,
                          CADSketch * /*sketch*/,
                          const Span<CADSketchDrag> /*drag*/)
{
  return false;
}

#endif

}  // namespace blender
