/* SPDX-FileCopyrightText: 2026 SimpleCAD Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup intern_cad_kernel
 *
 * Entry points of the compiled-in backends (private to this library).
 */

#pragma once

#include "CAD_kernel.hh"

namespace cad::kernel {

#ifdef WITH_CAD_OCCT
namespace occt {
const Kernel *kernel_instance();
}
#endif

#ifdef WITH_CAD_SMLIB
namespace smlib {
const Kernel *kernel_instance();
}
#endif

}  // namespace cad::kernel
