/* SPDX-FileCopyrightText: 2026 SimpleCAD Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edcad
 *
 * Sketch mode: the edit mode of CAD parts. One sketch feature of the part is edited at a time
 * (#CADPart.edit_sketch_uid); its points, curves and constraints are edited directly in the
 * original data and solved after every change. The model is rolled back to the sketch while it
 * is edited.
 */

#pragma once

#include <optional>
#include <string>
#include <utility>

#include "BLI_math_matrix_types.hh"
#include "BLI_span.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_vector.hh"

namespace blender {
struct wmKeyConfig;

struct ARegion;
struct bContext;
struct CADFeature;
struct CADPart;
struct CADSketch;
struct Object;
struct wmOperatorType;

namespace ed::cad {

/** The sketch being edited in the context (edit object is a CAD part in sketch mode). */
struct SketchEdit {
  Object *ob = nullptr;
  CADPart *part = nullptr;
  CADFeature *feature = nullptr;
  CADSketch *sketch = nullptr;
};
std::optional<SketchEdit> sketch_edit_get(const bContext *C);
bool sketch_edit_poll(bContext *C);
bool sketch_edit_poll_view3d(bContext *C);

/** Sketch plane frame: sketch (u, v, 0) to world space and back. */
struct SketchFrame {
  float4x4 to_world;
  float4x4 to_sketch;
  float3 origin_world;
  float3 normal_world;
};
SketchFrame sketch_frame(const Object *ob, const CADSketch *sketch);

/** Mouse position on the sketch plane, in sketch coordinates. */
std::optional<double2> sketch_mouse_co(const ARegion *region,
                                       const SketchFrame &frame,
                                       const float2 &mval);
/** Sketch coordinates to region pixels. */
float2 sketch_to_region(const ARegion *region, const SketchFrame &frame, const double2 &co);

/** Polyline of a curve in sketch coordinates (lines: 2 points). */
Vector<double2> sketch_curve_polyline(const CADSketch *sketch, int curve_index, int resolution);

enum class SketchHitType : int8_t {
  None,
  Point,
  Curve,
  Constraint,
  /** The sketch origin (points snap to it). */
  Origin,
};

struct SketchHit {
  SketchHitType type = SketchHitType::None;
  int uid = -1;
  /** Closest location on the hit element, sketch coordinates. */
  double2 co = {0.0, 0.0};
  float dist_px = 0.0f;
};

enum SketchPickFilter : uint8_t {
  PICK_POINTS = 1 << 0,
  PICK_CURVES = 1 << 1,
  PICK_CONSTRAINTS = 1 << 2,
  PICK_ORIGIN = 1 << 3,
  PICK_ALL = PICK_POINTS | PICK_CURVES | PICK_CONSTRAINTS | PICK_ORIGIN,
};

/** The element under the cursor: points first, then constraint labels, then curves. */
SketchHit sketch_pick(const ARegion *region,
                      const SketchFrame &frame,
                      const CADSketch *sketch,
                      const float2 &mval,
                      uint8_t filter = PICK_ALL,
                      int ignore_point = -1);

/** Where a constraint's glyph or dimension label is drawn, in region pixels. */
float2 sketch_constraint_label_position(const ARegion *region,
                                        const SketchFrame &frame,
                                        const CADSketch *sketch,
                                        int constraint_index);

/** Moves a constraint's label to `co` (sketch coordinates). */
void sketch_constraint_label_place(const ARegion *region,
                                   const SketchFrame &frame,
                                   CADSketch *sketch,
                                   int constraint_index,
                                   const double2 &co);

/** How a dimension is drawn, in region pixels. */
struct SketchDimensionGeometry {
  /** Extension and dimension lines (pairs of points). */
  Vector<float2> lines;
  /** Arrow tips and the unit direction they point in. */
  Vector<std::pair<float2, float2>> arrows;
};
SketchDimensionGeometry sketch_dimension_geometry(const ARegion *region,
                                                  const SketchFrame &frame,
                                                  const CADSketch *sketch,
                                                  int constraint_index);

/** Tool preview drawn in the viewport by the active sketch tool. */
struct SketchPreview {
  Vector<double2> lines;  /* Pairs of points. */
  Vector<double2> points;
  /** Inferred constraints shown next to the cursor ("H", "V", ...). */
  std::string hint;
  float2 hint_position;
};

/** Hover feedback, set by tools and the select operator. */
struct SketchHover {
  SketchHit hit;
  /** Optional tool preview. */
  SketchPreview preview;
};
SketchHover &sketch_hover();
void sketch_hover_clear();

/** Registers the viewport draw callbacks (once). */
void sketch_draw_init();
/** Registers the hover gizmo (once gizmo types can be registered). */
void sketch_gizmos_register();

/** Selection state of an element. */
bool sketch_element_selected(const CADSketch *sketch, SketchHitType type, int uid);
void sketch_element_select_set(CADSketch *sketch, SketchHitType type, int uid, bool select);

/**
 * Re-solves after an edit and tags the part. When `undo_on_conflict` uids are given and the
 * solve conflicts, those elements are removed again and false is returned.
 */
bool sketch_changed(bContext *C, const SketchEdit &edit, Span<int> remove_on_conflict = {});

/* Operators. */
void CAD_OT_sketch_add(wmOperatorType *ot);
void CAD_OT_sketch_edit(wmOperatorType *ot);
void CAD_OT_sketch_finish(wmOperatorType *ot);
void CAD_OT_sketch_select(wmOperatorType *ot);
void CAD_OT_sketch_select_all(wmOperatorType *ot);
void CAD_OT_sketch_drag(wmOperatorType *ot);
void CAD_OT_sketch_delete(wmOperatorType *ot);
void CAD_OT_sketch_line(wmOperatorType *ot);
void CAD_OT_sketch_rectangle(wmOperatorType *ot);
void CAD_OT_sketch_circle(wmOperatorType *ot);
void CAD_OT_sketch_arc(wmOperatorType *ot);
void CAD_OT_sketch_constrain(wmOperatorType *ot);
void CAD_OT_sketch_dimension(wmOperatorType *ot);
void CAD_OT_sketch_dimension_edit(wmOperatorType *ot);
void CAD_OT_sketch_construction_toggle(wmOperatorType *ot);

void sketch_keymap(wmKeyConfig *keyconf);

}  // namespace ed::cad
}  // namespace blender
