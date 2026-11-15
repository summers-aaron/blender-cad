/* SPDX-FileCopyrightText: 2026 SimpleCAD Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <numbers>

#include "testing/testing.h"

#include "CAD_eval.hh"

namespace blender::cad::tests {

class CadEvalTest : public testing::TestWithParam<kernel::KernelId> {
 protected:
  void SetUp() override
  {
    eval_cache_clear();
  }
};

static std::string kernel_name(const testing::TestParamInfo<kernel::KernelId> &info)
{
  return info.param == kernel::KernelId::OCCT ? "OCCT" : "SMLib";
}

static SketchCurve line(const int64_t uid, const double2 a, const double2 b)
{
  SketchCurve c;
  c.uid = uid;
  c.type = SketchCurveType::Line;
  c.start = a;
  c.end = b;
  return c;
}

/** Plate sketch (uid 1): 40 x 20 rectangle (curves 101..104). */
static Feature plate_sketch()
{
  SketchData sketch;
  sketch.curves.append(line(101, {0.0, 0.0}, {40.0, 0.0}));
  sketch.curves.append(line(102, {40.0, 0.0}, {40.0, 20.0}));
  sketch.curves.append(line(103, {40.0, 20.0}, {0.0, 20.0}));
  sketch.curves.append(line(104, {0.0, 20.0}, {0.0, 0.0}));
  return {1, "Sketch1", false, sketch};
}

static Feature plate_extrude(const double height)
{
  ExtrudeData data;
  data.sketch_uid = 1;
  data.distance = height;
  return {2, "Extrude1", false, data};
}

/** A doc whose fillet references the top edge along curve 101. */
static Document plate_doc(const kernel::KernelId kernel, const double height)
{
  Document doc;
  doc.kernel = kernel;
  doc.features.append(plate_sketch());
  doc.features.append(plate_extrude(height));
  FilletData fillet;
  fillet.radius = 2.0;
  fillet.edges.append({"F2:end|F2:side:101", {20.0, 0.0, height}});
  doc.features.append({3, "Fillet1", false, fillet});
  return doc;
}

static bool skip_unsupported(const EvalResult &result)
{
  for (const FeatureResult &fr : result.features) {
    if (fr.status == FeatureStatus::Error &&
        fr.message.find("not implemented") != std::string::npos)
    {
      return true;
    }
  }
  return false;
}

TEST_P(CadEvalTest, extrude_and_fillet)
{
  const Document doc = plate_doc(GetParam(), 10.0);
  const EvalResult result = evaluate(doc);
  if (skip_unsupported(result)) {
    GTEST_SKIP() << "operations not implemented in this kernel yet";
  }
  ASSERT_EQ(result.features.size(), 3);
  for (const FeatureResult &fr : result.features) {
    EXPECT_EQ(fr.status, FeatureStatus::Ok) << fr.uid << ": " << fr.message;
  }
  ASSERT_EQ(result.final_state->bodies.size(), 1);
  const Body &body = result.final_state->bodies[0];
  EXPECT_EQ(body.name, "Body1");
  EXPECT_GE(body.find_face("F2:start"), 0);
  EXPECT_GE(body.find_face("F2:end"), 0);
  EXPECT_GE(body.find_face("F2:side:102"), 0);
  EXPECT_GE(body.find_face("F3:blend(F2:end|F2:side:101)"), 0);
  const double volume = kernel::kernel_get(GetParam())->mass_properties(body.shape).volume;
  EXPECT_NEAR(volume, 40.0 * 20.0 * 10.0 - (4.0 - std::numbers::pi) * 40.0, 1e-3);
}

TEST_P(CadEvalTest, incremental_and_stable_names)
{
  const EvalResult first = evaluate(plate_doc(GetParam(), 10.0));
  if (skip_unsupported(first)) {
    GTEST_SKIP() << "operations not implemented in this kernel yet";
  }
  /* Same document again: everything comes from the cache. */
  const EvalResult again = evaluate(plate_doc(GetParam(), 10.0));
  for (const FeatureResult &fr : again.features) {
    EXPECT_TRUE(fr.cached || fr.uid == 1) << fr.uid;
  }
  EXPECT_EQ(again.final_state.get(), first.final_state.get());

  /* Upstream edit: the fillet's edge reference still resolves exactly by name. */
  const EvalResult taller = evaluate(plate_doc(GetParam(), 15.0));
  ASSERT_EQ(taller.errors_num(), 0);
  EXPECT_EQ(taller.find(3)->status, FeatureStatus::Ok) << taller.find(3)->message;
  EXPECT_FALSE(taller.find(2)->cached);
}

TEST_P(CadEvalTest, cut_extrude_and_errors)
{
  Document doc = plate_doc(GetParam(), 10.0);
  doc.features.remove(2); /* No fillet. */
  SketchData hole_sketch;
  hole_sketch.plane.origin = {0.0, 0.0, 10.0};
  SketchCurve c;
  c.uid = 201;
  c.type = SketchCurveType::Circle;
  c.center = {20.0, 10.0};
  c.radius = 3.0;
  hole_sketch.curves.append(c);
  doc.features.append({4, "Sketch2", false, hole_sketch});
  ExtrudeData cut;
  cut.sketch_uid = 4;
  cut.distance = -10.0;
  cut.operation = BodyOperation::Cut;
  doc.features.append({5, "Extrude2", false, cut});

  EvalResult result = evaluate(doc);
  if (skip_unsupported(result)) {
    GTEST_SKIP() << "operations not implemented in this kernel yet";
  }
  ASSERT_EQ(result.errors_num(), 0) << result.features.last().message;
  const Body &body = result.final_state->bodies[0];
  EXPECT_GE(body.find_face("F5:side:201"), 0);
  const double volume = kernel::kernel_get(GetParam())->mass_properties(body.shape).volume;
  EXPECT_NEAR(volume, 8000.0 - std::numbers::pi * 9.0 * 10.0, 1e-3);

  /* Rolling back before the cut. */
  doc.rollback_uid = 2;
  result = evaluate(doc);
  EXPECT_EQ(result.find(5)->status, FeatureStatus::RolledBack);
  EXPECT_NEAR(kernel::kernel_get(GetParam())
                  ->mass_properties(result.final_state->bodies[0].shape)
                  .volume,
              8000.0,
              1e-6);

  /* A failing feature leaves the model at the previous state. */
  doc.rollback_uid = -1;
  std::get<ExtrudeData>(doc.features[3].data).sketch_uid = 99;
  result = evaluate(doc);
  EXPECT_EQ(result.find(5)->status, FeatureStatus::Error);
  EXPECT_EQ(result.final_state.get(), result.find(4)->state.get());
}

INSTANTIATE_TEST_SUITE_P(AllKernels,
                         CadEvalTest,
                         testing::ValuesIn(kernel::kernels_available()),
                         kernel_name);

}  // namespace blender::cad::tests
