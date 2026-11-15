/* SPDX-FileCopyrightText: 2026 SimpleCAD Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edcad
 *
 * Undo for sketch mode. Memfile undo is not used in edit modes, so every sketch mode step stores
 * a copy of the edited sketch (small: points, curves and constraints) and the part's uid counter.
 */

#include "DNA_cad_types.h"
#include "DNA_object_types.h"

#include "BKE_cad.hh"
#include "BKE_context.hh"
#include "BKE_library.hh"
#include "BKE_main.hh"
#include "BKE_object.hh"
#include "BKE_undo_system.hh"

#include "CLG_log.h"

#include "DEG_depsgraph.hh"

#include "ED_undo.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "ED_cad.hh"

#include "cad_sketch.hh"

namespace blender {

static CLG_LogRef LOG = {"undo.cad"};

namespace ed::cad {

struct SketchUndoStep {
  UndoStep step;
  /** See #ED_undo_object_editmode_validate_scene_from_windows code comment for details. */
  UndoRefID_Scene scene_ref;
  UndoRefID_Object obedit_ref;
  CADSketch *sketch;
  int sketch_uid;
  int next_uid;
};

static bool undo_poll(bContext *C)
{
  const std::optional<SketchEdit> edit = sketch_edit_get(C);
  return edit && ID_IS_EDITABLE(&edit->part->id);
}

static bool undo_encode(bContext *C, Main *bmain, UndoStep *us_p)
{
  SketchUndoStep *us = reinterpret_cast<SketchUndoStep *>(us_p);
  const std::optional<SketchEdit> edit = sketch_edit_get(C);
  if (!edit) {
    return false;
  }
  us->scene_ref.ptr = CTX_data_scene(C);
  us->obedit_ref.ptr = edit->ob;
  us->sketch = BKE_cad_sketch_copy(edit->sketch);
  us->sketch_uid = edit->feature->uid;
  us->next_uid = edit->part->next_uid;
  bmain->is_memfile_undo_flush_needed = true;
  return true;
}

static void undo_decode(
    bContext *C, Main *bmain, UndoStep *us_p, const eUndoStepDir /*dir*/, bool /*is_final*/)
{
  SketchUndoStep *us = reinterpret_cast<SketchUndoStep *>(us_p);
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);

  ED_undo_object_editmode_validate_scene_from_windows(
      CTX_wm_manager(C), us->scene_ref.ptr, &scene, &view_layer);
  ED_undo_object_editmode_restore_helper(
      scene, view_layer, &us->obedit_ref.ptr, 1, sizeof(UndoRefID_Object));

  Object *ob = us->obedit_ref.ptr;
  CADPart *part = id_cast<CADPart *>(ob->data);
  if (CADFeature *feature = BKE_cad_feature_find(part, us->sketch_uid)) {
    BKE_cad_sketch_free(feature->sketch);
    feature->sketch = BKE_cad_sketch_copy(us->sketch);
    part->edit_sketch_uid = us->sketch_uid;
    part->next_uid = std::max(part->next_uid, us->next_uid);
  }
  sketch_hover_clear();
  DEG_id_tag_update(&part->id, ID_RECALC_GEOMETRY);

  ED_undo_object_set_active_or_warn(*bmain, scene, view_layer, ob, us_p->name, &LOG);
  bmain->is_memfile_undo_flush_needed = true;
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, nullptr);
}

static void undo_free(UndoStep *us_p)
{
  SketchUndoStep *us = reinterpret_cast<SketchUndoStep *>(us_p);
  BKE_cad_sketch_free(us->sketch);
  us->sketch = nullptr;
}

static void undo_foreach_ID_ref(UndoStep *us_p,
                                UndoTypeForEachIDRefFn foreach_ID_ref_fn,
                                void *user_data)
{
  SketchUndoStep *us = reinterpret_cast<SketchUndoStep *>(us_p);
  foreach_ID_ref_fn(user_data, reinterpret_cast<UndoRefID *>(&us->scene_ref));
  foreach_ID_ref_fn(user_data, reinterpret_cast<UndoRefID *>(&us->obedit_ref));
}

void undosys_type_register(UndoType *ut)
{
  ut->identifier = "EDIT_CAD";
  ut->poll = undo_poll;
  ut->step_encode = undo_encode;
  ut->step_decode = undo_decode;
  ut->step_free = undo_free;
  ut->step_foreach_ID_ref = undo_foreach_ID_ref;
  ut->flags = UNDOTYPE_FLAG_NEED_CONTEXT_FOR_ENCODE | UNDOTYPE_FLAG_ENCODE_PRE_MEMFILE_SUPPORTED;
  ut->step_size = sizeof(SketchUndoStep);
}

}  // namespace ed::cad
}  // namespace blender
