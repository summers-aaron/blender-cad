/* SPDX-FileCopyrightText: 2026 SimpleCAD Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editor/io
 *
 * STEP export of CAD parts. The exact B-rep solids are written (in millimeters), not the
 * tessellated mesh, so Blender modifiers on top of a part are not part of the file.
 */

#include "BKE_cad.hh"
#include "BKE_context.hh"
#include "BKE_file_handler.hh"
#include "BKE_layer.hh"
#include "BKE_modifier.hh"
#include "BKE_report.hh"

#include "BLI_listbase_iterator.hh"
#include "BLI_path_utils.hh"
#include "BLI_string.hh"
#include "BLI_string_utf8.hh"
#include "BLI_vector.hh"

#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_space_types.h"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"

#include "ED_fileselect.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "BLT_translation.hh"

#include "UI_interface.hh"
#include "UI_interface_layout.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "io_step_ops.hh"

namespace blender {

static bool wm_step_export_poll(bContext *C)
{
  if (CTX_data_scene(C) == nullptr) {
    return false;
  }
  if (!BKE_cad_step_export_supported()) {
    CTX_wm_operator_poll_msg_set(C, "STEP export needs the OpenCASCADE CAD kernel");
    return false;
  }
  return true;
}

static wmOperatorStatus wm_step_export_invoke(bContext *C,
                                              wmOperator *op,
                                              const wmEvent * /*event*/)
{
  ED_fileselect_ensure_default_filepath(C, op, ".step");
  WM_event_add_fileselect(C, op);
  return OPERATOR_RUNNING_MODAL;
}

static bool object_has_active_modifiers(const Scene *scene, const Object *object)
{
  for (const ModifierData &md : object->modifiers) {
    if (BKE_modifier_is_enabled(scene, const_cast<ModifierData *>(&md), eModifierMode_Realtime)) {
      return true;
    }
  }
  return false;
}

static wmOperatorStatus wm_step_export_exec(bContext *C, wmOperator *op)
{
  if (!RNA_struct_property_is_set_ex(op->ptr, "filepath", false)) {
    BKE_report(op->reports, RPT_ERROR, "No filename given");
    return OPERATOR_CANCELLED;
  }
  char filepath[FILE_MAX];
  RNA_string_get(op->ptr, "filepath", filepath);
  const bool selected_only = RNA_boolean_get(op->ptr, "export_selected_objects");
  const bool apply_transform = RNA_boolean_get(op->ptr, "apply_transform");

  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);

  Vector<const Object *> objects_eval;
  bool has_modifiers = false;
  BKE_view_layer_synced_ensure(*CTX_data_main(C), scene, view_layer);
  for (Base &base : *BKE_view_layer_object_bases_get(view_layer)) {
    Object *object = base.object;
    if (object->type != OB_CAD) {
      continue;
    }
    if (selected_only ? !(base.flag & BASE_SELECTED) :
                        !(base.flag & BASE_ENABLED_AND_VISIBLE_IN_DEFAULT_VIEWPORT))
    {
      continue;
    }
    objects_eval.append(DEG_get_evaluated(depsgraph, object));
    has_modifiers |= object_has_active_modifiers(scene, object);
  }
  if (objects_eval.is_empty()) {
    BKE_report(op->reports,
               RPT_ERROR,
               selected_only ? "No selected CAD parts to export" : "No visible CAD parts to export");
    return OPERATOR_CANCELLED;
  }

  std::string message;
  const int bodies_num = BKE_cad_export_step(objects_eval, filepath, apply_transform, message);
  if (bodies_num < 0) {
    BKE_reportf(op->reports, RPT_ERROR, "STEP export: %s", message.c_str());
    return OPERATOR_CANCELLED;
  }
  if (has_modifiers) {
    BKE_report(op->reports,
               RPT_WARNING,
               "STEP export writes the CAD solids; Blender modifiers on parts are not included");
  }
  BKE_reportf(op->reports, RPT_INFO, "Exported %d solid(s) to STEP", bodies_num);
  return OPERATOR_FINISHED;
}

static bool wm_step_export_check(bContext * /*C*/, wmOperator *op)
{
  char filepath[FILE_MAX];
  RNA_string_get(op->ptr, "filepath", filepath);
  if (!BLI_path_extension_check_n(filepath, ".step", ".stp", nullptr)) {
    BLI_path_extension_ensure(filepath, ".step");
    RNA_string_set(op->ptr, "filepath", filepath);
    return true;
  }
  return false;
}

static void wm_step_export_draw(bContext *C, wmOperator *op)
{
  ui::Layout &layout = *op->layout;
  PointerRNA *ptr = op->ptr;
  layout.use_property_split_set(true);
  layout.use_property_decorate_set(false);

  ui::Layout &col = layout.column(false, IFACE_("Include"));
  if (CTX_wm_space_file(C)) {
    col.prop(ptr, "export_selected_objects", UI_ITEM_NONE, IFACE_("Selection Only"), ICON_NONE);
  }
  col.prop(ptr, "apply_transform", UI_ITEM_NONE, IFACE_("Object Transform"), ICON_NONE);
}

void WM_OT_step_export(wmOperatorType *ot)
{
  ot->name = "Export STEP";
  ot->description =
      "Save CAD parts to a STEP file (exact solids in millimeters; Blender modifiers are not "
      "included)";
  ot->idname = "WM_OT_step_export";

  ot->invoke = wm_step_export_invoke;
  ot->exec = wm_step_export_exec;
  ot->poll = wm_step_export_poll;
  ot->check = wm_step_export_check;
  ot->ui = wm_step_export_draw;

  ot->flag = OPTYPE_PRESET;

  WM_operator_properties_filesel(ot,
                                 FILE_TYPE_FOLDER,
                                 FILE_BLENDER,
                                 FILE_SAVE,
                                 WM_FILESEL_FILEPATH | WM_FILESEL_SHOW_PROPS,
                                 FILE_DEFAULTDISPLAY,
                                 FILE_SORT_DEFAULT);

  RNA_def_boolean(ot->srna,
                  "export_selected_objects",
                  false,
                  "Export Selected Objects",
                  "Export only selected CAD parts instead of all visible ones");
  RNA_def_boolean(ot->srna,
                  "apply_transform",
                  true,
                  "Apply Object Transform",
                  "Place parts with their object location, rotation and uniform scale");

  PropertyRNA *prop = RNA_def_string(
      ot->srna, "filter_glob", "*.step;*.stp", 0, "Extension Filter", "");
  RNA_def_property_flag(prop, PROP_HIDDEN);
}

namespace ed::io {
void step_file_handler_add()
{
  auto fh = std::make_unique<bke::FileHandlerType>();
  STRNCPY_UTF8(fh->idname, "IO_FH_step");
  STRNCPY_UTF8(fh->export_operator, "WM_OT_step_export");
  STRNCPY_UTF8(fh->label, "STEP");
  STRNCPY_UTF8(fh->file_extensions_str, ".step;.stp");
  bke::file_handler_add(std::move(fh));
}
}  // namespace ed::io

}  // namespace blender
