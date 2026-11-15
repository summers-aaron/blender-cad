/* SPDX-FileCopyrightText: 2026 SimpleCAD Authors
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later */

/* Replaces FreeCAD's `SketcherGlobal.h` for the vendored PlaneGCS solver.
 *
 * Also provides the standard headers PlaneGCS gets through FreeCAD's precompiled header. */

#pragma once

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <ranges>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#ifndef SketcherExport
#  define SketcherExport
#endif
