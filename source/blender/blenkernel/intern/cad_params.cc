/* SPDX-FileCopyrightText: 2026 SimpleCAD Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * User parameters and expressions of CAD parts.
 *
 * Values are stored in Blender units (lengths) and radians (angles), as RNA shows them;
 * expressions are evaluated in document units (millimeters, degrees, see `CAD_expr.hh`).
 * An expression is only stored when it uses part parameters: anything else is just a way of
 * typing a value. Expressions are evaluated when they or the parameters change, on the original
 * data-block, so the values are always up to date in the UI, in files and in undo steps.
 */

#include <cmath>
#include <cstdlib>

#include "MEM_guardedalloc.h"

#include "DNA_cad_types.h"

#include "BLI_array.hh"
#include "BLI_listbase_iterator.hh"
#include "BLI_math_constants.hh"
#include "BLI_string.hh"
#include "BLI_string_utils.hh"

#include "BKE_cad.hh"
#include "BKE_idprop.hh"

#include "RNA_types.hh"

#ifdef WITH_CAD
#  include "CAD_expr.hh"
#endif

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Units and Evaluation
 * \{ */

static double to_doc_units(const CADPart *part, const double value, const eCADParamUnit unit)
{
  switch (unit) {
    case CAD_UNIT_LENGTH:
      return value * part->mm_per_unit;
    case CAD_UNIT_ANGLE:
      return value * (180.0 / M_PI);
    case CAD_UNIT_NONE:
      break;
  }
  return value;
}

static double from_doc_units(const CADPart *part, const double value, const eCADParamUnit unit)
{
  switch (unit) {
    case CAD_UNIT_LENGTH:
      return value / part->mm_per_unit;
    case CAD_UNIT_ANGLE:
      return value * (M_PI / 180.0);
    case CAD_UNIT_NONE:
      break;
  }
  return value;
}

bke::CADExprEval BKE_cad_expression_eval(const CADPart *part,
                                         const StringRef expression,
                                         const eCADParamUnit unit)
{
  bke::CADExprEval eval;
#ifdef WITH_CAD
  Vector<cad::ExprVariable> variables;
  for (int i = 0; i < part->user_params_num; i++) {
    const CADUserParam &param = part->user_params[i];
    variables.append({param.name, to_doc_units(part, param.value, eCADParamUnit(param.unit))});
  }
  const cad::ExprResult r = cad::expr_evaluate(expression, cad::ExprUnit(unit), variables);
  eval.ok = r.ok;
  eval.error = r.error;
  eval.value = r.ok ? from_doc_units(part, r.value, unit) : 0.0;
#else
  const std::string text(expression);
  char *end = nullptr;
  const double value = strtod(text.c_str(), &end);
  eval.ok = end != text.c_str();
  eval.value = eval.ok ? from_doc_units(part, value, unit) : 0.0;
  eval.error = eval.ok ? "" : "Invalid expression";
#endif
  return eval;
}

bool BKE_cad_expression_uses_params(const CADPart *part, const StringRef expression)
{
#ifdef WITH_CAD
  for (const std::string &name : cad::expr_variable_names(expression)) {
    if (BKE_cad_param_find(part, name) >= 0) {
      return true;
    }
  }
#else
  UNUSED_VARS(part, expression);
#endif
  return false;
}

std::string BKE_cad_value_format(const CADPart *part, const double value, const eCADParamUnit unit)
{
#ifdef WITH_CAD
  return cad::expr_format(to_doc_units(part, value, unit), cad::ExprUnit(unit));
#else
  return std::to_string(to_doc_units(part, value, unit));
#endif
}

static void string_set(char *&dst, const StringRef value)
{
  MEM_SAFE_DELETE(dst);
  dst = value.is_empty() ? nullptr : BLI_strdupn(value.data(), value.size());
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Feature Inputs
 * \{ */

static IDProperty *input_find(const CADFeature *feature, const StringRef key)
{
  IDProperty *prop = feature->params ? IDP_GetPropertyFromGroup(feature->params, key) : nullptr;
  return (prop && BKE_cad_feature_param_is_input(prop)) ? prop : nullptr;
}

static double input_value(const IDProperty *prop)
{
  return prop->type == IDP_DOUBLE ? IDP_double_get(prop) : double(IDP_float_get(prop));
}

static void input_value_set(IDProperty *prop, const double value)
{
  if (prop->type == IDP_DOUBLE) {
    IDP_double_set(prop, value);
  }
  else {
    IDP_float_set(prop, float(value));
  }
}

bool BKE_cad_feature_param_is_input(const IDProperty *prop)
{
  return prop->type == IDP_DOUBLE || prop->type == IDP_FLOAT;
}

eCADParamUnit BKE_cad_feature_param_unit(const IDProperty *prop)
{
  if (prop->ui_data) {
    switch (prop->ui_data->rna_subtype) {
      case PROP_DISTANCE:
      case PROP_DISTANCE_CAMERA:
      case PROP_DISTANCE_DIAMETER:
        return CAD_UNIT_LENGTH;
      case PROP_ANGLE:
        return CAD_UNIT_ANGLE;
      default:
        break;
    }
  }
  return CAD_UNIT_NONE;
}

CADParamExpr *BKE_cad_feature_expression_find(const CADFeature *feature, const StringRef key)
{
  for (int i = 0; i < feature->exprs_num; i++) {
    if (key == feature->exprs[i].key) {
      return &feature->exprs[i];
    }
  }
  return nullptr;
}

static void feature_expression_remove(CADFeature *feature, const StringRef key)
{
  CADParamExpr *expr = BKE_cad_feature_expression_find(feature, key);
  if (expr == nullptr) {
    return;
  }
  MEM_SAFE_DELETE(expr->expression);
  const int index = int(expr - feature->exprs);
  for (int i = index; i + 1 < feature->exprs_num; i++) {
    feature->exprs[i] = feature->exprs[i + 1];
  }
  feature->exprs_num--;
  if (feature->exprs_num == 0) {
    MEM_SAFE_DELETE(feature->exprs);
  }
}

static CADParamExpr &feature_expression_ensure(CADFeature *feature, const StringRef key)
{
  if (CADParamExpr *expr = BKE_cad_feature_expression_find(feature, key)) {
    return *expr;
  }
  CADParamExpr *grown = MEM_new_array<CADParamExpr>(size_t(feature->exprs_num + 1), __func__);
  if (feature->exprs) {
    std::copy_n(feature->exprs, feature->exprs_num, grown);
    MEM_delete(feature->exprs);
  }
  feature->exprs = grown;
  CADParamExpr &expr = feature->exprs[feature->exprs_num++];
  key.copy_utf8_truncated(expr.key);
  return expr;
}

bool BKE_cad_feature_input_set(CADPart *part,
                               CADFeature *feature,
                               const StringRef key,
                               const StringRef text,
                               std::string *r_error)
{
  IDProperty *prop = input_find(feature, key);
  if (prop == nullptr) {
    if (r_error) {
      *r_error = "No input \"" + std::string(key) + "\"";
    }
    return false;
  }
  const bke::CADExprEval eval = BKE_cad_expression_eval(
      part, text, BKE_cad_feature_param_unit(prop));
  if (!eval.ok) {
    if (r_error) {
      *r_error = eval.error;
    }
    return false;
  }
  input_value_set(prop, eval.value);
  if (BKE_cad_expression_uses_params(part, text)) {
    CADParamExpr &expr = feature_expression_ensure(feature, key);
    string_set(expr.expression, text);
    expr.flag = 0;
  }
  else {
    feature_expression_remove(feature, key);
  }
  return true;
}

void BKE_cad_feature_input_value_set(CADFeature *feature, const StringRef key, const double value)
{
  if (IDProperty *prop = input_find(feature, key)) {
    input_value_set(prop, value);
    feature_expression_remove(feature, key);
  }
}

std::string BKE_cad_feature_input_text(const CADPart *part,
                                       const CADFeature *feature,
                                       const StringRef key)
{
  if (const CADParamExpr *expr = BKE_cad_feature_expression_find(feature, key)) {
    if (expr->expression) {
      return expr->expression;
    }
  }
  const IDProperty *prop = input_find(feature, key);
  return prop ? BKE_cad_value_format(part, input_value(prop), BKE_cad_feature_param_unit(prop)) :
                std::string();
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Sketch Dimensions
 * \{ */

static eCADParamUnit constraint_unit(const CADSketchConstraint &c)
{
  return c.type == CAD_CONSTRAINT_ANGLE ? CAD_UNIT_ANGLE : CAD_UNIT_LENGTH;
}

/** Signed distances along an axis keep their side of the axis. */
static double constraint_value(const CADSketchConstraint &c, const double value)
{
  if ((c.type == CAD_CONSTRAINT_DISTANCE_X || c.type == CAD_CONSTRAINT_DISTANCE_Y) &&
      c.value < 0.0)
  {
    return -std::abs(value);
  }
  return value;
}

bool BKE_cad_constraint_input_set(CADPart *part,
                                  CADSketchConstraint *c,
                                  const StringRef text,
                                  std::string *r_error)
{
  if (c->type < CAD_CONSTRAINT_DISTANCE) {
    if (r_error) {
      *r_error = "Not a dimension";
    }
    return false;
  }
  const bke::CADExprEval eval = BKE_cad_expression_eval(part, text, constraint_unit(*c));
  if (!eval.ok) {
    if (r_error) {
      *r_error = eval.error;
    }
    return false;
  }
  c->value = constraint_value(*c, eval.value);
  string_set(c->expression, BKE_cad_expression_uses_params(part, text) ? text : StringRef());
  c->flag &= ~CAD_CONSTRAINT_EXPR_ERROR;
  return true;
}

std::string BKE_cad_constraint_input_text(const CADPart *part, const CADSketchConstraint *c)
{
  if (c->expression) {
    return c->expression;
  }
  const double value = (c->type == CAD_CONSTRAINT_DISTANCE_X ||
                        c->type == CAD_CONSTRAINT_DISTANCE_Y) ?
                           std::abs(c->value) :
                           c->value;
  return BKE_cad_value_format(part, value, constraint_unit(*c));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name User Parameters
 * \{ */

int BKE_cad_param_find(const CADPart *part, const StringRef name)
{
  for (int i = 0; i < part->user_params_num; i++) {
    if (name == part->user_params[i].name) {
      return i;
    }
  }
  return -1;
}

bool BKE_cad_param_name_is_valid(const StringRef name)
{
#ifdef WITH_CAD
  return name.size() < sizeof(CADUserParam::name) && cad::expr_is_valid_variable_name(name);
#else
  return !name.is_empty() && name.size() < sizeof(CADUserParam::name);
#endif
}

static std::string param_unique_name(const CADPart *part, const StringRef name, const int skip)
{
  std::string base = BKE_cad_param_name_is_valid(name) ? std::string(name) : std::string("param");
  std::string unique = base;
  for (int n = 1;; n++) {
    const int found = BKE_cad_param_find(part, unique);
    if (found < 0 || found == skip) {
      return unique;
    }
    unique = base + std::to_string(n);
  }
}

CADUserParam *BKE_cad_param_add(CADPart *part,
                                const StringRef name,
                                const eCADParamUnit unit,
                                const StringRef expression)
{
  CADUserParam *grown = MEM_new_array<CADUserParam>(size_t(part->user_params_num + 1), __func__);
  if (part->user_params) {
    std::copy_n(part->user_params, part->user_params_num, grown);
    MEM_delete(part->user_params);
  }
  part->user_params = grown;
  const int index = part->user_params_num++;
  CADUserParam &param = part->user_params[index];
  STRNCPY(param.name, param_unique_name(part, name, index).c_str());
  param.unit = unit;
  if (!expression.is_empty()) {
    string_set(param.expression, expression);
  }
  BKE_cad_expressions_update(part);
  return &part->user_params[index];
}

void BKE_cad_param_remove(CADPart *part, const int index)
{
  if (index < 0 || index >= part->user_params_num) {
    return;
  }
  MEM_SAFE_DELETE(part->user_params[index].expression);
  for (int i = index; i + 1 < part->user_params_num; i++) {
    part->user_params[i] = part->user_params[i + 1];
  }
  part->user_params_num--;
  if (part->user_params_num == 0) {
    MEM_SAFE_DELETE(part->user_params);
  }
  part->active_param_index = std::max(0, std::min(part->active_param_index,
                                                  part->user_params_num - 1));
  /* Expressions that used it now fail and keep their last values. */
  BKE_cad_expressions_update(part);
}

/** Calls `fn(char *&expression)` for every stored expression of the part. */
template<typename Fn> static void foreach_expression(CADPart *part, const Fn &fn)
{
  for (int i = 0; i < part->user_params_num; i++) {
    fn(part->user_params[i].expression);
  }
  for (CADFeature &feature : part->features) {
    for (int i = 0; i < feature.exprs_num; i++) {
      fn(feature.exprs[i].expression);
    }
    if (CADSketch *sketch = feature.sketch) {
      for (int i = 0; i < sketch->constraints_num; i++) {
        fn(sketch->constraints[i].expression);
      }
    }
  }
}

bool BKE_cad_param_rename(CADPart *part, const int index, const StringRef name)
{
  if (index < 0 || index >= part->user_params_num || !BKE_cad_param_name_is_valid(name)) {
    return false;
  }
  CADUserParam &param = part->user_params[index];
  if (name == param.name) {
    return true;
  }
  const std::string old_name = param.name;
  const std::string new_name = param_unique_name(part, name, index);
  STRNCPY(param.name, new_name.c_str());
#ifdef WITH_CAD
  /* Expressions follow the rename. */
  foreach_expression(part, [&](char *&expression) {
    if (expression) {
      string_set(expression, cad::expr_rename_variable(expression, old_name, new_name));
    }
  });
#endif
  BKE_cad_expressions_update(part);
  return true;
}

bool BKE_cad_param_expression_set(CADPart *part,
                                  const int index,
                                  const StringRef text,
                                  std::string *r_error)
{
  if (index < 0 || index >= part->user_params_num) {
    return false;
  }
  CADUserParam &param = part->user_params[index];
  const bke::CADExprEval eval = BKE_cad_expression_eval(part, text, eCADParamUnit(param.unit));
  if (!eval.ok) {
    if (r_error) {
      *r_error = eval.error;
    }
    return false;
  }
#ifdef WITH_CAD
  /* A parameter cannot depend on itself, also not through others. */
  Vector<int> stack = {index};
  Vector<int> seen;
  bool first = true;
  while (!stack.is_empty()) {
    const int i = stack.pop_last();
    const StringRef expression = first ? text :
                                         StringRef(part->user_params[i].expression ?
                                                       part->user_params[i].expression :
                                                       "");
    first = false;
    for (const std::string &name : cad::expr_variable_names(expression)) {
      const int used = BKE_cad_param_find(part, name);
      if (used == index) {
        if (r_error) {
          *r_error = "\"" + std::string(param.name) + "\" cannot depend on itself";
        }
        return false;
      }
      if (used >= 0 && !seen.contains(used)) {
        seen.append(used);
        stack.append(used);
      }
    }
  }
#endif
  string_set(param.expression, text);
  param.value = eval.value;
  param.flag &= ~CAD_EXPR_ERROR;
  BKE_cad_expressions_update(part);
  return true;
}

std::string BKE_cad_param_text(const CADPart *part, const int index)
{
  const CADUserParam &param = part->user_params[index];
  return param.expression ? std::string(param.expression) :
                            BKE_cad_value_format(part, param.value, eCADParamUnit(param.unit));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Update
 * \{ */

bool BKE_cad_expressions_update(CADPart *part)
{
  bool changed = false;
  auto set_value = [&](double &dst, const double value) {
    if (dst != value) {
      dst = value;
      changed = true;
    }
  };

  /* Parameters, in dependency order. */
  const int params_num = part->user_params_num;
  Array<int8_t> state(params_num, 0); /* 0: to do, 1: in progress, 2: done. */
  auto update_param = [&](auto &&self, const int i) -> void {
    CADUserParam &param = part->user_params[i];
    if (state[i] == 2) {
      return;
    }
    if (state[i] == 1) {
      /* Cyclic dependency: files made by scripts can have them. */
      param.flag |= CAD_EXPR_ERROR;
      return;
    }
    state[i] = 1;
    if (param.expression) {
#ifdef WITH_CAD
      for (const std::string &name : cad::expr_variable_names(param.expression)) {
        const int used = BKE_cad_param_find(part, name);
        if (used >= 0) {
          self(self, used);
        }
      }
#endif
      const bke::CADExprEval eval = BKE_cad_expression_eval(
          part, param.expression, eCADParamUnit(param.unit));
      if (eval.ok) {
        set_value(param.value, eval.value);
      }
      SET_FLAG_FROM_TEST(param.flag, !eval.ok, CAD_EXPR_ERROR);
    }
    else {
      param.flag &= ~CAD_EXPR_ERROR;
    }
    state[i] = 2;
  };
  for (int i = 0; i < params_num; i++) {
    update_param(update_param, i);
  }

  for (CADFeature &feature : part->features) {
    for (int i = 0; i < feature.exprs_num; i++) {
      CADParamExpr &expr = feature.exprs[i];
      IDProperty *prop = input_find(&feature, expr.key);
      if (prop == nullptr || expr.expression == nullptr) {
        expr.flag |= CAD_EXPR_ERROR;
        continue;
      }
      const bke::CADExprEval eval = BKE_cad_expression_eval(
          part, expr.expression, BKE_cad_feature_param_unit(prop));
      if (eval.ok && input_value(prop) != eval.value) {
        input_value_set(prop, eval.value);
        changed = true;
      }
      SET_FLAG_FROM_TEST(expr.flag, !eval.ok, CAD_EXPR_ERROR);
    }

    CADSketch *sketch = feature.sketch;
    if (sketch == nullptr) {
      continue;
    }
    bool sketch_changed = false;
    for (int i = 0; i < sketch->constraints_num; i++) {
      CADSketchConstraint &c = sketch->constraints[i];
      if (c.expression == nullptr) {
        c.flag &= ~CAD_CONSTRAINT_EXPR_ERROR;
        continue;
      }
      const bke::CADExprEval eval = BKE_cad_expression_eval(
          part, c.expression, constraint_unit(c));
      if (eval.ok) {
        const double value = constraint_value(c, eval.value);
        if (c.value != value) {
          c.value = value;
          sketch_changed = true;
        }
      }
      SET_FLAG_FROM_TEST(c.flag, !eval.ok, CAD_CONSTRAINT_EXPR_ERROR);
    }
    if (sketch_changed) {
      BKE_cad_sketch_solve(part, sketch);
      changed = true;
    }
  }
  return changed;
}

/** \} */

}  // namespace blender
