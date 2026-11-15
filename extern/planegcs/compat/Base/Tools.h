/* SPDX-FileCopyrightText: 2026 SimpleCAD Authors
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later */

/* Minimal stand-in for FreeCAD's `Base/Tools.h`: only `Base::TimeElapsed` is used. */

#pragma once

#include <chrono>

namespace Base {

class TimeElapsed {
 public:
  TimeElapsed() : time_(std::chrono::steady_clock::now()) {}

  static double diffTimeF(const TimeElapsed &start, const TimeElapsed &end)
  {
    return std::chrono::duration<double>(end.time_ - start.time_).count();
  }

 private:
  std::chrono::steady_clock::time_point time_;
};

}  // namespace Base
