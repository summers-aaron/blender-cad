# SPDX-FileCopyrightText: 2026 SimpleCAD Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# Kernel-neutral B-rep interface (CAD_kernel.hh) and its backends. Backend headers (OCCT, SMLib)
# never leave this library: consumers only see CAD_kernel.hh.

set(INC
  PUBLIC .
)

set(INC_SYS
)

set(SRC
  intern/kernel.cc

  CAD_kernel.hh
  intern/kernel_backends.hh
)

set(LIB
)

if(WITH_CAD_OCCT)
  list(APPEND SRC
    occt/occt_kernel.cc
  )
  list(APPEND LIB
    PRIVATE bf::dependencies::optional::opencascade
  )
endif()

if(WITH_CAD_SMLIB)
  list(APPEND SRC
    smlib/smlib_kernel.cc
  )
  list(APPEND LIB
    PRIVATE bf::extern::smlib
  )
  add_definitions(-DWITH_CAD_SMLIB)
endif()

blender_add_lib(bf_intern_cad_kernel "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")
add_library(bf::intern::cad_kernel ALIAS bf_intern_cad_kernel)

if(WITH_GTESTS)
  set(TEST_SRC
    tests/cad_kernel_test.cc
  )
  set(TEST_INC
  )
  set(TEST_LIB
    bf_intern_cad_kernel
  )
  blender_add_test_suite_executable(cad_kernel "${TEST_SRC}" "${INC};${TEST_INC}" "${INC_SYS}" "${LIB};${TEST_LIB}")
endif()
