# SPDX-FileCopyrightText: 2026 SimpleCAD Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# 3D View UI for CAD parts: feature dialogs (object mode) and sketch mode (the edit mode of
# CAD parts).

import bpy
from bpy.types import Menu, Panel


_FEATURE_ICONS = {
    'SKETCH': 'GREASEPENCIL',
    'BOX': 'MESH_CUBE',
    'EXTRUDE': 'MOD_SOLIDIFY',
    'FILLET': 'MOD_BEVEL',
    'CHAMFER': 'MOD_BEVEL',
}


def _cad_part(context):
    ob = context.active_object
    if ob is None or ob.type != 'CAD' or ob.data is None:
        return None
    return ob.data


def draw_feature_input(layout, inp, text=None):
    """An input as an expression field, with the value it evaluates to when it is bound."""
    col = layout.column(align=True)
    row = col.row(align=True)
    row.alert = inp.has_error
    row.prop(inp, "expression", text=inp.name if text is None else text,
             icon='LINKED' if inp.is_expression else 'NONE')
    if inp.is_expression:
        split = col.split(factor=0.4)
        split.label()
        split.label(text="= " + inp.value_text)


class VIEW3D_MT_cad_part(Menu):
    bl_label = "Part"

    def draw(self, context):
        layout = self.layout
        layout.operator("cad.feature_new", text="Extrude", icon='MOD_SOLIDIFY').type = 'EXTRUDE'
        layout.operator("cad.feature_new", text="Fillet", icon='MOD_BEVEL').type = 'FILLET'
        layout.operator("cad.feature_new", text="Chamfer", icon='MOD_BEVEL').type = 'CHAMFER'
        layout.operator("cad.feature_new", text="Box", icon='MESH_CUBE').type = 'BOX'
        layout.separator()
        layout.operator_menu_enum("cad.sketch_add", "plane", text="New Sketch", icon='GREASEPENCIL')
        layout.separator()
        layout.operator("cad.feature_edit", text="Edit Active Feature")


class CAD_MT_history_context(Menu):
    bl_label = "Feature"

    def draw(self, context):
        layout = self.layout
        part = _cad_part(context)
        feature = part.features.active if part else None
        if feature is None:
            layout.label(text="No feature")
            return
        layout.operator("cad.feature_edit", text="Edit Sketch" if feature.type == 'SKETCH' else "Edit Feature",
                        icon='PREFERENCES').feature_uid = feature.uid
        layout.operator("cad.rollback_set", text="Roll Back Here", icon='TRIA_LEFT_BAR').action = 'ACTIVE'
        layout.operator("cad.rollback_set", text="Roll to End", icon='TRIA_RIGHT_BAR').action = 'END'
        props = layout.operator("cad.feature_suppress", text="Unsuppress" if feature.suppress else "Suppress",
                                icon='HIDE_OFF' if feature.suppress else 'HIDE_ON')
        props.feature_uid = feature.uid
        props.action = 'TOGGLE'
        layout.separator()
        props = layout.operator("cad.feature_move", text="Move Earlier", icon='TRIA_LEFT')
        props.feature_uid = feature.uid
        props.direction = 'UP'
        props = layout.operator("cad.feature_move", text="Move Later", icon='TRIA_RIGHT')
        props.feature_uid = feature.uid
        props.direction = 'DOWN'
        layout.separator()
        layout.operator("cad.feature_delete", text="Delete", icon='TRASH').feature_uid = feature.uid


class VIEW3D_PT_cad_feature_dialog(Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "Feature"
    # The header shows "New Extrude", "Edit Fillet1", ...
    bl_label = ""

    @classmethod
    def poll(cls, context):
        part = _cad_part(context)
        return part is not None and part.dialog_feature is not None

    def draw_header(self, context):
        dialog = _cad_part(context).dialog_feature
        type_name = bpy.types.CADFeature.bl_rna.properties["type"].enum_items[dialog.type].name
        self.layout.label(text=("New " + type_name) if dialog.is_new else ("Edit " + dialog.name),
                          icon=_FEATURE_ICONS.get(dialog.type, 'DOT'))

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False
        dialog = _cad_part(context).dialog_feature

        layout.prop(dialog, "name")
        if dialog.status in {'ERROR', 'WARNING'} and dialog.status_message:
            box = layout.box()
            box.alert = dialog.status == 'ERROR'
            box.label(text=dialog.status_message, icon='ERROR' if dialog.status == 'WARNING' else 'CANCEL')

        inputs = dialog.inputs
        if dialog.type == 'EXTRUDE':
            layout.prop(dialog, "profile")
            layout.prop(dialog, '["extent"]', text="Extent")
            draw_feature_input(layout, inputs["distance"])
            if dialog.get("extent", 0) == 2:
                draw_feature_input(layout, inputs["distance2"])
            layout.prop(dialog, '["operation"]', text="Operation")
        elif dialog.type in {'FILLET', 'CHAMFER'}:
            count = len(dialog.references)
            split = layout.split(factor=0.4)
            split.alignment = 'RIGHT'
            split.label(text="Edges")
            split.operator("cad.references_pick",
                           text="{:d} Selected".format(count) if count else "Select",
                           icon='RESTRICT_SELECT_OFF')
            for inp in inputs:
                draw_feature_input(layout, inp)
        elif dialog.type == 'BOX':
            layout.prop(dialog, '["min"]', text="Corner")
            layout.prop(dialog, '["size"]', text="Size")
        else:
            for inp in inputs:
                draw_feature_input(layout, inp)

        layout.separator()
        row = layout.row(align=True)
        row.scale_y = 1.3
        row.operator("cad.dialog_ok", text="OK", icon='CHECKMARK')
        row.operator("cad.dialog_cancel", text="Cancel", icon='X')


class VIEW3D_PT_cad_feature_dialog_parameters(Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "Feature"
    bl_label = "Parameters"
    bl_parent_id = "VIEW3D_PT_cad_feature_dialog"
    bl_options = {'DEFAULT_CLOSED'}

    @classmethod
    def poll(cls, context):
        return len(_cad_part(context).parameters) > 0

    def draw(self, context):
        # Read-only: parameters are edited in the Properties editor (with undo).
        col = self.layout.column(align=True)
        for param in _cad_part(context).parameters:
            row = col.row()
            row.alert = param.has_error
            row.label(text=param.name)
            row.label(text=param.value_text)


def _edit_sketch(context):
    ob = context.edit_object
    if ob is None or ob.type != 'CAD':
        return None, None
    feature = ob.data.edit_sketch
    if feature is None:
        return None, None
    return feature, feature.sketch


class VIEW3D_MT_select_edit_cad(Menu):
    bl_label = "Select"

    def draw(self, _context):
        layout = self.layout
        layout.operator("cad.sketch_select_all", text="All").action = 'SELECT'
        layout.operator("cad.sketch_select_all", text="None").action = 'DESELECT'
        layout.operator("cad.sketch_select_all", text="Invert").action = 'INVERT'


class CAD_MT_sketch_constrain(Menu):
    bl_label = "Constrain"

    def draw(self, _context):
        layout = self.layout
        for type_id, text in (
                ('COINCIDENT', "Coincident"),
                ('HORIZONTAL_VERTICAL', "Horizontal/Vertical"),
                ('PARALLEL', "Parallel"),
                ('PERPENDICULAR', "Perpendicular"),
                ('TANGENT', "Tangent"),
                ('EQUAL', "Equal"),
                ('FIX', "Fix"),
        ):
            layout.operator("cad.sketch_constrain", text=text).type = type_id


class VIEW3D_MT_edit_cad(Menu):
    bl_label = "Sketch"

    def draw(self, _context):
        layout = self.layout
        layout.operator("cad.sketch_line", text="Line")
        layout.operator("cad.sketch_rectangle", text="Rectangle")
        layout.operator("cad.sketch_circle", text="Circle")
        layout.operator("cad.sketch_arc", text="Arc")
        layout.separator()
        layout.operator("cad.sketch_dimension", text="Dimension")
        layout.menu("CAD_MT_sketch_constrain")
        layout.operator("cad.sketch_construction_toggle")
        layout.separator()
        layout.operator("cad.sketch_delete")
        layout.separator()
        layout.operator("cad.sketch_finish", icon='CHECKMARK')


class VIEW3D_PT_cad_sketch(Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "Sketch"
    bl_label = "Sketch"

    @classmethod
    def poll(cls, context):
        return _edit_sketch(context)[1] is not None

    def draw(self, context):
        layout = self.layout
        feature, sketch = _edit_sketch(context)

        box = layout.box()
        row = box.row()
        row.label(text=feature.name, icon='GREASEPENCIL')
        if sketch.solve_status == 'CONFLICTING':
            box.label(text="Conflicting constraints", icon='ERROR')
        elif sketch.solve_status == 'FAILED':
            box.label(text="Solver failed", icon='ERROR')
        elif sketch.dof == 0:
            box.label(text="Fully constrained", icon='CHECKMARK')
        elif sketch.dof > 0:
            box.label(text="{:d} degree{:s} of freedom".format(sketch.dof, "" if sketch.dof == 1 else "s"),
                      icon='INFO')

        col = layout.column(align=True)
        col.label(text="Create")
        grid = col.grid_flow(columns=2, align=True)
        grid.operator("cad.sketch_line", text="Line")
        grid.operator("cad.sketch_rectangle", text="Rectangle")
        grid.operator("cad.sketch_circle", text="Circle")
        grid.operator("cad.sketch_arc", text="Arc")

        col = layout.column(align=True)
        col.label(text="Constrain")
        col.operator("cad.sketch_dimension", text="Dimension", icon='DRIVER_DISTANCE')
        grid = col.grid_flow(columns=2, align=True)
        grid.operator("cad.sketch_constrain", text="Coincident").type = 'COINCIDENT'
        grid.operator("cad.sketch_constrain", text="Horiz./Vert.").type = 'HORIZONTAL_VERTICAL'
        grid.operator("cad.sketch_constrain", text="Parallel").type = 'PARALLEL'
        grid.operator("cad.sketch_constrain", text="Perpendicular").type = 'PERPENDICULAR'
        grid.operator("cad.sketch_constrain", text="Tangent").type = 'TANGENT'
        grid.operator("cad.sketch_constrain", text="Equal").type = 'EQUAL'
        grid.operator("cad.sketch_constrain", text="Fix").type = 'FIX'
        grid.operator("cad.sketch_construction_toggle", text="Construction")

        dims = [c for c in sketch.constraints if c.type in {
            'DISTANCE', 'DISTANCE_X', 'DISTANCE_Y', 'RADIUS', 'DIAMETER', 'ANGLE'}]
        if dims:
            col = layout.column(align=True)
            col.label(text="Dimensions")
            for c in dims:
                row = col.row(align=True)
                row.alert = c.is_conflicting or c.has_expression_error
                row.prop(c, "expression", text=c.type.replace("_", " ").title())

        layout.separator()
        layout.operator("cad.sketch_finish", text="Finish Sketch", icon='CHECKMARK')


classes = (
    VIEW3D_MT_cad_part,
    CAD_MT_history_context,
    VIEW3D_PT_cad_feature_dialog,
    VIEW3D_PT_cad_feature_dialog_parameters,
    VIEW3D_MT_select_edit_cad,
    CAD_MT_sketch_constrain,
    VIEW3D_MT_edit_cad,
    VIEW3D_PT_cad_sketch,
)


if __name__ == "__main__":  # only for live edit.
    from bpy.utils import register_class
    for cls in classes:
        register_class(cls)
