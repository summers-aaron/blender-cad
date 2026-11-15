/* SPDX-FileCopyrightText: 2026 SimpleCAD Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup cad
 *
 * Feature timeline evaluation.
 *
 * Every feature gets a fingerprint: a hash of its own resolved inputs and the previous
 * feature's fingerprint. Model states are cached by fingerprint in a process-wide cache, so
 * re-evaluating after an edit only recomputes from the first changed feature on, and undo,
 * redo, copy-on-write copies and setting a value back are cache hits.
 */

#pragma once

#include <memory>
#include <string>

#include "BLI_map.hh"
#include "BLI_vector.hh"

#include "CAD_document.hh"
#include "CAD_kernel.hh"

namespace blender::cad {

/** A solid body and the persistent names of its faces and edges. */
struct Body {
  std::string name;
  kernel::Shape shape;
  /** Persistent name per face / edge index of `shape`. */
  Vector<std::string> face_names;
  Vector<std::string> edge_names;
  /** Topology of `shape` (adjacency, sample points), computed once when the body is made. */
  std::shared_ptr<const kernel::Topology> topology;
  /** Uid of the feature that last changed this body. */
  int feature_uid = -1;

  int find_face(StringRef name) const;
  int find_edge(StringRef name) const;
};

/** The model after a feature: all bodies. Immutable and shared once built. */
struct ModelState {
  Vector<Body> bodies;
};

enum class FeatureStatus : int8_t {
  Ok = 0,
  /** Evaluated, but something needs attention (e.g. a reference was re-bound by geometry). */
  Warning,
  /** Failed: the model continues from the previous state. */
  Error,
  Suppressed,
  /** After the rollback marker: not evaluated. */
  RolledBack,
};

struct FeatureResult {
  int uid = -1;
  FeatureStatus status = FeatureStatus::Ok;
  std::string message;
  uint64_t fingerprint = 0;
  /** State after this feature (shared with the previous one when nothing changed). */
  std::shared_ptr<const ModelState> state;
  /** True when the state came from the cache rather than the kernel. */
  bool cached = false;
};

struct EvalResult {
  Vector<FeatureResult> features;
  std::shared_ptr<const ModelState> final_state;

  const FeatureResult *find(int uid) const;
  int errors_num() const;
};

/** Evaluates `doc` up to its rollback marker. Thread-safe; never throws. */
EvalResult evaluate(const Document &doc);

/** Drops every cached model state (e.g. on file load or under memory pressure). */
void eval_cache_clear();

/* -------------------------------------------------------------------- */
/** \name Profiles
 * \{ */

/** A closed region of a sketch: outer loop plus holes, ready for the kernel. */
struct SketchRegion {
  /** Smallest curve uid of the outer loop: stable identity across edits. */
  int64_t key = 0;
  kernel::ProfileRegion region;
  double area = 0.0;
};

struct SketchProfiles {
  Vector<SketchRegion> regions;
  /** Curves that are not part of any closed loop (open chains, branches). */
  Vector<int64_t> open_curves;
};

/** Finds closed loops among the non-construction curves and nests them into regions. */
SketchProfiles sketch_profiles(const SketchData &sketch);

/** \} */

}  // namespace blender::cad
