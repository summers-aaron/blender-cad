# SPDX-FileCopyrightText: 2026 SimpleCAD Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Interactive test of CAD edge picking with simulated mouse and keyboard events. Needs a display
(e.g. Xvfb); screenshots are written when SHOT_DIR is set.

  xvfb-run -a -s "-screen 0 1920x1080x24" ./build/bin/blender --factory-startup \
      --enable-event-simulate --python tests/python/cad_gui_pick.py
"""

import os
import sys

import bpy
from bpy_extras.view3d_utils import location_3d_to_region_2d

OUT = os.environ.get("SHOT_DIR", "")
bpy.context.preferences.view.show_splash = False
steps = []
step = steps.append
state = {}


def v3d():
    win = bpy.context.window_manager.windows[0]
    area = next(a for a in win.screen.areas if a.type == 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    return win, area, region


def to_window(co):
    win, area, region = v3d()
    p = location_3d_to_region_2d(region, area.spaces[0].region_3d, co)
    return int(region.x + p.x), int(region.y + p.y)


def move(co):
    win = v3d()[0]
    x, y = to_window(co)
    win.event_simulate(type='MOUSEMOVE', value='NOTHING', x=x, y=y)


def click(co):
    win = v3d()[0]
    x, y = to_window(co)
    win.event_simulate(type='MOUSEMOVE', value='NOTHING', x=x, y=y)
    win.event_simulate(type='LEFTMOUSE', value='PRESS', x=x, y=y)
    win.event_simulate(type='LEFTMOUSE', value='RELEASE', x=x, y=y)


def screenshot(name):
    if not OUT:
        return
    win, area, region = v3d()
    with bpy.context.temp_override(window=win, area=area, region=region):
        bpy.ops.screen.screenshot_area(filepath=os.path.join(OUT, name))


def check(cond, msg):
    if not cond:
        print("FAIL:", msg)
        sys.stdout.flush()
        os._exit(1)
    print("ok:", msg)


def part():
    return bpy.data.objects["Part"].data


def expected():
    uid = state["box_uid"]
    return ["F%d:front|F%d:top" % (uid, uid), "F%d:right|F%d:top" % (uid, uid)]


@step
def setup():
    win, area, region = v3d()
    with bpy.context.temp_override(window=win, area=area, region=region):
        bpy.ops.object.select_all(action='SELECT')
        bpy.ops.object.delete()
        bpy.ops.object.cad_part_add(size=2.0)
        argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
        part().kernel = argv[0] if argv else 'OCCT'
        bpy.ops.cad.feature_add(type='FILLET')
        part().features.active["radius"] = 0.25
        bpy.ops.ed.undo_push(message="Fillet added")
        bpy.ops.view3d.view_selected()
    bpy.context.view_layer.objects.active.select_set(False)
    state["box_uid"] = part().features["Box1"].uid


@step
def start_pick():
    # Started from the Properties editor button, like a user would.
    win = v3d()[0]
    props = next(a for a in win.screen.areas if a.type == 'PROPERTIES')
    props.spaces[0].context = 'DATA'
    region = next(r for r in props.regions if r.type == 'WINDOW')
    with bpy.context.temp_override(window=win, area=props, region=region):
        bpy.ops.cad.references_pick('INVOKE_DEFAULT')
    rb = part().rollback_feature
    check(rb is not None and rb.name == "Box1", "model rolled back to the fillet input while picking")


@step
def pick_two():
    click((0.0, -1.0, 1.0))   # top/front
    click((1.0, 0.0, 1.0))    # top/right


@step
def hover_third():
    move((-0.4, -1.0, -1.0))  # bottom/front, hovered only


@step
def shot_picking():
    screenshot("pick_1_modal.png")
    names = sorted(r.name for r in part().features["Fillet1"].references)
    check(names == expected(), "two edges picked while modal: %s" % names)


@step
def toggle_off_and_on():
    # Clicking a picked edge again removes it; pick it once more.
    click((0.3, -1.0, 1.0))
    click((0.3, -1.0, 1.0))


@step
def confirm():
    win, area, region = v3d()
    x, y = to_window((0.0, 0.0, 0.0))
    win.event_simulate(type='RET', value='PRESS', x=x, y=y)
    win.event_simulate(type='RET', value='RELEASE', x=x, y=y)


@step
def after_confirm():
    f = part().features["Fillet1"]
    bpy.context.evaluated_depsgraph_get()
    names = sorted(r.name for r in f.references)
    check(names == expected(), "re-clicked edge toggled off and on: %s" % names)
    check(f.status == 'OK', "fillet evaluates (%s %s)" % (f.status, f.status_message))
    check(part().rollback_feature is None, "rollback restored after Enter")


@step
def shot_done():
    screenshot("pick_2_done.png")


@step
def resize_box():
    box = part().features["Box1"]
    box["size"] = (3.0, 2.0, 1.5)
    part().update_tag()


@step
def after_resize():
    f = part().features["Fillet1"]
    bpy.context.evaluated_depsgraph_get()
    check(f.status == 'OK' and not any(r.is_lost for r in f.references),
          "picked edges still resolve after resizing the box (%s %s)" % (f.status, f.status_message))
    screenshot("pick_3_resized.png")


@step
def undo_pick():
    win, area, region = v3d()
    with bpy.context.temp_override(window=win, area=area, region=region):
        bpy.ops.ed.undo_push(message="Resize")
        bpy.ops.ed.undo()  # back to resize-less state after pick
        bpy.ops.ed.undo()  # before the pick
    names = [r.name for r in part().features["Fillet1"].references]
    check(names == [], "undo removes the picked edges: %s" % names)


@step
def finish():
    print("ALL OK (GUI pick)")
    sys.stdout.flush()
    bpy.ops.wm.quit_blender()


def run():
    fn = steps.pop(0)
    try:
        fn()
    except Exception:
        import traceback
        traceback.print_exc()
        os._exit(1)
    return 0.7 if steps else None


bpy.app.timers.register(run, first_interval=2.0)
