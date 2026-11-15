/* SPDX-FileCopyrightText: 2026 SimpleCAD Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Kernel conformance suite: every scenario runs on every compiled-in backend, and checks the
 * contract of CAD_kernel.hh (validity, analytic volumes, closed welded tessellation, history
 * completeness, STEP round trip).
 */

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <set>

#include "testing/testing.h"

#include "CAD_kernel.hh"

namespace cad::kernel::tests {

/** Backends grow operation by operation: an unimplemented operation skips, it does not fail. */
#define SKIP_IF_UNSUPPORTED(op_result) \
  if ((op_result).status == Status::Unsupported) { \
    GTEST_SKIP() << (op_result).message; \
  } \
  ((void)0)

static std::string kernel_test_name(const testing::TestParamInfo<KernelId> &info)
{
  switch (info.param) {
    case KernelId::OCCT:
      return "OCCT";
    case KernelId::SMLib:
      return "SMLib";
  }
  return "Unknown";
}

class KernelTest : public testing::TestWithParam<KernelId> {
 protected:
  const Kernel &kernel() const
  {
    return *kernel_get(GetParam());
  }
};

/* -------------------------------------------------------------------- */
/** \name Helpers
 * \{ */

static ProfileCurve line(const int64_t id, const Vec2 a, const Vec2 b)
{
  ProfileCurve c;
  c.type = ProfileCurveType::Line;
  c.id = id;
  c.start = a;
  c.end = b;
  return c;
}

static ProfileCurve circle(const int64_t id, const Vec2 center, const double radius)
{
  ProfileCurve c;
  c.type = ProfileCurveType::Circle;
  c.id = id;
  c.center = center;
  c.radius = radius;
  return c;
}

/** Axis-aligned rectangle loop, curve ids `first_id` .. `first_id + 3`. */
static ProfileLoop rectangle(const int64_t first_id, const Vec2 min, const Vec2 max, bool ccw)
{
  ProfileLoop loop;
  const Vec2 p0 = min, p1 = {max[0], min[1]}, p2 = max, p3 = {min[0], max[1]};
  if (ccw) {
    loop.curves = {line(first_id, p0, p1),
                   line(first_id + 1, p1, p2),
                   line(first_id + 2, p2, p3),
                   line(first_id + 3, p3, p0)};
  }
  else {
    loop.curves = {line(first_id, p0, p3),
                   line(first_id + 1, p3, p2),
                   line(first_id + 2, p2, p1),
                   line(first_id + 3, p1, p0)};
  }
  return loop;
}

static void expect_closed_mesh(const Tessellation &mesh)
{
  /* Every undirected edge of a closed, consistently oriented triangle mesh is used exactly
   * twice, once in each direction. */
  std::map<std::pair<int, int>, int> directed;
  for (const std::array<int, 3> &tri : mesh.triangles) {
    for (int i = 0; i < 3; i++) {
      directed[{tri[i], tri[(i + 1) % 3]}]++;
    }
  }
  int open = 0, flipped = 0;
  for (const auto &[edge, count] : directed) {
    const auto reverse = directed.find({edge.second, edge.first});
    if (reverse == directed.end()) {
      open++;
    }
    if (count > 1) {
      flipped++;
    }
  }
  EXPECT_EQ(open, 0) << "open (unwelded or missing) mesh edges";
  EXPECT_EQ(flipped, 0) << "inconsistently oriented triangles";
}

static double mesh_volume(const Tessellation &mesh)
{
  double volume = 0.0;
  for (const std::array<int, 3> &tri : mesh.triangles) {
    const Float3 &a = mesh.positions[tri[0]];
    const Float3 &b = mesh.positions[tri[1]];
    const Float3 &c = mesh.positions[tri[2]];
    volume += (double(a[0]) * (double(b[1]) * c[2] - double(b[2]) * c[1]) -
               double(a[1]) * (double(b[0]) * c[2] - double(b[2]) * c[0]) +
               double(a[2]) * (double(b[0]) * c[1] - double(b[1]) * c[0])) /
              6.0;
  }
  return volume;
}

static void expect_history_complete(const Shape &shape, const OpHistory &history)
{
  ASSERT_EQ(int(history.faces.size()), shape.faces_num());
  for (size_t i = 0; i < history.faces.size(); i++) {
    EXPECT_NE(history.faces[i].origin, ElementOrigin::Unknown) << "face " << i;
    EXPECT_FALSE(history.faces[i].sources.empty()) << "face " << i;
  }
}

static bool has_source(const ElementHistory &h, const SourceKind kind, const int64_t index)
{
  for (const SourceRef &s : h.sources) {
    if (s.kind == kind && s.index == index) {
      return true;
    }
  }
  return false;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Tests
 * \{ */

TEST_P(KernelTest, Box)
{
  BoxParams params;
  params.min = {1.0, 2.0, 3.0};
  params.size = {10.0, 20.0, 30.0};
  const OpResult result = kernel().box(params);
  SKIP_IF_UNSUPPORTED(result);
  ASSERT_TRUE(result.ok()) << result.message;
  EXPECT_EQ(result.shape.faces_num(), 6);
  EXPECT_EQ(result.shape.edges_num(), 12);
  EXPECT_EQ(result.shape.vertices_num(), 8);
  EXPECT_NEAR(kernel().mass_properties(result.shape).volume, 6000.0, 1e-6);

  /* Each primitive face role appears exactly once. */
  std::set<int64_t> roles;
  for (const ElementHistory &h : result.history.faces) {
    ASSERT_EQ(h.sources.size(), 1);
    EXPECT_EQ(h.sources[0].kind, SourceKind::PrimitiveFace);
    roles.insert(h.sources[0].index);
  }
  EXPECT_EQ(roles.size(), 6);
}

TEST_P(KernelTest, ExtrudeRectangleWithHole)
{
  ExtrudeParams params;
  ProfileRegion region;
  region.outer = rectangle(1, {0.0, 0.0}, {40.0, 20.0}, true);
  /* Hole given clockwise on purpose: orientation must not matter. */
  ProfileLoop hole;
  hole.curves = {circle(10, {20.0, 10.0}, 5.0)};
  region.holes.push_back(hole);
  params.profile.regions.push_back(region);
  params.distance = 8.0;

  const OpResult result = kernel().extrude(params);
  SKIP_IF_UNSUPPORTED(result);
  ASSERT_TRUE(result.ok()) << result.message;
  EXPECT_EQ(result.shape.solids_num(), 1);
  const double expected = (40.0 * 20.0 - M_PI * 25.0) * 8.0;
  EXPECT_NEAR(kernel().mass_properties(result.shape).volume, expected, 1e-3);
  EXPECT_TRUE(kernel().is_valid(result.shape, nullptr));

  expect_history_complete(result.shape, result.history);
  /* Two caps, four sides named after the rectangle curves, the bore after the circle. */
  int caps = 0;
  std::set<int64_t> side_curves;
  for (const ElementHistory &h : result.history.faces) {
    for (const SourceRef &s : h.sources) {
      if (s.kind == SourceKind::Cap) {
        caps++;
      }
      if (s.kind == SourceKind::ProfileCurve) {
        side_curves.insert(s.index);
      }
    }
  }
  EXPECT_EQ(caps, 2);
  EXPECT_EQ(side_curves, (std::set<int64_t>{1, 2, 3, 4, 10}));

  /* End cap is the one at z = 8. */
  const Topology topo = kernel().topology(result.shape);
  for (size_t i = 0; i < result.history.faces.size(); i++) {
    if (has_source(result.history.faces[i], SourceKind::Cap, int64_t(CapRole::End))) {
      EXPECT_NEAR(topo.faces[i].sample_point[2], 8.0, 1e-6);
      EXPECT_NEAR(topo.faces[i].sample_normal[2], 1.0, 1e-6);
    }
  }
}

TEST_P(KernelTest, ExtrudeExtents)
{
  ExtrudeParams params;
  ProfileRegion region;
  region.outer = rectangle(1, {0.0, 0.0}, {10.0, 10.0}, true);
  params.profile.regions.push_back(region);

  params.distance = -5.0;
  OpResult result = kernel().extrude(params);
  SKIP_IF_UNSUPPORTED(result);
  ASSERT_TRUE(result.ok()) << result.message;
  EXPECT_NEAR(kernel().mass_properties(result.shape).volume, 500.0, 1e-6);
  EXPECT_NEAR(kernel().mass_properties(result.shape).center_of_mass[2], -2.5, 1e-6);

  params.extent = ExtentMode::Symmetric;
  params.distance = 6.0;
  result = kernel().extrude(params);
  SKIP_IF_UNSUPPORTED(result);
  ASSERT_TRUE(result.ok()) << result.message;
  EXPECT_NEAR(kernel().mass_properties(result.shape).center_of_mass[2], 0.0, 1e-6);

  params.extent = ExtentMode::TwoSides;
  params.distance = 4.0;
  params.distance2 = 2.0;
  result = kernel().extrude(params);
  SKIP_IF_UNSUPPORTED(result);
  ASSERT_TRUE(result.ok()) << result.message;
  EXPECT_NEAR(kernel().mass_properties(result.shape).volume, 600.0, 1e-6);
  EXPECT_NEAR(kernel().mass_properties(result.shape).center_of_mass[2], 1.0, 1e-6);
}

TEST_P(KernelTest, ExtrudeOnTiltedPlane)
{
  ExtrudeParams params;
  params.profile.plane.origin = {5.0, 0.0, 0.0};
  params.profile.plane.normal = {1.0, 0.0, 0.0};
  params.profile.plane.x_dir = {0.0, 1.0, 0.0};
  ProfileRegion region;
  region.outer.curves = {circle(1, {0.0, 0.0}, 2.0)};
  params.profile.regions.push_back(region);
  params.distance = 10.0;
  const OpResult result = kernel().extrude(params);
  SKIP_IF_UNSUPPORTED(result);
  ASSERT_TRUE(result.ok()) << result.message;
  const MassProperties mass = kernel().mass_properties(result.shape);
  EXPECT_NEAR(mass.volume, M_PI * 4.0 * 10.0, 1e-3);
  EXPECT_NEAR(mass.center_of_mass[0], 10.0, 1e-6);
}

TEST_P(KernelTest, OpenProfileIsRejected)
{
  ExtrudeParams params;
  ProfileRegion region;
  region.outer.curves = {line(1, {0.0, 0.0}, {10.0, 0.0}), line(2, {10.0, 0.0}, {10.0, 10.0})};
  params.profile.regions.push_back(region);
  const OpResult result = kernel().extrude(params);
  SKIP_IF_UNSUPPORTED(result);
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.status, Status::InvalidInput);
}

TEST_P(KernelTest, BooleanSubtractCylinder)
{
  BoxParams box;
  box.size = {20.0, 20.0, 10.0};
  const OpResult base = kernel().box(box);
  SKIP_IF_UNSUPPORTED(base);
  ASSERT_TRUE(base.ok());

  ExtrudeParams pin;
  ProfileRegion region;
  region.outer.curves = {circle(7, {10.0, 10.0}, 3.0)};
  pin.profile.regions.push_back(region);
  pin.profile.plane.origin = {0.0, 0.0, -1.0};
  pin.distance = 12.0;
  const OpResult tool = kernel().extrude(pin);
  SKIP_IF_UNSUPPORTED(tool);
  ASSERT_TRUE(tool.ok());

  const Shape tools[] = {tool.shape};
  const OpResult result = kernel().boolean(base.shape, tools, BooleanOp::Subtract);
  SKIP_IF_UNSUPPORTED(result);
  ASSERT_TRUE(result.ok()) << result.message;
  EXPECT_NEAR(kernel().mass_properties(result.shape).volume, 4000.0 - M_PI * 9.0 * 10.0, 1e-3);
  EXPECT_EQ(result.shape.faces_num(), 7);
  expect_history_complete(result.shape, result.history);

  /* The bore comes from the tool's side face (input 1). */
  int bore_faces = 0;
  for (const ElementHistory &h : result.history.faces) {
    for (const SourceRef &s : h.sources) {
      if (s.input == 1 && s.kind == SourceKind::Face) {
        bore_faces++;
      }
    }
  }
  EXPECT_GE(bore_faces, 1);
}

TEST_P(KernelTest, FilletBoxEdge)
{
  BoxParams box;
  box.size = {10.0, 10.0, 10.0};
  const OpResult base = kernel().box(box);
  SKIP_IF_UNSUPPORTED(base);
  ASSERT_TRUE(base.ok());
  const Topology topo = kernel().topology(base.shape);

  /* Pick one edge parallel to Z. */
  int edge = -1;
  for (size_t i = 0; i < topo.edges.size(); i++) {
    const EdgeInfo &e = topo.edges[i];
    if (std::abs(e.start[2] - e.end[2]) > 5.0) {
      edge = int(i);
      break;
    }
  }
  ASSERT_GE(edge, 0);

  FilletParams params;
  params.edges = {edge};
  params.radius = 2.0;
  const OpResult result = kernel().fillet(base.shape, params);
  SKIP_IF_UNSUPPORTED(result);
  ASSERT_TRUE(result.ok()) << result.message;
  const double expected = 1000.0 - (4.0 - M_PI) * 10.0; /* (r^2 - pi r^2 / 4) * h */
  EXPECT_NEAR(kernel().mass_properties(result.shape).volume, expected, 1e-3);
  EXPECT_EQ(result.shape.faces_num(), 7);
  expect_history_complete(result.shape, result.history);

  /* Exactly one face was generated from the filleted edge, and it is smooth to its sides. */
  int blend = -1;
  for (size_t i = 0; i < result.history.faces.size(); i++) {
    const ElementHistory &h = result.history.faces[i];
    if (h.origin == ElementOrigin::Generated && has_source(h, SourceKind::Edge, edge)) {
      EXPECT_EQ(blend, -1);
      blend = int(i);
    }
  }
  ASSERT_GE(blend, 0);
  const Topology filleted = kernel().topology(result.shape);
  EXPECT_EQ(filleted.faces[blend].type, SurfaceType::Cylinder);
  EXPECT_NEAR(filleted.faces[blend].radius, 2.0, 1e-9);
  int smooth_edges = 0;
  for (const EdgeInfo &e : filleted.edges) {
    smooth_edges += e.smooth ? 1 : 0;
  }
  EXPECT_EQ(smooth_edges, 2);
}

TEST_P(KernelTest, ChamferBoxEdge)
{
  BoxParams box;
  box.size = {10.0, 10.0, 10.0};
  const OpResult base = kernel().box(box);
  SKIP_IF_UNSUPPORTED(base);
  ASSERT_TRUE(base.ok());
  ChamferParams params;
  params.edges = {0};
  params.distance = 1.0;
  const OpResult result = kernel().chamfer(base.shape, params);
  SKIP_IF_UNSUPPORTED(result);
  ASSERT_TRUE(result.ok()) << result.message;
  EXPECT_NEAR(kernel().mass_properties(result.shape).volume, 1000.0 - 0.5 * 10.0, 1e-6);
  expect_history_complete(result.shape, result.history);
}

TEST_P(KernelTest, FilletTooLargeFails)
{
  BoxParams box;
  box.size = {10.0, 10.0, 10.0};
  const OpResult base = kernel().box(box);
  SKIP_IF_UNSUPPORTED(base);
  FilletParams params;
  params.edges = {0};
  params.radius = 50.0;
  const OpResult result = kernel().fillet(base.shape, params);
  SKIP_IF_UNSUPPORTED(result);
  EXPECT_FALSE(result.ok());
}

TEST_P(KernelTest, TessellationIsWeldedAndClosed)
{
  ExtrudeParams params;
  ProfileRegion region;
  region.outer = rectangle(1, {0.0, 0.0}, {30.0, 20.0}, true);
  ProfileLoop hole;
  hole.curves = {circle(9, {15.0, 10.0}, 4.0)};
  region.holes.push_back(hole);
  params.profile.regions.push_back(region);
  params.distance = 5.0;
  const OpResult solid = kernel().extrude(params);
  SKIP_IF_UNSUPPORTED(solid);
  ASSERT_TRUE(solid.ok());

  /* Fillet the outer top edges for curved faces next to planar ones. */
  const Topology topo = kernel().topology(solid.shape);
  FilletParams fillet;
  for (size_t i = 0; i < topo.edges.size(); i++) {
    const EdgeInfo &e = topo.edges[i];
    if (e.type == CurveType::Line && std::abs(e.start[2] - 5.0) < 1e-6 &&
        std::abs(e.end[2] - 5.0) < 1e-6)
    {
      fillet.edges.push_back(int(i));
    }
  }
  ASSERT_EQ(fillet.edges.size(), 4);
  fillet.radius = 1.5;
  const OpResult result = kernel().fillet(solid.shape, fillet);
  SKIP_IF_UNSUPPORTED(result);
  ASSERT_TRUE(result.ok()) << result.message;

  Tessellation mesh;
  TessellationParams tess;
  tess.linear_deflection = 0.02;
  ASSERT_EQ(kernel().tessellate(result.shape, tess, mesh), Status::Ok);
  EXPECT_FALSE(mesh.triangles.empty());
  EXPECT_EQ(mesh.triangle_faces.size(), mesh.triangles.size());
  EXPECT_EQ(mesh.corner_normals.size(), mesh.triangles.size() * 3);
  EXPECT_EQ(int(mesh.edge_polylines.size()), result.shape.edges_num());
  EXPECT_EQ(int(mesh.edge_smooth.size()), result.shape.edges_num());
  expect_closed_mesh(mesh);

  const double brep_volume = kernel().mass_properties(result.shape).volume;
  EXPECT_NEAR(mesh_volume(mesh), brep_volume, brep_volume * 0.01);

  /* Every face got triangles. */
  std::set<int> faces(mesh.triangle_faces.begin(), mesh.triangle_faces.end());
  EXPECT_EQ(int(faces.size()), result.shape.faces_num());
}

TEST_P(KernelTest, StepRoundTrip)
{
  if (!kernel().capabilities().step_export) {
    GTEST_SKIP() << "no STEP support in this backend";
  }
  BoxParams box;
  box.size = {12.0, 7.0, 3.0};
  const OpResult base = kernel().box(box);
  SKIP_IF_UNSUPPORTED(base);
  ASSERT_TRUE(base.ok());

  const std::filesystem::path path = std::filesystem::temp_directory_path() /
                                     ("cad_kernel_test_" + kernel_test_name({GetParam(), 0}) +
                                      ".step");
  const Shape shapes[] = {base.shape};
  const std::string names[] = {"Box"};
  std::string message;
  ASSERT_EQ(kernel().export_step(shapes, names, path.string(), &message), Status::Ok) << message;

  std::vector<Shape> imported;
  ASSERT_EQ(kernel().import_step(path.string(), imported, &message), Status::Ok) << message;
  ASSERT_EQ(imported.size(), 1);
  EXPECT_NEAR(kernel().mass_properties(imported[0]).volume, 252.0, 1e-6);
  std::filesystem::remove(path);
}

INSTANTIATE_TEST_SUITE_P(AllKernels,
                         KernelTest,
                         testing::ValuesIn(kernels_available()),
                         kernel_test_name);

/** \} */

}  // namespace cad::kernel::tests
