# SPDX-FileCopyrightText: 2026 SimpleCAD Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Headless check of the CAD feature operators, undo and STEP export:

  ./build/bin/blender -b --factory-startup --python tests/python/cad_part_ops.py -- [OCCT|SMLIB]
"""

import os
import re
import sys
import tempfile

import bpy


def check(cond, msg):
    if not cond:
        print("FAIL:", msg)
        sys.exit(1)
    print("ok:", msg)


def names(part):
    return [f.name for f in part.features]


def evaluate(ob):
    depsgraph = bpy.context.evaluated_depsgraph_get()
    ob_eval = ob.evaluated_get(depsgraph)
    return ob_eval


def step_solids(path):
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        text = fh.read()
    return text.count("MANIFOLD_SOLID_BREP("), text


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    kernel = argv[0] if argv else 'OCCT'
    tmpdir = tempfile.mkdtemp()

    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete()

    bpy.ops.object.cad_part_add(size=2.0)
    ob = bpy.context.object
    part = ob.data
    part.kernel = kernel

    # Sketch on top of the box, then Extrude: the operator picks the active sketch and,
    # because a solid exists, joins.
    bpy.ops.cad.feature_add(type='SKETCH')
    check(names(part) == ["Box1", "Sketch1"], "sketch added: %s" % names(part))
    sketch = part.features.active
    check(sketch.name == "Sketch1", "new feature is active")
    sk = sketch.sketch
    sk.origin = (0.0, 0.0, 1.0)
    pts = [sk.point_add(co) for co in ((-0.4, -0.4), (0.4, -0.4), (0.4, 0.4), (-0.4, 0.4))]
    for i in range(4):
        sk.line_add(pts[i], pts[(i + 1) % 4])

    bpy.ops.cad.feature_add(type='EXTRUDE', name="Boss")
    extrude = part.features.active
    check(extrude.name == "Boss", "named extrude")
    check(extrude["sketch"] == sketch.uid, "extrude uses the active sketch")
    check(extrude["operation"] == 1, "extrude joins the existing solid")
    extrude["distance"] = 0.5
    part.update_tag()
    evaluate(ob)
    check(extrude.status == 'OK', "extrude evaluates (%s %s)" % (extrude.status, extrude.status_message))

    # Fillet with a reference into the box.
    bpy.ops.cad.feature_add(type='FILLET')
    fillet = part.features.active
    box_uid = part.features["Box1"].uid
    fillet.reference_add("F%d:front|F%d:right" % (box_uid, box_uid), point=(1.0, -1.0, 0.0))
    fillet["radius"] = 0.2
    part.update_tag()
    evaluate(ob)
    check(fillet.status == 'OK', "fillet evaluates (%s %s)" % (fillet.status, fillet.status_message))

    # Reorder: Boss cannot move before its sketch; Fillet can move above Boss.
    part.features.active = extrude
    try:
        bpy.ops.cad.feature_move(direction='UP')
        moved = True
    except RuntimeError as ex:
        print("  expected:", ex)
        moved = False
    check(not moved and names(part)[2] == "Boss", "extrude cannot move above its sketch")
    part.features.active = fillet
    bpy.ops.cad.feature_move(direction='UP')
    check(names(part) == ["Box1", "Sketch1", "Fillet1", "Boss"], "fillet moved up: %s" % names(part))
    bpy.ops.cad.feature_move(direction='DOWN')
    check(names(part) == ["Box1", "Sketch1", "Boss", "Fillet1"], "fillet moved back down")

    # Suppress toggles.
    bpy.ops.cad.feature_suppress(feature_uid=extrude.uid)
    evaluate(ob)
    check(extrude.suppress and extrude.status == 'SUPPRESSED', "extrude suppressed")
    bpy.ops.cad.feature_suppress(feature_uid=extrude.uid)
    check(not extrude.suppress, "extrude unsuppressed")

    # Rollback marker.
    part.features.active = sketch
    bpy.ops.cad.rollback_set(action='ACTIVE')
    check(part.rollback_feature == sketch, "rollback at the sketch")
    evaluate(ob)
    check(extrude.status == 'ROLLED_BACK', "extrude rolled back")
    bpy.ops.cad.rollback_set(action='NEXT')
    check(part.rollback_feature == extrude, "rollback stepped forward")
    # Adding at the marker inserts there and moves the marker along.
    bpy.ops.cad.feature_add(type='CHAMFER')
    check(names(part) == ["Box1", "Sketch1", "Boss", "Chamfer1", "Fillet1"],
          "feature inserted at the marker: %s" % names(part))
    check(part.rollback_feature.name == "Chamfer1", "marker follows the inserted feature")
    bpy.ops.cad.feature_delete()
    check(names(part) == ["Box1", "Sketch1", "Boss", "Fillet1"], "inserted feature deleted")
    bpy.ops.cad.rollback_set(action='NEXT')
    check(part.rollback_feature is None, "marker on the last feature means no marker")
    bpy.ops.cad.rollback_set(action='PREVIOUS')
    check(part.rollback_feature == extrude, "rollback stepped back from the end")
    bpy.ops.cad.rollback_set(action='END')
    check(part.rollback_feature is None, "rollback cleared")

    # Deleting a feature others depend on still works, with a warning; the dependent errors.
    # Memfile undo is not set up in background mode until the first explicit push; after that,
    # operators push their own steps like in the UI.
    bpy.ops.ed.undo_push(message="Initial")
    bpy.ops.ed.undo_push(message="Before delete")
    part.features.active = sketch
    # Positional ('EXEC_DEFAULT', True): push an undo step like a UI call does.
    bpy.ops.cad.feature_delete('EXEC_DEFAULT', True)
    check(names(part) == ["Box1", "Boss", "Fillet1"], "sketch deleted: %s" % names(part))
    evaluate(ob)
    check(part.features["Boss"].status == 'ERROR', "orphaned extrude reports an error")
    check(part.features.active.name == "Box1", "neighbor becomes active")

    # Undo / redo through the memfile undo stack.
    ob_name = ob.name
    bpy.ops.ed.undo()
    ob = bpy.data.objects[ob_name]
    part = ob.data
    check(names(part) == ["Box1", "Sketch1", "Boss", "Fillet1"],
          "undo restores the deleted sketch: %s" % names(part))
    evaluate(ob)
    check(part.features["Boss"].status == 'OK', "restored extrude evaluates again")
    bpy.ops.ed.redo()
    part = bpy.data.objects[ob_name].data
    check("Sketch1" not in names(part), "redo deletes it again")
    bpy.ops.ed.undo()
    ob = bpy.data.objects[ob_name]
    part = ob.data
    check("Sketch1" in names(part), "undo again")

    # Undo of a parameter edit made through RNA (like dragging a field in the UI).
    part.features["Boss"]["distance"] = 0.25
    part.update_tag()
    bpy.ops.ed.undo_push(message="Distance")
    part.features["Boss"]["distance"] = 0.75
    part.update_tag()
    bpy.ops.ed.undo_push(message="Distance")
    bpy.ops.ed.undo()
    ob = bpy.data.objects[ob_name]
    part = ob.data
    check(abs(part.features["Boss"]["distance"] - 0.25) < 1e-9, "parameter edit undone")
    ob_eval = evaluate(ob)
    check(abs(ob_eval.dimensions.z - 2.25) < 1e-3,
          "undone value is evaluated (height %.4f)" % ob_eval.dimensions.z)

    evaluate(ob)
    for f in part.features:
        print("  %-8s %-10s %s" % (f.name, f.status, f.status_message))

    # STEP export, placed with the object transform.
    ob.location = (10.0, 0.0, 0.0)
    path = os.path.join(tmpdir, "part.step")
    res = bpy.ops.wm.step_export(filepath=path)
    check(res == {'FINISHED'} and os.path.exists(path), "STEP written")
    solids, text = step_solids(path)
    check(solids == 1, "one solid in the STEP file (%d)" % solids)
    # 3D points only (pcurves have 2D points in surface parameter space); skip the origin of
    # the product placement.
    pts = [tuple(float(v) for v in m.groups()) for m in
           re.finditer(r"CARTESIAN_POINT\('',\(([-0-9.E+]+),([-0-9.E+]+),([-0-9.E+]+)\)\)", text)]
    xs = [p[0] for p in pts if p != (0.0, 0.0, 0.0)]
    check(xs and min(xs) > 8999.0 and max(xs) < 11001.0,
          "object location (10 m = 10000 mm) applied (x %.1f .. %.1f)" % (min(xs), max(xs)))

    # Two parts, selection only.
    bpy.ops.object.cad_part_add(size=1.0, location=(-5.0, 0.0, 0.0))
    second = bpy.context.object
    second.data.kernel = kernel
    path2 = os.path.join(tmpdir, "two.step")
    bpy.ops.wm.step_export(filepath=path2)
    check(step_solids(path2)[0] == 2, "both parts exported")
    path3 = os.path.join(tmpdir, "selected.step")
    bpy.ops.object.select_all(action='DESELECT')
    second.select_set(True)
    bpy.ops.wm.step_export(filepath=path3, export_selected_objects=True)
    check(step_solids(path3)[0] == 1, "selection only")

    # Modifiers are not exported: a warning, not an error.
    second.modifiers.new("Bevel", 'BEVEL')
    res = bpy.ops.wm.step_export(filepath=path3, export_selected_objects=True)
    check(res == {'FINISHED'}, "export with modifiers still works")

    print("ALL OK (%s)" % kernel)


main()
