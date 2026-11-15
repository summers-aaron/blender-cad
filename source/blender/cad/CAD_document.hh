/* SPDX-FileCopyrightText: 2026 SimpleCAD Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup cad
 *
 * The CAD document as the core evaluates it: a plain C++ snapshot of a part's feature
 * timeline, built from DNA by blenkernel (with expressions, drivers and units already resolved
 * to numbers) and independent of DNA, RNA and the depsgraph.
 *
 * All lengths are millimeters, angles radians.
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>

#include "BLI_math_vector_types.hh"
#include "BLI_vector.hh"

#include "CAD_kernel.hh"

namespace blender::cad {

/** The geometry kernel interface (intern/cad_kernel). */
namespace kernel = ::cad::kernel;

/** Stored in files: never reorder or reuse values. */
enum class FeatureType : int8_t {
  Sketch = 0,
  Box = 1,
  Extrude = 2,
  Fillet = 3,
  Chamfer = 4,
};

const char *feature_type_name(FeatureType type);

/* -------------------------------------------------------------------- */
/** \name Sketch
 * \{ */

/** Stored in files: never reorder or reuse values. */
enum class SketchCurveType : int8_t {
  Line = 0,
  Arc = 1,
  Circle = 2,
};

/** Solved sketch geometry in plane coordinates. */
struct SketchCurve {
  /** Unique within the part; side faces of extrusions are named after it. */
  int64_t uid = 0;
  SketchCurveType type = SketchCurveType::Line;
  double2 start = {0.0, 0.0};
  double2 end = {0.0, 0.0};
  double2 center = {0.0, 0.0};
  double radius = 0.0;
  /** Arc direction from `start` to `end`. */
  bool ccw = true;
  /** Construction geometry never forms profiles. */
  bool construction = false;
};

struct SketchData {
  kernel::Plane plane;
  Vector<SketchCurve> curves;
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Features
 * \{ */

/**
 * A persistent reference to a face or edge of an earlier result: the persistent name, plus the
 * point that was picked, used to re-find the element geometrically if the name is gone.
 */
struct TopoRef {
  std::string name;
  double3 point = {0.0, 0.0, 0.0};
};

struct BoxData {
  double3 min = {0.0, 0.0, 0.0};
  double3 size = {10.0, 10.0, 10.0};
};

/** Stored in files: never reorder or reuse values. */
enum class BodyOperation : int8_t {
  NewBody = 0,
  Join = 1,
  Cut = 2,
  Intersect = 3,
};

struct ExtrudeData {
  /** The sketch feature providing the profile. */
  int sketch_uid = -1;
  /** Selected regions, each identified by the smallest curve uid of its outer loop (stable
   * under edits). Empty selects every region of the sketch. */
  Vector<int64_t> regions;
  kernel::ExtentMode extent = kernel::ExtentMode::OneSide;
  double distance = 10.0;
  double distance2 = 0.0;
  BodyOperation operation = BodyOperation::NewBody;
};

struct FilletData {
  Vector<TopoRef> edges;
  double radius = 1.0;
};

struct ChamferData {
  Vector<TopoRef> edges;
  double distance = 1.0;
};

using FeatureData = std::variant<SketchData, BoxData, ExtrudeData, FilletData, ChamferData>;

struct Feature {
  int uid = 0;
  std::string name;
  bool suppressed = false;
  FeatureData data;

  FeatureType type() const
  {
    return FeatureType(data.index());
  }
};

struct Document {
  kernel::KernelId kernel = kernel::KernelId::OCCT;
  Vector<Feature> features;
  /** Evaluate features up to and including this uid; -1 evaluates the whole timeline. */
  int rollback_uid = -1;

  const Feature *find_feature(int uid) const;
};

/** \} */

}  // namespace blender::cad
