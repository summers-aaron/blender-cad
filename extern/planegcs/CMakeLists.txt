# SPDX-FileCopyrightText: 2026 SimpleCAD Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# PlaneGCS: FreeCAD's 2D geometric constraint solver, vendored as a standalone library.
# See README.blender for the upstream revision and the local patches.

set(INC
  PUBLIC src
  PUBLIC compat
)

set(INC_SYS
)

set(SRC
  src/Constraints.cpp
  src/GCS.cpp
  src/Geo.cpp
  src/qp_eq.cpp
  src/SubSystem.cpp

  src/Constraints.h
  src/GCS.h
  src/Geo.h
  src/qp_eq.h
  src/SubSystem.h
  src/Util.h

  compat/Base/Console.h
  compat/Base/Tools.h
  compat/FCConfig.h
  compat/planegcs_compat.h
  compat/planegcs_union_find.h
)

set(LIB
  PUBLIC bf::dependencies::eigen
)

blender_add_lib(extern_planegcs "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")
add_library(bf::extern::planegcs ALIAS extern_planegcs)

# Upstream FreeCAD builds PlaneGCS as C++23 (e.g. std::unreachable).
set_target_properties(extern_planegcs PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
