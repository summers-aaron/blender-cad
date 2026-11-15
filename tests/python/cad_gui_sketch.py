# SPDX-FileCopyrightText: 2026 SimpleCAD Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Interactive test of sketch mode with simulated mouse and keyboard events. Needs a display
(e.g. Xvfb); screenshots are written when SHOT_DIR is set.

  xvfb-run -a -s "-screen 0 1920x1080x24" ./build/bin/blender --factory-startup \\
      --enable-event-simulate --python tests/python/cad_gui_sketch.py -- [OCCT|SMLIB]
"""

import os
import subprocess
import sys

import bpy
from bpy_extras.view3d_utils import location_3d_to_region_2d

OUT = os.environ.get("SHOT_DIR", "")
bpy.context.preferences.view.show_splash = False
steps = []
state = {}


def step(fn):
    steps.append(fn)
    return fn


def check(cond, msg):
    if not cond:
        print("FAIL:", msg)
        sys.stdout.flush()
        os._exit(1)
    print("ok:", msg)


def v3d():
    win = bpy.context.window_manager.windows[0]
    area = next(a for a in win.screen.areas if a.type == 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    return win, area, region


def override():
    win, area, region = v3d()
    return bpy.context.temp_override(window=win, area=area, region=region)


def to_window(co):
    win, area, region = v3d()
    p = location_3d_to_region_2d(region, area.spaces[0].region_3d, (co[0], co[1], 0.0))
    return int(region.x + p.x), int(region.y + p.y)


def ev(type, value, co=None, xy=None, **mods):
    win = v3d()[0]
    if xy is None:
        xy = to_window(co) if co is not None else state.get("mouse", (900, 500))
    state["mouse"] = xy
    win.event_simulate(type=type, value=value, x=xy[0], y=xy[1], **mods)


def move(co):
    ev('MOUSEMOVE', 'NOTHING', co)


def click(co):
    move(co)
    ev('LEFTMOUSE', 'PRESS', co)
    ev('LEFTMOUSE', 'RELEASE', co)


def key(k, **mods):
    ev(k, 'PRESS', **mods)
    ev(k, 'RELEASE', **mods)


_DIGITS = ['ZERO', 'ONE', 'TWO', 'THREE', 'FOUR', 'FIVE', 'SIX', 'SEVEN', 'EIGHT', 'NINE']


def type_text(text):
    """Types into the active text field (the dimension value popup starts editing)."""
    for ch in text:
        k = _DIGITS[int(ch)] if ch.isdigit() else {'.': 'PERIOD', ' ': 'SPACE'}.get(ch, ch.upper())
        ev(k, 'PRESS', unicode=ch)
        ev(k, 'RELEASE')


def screenshot(name):
    # Captures the (virtual) X screen: reading back Blender's window is black with Mesa
    # llvmpipe on Xvfb. Needs ImageMagick's `import`.
    if OUT and os.environ.get("DISPLAY"):
        subprocess.run(["import", "-window", "root", "-display", os.environ["DISPLAY"],
                        os.path.join(OUT, name)], check=False)


def part():
    return bpy.data.objects["Part"].data


def sketch():
    return part().features["Sketch1"].sketch


def point_at(co, eps=1e-4):
    return next((p for p in sketch().points
                 if abs(p.co[0] - co[0]) < eps and abs(p.co[1] - co[1]) < eps), None)


@step
def setup():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    with override():
        bpy.ops.object.select_all(action='SELECT')
        bpy.ops.object.delete()
        bpy.ops.object.cad_part_add(add_box=False)
        part().kernel = argv[0] if argv else 'OCCT'
        bpy.ops.cad.sketch_add('EXEC_DEFAULT', True, plane='XY')
        bpy.ops.view3d.view_axis(type='TOP')
        bpy.ops.view3d.view_all()
    check(bpy.context.mode == 'EDIT_CAD', "new sketch enters sketch mode (%s)" % bpy.context.mode)
    check(part().edit_sketch is not None and part().edit_sketch.name == "Sketch1", "editing Sketch1")


@step
def zoom():
    # A 1 m view: easy coordinates. Zoom so 1 m fills a good part of the region.
    _win, area, _region = v3d()
    rv3d = area.spaces[0].region_3d
    rv3d.view_distance = 2.5
    rv3d.view_location = (0.4, 0.3, 0.0)


@step
def rectangle_start():
    move((0.003, 0.002))  # near the origin: snaps to it
    key('R')


@step
def rectangle_corner_a():
    click((0.003, 0.002))


@step
def rectangle_corner_c():
    move((0.5, 0.35))
    click((0.6, 0.4))


@step
def rectangle_done():
    sk = sketch()
    check(len(sk.curves) == 4, "rectangle has 4 lines (%d)" % len(sk.curves))
    types = sorted(c.type for c in sk.constraints)
    check(types.count('HORIZONTAL') == 2 and types.count('VERTICAL') == 2 and 'COINCIDENT' in types,
          "rectangle got H/V constraints and snapped to the origin: %s" % types)
    corner = point_at((0.0, 0.0))
    check(corner is not None, "corner exactly at the origin")
    check(sk.dof == 2, "free size: 2 degrees of freedom (%d)" % sk.dof)
    screenshot("sketch_1_rectangle.png")


@step
def drag_corner_press():
    # Drag the far corner: the rectangle stays a rectangle anchored at the origin.
    move((0.6, 0.4))
    ev('LEFTMOUSE', 'PRESS', (0.6, 0.4))


@step
def drag_corner_move():
    move((0.65, 0.42))
    move((0.7, 0.45))


@step
def drag_corner_release():
    ev('LEFTMOUSE', 'RELEASE', (0.7, 0.45))


@step
def drag_done():
    far = max(sketch().points, key=lambda p: p.co[0] + p.co[1])
    check(abs(far.co[0] - 0.7) < 0.01 and abs(far.co[1] - 0.45) < 0.01,
          "dragged corner follows the mouse (%.3f, %.3f)" % tuple(far.co))
    check(point_at((0.0, 0.0)) is not None, "origin corner did not move")
    check(point_at((far.co[0], 0.0), 1e-6) is not None, "still a rectangle")


@step
def dimension_width():
    move((0.35, 0.0))
    key('D')


@step
def dimension_width_pick():
    click((0.35, 0.0))


@step
def dimension_width_place():
    click((0.35, -0.1))


@step
def dimension_width_set():
    # The value popup is open with its field active: type the value (bare numbers are mm) and
    # confirm the text, then the popup.
    type_text("500")
    key('RET')


@step
def dimension_width_ok():
    key('RET')


@step
def dimension_width_done():
    dims = [c for c in sketch().constraints if c.type == 'DISTANCE']
    check(len(dims) == 1, "width dimension added (%d)" % len(dims))
    check(abs(dims[0].length - 0.5) < 1e-6, "typed width (%.4f)" % dims[0].length)
    state["width_uid"] = dims[0].uid


@step
def dimension_height():
    move((0.0, 0.2))
    key('D')


@step
def dimension_height_pick():
    click((0.0, 0.2))


@step
def dimension_height_place():
    click((-0.1, 0.2))


@step
def dimension_height_set():
    type_text("300")
    key('RET')


@step
def dimension_height_ok():
    key('RET')


@step
def dimension_height_done():
    dims = [c for c in sketch().constraints if c.type == 'DISTANCE' and c.uid != state["width_uid"]]
    check(len(dims) == 1, "height dimension added")
    check(abs(dims[0].length - 0.3) < 1e-6, "typed height (%.4f)" % dims[0].length)


@step
def fully_constrained():
    sk = sketch()
    check(sk.dof == 0 and sk.solve_status == 'OK', "rectangle fully constrained (dof %d)" % sk.dof)
    check(point_at((0.5, 0.3)) is not None, "corner at (0.5, 0.3) from the dimensions")
    move((0.25, 0.15))
    screenshot("sketch_2_dimensioned.png")


@step
def triangle_start():
    move((0.8, 0.1))
    key('L')


@step
def triangle_1():
    click((0.8, 0.1))


@step
def triangle_2():
    click((1.0, 0.1))


@step
def triangle_3():
    click((0.9, 0.3))


@step
def triangle_close():
    click((0.8, 0.1))


@step
def triangle_done():
    sk = sketch()
    lines = [c for c in sk.curves]
    check(len(lines) == 7, "triangle closed with 3 more lines (%d)" % len(lines))
    h = [c for c in sk.constraints if c.type == 'HORIZONTAL']
    check(len(h) == 3, "the triangle's base got a horizontal constraint (%d)" % len(h))
    check(sk.points[-1].uid != 0, "points created")
    state["points_before_undo"] = len(sk.points)


@step
def undo_triangle():
    with override():
        bpy.ops.ed.undo()


@step
def undo_done():
    check(bpy.context.mode == 'EDIT_CAD', "still in sketch mode after undo (%s)" % bpy.context.mode)
    check(len(sketch().curves) == 4, "undo removed the triangle (%d curves)" % len(sketch().curves))
    check(sketch().dof == 0, "rectangle still fully constrained")


@step
def circle_start():
    move((0.9, 0.2))
    key('C')


@step
def circle_center():
    click((0.9, 0.2))


@step
def circle_rim():
    click((1.0, 0.2))


@step
def diameter_start():
    check(any(c.type == 'CIRCLE' for c in sketch().curves), "circle added")
    state["constraints_before_dims"] = len(sketch().constraints)
    move((0.9, 0.3))
    key('D')


@step
def diameter_pick():
    click((0.9, 0.3))


@step
def diameter_place():
    # One element: the click that places the label creates the dimension.
    click((1.15, 0.45))


@step
def distance_start():
    key('ESC')  # The value popup (text editing, then the popup).
    key('ESC')
    check(any(c.type == 'DIAMETER' for c in sketch().constraints), "diameter dimension added")
    move((0.9, 0.2))
    key('D')


@step
def distance_pick_a():
    click((0.9, 0.2))  # Circle center.


@step
def distance_pick_b():
    click((0.5, 0.3))  # Rectangle corner: two elements, the label follows the mouse.


@step
def distance_placing():
    dist = [c for c in sketch().constraints if c.type == 'DISTANCE' and c.uid != state["width_uid"]]
    check(len(dist) == 2, "point distance exists while its label is placed (%d)" % len(dist))
    move((0.65, 0.5))


@step
def distance_place():
    click((0.7, 0.55))


@step
def distance_cancel_start():
    key('ESC')  # The value popup (text editing, then the popup).
    key('ESC')
    sk = sketch()
    check(len(sk.constraints) == state["constraints_before_dims"] + 2, "both dimensions kept")
    check(sk.dof == 1, "circle center has one degree of freedom left (%d)" % sk.dof)
    check(all(c.is_constrained for c in sk.curves if c.type == 'LINE'), "rectangle lines fully constrained")
    check(not next(c for c in sk.curves if c.type == 'CIRCLE').is_constrained, "circle can still move")
    move((0.9, 0.2))


@step
def distance_cancel_start_tool():
    screenshot("sketch_2b_dimensions.png")
    key('D')


@step
def distance_cancel_pick_a():
    click((0.9, 0.2))


@step
def distance_cancel_pick_b():
    click((0.0, 0.0))  # The sketch origin.


@step
def distance_cancel_escape():
    key('ESC')  # Cancel while placing removes the new dimension.


@step
def distance_cancel_done():
    check(len(sketch().constraints) == state["constraints_before_dims"] + 2,
          "cancelled dimension removed")
    with override():
        for _ in range(3):
            bpy.ops.ed.undo()


@step
def circle_undone():
    sk = sketch()
    check(len(sk.curves) == 4 and len(sk.constraints) == state["constraints_before_dims"],
          "undo removed the circle and its dimensions (%d curves)" % len(sk.curves))
    check(sk.dof == 0, "rectangle still fully constrained")


@step
def finish():
    key('RET', ctrl=True)


@step
def extrude():
    check(bpy.context.mode == 'OBJECT', "Ctrl+Enter finishes the sketch (%s)" % bpy.context.mode)
    check(part().rollback_feature is None, "rollback restored")
    with override():
        bpy.ops.cad.feature_add('EXEC_DEFAULT', True, type='EXTRUDE')
    ext = part().features.active
    ext["distance"] = 0.2
    part().update_tag()


@step
def extrude_done():
    ext = part().features["Extrude1"]
    ob_eval = bpy.data.objects["Part"].evaluated_get(bpy.context.evaluated_depsgraph_get())
    check(ext.status == 'OK', "extrude of the sketch (%s %s)" % (ext.status, ext.status_message))
    d = ob_eval.dimensions
    check(abs(d.x - 0.5) < 1e-4 and abs(d.y - 0.3) < 1e-4 and abs(d.z - 0.2) < 1e-4,
          "solid is 0.5 x 0.3 x 0.2 (%s)" % (tuple(round(v, 4) for v in d),))
    with override():
        bpy.ops.view3d.view_axis(type='FRONT')
        bpy.ops.view3d.view_orbit(angle=0.6, type='ORBITUP')
        bpy.ops.view3d.view_orbit(angle=0.6, type='ORBITLEFT')
        bpy.ops.view3d.view_all()


@step
def edit_again():
    screenshot("sketch_3_extruded.png")
    with override():
        bpy.ops.cad.sketch_edit('EXEC_DEFAULT', True, feature_uid=part().features["Sketch1"].uid, look_at=True)
    check(bpy.context.mode == 'EDIT_CAD', "edit the sketch again")
    check(part().rollback_feature.name == "Sketch1", "model rolled back to the sketch while editing")
    width = next(c for c in sketch().constraints if c.uid == state["width_uid"])
    width.length = 0.8


@step
def edit_again_done():
    key('TAB')


@step
def final():
    check(bpy.context.mode == 'OBJECT', "Tab leaves sketch mode")
    ob_eval = bpy.data.objects["Part"].evaluated_get(bpy.context.evaluated_depsgraph_get())
    check(abs(ob_eval.dimensions.x - 0.8) < 1e-4, "new width flows into the solid (%.4f)" % ob_eval.dimensions.x)


@step
def undo_exit():
    # Undo leaving sketch mode: back in the sketch as it was when editing started.
    with override():
        bpy.ops.ed.undo()


@step
def undo_exit_done():
    check(bpy.context.mode == 'EDIT_CAD', "undo re-enters sketch mode (%s)" % bpy.context.mode)
    check(part().rollback_feature is not None and part().rollback_feature.name == "Sketch1",
          "rolled back to the sketch again")
    width = next(c for c in sketch().constraints if c.uid == state["width_uid"])
    check(abs(width.length - 0.5) < 1e-6, "width as it was when editing started (%.4f)" % width.length)
    key('TAB')


@step
def final_again():
    check(bpy.context.mode == 'OBJECT', "Tab leaves sketch mode again")
    check(part().rollback_feature is None, "rollback restored after undo")
    ob_eval = bpy.data.objects["Part"].evaluated_get(bpy.context.evaluated_depsgraph_get())
    check(abs(ob_eval.dimensions.x - 0.5) < 1e-4, "solid width back to 0.5 (%.4f)" % ob_eval.dimensions.x)
    print("ALL OK (GUI sketch)")
    sys.stdout.flush()
    bpy.ops.wm.quit_blender()


def run():
    fn = steps.pop(0)
    try:
        fn()
    except Exception:
        import traceback
        traceback.print_exc()
        sys.stdout.flush()
        os._exit(1)
    return 0.4 if steps else None


bpy.app.timers.register(run, first_interval=2.0)
