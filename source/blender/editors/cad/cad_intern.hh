/* SPDX-FileCopyrightText: 2026 SimpleCAD Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edcad
 */

#pragma once

#include <optional>
#include <string>

#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"

namespace blender {

struct ARegion;
struct bContext;
struct Depsgraph;
struct Object;
struct View3D;
struct wmOperatorType;

namespace ed::cad {

enum class PickType {
  Face,
  Edge,
};

/** A B-rep face or edge under the cursor. */
struct TopoPick {
  /** Body index in the evaluated result. */
  int body = -1;
  /** B-rep face hit by the ray (-1 when the ray missed). */
  int face = -1;
  /** B-rep edge closest to the cursor (edge picks only). */
  int edge = -1;
  /** Point on the face / edge in object space (Blender units). */
  float3 location = float3(0.0f);
  /** Persistent name of the picked element (set by the context variant). */
  std::string name;
};

std::optional<TopoPick> pick_topology(const ARegion *region,
                                      const View3D *v3d,
                                      Depsgraph *depsgraph,
                                      const Object *ob_eval,
                                      const float2 &mval,
                                      PickType type);
/** Picks on `ob` (original) in the context's 3D view and fills in the persistent name. */
std::optional<TopoPick> pick_topology(bContext *C,
                                      const Object *ob,
                                      const int2 &mval,
                                      PickType type);

/** Draws the named edges (selection color) and the hovered edge in a 3D view draw callback. */
void draw_edge_highlights(const ARegion *region,
                          const Object *ob_eval,
                          Span<std::string> selected_names,
                          const TopoPick *hover);

void CAD_OT_reference_toggle(wmOperatorType *ot);
void CAD_OT_references_pick(wmOperatorType *ot);

}  // namespace ed::cad
}  // namespace blender
