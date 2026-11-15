/* SPDX-FileCopyrightText: 2026 SimpleCAD Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editors
 *
 * Parametric CAD part editing: feature timeline operators, picking, sketch mode.
 */

#pragma once

#include "DNA_listBase.h"

namespace blender {
struct ARegion;
struct ARegionType;
struct bContext;
struct Main;
struct WorkSpace;
struct UndoType;
struct wmKeyConfig;
struct Object;
struct Scene;
}  // namespace blender

namespace blender::ed::cad {

void operatortypes_cad();
void keymap_cad(wmKeyConfig *keyconf);
/** Gizmo types and groups; call after the editors' gizmo types are registered. */
void gizmotypes_cad();

/**
 * Sketch mode is the edit mode of CAD parts. Entering picks #CADPart.edit_sketch_uid (or the
 * active sketch feature, or a new sketch) and rolls the model back to it; exiting restores the
 * rollback marker. Called from object edit mode enter / exit.
 */
bool sketch_edit_enter(Main *bmain, Scene *scene, Object *ob);
void sketch_edit_exit(Main *bmain, Object *ob);

/**
 * Undo while a feature dialog is open cancels the dialog instead (its changes are not undo
 * steps). Returns true when a dialog was cancelled.
 */
bool dialog_cancel_for_undo(bContext *C);

/**
 * The History strip: the #RGN_TYPE_FOOTER region of 3D views, shown in CAD workspaces
 * (#WORKSPACE_USE_CAD_UI).
 */
void history_region_type_init(ARegionType *art);
/** Adds the History region to a 3D view's region list (new 3D views). */
void history_region_add(ListBaseT<ARegion> *regions);
/** Turns the CAD shell of a workspace on or off, adding the regions it needs. */
void workspace_cad_ui_set(WorkSpace *workspace, bool enable);

/** Undo for sketch mode (edit modes do not use memfile undo). */
void undosys_type_register(UndoType *ut);

}  // namespace blender::ed::cad
