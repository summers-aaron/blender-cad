/* SPDX-FileCopyrightText: 2026 SimpleCAD Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <numbers>

#include "testing/testing.h"

#include "CAD_eval.hh"

namespace blender::cad::tests {

static SketchCurve line(const int64_t uid, const double2 a, const double2 b)
{
  SketchCurve c;
  c.uid = uid;
  c.type = SketchCurveType::Line;
  c.start = a;
  c.end = b;
  return c;
}

static SketchCurve circle(const int64_t uid, const double2 center, const double radius)
{
  SketchCurve c;
  c.uid = uid;
  c.type = SketchCurveType::Circle;
  c.center = center;
  c.radius = radius;
  return c;
}

static void add_rectangle(SketchData &sketch, const int64_t uid, const double2 a, const double2 b)
{
  /* Deliberately mixed directions: profiles must chain them regardless. */
  sketch.curves.append(line(uid, a, {b.x, a.y}));
  sketch.curves.append(line(uid + 1, b, {b.x, a.y}));
  sketch.curves.append(line(uid + 2, b, {a.x, b.y}));
  sketch.curves.append(line(uid + 3, a, {a.x, b.y}));
}

TEST(cad_profile, rectangle_with_hole)
{
  SketchData sketch;
  add_rectangle(sketch, 10, {0.0, 0.0}, {40.0, 20.0});
  sketch.curves.append(circle(5, {20.0, 10.0}, 4.0));
  const SketchProfiles profiles = sketch_profiles(sketch);
  ASSERT_EQ(profiles.regions.size(), 1);
  const SketchRegion &region = profiles.regions[0];
  EXPECT_EQ(region.key, 10);
  EXPECT_EQ(region.region.outer.curves.size(), 4);
  EXPECT_EQ(region.region.holes.size(), 1);
  EXPECT_NEAR(region.area, 800.0 - std::numbers::pi * 16.0, 1.0);
  EXPECT_TRUE(profiles.open_curves.is_empty());

  /* The loop is chained end to start. */
  const auto &curves = region.region.outer.curves;
  for (size_t i = 0; i < curves.size(); i++) {
    const auto &next = curves[(i + 1) % curves.size()];
    EXPECT_NEAR(curves[i].end[0], next.start[0], 1e-9);
    EXPECT_NEAR(curves[i].end[1], next.start[1], 1e-9);
  }
}

TEST(cad_profile, disjoint_regions_and_island)
{
  SketchData sketch;
  add_rectangle(sketch, 1, {0.0, 0.0}, {10.0, 10.0});
  add_rectangle(sketch, 20, {20.0, 0.0}, {30.0, 10.0});
  /* Ring with an island: outer circle, hole circle, island circle inside the hole. */
  sketch.curves.append(circle(40, {0.0, 50.0}, 10.0));
  sketch.curves.append(circle(41, {0.0, 50.0}, 6.0));
  sketch.curves.append(circle(42, {0.0, 50.0}, 2.0));
  const SketchProfiles profiles = sketch_profiles(sketch);
  ASSERT_EQ(profiles.regions.size(), 4);
  EXPECT_EQ(profiles.regions[0].key, 1);
  EXPECT_EQ(profiles.regions[1].key, 20);
  EXPECT_EQ(profiles.regions[2].key, 40);
  EXPECT_EQ(profiles.regions[2].region.holes.size(), 1);
  EXPECT_EQ(profiles.regions[3].key, 42);
  EXPECT_EQ(profiles.regions[3].region.holes.size(), 0);
}

TEST(cad_profile, open_and_construction_curves)
{
  SketchData sketch;
  sketch.curves.append(line(1, {0.0, 0.0}, {10.0, 0.0}));
  sketch.curves.append(line(2, {10.0, 0.0}, {10.0, 10.0}));
  SketchCurve construction = circle(3, {0.0, 0.0}, 5.0);
  construction.construction = true;
  sketch.curves.append(construction);
  const SketchProfiles profiles = sketch_profiles(sketch);
  EXPECT_TRUE(profiles.regions.is_empty());
  EXPECT_EQ(profiles.open_curves.size(), 2);
}

TEST(cad_profile, slot_with_arcs)
{
  SketchData sketch;
  SketchCurve arc_right;
  arc_right.uid = 3;
  arc_right.type = SketchCurveType::Arc;
  arc_right.center = {10.0, 0.0};
  arc_right.radius = 2.0;
  arc_right.start = {10.0, -2.0};
  arc_right.end = {10.0, 2.0};
  arc_right.ccw = true;
  SketchCurve arc_left = arc_right;
  arc_left.uid = 4;
  arc_left.center = {0.0, 0.0};
  arc_left.start = {0.0, 2.0};
  arc_left.end = {0.0, -2.0};
  sketch.curves.append(line(1, {0.0, -2.0}, {10.0, -2.0}));
  sketch.curves.append(arc_right);
  sketch.curves.append(line(2, {10.0, 2.0}, {0.0, 2.0}));
  sketch.curves.append(arc_left);
  const SketchProfiles profiles = sketch_profiles(sketch);
  ASSERT_EQ(profiles.regions.size(), 1);
  EXPECT_NEAR(profiles.regions[0].area, 40.0 + std::numbers::pi * 4.0, 0.2);
}

}  // namespace blender::cad::tests
