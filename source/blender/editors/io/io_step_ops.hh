/* SPDX-FileCopyrightText: 2026 SimpleCAD Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editor/io
 */

#pragma once

namespace blender {

struct wmOperatorType;

void WM_OT_step_export(wmOperatorType *ot);

namespace ed::io {
void step_file_handler_add();
}

}  // namespace blender
