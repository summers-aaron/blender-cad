/* SPDX-FileCopyrightText: 2026 SimpleCAD Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup intern_cad_kernel
 *
 * Backend registry.
 */

#include "CAD_kernel.hh"

#include "kernel_backends.hh"

namespace cad::kernel {

const char *status_name(const Status status)
{
  switch (status) {
    case Status::Ok:
      return "Ok";
    case Status::Failed:
      return "Failed";
    case Status::InvalidInput:
      return "Invalid input";
    case Status::Unsupported:
      return "Unsupported";
    case Status::InvalidResult:
      return "Invalid result";
  }
  return "Unknown";
}

const Kernel *kernel_get(const KernelId id)
{
  switch (id) {
    case KernelId::OCCT:
#ifdef WITH_CAD_OCCT
      return occt::kernel_instance();
#else
      return nullptr;
#endif
    case KernelId::SMLib:
#ifdef WITH_CAD_SMLIB
      return smlib::kernel_instance();
#else
      return nullptr;
#endif
  }
  return nullptr;
}

std::vector<KernelId> kernels_available()
{
  std::vector<KernelId> ids;
#ifdef WITH_CAD_OCCT
  ids.push_back(KernelId::OCCT);
#endif
#ifdef WITH_CAD_SMLIB
  ids.push_back(KernelId::SMLib);
#endif
  return ids;
}

}  // namespace cad::kernel
