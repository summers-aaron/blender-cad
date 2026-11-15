/* SPDX-FileCopyrightText: 2026 SimpleCAD Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edcad
 *
 * Drawing of the sketch being edited: geometry on the sketch plane (on top of the model),
 * constraint glyphs, dimension values and the solve state.
 *
 * Colors follow common CAD conventions: under-constrained geometry is blue, a fully
 * constrained sketch is drawn in a neutral color, construction geometry is dashed orange-ish,
 * selection uses the theme's selection color and conflicts are red.
 */

#include <fmt/format.h>

#include "DNA_cad_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_view3d_types.h"

#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"

#include "BKE_cad.hh"
#include "BKE_context.hh"
#include "BKE_screen.hh"
#include "BKE_unit.hh"

#include "BLF_api.hh"

#include "ED_space_api.hh"
#include "ED_view3d.hh"

#include "GPU_immediate.hh"
#include "GPU_matrix.hh"
#include "GPU_state.hh"

#include "UI_interface_c.hh"
#include "UI_resources.hh"

#include "cad_sketch.hh"

namespace blender::ed::cad {

static const float4 COLOR_FREE = {0.38f, 0.68f, 1.0f, 1.0f};
static const float4 COLOR_CONSTRAINED = {0.92f, 0.92f, 0.92f, 1.0f};
static const float4 COLOR_CONSTRUCTION = {0.95f, 0.62f, 0.28f, 0.8f};
static const float4 COLOR_CONFLICT = {1.0f, 0.25f, 0.2f, 1.0f};
static const float4 COLOR_HOVER = {1.0f, 0.95f, 0.55f, 1.0f};
static const float4 COLOR_PREVIEW = {0.55f, 0.85f, 1.0f, 0.9f};

static float4 select_color()
{
  float4 color(0.0f, 0.0f, 0.0f, 1.0f);
  ui::theme::get_color_3fv(TH_SELECT, color);
  return color;
}

/** Entities of conflicting constraints, drawn red. */
static Set<int> conflicting_entities(const CADSketch *sketch)
{
  Set<int> uids;
  for (int i = 0; i < sketch->constraints_num; i++) {
    const CADSketchConstraint &c = sketch->constraints[i];
    if (c.flag & CAD_CONSTRAINT_CONFLICTING) {
      for (const int e : c.entities) {
        if (e >= 0) {
          uids.add(e);
        }
      }
    }
  }
  return uids;
}

static float4 element_color(const SketchHitType type,
                            const int uid,
                            const char flag,
                            const Set<int> &conflicts)
{
  const SketchHover &hover = sketch_hover();
  if (hover.hit.type == type && hover.hit.uid == uid) {
    return COLOR_HOVER;
  }
  if (flag & CAD_SKETCH_SELECT) {
    return select_color();
  }
  if (conflicts.contains(uid)) {
    return COLOR_CONFLICT;
  }
  if (flag & CAD_SKETCH_CONSTRUCTION) {
    return COLOR_CONSTRUCTION;
  }
  /* Like other CAD tools: what can still move is drawn in the "free" color. */
  return (flag & CAD_SKETCH_CONSTRAINED) ? COLOR_CONSTRAINED : COLOR_FREE;
}

static void draw_geometry(const bContext *C, ARegion *region, void * /*arg*/)
{
  const std::optional<SketchEdit> edit = sketch_edit_get(C);
  if (!edit) {
    return;
  }
  const CADSketch *sketch = edit->sketch;
  const SketchFrame frame = sketch_frame(edit->ob, sketch);
  const RegionView3D *rv3d = static_cast<const RegionView3D *>(region->regiondata);
  const Set<int> conflicts = conflicting_entities(sketch);

  GPU_matrix_push();
  GPU_matrix_mul(frame.to_world.ptr());
  GPU_depth_test(GPU_DEPTH_NONE);
  GPU_blend(GPU_BLEND_ALPHA);

  const uint pos = GPU_vertformat_attr_add(
      immVertexFormat(), "pos", gpu::VertAttrType::SFLOAT_32_32_32);
  float viewport[4];
  GPU_viewport_size_get_f(viewport);

  /* Sketch axes, sized in pixels at the origin. */
  const float axis_len = ED_view3d_pixel_size(rv3d, frame.origin_world) * 80.0f /
                         std::max(1e-8f, math::length(frame.to_world.x_axis()));
  immBindBuiltinProgram(GPU_SHADER_3D_POLYLINE_UNIFORM_COLOR);
  immUniform2fv("viewportSize", &viewport[2]);
  immUniform1f("lineWidth", 1.0f * U.pixelsize);
  immUniformColor4f(0.9f, 0.3f, 0.3f, 0.6f);
  immBegin(GPU_PRIM_LINES, 2);
  immVertex3f(pos, -axis_len, 0.0f, 0.0f);
  immVertex3f(pos, axis_len, 0.0f, 0.0f);
  immEnd();
  immUniformColor4f(0.4f, 0.85f, 0.4f, 0.6f);
  immBegin(GPU_PRIM_LINES, 2);
  immVertex3f(pos, 0.0f, -axis_len, 0.0f);
  immVertex3f(pos, 0.0f, axis_len, 0.0f);
  immEnd();

  /* Curves. */
  for (int i = 0; i < sketch->curves_num; i++) {
    const CADSketchCurve &c = sketch->curves[i];
    const bool construction = c.flag & CAD_SKETCH_CONSTRUCTION;
    const float4 color = element_color(SketchHitType::Curve, c.uid, c.flag, conflicts);
    immUniformColor4fv(color);
    immUniform1f("lineWidth", (construction ? 1.0f : 2.0f) * U.pixelsize);
    const Vector<double2> poly = sketch_curve_polyline(sketch, i, 96);
    if (poly.size() < 2) {
      continue;
    }
    /* Construction geometry dashed: draw every other segment of a finer polyline. */
    immBegin(GPU_PRIM_LINES, uint(poly.size() - 1) * 2);
    for (int k = 0; k + 1 < poly.size(); k++) {
      immVertex3f(pos, float(poly[k].x), float(poly[k].y), 0.0f);
      immVertex3f(pos, float(poly[k + 1].x), float(poly[k + 1].y), 0.0f);
    }
    immEnd();
  }

  /* Tool preview. */
  const SketchPreview &preview = sketch_hover().preview;
  if (!preview.lines.is_empty()) {
    immUniformColor4fv(COLOR_PREVIEW);
    immUniform1f("lineWidth", 1.5f * U.pixelsize);
    immBegin(GPU_PRIM_LINES, uint(preview.lines.size()));
    for (const double2 &co : preview.lines) {
      immVertex3f(pos, float(co.x), float(co.y), 0.0f);
    }
    immEnd();
  }
  immUnbindProgram();

  /* Points. */
  immBindBuiltinProgram(GPU_SHADER_3D_POINT_UNIFORM_SIZE_UNIFORM_COLOR_AA);
  immUniform1f("size", 7.0f * U.pixelsize);
  immUniformColor4f(1.0f, 1.0f, 1.0f, 0.9f);
  immBegin(GPU_PRIM_POINTS, 1);
  immVertex3f(pos, 0.0f, 0.0f, 0.0f);
  immEnd();
  for (int i = 0; i < sketch->points_num; i++) {
    const CADSketchPoint &p = sketch->points[i];
    const float4 color = element_color(SketchHitType::Point, p.uid, p.flag, conflicts);
    immUniformColor4fv(color);
    immBegin(GPU_PRIM_POINTS, 1);
    immVertex3f(pos, float(p.co[0]), float(p.co[1]), 0.0f);
    immEnd();
  }
  if (!preview.points.is_empty()) {
    immUniformColor4fv(COLOR_PREVIEW);
    immBegin(GPU_PRIM_POINTS, uint(preview.points.size()));
    for (const double2 &co : preview.points) {
      immVertex3f(pos, float(co.x), float(co.y), 0.0f);
    }
    immEnd();
  }
  if (sketch_hover().hit.type == SketchHitType::Origin) {
    immUniformColor4fv(COLOR_HOVER);
    immUniform1f("size", 10.0f * U.pixelsize);
    immBegin(GPU_PRIM_POINTS, 1);
    immVertex3f(pos, 0.0f, 0.0f, 0.0f);
    immEnd();
  }
  immUnbindProgram();

  GPU_blend(GPU_BLEND_NONE);
  GPU_matrix_pop();
}

/** Glyph for a geometric constraint. */
static const char *constraint_glyph(const short type)
{
  switch (type) {
    case CAD_CONSTRAINT_HORIZONTAL:
      return "H";
    case CAD_CONSTRAINT_VERTICAL:
      return "V";
    case CAD_CONSTRAINT_PARALLEL:
      return "\xe2\x88\xa5"; /* ∥ */
    case CAD_CONSTRAINT_PERPENDICULAR:
      return "\xe2\x8a\xa5"; /* ⊥ */
    case CAD_CONSTRAINT_TANGENT:
      return "T";
    case CAD_CONSTRAINT_EQUAL:
      return "=";
    case CAD_CONSTRAINT_FIX:
      return "\xf0\x9f\x94\x92"; /* lock */
    default:
      return nullptr;
  }
}

static std::string dimension_text(const Scene *scene, const CADSketchConstraint &c)
{
  char buf[64];
  if (c.type == CAD_CONSTRAINT_ANGLE) {
    BKE_unit_value_as_string(buf, c.value, 2, B_UNIT_ROTATION, scene->unit, false, true);
    return buf;
  }
  BKE_unit_value_as_string_scaled(buf, std::abs(c.value), 3, B_UNIT_LENGTH, scene->unit, false, true);
  switch (c.type) {
    case CAD_CONSTRAINT_RADIUS:
      return std::string("R ") + buf;
    case CAD_CONSTRAINT_DIAMETER:
      return std::string("\xe2\x8c\x80 ") + buf; /* ⌀ */
    default:
      return buf;
  }
}

static void draw_text_box(const int font,
                          const std::string &text,
                          const float2 &center,
                          const float4 &text_color,
                          const float4 &box_color)
{
  const float width = BLF_width(font, text.c_str(), text.size());
  const float height = BLF_height(font, "8", 1);
  const float pad = 3.0f * UI_SCALE_FAC;
  if (box_color.w > 0.0f) {
    const rctf rect = {center.x - width / 2 - pad,
                       center.x + width / 2 + pad,
                       center.y - height / 2 - pad,
                       center.y + height / 2 + pad};
    ui::draw_roundbox_corner_set(ui::CNR_ALL);
    ui::draw_roundbox_4fv(&rect, true, 3.0f * UI_SCALE_FAC, box_color);
  }
  BLF_color4fv(font, text_color);
  BLF_position(font, center.x - width / 2, center.y - height / 2, 0.0f);
  BLF_draw(font, text.c_str(), text.size());
}

/** Extension lines, dimension line and arrows of a dimension (region pixels). */
static void draw_dimension_geometry(const SketchDimensionGeometry &geom, const float4 &color)
{
  if (geom.lines.is_empty() && geom.arrows.is_empty()) {
    return;
  }
  GPUVertFormat *format = immVertexFormat();
  const uint pos = GPU_vertformat_attr_add(format, "pos", gpu::VertAttrType::SFLOAT_32_32_32);
  if (!geom.lines.is_empty()) {
    float viewport[4];
    GPU_viewport_size_get_f(viewport);
    immBindBuiltinProgram(GPU_SHADER_3D_POLYLINE_UNIFORM_COLOR);
    immUniform2fv("viewportSize", &viewport[2]);
    immUniform1f("lineWidth", 1.0f * U.pixelsize);
    immUniformColor4fv(color);
    immBegin(GPU_PRIM_LINES, uint(geom.lines.size()));
    for (const float2 &p : geom.lines) {
      immVertex3f(pos, p.x, p.y, 0.0f);
    }
    immEnd();
    immUnbindProgram();
  }
  if (!geom.arrows.is_empty()) {
    const float length = 9.0f * UI_SCALE_FAC;
    const float half_width = 3.0f * UI_SCALE_FAC;
    immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
    immUniformColor4fv(color);
    immBegin(GPU_PRIM_TRIS, uint(geom.arrows.size()) * 3);
    for (const auto &[tip, dir] : geom.arrows) {
      const float2 side(-dir.y, dir.x);
      const float2 base = tip - dir * length;
      const float2 l = base + side * half_width, r = base - side * half_width;
      immVertex3f(pos, tip.x, tip.y, 0.0f);
      immVertex3f(pos, l.x, l.y, 0.0f);
      immVertex3f(pos, r.x, r.y, 0.0f);
    }
    immEnd();
    immUnbindProgram();
  }
}

static void draw_labels(const bContext *C, ARegion *region, void * /*arg*/)
{
  const std::optional<SketchEdit> edit = sketch_edit_get(C);
  if (!edit) {
    return;
  }
  const Scene *scene = CTX_data_scene(C);
  const CADSketch *sketch = edit->sketch;
  const SketchFrame frame = sketch_frame(edit->ob, sketch);
  const SketchHover &hover = sketch_hover();

  const uiFontStyle *fstyle = UI_FSTYLE_WIDGET;
  const int font = fstyle->uifont_id;
  BLF_size(font, fstyle->points * UI_SCALE_FAC);
  GPU_blend(GPU_BLEND_ALPHA);

  const float4 box = {0.12f, 0.12f, 0.12f, 0.75f};
  auto constraint_color = [&](const CADSketchConstraint &c, const bool is_dimension) {
    float4 color = is_dimension ? float4(1.0f) : float4(0.75f, 0.85f, 1.0f, 0.95f);
    if (c.flag & CAD_CONSTRAINT_CONFLICTING) {
      color = COLOR_CONFLICT;
    }
    else if (c.flag & CAD_CONSTRAINT_REDUNDANT) {
      color = float4(0.95f, 0.8f, 0.3f, 1.0f);
    }
    if (c.flag & CAD_CONSTRAINT_SELECT) {
      color = select_color();
    }
    if (hover.hit.type == SketchHitType::Constraint && hover.hit.uid == c.uid) {
      color = COLOR_HOVER;
    }
    return color;
  };
  /* Dimension lines first, labels on top. */
  for (int i = 0; i < sketch->constraints_num; i++) {
    const CADSketchConstraint &c = sketch->constraints[i];
    if (c.type >= CAD_CONSTRAINT_DISTANCE) {
      float4 color = constraint_color(c, true);
      color.w *= 0.8f;
      draw_dimension_geometry(sketch_dimension_geometry(region, frame, sketch, i), color);
    }
  }
  for (int i = 0; i < sketch->constraints_num; i++) {
    const CADSketchConstraint &c = sketch->constraints[i];
    const bool is_dimension = c.type >= CAD_CONSTRAINT_DISTANCE;
    const char *glyph = constraint_glyph(c.type);
    if (!is_dimension && glyph == nullptr) {
      continue;
    }
    const float2 at = sketch_constraint_label_position(region, frame, sketch, i);
    const float4 color = constraint_color(c, is_dimension);
    draw_text_box(font, is_dimension ? dimension_text(scene, c) : glyph, at, color, box);
  }

  if (!hover.preview.hint.empty()) {
    draw_text_box(font,
                  hover.preview.hint,
                  hover.preview.hint_position,
                  COLOR_PREVIEW,
                  float4(0.0f));
  }

  /* Solve state: what the user most needs while sketching. */
  std::string status;
  float4 color = COLOR_FREE;
  if (sketch->solve_status == CAD_SKETCH_SOLVE_CONFLICTING) {
    status = "Conflicting constraints";
    color = COLOR_CONFLICT;
  }
  else if (sketch->solve_status == CAD_SKETCH_SOLVE_FAILED) {
    status = "Solver failed";
    color = COLOR_CONFLICT;
  }
  else if (sketch->dof == 0) {
    status = "Fully constrained";
    color = COLOR_CONSTRAINED;
  }
  else if (sketch->dof > 0) {
    status = fmt::format("{} degree{} of freedom", sketch->dof, sketch->dof == 1 ? "" : "s");
  }
  if (!status.empty()) {
    const std::string text = std::string(edit->feature->name) + "  \xc2\xb7  " + status;
    /* Bottom center: the corner holds the last operator's panel. */
    draw_text_box(
        font, text, float2(region->winx * 0.5f, 24.0f * UI_SCALE_FAC), color, box);
  }
  GPU_blend(GPU_BLEND_NONE);
}

void sketch_draw_init()
{
  static bool initialized = false;
  if (initialized) {
    return;
  }
  initialized = true;
  SpaceType *st = BKE_spacetype_from_id(SPACE_VIEW3D);
  ARegionType *art = st ? BKE_regiontype_from_id(st, RGN_TYPE_WINDOW) : nullptr;
  if (art == nullptr) {
    return;
  }
  ED_region_draw_cb_activate(art, draw_geometry, nullptr, REGION_DRAW_POST_VIEW);
  ED_region_draw_cb_activate(art, draw_labels, nullptr, REGION_DRAW_POST_PIXEL);
}

}  // namespace blender::ed::cad
