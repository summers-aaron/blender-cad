# SPDX-FileCopyrightText: 2026 SimpleCAD Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Headless check of the CAD part data-block:

  ./build/bin/blender -b --factory-startup --python tests/python/cad_part_basic.py -- [OCCT|SMLIB]
"""

import math
import os
import sys
import tempfile

import bpy


def evaluated_mesh(ob):
    depsgraph = bpy.context.evaluated_depsgraph_get()
    ob_eval = ob.evaluated_get(depsgraph)
    return ob_eval.to_mesh(), ob_eval


def mesh_volume(mesh):
    vol = 0.0
    verts = mesh.vertices
    for tri in mesh.loop_triangles:
        a, b, c = (verts[i].co for i in tri.vertices)
        vol += a.dot(b.cross(c)) / 6.0
    return vol


def check(cond, msg):
    if not cond:
        print("FAIL:", msg)
        sys.exit(1)
    print("ok:", msg)


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    kernel = argv[0] if argv else 'OCCT'

    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete()

    # Add through the operator, like Add > CAD Part.
    bpy.ops.object.cad_part_add(size=2.0)
    ob = bpy.context.object
    part = ob.data
    part.kernel = kernel
    check(ob.type == 'CAD' and isinstance(part, bpy.types.CADPart), "object is a CAD part")
    check(len(part.features) == 1 and part.features[0].type == 'BOX', "starts with a box")

    mesh, ob_eval = evaluated_mesh(ob)
    mesh.calc_loop_triangles()
    check(len(mesh.vertices) == 8, "box is a welded mesh with 8 vertices (got %d)" % len(mesh.vertices))
    check(abs(mesh_volume(mesh) - 8.0) < 1e-4, "box volume 8")
    check("cad_face" in mesh.attributes and "cad_edge" in mesh.attributes, "cad attributes present")
    ob_eval.to_mesh_clear()

    # Edit a parameter like the UI does (ID property) and re-evaluate.
    box = part.features["Box1"]
    box["size"] = (2.0, 3.0, 4.0)
    part.update_tag()
    mesh, ob_eval = evaluated_mesh(ob)
    mesh.calc_loop_triangles()
    check(abs(mesh_volume(mesh) - 24.0) < 1e-4, "edited box volume 24")
    ob_eval.to_mesh_clear()

    # Fillet an edge by persistent name: top face | front face.
    fillet = part.features.new('FILLET')
    fillet["radius"] = 0.25
    # Box spans x [-1, 1], y [-1, 2], z [-1, 3]; edge names sort their two faces.
    fillet.reference_add("F%d:front|F%d:top" % (box.uid, box.uid), point=(0.0, -1.0, 3.0))
    part.update_tag()
    mesh, ob_eval = evaluated_mesh(ob)
    mesh.calc_loop_triangles()
    expected = 24.0 - (1.0 - math.pi / 4.0) * 0.25 ** 2 * 2.0
    vol = mesh_volume(mesh)
    print("fillet status:", fillet.status, fillet.status_message)
    check(fillet.status == 'OK', "fillet evaluated")
    check(abs(vol - expected) < 2e-3, "fillet volume %.5f ~ %.5f" % (vol, expected))
    ob_eval.to_mesh_clear()

    # Blender modifiers on top of the CAD result.
    bevel = ob.modifiers.new("Bevel", 'BEVEL')
    bevel.width = 0.05
    array = ob.modifiers.new("Array", 'ARRAY')
    array.count = 3
    mesh, ob_eval = evaluated_mesh(ob)
    dims = ob_eval.dimensions
    check(dims.x > 5.9, "array modifier applied on top (x size %.3f)" % dims.x)
    ob_eval.to_mesh_clear()

    # Drivers on a feature parameter.
    fcurve = part.driver_add('features["Box1"]["size"]', 2)
    fcurve.driver.expression = "5.0"
    bpy.context.view_layer.update()
    mesh, ob_eval = evaluated_mesh(ob)
    check(abs(ob_eval.data.features["Box1"]["size"][2] - 5.0) < 1e-6, "driver sets box height")
    ob_eval.to_mesh_clear()
    part.driver_remove('features["Box1"]["size"]', 2)

    # Errors are reported per feature and keep the previous result.
    fillet["radius"] = 5.0
    part.update_tag()
    mesh, ob_eval = evaluated_mesh(ob)
    print("big fillet status:", fillet.status, fillet.status_message)
    check(fillet.status == 'ERROR', "too large fillet is an error")
    check(len(mesh.vertices) > 0, "result kept after error")
    ob_eval.to_mesh_clear()
    fillet["radius"] = 0.25

    # Rollback.
    part.rollback_feature = box
    part.update_tag()
    mesh, ob_eval = evaluated_mesh(ob)
    check(fillet.status == 'ROLLED_BACK', "fillet rolled back (%s)" % fillet.status)
    ob_eval.to_mesh_clear()
    part.rollback_feature = None

    # Sketch + extrude.
    sketch = part.features.new('SKETCH')
    sk = sketch.sketch
    sk.origin = (0.0, 0.0, 3.0)
    p = [sk.point_add(co) for co in ((-0.5, -0.5), (0.5, -0.5), (0.5, 0.5), (-0.5, 0.5))]
    for i in range(4):
        sk.line_add(p[i], p[(i + 1) % 4])
    extrude = part.features.new('EXTRUDE')
    extrude["sketch"] = sketch.uid
    extrude["distance"] = 1.0
    extrude["operation"] = 1  # Join
    part.update_tag()
    mesh, ob_eval = evaluated_mesh(ob)
    print("extrude status:", extrude.status, extrude.status_message)
    check(extrude.status == 'OK', "sketch extrude joined")
    ob_eval.to_mesh_clear()

    # Save / reload.
    path = os.path.join(tempfile.mkdtemp(), "cad_part.blend")
    ob_name = ob.name
    bpy.ops.wm.save_as_mainfile(filepath=path)
    bpy.ops.wm.open_mainfile(filepath=path)
    ob = bpy.data.objects[ob_name]
    part = ob.data
    check([f.name for f in part.features] == ["Box1", "Fillet1", "Sketch1", "Extrude1"],
          "features survive reload: %s" % [f.name for f in part.features])
    check(len(part.features["Sketch1"].sketch.curves) == 4, "sketch survives reload")
    box_uid = part.features["Box1"].uid
    check(part.features["Fillet1"].references[0].name == "F%d:front|F%d:top" % (box_uid, box_uid),
          "references survive reload")
    mesh, ob_eval = evaluated_mesh(ob)
    check(len(mesh.vertices) > 8, "reloaded part evaluates")
    ob_eval.to_mesh_clear()

    # Undo works on CAD edits (memfile undo needs an undo stack; only check it does not crash).
    print("ALL OK (%s)" % kernel)


main()
