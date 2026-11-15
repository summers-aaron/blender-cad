# SPDX-FileCopyrightText: 2026 SimpleCAD Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Interactive test of the CAD workspace and its History strip with simulated events: chips,
selection, double-click to edit, the rollback marker, the context menu. Needs a display (Xvfb);
screenshots are written when SHOT_DIR is set.

  xvfb-run -a -s "-screen 0 1920x1080x24" env WAYLAND_DISPLAY=no-such-display \\
      ./build/bin/blender --factory-startup --enable-event-simulate \\
      --python tests/python/cad_gui_history.py -- [OCCT|SMLIB]
"""

import os
import subprocess
import sys

import blf
import bpy

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


def win():
    return bpy.context.window_manager.windows[0]


def v3d_area():
    return next(a for a in win().screen.areas if a.type == 'VIEW_3D')


def region(type):
    return next((r for r in v3d_area().regions if r.type == type), None)


def footer_height():
    footer = region('FOOTER')
    return footer.height if footer else 0


def override(region_type='WINDOW'):
    area = v3d_area()
    return bpy.context.temp_override(window=win(), area=area, region=region(region_type))


def ev(type, value, xy=None, **kw):
    if xy is None:
        xy = state.get("mouse", (700, 500))
    state["mouse"] = xy
    win().event_simulate(type=type, value=value, x=int(xy[0]), y=int(xy[1]), **kw)


def key(k, **mods):
    ev(k, 'PRESS', **mods)
    ev(k, 'RELEASE', **mods)


def click(xy):
    ev('MOUSEMOVE', 'NOTHING', xy)
    ev('LEFTMOUSE', 'PRESS', xy)
    ev('LEFTMOUSE', 'RELEASE', xy)


def screenshot(name):
    # Captures the (virtual) X screen: reading back Blender's window is black with Mesa
    # llvmpipe on Xvfb. Needs ImageMagick's `import`.
    if OUT and os.environ.get("DISPLAY"):
        subprocess.run(["import", "-window", "root", "-display", os.environ["DISPLAY"],
                        os.path.join(OUT, name)], check=False)


def part():
    return bpy.data.objects["Part"].data


def chips():
    """Chip rectangles (window x ranges) and the marker x, like the History strip lays them out
    (see `history_layout` in cad_history.cc): 6 px margins and gaps, an icon, the name."""
    footer = region('FOOTER')
    scale = bpy.context.preferences.system.ui_scale
    style = bpy.context.preferences.ui_styles[0].widget
    blf.size(0, style.points * scale)
    pad = 6 * scale
    x = 6 * scale
    result = []
    p = part()
    marker_x = None
    for f in p.features:
        width = pad + 16 * scale + pad * 0.75 + blf.dimensions(0, f.name)[0] + pad
        if f.status in {'WARNING', 'ERROR'}:
            width += 16 * scale + pad * 0.5
        result.append((f.name, footer.x + x, footer.x + x + width))
        x += width + 6 * scale
        if p.rollback_feature == f:
            marker_x = footer.x + x - 3 * scale
    if marker_x is None:
        marker_x = footer.x + x - 3 * scale
    return result, marker_x


def chip_center(name):
    footer = region('FOOTER')
    for n, x0, x1 in chips()[0]:
        if n == name:
            return ((x0 + x1) / 2, footer.y + footer.height / 2)
    raise KeyError(name)


@step
def setup():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    with override():
        bpy.ops.object.select_all(action='SELECT')
        bpy.ops.object.delete()
        bpy.ops.object.cad_part_add(add_box=False)
    p = part()
    p.kernel = argv[0] if argv else 'OCCT'
    sketch = p.features.new(type='SKETCH')
    sk = sketch.sketch
    pts = [sk.point_add(co) for co in ((0.0, 0.0), (0.5, 0.0), (0.5, 0.4), (0.0, 0.4))]
    for i in range(4):
        sk.line_add(pts[i], pts[(i + 1) % 4])
    extrude = p.features.new(type='EXTRUDE')
    extrude["sketch"] = sketch.uid
    extrude["distance"] = 0.2
    p.features.active = sketch
    with override():
        bpy.ops.ed.undo_push(message="Setup")
    state["workspaces"] = len(bpy.data.workspaces)
    # The strip only shows in CAD workspaces.
    check(footer_height() <= 1, "no History strip outside CAD workspaces (%d)" % footer_height())
    with bpy.context.temp_override(window=win()):
        bpy.ops.cad.workspace_add()


@step
def workspace_added():
    ws = bpy.context.window.workspace
    check(ws.name.startswith("CAD") and ws.use_cad_ui, "CAD workspace active (%s)" % ws.name)
    check(len(bpy.data.workspaces) == state["workspaces"] + 1, "one workspace added")
    footer = region('FOOTER')
    check(footer.height >= 30, "History strip shown (%d px)" % footer.height)
    ev('MOUSEMOVE', 'NOTHING', chip_center("Extrude1"))


@step
def hover_shot():
    screenshot("history_1_strip.png")
    click(chip_center("Extrude1"))


def double_click(xy):
    # Simulated events carry no time stamps, so they never make a DOUBLE_CLICK: check the key
    # map item and invoke its operator under the cursor instead.
    km = bpy.context.window_manager.keyconfigs.active.keymaps["CAD History"]
    check(any(i.idname == "cad.history_edit" and i.value == 'DOUBLE_CLICK' for i in km.keymap_items),
          "double-click edits")
    ev('MOUSEMOVE', 'NOTHING', xy)
    with override('FOOTER'):
        bpy.ops.cad.history_edit('INVOKE_DEFAULT')


@step
def clicked():
    check(part().features.active.name == "Extrude1", "click makes the feature active")
    ev('MOUSEMOVE', 'NOTHING', chip_center("Sketch1"))


@step
def double_click_sketch():
    double_click(chip_center("Sketch1"))


@step
def double_clicked_sketch():
    check(bpy.context.mode == 'EDIT_CAD', "double-click on a sketch edits it (%s)" % bpy.context.mode)
    with override():
        bpy.ops.object.mode_set(mode='OBJECT')


@step
def double_click_extrude():
    double_click(chip_center("Extrude1"))


@step
def dialog_from_history():
    dialog = part().dialog_feature
    check(dialog is not None and dialog.name == "Extrude1" and not dialog.is_new,
          "double-click on a feature opens its dialog")
    screenshot("history_2_edit.png")
    with override():
        bpy.ops.cad.dialog_cancel()


@step
def drag_marker():
    check(part().dialog_feature is None, "dialog cancelled")
    check(part().rollback_feature is None, "marker at the end")
    footer = region('FOOTER')
    y = footer.y + footer.height / 2
    _chips, marker_x = chips()
    state["marker_x"] = marker_x
    sketch_end = chips()[0][0][2]
    ev('MOUSEMOVE', 'NOTHING', (marker_x, y))
    ev('LEFTMOUSE', 'PRESS', (marker_x, y))
    for i in range(1, 9):
        x = marker_x + (sketch_end + 3 - marker_x) * i / 8
        ev('MOUSEMOVE', 'NOTHING', (x, y))


@step
def drag_release():
    ev('LEFTMOUSE', 'RELEASE')


@step
def dragged():
    p = part()
    check(p.rollback_feature is not None and p.rollback_feature.name == "Sketch1",
          "dragging the marker rolls back (%s)" % (p.rollback_feature.name if p.rollback_feature else None))
    screenshot("history_3_rolled_back.png")
    ev('MOUSEMOVE', 'NOTHING', chip_center("Sketch1"))
    key('Z', ctrl=True)


@step
def undone():
    check(part().rollback_feature is None, "undo moves the marker back to the end")
    xy = chip_center("Extrude1")
    ev('MOUSEMOVE', 'NOTHING', xy)
    ev('RIGHTMOUSE', 'PRESS', xy)
    ev('RIGHTMOUSE', 'RELEASE', xy)


@step
def context_menu():
    screenshot("history_4_menu.png")
    check(part().features.active.name == "Extrude1", "right-click makes the feature active")
    key('ESC')


@step
def back_to_layout():
    with bpy.context.temp_override(window=win()):
        bpy.context.window.workspace = bpy.data.workspaces["Layout"]


@step
def hidden_again():
    check(footer_height() <= 1, "History strip hidden in other workspaces (%d)" % footer_height())
    print("ALL OK (GUI history)")
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
