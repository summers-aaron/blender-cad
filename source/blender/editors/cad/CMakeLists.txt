# SPDX-FileCopyrightText: 2026 SimpleCAD Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

set(INC
  ../include
  ../../makesrna

  # RNA_prototypes.hh
  ${CMAKE_BINARY_DIR}/source/blender/makesrna
)

set(INC_SYS
)

set(SRC
  cad_dialog.cc
  cad_ops.cc
  cad_pick.cc
  cad_sketch_draw.cc
  cad_sketch_edit.cc
  cad_sketch_tools.cc
  cad_sketch_undo.cc

  cad_intern.hh
  cad_sketch.hh
)

set(LIB
  PRIVATE bf::blenkernel
  PRIVATE bf::blenlib
  PRIVATE bf::depsgraph
  PRIVATE bf::dna
  PRIVATE bf::gpu
  PRIVATE bf::blentranslation
  PRIVATE bf::blenfont
  PRIVATE bf::intern::clog
  PRIVATE bf::intern::guardedalloc
  PRIVATE bf::windowmanager
  PRIVATE bf::dependencies::fmt
)

blender_add_lib(bf_editor_cad "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")
add_dependencies(bf_editor_cad bf_rna)
