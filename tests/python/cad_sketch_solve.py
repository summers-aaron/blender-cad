# SPDX-FileCopyrightText: 2026 SimpleCAD Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Headless check of sketch constraint solving through the Python API:

  ./build/bin/blender -b --factory-startup --python tests/python/cad_sketch_solve.py -- [OCCT|SMLIB]
"""

import math
import os
import sys
import tempfile

import bpy

ORIGIN, X_AXIS, Y_AXIS = -2, -3, -4


def check(cond, msg):
    if not cond:
        print("FAIL:", msg)
        sys.exit(1)
    print("ok:", msg)


def evaluated(ob):
    return ob.evaluated_get(bpy.context.evaluated_depsgraph_get())


def volume(ob):
    ob_eval = evaluated(ob)
    mesh = ob_eval.to_mesh()
    mesh.calc_loop_triangles()
    v = 0.0
    for tri in mesh.loop_triangles:
        a, b, c = (mesh.vertices[i].co for i in tri.vertices)
        v += a.dot(b.cross(c)) / 6.0
    ob_eval.to_mesh_clear()
    return v


def point(sk, uid):
    return next(p for p in sk.points if p.uid == uid)


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    kernel = argv[0] if argv else 'OCCT'

    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete()
    bpy.ops.object.cad_part_add(add_box=False)
    ob = bpy.context.object
    part = ob.data
    part.kernel = kernel

    sketch = part.features.new('SKETCH')
    sk = sketch.sketch
    check(sk.dof == -1, "new sketch is not solved yet")

    # A rough quadrilateral: the constraints make it an exact 0.3 x 0.2 rectangle at the origin.
    p = [sk.point_add(co) for co in ((0.01, -0.02), (0.28, 0.01), (0.31, 0.22), (-0.01, 0.19))]
    lines = [sk.line_add(p[i], p[(i + 1) % 4]) for i in range(4)]
    check(sk.solve() and sk.dof == 8, "free quadrilateral has 8 degrees of freedom (%d)" % sk.dof)

    sk.constraint_add('HORIZONTAL', [lines[0]])
    sk.constraint_add('HORIZONTAL', [lines[2]])
    sk.constraint_add('VERTICAL', [lines[1]])
    sk.constraint_add('VERTICAL', [lines[3]])
    sk.constraint_add('COINCIDENT', [p[0], ORIGIN])
    width = sk.constraint_add('DISTANCE', [lines[0]], value=0.3)
    sk.constraint_add('DISTANCE', [lines[3]], value=0.2)
    check(sk.solve_status == 'OK' and sk.dof == 0, "rectangle fully constrained (dof %d)" % sk.dof)
    c = point(sk, p[2]).co
    check(abs(c[0] - 0.3) < 1e-6 and abs(c[1] - 0.2) < 1e-6, "corner solved to (0.3, 0.2): %s" % (tuple(c),))

    extrude = part.features.new('EXTRUDE')
    extrude["sketch"] = sketch.uid
    extrude["distance"] = 0.1
    part.update_tag()
    check(abs(volume(ob) - 0.3 * 0.2 * 0.1) < 1e-6, "extruded rectangle volume")

    # Editing a dimension re-solves (like typing a new value in the UI).
    dim = next(c for c in sk.constraints if c.uid == width)
    dim.value = 0.5
    c = point(sk, p[2]).co
    check(abs(c[0] - 0.5) < 1e-6, "width dimension edit moves the geometry (%.4f)" % c[0])
    check(abs(volume(ob) - 0.5 * 0.2 * 0.1) < 1e-6, "extrude follows the dimension")

    # A driver on a dimension is solved during evaluation.
    path = 'features["%s"].sketch.constraints[%d].value' % (sketch.name, list(sk.constraints).index(dim))
    fcurve = part.driver_add(path)
    fcurve.driver.expression = "0.4"
    bpy.context.view_layer.update()
    check(abs(volume(ob) - 0.4 * 0.2 * 0.1) < 1e-6, "driven dimension changes the solid")
    part.driver_remove(path)
    dim.value = 0.5

    # Conflicting constraint: geometry stays, the constraint is flagged.
    bad = sk.constraint_add('DISTANCE', [p[0], p[1]], value=0.7)
    check(sk.solve_status == 'CONFLICTING', "conflict detected")
    bad_c = next(c for c in sk.constraints if c.uid == bad)
    check(bad_c.is_conflicting, "conflicting constraint flagged")
    c = point(sk, p[2]).co
    check(abs(c[0] - 0.5) < 1e-6, "geometry unchanged by the conflict")

    # Redundant constraint: solves, flagged.
    sk.constraint_add('PARALLEL', [lines[0], lines[2]])
    # Remove the conflict by giving the constraint a consistent value. Adding constraints
    # reallocates the array: look the constraint up again (like mesh elements).
    bad_c = next(c for c in sk.constraints if c.uid == bad)
    bad_c.value = 0.5
    check(sk.solve_status == 'OK', "consistent again (%s)" % sk.solve_status)
    check(any(c.is_redundant for c in sk.constraints), "redundant constraint flagged")

    # Circles and radius / angle dimensions.
    center = sk.point_add((0.25, 0.1))
    circle = sk.circle_add(center, 0.03)
    sk.constraint_add('RADIUS', [circle], value=0.05)
    sk.constraint_add('DISTANCE_X', [p[0], center], value=0.25)
    sk.constraint_add('DISTANCE_Y', [p[0], center], value=0.1)
    curve = next(cv for cv in sk.curves if cv.uid == circle)
    check(abs(curve.radius - 0.05) < 1e-6, "circle radius solved")
    check(sk.dof == 0, "still fully constrained (%d)" % sk.dof)
    part.update_tag()
    expected = (0.5 * 0.2 - math.pi * 0.05 ** 2) * 0.1
    check(abs(volume(ob) - expected) < 2e-4, "hole from the circle (%.6f vs %.6f)" % (volume(ob), expected))

    # Save / reload keeps the solve state and flags.
    tmp = os.path.join(tempfile.mkdtemp(), "sketch.blend")
    ob_name = ob.name
    bpy.ops.wm.save_as_mainfile(filepath=tmp)
    bpy.ops.wm.open_mainfile(filepath=tmp)
    sk = bpy.data.objects[ob_name].data.features["Sketch1"].sketch
    check(sk.dof == 0 and sk.solve_status == 'OK', "solve state survives reload")
    check(any(c.is_redundant for c in sk.constraints), "flags survive reload")

    print("ALL OK (%s)" % kernel)


main()
