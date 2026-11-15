/* SPDX-FileCopyrightText: 2026 SimpleCAD Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup cad
 *
 * CAD expressions are rewritten into Python-like expressions (numbers with units become
 * products with a conversion factor) and evaluated with `BLI_expr_pylike_eval`.
 */

#include <cmath>
#include <cstdio>
#include <cstring>
#include <optional>

#include "BLI_expr_pylike_eval.hh"
#include "BLI_math_constants.hh"

#include "CAD_expr.hh"

namespace blender::cad {

namespace {

struct UnitDef {
  const char *name;
  ExprUnit kind;
  /** To millimeters or degrees. */
  double factor;
};

constexpr UnitDef units[] = {
    {"mm", ExprUnit::Length, 1.0},
    {"cm", ExprUnit::Length, 10.0},
    {"m", ExprUnit::Length, 1000.0},
    {"km", ExprUnit::Length, 1e6},
    {"um", ExprUnit::Length, 1e-3},
    {"\xc2\xb5m", ExprUnit::Length, 1e-3}, /* µm */
    {"in", ExprUnit::Length, 25.4},
    {"inch", ExprUnit::Length, 25.4},
    {"inches", ExprUnit::Length, 25.4},
    {"ft", ExprUnit::Length, 304.8},
    {"foot", ExprUnit::Length, 304.8},
    {"feet", ExprUnit::Length, 304.8},
    {"yd", ExprUnit::Length, 914.4},
    {"mil", ExprUnit::Length, 0.0254},
    {"thou", ExprUnit::Length, 0.0254},
    {"deg", ExprUnit::Angle, 1.0},
    {"degree", ExprUnit::Angle, 1.0},
    {"degrees", ExprUnit::Angle, 1.0},
    {"\xc2\xb0", ExprUnit::Angle, 1.0}, /* ° */
    {"rad", ExprUnit::Angle, 180.0 / M_PI},
    {"radian", ExprUnit::Angle, 180.0 / M_PI},
};

/** Functions, constants and keywords of `BLI_expr_pylike_eval`. */
constexpr const char *builtins[] = {
    "radians", "degrees", "abs",   "fabs",  "floor", "ceil",  "trunc", "round",      "int",
    "sin",     "cos",     "tan",   "asin",  "acos",  "atan",  "atan2", "exp",        "log",
    "sqrt",    "pow",     "fmod",  "lerp",  "clamp", "min",   "max",   "smoothstep", "pi",
    "True",    "False",   "and",   "or",    "not",   "if",    "else",
};

bool is_ident_start(const char c)
{
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool is_ident_char(const char c)
{
  return is_ident_start(c) || (c >= '0' && c <= '9');
}

bool is_digit(const char c)
{
  return c >= '0' && c <= '9';
}

bool is_builtin(const StringRef name)
{
  for (const char *builtin : builtins) {
    if (name == builtin) {
      return true;
    }
  }
  return false;
}

const UnitDef *find_unit(const StringRef name)
{
  for (const UnitDef &unit : units) {
    if (name == unit.name) {
      return &unit;
    }
  }
  return nullptr;
}

/** Length of the identifier (or unit symbol) starting at `pos`, 0 when there is none. */
int64_t word_length(const StringRef s, const int64_t pos)
{
  /* Unit symbols that are not identifiers. */
  for (const UnitDef &unit : units) {
    if (!is_ident_start(unit.name[0]) && s.substr(pos).startswith(unit.name)) {
      int64_t end = pos + int64_t(strlen(unit.name));
      while (end < s.size() && is_ident_char(s[end])) {
        end++;
      }
      return end - pos;
    }
  }
  if (pos >= s.size() || !is_ident_start(s[pos])) {
    return 0;
  }
  int64_t end = pos + 1;
  while (end < s.size() && is_ident_char(s[end])) {
    end++;
  }
  return end - pos;
}

/** Length of the number starting at `pos` (digits, point, exponent), 0 when there is none. */
int64_t number_length(const StringRef s, const int64_t pos)
{
  int64_t end = pos;
  bool digits = false;
  while (end < s.size() && is_digit(s[end])) {
    end++;
    digits = true;
  }
  if (end < s.size() && s[end] == '.') {
    end++;
    while (end < s.size() && is_digit(s[end])) {
      end++;
      digits = true;
    }
  }
  if (!digits) {
    return 0;
  }
  if (end < s.size() && (s[end] == 'e' || s[end] == 'E')) {
    int64_t exp = end + 1;
    if (exp < s.size() && (s[exp] == '+' || s[exp] == '-')) {
      exp++;
    }
    if (exp < s.size() && is_digit(s[exp])) {
      while (exp < s.size() && is_digit(s[exp])) {
        exp++;
      }
      end = exp;
    }
  }
  return end - pos;
}

std::string format_double(const double value)
{
  char buf[64];
  snprintf(buf, sizeof(buf), "%.17g", value);
  return buf;
}

struct Rewritten {
  /** The Python-like expression. */
  std::string text;
  /** Names that are not functions, constants or units, in order of first use. */
  Vector<std::string> names;
  std::string error;
};

const char *unit_kind_name(const ExprUnit unit)
{
  switch (unit) {
    case ExprUnit::Length:
      return "length";
    case ExprUnit::Angle:
      return "angle";
    case ExprUnit::None:
      break;
  }
  return "number";
}

/**
 * Rewrites numbers with units into products with conversion factors. With `unit` set, a unit of
 * another kind is an error.
 */
Rewritten rewrite(const StringRef s, const std::optional<ExprUnit> unit)
{
  Rewritten r;
  int64_t i = 0;
  while (i < s.size()) {
    const char c = s[i];
    if (const int64_t num_len = number_length(s, i)) {
      const StringRef number = s.substr(i, num_len);
      int64_t k = i + num_len;
      while (k < s.size() && s[k] == ' ') {
        k++;
      }
      const int64_t word_len = word_length(s, k);
      const UnitDef *unit_def = word_len ? find_unit(s.substr(k, word_len)) : nullptr;
      if (unit_def) {
        if (unit && *unit != ExprUnit::None && unit_def->kind != *unit) {
          r.error = std::string("\"") + unit_def->name + "\" is not a unit of " +
                    unit_kind_name(*unit);
          return r;
        }
        r.text += "(" + std::string(number) + "*" + format_double(unit_def->factor) + ")";
        i = k + word_len;
      }
      else {
        r.text += number;
        i += num_len;
      }
      continue;
    }
    if (const int64_t word_len = word_length(s, i)) {
      const StringRef word = s.substr(i, word_len);
      if (const UnitDef *unit_def = find_unit(word)) {
        /* A unit after a parenthesized group applies to the group: "(2 + 3) cm". */
        const int64_t prev = r.text.find_last_not_of(' ');
        if (prev == std::string::npos || r.text[prev] != ')') {
          r.error = std::string("Unit \"") + std::string(word) + "\" needs a number before it";
          return r;
        }
        if (unit && *unit != ExprUnit::None && unit_def->kind != *unit) {
          r.error = std::string("\"") + unit_def->name + "\" is not a unit of " +
                    unit_kind_name(*unit);
          return r;
        }
        r.text.resize(prev + 1);
        r.text += "*" + format_double(unit_def->factor);
        i += word_len;
        continue;
      }
      if (!is_builtin(word)) {
        const std::string name(word);
        if (!r.names.contains(name)) {
          r.names.append(name);
        }
      }
      r.text += word;
      i += word_len;
      continue;
    }
    if ((unsigned char)c >= 0x80) {
      r.error = "Unexpected character";
      return r;
    }
    r.text += c;
    i++;
  }
  return r;
}

}  // namespace

ExprResult expr_evaluate(const StringRef expression,
                         const ExprUnit unit,
                         const Span<ExprVariable> variables)
{
  ExprResult result;
  const Rewritten r = rewrite(expression, unit);
  if (!r.error.empty()) {
    result.error = r.error;
    return result;
  }
  if (r.text.find_first_not_of(" \t") == std::string::npos) {
    result.error = "Empty expression";
    return result;
  }

  Vector<const char *> param_names;
  Vector<double> param_values;
  for (const std::string &name : r.names) {
    const ExprVariable *variable = nullptr;
    for (const ExprVariable &v : variables) {
      if (v.name == name) {
        variable = &v;
        break;
      }
    }
    if (variable == nullptr) {
      result.error = "Unknown name \"" + name + "\"";
      return result;
    }
    param_names.append(name.c_str());
    param_values.append(variable->value);
  }

  ExprPyLike_Parsed *parsed = BLI_expr_pylike_parse(
      r.text.c_str(), param_names.data(), int(param_names.size()));
  if (!BLI_expr_pylike_is_valid(parsed)) {
    BLI_expr_pylike_free(parsed);
    result.error = "Invalid expression";
    return result;
  }
  double value = 0.0;
  const eExprPyLike_EvalStatus status = BLI_expr_pylike_eval(
      parsed, param_values.data(), int(param_values.size()), &value);
  BLI_expr_pylike_free(parsed);

  switch (status) {
    case EXPR_PYLIKE_SUCCESS:
      break;
    case EXPR_PYLIKE_DIV_BY_ZERO:
      result.error = "Division by zero";
      return result;
    case EXPR_PYLIKE_MATH_ERROR:
      result.error = "Math error";
      return result;
    default:
      result.error = "Invalid expression";
      return result;
  }
  if (!std::isfinite(value)) {
    result.error = "Math error";
    return result;
  }
  result.ok = true;
  result.value = value;
  return result;
}

Vector<std::string> expr_variable_names(const StringRef expression)
{
  return rewrite(expression, std::nullopt).names;
}

std::string expr_rename_variable(const StringRef expression,
                                 const StringRef old_name,
                                 const StringRef new_name)
{
  std::string out;
  int64_t i = 0;
  bool after_number = false;
  while (i < expression.size()) {
    if (const int64_t num_len = number_length(expression, i)) {
      out += expression.substr(i, num_len);
      i += num_len;
      after_number = true;
      continue;
    }
    if (const int64_t word_len = word_length(expression, i)) {
      const StringRef word = expression.substr(i, word_len);
      /* A word right after a number is its unit. */
      out += (!after_number && word == old_name) ? new_name : word;
      i += word_len;
      after_number = false;
      continue;
    }
    if (expression[i] != ' ') {
      after_number = false;
    }
    out += expression[i];
    i++;
  }
  return out;
}

bool expr_is_valid_variable_name(const StringRef name)
{
  if (name.is_empty() || !is_ident_start(name[0])) {
    return false;
  }
  for (const char c : name) {
    if (!is_ident_char(c)) {
      return false;
    }
  }
  return !is_builtin(name) && !find_unit(name);
}

std::string expr_format(const double value, const ExprUnit unit)
{
  /* 4 decimals: 0.1 micrometer, hides float32 noise of values set through RNA. */
  char buf[64];
  snprintf(buf, sizeof(buf), "%.4f", value);
  std::string text = buf;
  /* Strip trailing zeros and the point: "12.5000" -> "12.5", "3.0000" -> "3". */
  text.erase(text.find_last_not_of('0') + 1);
  if (!text.empty() && text.back() == '.') {
    text.pop_back();
  }
  if (text == "-0") {
    text = "0";
  }
  switch (unit) {
    case ExprUnit::Length:
      return text + " mm";
    case ExprUnit::Angle:
      return text + " deg";
    case ExprUnit::None:
      break;
  }
  return text;
}

}  // namespace blender::cad
