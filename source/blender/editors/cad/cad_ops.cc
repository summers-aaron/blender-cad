/* SPDX-FileCopyrightText: 2026 SimpleCAD Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edcad
 *
 * Feature timeline operators for CAD parts: add, delete, reorder, suppress, rollback.
 * They are thin, undoable wrappers over BKE_cad; the same edits are available from Python
 * through `part.features`.
 */

#include <fmt/format.h>

#include "DNA_cad_types.h"
#include "DNA_object_types.h"

#include "BLI_listbase.hh"
#include "BLI_listbase_iterator.hh"
#include "BLI_string.hh"
#include "BLI_vector.hh"

#include "BKE_cad.hh"
#include "BKE_context.hh"
#include "BKE_library.hh"
#include "BKE_report.hh"

#include "DEG_depsgraph.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_enum_types.hh"
#include "RNA_prototypes.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "ED_cad.hh"

#include "cad_intern.hh"
#include "cad_sketch.hh"

namespace blender::ed::cad {

/* -------------------------------------------------------------------- */
/** \name Utilities
 * \{ */

/** The part from the context: a pinned/context `cad_part`, else the active object's data. */
static CADPart *context_part(const bContext *C)
{
  PointerRNA ptr = CTX_data_pointer_get_type(C, "cad_part", RNA_CADPart);
  if (ptr.data) {
    return static_cast<CADPart *>(ptr.data);
  }
  Object *ob = CTX_data_active_object(C);
  if (ob && ob->type == OB_CAD && ob->data) {
    return id_cast<CADPart *>(ob->data);
  }
  return nullptr;
}

static bool part_editable_poll(bContext *C)
{
  const CADPart *part = context_part(C);
  if (part == nullptr) {
    CTX_wm_operator_poll_msg_set(C, "No active CAD part");
    return false;
  }
  if (!ID_IS_EDITABLE(&part->id) || ID_IS_OVERRIDE_LIBRARY(&part->id)) {
    CTX_wm_operator_poll_msg_set(C, "CAD part is not editable");
    return false;
  }
  return true;
}

static bool active_feature_poll(bContext *C)
{
  if (!part_editable_poll(C)) {
    return false;
  }
  if (BKE_cad_feature_active(context_part(C)) == nullptr) {
    CTX_wm_operator_poll_msg_set(C, "No active feature");
    return false;
  }
  return true;
}

static void part_changed(bContext *C, CADPart *part)
{
  DEG_id_tag_update(&part->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, part);
}

/** Feature from the `feature_uid` property, or the active feature when it is not set. */
static CADFeature *feature_from_op(bContext *C, wmOperator *op)
{
  CADPart *part = context_part(C);
  const int uid = RNA_int_get(op->ptr, "feature_uid");
  return uid >= 0 ? BKE_cad_feature_find(part, uid) : BKE_cad_feature_active(part);
}

static void prop_feature_uid_def(wmOperatorType *ot)
{
  PropertyRNA *prop = RNA_def_int(ot->srna,
                                  "feature_uid",
                                  -1,
                                  -1,
                                  INT_MAX,
                                  "Feature",
                                  "Unique id of the feature (-1: the active feature)",
                                  -1,
                                  INT_MAX);
  RNA_def_property_flag(prop, PropertyFlag(PROP_HIDDEN | PROP_SKIP_SAVE));
}

/** True when `name` (a persistent topology name) refers to geometry made by feature `uid`. */
static bool topo_name_uses_feature(const StringRef name, const int uid)
{
  const std::string token = fmt::format("F{}:", uid);
  for (int64_t pos = name.find(token); pos != StringRef::not_found;
       pos = name.find(token, pos + 1))
  {
    if (pos == 0 || !(name[pos - 1] >= '0' && name[pos - 1] <= '9')) {
      return true;
    }
  }
  return false;
}

/** Features after `feature` that use its sketch or its topology. */
static Vector<const CADFeature *> feature_dependents(const CADPart * /*part*/,
                                                    const CADFeature *feature)
{
  Vector<const CADFeature *> dependents;
  for (const CADFeature *f = feature->next; f; f = f->next) {
    bool uses = f->type == CAD_FEATURE_EXTRUDE &&
                BKE_cad_feature_param_get_int(f, "sketch", -1) == feature->uid;
    for (int i = 0; i < f->refs_num && !uses; i++) {
      uses = f->refs[i].name && topo_name_uses_feature(f->refs[i].name, feature->uid);
    }
    if (uses) {
      dependents.append(f);
    }
  }
  return dependents;
}

void feature_insert_at_rollback(CADPart *part, CADFeature *feature)
{
  CADFeature *marker = BKE_cad_feature_find(part, part->rollback_uid);
  if (marker == nullptr || marker == feature) {
    return;
  }
  BLI_remlink(&part->features, feature);
  BLI_insertlinkafter(&part->features, marker, feature);
  part->rollback_uid = feature->uid;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Add Feature
 * \{ */

static wmOperatorStatus feature_add_exec(bContext *C, wmOperator *op)
{
  CADPart *part = context_part(C);
  const eCADFeatureType type = eCADFeatureType(RNA_enum_get(op->ptr, "type"));
  const CADFeature *prev_active = BKE_cad_feature_active(part);

  CADFeature *feature = BKE_cad_feature_add(part, type);
  feature_insert_at_rollback(part, feature);

  if (type == CAD_FEATURE_EXTRUDE) {
    /* Extrude the active sketch, else the closest sketch before it. */
    const CADFeature *sketch = (prev_active && prev_active->type == CAD_FEATURE_SKETCH &&
                                BLI_findindex(&part->features, prev_active) <
                                    BLI_findindex(&part->features, feature)) ?
                                   prev_active :
                                   nullptr;
    bool has_solid = false;
    for (const CADFeature *f = feature->prev; f; f = f->prev) {
      if (f->type == CAD_FEATURE_SKETCH) {
        if (sketch == nullptr) {
          sketch = f;
        }
      }
      else if (!(f->flag & CAD_FEATURE_SUPPRESSED)) {
        has_solid = true;
      }
    }
    if (sketch) {
      BKE_cad_feature_param_set_int(feature, "sketch", sketch->uid);
    }
    /* Like other CAD tools: the first extrude makes a body, later ones join it. */
    BKE_cad_feature_param_set_int(feature, "operation", has_solid ? 1 : 0);
  }

  char name[sizeof(CADFeature::name)];
  RNA_string_get(op->ptr, "name", name);
  if (name[0]) {
    PointerRNA ptr = RNA_pointer_create_discrete(&part->id, RNA_CADFeature, feature);
    RNA_string_set(&ptr, "name", name);
  }

  part_changed(C, part);
  return OPERATOR_FINISHED;
}

static void CAD_OT_feature_add(wmOperatorType *ot)
{
  ot->name = "Add Feature";
  ot->description = "Add a feature to the CAD part's timeline";
  ot->idname = "CAD_OT_feature_add";

  ot->exec = feature_add_exec;
  ot->poll = part_editable_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  ot->prop = RNA_def_enum(
      ot->srna, "type", rna_enum_cad_feature_type_items, CAD_FEATURE_SKETCH, "Type", "");
  PropertyRNA *prop = RNA_def_string(
      ot->srna, "name", nullptr, sizeof(CADFeature::name), "Name", "Name of the new feature");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Delete Feature
 * \{ */

static wmOperatorStatus feature_delete_exec(bContext *C, wmOperator *op)
{
  CADPart *part = context_part(C);
  CADFeature *feature = feature_from_op(C, op);
  if (feature == nullptr) {
    return OPERATOR_CANCELLED;
  }

  const Vector<const CADFeature *> dependents = feature_dependents(part, feature);
  if (!dependents.is_empty()) {
    std::string names;
    for (const CADFeature *f : dependents) {
      names += (names.empty() ? "" : ", ") + std::string(f->name);
    }
    BKE_reportf(op->reports,
                RPT_WARNING,
                "Deleted \"%s\"; features that used it need new inputs: %s",
                feature->name,
                names.c_str());
  }

  /* Keep a neighbor active so repeated deletes walk the timeline. */
  const CADFeature *neighbor = feature->prev ? feature->prev : feature->next;
  const bool was_active = part->active_feature_uid == feature->uid;
  BKE_cad_feature_remove(part, feature);
  if (was_active) {
    part->active_feature_uid = neighbor ? neighbor->uid : -1;
  }

  part_changed(C, part);
  return OPERATOR_FINISHED;
}

static void CAD_OT_feature_delete(wmOperatorType *ot)
{
  ot->name = "Delete Feature";
  ot->description = "Remove a feature from the CAD part's timeline";
  ot->idname = "CAD_OT_feature_delete";

  ot->exec = feature_delete_exec;
  ot->poll = active_feature_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  prop_feature_uid_def(ot);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Move Feature
 * \{ */

static wmOperatorStatus feature_move_exec(bContext *C, wmOperator *op)
{
  CADPart *part = context_part(C);
  CADFeature *feature = feature_from_op(C, op);
  if (feature == nullptr) {
    return OPERATOR_CANCELLED;
  }
  const int offset = RNA_enum_get(op->ptr, "direction");

  /* Inputs must come before the features that use them. */
  if (offset < 0 && feature->prev) {
    const Vector<const CADFeature *> deps = feature_dependents(part, feature->prev);
    if (deps.contains(feature)) {
      BKE_reportf(op->reports,
                  RPT_ERROR,
                  "\"%s\" uses \"%s\" and cannot move before it",
                  feature->name,
                  feature->prev->name);
      return OPERATOR_CANCELLED;
    }
  }
  if (offset > 0 && feature->next) {
    const Vector<const CADFeature *> deps = feature_dependents(part, feature);
    if (deps.contains(feature->next)) {
      BKE_reportf(op->reports,
                  RPT_ERROR,
                  "\"%s\" uses \"%s\" and cannot move before it",
                  feature->next->name,
                  feature->name);
      return OPERATOR_CANCELLED;
    }
  }

  if (!BKE_cad_feature_move(part, feature, offset)) {
    return OPERATOR_CANCELLED;
  }
  part_changed(C, part);
  return OPERATOR_FINISHED;
}

static void CAD_OT_feature_move(wmOperatorType *ot)
{
  static const EnumPropertyItem direction_items[] = {
      {-1, "UP", 0, "Up", "Move earlier in the timeline"},
      {1, "DOWN", 0, "Down", "Move later in the timeline"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  ot->name = "Move Feature";
  ot->description = "Move a feature earlier or later in the CAD part's timeline";
  ot->idname = "CAD_OT_feature_move";

  ot->exec = feature_move_exec;
  ot->poll = active_feature_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_enum(ot->srna, "direction", direction_items, -1, "Direction", "");
  prop_feature_uid_def(ot);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Suppress Feature
 * \{ */

static wmOperatorStatus feature_suppress_exec(bContext *C, wmOperator *op)
{
  CADPart *part = context_part(C);
  CADFeature *feature = feature_from_op(C, op);
  if (feature == nullptr) {
    return OPERATOR_CANCELLED;
  }
  const int action = RNA_enum_get(op->ptr, "action");
  const bool suppress = action == 2 ? !(feature->flag & CAD_FEATURE_SUPPRESSED) : action == 1;
  SET_FLAG_FROM_TEST(feature->flag, suppress, CAD_FEATURE_SUPPRESSED);
  part_changed(C, part);
  return OPERATOR_FINISHED;
}

static void CAD_OT_feature_suppress(wmOperatorType *ot)
{
  static const EnumPropertyItem action_items[] = {
      {2, "TOGGLE", 0, "Toggle", ""},
      {1, "SUPPRESS", 0, "Suppress", "Skip the feature when building the part"},
      {0, "UNSUPPRESS", 0, "Unsuppress", "Build the feature again"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  ot->name = "Suppress Feature";
  ot->description = "Skip a feature when building the CAD part, without deleting it";
  ot->idname = "CAD_OT_feature_suppress";

  ot->exec = feature_suppress_exec;
  ot->poll = active_feature_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_enum(ot->srna, "action", action_items, 2, "Action", "");
  prop_feature_uid_def(ot);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Rollback
 * \{ */

enum class RollbackAction {
  Active = 0,
  End = 1,
  Previous = 2,
  Next = 3,
};

static wmOperatorStatus rollback_set_exec(bContext *C, wmOperator *op)
{
  CADPart *part = context_part(C);
  const RollbackAction action = RollbackAction(RNA_enum_get(op->ptr, "action"));
  const CADFeature *marker = BKE_cad_feature_find(part, part->rollback_uid);
  const CADFeature *last = part->features.last();
  int uid = part->rollback_uid;

  switch (action) {
    case RollbackAction::Active: {
      const CADFeature *active = BKE_cad_feature_active(part);
      if (active == nullptr) {
        BKE_report(op->reports, RPT_ERROR, "No active feature");
        return OPERATOR_CANCELLED;
      }
      uid = active->uid;
      break;
    }
    case RollbackAction::End:
      uid = -1;
      break;
    case RollbackAction::Previous: {
      const CADFeature *current = marker ? marker : last;
      if (current == nullptr || current->prev == nullptr) {
        return OPERATOR_CANCELLED;
      }
      uid = current->prev->uid;
      break;
    }
    case RollbackAction::Next:
      if (marker == nullptr) {
        return OPERATOR_CANCELLED;
      }
      uid = marker->next ? marker->next->uid : -1;
      break;
  }
  /* A marker on the last feature is the same as no marker. */
  if (last && uid == last->uid) {
    uid = -1;
  }
  if (uid == part->rollback_uid) {
    return OPERATOR_CANCELLED;
  }
  part->rollback_uid = uid;
  part_changed(C, part);
  return OPERATOR_FINISHED;
}

static void CAD_OT_rollback_set(wmOperatorType *ot)
{
  static const EnumPropertyItem action_items[] = {
      {int(RollbackAction::Active),
       "ACTIVE",
       0,
       "Active Feature",
       "Build the part up to and including the active feature"},
      {int(RollbackAction::End), "END", 0, "End", "Build the whole timeline"},
      {int(RollbackAction::Previous), "PREVIOUS", 0, "Step Back", "Move the marker one back"},
      {int(RollbackAction::Next), "NEXT", 0, "Step Forward", "Move the marker one forward"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  ot->name = "Set Rollback";
  ot->description = "Move the rollback marker: features after it are not built";
  ot->idname = "CAD_OT_rollback_set";

  ot->exec = rollback_set_exec;
  ot->poll = part_editable_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  ot->prop = RNA_def_enum(
      ot->srna, "action", action_items, int(RollbackAction::Active), "Action", "");
}

/** \} */

void operatortypes_cad()
{
  WM_operatortype_append(CAD_OT_feature_add);
  WM_operatortype_append(CAD_OT_feature_delete);
  WM_operatortype_append(CAD_OT_feature_move);
  WM_operatortype_append(CAD_OT_feature_suppress);
  WM_operatortype_append(CAD_OT_rollback_set);
  WM_operatortype_append(CAD_OT_reference_toggle);
  WM_operatortype_append(CAD_OT_references_pick);

  WM_operatortype_append(CAD_OT_sketch_add);
  WM_operatortype_append(CAD_OT_sketch_edit);
  WM_operatortype_append(CAD_OT_sketch_finish);
  WM_operatortype_append(CAD_OT_sketch_select);
  WM_operatortype_append(CAD_OT_sketch_select_all);
  WM_operatortype_append(CAD_OT_sketch_drag);
  WM_operatortype_append(CAD_OT_sketch_delete);
  WM_operatortype_append(CAD_OT_sketch_line);
  WM_operatortype_append(CAD_OT_sketch_rectangle);
  WM_operatortype_append(CAD_OT_sketch_circle);
  WM_operatortype_append(CAD_OT_sketch_arc);
  WM_operatortype_append(CAD_OT_sketch_constrain);
  WM_operatortype_append(CAD_OT_sketch_dimension);
  WM_operatortype_append(CAD_OT_sketch_dimension_edit);
  WM_operatortype_append(CAD_OT_sketch_construction_toggle);

  sketch_draw_init();
}

void keymap_cad(wmKeyConfig *keyconf)
{
  sketch_keymap(keyconf);
}

void gizmotypes_cad()
{
  sketch_gizmos_register();
}

}  // namespace blender::ed::cad
