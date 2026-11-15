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
struct CADFeature;
struct CADPart;
struct Depsgraph;
struct Object;
struct ScrArea;
struct View3D;
struct wmKeyConfig;
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

/** The part from the context: a pinned/context `cad_part`, else the active object's data. */
CADPart *context_part(const bContext *C);
bool part_editable_poll(bContext *C);
/** Tags the part for evaluation and redraw. */
void part_changed(bContext *C, CADPart *part);
/** The largest 3D view of the context's screen. */
ScrArea *largest_view3d(bContext *C);

/**
 * Defaults of a new feature that depend on the timeline: an Extrude uses the active sketch
 * (else the closest one before it) and joins an existing body.
 */
void feature_init_defaults(CADPart *part, CADFeature *feature, const CADFeature *prev_active);

/** After #BKE_cad_feature_add: with a rollback marker, move the feature to the marker and the
 * marker to the feature, so it is built on the model as it is shown. */
void feature_insert_at_rollback(CADPart *part, CADFeature *feature);

void CAD_OT_reference_toggle(wmOperatorType *ot);
void CAD_OT_references_pick(wmOperatorType *ot);

/* Feature dialogs (cad_dialog.cc). */
void CAD_OT_feature_new(wmOperatorType *ot);
void CAD_OT_feature_edit(wmOperatorType *ot);
void CAD_OT_dialog_ok(wmOperatorType *ot);
void CAD_OT_dialog_cancel(wmOperatorType *ot);
void dialog_gizmos_register();
/** "CAD Part": object mode keys of CAD parts (feature dialogs, new features). */
void part_keymap(wmKeyConfig *keyconf);

}  // namespace ed::cad
}  // namespace blender
