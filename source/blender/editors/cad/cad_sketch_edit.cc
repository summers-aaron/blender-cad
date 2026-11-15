/* SPDX-FileCopyrightText: 2026 SimpleCAD Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edcad
 *
 * Sketch mode: entering and leaving, the sketch plane, picking sketch elements, and the
 * operators that start and finish sketching.
 */

#include <numbers>
#include <tuple>

#include "DNA_cad_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_view3d_types.h"
#include "DNA_windowmanager_types.h"

#include "BLI_listbase.hh"
#include "BLI_math_geom_c.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_quaternion.hh"
#include "BLI_math_rotation_c.hh"
#include "BLI_math_vector.hh"
#include "BLI_set.hh"

#include "BKE_cad.hh"
#include "BKE_context.hh"
#include "BKE_library.hh"
#include "BKE_report.hh"
#include "BKE_screen.hh"

#include "DEG_depsgraph.hh"

#include "ED_object.hh"
#include "ED_screen.hh"
#include "ED_view3d.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "UI_resources.hh"

#include "WM_api.hh"
#include "WM_keymap.hh"
#include "WM_types.hh"

#include "ED_cad.hh"

#include "cad_intern.hh"
#include "cad_sketch.hh"

namespace blender::ed::cad {

/* -------------------------------------------------------------------- */
/** \name Context
 * \{ */

std::optional<SketchEdit> sketch_edit_get(const bContext *C)
{
  Object *ob = CTX_data_edit_object(C);
  if (ob == nullptr || ob->type != OB_CAD || ob->data == nullptr) {
    return std::nullopt;
  }
  CADPart *part = id_cast<CADPart *>(ob->data);
  CADFeature *feature = BKE_cad_feature_find(part, part->edit_sketch_uid);
  if (feature == nullptr || feature->sketch == nullptr) {
    return std::nullopt;
  }
  return SketchEdit{ob, part, feature, feature->sketch};
}

bool sketch_edit_poll(bContext *C)
{
  return sketch_edit_get(C).has_value();
}

bool sketch_edit_poll_view3d(bContext *C)
{
  return sketch_edit_poll(C) && CTX_wm_region_view3d(C) != nullptr;
}

bool sketch_changed(bContext *C, const SketchEdit &edit, const Span<int> remove_on_conflict)
{
  bool ok = BKE_cad_sketch_solve(edit.part, edit.sketch);
  if (!ok && edit.sketch->solve_status == CAD_SKETCH_SOLVE_CONFLICTING &&
      !remove_on_conflict.is_empty())
  {
    BKE_cad_sketch_remove(edit.sketch, remove_on_conflict);
    BKE_cad_sketch_solve(edit.part, edit.sketch);
  }
  DEG_id_tag_update(&edit.part->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, edit.part);
  if (ARegion *region = CTX_wm_region(C)) {
    ED_region_tag_redraw(region);
  }
  return ok;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Sketch Plane
 * \{ */

SketchFrame sketch_frame(const Object *ob, const CADSketch *sketch)
{
  const float3 origin(float(sketch->origin[0]), float(sketch->origin[1]), float(sketch->origin[2]));
  const float3 normal = math::normalize(
      float3(float(sketch->normal[0]), float(sketch->normal[1]), float(sketch->normal[2])));
  float3 x_dir = float3(float(sketch->x_dir[0]), float(sketch->x_dir[1]), float(sketch->x_dir[2]));
  x_dir = math::normalize(x_dir - normal * math::dot(x_dir, normal));
  const float3 y_dir = math::cross(normal, x_dir);

  float4x4 local = float4x4::identity();
  local.x_axis() = x_dir;
  local.y_axis() = y_dir;
  local.z_axis() = normal;
  local.location() = origin;

  SketchFrame frame;
  frame.to_world = ob->object_to_world() * local;
  frame.to_sketch = math::invert(frame.to_world);
  frame.origin_world = frame.to_world.location();
  frame.normal_world = math::normalize(frame.to_world.z_axis());
  return frame;
}

std::optional<double2> sketch_mouse_co(const ARegion *region,
                                       const SketchFrame &frame,
                                       const float2 &mval)
{
  float plane[4];
  plane_from_point_normal_v3(plane, frame.origin_world, frame.normal_world);
  float3 world;
  if (!ED_view3d_win_to_3d_on_plane(region, plane, mval, false, world)) {
    return std::nullopt;
  }
  const float3 local = math::transform_point(frame.to_sketch, world);
  return double2(local.x, local.y);
}

float2 sketch_to_region(const ARegion *region, const SketchFrame &frame, const double2 &co)
{
  const float3 world = math::transform_point(frame.to_world, float3(float(co.x), float(co.y), 0.0f));
  float2 r_co(0.0f);
  ED_view3d_project_float_global(region, world, r_co, V3D_PROJ_TEST_NOP);
  return r_co;
}

static double2 point_co(const CADSketch *sketch, const int uid)
{
  if (const CADSketchPoint *p = BKE_cad_sketch_point_find(sketch, uid)) {
    return double2(p->co[0], p->co[1]);
  }
  return double2(0.0);
}

Vector<double2> sketch_curve_polyline(const CADSketch *sketch,
                                      const int curve_index,
                                      const int resolution)
{
  const CADSketchCurve &c = sketch->curves[curve_index];
  Vector<double2> poly;
  switch (c.type) {
    case CAD_SKETCH_LINE:
      poly.append(point_co(sketch, c.points[0]));
      poly.append(point_co(sketch, c.points[1]));
      break;
    case CAD_SKETCH_CIRCLE: {
      const double2 center = point_co(sketch, c.points[2]);
      for (int i = 0; i <= resolution; i++) {
        const double a = 2.0 * std::numbers::pi * i / resolution;
        poly.append(center + double2(std::cos(a), std::sin(a)) * c.radius);
      }
      break;
    }
    case CAD_SKETCH_ARC: {
      const double2 center = point_co(sketch, c.points[2]);
      const double2 start = point_co(sketch, c.points[0]);
      const double2 end = point_co(sketch, c.points[1]);
      const double r = math::distance(start, center);
      const double a0 = std::atan2(start.y - center.y, start.x - center.x);
      const double a1 = std::atan2(end.y - center.y, end.x - center.x);
      double sweep = a1 - a0;
      if (c.ccw) {
        while (sweep <= 0.0) {
          sweep += 2.0 * std::numbers::pi;
        }
      }
      else {
        while (sweep >= 0.0) {
          sweep -= 2.0 * std::numbers::pi;
        }
      }
      const int steps = std::max(2, int(resolution * std::abs(sweep) / (2.0 * std::numbers::pi)));
      for (int i = 0; i <= steps; i++) {
        const double a = a0 + sweep * i / steps;
        poly.append(center + double2(std::cos(a), std::sin(a)) * r);
      }
      break;
    }
  }
  return poly;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Selection State
 * \{ */

bool sketch_element_selected(const CADSketch *sketch, const SketchHitType type, const int uid)
{
  switch (type) {
    case SketchHitType::Point:
      if (const CADSketchPoint *p = BKE_cad_sketch_point_find(sketch, uid)) {
        return p->flag & CAD_SKETCH_SELECT;
      }
      break;
    case SketchHitType::Curve:
      if (const CADSketchCurve *c = BKE_cad_sketch_curve_find(sketch, uid)) {
        return c->flag & CAD_SKETCH_SELECT;
      }
      break;
    case SketchHitType::Constraint:
      if (const CADSketchConstraint *c = BKE_cad_sketch_constraint_find(sketch, uid)) {
        return c->flag & CAD_CONSTRAINT_SELECT;
      }
      break;
    default:
      break;
  }
  return false;
}

void sketch_element_select_set(CADSketch *sketch,
                               const SketchHitType type,
                               const int uid,
                               const bool select)
{
  switch (type) {
    case SketchHitType::Point:
      if (CADSketchPoint *p = BKE_cad_sketch_point_find(sketch, uid)) {
        SET_FLAG_FROM_TEST(p->flag, select, CAD_SKETCH_SELECT);
      }
      break;
    case SketchHitType::Curve:
      if (CADSketchCurve *c = BKE_cad_sketch_curve_find(sketch, uid)) {
        SET_FLAG_FROM_TEST(c->flag, select, CAD_SKETCH_SELECT);
      }
      break;
    case SketchHitType::Constraint:
      if (CADSketchConstraint *c = BKE_cad_sketch_constraint_find(sketch, uid)) {
        SET_FLAG_FROM_TEST(c->flag, select, CAD_CONSTRAINT_SELECT);
      }
      break;
    default:
      break;
  }
}

static SketchHover g_hover;

SketchHover &sketch_hover()
{
  return g_hover;
}

void sketch_hover_clear()
{
  g_hover = SketchHover();
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Picking
 * \{ */

static float dist_to_segment(const float2 &p, const float2 &a, const float2 &b, float &r_t)
{
  const float2 ab = b - a;
  const float len_sq = math::length_squared(ab);
  r_t = len_sq > 0.0f ? std::clamp(math::dot(p - a, ab) / len_sq, 0.0f, 1.0f) : 0.0f;
  return math::distance(p, a + ab * r_t);
}

static bool constraint_has_label(const CADSketchConstraint &c)
{
  return !ELEM(c.type, CAD_CONSTRAINT_COINCIDENT, CAD_CONSTRAINT_POINT_ON_CURVE);
}

/** Anchor of a constraint label in sketch coordinates, and a pixel offset from it. */
static void constraint_anchor(const ARegion *region,
                              const SketchFrame &frame,
                              const CADSketch *sketch,
                              const CADSketchConstraint &c,
                              double2 &r_anchor,
                              float2 &r_offset_px)
{
  r_offset_px = float2(0.0f);
  auto curve_mid = [&](const int uid, double2 &r_mid, float2 &r_side) -> bool {
    const CADSketchCurve *curve = BKE_cad_sketch_curve_find(sketch, uid);
    if (curve == nullptr) {
      return false;
    }
    const int index = int(curve - sketch->curves);
    const Vector<double2> poly = sketch_curve_polyline(sketch, index, 32);
    if (curve->type == CAD_SKETCH_LINE) {
      r_mid = (poly[0] + poly[1]) * 0.5;
      const float2 a = sketch_to_region(region, frame, poly[0]);
      const float2 b = sketch_to_region(region, frame, poly[1]);
      const float2 d = math::normalize(b - a);
      r_side = float2(-d.y, d.x);
    }
    else {
      const double2 center = point_co(sketch, curve->points[2]);
      const double r = curve->type == CAD_SKETCH_CIRCLE ?
                           curve->radius :
                           math::distance(point_co(sketch, curve->points[0]), center);
      r_mid = center + double2(std::numbers::sqrt2 / 2.0, std::numbers::sqrt2 / 2.0) * r;
      if (curve->type == CAD_SKETCH_ARC) {
        r_mid = poly[poly.size() / 2];
      }
      r_side = math::normalize(sketch_to_region(region, frame, r_mid) -
                               sketch_to_region(region, frame, center));
    }
    return true;
  };

  const int e0 = c.entities[0], e1 = c.entities[1];
  const bool p0 = BKE_cad_sketch_point_find(sketch, e0) != nullptr;
  const bool p1 = BKE_cad_sketch_point_find(sketch, e1) != nullptr;
  double2 mid(0.0);
  float2 side(0.0f, 1.0f);
  const bool is_dimension = c.type >= CAD_CONSTRAINT_DISTANCE;
  const float gap = (is_dimension ? 22.0f : 12.0f) * UI_SCALE_FAC;

  if (p0 && p1) {
    mid = (point_co(sketch, e0) + point_co(sketch, e1)) * 0.5;
    const float2 a = sketch_to_region(region, frame, point_co(sketch, e0));
    const float2 b = sketch_to_region(region, frame, point_co(sketch, e1));
    const float2 d = math::length(b - a) > 0.0f ? math::normalize(b - a) : float2(1.0f, 0.0f);
    side = float2(-d.y, d.x);
  }
  else if (p0) {
    mid = point_co(sketch, e0);
    side = math::normalize(float2(1.0f, 1.0f));
    if (BKE_cad_sketch_curve_find(sketch, e1)) {
      /* Point to line distance: between the point and the line. */
      double2 line_mid;
      float2 unused;
      curve_mid(e1, line_mid, unused);
      mid = (mid + line_mid) * 0.5;
    }
  }
  else if (curve_mid(e0, mid, side)) {
    if (c.type == CAD_CONSTRAINT_ANGLE && BKE_cad_sketch_curve_find(sketch, e1)) {
      double2 mid1;
      float2 unused;
      curve_mid(e1, mid1, unused);
      mid = (mid + mid1) * 0.5;
      side = float2(0.0f);
    }
  }
  else if (e0 == CAD_SKETCH_ORIGIN) {
    mid = double2(0.0);
  }
  /* Without a placed position, the label keeps a fixed pixel distance from the geometry. */
  const bool placed = c.label_offset[0] != 0.0f || c.label_offset[1] != 0.0f;
  r_anchor = mid + double2(c.label_offset[0], c.label_offset[1]);
  r_offset_px = placed ? float2(0.0f) : side * gap;
}

float2 sketch_constraint_label_position(const ARegion *region,
                                        const SketchFrame &frame,
                                        const CADSketch *sketch,
                                        const int constraint_index)
{
  double2 anchor;
  float2 offset;
  constraint_anchor(region, frame, sketch, sketch->constraints[constraint_index], anchor, offset);
  return sketch_to_region(region, frame, anchor) + offset;
}

void sketch_constraint_label_place(const ARegion *region,
                                   const SketchFrame &frame,
                                   CADSketch *sketch,
                                   const int constraint_index,
                                   const double2 &co)
{
  CADSketchConstraint &c = sketch->constraints[constraint_index];
  c.label_offset[0] = c.label_offset[1] = 0.0f;
  double2 anchor;
  float2 unused;
  constraint_anchor(region, frame, sketch, c, anchor, unused);
  float2 offset(co - anchor);
  if (offset.x == 0.0f && offset.y == 0.0f) {
    /* Zero means "not placed". */
    offset.x = 1e-6f;
  }
  c.label_offset[0] = offset.x;
  c.label_offset[1] = offset.y;
}

static bool is_point_uid(const CADSketch *sketch, const int uid)
{
  return uid == CAD_SKETCH_ORIGIN || BKE_cad_sketch_point_find(sketch, uid) != nullptr;
}

SketchDimensionGeometry sketch_dimension_geometry(const ARegion *region,
                                                  const SketchFrame &frame,
                                                  const CADSketch *sketch,
                                                  const int constraint_index)
{
  SketchDimensionGeometry g;
  const CADSketchConstraint &c = sketch->constraints[constraint_index];
  if (c.type < CAD_CONSTRAINT_DISTANCE) {
    return g;
  }
  const float2 label = sketch_constraint_label_position(region, frame, sketch, constraint_index);
  auto px = [&](const double2 &co) { return sketch_to_region(region, frame, co); };
  const float ext_gap = 4.0f * UI_SCALE_FAC;
  const float ext_over = 6.0f * UI_SCALE_FAC;

  auto line = [&](const float2 &a, const float2 &b) {
    g.lines.append(a);
    g.lines.append(b);
  };
  /* A linear dimension measuring `a` to `b` along `dir` (region pixels): extension lines from
   * the measured points to a dimension line through the label, arrows at both ends. */
  auto linear = [&](float2 a, float2 b, float2 dir) {
    if (math::dot(b - a, dir) < 0.0f) {
      std::swap(a, b);
    }
    const float2 n(-dir.y, dir.x);
    const float ha = math::dot(label - a, n);
    const float hb = math::dot(label - b, n);
    const float2 a2 = a + n * ha;
    const float2 b2 = b + n * hb;
    for (const auto &[p, h] : {std::pair(a, ha), std::pair(b, hb)}) {
      if (std::abs(h) > ext_gap) {
        const float side = h > 0.0f ? 1.0f : -1.0f;
        line(p + n * (side * ext_gap), p + n * (h + side * ext_over));
      }
    }
    const float length = math::dot(b2 - a2, dir);
    const float t_label = math::dot(label - a2, dir);
    line(a2 + dir * std::min(0.0f, t_label), a2 + dir * std::max(length, t_label));
    g.arrows.append({a2, -dir});
    g.arrows.append({b2, dir});
  };
  auto axis_dir = [&](const double2 &axis) {
    const float2 d = px(axis) - px(double2(0.0));
    return math::length(d) > 0.0f ? math::normalize(d) : float2(1.0f, 0.0f);
  };

  const int e0 = c.entities[0], e1 = c.entities[1];
  const CADSketchCurve *c0 = BKE_cad_sketch_curve_find(sketch, e0);
  const CADSketchCurve *c1 = BKE_cad_sketch_curve_find(sketch, e1);

  switch (c.type) {
    case CAD_CONSTRAINT_DISTANCE:
    case CAD_CONSTRAINT_DISTANCE_X:
    case CAD_CONSTRAINT_DISTANCE_Y: {
      double2 a, b;
      if (c0 && c0->type == CAD_SKETCH_LINE && e1 == -1) {
        a = point_co(sketch, c0->points[0]);
        b = point_co(sketch, c0->points[1]);
      }
      else if (is_point_uid(sketch, e0) && c1 && c1->type == CAD_SKETCH_LINE) {
        /* Point to line: measured along the line's normal, from the foot point. */
        a = point_co(sketch, e0);
        const double2 l0 = point_co(sketch, c1->points[0]);
        const double2 l1 = point_co(sketch, c1->points[1]);
        const double2 d = l1 - l0;
        const double len_sq = math::dot(d, d);
        b = len_sq > 0.0 ? l0 + d * (math::dot(a - l0, d) / len_sq) : l0;
      }
      else if (is_point_uid(sketch, e0) && is_point_uid(sketch, e1)) {
        a = point_co(sketch, e0);
        b = point_co(sketch, e1);
      }
      else {
        break;
      }
      const float2 pa = px(a), pb = px(b);
      float2 dir;
      if (c.type == CAD_CONSTRAINT_DISTANCE_X) {
        dir = axis_dir(double2(1.0, 0.0));
      }
      else if (c.type == CAD_CONSTRAINT_DISTANCE_Y) {
        dir = axis_dir(double2(0.0, 1.0));
      }
      else if (math::distance(pa, pb) > 1e-3f) {
        dir = math::normalize(pb - pa);
      }
      else {
        break;
      }
      linear(pa, pb, dir);
      break;
    }
    case CAD_CONSTRAINT_RADIUS:
    case CAD_CONSTRAINT_DIAMETER: {
      if (c0 == nullptr || c0->type == CAD_SKETCH_LINE) {
        break;
      }
      const double2 center = point_co(sketch, c0->points[2]);
      const double radius = c0->type == CAD_SKETCH_CIRCLE ?
                                c0->radius :
                                math::distance(point_co(sketch, c0->points[0]), center);
      const std::optional<double2> label_co = sketch_mouse_co(region, frame, label);
      double2 d = label_co ? *label_co - center : double2(1.0, 0.0);
      d = math::length(d) > 0.0 ? math::normalize(d) : double2(1.0, 0.0);
      const float2 pc = px(center), rim = px(center + d * radius);
      const float2 out = math::length(rim - pc) > 0.0f ? math::normalize(rim - pc) :
                                                         float2(1.0f, 0.0f);
      const bool label_outside = math::distance(label, pc) > math::distance(rim, pc);
      const float2 end = label_outside ? label : rim;
      if (c.type == CAD_CONSTRAINT_RADIUS) {
        line(pc, end);
      }
      else {
        const float2 opposite = px(center - d * radius);
        line(opposite, end);
        g.arrows.append({opposite, -out});
      }
      g.arrows.append({rim, out});
      break;
    }
    case CAD_CONSTRAINT_ANGLE: {
      if (c0 == nullptr || c1 == nullptr || c0->type != CAD_SKETCH_LINE ||
          c1->type != CAD_SKETCH_LINE)
      {
        break;
      }
      const double2 p0 = point_co(sketch, c0->points[0]), q0 = point_co(sketch, c1->points[0]);
      const double2 d0 = math::normalize(point_co(sketch, c0->points[1]) - p0);
      const double2 d1 = math::normalize(point_co(sketch, c1->points[1]) - q0);
      auto cross = [](const double2 &u, const double2 &v) { return u.x * v.y - u.y * v.x; };
      const double denom = cross(d0, d1);
      const std::optional<double2> label_co = sketch_mouse_co(region, frame, label);
      if (std::abs(denom) < 1e-9 || !label_co) {
        break;
      }
      const double2 x = p0 + d0 * (cross(q0 - p0, d1) / denom);
      const double2 to_label = *label_co - x;
      const double rho = math::length(to_label);
      if (rho <= 0.0) {
        break;
      }
      /* The sector between the two rays (each line in either direction) holding the label. */
      for (const double s0 : {1.0, -1.0}) {
        for (const double s1 : {1.0, -1.0}) {
          const double2 r0 = d0 * s0, r1 = d1 * s1;
          const double sweep = cross(r0, r1);
          if (cross(r0, to_label) * sweep < 0.0 || cross(to_label, r1) * sweep < 0.0 ||
              math::dot(r0 + r1, to_label) < 0.0)
          {
            continue;
          }
          const double a0 = std::atan2(r0.y, r0.x);
          const double angle = std::atan2(sweep, math::dot(r0, r1));
          const int steps = 24;
          float2 prev = px(x + r0 * rho);
          for (int i = 1; i <= steps; i++) {
            const double a = a0 + angle * i / steps;
            const float2 cur = px(x + double2(std::cos(a), std::sin(a)) * rho);
            line(prev, cur);
            prev = cur;
          }
          const float2 start = px(x + r0 * rho), end = prev;
          /* Arrows tangent to the arc, pointing away from its middle. */
          auto tangent = [&](const double a, const double sign) {
            const float2 t = px(x + double2(std::cos(a), std::sin(a)) * rho) -
                             px(x + double2(std::cos(a - sign * 0.01), std::sin(a - sign * 0.01)) * rho);
            return math::length(t) > 0.0f ? math::normalize(t) : float2(0.0f);
          };
          const double dir_sign = angle >= 0.0 ? 1.0 : -1.0;
          g.arrows.append({start, tangent(a0, -dir_sign)});
          g.arrows.append({end, tangent(a0 + angle, dir_sign)});
          /* Extension lines when the arc lies beyond the lines' ends. */
          for (const auto &[r, p, q] : {std::tuple(r0, p0, point_co(sketch, c0->points[1])),
                                        std::tuple(r1, q0, point_co(sketch, c1->points[1]))})
          {
            const double reach = std::max(math::dot(p - x, r), math::dot(q - x, r));
            if (rho > reach) {
              const float2 from = px(x + r * std::max(reach, 0.0));
              const float2 to = px(x + r * rho);
              line(from, to + math::normalize(to - from) * ext_over);
            }
          }
          return g;
        }
      }
      break;
    }
    default:
      break;
  }
  return g;
}

SketchHit sketch_pick(const ARegion *region,
                      const SketchFrame &frame,
                      const CADSketch *sketch,
                      const float2 &mval,
                      const uint8_t filter,
                      const int ignore_point)
{
  const float point_radius = 9.0f * UI_SCALE_FAC;
  const float curve_radius = 7.0f * UI_SCALE_FAC;
  const float label_radius = 13.0f * UI_SCALE_FAC;

  SketchHit best;
  best.dist_px = point_radius;
  if (filter & PICK_POINTS) {
    for (int i = 0; i < sketch->points_num; i++) {
      const CADSketchPoint &p = sketch->points[i];
      if (p.uid == ignore_point) {
        continue;
      }
      const double2 co(p.co[0], p.co[1]);
      const float d = math::distance(sketch_to_region(region, frame, co), mval);
      if (d < best.dist_px) {
        best = {SketchHitType::Point, p.uid, co, d};
      }
    }
  }
  if (filter & PICK_ORIGIN) {
    const float d = math::distance(sketch_to_region(region, frame, double2(0.0)), mval);
    if (d < best.dist_px) {
      best = {SketchHitType::Origin, CAD_SKETCH_ORIGIN, double2(0.0), d};
    }
  }
  if (best.type != SketchHitType::None) {
    return best;
  }

  if (filter & PICK_CONSTRAINTS) {
    best.dist_px = label_radius;
    for (int i = 0; i < sketch->constraints_num; i++) {
      const CADSketchConstraint &c = sketch->constraints[i];
      if (!constraint_has_label(c)) {
        continue;
      }
      const float d = math::distance(sketch_constraint_label_position(region, frame, sketch, i),
                                     mval);
      if (d < best.dist_px) {
        best = {SketchHitType::Constraint, c.uid, double2(0.0), d};
      }
    }
    if (best.type != SketchHitType::None) {
      return best;
    }
  }

  if (filter & PICK_CURVES) {
    best.dist_px = curve_radius;
    for (int i = 0; i < sketch->curves_num; i++) {
      const Vector<double2> poly = sketch_curve_polyline(sketch, i, 64);
      for (int k = 0; k + 1 < poly.size(); k++) {
        const float2 a = sketch_to_region(region, frame, poly[k]);
        const float2 b = sketch_to_region(region, frame, poly[k + 1]);
        float t;
        const float d = dist_to_segment(mval, a, b, t);
        if (d < best.dist_px) {
          best = {SketchHitType::Curve,
                  sketch->curves[i].uid,
                  math::interpolate(poly[k], poly[k + 1], double(t)),
                  d};
        }
      }
    }
  }
  return best;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Enter / Exit
 * \{ */

bool sketch_edit_enter(Main * /*bmain*/, Scene * /*scene*/, Object *ob)
{
  CADPart *part = id_cast<CADPart *>(ob->data);
  CADFeature *feature = BKE_cad_feature_find(part, part->edit_sketch_uid);
  if (feature == nullptr || feature->sketch == nullptr) {
    CADFeature *active = BKE_cad_feature_active(part);
    feature = (active && active->sketch) ? active : nullptr;
  }
  if (feature == nullptr) {
    /* Nothing to edit yet: start a sketch on the XY plane. */
    feature = BKE_cad_feature_add(part, CAD_FEATURE_SKETCH);
    feature_insert_at_rollback(part, feature);
  }
  part->edit_sketch_uid = feature->uid;
  part->active_feature_uid = feature->uid;
  /* Show the model as it is when the sketch is made. Re-entering after an undo step or file
   * load (the flag is still set) keeps the marker saved when the mode was entered. */
  if ((part->flag & CAD_PART_SKETCH_EDITING) == 0) {
    part->edit_prev_rollback_uid = part->rollback_uid;
    part->flag |= CAD_PART_SKETCH_EDITING;
  }
  part->rollback_uid = feature->uid;
  /* Also without constraints: the solver tells what is fully constrained. */
  BKE_cad_sketch_solve(part, feature->sketch);
  sketch_hover_clear();
  DEG_id_tag_update(&part->id, ID_RECALC_GEOMETRY);
  return true;
}

void sketch_edit_exit(Main * /*bmain*/, Object *ob)
{
  CADPart *part = id_cast<CADPart *>(ob->data);
  if (CADFeature *feature = BKE_cad_feature_find(part, part->edit_sketch_uid)) {
    if (feature->sketch) {
      BKE_cad_sketch_select_all(feature->sketch, false);
    }
  }
  /* The marker returns where it was (the end, when it was not set). */
  part->rollback_uid = part->edit_prev_rollback_uid;
  if (part->rollback_uid != -1 && !BKE_cad_feature_find(part, part->rollback_uid)) {
    part->rollback_uid = -1;
  }
  part->edit_prev_rollback_uid = -1;
  part->edit_sketch_uid = -1;
  part->flag &= ~CAD_PART_SKETCH_EDITING;
  sketch_hover_clear();
  DEG_id_tag_update(&part->id, ID_RECALC_GEOMETRY);
}

/** Turns the view to look straight at the sketch plane. */
static void view_look_at_sketch(bContext *C, const SketchFrame &frame)
{
  ARegion *region = CTX_wm_region(C);
  RegionView3D *rv3d = CTX_wm_region_view3d(C);
  if (region == nullptr || rv3d == nullptr) {
    return;
  }
  /* Rows of the view rotation: sketch X right, sketch Y up, normal towards the viewer. */
  float3x3 basis;
  basis.x_axis() = math::normalize(frame.to_world.x_axis());
  basis.y_axis() = math::normalize(frame.to_world.y_axis());
  basis.z_axis() = math::normalize(frame.to_world.z_axis());
  const float3x3 view_rot = math::transpose(basis);
  float quat[4];
  mat3_to_quat(quat, view_rot.ptr());
  copy_qt_qt(rv3d->viewquat, quat);
  rv3d->view = RV3D_VIEW_USER;
  rv3d->ofs[0] = -frame.origin_world.x;
  rv3d->ofs[1] = -frame.origin_world.y;
  rv3d->ofs[2] = -frame.origin_world.z;
  ED_region_tag_redraw(region);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Operators
 * \{ */

static CADPart *active_part(bContext *C, Object **r_ob)
{
  Object *ob = CTX_data_active_object(C);
  if (ob == nullptr || ob->type != OB_CAD || ob->data == nullptr) {
    return nullptr;
  }
  *r_ob = ob;
  return id_cast<CADPart *>(ob->data);
}

static bool active_part_editable_poll(bContext *C)
{
  Object *ob = nullptr;
  CADPart *part = active_part(C, &ob);
  if (part == nullptr) {
    CTX_wm_operator_poll_msg_set(C, "No active CAD part");
    return false;
  }
  return ID_IS_EDITABLE(&part->id) && ID_IS_EDITABLE(&ob->id);
}

/** Enters sketch mode on `feature` (leaving another sketch first). */
static bool sketch_mode_begin(bContext *C, Object *ob, CADFeature *feature, const bool look_at)
{
  CADPart *part = id_cast<CADPart *>(ob->data);
  if (ob->mode & OB_MODE_EDIT) {
    if (part->edit_sketch_uid == feature->uid) {
      return true;
    }
    object::mode_set(C, OB_MODE_OBJECT);
  }
  part->edit_sketch_uid = feature->uid;
  if (!object::mode_set(C, OB_MODE_EDIT)) {
    part->edit_sketch_uid = -1;
    return false;
  }
  if (look_at) {
    view_look_at_sketch(C, sketch_frame(ob, feature->sketch));
  }
  return true;
}

static wmOperatorStatus sketch_add_exec(bContext *C, wmOperator *op)
{
  Object *ob = nullptr;
  CADPart *part = active_part(C, &ob);
  const int plane = RNA_enum_get(op->ptr, "plane");
  const float offset = RNA_float_get(op->ptr, "offset");

  if (ob->mode & OB_MODE_EDIT) {
    object::mode_set(C, OB_MODE_OBJECT);
  }
  CADFeature *feature = BKE_cad_feature_add(part, CAD_FEATURE_SKETCH);
  feature_insert_at_rollback(part, feature);
  CADSketch *sketch = feature->sketch;
  /* Frames match the view axes: looking down at XY, from the front at XZ, from the right
   * at YZ. */
  const double3 x_dir[3] = {{1, 0, 0}, {1, 0, 0}, {0, 1, 0}};
  const double3 normal[3] = {{0, 0, 1}, {0, -1, 0}, {1, 0, 0}};
  for (int i = 0; i < 3; i++) {
    sketch->x_dir[i] = x_dir[plane][i];
    sketch->normal[i] = normal[plane][i];
    sketch->origin[i] = normal[plane][i] * offset;
  }
  DEG_id_tag_update(&part->id, ID_RECALC_GEOMETRY);

  if (!sketch_mode_begin(C, ob, feature, RNA_boolean_get(op->ptr, "look_at"))) {
    BKE_report(op->reports, RPT_ERROR, "Could not enter sketch mode");
    return OPERATOR_CANCELLED;
  }
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, part);
  return OPERATOR_FINISHED;
}

void CAD_OT_sketch_add(wmOperatorType *ot)
{
  static const EnumPropertyItem plane_items[] = {
      {0, "XY", 0, "XY Plane", "Sketch on the ground plane (top view)"},
      {1, "XZ", 0, "XZ Plane", "Sketch on the front plane"},
      {2, "YZ", 0, "YZ Plane", "Sketch on the side plane"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  ot->name = "New Sketch";
  ot->description = "Start a sketch on an origin plane of the CAD part";
  ot->idname = "CAD_OT_sketch_add";

  ot->exec = sketch_add_exec;
  ot->poll = active_part_editable_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  ot->prop = RNA_def_enum(ot->srna, "plane", plane_items, 0, "Plane", "");
  RNA_def_float_distance(
      ot->srna, "offset", 0.0f, -FLT_MAX, FLT_MAX, "Offset", "Distance along the normal", -10, 10);
  PropertyRNA *prop = RNA_def_boolean(
      ot->srna, "look_at", true, "Look At", "Turn the view to face the sketch");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
}

static wmOperatorStatus sketch_edit_exec(bContext *C, wmOperator *op)
{
  Object *ob = nullptr;
  CADPart *part = active_part(C, &ob);
  const int uid = RNA_int_get(op->ptr, "feature_uid");
  CADFeature *feature = uid >= 0 ? BKE_cad_feature_find(part, uid) : BKE_cad_feature_active(part);
  if (feature == nullptr || feature->sketch == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "Not a sketch");
    return OPERATOR_CANCELLED;
  }
  if (!sketch_mode_begin(C, ob, feature, RNA_boolean_get(op->ptr, "look_at"))) {
    BKE_report(op->reports, RPT_ERROR, "Could not enter sketch mode");
    return OPERATOR_CANCELLED;
  }
  return OPERATOR_FINISHED;
}

void CAD_OT_sketch_edit(wmOperatorType *ot)
{
  ot->name = "Edit Sketch";
  ot->description = "Edit a sketch of the CAD part (sketch mode)";
  ot->idname = "CAD_OT_sketch_edit";

  ot->exec = sketch_edit_exec;
  ot->poll = active_part_editable_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  PropertyRNA *prop = RNA_def_int(ot->srna,
                                  "feature_uid",
                                  -1,
                                  -1,
                                  INT_MAX,
                                  "Feature",
                                  "Uid of the sketch feature (-1: the active feature)",
                                  -1,
                                  INT_MAX);
  RNA_def_property_flag(prop, PropertyFlag(PROP_HIDDEN | PROP_SKIP_SAVE));
  prop = RNA_def_boolean(ot->srna, "look_at", false, "Look At", "Turn the view to face the sketch");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
}

static wmOperatorStatus sketch_finish_exec(bContext *C, wmOperator * /*op*/)
{
  object::mode_set(C, OB_MODE_OBJECT);
  return OPERATOR_FINISHED;
}

void CAD_OT_sketch_finish(wmOperatorType *ot)
{
  ot->name = "Finish Sketch";
  ot->description = "Leave sketch mode";
  ot->idname = "CAD_OT_sketch_finish";

  ot->exec = sketch_finish_exec;
  ot->poll = sketch_edit_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Keymap
 * \{ */

static bool sketch_keymap_poll(bContext *C)
{
  return sketch_edit_poll(C);
}

void sketch_keymap(wmKeyConfig *keyconf)
{
  wmKeyMap *keymap = WM_keymap_ensure(keyconf, "CAD Sketch", SPACE_EMPTY, RGN_TYPE_WINDOW);
  keymap->poll = sketch_keymap_poll;
}

/** \} */

}  // namespace blender::ed::cad
