/* SPDX-FileCopyrightText: 2026 SimpleCAD Authors
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later */

/* Minimal stand-in for FreeCAD's `Base::Console` used by the vendored PlaneGCS solver.
 * Output goes to stderr only when the `PLANEGCS_VERBOSE` environment variable is set,
 * so the solver stays silent inside Blender by default. */

#pragma once

#include <cstdio>
#include <cstdlib>
#include <format>
#include <string>
#include <utility>

namespace Base {

class ConsoleSingleton {
 public:
  template<typename... Args> void log(std::format_string<Args...> fmt, Args &&...args)
  {
    write(std::format(fmt, std::forward<Args>(args)...));
  }
  template<typename... Args> void warning(std::format_string<Args...> fmt, Args &&...args)
  {
    write(std::format(fmt, std::forward<Args>(args)...));
  }
  template<typename... Args> void error(std::format_string<Args...> fmt, Args &&...args)
  {
    write(std::format(fmt, std::forward<Args>(args)...));
  }

 private:
  static void write(const std::string &text)
  {
    static const bool verbose = std::getenv("PLANEGCS_VERBOSE") != nullptr;
    if (verbose) {
      std::fputs(text.c_str(), stderr);
    }
  }
};

inline ConsoleSingleton &Console()
{
  static ConsoleSingleton console;
  return console;
}

}  // namespace Base
