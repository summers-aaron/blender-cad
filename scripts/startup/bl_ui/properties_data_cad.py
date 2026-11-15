# SPDX-FileCopyrightText: 2026 SimpleCAD Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import bpy
from bpy.types import Panel, UIList
from rna_prop_ui import PropertyPanel


_FEATURE_ICONS = {
    'SKETCH': 'GREASEPENCIL',
    'BOX': 'MESH_CUBE',
    'EXTRUDE': 'MOD_SOLIDIFY',
    'FILLET': 'MOD_BEVEL',
    'CHAMFER': 'MOD_BEVEL',
}

_STATUS_ICONS = {
    'WARNING': 'ERROR',
    'ERROR': 'CANCEL',
}

# Parameters that are drawn specially or are internal.
_HIDDEN_PARAMS = {"sketch", "regions"}


class DataButtonsPanel:
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = "data"

    @classmethod
    def poll(cls, context):
        engine = context.scene.render.engine
        return getattr(context, "cad_part", None) and (engine in cls.COMPAT_ENGINES)


_COMPAT_ENGINES = {
    'BLENDER_RENDER',
    'BLENDER_EEVEE',
    'BLENDER_WORKBENCH',
}


class CAD_UL_features(UIList):
    def draw_item(self, _context, layout, data, feature, _icon, _active_data, _active_propname, _index):
        part = data
        row = layout.row(align=True)
        rolled_back = feature.status == 'ROLLED_BACK'
        row.active = not (feature.suppress or rolled_back)
        row.prop(feature, "name", text="", emboss=False, icon=_FEATURE_ICONS.get(feature.type, 'DOT'))
        status_icon = _STATUS_ICONS.get(feature.status)
        if status_icon:
            row.label(text="", icon=status_icon)
        if part.rollback_feature == feature:
            row.label(text="", icon='TRIA_LEFT_BAR')
        row.prop(feature, "suppress", text="", emboss=False,
                 icon='HIDE_ON' if feature.suppress else 'HIDE_OFF')


class DATA_PT_context_cad_part(DataButtonsPanel, Panel):
    bl_label = ""
    bl_options = {'HIDE_HEADER'}
    COMPAT_ENGINES = _COMPAT_ENGINES

    def draw(self, context):
        layout = self.layout

        ob = context.object
        part = context.cad_part
        space = context.space_data

        if ob:
            layout.template_ID(ob, "data")
        elif part:
            layout.template_ID(space, "pin_id")


class DATA_PT_cad_features(DataButtonsPanel, Panel):
    bl_label = "Features"
    COMPAT_ENGINES = _COMPAT_ENGINES

    def draw(self, context):
        layout = self.layout
        part = context.cad_part

        row = layout.row()
        row.template_list(
            "CAD_UL_features",
            "",
            part,
            "features",
            part.features,
            "active_index",
            rows=5,
        )

        col = row.column(align=True)
        col.operator_menu_enum("cad.feature_add", "type", icon='ADD', text="")
        col.operator_menu_enum("cad.sketch_add", "plane", icon='GREASEPENCIL', text="")
        col.operator("cad.feature_delete", icon='REMOVE', text="")
        col.separator()
        col.operator("cad.feature_move", icon='TRIA_UP', text="").direction = 'UP'
        col.operator("cad.feature_move", icon='TRIA_DOWN', text="").direction = 'DOWN'

        row = layout.row(align=True)
        row.operator("cad.rollback_set", text="Roll Back Here", icon='TRIA_LEFT_BAR').action = 'ACTIVE'
        sub = row.row(align=True)
        sub.operator("cad.rollback_set", text="", icon='TRIA_LEFT').action = 'PREVIOUS'
        sub.operator("cad.rollback_set", text="", icon='TRIA_RIGHT').action = 'NEXT'
        sub.operator("cad.rollback_set", text="", icon='TRIA_RIGHT_BAR').action = 'END'
        if part.rollback_feature:
            layout.label(text="Built up to " + part.rollback_feature.name, icon='INFO')


class DATA_PT_cad_feature(DataButtonsPanel, Panel):
    # The header shows the active feature's name instead.
    bl_label = ""
    bl_parent_id = "DATA_PT_cad_features"
    COMPAT_ENGINES = _COMPAT_ENGINES

    @classmethod
    def poll(cls, context):
        return super().poll(context) and context.cad_part.features.active is not None

    def draw_header(self, context):
        feature = context.cad_part.features.active
        self.layout.label(text=feature.name, icon=_FEATURE_ICONS.get(feature.type, 'DOT'))

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = True

        part = context.cad_part
        feature = part.features.active

        if feature.status in {'ERROR', 'WARNING'} and feature.status_message:
            box = layout.box()
            box.label(text=feature.status_message, icon=_STATUS_ICONS[feature.status])

        if feature.type == 'EXTRUDE':
            sketch_uid = feature.get("sketch", -1)
            sketch = next((f for f in part.features if f.uid == sketch_uid), None)
            layout.label(text="Sketch: " + (sketch.name if sketch else "None"), icon='GREASEPENCIL')

        for key in feature.keys():
            if key in _HIDDEN_PARAMS:
                continue
            layout.prop(feature, '["{:s}"]'.format(bpy.utils.escape_identifier(key)), text=key.replace("_", " ").title())

        if feature.type == 'SKETCH':
            sketch = feature.sketch
            col = layout.column(align=True)
            col.label(text="{:d} points, {:d} curves, {:d} constraints".format(
                len(sketch.points), len(sketch.curves), len(sketch.constraints)))
            if sketch.dof >= 0:
                col.label(text="Fully constrained" if sketch.dof == 0 else
                          "{:d} degrees of freedom".format(sketch.dof))
            if part.edit_sketch != feature:
                props = layout.operator("cad.sketch_edit", text="Edit Sketch", icon='GREASEPENCIL')
                props.feature_uid = feature.uid
                props.look_at = True

        if feature.type in {'FILLET', 'CHAMFER'}:
            row = layout.row()
            row.operator("cad.references_pick", text="Select Edges", icon='RESTRICT_SELECT_OFF')
            row.label(text="{:d} edge{:s}".format(len(feature.references),
                                                  "" if len(feature.references) == 1 else "s"))

        if len(feature.references):
            col = layout.column(align=True)
            col.label(text="References")
            for ref in feature.references:
                col.label(text=ref.name, icon='ERROR' if ref.is_lost else 'LINKED')


class DATA_PT_cad_part_settings(DataButtonsPanel, Panel):
    bl_label = "Part"
    bl_options = {'DEFAULT_CLOSED'}
    COMPAT_ENGINES = _COMPAT_ENGINES

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        part = context.cad_part

        layout.prop(part, "kernel")
        layout.prop(part, "show_edges")
        col = layout.column(align=True)
        col.prop(part, "chord_tolerance")
        col.prop(part, "angle_tolerance")
        row = layout.row()
        row.enabled = False
        row.prop(part, "mm_per_unit")


class DATA_PT_custom_props_cad_part(DataButtonsPanel, PropertyPanel, Panel):
    COMPAT_ENGINES = _COMPAT_ENGINES
    _context_path = "object.data"
    _property_type = bpy.types.CADPart if hasattr(bpy.types, "CADPart") else None


classes = (
    CAD_UL_features,
    DATA_PT_context_cad_part,
    DATA_PT_cad_features,
    DATA_PT_cad_feature,
    DATA_PT_cad_part_settings,
    DATA_PT_custom_props_cad_part,
)

if __name__ == "__main__":  # only for live edit.
    from bpy.utils import register_class
    for cls in classes:
        register_class(cls)
