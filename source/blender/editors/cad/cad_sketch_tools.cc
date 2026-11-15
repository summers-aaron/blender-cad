/* SPDX-FileCopyrightText: 2026 SimpleCAD Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edcad
 *
 * Sketch mode tools: hover feedback, selection, dragging (solved), drawing lines, rectangles,
 * circles and arcs with snapping and inferred constraints, constraints on the selection and
 * dimensions.
 */

#include <fmt/format.h>
#include <numbers>

#include "DNA_cad_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_view3d_types.h"
#include "DNA_windowmanager_types.h"

#include "BLI_math_vector.hh"
#include "BLI_set.hh"

#include "BKE_cad.hh"
#include "BKE_context.hh"
#include "BKE_report.hh"
#include "BKE_unit.hh"

#include "BLT_translation.hh"

#include "DEG_depsgraph.hh"

#include "ED_screen.hh"
#include "ED_select_utils.hh"
#include "ED_view3d.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "cad_sketch.hh"

namespace blender::ed::cad {

/* -------------------------------------------------------------------- */
/** \name Helpers
 * \{ */

static double2 point_co(const CADSketch *sketch, const int uid)
{
  if (uid == CAD_SKETCH_ORIGIN) {
    return double2(0.0);
  }
  if (const CADSketchPoint *p = BKE_cad_sketch_point_find(sketch, uid)) {
    return double2(p->co[0], p->co[1]);
  }
  return double2(0.0);
}

static void tag_redraw(bContext *C)
{
  if (ARegion *region = CTX_wm_region(C)) {
    ED_region_tag_redraw(region);
  }
}

/** Snap target under the cursor while drawing. */
struct Snap {
  double2 co = {0.0, 0.0};
  /** Existing point (or #CAD_SKETCH_ORIGIN). */
  int point = -1;
  /** Curve the point lies on (point-on-curve). */
  int curve = -1;
};

static std::optional<Snap> snap_cursor(const ARegion *region,
                                       const SketchFrame &frame,
                                       const CADSketch *sketch,
                                       const float2 &mval,
                                       const int ignore_point = -1)
{
  const SketchHit hit = sketch_pick(
      region, frame, sketch, mval, PICK_POINTS | PICK_ORIGIN | PICK_CURVES, ignore_point);
  Snap snap;
  if (hit.type == SketchHitType::Point || hit.type == SketchHitType::Origin) {
    snap.point = hit.uid;
    snap.co = hit.co;
    sketch_hover().hit = hit;
    return snap;
  }
  const std::optional<double2> co = sketch_mouse_co(region, frame, mval);
  if (!co) {
    return std::nullopt;
  }
  snap.co = *co;
  if (hit.type == SketchHitType::Curve) {
    snap.curve = hit.uid;
    snap.co = hit.co;
  }
  sketch_hover().hit = hit;
  return snap;
}

/** A point for a snap: existing, or new (constrained to the origin / curve it snapped to).
 * New elements are added to `r_created`. */
static int snap_point(const SketchEdit &edit, const Snap &snap, Vector<int> &r_created)
{
  if (snap.point >= 0) {
    return snap.point;
  }
  const int uid = BKE_cad_sketch_point_add(edit.part, edit.sketch, snap.co);
  r_created.append(uid);
  if (snap.point == CAD_SKETCH_ORIGIN) {
    const int entities[2] = {uid, CAD_SKETCH_ORIGIN};
    r_created.append(BKE_cad_sketch_constraint_add(
        edit.part, edit.sketch, CAD_CONSTRAINT_COINCIDENT, entities, 0.0));
  }
  else if (snap.curve >= 0) {
    const int entities[2] = {uid, snap.curve};
    r_created.append(BKE_cad_sketch_constraint_add(
        edit.part, edit.sketch, CAD_CONSTRAINT_POINT_ON_CURVE, entities, 0.0));
  }
  return uid;
}

/** Horizontal / vertical inference for a line from `a` to `b` (on screen). */
enum class Align { None, Horizontal, Vertical };
static Align infer_alignment(const ARegion *region,
                             const SketchFrame &frame,
                             const double2 &a,
                             double2 &b)
{
  const double2 d = b - a;
  const float len_px = math::distance(sketch_to_region(region, frame, a),
                                      sketch_to_region(region, frame, b));
  if (len_px < 8.0f * UI_SCALE_FAC) {
    return Align::None;
  }
  const double angle = std::atan2(std::abs(d.y), std::abs(d.x));
  const double tolerance = 3.0 * std::numbers::pi / 180.0;
  if (angle < tolerance) {
    b.y = a.y;
    return Align::Horizontal;
  }
  if (angle > std::numbers::pi / 2.0 - tolerance) {
    b.x = a.x;
    return Align::Vertical;
  }
  return Align::None;
}

static void preview_clear()
{
  sketch_hover().preview = SketchPreview();
}

static void preview_line(const double2 &a, const double2 &b)
{
  sketch_hover().preview.lines.append(a);
  sketch_hover().preview.lines.append(b);
}

static void preview_hint(const ARegion *region,
                         const SketchFrame &frame,
                         const double2 &at,
                         std::string hint)
{
  SketchPreview &preview = sketch_hover().preview;
  preview.hint = std::move(hint);
  preview.hint_position = sketch_to_region(region, frame, at) +
                          float2(18.0f, 18.0f) * UI_SCALE_FAC;
}

static std::string length_hint(const bContext *C, const double length)
{
  char buf[64];
  BKE_unit_value_as_string_scaled(
      buf, length, 3, B_UNIT_LENGTH, CTX_data_scene(C)->unit, false, true);
  return buf;
}

static bool is_cancel_event(const wmEvent *event)
{
  return event->val == KM_PRESS && ELEM(event->type, EVT_ESCKEY, RIGHTMOUSE);
}

static bool is_confirm_event(const wmEvent *event)
{
  return event->val == KM_PRESS && ELEM(event->type, EVT_RETKEY, EVT_PADENTER);
}

static bool is_navigation_event(const wmEvent *event)
{
  return ISMOUSE_WHEEL(event->type) || ISNDOF(event->type) ||
         ELEM(event->type, MIDDLEMOUSE, MOUSEPAN, MOUSEZOOM, MOUSEROTATE, MOUSESMARTZOOM) ||
         (event->type >= EVT_PAD0 && event->type <= EVT_PADPERIOD);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Hover Gizmo
 *
 * Never "highlighted", so clicks pass on to the keymap; it only feeds hover feedback.
 * \{ */

static int hover_gizmo_test_select(bContext *C, wmGizmo * /*gz*/, const int mval[2])
{
  const std::optional<SketchEdit> edit = sketch_edit_get(C);
  ARegion *region = CTX_wm_region(C);
  if (!edit || region == nullptr) {
    return -1;
  }
  const SketchFrame frame = sketch_frame(edit->ob, edit->sketch);
  const SketchHit hit = sketch_pick(region, frame, edit->sketch, float2(mval[0], mval[1]));
  SketchHover &hover = sketch_hover();
  if (hit.type != hover.hit.type || hit.uid != hover.hit.uid) {
    hover.hit = hit;
    ED_region_tag_redraw(region);
  }
  return -1;
}

static void hover_gizmo_draw(const bContext * /*C*/, wmGizmo * /*gz*/) {}

static void hover_gizmo_setup(wmGizmo *gz)
{
  gz->flag |= WM_GIZMO_HIDDEN_KEYMAP;
}

static void GIZMO_GT_cad_sketch_hover(wmGizmoType *gzt)
{
  gzt->idname = "GIZMO_GT_cad_sketch_hover";
  gzt->draw = hover_gizmo_draw;
  gzt->test_select = hover_gizmo_test_select;
  gzt->setup = hover_gizmo_setup;
  gzt->struct_size = sizeof(wmGizmo);
}

static bool hover_group_poll(const bContext *C, wmGizmoGroupType * /*gzgt*/)
{
  return sketch_edit_get(C).has_value();
}

static void hover_group_setup(const bContext * /*C*/, wmGizmoGroup *gzgroup)
{
  const wmGizmoType *gzt = WM_gizmotype_find("GIZMO_GT_cad_sketch_hover", true);
  WM_gizmo_new_ptr(gzt, gzgroup, nullptr);
}

static void VIEW3D_GGT_cad_sketch_hover(wmGizmoGroupType *gzgt)
{
  gzgt->name = "CAD Sketch Hover";
  gzgt->idname = "VIEW3D_GGT_cad_sketch_hover";
  gzgt->flag = WM_GIZMOGROUPTYPE_3D;
  gzgt->gzmap_params.spaceid = SPACE_VIEW3D;
  gzgt->gzmap_params.regionid = RGN_TYPE_WINDOW;
  gzgt->poll = hover_group_poll;
  gzgt->setup = hover_group_setup;
}

void sketch_gizmos_register()
{
  WM_gizmotype_append(GIZMO_GT_cad_sketch_hover);
  wmGizmoMapType_Params params{SPACE_VIEW3D, RGN_TYPE_WINDOW};
  wmGizmoMapType *gzmap_type = WM_gizmomaptype_ensure(&params);
  WM_gizmogrouptype_append_and_link(gzmap_type, VIEW3D_GGT_cad_sketch_hover);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select
 * \{ */

static wmOperatorStatus sketch_select_exec(bContext *C, wmOperator *op)
{
  const std::optional<SketchEdit> edit = sketch_edit_get(C);
  const ARegion *region = CTX_wm_region(C);
  int mval[2];
  RNA_int_get_array(op->ptr, "location", mval);
  const SketchFrame frame = sketch_frame(edit->ob, edit->sketch);
  const SketchHit hit = sketch_pick(
      region, frame, edit->sketch, float2(mval[0], mval[1]), PICK_POINTS | PICK_CURVES | PICK_CONSTRAINTS);
  const bool extend = RNA_boolean_get(op->ptr, "extend");

  if (hit.type == SketchHitType::None) {
    if (!RNA_boolean_get(op->ptr, "deselect_all")) {
      return OPERATOR_CANCELLED | OPERATOR_PASS_THROUGH;
    }
    if (!BKE_cad_sketch_select_all(edit->sketch, false)) {
      return OPERATOR_CANCELLED | OPERATOR_PASS_THROUGH;
    }
  }
  else if (extend) {
    const bool selected = sketch_element_selected(edit->sketch, hit.type, hit.uid);
    sketch_element_select_set(edit->sketch, hit.type, hit.uid, !selected);
  }
  else {
    BKE_cad_sketch_select_all(edit->sketch, false);
    sketch_element_select_set(edit->sketch, hit.type, hit.uid, true);
  }
  WM_event_add_notifier(C, NC_GEOM | ND_SELECT, edit->part);
  tag_redraw(C);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus sketch_select_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  RNA_int_set_array(op->ptr, "location", event->mval);
  return sketch_select_exec(C, op);
}

void CAD_OT_sketch_select(wmOperatorType *ot)
{
  ot->name = "Select Sketch Element";
  ot->description = "Select a point, curve or constraint of the sketch";
  ot->idname = "CAD_OT_sketch_select";

  ot->invoke = sketch_select_invoke;
  ot->exec = sketch_select_exec;
  ot->poll = sketch_edit_poll_view3d;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_boolean(ot->srna, "extend", false, "Extend", "Toggle the element in the selection");
  RNA_def_boolean(
      ot->srna, "deselect_all", false, "Deselect All", "Deselect everything when nothing is hit");
  PropertyRNA *prop = RNA_def_int_array(
      ot->srna, "location", 2, nullptr, INT_MIN, INT_MAX, "Location", "", INT_MIN, INT_MAX);
  RNA_def_property_flag(prop, PROP_HIDDEN);
}

static wmOperatorStatus sketch_select_all_exec(bContext *C, wmOperator *op)
{
  const std::optional<SketchEdit> edit = sketch_edit_get(C);
  const int action = RNA_enum_get(op->ptr, "action");
  bool select = action == SEL_SELECT;
  if (action == SEL_TOGGLE) {
    select = true;
    for (int i = 0; i < edit->sketch->points_num; i++) {
      if (edit->sketch->points[i].flag & CAD_SKETCH_SELECT) {
        select = false;
      }
    }
    for (int i = 0; i < edit->sketch->curves_num; i++) {
      if (edit->sketch->curves[i].flag & CAD_SKETCH_SELECT) {
        select = false;
      }
    }
  }
  BKE_cad_sketch_select_all(edit->sketch, select);
  WM_event_add_notifier(C, NC_GEOM | ND_SELECT, edit->part);
  tag_redraw(C);
  return OPERATOR_FINISHED;
}

void CAD_OT_sketch_select_all(wmOperatorType *ot)
{
  ot->name = "(De)select All";
  ot->description = "Select or deselect all sketch elements";
  ot->idname = "CAD_OT_sketch_select_all";
  ot->exec = sketch_select_all_exec;
  ot->poll = sketch_edit_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
  WM_operator_properties_select_all(ot);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Delete
 * \{ */

static wmOperatorStatus sketch_delete_exec(bContext *C, wmOperator * /*op*/)
{
  const std::optional<SketchEdit> edit = sketch_edit_get(C);
  if (!BKE_cad_sketch_remove_selected(edit->sketch)) {
    return OPERATOR_CANCELLED;
  }
  sketch_hover_clear();
  sketch_changed(C, *edit);
  return OPERATOR_FINISHED;
}

void CAD_OT_sketch_delete(wmOperatorType *ot)
{
  ot->name = "Delete";
  ot->description = "Delete the selected sketch geometry and constraints";
  ot->idname = "CAD_OT_sketch_delete";
  ot->exec = sketch_delete_exec;
  ot->poll = sketch_edit_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Drag
 * \{ */

struct DragData {
  SketchEdit edit;
  SketchHit target;
  double2 start_co;
  /** Points pulled along and their start positions. */
  Vector<std::pair<int, double2>> points;
  /** Undo state: all point positions and radii before the drag. */
  Vector<double2> orig_points;
  Vector<double> orig_radii;
  float2 orig_label_offset;
};

static void drag_restore(DragData &data)
{
  CADSketch *sketch = data.edit.sketch;
  for (int i = 0; i < sketch->points_num && i < data.orig_points.size(); i++) {
    sketch->points[i].co[0] = data.orig_points[i].x;
    sketch->points[i].co[1] = data.orig_points[i].y;
  }
  for (int i = 0; i < sketch->curves_num && i < data.orig_radii.size(); i++) {
    sketch->curves[i].radius = data.orig_radii[i];
  }
  if (data.target.type == SketchHitType::Constraint) {
    if (CADSketchConstraint *c = BKE_cad_sketch_constraint_find(sketch, data.target.uid)) {
      c->label_offset[0] = data.orig_label_offset.x;
      c->label_offset[1] = data.orig_label_offset.y;
    }
  }
}

static wmOperatorStatus sketch_drag_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  const std::optional<SketchEdit> edit = sketch_edit_get(C);
  const ARegion *region = CTX_wm_region(C);
  const SketchFrame frame = sketch_frame(edit->ob, edit->sketch);
  /* Pick where the drag started, not where the cursor is now. */
  const float2 press(float(event->prev_press_xy[0] - region->winrct.xmin),
                     float(event->prev_press_xy[1] - region->winrct.ymin));
  const SketchHit hit = sketch_pick(
      region, frame, edit->sketch, press, PICK_POINTS | PICK_CURVES | PICK_CONSTRAINTS);
  const std::optional<double2> start = sketch_mouse_co(region, frame, press);
  if (hit.type == SketchHitType::None || !start) {
    return OPERATOR_CANCELLED | OPERATOR_PASS_THROUGH;
  }

  DragData *data = MEM_new<DragData>(__func__);
  data->edit = *edit;
  data->target = hit;
  data->start_co = *start;
  CADSketch *sketch = edit->sketch;
  for (int i = 0; i < sketch->points_num; i++) {
    data->orig_points.append(double2(sketch->points[i].co[0], sketch->points[i].co[1]));
  }
  for (int i = 0; i < sketch->curves_num; i++) {
    data->orig_radii.append(sketch->curves[i].radius);
  }
  if (hit.type == SketchHitType::Point) {
    data->points.append({hit.uid, point_co(sketch, hit.uid)});
  }
  else if (hit.type == SketchHitType::Curve) {
    const CADSketchCurve *curve = BKE_cad_sketch_curve_find(sketch, hit.uid);
    if (curve->type == CAD_SKETCH_LINE) {
      for (const int p : {curve->points[0], curve->points[1]}) {
        data->points.append({p, point_co(sketch, p)});
      }
    }
    else {
      /* Dragging a circle / arc rim changes its size: pull the closest rim point. For circles
       * there is no rim point, so the radius follows the cursor directly. */
      data->points.append({curve->points[2], point_co(sketch, curve->points[2])});
      data->points.clear();
    }
  }
  else {
    CADSketchConstraint *c = BKE_cad_sketch_constraint_find(sketch, hit.uid);
    const float2 orig_offset(c->label_offset[0], c->label_offset[1]);
    if (orig_offset == float2(0.0f)) {
      /* Not placed yet: pin the label where it is drawn, then move it from there. */
      const int index = int(c - sketch->constraints);
      const float2 label = sketch_constraint_label_position(region, frame, sketch, index);
      if (const std::optional<double2> co = sketch_mouse_co(region, frame, label)) {
        sketch_constraint_label_place(region, frame, sketch, index, *co);
      }
    }
    data->orig_label_offset = float2(c->label_offset[0], c->label_offset[1]);
  }
  op->customdata = data;
  WM_event_add_modal_handler(C, op);
  return OPERATOR_RUNNING_MODAL;
}

static void drag_update(bContext *C, DragData &data, const double2 &co)
{
  CADSketch *sketch = data.edit.sketch;
  const double2 delta = co - data.start_co;
  if (data.target.type == SketchHitType::Constraint) {
    if (CADSketchConstraint *c = BKE_cad_sketch_constraint_find(sketch, data.target.uid)) {
      c->label_offset[0] = data.orig_label_offset.x + float(delta.x);
      c->label_offset[1] = data.orig_label_offset.y + float(delta.y);
    }
    tag_redraw(C);
    return;
  }
  if (data.target.type == SketchHitType::Curve && data.points.is_empty()) {
    CADSketchCurve *curve = BKE_cad_sketch_curve_find(sketch, data.target.uid);
    if (curve && curve->type == CAD_SKETCH_CIRCLE) {
      /* Resize: solve with the radius as the drag result. Radius-dimensioned circles snap
       * back in the solve below. */
      curve->radius = math::distance(co, point_co(sketch, curve->points[2]));
    }
    BKE_cad_sketch_solve(data.edit.part, sketch);
  }
  else {
    Vector<CADSketchDrag> targets;
    for (const auto &[uid, start] : data.points) {
      targets.append({uid, start + delta});
    }
    if (!BKE_cad_sketch_solve(data.edit.part, sketch, targets)) {
      /* Fully constrained or conflicting: nothing may move. */
    }
    if (sketch->constraints_num == 0) {
      /* Nothing to solve: move the points directly. */
      for (const CADSketchDrag &t : targets) {
        if (CADSketchPoint *p = BKE_cad_sketch_point_find(sketch, t.point)) {
          p->co[0] = t.co.x;
          p->co[1] = t.co.y;
        }
      }
    }
  }
  DEG_id_tag_update(&data.edit.part->id, ID_RECALC_GEOMETRY);
  tag_redraw(C);
}

static wmOperatorStatus sketch_drag_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  DragData *data = static_cast<DragData *>(op->customdata);
  const ARegion *region = CTX_wm_region(C);
  const SketchFrame frame = sketch_frame(data->edit.ob, data->edit.sketch);

  if (event->type == MOUSEMOVE) {
    if (const std::optional<double2> co = sketch_mouse_co(region, frame, float2(event->mval))) {
      drag_update(C, *data, *co);
    }
    return OPERATOR_RUNNING_MODAL;
  }
  if (ELEM(event->type, LEFTMOUSE) && event->val == KM_RELEASE) {
    sketch_changed(C, data->edit);
    MEM_delete(data);
    return OPERATOR_FINISHED;
  }
  if (is_cancel_event(event)) {
    drag_restore(*data);
    sketch_changed(C, data->edit);
    MEM_delete(data);
    return OPERATOR_CANCELLED;
  }
  return OPERATOR_RUNNING_MODAL;
}

static void sketch_drag_cancel(bContext *C, wmOperator *op)
{
  DragData *data = static_cast<DragData *>(op->customdata);
  drag_restore(*data);
  sketch_changed(C, data->edit);
  MEM_delete(data);
}

void CAD_OT_sketch_drag(wmOperatorType *ot)
{
  ot->name = "Drag";
  ot->description =
      "Drag a point, line or dimension label; the sketch's constraints keep holding";
  ot->idname = "CAD_OT_sketch_drag";
  ot->invoke = sketch_drag_invoke;
  ot->modal = sketch_drag_modal;
  ot->cancel = sketch_drag_cancel;
  ot->poll = sketch_edit_poll_view3d;
  ot->flag = OPTYPE_UNDO | OPTYPE_BLOCKING;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Drawing Tools
 *
 * One modal operator per shape. Each click places the next point; points snap to existing
 * points, the origin and curves (adding coincident / point-on-curve constraints) and lines snap
 * horizontal / vertical (adding those constraints).
 * \{ */

enum class Shape { Line, Rectangle, Circle, Arc };

struct DrawData {
  SketchEdit edit;
  Shape shape;
  /** Placed points (uids) of the current shape. */
  Vector<int> placed;
  Vector<double2> placed_co;
  /** Everything created by this operator run (for cleanup). */
  Vector<int> created;
  /** Line chains: first point, to close loops. */
  int chain_first = -1;
  int shapes_made = 0;
  /** Arc: accumulated sweep from the start point, for the direction. */
  double arc_sweep = 0.0;
  double arc_last_angle = 0.0;
  Snap cursor;
  Align align = Align::None;
};

static void draw_preview(bContext *C, DrawData &data, const float2 &mval)
{
  const ARegion *region = CTX_wm_region(C);
  const CADSketch *sketch = data.edit.sketch;
  const SketchFrame frame = sketch_frame(data.edit.ob, sketch);
  preview_clear();
  const int ignore = data.placed.is_empty() ? -1 : data.placed.last();
  std::optional<Snap> snap = snap_cursor(region, frame, sketch, mval, ignore);
  if (!snap) {
    return;
  }
  data.cursor = *snap;
  data.align = Align::None;
  SketchPreview &preview = sketch_hover().preview;

  if (data.placed.is_empty()) {
    preview.points.append(snap->co);
    tag_redraw(C);
    return;
  }
  const double2 a = data.placed_co[0];
  switch (data.shape) {
    case Shape::Line: {
      const double2 start = data.placed_co.last();
      double2 end = data.cursor.co;
      if (data.cursor.point < 0) {
        data.align = infer_alignment(region, frame, start, end);
        data.cursor.co = end;
        if (data.align != Align::None) {
          data.cursor.curve = -1;
        }
      }
      preview_line(start, end);
      preview.points.append(end);
      std::string hint = length_hint(C, math::distance(start, end));
      if (data.align == Align::Horizontal) {
        hint += "  H";
      }
      else if (data.align == Align::Vertical) {
        hint += "  V";
      }
      preview_hint(region, frame, end, hint);
      break;
    }
    case Shape::Rectangle: {
      const double2 c = data.cursor.co;
      const double2 b(c.x, a.y), d(a.x, c.y);
      preview_line(a, b);
      preview_line(b, c);
      preview_line(c, d);
      preview_line(d, a);
      preview_hint(region,
                   frame,
                   c,
                   length_hint(C, std::abs(c.x - a.x)) + " x " +
                       length_hint(C, std::abs(c.y - a.y)));
      break;
    }
    case Shape::Circle: {
      const double r = math::distance(a, data.cursor.co);
      for (int i = 0; i < 64; i++) {
        const double t0 = 2.0 * std::numbers::pi * i / 64, t1 = 2.0 * std::numbers::pi * (i + 1) / 64;
        preview_line(a + double2(std::cos(t0), std::sin(t0)) * r,
                     a + double2(std::cos(t1), std::sin(t1)) * r);
      }
      preview_line(a, data.cursor.co);
      preview_hint(region, frame, data.cursor.co, "R " + length_hint(C, r));
      break;
    }
    case Shape::Arc: {
      if (data.placed.size() == 1) {
        /* Picking the start point: show the radius. */
        preview_line(a, data.cursor.co);
        preview_hint(region, frame, data.cursor.co, "R " + length_hint(C, math::distance(a, data.cursor.co)));
        break;
      }
      const double2 start = data.placed_co[1];
      const double r = math::distance(a, start);
      const double a0 = std::atan2(start.y - a.y, start.x - a.x);
      const double angle = std::atan2(data.cursor.co.y - a.y, data.cursor.co.x - a.x);
      double step = angle - data.arc_last_angle;
      while (step > std::numbers::pi) {
        step -= 2.0 * std::numbers::pi;
      }
      while (step < -std::numbers::pi) {
        step += 2.0 * std::numbers::pi;
      }
      data.arc_sweep = std::clamp(data.arc_sweep + step, -2.0 * std::numbers::pi + 1e-3, 2.0 * std::numbers::pi - 1e-3);
      data.arc_last_angle = angle;
      const int steps = 48;
      for (int i = 0; i < steps; i++) {
        const double t0 = a0 + data.arc_sweep * i / steps, t1 = a0 + data.arc_sweep * (i + 1) / steps;
        preview_line(a + double2(std::cos(t0), std::sin(t0)) * r,
                     a + double2(std::cos(t1), std::sin(t1)) * r);
      }
      if (data.cursor.point < 0) {
        const double end_angle = a0 + data.arc_sweep;
        data.cursor.co = a + double2(std::cos(end_angle), std::sin(end_angle)) * r;
        data.cursor.curve = -1;
      }
      preview.points.append(data.cursor.co);
      preview_hint(region,
                   frame,
                   data.cursor.co,
                   fmt::format("{:.1f}\xc2\xb0", std::abs(data.arc_sweep) * 180.0 / std::numbers::pi));
      break;
    }
  }
  tag_redraw(C);
}

static void draw_place_point(DrawData &data)
{
  const int uid = snap_point(data.edit, data.cursor, data.created);
  data.placed.append(uid);
  data.placed_co.append(data.cursor.co);
}

/** Finishes the current shape. Returns true when the operator is done (loop closed). */
static bool draw_commit_shape(bContext *C, DrawData &data)
{
  const SketchEdit &edit = data.edit;
  CADPart *part = edit.part;
  CADSketch *sketch = edit.sketch;
  Vector<int> inferred;
  bool done = false;

  switch (data.shape) {
    case Shape::Line: {
      const int start = data.placed[data.placed.size() - 2];
      const int end = data.placed.last();
      if (start == end) {
        data.placed.pop_last();
        data.placed_co.pop_last();
        return false;
      }
      const int line = BKE_cad_sketch_line_add(part, sketch, start, end);
      data.created.append(line);
      if (data.align != Align::None) {
        const int entities[1] = {line};
        inferred.append(BKE_cad_sketch_constraint_add(
            part,
            sketch,
            data.align == Align::Horizontal ? CAD_CONSTRAINT_HORIZONTAL : CAD_CONSTRAINT_VERTICAL,
            entities,
            0.0));
      }
      /* Closing the chain ends the tool, like in other CAD tools. */
      done = end == data.chain_first;
      /* Keep only the last point: the next line starts there. */
      data.placed = {end};
      data.placed_co = {data.placed_co.last()};
      break;
    }
    case Shape::Rectangle: {
      const double2 a = data.placed_co[0], c = data.placed_co[1];
      if (a.x == c.x || a.y == c.y) {
        return false;
      }
      const int pa = data.placed[0], pc = data.placed[1];
      const int pb = BKE_cad_sketch_point_add(part, sketch, double2(c.x, a.y));
      const int pd = BKE_cad_sketch_point_add(part, sketch, double2(a.x, c.y));
      data.created.extend({pb, pd});
      const int lines[4] = {BKE_cad_sketch_line_add(part, sketch, pa, pb),
                            BKE_cad_sketch_line_add(part, sketch, pb, pc),
                            BKE_cad_sketch_line_add(part, sketch, pc, pd),
                            BKE_cad_sketch_line_add(part, sketch, pd, pa)};
      data.created.extend(lines);
      for (int i = 0; i < 4; i++) {
        const int entities[1] = {lines[i]};
        inferred.append(BKE_cad_sketch_constraint_add(
            part, sketch, i % 2 == 0 ? CAD_CONSTRAINT_HORIZONTAL : CAD_CONSTRAINT_VERTICAL,
            entities, 0.0));
      }
      data.placed.clear();
      data.placed_co.clear();
      break;
    }
    case Shape::Circle: {
      const double r = math::distance(data.placed_co[0], data.placed_co[1]);
      /* The second click only gives the radius. */
      const int rim = data.placed[1];
      if (r <= 0.0) {
        return false;
      }
      data.created.append(BKE_cad_sketch_circle_add(part, sketch, data.placed[0], r));
      if (std::find(data.created.begin(), data.created.end(), rim) != data.created.end() &&
          !BKE_cad_sketch_point_is_used(sketch, rim))
      {
        BKE_cad_sketch_remove(sketch, {rim});
      }
      data.placed.clear();
      data.placed_co.clear();
      break;
    }
    case Shape::Arc: {
      const int arc = BKE_cad_sketch_arc_add(
          part, sketch, data.placed[0], data.placed[1], data.placed[2], data.arc_sweep > 0.0);
      data.created.append(arc);
      data.placed.clear();
      data.placed_co.clear();
      data.arc_sweep = 0.0;
      break;
    }
  }
  data.shapes_made++;
  /* Inferred constraints that would over-constrain are dropped again. */
  sketch_changed(C, edit, inferred);
  return done;
}

static int shape_points_needed(const Shape shape)
{
  switch (shape) {
    case Shape::Line:
      return 2;
    case Shape::Rectangle:
    case Shape::Circle:
      return 2;
    case Shape::Arc:
      return 3;
  }
  return 2;
}

static void draw_status(bContext *C, const Shape shape)
{
  WorkspaceStatus status(C);
  status.item(shape == Shape::Line ? IFACE_("Next Point") : IFACE_("Place Point"), ICON_MOUSE_LMB);
  status.item(IFACE_("Done"), ICON_EVENT_ESC, ICON_MOUSE_RMB);
}

static wmOperatorStatus draw_finish(bContext *C, wmOperator *op, const bool cancel)
{
  DrawData *data = static_cast<DrawData *>(op->customdata);
  /* Points placed for an unfinished shape go away. */
  Vector<int> unused;
  for (const int uid : data->placed) {
    if (std::find(data->created.begin(), data->created.end(), uid) != data->created.end() &&
        !BKE_cad_sketch_point_is_used(data->edit.sketch, uid))
    {
      unused.append(uid);
    }
  }
  if (!unused.is_empty()) {
    BKE_cad_sketch_remove(data->edit.sketch, unused);
  }
  const bool made = data->shapes_made > 0;
  preview_clear();
  sketch_changed(C, data->edit);
  ED_workspace_status_text(C, nullptr);
  MEM_delete(data);
  op->customdata = nullptr;
  return (made && !cancel) || made ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
}

static wmOperatorStatus draw_invoke(bContext *C, wmOperator *op, const wmEvent *event, Shape shape)
{
  const std::optional<SketchEdit> edit = sketch_edit_get(C);
  DrawData *data = MEM_new<DrawData>(__func__);
  data->edit = *edit;
  data->shape = shape;
  op->customdata = data;
  draw_preview(C, *data, float2(event->mval));
  /* Started by a click (tool keymap): that click places the first point. */
  if (event->type == LEFTMOUSE && event->val == KM_PRESS) {
    draw_place_point(*data);
    data->chain_first = data->placed.last();
    if (shape == Shape::Arc) {
      data->arc_last_angle = 0.0;
    }
  }
  draw_status(C, shape);
  WM_event_add_modal_handler(C, op);
  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus draw_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  DrawData *data = static_cast<DrawData *>(op->customdata);
  if (event->type == MOUSEMOVE) {
    draw_preview(C, *data, float2(event->mval));
    return OPERATOR_RUNNING_MODAL;
  }
  if (event->type == LEFTMOUSE && event->val == KM_PRESS) {
    draw_preview(C, *data, float2(event->mval));
    draw_place_point(*data);
    if (data->placed.size() == 1) {
      if (data->chain_first < 0 || data->shape != Shape::Line) {
        data->chain_first = data->placed.last();
      }
    }
    if (data->shape == Shape::Arc && data->placed.size() == 2) {
      const double2 c = data->placed_co[0], s = data->placed_co[1];
      data->arc_last_angle = std::atan2(s.y - c.y, s.x - c.x);
      data->arc_sweep = 0.0;
    }
    if (data->placed.size() >= shape_points_needed(data->shape)) {
      if (draw_commit_shape(C, *data)) {
        return draw_finish(C, op, false);
      }
      if (data->shape != Shape::Line) {
        /* One shape per run: the tool stays active for the next one. */
        return draw_finish(C, op, false);
      }
    }
    draw_preview(C, *data, float2(event->mval));
    return OPERATOR_RUNNING_MODAL;
  }
  if (is_cancel_event(event) || is_confirm_event(event)) {
    return draw_finish(C, op, true);
  }
  if (is_navigation_event(event)) {
    return OPERATOR_PASS_THROUGH;
  }
  return OPERATOR_RUNNING_MODAL;
}

static void draw_cancel(bContext *C, wmOperator *op)
{
  draw_finish(C, op, true);
}

static void draw_ot_common(wmOperatorType *ot)
{
  ot->modal = draw_modal;
  ot->cancel = draw_cancel;
  ot->poll = sketch_edit_poll_view3d;
  ot->flag = OPTYPE_UNDO | OPTYPE_BLOCKING;
}

static wmOperatorStatus line_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  return draw_invoke(C, op, event, Shape::Line);
}
static wmOperatorStatus rectangle_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  return draw_invoke(C, op, event, Shape::Rectangle);
}
static wmOperatorStatus circle_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  return draw_invoke(C, op, event, Shape::Circle);
}
static wmOperatorStatus arc_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  return draw_invoke(C, op, event, Shape::Arc);
}

void CAD_OT_sketch_line(wmOperatorType *ot)
{
  ot->name = "Line";
  ot->description = "Draw connected lines; click the first point again to close the loop";
  ot->idname = "CAD_OT_sketch_line";
  ot->invoke = line_invoke;
  draw_ot_common(ot);
}

void CAD_OT_sketch_rectangle(wmOperatorType *ot)
{
  ot->name = "Rectangle";
  ot->description = "Draw a rectangle from two corners";
  ot->idname = "CAD_OT_sketch_rectangle";
  ot->invoke = rectangle_invoke;
  draw_ot_common(ot);
}

void CAD_OT_sketch_circle(wmOperatorType *ot)
{
  ot->name = "Circle";
  ot->description = "Draw a circle from its center and a point on it";
  ot->idname = "CAD_OT_sketch_circle";
  ot->invoke = circle_invoke;
  draw_ot_common(ot);
}

void CAD_OT_sketch_arc(wmOperatorType *ot)
{
  ot->name = "Arc";
  ot->description = "Draw an arc from its center, start and end";
  ot->idname = "CAD_OT_sketch_arc";
  ot->invoke = arc_invoke;
  draw_ot_common(ot);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Constraints on the Selection
 * \{ */

enum ConstrainType {
  CONSTRAIN_COINCIDENT = 0,
  CONSTRAIN_HORIZONTAL_VERTICAL,
  CONSTRAIN_HORIZONTAL,
  CONSTRAIN_VERTICAL,
  CONSTRAIN_PARALLEL,
  CONSTRAIN_PERPENDICULAR,
  CONSTRAIN_TANGENT,
  CONSTRAIN_EQUAL,
  CONSTRAIN_FIX,
};

static wmOperatorStatus sketch_constrain_exec(bContext *C, wmOperator *op)
{
  const std::optional<SketchEdit> edit = sketch_edit_get(C);
  CADPart *part = edit->part;
  CADSketch *sketch = edit->sketch;
  const int type = RNA_enum_get(op->ptr, "type");

  Vector<int> points, lines, rounds;
  for (int i = 0; i < sketch->points_num; i++) {
    if (sketch->points[i].flag & CAD_SKETCH_SELECT) {
      points.append(sketch->points[i].uid);
    }
  }
  for (int i = 0; i < sketch->curves_num; i++) {
    const CADSketchCurve &c = sketch->curves[i];
    if (c.flag & CAD_SKETCH_SELECT) {
      (c.type == CAD_SKETCH_LINE ? lines : rounds).append(c.uid);
    }
  }

  Vector<int> added;
  auto add = [&](const eCADConstraintType t, std::initializer_list<int> entities) {
    added.append(BKE_cad_sketch_constraint_add(part, sketch, t, Span<int>(entities), 0.0));
  };
  switch (type) {
    case CONSTRAIN_COINCIDENT:
      if (points.size() >= 2) {
        for (int i = 1; i < points.size(); i++) {
          add(CAD_CONSTRAINT_COINCIDENT, {points[0], points[i]});
        }
      }
      else if (points.size() == 1 && (lines.size() + rounds.size()) == 1) {
        add(CAD_CONSTRAINT_POINT_ON_CURVE, {points[0], lines.is_empty() ? rounds[0] : lines[0]});
      }
      break;
    case CONSTRAIN_HORIZONTAL_VERTICAL:
    case CONSTRAIN_HORIZONTAL:
    case CONSTRAIN_VERTICAL:
      for (const int line : lines) {
        const CADSketchCurve *c = BKE_cad_sketch_curve_find(sketch, line);
        const double2 d = point_co(sketch, c->points[1]) - point_co(sketch, c->points[0]);
        const bool horizontal = type == CONSTRAIN_HORIZONTAL ||
                                (type == CONSTRAIN_HORIZONTAL_VERTICAL &&
                                 std::abs(d.x) >= std::abs(d.y));
        add(horizontal ? CAD_CONSTRAINT_HORIZONTAL : CAD_CONSTRAINT_VERTICAL, {line});
      }
      if (lines.is_empty() && points.size() == 2) {
        const double2 d = point_co(sketch, points[1]) - point_co(sketch, points[0]);
        const bool horizontal = type == CONSTRAIN_HORIZONTAL ||
                                (type == CONSTRAIN_HORIZONTAL_VERTICAL &&
                                 std::abs(d.x) >= std::abs(d.y));
        add(horizontal ? CAD_CONSTRAINT_HORIZONTAL : CAD_CONSTRAINT_VERTICAL,
            {points[0], points[1]});
      }
      break;
    case CONSTRAIN_PARALLEL:
    case CONSTRAIN_PERPENDICULAR:
      if (lines.size() == 2) {
        add(type == CONSTRAIN_PARALLEL ? CAD_CONSTRAINT_PARALLEL : CAD_CONSTRAINT_PERPENDICULAR,
            {lines[0], lines[1]});
      }
      break;
    case CONSTRAIN_TANGENT:
      if (lines.size() == 1 && rounds.size() == 1) {
        add(CAD_CONSTRAINT_TANGENT, {lines[0], rounds[0]});
      }
      else if (rounds.size() == 2) {
        add(CAD_CONSTRAINT_TANGENT, {rounds[0], rounds[1]});
      }
      break;
    case CONSTRAIN_EQUAL: {
      const Vector<int> &group = lines.size() >= 2 ? lines : rounds;
      for (int i = 1; i < group.size(); i++) {
        add(CAD_CONSTRAINT_EQUAL, {group[0], group[i]});
      }
      break;
    }
    case CONSTRAIN_FIX:
      for (const int p : points) {
        add(CAD_CONSTRAINT_FIX, {p});
      }
      break;
  }
  if (added.is_empty()) {
    BKE_report(op->reports, RPT_ERROR, "The selection does not fit this constraint");
    return OPERATOR_CANCELLED;
  }
  if (!sketch_changed(C, *edit, added)) {
    BKE_report(op->reports, RPT_WARNING, "Not added: it conflicts with existing constraints");
    return OPERATOR_CANCELLED;
  }
  return OPERATOR_FINISHED;
}

void CAD_OT_sketch_constrain(wmOperatorType *ot)
{
  static const EnumPropertyItem type_items[] = {
      {CONSTRAIN_COINCIDENT, "COINCIDENT", 0, "Coincident", "Points together, or a point on a curve"},
      {CONSTRAIN_HORIZONTAL_VERTICAL,
       "HORIZONTAL_VERTICAL",
       0,
       "Horizontal/Vertical",
       "Horizontal or vertical, whichever is closer"},
      {CONSTRAIN_HORIZONTAL, "HORIZONTAL", 0, "Horizontal", ""},
      {CONSTRAIN_VERTICAL, "VERTICAL", 0, "Vertical", ""},
      {CONSTRAIN_PARALLEL, "PARALLEL", 0, "Parallel", "Two lines"},
      {CONSTRAIN_PERPENDICULAR, "PERPENDICULAR", 0, "Perpendicular", "Two lines"},
      {CONSTRAIN_TANGENT, "TANGENT", 0, "Tangent", "A line and a circle/arc, or two circles/arcs"},
      {CONSTRAIN_EQUAL, "EQUAL", 0, "Equal", "Equal lengths or radii"},
      {CONSTRAIN_FIX, "FIX", 0, "Fix", "Points do not move"},
      {0, nullptr, 0, nullptr, nullptr},
  };
  ot->name = "Add Constraint";
  ot->description = "Constrain the selected sketch geometry";
  ot->idname = "CAD_OT_sketch_constrain";
  ot->exec = sketch_constrain_exec;
  ot->poll = sketch_edit_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
  ot->prop = RNA_def_enum(ot->srna, "type", type_items, CONSTRAIN_COINCIDENT, "Type", "");
}

static wmOperatorStatus construction_toggle_exec(bContext *C, wmOperator * /*op*/)
{
  const std::optional<SketchEdit> edit = sketch_edit_get(C);
  bool changed = false;
  for (int i = 0; i < edit->sketch->curves_num; i++) {
    CADSketchCurve &c = edit->sketch->curves[i];
    if (c.flag & CAD_SKETCH_SELECT) {
      c.flag ^= CAD_SKETCH_CONSTRUCTION;
      changed = true;
    }
  }
  if (!changed) {
    return OPERATOR_CANCELLED;
  }
  sketch_changed(C, *edit);
  return OPERATOR_FINISHED;
}

void CAD_OT_sketch_construction_toggle(wmOperatorType *ot)
{
  ot->name = "Toggle Construction";
  ot->description = "Construction geometry guides constraints but does not form profiles";
  ot->idname = "CAD_OT_sketch_construction_toggle";
  ot->exec = construction_toggle_exec;
  ot->poll = sketch_edit_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Dimensions
 * \{ */

/** Opens the value popup for a dimension. */
static void dimension_edit_popup(bContext *C, const int constraint_uid)
{
  wmOperatorType *ot = WM_operatortype_find("CAD_OT_sketch_dimension_edit", false);
  PointerRNA ptr = WM_operator_properties_create_ptr(ot);
  RNA_int_set(&ptr, "constraint_uid", constraint_uid);
  WM_operator_name_call_ptr(C, ot, wm::OpCallContext::InvokeDefault, &ptr, nullptr);
  WM_operator_properties_free(&ptr);
}

struct DimensionData {
  SketchEdit edit;
  SketchHit first;
  /** The dimension made from two picked elements, while its label is placed (else -1). */
  int placing_uid = -1;
};

/** Moves the label of dimension `uid` under the mouse. */
static void dimension_label_to_mouse(const ARegion *region,
                                     const SketchEdit &edit,
                                     const int uid,
                                     const float2 &mval)
{
  CADSketch *sketch = edit.sketch;
  const CADSketchConstraint *c = BKE_cad_sketch_constraint_find(sketch, uid);
  const SketchFrame frame = sketch_frame(edit.ob, sketch);
  if (c == nullptr) {
    return;
  }
  if (const std::optional<double2> co = sketch_mouse_co(region, frame, mval)) {
    sketch_constraint_label_place(region, frame, sketch, int(c - sketch->constraints), *co);
  }
}

static void dimension_data_free(bContext *C, wmOperator *op)
{
  MEM_delete(static_cast<DimensionData *>(op->customdata));
  op->customdata = nullptr;
  ED_workspace_status_text(C, nullptr);
  sketch_hover_clear();
}

/** Creates the dimension for one or two picked elements. Returns its uid or -1. */
static int dimension_create(const SketchEdit &edit, const SketchHit &a, const SketchHit *b)
{
  CADPart *part = edit.part;
  CADSketch *sketch = edit.sketch;
  auto add = [&](const eCADConstraintType t, std::initializer_list<int> e, const double value) {
    return BKE_cad_sketch_constraint_add(part, sketch, t, Span<int>(e), value);
  };
  auto is_point = [&](const SketchHit &h) {
    return h.type == SketchHitType::Point || h.type == SketchHitType::Origin;
  };
  const CADSketchCurve *ca = a.type == SketchHitType::Curve ?
                                 BKE_cad_sketch_curve_find(sketch, a.uid) :
                                 nullptr;
  const CADSketchCurve *cb = (b && b->type == SketchHitType::Curve) ?
                                 BKE_cad_sketch_curve_find(sketch, b->uid) :
                                 nullptr;
  if (b == nullptr) {
    if (ca && ca->type == CAD_SKETCH_LINE) {
      return add(CAD_CONSTRAINT_DISTANCE,
                 {ca->uid},
                 math::distance(point_co(sketch, ca->points[0]), point_co(sketch, ca->points[1])));
    }
    if (ca && ca->type == CAD_SKETCH_CIRCLE) {
      return add(CAD_CONSTRAINT_DIAMETER, {ca->uid}, ca->radius * 2.0);
    }
    if (ca && ca->type == CAD_SKETCH_ARC) {
      return add(CAD_CONSTRAINT_RADIUS,
                 {ca->uid},
                 math::distance(point_co(sketch, ca->points[0]), point_co(sketch, ca->points[2])));
    }
    return -1;
  }
  if (is_point(a) && is_point(*b) && a.uid != b->uid) {
    return add(CAD_CONSTRAINT_DISTANCE,
               {a.uid, b->uid},
               math::distance(point_co(sketch, a.uid), point_co(sketch, b->uid)));
  }
  const bool a_line = ca && ca->type == CAD_SKETCH_LINE;
  const bool b_line = cb && cb->type == CAD_SKETCH_LINE;
  if ((is_point(a) && b_line) || (is_point(*b) && a_line)) {
    const int p = is_point(a) ? a.uid : b->uid;
    const CADSketchCurve *line = a_line ? ca : cb;
    const double2 p0 = point_co(sketch, line->points[0]), p1 = point_co(sketch, line->points[1]);
    const double2 d = math::normalize(p1 - p0);
    const double2 v = point_co(sketch, p) - p0;
    return add(CAD_CONSTRAINT_DISTANCE, {p, line->uid}, std::abs(d.x * v.y - d.y * v.x));
  }
  if (a_line && b_line && ca != cb) {
    const double2 d0 = point_co(sketch, ca->points[1]) - point_co(sketch, ca->points[0]);
    const double2 d1 = point_co(sketch, cb->points[1]) - point_co(sketch, cb->points[0]);
    const double angle = std::atan2(d0.x * d1.y - d0.y * d1.x, math::dot(d0, d1));
    if (std::abs(std::sin(angle)) < 1e-6) {
      /* Parallel lines: their distance. */
      const double2 n = math::normalize(double2(-d0.y, d0.x));
      return add(CAD_CONSTRAINT_DISTANCE,
                 {cb->points[0], ca->uid},
                 std::abs(math::dot(point_co(sketch, cb->points[0]) - point_co(sketch, ca->points[0]), n)));
    }
    return add(CAD_CONSTRAINT_ANGLE, {ca->uid, cb->uid}, angle);
  }
  return -1;
}

/**
 * Creates the dimension for the picked elements. A dimension of one element is placed at
 * `place_mval`; one of two elements gets its label placed with another click.
 */
static wmOperatorStatus dimension_create_from_picks(bContext *C,
                                                    wmOperator *op,
                                                    DimensionData *data,
                                                    const SketchHit *second,
                                                    const float2 &place_mval)
{
  const SketchEdit edit = data->edit;
  const int uid = dimension_create(edit, data->first, second);
  if (uid < 0) {
    dimension_data_free(C, op);
    BKE_report(op->reports, RPT_WARNING, "No dimension for this selection");
    tag_redraw(C);
    return OPERATOR_CANCELLED;
  }
  if (!sketch_changed(C, edit, {uid})) {
    dimension_data_free(C, op);
    BKE_report(op->reports,
               RPT_WARNING,
               "Not added: the geometry is already fully defined by other constraints");
    return OPERATOR_CANCELLED;
  }
  dimension_label_to_mouse(CTX_wm_region(C), edit, uid, place_mval);
  if (second != nullptr) {
    data->placing_uid = uid;
    WorkspaceStatus status(C);
    status.item(IFACE_("Place Dimension"), ICON_MOUSE_LMB);
    status.item(IFACE_("Cancel"), ICON_EVENT_ESC, ICON_MOUSE_RMB);
    tag_redraw(C);
    return OPERATOR_RUNNING_MODAL;
  }
  dimension_data_free(C, op);
  dimension_edit_popup(C, uid);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus dimension_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  const std::optional<SketchEdit> edit = sketch_edit_get(C);
  DimensionData *data = MEM_new<DimensionData>(__func__);
  data->edit = *edit;
  op->customdata = data;
  if (event->type == LEFTMOUSE && event->val == KM_PRESS) {
    const SketchFrame frame = sketch_frame(edit->ob, edit->sketch);
    data->first = sketch_pick(CTX_wm_region(C),
                              frame,
                              edit->sketch,
                              float2(event->mval),
                              PICK_POINTS | PICK_CURVES | PICK_ORIGIN);
  }
  WorkspaceStatus status(C);
  status.item(IFACE_("Pick Geometry / Place"), ICON_MOUSE_LMB);
  status.item(IFACE_("Cancel"), ICON_EVENT_ESC, ICON_MOUSE_RMB);
  WM_event_add_modal_handler(C, op);
  return OPERATOR_RUNNING_MODAL;
}

/** Cancels the tool, removing a dimension whose label was being placed. */
static void dimension_abort(bContext *C, wmOperator *op)
{
  DimensionData *data = static_cast<DimensionData *>(op->customdata);
  if (data == nullptr) {
    return;
  }
  if (data->placing_uid != -1) {
    const int uid = data->placing_uid;
    BKE_cad_sketch_remove(data->edit.sketch, Span<int>(&uid, 1));
    sketch_changed(C, data->edit);
  }
  dimension_data_free(C, op);
  tag_redraw(C);
}

static wmOperatorStatus dimension_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  DimensionData *data = static_cast<DimensionData *>(op->customdata);
  const ARegion *region = CTX_wm_region(C);
  const SketchFrame frame = sketch_frame(data->edit.ob, data->edit.sketch);
  const float2 mval(event->mval);

  if (data->placing_uid != -1) {
    /* The label follows the mouse until a click places it. */
    if (event->type == MOUSEMOVE) {
      dimension_label_to_mouse(region, data->edit, data->placing_uid, mval);
      tag_redraw(C);
      return OPERATOR_RUNNING_MODAL;
    }
    if (event->type == LEFTMOUSE && event->val == KM_PRESS) {
      const int uid = data->placing_uid;
      dimension_label_to_mouse(region, data->edit, uid, mval);
      dimension_data_free(C, op);
      tag_redraw(C);
      dimension_edit_popup(C, uid);
      return OPERATOR_FINISHED;
    }
  }
  else if (event->type == MOUSEMOVE) {
    sketch_hover().hit = sketch_pick(
        region, frame, data->edit.sketch, mval, PICK_POINTS | PICK_CURVES | PICK_ORIGIN);
    tag_redraw(C);
    return OPERATOR_RUNNING_MODAL;
  }
  else if (event->type == LEFTMOUSE && event->val == KM_PRESS) {
    const SketchHit hit = sketch_pick(
        region, frame, data->edit.sketch, mval, PICK_POINTS | PICK_CURVES | PICK_ORIGIN);
    if (data->first.type == SketchHitType::None) {
      data->first = hit;
      return OPERATOR_RUNNING_MODAL;
    }
    const bool single = hit.type == SketchHitType::None ||
                        (hit.type == data->first.type && hit.uid == data->first.uid);
    return dimension_create_from_picks(C, op, data, single ? nullptr : &hit, mval);
  }
  if (is_cancel_event(event)) {
    dimension_abort(C, op);
    return OPERATOR_CANCELLED;
  }
  if (is_navigation_event(event)) {
    return OPERATOR_PASS_THROUGH;
  }
  return OPERATOR_RUNNING_MODAL;
}

static void dimension_cancel(bContext *C, wmOperator *op)
{
  dimension_abort(C, op);
}

void CAD_OT_sketch_dimension(wmOperatorType *ot)
{
  ot->name = "Dimension";
  ot->description =
      "Dimension a line, circle or arc (click it, then click to place), or the distance or "
      "angle between two elements (click both)";
  ot->idname = "CAD_OT_sketch_dimension";
  ot->invoke = dimension_invoke;
  ot->modal = dimension_modal;
  ot->cancel = dimension_cancel;
  ot->poll = sketch_edit_poll_view3d;
  ot->flag = OPTYPE_UNDO | OPTYPE_BLOCKING;
}

static CADSketchConstraint *dimension_from_op(bContext *C, wmOperator *op)
{
  const std::optional<SketchEdit> edit = sketch_edit_get(C);
  if (!edit) {
    return nullptr;
  }
  CADSketchConstraint *c = BKE_cad_sketch_constraint_find(edit->sketch,
                                                          RNA_int_get(op->ptr, "constraint_uid"));
  return (c && c->type >= CAD_CONSTRAINT_DISTANCE) ? c : nullptr;
}

static wmOperatorStatus dimension_edit_exec(bContext *C, wmOperator *op)
{
  const std::optional<SketchEdit> edit = sketch_edit_get(C);
  CADSketchConstraint *c = dimension_from_op(C, op);
  if (c == nullptr) {
    return OPERATOR_CANCELLED;
  }
  const bool is_angle = c->type == CAD_CONSTRAINT_ANGLE;
  const double old_value = c->value;
  const double value = is_angle ? RNA_float_get(op->ptr, "angle") :
                                  RNA_float_get(op->ptr, "length");
  c->value = (c->type == CAD_CONSTRAINT_DISTANCE_X || c->type == CAD_CONSTRAINT_DISTANCE_Y) &&
                     old_value < 0.0 ?
                 -std::abs(value) :
                 value;
  if (!BKE_cad_sketch_solve(edit->part, edit->sketch)) {
    /* Keep the sketch solvable: restore the old value. */
    c = dimension_from_op(C, op);
    c->value = old_value;
    sketch_changed(C, *edit);
    BKE_report(op->reports, RPT_ERROR, "The sketch cannot take this value");
    return OPERATOR_CANCELLED;
  }
  sketch_changed(C, *edit);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus dimension_edit_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  if (!RNA_struct_property_is_set(op->ptr, "constraint_uid")) {
    /* Double-click: the dimension label under the cursor. */
    const std::optional<SketchEdit> edit = sketch_edit_get(C);
    const SketchFrame frame = sketch_frame(edit->ob, edit->sketch);
    const SketchHit hit = sketch_pick(
        CTX_wm_region(C), frame, edit->sketch, float2(event->mval), PICK_CONSTRAINTS);
    if (hit.type != SketchHitType::Constraint) {
      return OPERATOR_CANCELLED | OPERATOR_PASS_THROUGH;
    }
    RNA_int_set(op->ptr, "constraint_uid", hit.uid);
  }
  CADSketchConstraint *c = dimension_from_op(C, op);
  if (c == nullptr) {
    return OPERATOR_CANCELLED | OPERATOR_PASS_THROUGH;
  }
  const bool is_angle = c->type == CAD_CONSTRAINT_ANGLE;
  RNA_float_set(op->ptr, is_angle ? "angle" : "length", float(is_angle ? c->value : std::abs(c->value)));
  return WM_operator_props_popup_confirm_ex(
      C, op, event, IFACE_("Dimension"), IFACE_("Set"));
}

static void dimension_edit_draw(bContext *C, wmOperator *op)
{
  CADSketchConstraint *c = dimension_from_op(C, op);
  ui::Layout &layout = *op->layout;
  layout.use_property_split_set(true);
  layout.use_property_decorate_set(false);
  const bool is_angle = c && c->type == CAD_CONSTRAINT_ANGLE;
  layout.prop(op->ptr, is_angle ? "angle" : "length", UI_ITEM_NONE, std::nullopt, ICON_NONE);
}

void CAD_OT_sketch_dimension_edit(wmOperatorType *ot)
{
  ot->name = "Edit Dimension";
  ot->description = "Set the value of a sketch dimension";
  ot->idname = "CAD_OT_sketch_dimension_edit";
  ot->invoke = dimension_edit_invoke;
  ot->exec = dimension_edit_exec;
  ot->ui = dimension_edit_draw;
  ot->poll = sketch_edit_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  PropertyRNA *prop = RNA_def_int(
      ot->srna, "constraint_uid", -1, -1, INT_MAX, "Dimension", "", -1, INT_MAX);
  RNA_def_property_flag(prop, PropertyFlag(PROP_HIDDEN | PROP_SKIP_SAVE));
  RNA_def_float_distance(ot->srna, "length", 0.0f, 0.0f, FLT_MAX, "Value", "", 0.0f, 100.0f);
  prop = RNA_def_float_rotation(
      ot->srna, "angle", 0, nullptr, -FLT_MAX, FLT_MAX, "Value", "", -FLT_MAX, FLT_MAX);
}

/** \} */

}  // namespace blender::ed::cad
