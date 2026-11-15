# SPDX-FileCopyrightText: 2026 SimpleCAD Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Interactive test of feature dialogs with simulated mouse and keyboard events: new Extrude (E),
sidebar dialog, distance arrow gizmo, OK / Cancel / undo, editing, and typing an expression into
a dimension. Needs a display (Xvfb); screenshots are written when SHOT_DIR is set.

  xvfb-run -a -s "-screen 0 1920x1080x24" env WAYLAND_DISPLAY=no-such-display \\
      ./build/bin/blender --factory-startup --enable-event-simulate \\
      --python tests/python/cad_gui_dialog.py -- [OCCT|SMLIB]
"""

import math
import os
import subprocess
import sys

import bpy
from bpy_extras.view3d_utils import location_3d_to_region_2d
from mathutils import Euler, Vector

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
    """3D location to window pixels."""
    _win, area, region = v3d()
    p = location_3d_to_region_2d(region, area.spaces[0].region_3d, co)
    return int(region.x + p.x), int(region.y + p.y)


def ev(type, value, xy=None, **kw):
    win = v3d()[0]
    if xy is None:
        xy = state.get("mouse", (700, 500))
    state["mouse"] = xy
    win.event_simulate(type=type, value=value, x=xy[0], y=xy[1], **kw)


def key(k, **mods):
    ev(k, 'PRESS', **mods)
    ev(k, 'RELEASE', **mods)


_CHAR_KEYS = {'/': 'SLASH', '*': 'NDOF_BUTTON_MENU', ' ': 'SPACE', '.': 'PERIOD'}
_DIGITS = ['ZERO', 'ONE', 'TWO', 'THREE', 'FOUR', 'FIVE', 'SIX', 'SEVEN', 'EIGHT', 'NINE']


def type_text(text):
    for ch in text:
        if ch.isdigit():
            k = _DIGITS[int(ch)]
        elif ch.isalpha():
            k = ch.upper()
        else:
            k = _CHAR_KEYS[ch]
        ev(k, 'PRESS', unicode=ch)
        ev(k, 'RELEASE')


def screenshot(name):
    # Captures the (virtual) X screen: reading back Blender's window is black with Mesa
    # llvmpipe on Xvfb. Needs ImageMagick's `import`.
    if OUT and os.environ.get("DISPLAY"):
        subprocess.run(["import", "-window", "root", "-display", os.environ["DISPLAY"],
                        os.path.join(OUT, name)], check=False)


def ob():
    return bpy.data.objects["Part"]


def part():
    return ob().data


def extrudes():
    return [f for f in part().features if f.type == 'EXTRUDE']


def top_z():
    ob_eval = ob().evaluated_get(bpy.context.evaluated_depsgraph_get())
    return max(v[2] for v in ob_eval.bound_box)


def sidebar():
    _win, area, _region = v3d()
    return next(r for r in area.regions if r.type == 'UI'), area


@step
def setup():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    with override():
        bpy.ops.object.select_all(action='SELECT')
        bpy.ops.object.delete()
        bpy.ops.object.cad_part_add(add_box=False)
    p = part()
    p.kernel = argv[0] if argv else 'OCCT'
    p.parameters.new(name="width", expression="500 mm")
    sketch = p.features.new(type='SKETCH')
    sk = sketch.sketch
    pts = [sk.point_add(co) for co in ((0.0, 0.0), (0.5, 0.0), (0.5, 0.4), (0.0, 0.4))]
    lines = [sk.line_add(pts[i], pts[(i + 1) % 4]) for i in range(4)]
    for i in range(4):
        sk.constraint_add(type='HORIZONTAL' if i % 2 == 0 else 'VERTICAL', entities=[lines[i]])
    state["dim_uid"] = sk.constraint_add(type='DISTANCE', entities=[lines[0]], value=0.5)
    p.features.active = sketch
    check(bpy.context.mode == 'OBJECT', "object mode")
    # Scripted setup makes no undo steps of its own.
    with override():
        bpy.ops.ed.undo_push(message="Setup")

    # An oblique view so the extrusion direction is visible.
    _win, area, _region = v3d()
    space = area.spaces[0]
    space.show_region_ui = False
    rv3d = space.region_3d
    rv3d.view_perspective = 'PERSP'
    rv3d.view_rotation = Euler((math.radians(60), 0.0, math.radians(30))).to_quaternion()
    rv3d.view_distance = 2.2
    rv3d.view_location = (0.25, 0.2, 0.1)


@step
def new_extrude_key():
    # E in the viewport: new Extrude of the active sketch, in a dialog.
    ev('MOUSEMOVE', 'NOTHING', xy=to_window((0.6, -0.3, 0.0)))
    key('E')


@step
def new_extrude_open():
    dialog = part().dialog_feature
    check(dialog is not None and dialog.type == 'EXTRUDE' and dialog.is_new, "E opens a new Extrude dialog")
    check(dialog.profile == "Sketch1", "profile is the active sketch")
    region, area = sidebar()
    check(area.spaces[0].show_region_ui, "sidebar shown")
    check(region.active_panel_category == "Feature", "Feature tab active (%s)" % region.active_panel_category)
    state["top0"] = top_z()
    check(abs(state["top0"] - 0.01) < 1e-4, "preview at the default distance (top %.4f)" % state["top0"])
    screenshot("dialog_1_new_extrude.png")


@step
def gizmo_hover():
    # The arrow starts at the end of the extrusion, in the middle of the profile, along +Z.
    base = Vector((0.25, 0.2, 0.01))
    a, b = Vector(to_window(base)), Vector(to_window(base + Vector((0.0, 0.0, 0.2))))
    direction = (b - a).normalized()
    state["dir"] = direction
    grab = a + direction * 35.0
    state["grab"] = grab
    ev('MOUSEMOVE', 'NOTHING', xy=(int(grab.x), int(grab.y)))


@step
def gizmo_press():
    grab = state["grab"]
    ev('LEFTMOUSE', 'PRESS', xy=(int(grab.x), int(grab.y)))
    for i in range(1, 11):
        p = grab + state["dir"] * (8.0 * i)
        ev('MOUSEMOVE', 'NOTHING', xy=(int(p.x), int(p.y)))


@step
def gizmo_release():
    ev('LEFTMOUSE', 'RELEASE')


@step
def gizmo_dragged():
    dialog = part().dialog_feature
    distance = dialog.inputs["distance"].value
    check(distance > 0.05, "dragging the arrow lengthens the extrusion (%.4f)" % distance)
    check(abs(top_z() - distance) < 1e-4, "preview follows the drag (top %.4f)" % top_z())
    # Bind it to the parameter, like typing it in the dialog.
    dialog.inputs["distance"].expression = "width / 5"
    check(abs(dialog.inputs["distance"].value - 0.1) < 1e-6, "distance bound to a parameter")
    screenshot("dialog_2_dragged.png")


@step
def ok_key():
    ev('MOUSEMOVE', 'NOTHING', xy=to_window((0.6, -0.3, 0.0)))
    key('RET')


@step
def ok_done():
    check(part().dialog_feature is None, "Enter confirms the dialog")
    check(len(extrudes()) == 1 and extrudes()[0].inputs["distance"].expression == "width / 5",
          "extrude kept with its expression")
    state["extrude"] = extrudes()[0].name
    key('Z', ctrl=True)


@step
def undo_ok():
    check(len(extrudes()) == 0, "one undo removes the confirmed extrude")
    key('Z', ctrl=True, shift=True)


@step
def redo_ok():
    check(len(extrudes()) == 1 and part().dialog_feature is None, "redo brings it back")
    # Edit it: rolled back to it; Esc cancels.
    with override():
        bpy.ops.cad.feature_edit('INVOKE_DEFAULT', feature_uid=extrudes()[0].uid)


@step
def edit_open():
    dialog = part().dialog_feature
    check(dialog is not None and not dialog.is_new, "edit dialog open")
    dialog.inputs["distance"].expression = "30 mm"
    check(abs(top_z() - 0.03) < 1e-4, "edit previews (top %.4f)" % top_z())
    key('ESC')


@step
def edit_cancelled():
    check(part().dialog_feature is None, "Esc cancels")
    check(extrudes()[0].inputs["distance"].expression == "width / 5", "cancel restores the input")
    check(abs(top_z() - 0.1) < 1e-4, "model restored (top %.4f)" % top_z())
    # Undo while a dialog is open cancels only the dialog.
    key('E')


@step
def second_extrude_open():
    check(part().dialog_feature is not None and len(extrudes()) == 2, "second Extrude dialog")
    key('Z', ctrl=True)


@step
def undo_in_dialog():
    check(part().dialog_feature is None and len(extrudes()) == 1,
          "Ctrl+Z in a dialog cancels it and keeps earlier work (%d extrudes)" % len(extrudes()))
    # Type an expression into a dimension: sketch mode, then the value popup.
    with override():
        bpy.ops.cad.feature_edit('INVOKE_DEFAULT', feature_uid=part().features["Sketch1"].uid)


@step
def dimension_popup():
    check(bpy.context.mode == 'EDIT_CAD', "editing a sketch from the dialog operator")
    ev('MOUSEMOVE', 'NOTHING', xy=to_window((0.25, -0.2, 0.0)))
    with override():
        bpy.ops.cad.sketch_dimension_edit('INVOKE_DEFAULT', constraint_uid=state["dim_uid"])


@step
def dimension_type():
    screenshot("dialog_3a_dimension_popup.png")
    # The value field is active right away: typing replaces it.
    type_text("width/2")


@step
def dimension_apply():
    screenshot("dialog_3_dimension_expression.png")
    key('RET')


@step
def dimension_confirm():
    screenshot("dialog_3b_dimension_applied.png")
    key('RET')


@step
def dimension_done():
    sk = part().features["Sketch1"].sketch
    dim = next(c for c in sk.constraints if c.uid == state["dim_uid"])
    check(dim.expression == "width/2", "dimension takes the typed expression (%s)" % dim.expression)
    check(abs(dim.length - 0.25) < 1e-6, "dimension evaluated (%.4f)" % dim.length)
    key('TAB')


@step
def parameter_drives_all():
    check(bpy.context.mode == 'OBJECT', "Tab leaves sketch mode")
    part().parameters["width"].expression = "800 mm"
    ob_eval = ob().evaluated_get(bpy.context.evaluated_depsgraph_get())
    check(abs(ob_eval.dimensions.x - 0.4) < 1e-4, "dimension follows the parameter (%.4f)" % ob_eval.dimensions.x)
    check(abs(ob_eval.dimensions.z - 0.16) < 1e-4, "extrude follows the parameter (%.4f)" % ob_eval.dimensions.z)
    screenshot("dialog_4_parameters.png")
    print("ALL OK (GUI dialog)")
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
