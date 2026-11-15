# SPDX-FileCopyrightText: 2026 SimpleCAD Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Headless smoke test of the CAD part UI layouts: runs poll/draw of every panel and UI list in
`bl_ui.properties_data_cad` against a layout stand-in that checks each referenced property,
ID property, operator and enum value exists. Catches typos and API drift without a window.

  ./build/bin/blender -b --factory-startup --python tests/python/cad_ui_smoke.py
"""

import sys
from types import SimpleNamespace

import bpy
from bl_ui import properties_data_cad as ui


class UIError(Exception):
    pass


def op_rna(idname):
    mod, name = idname.split(".")
    try:
        return getattr(getattr(bpy.ops, mod), name).get_rna_type()
    except (AttributeError, KeyError):
        raise UIError("unknown operator %r" % idname)


class OpProps:
    def __init__(self, idname):
        object.__setattr__(self, "_idname", idname)
        object.__setattr__(self, "_rna", op_rna(idname))

    def __setattr__(self, key, value):
        props = self._rna.properties
        if key not in props:
            raise UIError("%s has no property %r" % (self._idname, key))
        prop = props[key]
        if prop.type == 'ENUM' and value not in {item.identifier for item in prop.enum_items}:
            raise UIError("%s.%s has no enum item %r" % (self._idname, key, value))


class Layout:
    """Accepts the `UILayout` calls the CAD panels use and checks their arguments."""

    def __init__(self, log):
        self._log = log
        self.use_property_split = False
        self.use_property_decorate = False
        self.active = True
        self.enabled = True
        self.alert = False

    def _sub(self, *_args, **_kw):
        return Layout(self._log)

    row = column = box = split = grid_flow = column_flow = _sub

    def separator(self, **_kw):
        pass

    def label(self, *, text="", icon='NONE', **_kw):
        self._check_icon(icon)
        self._log.append("label " + text)

    def _check_icon(self, icon):
        items = bpy.types.UILayout.bl_rna.functions["label"].parameters["icon"].enum_items
        if icon not in items:
            raise UIError("unknown icon %r" % icon)

    def prop(self, data, prop, *, icon='NONE', **_kw):
        self._check_icon(icon)
        if prop.startswith('["'):
            key = prop[2:-2].encode().decode("unicode_escape")
            if key not in data.keys():
                raise UIError("%r has no ID property %r" % (data, key))
        elif prop not in data.bl_rna.properties:
            raise UIError("%s has no property %r" % (data.bl_rna.identifier, prop))
        self._log.append("prop %s.%s" % (data.bl_rna.identifier, prop))

    def operator(self, idname, *, icon='NONE', **_kw):
        self._check_icon(icon)
        self._log.append("operator " + idname)
        return OpProps(idname)

    def operator_menu_enum(self, idname, prop, *, icon='NONE', **_kw):
        self._check_icon(icon)
        if prop not in op_rna(idname).properties:
            raise UIError("%s has no property %r" % (idname, prop))
        self._log.append("operator_menu_enum %s.%s" % (idname, prop))

    def template_ID(self, data, prop, **_kw):
        if prop not in data.bl_rna.properties:
            raise UIError("%s has no property %r" % (data.bl_rna.identifier, prop))

    def template_list(self, list_type, _list_id, data, propname, active_data, active_propname, **_kw):
        if propname not in data.bl_rna.properties:
            raise UIError("%s has no collection %r" % (data.bl_rna.identifier, propname))
        if active_propname not in active_data.bl_rna.properties:
            raise UIError("%s has no property %r" % (active_data.bl_rna.identifier, active_propname))
        ul = getattr(bpy.types, list_type)
        for index, item in enumerate(getattr(data, propname)):
            ul.draw_item(SimpleNamespace(), None, Layout(self._log), data, item, 0,
                         active_data, active_propname, index)
        self._log.append("template_list %s" % list_type)


def draw_panels(context, label):
    failures = []
    drawn = []
    for cls in ui.classes:
        if not issubclass(cls, bpy.types.Panel):
            continue
        if cls is ui.DATA_PT_custom_props_cad_part:
            continue  # Generic rna_prop_ui code.
        try:
            if not cls.poll(context):
                continue
            log = []
            panel = SimpleNamespace(layout=Layout(log))
            if hasattr(cls, "draw_header"):
                cls.draw_header(panel, context)
            cls.draw(panel, context)
            drawn.append(cls.__name__)
        except Exception as ex:
            failures.append("%s [%s]: %s: %s" % (cls.__name__, label, type(ex).__name__, ex))
    return drawn, failures


def main():
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete()
    bpy.ops.object.cad_part_add(size=2.0)
    ob = bpy.context.object
    part = ob.data

    # One of each feature, a sketch, an error and a reference, so every branch draws.
    sketch = part.features.new('SKETCH')
    sk = sketch.sketch
    pts = [sk.point_add(co) for co in ((-0.4, -0.4), (0.4, -0.4), (0.4, 0.4))]
    for i in range(3):
        sk.line_add(pts[i], pts[(i + 1) % 3])
    extrude = part.features.new('EXTRUDE')
    extrude["sketch"] = sketch.uid
    fillet = part.features.new('FILLET')
    fillet.reference_add("F1:front|F1:top", point=(0.0, -1.0, 1.0))
    chamfer = part.features.new('CHAMFER')
    chamfer["distance"] = 50.0
    chamfer.reference_add("F1:missing", point=(0.0, 0.0, 0.0))
    part.update_tag()
    bpy.context.evaluated_depsgraph_get()

    context = SimpleNamespace(
        object=ob,
        cad_part=part,
        scene=bpy.context.scene,
        space_data=SimpleNamespace(pin_id=None),
    )

    failures = []
    for feature in part.features:
        part.features.active = feature
        drawn, fails = draw_panels(context, feature.name + "/" + feature.status)
        failures += fails
        print("ok: drew %s with active %s (%s)" % (", ".join(drawn), feature.name, feature.status))

    part.rollback_feature = sketch
    _drawn, fails = draw_panels(context, "rollback")
    failures += fails

    # Panels must not draw for other objects.
    context.cad_part = None
    for cls in ui.classes:
        if issubclass(cls, bpy.types.Panel) and cls.poll(context):
            failures.append("%s polls true without a CAD part" % cls.__name__)

    # The Add menu and File > Export entries exist.
    op_rna("object.cad_part_add")
    op_rna("wm.step_export")

    # The History strip context menu, for every feature.
    from bl_ui import space_view3d_cad
    menu_context = SimpleNamespace(active_object=ob)
    for feature in part.features:
        part.features.active = feature
        feature.suppress = not feature.suppress
        for _ in range(2):
            try:
                space_view3d_cad.CAD_MT_history_context.draw(
                    SimpleNamespace(layout=Layout([])), menu_context)
            except Exception as ex:
                failures.append("CAD_MT_history_context [%s]: %s: %s" % (feature.name, type(ex).__name__, ex))
            feature.suppress = not feature.suppress

    # CAD workspaces get a History strip (the 3D View footer) in every 3D View.
    workspace = bpy.data.workspaces["Layout"]
    workspace.use_cad_ui = True
    if not workspace.use_cad_ui:
        failures.append("use_cad_ui not set")
    for screen in workspace.screens:
        for area in screen.areas:
            if area.type == 'VIEW_3D' and not any(r.type == 'FOOTER' for r in area.regions):
                failures.append("no History strip region in a 3D View of %s" % screen.name)
    workspace.use_cad_ui = False
    print("ok: CAD workspace flag")

    if failures:
        for f in failures:
            print("FAIL:", f)
        sys.exit(1)
    print("ALL OK (UI)")


main()
