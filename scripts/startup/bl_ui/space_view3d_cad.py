# SPDX-FileCopyrightText: 2026 SimpleCAD Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# 3D View UI for CAD parts in sketch mode (the edit mode of CAD parts).

import bpy
from bpy.types import Menu, Panel


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
                row.alert = c.is_conflicting
                row.prop(c, "angle" if c.type == 'ANGLE' else "length", text=c.type.replace("_", " ").title())

        layout.separator()
        layout.operator("cad.sketch_finish", text="Finish Sketch", icon='CHECKMARK')


classes = (
    VIEW3D_MT_select_edit_cad,
    CAD_MT_sketch_constrain,
    VIEW3D_MT_edit_cad,
    VIEW3D_PT_cad_sketch,
)


if __name__ == "__main__":  # only for live edit.
    from bpy.utils import register_class
    for cls in classes:
        register_class(cls)
