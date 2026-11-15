# SPDX-FileCopyrightText: 2026 SimpleCAD Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# NVIDIA usd-brep kernel layer: SMLib (B-rep kernel, incl. NLib) and SM_API.
# Mirrors upstream `source/SMLib/premake5.lua` and `source/SM_API/premake5.lua`
# (release configuration), built as static libraries.
#
# Deviations from upstream, all deliberate:
# - Static instead of shared libraries.
# - TBB is never used: `SM_USE_TBB` / `SM_USE_TBB_SCALABLE` stay undefined (upstream's own
#   default), so the serial code paths are compiled and the global operator new/delete
#   replacement in SmMemory.cpp is not, which would clash with Blender's allocator.
# - Warnings silenced (upstream builds with -Wall and fatal warnings).
#
# SMLib uses one process-global context (SmApiGetOrCreateContext) whose state is not
# synchronized: every call into this library must be serialized by the caller.

set(SMLIB_DEFINES
  NDEBUG
  NOMINMAX
  SM_GRAPHICS_CALLBACKS=1
  SM_NO_DEBUG_CODE
  SM_NO_GFX_OUTPUT_CODE
)

if(UNIX)
  list(APPEND SMLIB_DEFINES _GLIBCXX_USE_CXX11_ABI=1)
endif()

if(MSVC)
  set(SMLIB_OPTIONS /utf-8 /bigobj /w)
  list(APPEND SMLIB_DEFINES _CRT_SECURE_NO_WARNINGS _SCL_SECURE_NO_WARNINGS)
else()
  # No fused multiply-add contraction: upstream saw it change boolean results.
  set(SMLIB_OPTIONS -w -ffp-contract=off -fno-fast-math -fexceptions -frtti)
endif()

file(GLOB SMLIB_SRC CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/SMLib/src/*.cpp)
file(GLOB SM_API_SRC CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/SM_API/src/*.cpp)

add_library(extern_smlib_kernel STATIC ${SMLIB_SRC})
target_include_directories(extern_smlib_kernel PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/SMLib/inc)
target_compile_definitions(extern_smlib_kernel PUBLIC ${SMLIB_DEFINES})
target_compile_options(extern_smlib_kernel PRIVATE ${SMLIB_OPTIONS})
set_target_properties(extern_smlib_kernel PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON
)

add_library(extern_smlib STATIC ${SM_API_SRC})
target_include_directories(extern_smlib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/SM_API/inc)
# SM_API_EXPORTS selects the non-dllimport branch of SMAPI_EXPORT (needed for static linking).
target_compile_definitions(extern_smlib PUBLIC SM_API_EXPORTS)
target_compile_options(extern_smlib PRIVATE ${SMLIB_OPTIONS})
if(NOT MSVC)
  target_compile_options(extern_smlib PRIVATE -Wno-write-strings)
endif()
set_target_properties(extern_smlib PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON
)

find_package(Threads REQUIRED)
target_link_libraries(extern_smlib PUBLIC extern_smlib_kernel)
target_link_libraries(extern_smlib_kernel PUBLIC Threads::Threads)
if(UNIX)
  target_link_libraries(extern_smlib_kernel PUBLIC m)
endif()

add_library(bf::extern::smlib ALIAS extern_smlib)
