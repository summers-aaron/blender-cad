/* SPDX-FileCopyrightText: 2026 SimpleCAD Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edcad
 *
 * CAD workspaces and the History strip: a region at the bottom of the 3D views of CAD
 * workspaces that shows the feature timeline of the active part as a row of chips, with the
 * rollback marker after the last feature that is built.
 *
 * Click a chip to make it active, double-click to edit it, right-click for its menu, drag the
 * marker to roll the model back, use the wheel to scroll.
 */

#include <fmt/format.h>

#include "DNA_cad_types.h"
#include "DNA_object_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_userdef_types.h"
#include "DNA_workspace_types.h"

#include "BLI_listbase.hh"
#include "BLI_listbase_iterator.hh"
#include "BLI_rect.hh"
#include "BLI_string.hh"
#include "BLI_vector.hh"

#include "BKE_cad.hh"
#include "BKE_context.hh"
#include "BKE_lib_id.hh"
#include "BKE_library.hh"
#include "BKE_main.hh"
#include "BKE_screen.hh"
#include "BKE_workspace.hh"

#include "BLT_translation.hh"

#include "DEG_depsgraph.hh"

#include "GPU_immediate.hh"
#include "GPU_state.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "UI_interface_c.hh"
#include "UI_interface_icons.hh"
#include "UI_resources.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "ED_cad.hh"
#include "ED_screen.hh"

#include "cad_intern.hh"

namespace blender::ed::cad {

/* -------------------------------------------------------------------- */
/** \name Layout
 * \{ */

struct HistoryChip {
  const CADFeature *feature = nullptr;
  rctf rect;
};

struct HistoryLayout {
  Vector<HistoryChip> chips;
  /** Where the rollback marker is drawn. */
  float marker_x = 0.0f;
  /** Width of all chips, before scrolling. */
  float content_width = 0.0f;
};

static float history_margin()
{
  return 6.0f * UI_SCALE_FAC;
}

static float history_gap()
{
  return 6.0f * UI_SCALE_FAC;
}

static float history_icon_size()
{
  return 16.0f * UI_SCALE_FAC;
}

static int feature_icon(const CADFeature &feature)
{
  switch (eCADFeatureType(feature.type)) {
    case CAD_FEATURE_SKETCH:
      return ICON_GREASEPENCIL;
    case CAD_FEATURE_BOX:
      return ICON_MESH_CUBE;
    case CAD_FEATURE_EXTRUDE:
      return ICON_MOD_SOLIDIFY;
    case CAD_FEATURE_FILLET:
    case CAD_FEATURE_CHAMFER:
      return ICON_MOD_BEVEL;
  }
  return ICON_DOT;
}

/** The name, shortened with "..." to fit `max_width`. */
static std::string fit_text(const uiFontStyle *fstyle, const char *text, const float max_width)
{
  std::string fitted = text;
  if (ui::fontstyle_string_width(fstyle, fitted.c_str()) <= max_width) {
    return fitted;
  }
  while (!fitted.empty()) {
    fitted.pop_back();
    const std::string candidate = fitted + "...";
    if (ui::fontstyle_string_width(fstyle, candidate.c_str()) <= max_width) {
      return candidate;
    }
  }
  return "...";
}

static float chip_name_max_width()
{
  return 7.0f * UI_UNIT_X;
}

static float chip_width(const uiFontStyle *fstyle, const CADFeature &feature, const bool badge)
{
  const float pad = 6.0f * UI_SCALE_FAC;
  const std::string name = fit_text(fstyle, feature.name, chip_name_max_width());
  float width = pad + history_icon_size() + pad * 0.75f +
                ui::fontstyle_string_width(fstyle, name.c_str()) + pad;
  if (badge) {
    width += history_icon_size() + pad * 0.5f;
  }
  return width;
}

/** The evaluation status of a feature (`cad::FeatureStatus`): 1 warning, 2 error. */
static int feature_status(const CADPart *part, const CADFeature &feature)
{
  const bke::CADFeatureStatus *status = BKE_cad_feature_status(part, feature.uid);
  return status ? status->status : -1;
}

static bool feature_has_badge(const CADPart *part, const CADFeature &feature)
{
  const int status = feature_status(part, feature);
  return status == 1 || status == 2;
}

static float history_scroll(const ARegion *region)
{
  /* The region has no 2D view of its own: the scroll offset lives in it. */
  return region->v2d.cur.xmin;
}

static HistoryLayout history_layout(const ARegion *region, const CADPart *part)
{
  HistoryLayout layout;
  const uiFontStyle *fstyle = UI_FSTYLE_WIDGET;
  const float margin = history_margin();
  const float scroll = history_scroll(region);
  float x = margin;
  const CADFeature *marker = BKE_cad_feature_find(part, part->rollback_uid);
  for (const CADFeature &feature : part->features) {
    HistoryChip chip;
    chip.feature = &feature;
    const float width = chip_width(fstyle, feature, feature_has_badge(part, feature));
    BLI_rctf_init(&chip.rect, x - scroll, x - scroll + width, margin, region->winy - margin);
    layout.chips.append(chip);
    x += width + history_gap();
    if (&feature == marker) {
      layout.marker_x = x - scroll - history_gap() * 0.5f;
    }
  }
  if (marker == nullptr) {
    layout.marker_x = x - scroll - history_gap() * 0.5f;
  }
  layout.content_width = x;
  return layout;
}

static void history_scroll_clamp(ARegion *region, const CADPart *part)
{
  const HistoryLayout layout = history_layout(region, part);
  const float max_scroll = std::max(0.0f, layout.content_width - float(region->winx));
  region->v2d.cur.xmin = std::clamp(region->v2d.cur.xmin, 0.0f, max_scroll);
}

enum class HistoryHitType { None, Chip, Marker };

struct HistoryHit {
  HistoryHitType type = HistoryHitType::None;
  int uid = -1;
};

static HistoryHit history_hit(const ARegion *region, const CADPart *part, const int2 &mval)
{
  const HistoryLayout layout = history_layout(region, part);
  HistoryHit hit;
  if (std::abs(float(mval.x) - layout.marker_x) <= 5.0f * UI_SCALE_FAC) {
    hit.type = HistoryHitType::Marker;
    return hit;
  }
  for (const HistoryChip &chip : layout.chips) {
    if (BLI_rctf_isect_pt(&chip.rect, float(mval.x), float(mval.y))) {
      hit.type = HistoryHitType::Chip;
      hit.uid = chip.feature->uid;
      return hit;
    }
  }
  return hit;
}

/** The part whose history is shown: the active object's. */
static CADPart *history_part(const bContext *C)
{
  Object *ob = CTX_data_active_object(C);
  if (ob == nullptr || ob->type != OB_CAD || ob->data == nullptr) {
    return nullptr;
  }
  return id_cast<CADPart *>(ob->data);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Drawing
 * \{ */

static void draw_text(const float x, const float y, const char *text, const int colorid)
{
  uchar color[4];
  ui::theme::get_color_4ubv(colorid, color);
  ui::fontstyle_draw_simple(UI_FSTYLE_WIDGET, x, y, text, color);
}

static float text_baseline(const rctf &rect)
{
  const uiFontStyle *fstyle = UI_FSTYLE_WIDGET;
  return BLI_rctf_cent_y(&rect) - fstyle->points * UI_SCALE_FAC * 0.38f;
}

static void draw_chip(const CADPart *part,
                      const HistoryChip &chip,
                      const bool is_rolled_back,
                      const bool is_hovered)
{
  const CADFeature &feature = *chip.feature;
  const bool is_active = feature.uid == part->active_feature_uid;
  const bool is_dialog = feature.uid == part->dialog_uid;
  const bool is_suppressed = (feature.flag & CAD_FEATURE_SUPPRESSED) != 0;
  const float alpha = (is_suppressed || is_rolled_back) ? 0.45f : 1.0f;
  const float pad = 6.0f * UI_SCALE_FAC;

  float fill[4], outline[4];
  ui::theme::get_color_shade_4fv(TH_HEADER, is_hovered ? 28 : 16, fill);
  fill[3] *= alpha;
  if (is_dialog) {
    ui::theme::get_color_4fv(TH_GIZMO_PRIMARY, outline);
  }
  else if (is_active) {
    ui::theme::get_color_4fv(TH_TEXT_HI, outline);
  }
  else {
    ui::theme::get_color_shade_4fv(TH_HEADER, 32, outline);
  }
  outline[3] *= alpha;
  ui::draw_roundbox_corner_set(ui::CNR_ALL);
  ui::draw_roundbox_4fv_ex(&chip.rect,
                           fill,
                           nullptr,
                           0.0f,
                           outline,
                           (is_active || is_dialog) ? 1.5f * UI_SCALE_FAC : UI_SCALE_FAC,
                           4.0f * UI_SCALE_FAC);

  float x = chip.rect.xmin + pad;
  const float icon_y = BLI_rctf_cent_y(&chip.rect) - history_icon_size() * 0.5f;
  GPU_blend(GPU_BLEND_ALPHA);
  ui::icon_draw_alpha(x, icon_y, feature_icon(feature), alpha);
  x += history_icon_size() + pad * 0.75f;

  const uiFontStyle *fstyle = UI_FSTYLE_WIDGET;
  const std::string name = fit_text(fstyle, feature.name, chip_name_max_width());
  uchar text_color[4];
  ui::theme::get_color_4ubv(is_active ? TH_TEXT_HI : TH_TEXT, text_color);
  text_color[3] = uchar(255.0f * alpha);
  ui::fontstyle_draw_simple(fstyle, x, text_baseline(chip.rect), name.c_str(), text_color);
  const float text_width = ui::fontstyle_string_width(fstyle, name.c_str());

  const int status = feature_status(part, feature);
  if (status == 1 || status == 2) {
    ui::icon_draw_alpha(x + text_width + pad * 0.5f,
                        icon_y,
                        status == 2 ? ICON_CANCEL : ICON_ERROR,
                        alpha);
  }
  GPU_blend(GPU_BLEND_NONE);

  if (is_suppressed) {
    /* Struck through. */
    float color[4];
    ui::theme::get_color_4fv(TH_TEXT, color);
    color[3] = 0.6f;
    GPU_blend(GPU_BLEND_ALPHA);
    GPUVertFormat *format = immVertexFormat();
    const uint pos = GPU_vertformat_attr_add(
        format, "pos", gpu::VertAttrType::SFLOAT_32_32);
    immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
    immUniformColor4fv(color);
    immBegin(GPU_PRIM_LINES, 2);
    immVertex2f(pos, chip.rect.xmin + pad, BLI_rctf_cent_y(&chip.rect));
    immVertex2f(pos, chip.rect.xmax - pad, BLI_rctf_cent_y(&chip.rect));
    immEnd();
    immUnbindProgram();
    GPU_blend(GPU_BLEND_NONE);
  }
}

static void draw_marker(const ARegion *region, const float x)
{
  float color[4];
  ui::theme::get_color_4fv(TH_CFRAME, color);
  const float half = 4.0f * UI_SCALE_FAC;
  const float top = float(region->winy) - 2.0f * UI_SCALE_FAC;

  GPU_blend(GPU_BLEND_ALPHA);
  GPUVertFormat *format = immVertexFormat();
  const uint pos = GPU_vertformat_attr_add(format, "pos", gpu::VertAttrType::SFLOAT_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
  immUniformColor4fv(color);
  immRectf(pos, x - UI_SCALE_FAC, 2.0f * UI_SCALE_FAC, x + UI_SCALE_FAC, top);
  /* Handle at the top: a downwards triangle. */
  immBegin(GPU_PRIM_TRIS, 3);
  immVertex2f(pos, x - half, top);
  immVertex2f(pos, x + half, top);
  immVertex2f(pos, x, top - half * 1.5f);
  immEnd();
  immUnbindProgram();
  GPU_blend(GPU_BLEND_NONE);
}

static void history_region_draw(const bContext *C, ARegion *region)
{
  ui::theme::frame_buffer_clear(TH_HEADER);
  wmOrtho2_region_pixelspace(region);

  const CADPart *part = history_part(C);
  rctf full;
  BLI_rctf_init(&full, 0.0f, float(region->winx), 0.0f, float(region->winy));
  if (part == nullptr) {
    draw_text(history_margin() * 2.0f,
              text_baseline(full),
              IFACE_("History: select a CAD part to see its features"),
              TH_TEXT);
    return;
  }
  if (BLI_listbase_is_empty(&part->features)) {
    draw_text(history_margin() * 2.0f,
              text_baseline(full),
              IFACE_("History: no features yet (add a sketch or a box)"),
              TH_TEXT);
    return;
  }

  const HistoryLayout layout = history_layout(region, part);
  int2 mval(-1, -1);
  if (const wmWindow *win = CTX_wm_window(C)) {
    mval = int2(win->runtime->eventstate->xy) - int2(region->winrct.xmin, region->winrct.ymin);
  }
  const CADFeature *marker = BKE_cad_feature_find(part, part->rollback_uid);
  bool rolled_back = false;
  const HistoryChip *hovered = nullptr;
  for (const HistoryChip &chip : layout.chips) {
    const bool is_hovered = BLI_rctf_isect_pt(&chip.rect, float(mval.x), float(mval.y));
    if (is_hovered) {
      hovered = &chip;
    }
    draw_chip(part, chip, rolled_back, is_hovered);
    if (chip.feature == marker) {
      rolled_back = true;
    }
  }
  draw_marker(region, layout.marker_x);

  /* What the hovered feature is and how it went. */
  if (hovered && layout.content_width < region->winx - 20 * UI_UNIT_X) {
    const CADFeature &feature = *hovered->feature;
    const bke::CADFeatureStatus *status = BKE_cad_feature_status(part, feature.uid);
    std::string info = fmt::format(
        "{}  ({})", feature.name, BKE_cad_feature_type_name(eCADFeatureType(feature.type)));
    if (status && !status->message.empty()) {
      info += ":  " + status->message;
    }
    draw_text(layout.content_width + 2.0f * UI_UNIT_X,
              text_baseline(full),
              info.c_str(),
              (status && status->status == 2) ? TH_REDALERT : TH_TEXT);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Region Type
 * \{ */

static bool history_region_poll(const RegionPollParams *params)
{
  const WorkSpace *workspace = params->context ? CTX_wm_workspace(params->context) : nullptr;
  return workspace && (workspace->flags & WORKSPACE_USE_CAD_UI);
}

static void history_region_init(wmWindowManager *wm, ARegion *region)
{
  wmKeyMap *keymap = WM_keymap_ensure(
      wm->runtime->defaultconf, "CAD History", SPACE_VIEW3D, RGN_TYPE_FOOTER);
  WM_event_add_keymap_handler(&region->runtime->handlers, keymap);
}

static void history_region_listener(const wmRegionListenerParams *params)
{
  const wmNotifier *wmn = params->notifier;
  switch (wmn->category) {
    case NC_GEOM:
    case NC_OBJECT:
    case NC_SCENE:
    case NC_WM:
    case NC_SCREEN:
      ED_region_tag_redraw(params->region);
      break;
    default:
      break;
  }
}

void history_region_type_init(ARegionType *art)
{
  art->regionid = RGN_TYPE_FOOTER;
  art->prefsizey = 34;
  art->keymapflag = ED_KEYMAP_UI | ED_KEYMAP_FRAMES;
  art->poll = history_region_poll;
  art->init = history_region_init;
  art->draw = history_region_draw;
  art->listener = history_region_listener;
}

static ARegion *find_region(ListBaseT<ARegion> &regions, const int regiontype)
{
  for (ARegion &region : regions) {
    if (region.regiontype == regiontype) {
      return &region;
    }
  }
  return nullptr;
}

void history_region_add(ListBaseT<ARegion> *regions)
{
  if (find_region(*regions, RGN_TYPE_FOOTER)) {
    return;
  }
  ARegion *region = BKE_area_region_new();
  region->regiontype = RGN_TYPE_FOOTER;
  region->alignment = RGN_ALIGN_BOTTOM;
  /* Along the bottom edge, below the asset shelf: before it in the list. */
  ARegion *before = find_region(*regions, RGN_TYPE_ASSET_SHELF);
  if (before == nullptr) {
    before = find_region(*regions, RGN_TYPE_TOOLS);
  }
  if (before) {
    BLI_insertlinkbefore(regions, before, region);
  }
  else {
    BLI_addhead(regions, region);
  }
}

/** Adds the History region to the 3D views of an area that lack it (older files). */
static void history_region_ensure(ScrArea *area)
{
  for (SpaceLink &sl : area->spacedata) {
    if (sl.spacetype != SPACE_VIEW3D) {
      continue;
    }
    /* The active space's regions live in the area, inactive spaces keep their own. */
    history_region_add(&sl == area->spacedata.first() ? &area->regionbase : &sl.regionbase);
  }
}

void workspace_cad_ui_set(WorkSpace *workspace, const bool enable)
{
  SET_FLAG_FROM_TEST(workspace->flags, enable, WORKSPACE_USE_CAD_UI);
  if (!enable) {
    return;
  }
  for (WorkSpaceLayout &layout : workspace->layouts) {
    bScreen *screen = BKE_workspace_layout_screen_get(&layout);
    for (ScrArea &area : screen->areabase) {
      history_region_ensure(&area);
    }
    screen->do_refresh = true;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Operators
 * \{ */

static bool history_poll(bContext *C)
{
  const ARegion *region = CTX_wm_region(C);
  const ScrArea *area = CTX_wm_area(C);
  return region && area && region->regiontype == RGN_TYPE_FOOTER &&
         area->spacetype == SPACE_VIEW3D && history_part(C) != nullptr;
}

static bool history_edit_poll(bContext *C)
{
  if (!history_poll(C)) {
    return false;
  }
  const CADPart *part = history_part(C);
  return ID_IS_EDITABLE(&part->id) && !ID_IS_OVERRIDE_LIBRARY(&part->id);
}

static void prop_location_def(wmOperatorType *ot)
{
  PropertyRNA *prop = RNA_def_int_array(ot->srna,
                                        "location",
                                        2,
                                        nullptr,
                                        INT_MIN,
                                        INT_MAX,
                                        "Location",
                                        "Position in the region",
                                        INT_MIN,
                                        INT_MAX);
  RNA_def_property_flag(prop, PropertyFlag(PROP_HIDDEN | PROP_SKIP_SAVE));
}

static HistoryHit history_hit_from_op(bContext *C, wmOperator *op)
{
  int2 mval;
  RNA_int_get_array(op->ptr, "location", mval);
  return history_hit(CTX_wm_region(C), history_part(C), mval);
}

/* Select. */

static wmOperatorStatus history_select_exec(bContext *C, wmOperator *op)
{
  const HistoryHit hit = history_hit_from_op(C, op);
  if (hit.type != HistoryHitType::Chip) {
    return OPERATOR_CANCELLED | OPERATOR_PASS_THROUGH;
  }
  CADPart *part = history_part(C);
  part->active_feature_uid = hit.uid;
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, part);
  ED_region_tag_redraw(CTX_wm_region(C));
  return OPERATOR_FINISHED;
}

static wmOperatorStatus history_select_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  RNA_int_set_array(op->ptr, "location", event->mval);
  return history_select_exec(C, op);
}

static void CAD_OT_history_select(wmOperatorType *ot)
{
  ot->name = "Select Feature";
  ot->description = "Make the feature under the cursor the active feature";
  ot->idname = "CAD_OT_history_select";

  ot->invoke = history_select_invoke;
  ot->exec = history_select_exec;
  ot->poll = history_poll;

  ot->flag = OPTYPE_INTERNAL;

  prop_location_def(ot);
}

/* Edit. */

static wmOperatorStatus history_edit_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  RNA_int_set_array(op->ptr, "location", event->mval);
  const HistoryHit hit = history_hit_from_op(C, op);
  if (hit.type != HistoryHitType::Chip) {
    return OPERATOR_CANCELLED | OPERATOR_PASS_THROUGH;
  }
  history_part(C)->active_feature_uid = hit.uid;
  wmOperatorType *ot = WM_operatortype_find("CAD_OT_feature_edit", false);
  PointerRNA ptr = WM_operator_properties_create_ptr(ot);
  RNA_int_set(&ptr, "feature_uid", hit.uid);
  WM_operator_name_call_ptr(C, ot, wm::OpCallContext::InvokeDefault, &ptr, nullptr);
  WM_operator_properties_free(&ptr);
  return OPERATOR_FINISHED;
}

static void CAD_OT_history_edit(wmOperatorType *ot)
{
  ot->name = "Edit Feature";
  ot->description = "Edit the feature under the cursor (sketches open in sketch mode)";
  ot->idname = "CAD_OT_history_edit";

  ot->invoke = history_edit_invoke;
  ot->poll = history_edit_poll;

  ot->flag = OPTYPE_INTERNAL;

  prop_location_def(ot);
}

/* Context menu. */

static wmOperatorStatus history_menu_invoke(bContext *C, wmOperator * /*op*/, const wmEvent *event)
{
  CADPart *part = history_part(C);
  const HistoryHit hit = history_hit(CTX_wm_region(C), part, int2(event->mval));
  if (hit.type != HistoryHitType::Chip) {
    return OPERATOR_CANCELLED | OPERATOR_PASS_THROUGH;
  }
  part->active_feature_uid = hit.uid;
  ED_region_tag_redraw(CTX_wm_region(C));
  WM_menu_name_call(C, "CAD_MT_history_context", wm::OpCallContext::InvokeDefault);
  return OPERATOR_FINISHED;
}

static void CAD_OT_history_menu(wmOperatorType *ot)
{
  ot->name = "Feature Menu";
  ot->description = "Show the menu of the feature under the cursor";
  ot->idname = "CAD_OT_history_menu";

  ot->invoke = history_menu_invoke;
  ot->poll = history_poll;

  ot->flag = OPTYPE_INTERNAL;
}

/* Rollback marker drag. */

struct RollbackDragData {
  int rollback_uid_orig = -1;
};

/** The marker position closest to `x`: after the chip it follows (-1 after the last). */
static int rollback_uid_at(const ARegion *region, const CADPart *part, const float x)
{
  const HistoryLayout layout = history_layout(region, part);
  int best_uid = -1;
  float best_dist = FLT_MAX;
  for (const int i : layout.chips.index_range()) {
    const float slot_x = layout.chips[i].rect.xmax + history_gap() * 0.5f;
    const float dist = std::abs(slot_x - x);
    if (dist < best_dist) {
      best_dist = dist;
      best_uid = (i + 1 == layout.chips.size()) ? -1 : layout.chips[i].feature->uid;
    }
  }
  return best_uid;
}

static wmOperatorStatus rollback_drag_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  ARegion *region = CTX_wm_region(C);
  CADPart *part = history_part(C);
  int2 start;
  WM_event_drag_start_mval(event, region, start);
  if (history_hit(region, part, start).type != HistoryHitType::Marker) {
    return OPERATOR_CANCELLED | OPERATOR_PASS_THROUGH;
  }
  RollbackDragData *data = MEM_new<RollbackDragData>(__func__);
  data->rollback_uid_orig = part->rollback_uid;
  op->customdata = data;
  WM_event_add_modal_handler(C, op);
  return OPERATOR_RUNNING_MODAL;
}

static void rollback_drag_set(bContext *C, CADPart *part, const int uid)
{
  if (part->rollback_uid != uid) {
    part->rollback_uid = uid;
    part_changed(C, part);
  }
  ED_region_tag_redraw(CTX_wm_region(C));
}

static wmOperatorStatus rollback_drag_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  RollbackDragData *data = static_cast<RollbackDragData *>(op->customdata);
  CADPart *part = history_part(C);
  if (part == nullptr) {
    MEM_delete(data);
    return OPERATOR_CANCELLED;
  }
  switch (event->type) {
    case MOUSEMOVE:
      rollback_drag_set(C, part, rollback_uid_at(CTX_wm_region(C), part, event->mval[0]));
      break;
    case LEFTMOUSE:
      if (event->val == KM_RELEASE) {
        const bool changed = part->rollback_uid != data->rollback_uid_orig;
        MEM_delete(data);
        return changed ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
      }
      break;
    case EVT_ESCKEY:
    case RIGHTMOUSE:
      if (event->val == KM_PRESS) {
        rollback_drag_set(C, part, data->rollback_uid_orig);
        MEM_delete(data);
        return OPERATOR_CANCELLED;
      }
      break;
    default:
      break;
  }
  return OPERATOR_RUNNING_MODAL;
}

static void rollback_drag_cancel(bContext *C, wmOperator *op)
{
  RollbackDragData *data = static_cast<RollbackDragData *>(op->customdata);
  if (CADPart *part = history_part(C)) {
    rollback_drag_set(C, part, data->rollback_uid_orig);
  }
  MEM_delete(data);
}

static void CAD_OT_history_rollback_drag(wmOperatorType *ot)
{
  ot->name = "Move Rollback Marker";
  ot->description = "Drag the rollback marker: features after it are not built";
  ot->idname = "CAD_OT_history_rollback_drag";

  ot->invoke = rollback_drag_invoke;
  ot->modal = rollback_drag_modal;
  ot->cancel = rollback_drag_cancel;
  ot->poll = history_edit_poll;

  ot->flag = OPTYPE_UNDO | OPTYPE_BLOCKING;
}

/* Scroll and hover. */

static wmOperatorStatus history_scroll_exec(bContext *C, wmOperator *op)
{
  ARegion *region = CTX_wm_region(C);
  region->v2d.cur.xmin += float(RNA_int_get(op->ptr, "delta")) * 3.0f * UI_UNIT_X;
  history_scroll_clamp(region, history_part(C));
  ED_region_tag_redraw(region);
  return OPERATOR_FINISHED;
}

static void CAD_OT_history_scroll(wmOperatorType *ot)
{
  ot->name = "Scroll History";
  ot->description = "Scroll the feature history";
  ot->idname = "CAD_OT_history_scroll";

  ot->exec = history_scroll_exec;
  ot->poll = history_poll;

  ot->flag = OPTYPE_INTERNAL;

  RNA_def_int(ot->srna, "delta", 1, -100, 100, "Delta", "", -100, 100);
}

static wmOperatorStatus history_hover_invoke(bContext *C,
                                             wmOperator * /*op*/,
                                             const wmEvent * /*event*/)
{
  /* Hover highlights are drawn from the mouse position. */
  ED_region_tag_redraw(CTX_wm_region(C));
  return OPERATOR_CANCELLED | OPERATOR_PASS_THROUGH;
}

static void CAD_OT_history_hover(wmOperatorType *ot)
{
  ot->name = "History Hover";
  ot->description = "Redraw hover highlights of the feature history";
  ot->idname = "CAD_OT_history_hover";

  ot->invoke = history_hover_invoke;
  ot->poll = history_poll;

  ot->flag = OPTYPE_INTERNAL;
}

/* CAD workspace. */

static wmOperatorStatus workspace_add_exec(bContext *C, wmOperator * /*op*/)
{
  Main *bmain = CTX_data_main(C);
  wmWindow *win = CTX_wm_window(C);
  WorkSpace *workspace = ED_workspace_duplicate(CTX_wm_workspace(C), bmain, win);
  BKE_libblock_rename(*bmain, workspace->id, "CAD");
  workspace->object_mode = OB_MODE_OBJECT;
  workspace_cad_ui_set(workspace, true);
  WM_event_add_notifier(C, NC_SCREEN | ND_WORKSPACE_SET, workspace);
  return OPERATOR_FINISHED;
}

static void CAD_OT_workspace_add(wmOperatorType *ot)
{
  ot->name = "Add CAD Workspace";
  ot->description =
      "Add a workspace for CAD parts: its 3D views show the feature history of the active part";
  ot->idname = "CAD_OT_workspace_add";

  ot->exec = workspace_add_exec;
  ot->poll = WM_operator_winactive;

  ot->flag = 0;
}

void history_operatortypes()
{
  WM_operatortype_append(CAD_OT_history_select);
  WM_operatortype_append(CAD_OT_history_edit);
  WM_operatortype_append(CAD_OT_history_menu);
  WM_operatortype_append(CAD_OT_history_rollback_drag);
  WM_operatortype_append(CAD_OT_history_scroll);
  WM_operatortype_append(CAD_OT_history_hover);
  WM_operatortype_append(CAD_OT_workspace_add);
}

void history_keymap(wmKeyConfig *keyconf)
{
  WM_keymap_ensure(keyconf, "CAD History", SPACE_VIEW3D, RGN_TYPE_FOOTER);
}

/** \} */

}  // namespace blender::ed::cad
