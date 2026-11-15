/* SPDX-FileCopyrightText: 2026 SimpleCAD Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup cad
 *
 * Expressions for CAD values: dimensions, feature inputs and user parameters, e.g.
 * `20 mm`, `width / 2 + 1 in`, `max(depth, 5)` or `30 deg`.
 *
 * Expressions are calculated in document units: millimeters for lengths and degrees for angles.
 * Numbers without a unit are in those units, numbers with a unit are converted to them, and
 * names refer to variables (user parameters) whose values are given in the same units. The
 * arithmetic is Python-like (see `BLI_expr_pylike_eval.hh`): `+ - * /`, comparisons,
 * `a if c else b`, `and/or/not`, `min/max/pow/abs/sqrt/sin/...` and `pi`.
 */

#pragma once

#include <string>

#include "BLI_span.hh"
#include "BLI_string_ref.hh"
#include "BLI_vector.hh"

namespace blender::cad {

/** Stored in files (matches `eCADParamUnit`): never reorder or reuse values. */
enum class ExprUnit : int8_t {
  None = 0,
  Length = 1,
  Angle = 2,
};

struct ExprVariable {
  std::string name;
  /** In document units (millimeters, degrees). */
  double value = 0.0;
};

struct ExprResult {
  bool ok = false;
  /** In document units of the expression's unit (millimeters, degrees). */
  double value = 0.0;
  /** Why evaluation failed (when not #ok). */
  std::string error;
};

/**
 * Evaluates an expression. Units of the wrong kind (a length in an angle) are an error, so are
 * names that are neither variables nor functions.
 */
ExprResult expr_evaluate(StringRef expression, ExprUnit unit, Span<ExprVariable> variables);

/** Names of variables the expression uses (no functions, constants or units), in order. */
Vector<std::string> expr_variable_names(StringRef expression);

/** The expression with every use of the variable `old_name` replaced by `new_name`. */
std::string expr_rename_variable(StringRef expression, StringRef old_name, StringRef new_name);

/**
 * Whether `name` can name a variable: an identifier (letters, digits, underscores, not starting
 * with a digit) that is not a function, constant, keyword or unit.
 */
bool expr_is_valid_variable_name(StringRef name);

/** A value as expression text in document units, to 4 decimals: "12.5 mm", "30 deg", "2". */
std::string expr_format(double value, ExprUnit unit);

}  // namespace blender::cad
