# SPDX-FileCopyrightText: 2026 SimpleCAD Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Headless check of feature dialogs, user parameters and expressions:

  ./build/bin/blender -b --factory-startup --python tests/python/cad_part_dialog.py -- [OCCT|SMLIB]
"""

import math
import os
import sys
import tempfile

import bpy


def check(cond, msg):
    if not cond:
        print("FAIL:", msg)
        sys.exit(1)
    print("ok:", msg)


def close(a, b, eps=1e-6):
    return abs(a - b) < eps


def evaluate(ob):
    depsgraph = bpy.context.evaluated_depsgraph_get()
    return ob.evaluated_get(depsgraph)


def point(sketch, uid):
    return next(p for p in sketch.points if p.uid == uid)


def line_length(sketch, line_uid):
    curve = next(c for c in sketch.curves if c.uid == line_uid)
    a, b = point(sketch, curve.points[0]).co, point(sketch, curve.points[1]).co
    return math.hypot(b[0] - a[0], b[1] - a[1])


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    kernel = argv[0] if argv else 'OCCT'

    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete()
    bpy.ops.object.cad_part_add(size=2.0)
    ob = bpy.context.object
    part = ob.data
    part.kernel = kernel
    mm = 1.0 / part.mm_per_unit  # Blender units per millimeter.

    # ------------------------------------------------------------------
    # Parameters.
    width = part.parameters.new(name="width", expression="800 mm")
    check(close(width.value, 800 * mm), "parameter value (%f)" % width.value)
    check(width.value_text == "800 mm", "parameter value text: %s" % width.value_text)
    part.parameters.new(name="height", expression="width / 4")
    height = part.parameters["height"]
    check(close(height.value, 200 * mm), "parameter from another (%f)" % height.value)
    part.parameters.new(name="angle", expression="30", unit='ANGLE')
    check(close(part.parameters["angle"].value, math.radians(30)), "bare angles are degrees")
    dup = part.parameters.new(name="width", expression="1")
    check(dup.name == "width1", "duplicate names are made unique: %s" % dup.name)
    part.parameters.remove(part.parameters["width1"])

    # A parameter cannot depend on itself through others: the change is rejected.
    part.parameters["width"].expression = "height * 2"
    check(part.parameters["width"].expression == "800 mm", "cyclic expression rejected")
    # Unknown names are rejected too.
    part.parameters["width"].expression = "depth"
    check(part.parameters["width"].expression == "800 mm", "unknown name rejected")

    # Renaming updates the expressions using it.
    part.parameters["width"].name = "w"
    check(part.parameters["height"].expression == "w / 4",
          "rename follows into expressions: %s" % part.parameters["height"].expression)
    part.parameters["w"].name = "width"
    # Invalid names are rejected.
    part.parameters["width"].name = "mm"
    check(part.parameters[0].name == "width", "unit names cannot name parameters")

    # ------------------------------------------------------------------
    # A sketch dimension bound to a parameter.
    bpy.ops.cad.feature_add(type='SKETCH')
    sketch_feature = part.features.active
    sk = sketch_feature.sketch
    sk.origin = (0.0, 0.0, 1.0)
    pts = [sk.point_add(co) for co in ((-0.4, -0.4), (0.4, -0.4), (0.4, 0.4), (-0.4, 0.4))]
    lines = [sk.line_add(pts[i], pts[(i + 1) % 4]) for i in range(4)]
    c_uid = sk.constraint_add(type='DISTANCE', entities=[lines[0]], value=0.5)
    dim = next(c for c in sk.constraints if c.uid == c_uid)
    dim.expression = "width"
    check(close(dim.length, 0.8), "dimension bound to a parameter (%f)" % dim.length)
    check(dim.expression == "width", "dimension keeps its expression")
    check(close(line_length(sk, lines[0]), 0.8), "sketch solved to the parameter")

    part.parameters["width"].expression = "1000 mm"
    check(close(dim.length, 1.0), "dimension follows the parameter (%f)" % dim.length)
    check(close(line_length(sk, lines[0]), 1.0), "sketch re-solved after the parameter change")
    check(close(part.parameters["height"].value, 250 * mm), "dependent parameter updated")

    # Plain values unbind.
    dim.expression = "0.9 m"
    check(dim.expression == "900 mm" and close(dim.length, 0.9), "plain value: %s" % dim.expression)
    dim.expression = "width"

    # ------------------------------------------------------------------
    # New feature in a dialog: Cancel removes it.
    bpy.ops.ed.undo_push(message="Initial")
    features_before = [f.name for f in part.features]
    bpy.ops.cad.feature_new(type='EXTRUDE')
    dialog = part.dialog_feature
    check(dialog is not None and dialog.is_new and dialog.type == 'EXTRUDE', "dialog opened")
    check(dialog.profile == sketch_feature.name, "dialog profile is the sketch: %s" % dialog.profile)
    dialog.inputs["distance"].expression = "height"
    check(close(dialog.inputs["distance"].value, 0.25), "input bound to a parameter")
    check(dialog.inputs["distance"].is_expression, "input keeps the expression")
    evaluate(ob)
    check(dialog.status == 'OK', "preview evaluates (%s %s)" % (dialog.status, dialog.status_message))
    bpy.ops.cad.dialog_cancel()
    check(part.dialog_feature is None, "dialog closed")
    check([f.name for f in part.features] == features_before, "cancel removes the new feature")

    # New feature, OK: one undo step.
    bpy.ops.ed.undo_push(message="Before extrude")
    bpy.ops.cad.feature_new(type='EXTRUDE')
    part.dialog_feature.inputs["distance"].expression = "height"
    part.dialog_feature["operation"] = 1  # Join.
    bpy.ops.cad.dialog_ok()
    check(part.dialog_feature is None, "OK closes the dialog")
    extrude = part.features.active
    check(extrude.type == 'EXTRUDE', "OK keeps the feature")
    check(extrude.inputs["distance"].is_expression, "kept feature is bound")
    top = evaluate(ob).bound_box[1][2]
    check(close(top, 1.0 + 0.25, 1e-4), "extruded to the parameter (top %.4f)" % top)

    part.parameters["height"].expression = "width / 2"
    check(close(extrude.inputs["distance"].value, 0.5), "parameter change drives the extrude")
    top = evaluate(ob).bound_box[1][2]
    check(close(top, 1.5, 1e-4), "model rebuilt (top %.4f)" % top)
    part.parameters["height"].expression = "width / 4"

    bpy.ops.ed.undo_push(message="After extrude")
    bpy.ops.ed.undo()
    bpy.ops.ed.undo()
    part = ob.data
    check(not any(f.type == 'EXTRUDE' for f in part.features), "one undo removes the extrude")
    bpy.ops.ed.redo()
    part = ob.data
    extrude = next((f for f in part.features if f.type == 'EXTRUDE'), None)
    check(extrude is not None and part.dialog_feature is None, "redo brings it back, dialog closed")

    # ------------------------------------------------------------------
    # Editing an existing feature: rolled back to it, Cancel restores.
    bpy.ops.ed.undo_push(message="Before edit")
    bpy.ops.cad.feature_edit(feature_uid=extrude.uid)
    dialog = part.dialog_feature
    check(dialog is not None and not dialog.is_new and dialog.uid == extrude.uid, "edit dialog")
    dialog.inputs["distance"].expression = "123 mm"
    check(close(extrude.inputs["distance"].value, 0.123), "edit previews")
    check(not extrude.inputs["distance"].is_expression, "plain value unbinds")
    bpy.ops.cad.dialog_cancel()
    check(extrude.inputs["distance"].is_expression and extrude.inputs["distance"].expression == "height",
          "cancel restores the expression")
    check(close(extrude.inputs["distance"].value, 0.25), "cancel restores the value")

    # Undo while a dialog is open cancels the dialog and nothing else.
    bpy.ops.cad.feature_edit(feature_uid=extrude.uid)
    part.dialog_feature.inputs["distance"].expression = "50 mm"
    bpy.ops.ed.undo()
    part = ob.data
    extrude = next((f for f in part.features if f.type == 'EXTRUDE'), None)
    check(part.dialog_feature is None, "undo cancels the dialog")
    check(extrude is not None and close(extrude.inputs["distance"].value, 0.25),
          "undo in a dialog restores the feature only")

    # Edit + OK.
    bpy.ops.cad.feature_edit(feature_uid=extrude.uid)
    part.dialog_feature.inputs["distance"].expression = "height * 2"
    bpy.ops.cad.dialog_ok()
    check(close(extrude.inputs["distance"].value, 0.5), "edit kept")

    # Setting a value directly (gizmos do) unbinds.
    extrude.inputs["distance"].value = 0.3
    check(not extrude.inputs["distance"].is_expression and extrude.inputs["distance"].expression == "300 mm",
          "value set unbinds: %s" % extrude.inputs["distance"].expression)

    # A new dialog while another is open confirms the open one.
    bpy.ops.cad.feature_edit(feature_uid=extrude.uid)
    part.dialog_feature.inputs["distance"].expression = "350 mm"
    bpy.ops.cad.feature_new(type='EXTRUDE')
    check(close(extrude.inputs["distance"].value, 0.35), "open dialog confirmed by the next one")
    bpy.ops.cad.dialog_cancel()

    # Removing a parameter leaves expressions with their last values and an error.
    extrude.inputs["distance"].expression = "height"
    part.parameters.remove(part.parameters["height"])
    check(extrude.inputs["distance"].has_error, "expression using a removed parameter has an error")
    check(close(extrude.inputs["distance"].value, 0.25), "and keeps its last value")
    part.parameters.new(name="height", expression="100 mm")
    check(not extrude.inputs["distance"].has_error and close(extrude.inputs["distance"].value, 0.1),
          "re-adding the parameter binds it again")

    # ------------------------------------------------------------------
    # Save and reload, with a dialog open.
    bpy.ops.cad.feature_edit(feature_uid=extrude.uid)
    part.dialog_feature.inputs["distance"].expression = "40 mm"
    path = os.path.join(tempfile.mkdtemp(), "dialog.blend")
    bpy.ops.wm.save_as_mainfile(filepath=path)
    bpy.ops.wm.open_mainfile(filepath=path)
    part = bpy.data.cad_parts[0]
    check([p.name for p in part.parameters] == ["width", "angle", "height"],
          "parameters saved: %s" % [p.name for p in part.parameters])
    check(part.parameters["height"].expression == "100 mm", "parameter expression saved")
    sk = next(f for f in part.features if f.type == 'SKETCH').sketch
    check(next(c for c in sk.constraints if c.type == 'DISTANCE').expression == "width",
          "dimension expression saved")
    check(part.dialog_feature is not None, "open dialog saved")
    bpy.ops.cad.dialog_cancel()
    extrude = next(f for f in part.features if f.type == 'EXTRUDE')
    check(extrude.inputs["distance"].expression == "height" and close(extrude.inputs["distance"].value, 0.1),
          "cancel after reload restores the feature")

    print("ALL OK (dialog)")


main()
