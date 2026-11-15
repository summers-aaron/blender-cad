# SPDX-FileCopyrightText: 2026 SimpleCAD Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# Parametric CAD core: document snapshot, feature evaluation, persistent naming, profiles.
# Free of DNA/RNA/depsgraph; blenkernel converts CADPart data into a cad::Document.

set(INC
  PUBLIC .
)

set(INC_SYS
)

set(SRC
  intern/eval.cc
  intern/profile.cc
  intern/sketch_solver_planegcs.cc

  CAD_document.hh
  CAD_eval.hh
  CAD_sketch_solver.hh
)

set(LIB
  PUBLIC bf::intern::cad_kernel
  PRIVATE bf::blenlib
  PRIVATE bf::intern::guardedalloc
  PRIVATE bf::extern::planegcs
)

blender_add_lib(bf_cad "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")
add_library(bf::cad ALIAS bf_cad)

if(WITH_GTESTS)
  set(TEST_INC
  )
  set(TEST_SRC
    tests/cad_eval_test.cc
    tests/cad_profile_test.cc
    tests/cad_sketch_solver_test.cc
  )
  set(TEST_LIB
    bf_cad
  )
  blender_add_test_suite_lib(bf_cad_tests "${TEST_SRC}" "${INC};${TEST_INC}" "${INC_SYS}" "${LIB};${TEST_LIB}")
endif()
