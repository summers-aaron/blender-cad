/* SPDX-FileCopyrightText: 2026 SimpleCAD Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "BLI_math_constants.hh"

#include "CAD_expr.hh"

namespace blender::cad::tests {

static double eval_ok(const StringRef expr,
                      const ExprUnit unit = ExprUnit::Length,
                      const Span<ExprVariable> vars = {})
{
  const ExprResult r = expr_evaluate(expr, unit, vars);
  EXPECT_TRUE(r.ok) << std::string(expr) << ": " << r.error;
  return r.value;
}

static std::string eval_error(const StringRef expr,
                              const ExprUnit unit = ExprUnit::Length,
                              const Span<ExprVariable> vars = {})
{
  const ExprResult r = expr_evaluate(expr, unit, vars);
  EXPECT_FALSE(r.ok) << std::string(expr);
  return r.error;
}

TEST(cad_expr, numbers_and_units)
{
  EXPECT_DOUBLE_EQ(eval_ok("20"), 20.0);
  EXPECT_DOUBLE_EQ(eval_ok("20 mm"), 20.0);
  EXPECT_DOUBLE_EQ(eval_ok("2cm"), 20.0);
  EXPECT_DOUBLE_EQ(eval_ok("1 in"), 25.4);
  EXPECT_DOUBLE_EQ(eval_ok("0.5m + 1 ft"), 500.0 + 304.8);
  EXPECT_DOUBLE_EQ(eval_ok("1.5e1 mm"), 15.0);
  EXPECT_DOUBLE_EQ(eval_ok(".5"), 0.5);
  EXPECT_DOUBLE_EQ(eval_ok("2 \xc2\xb5m"), 0.002);
  EXPECT_DOUBLE_EQ(eval_ok("30 deg", ExprUnit::Angle), 30.0);
  EXPECT_DOUBLE_EQ(eval_ok("45\xc2\xb0", ExprUnit::Angle), 45.0);
  EXPECT_NEAR(eval_ok("1 rad", ExprUnit::Angle), 57.29577951308232, 1e-12);
  /* Any unit converts for unitless values. */
  EXPECT_DOUBLE_EQ(eval_ok("1 cm", ExprUnit::None), 10.0);
}

TEST(cad_expr, arithmetic_and_functions)
{
  EXPECT_DOUBLE_EQ(eval_ok("(10 + 2) * 3 - 6 / 2"), 33.0);
  EXPECT_DOUBLE_EQ(eval_ok("-5 mm"), -5.0);
  EXPECT_DOUBLE_EQ(eval_ok("max(3, 7 mm, 5)"), 7.0);
  EXPECT_DOUBLE_EQ(eval_ok("pow(2, 3)"), 8.0);
  EXPECT_DOUBLE_EQ(eval_ok("sqrt(16) mm"), 4.0);
  EXPECT_DOUBLE_EQ(eval_ok("(2 + 3) cm"), 50.0);
  EXPECT_DOUBLE_EQ(eval_ok("1 + (2) in"), 1.0 + 50.8);
  EXPECT_DOUBLE_EQ(eval_ok("10 if 2 > 1 else 20"), 10.0);
  EXPECT_NEAR(eval_ok("pi"), M_PI, 1e-12);
}

TEST(cad_expr, variables)
{
  const ExprVariable vars[] = {{"width", 40.0}, {"depth_2", 5.0}};
  EXPECT_DOUBLE_EQ(eval_ok("width", ExprUnit::Length, vars), 40.0);
  EXPECT_DOUBLE_EQ(eval_ok("width / 2 + 1 cm", ExprUnit::Length, vars), 30.0);
  EXPECT_DOUBLE_EQ(eval_ok("max(width, depth_2 * 10)", ExprUnit::Length, vars), 50.0);
  EXPECT_EQ(eval_error("width + height", ExprUnit::Length, vars), "Unknown name \"height\"");

  const Vector<std::string> names = expr_variable_names("width*2 + min(depth, width) + 3 mm");
  ASSERT_EQ(names.size(), 2);
  EXPECT_EQ(names[0], "width");
  EXPECT_EQ(names[1], "depth");
}

TEST(cad_expr, errors)
{
  EXPECT_EQ(eval_error(""), "Empty expression");
  EXPECT_EQ(eval_error("   "), "Empty expression");
  EXPECT_EQ(eval_error("1 / 0"), "Division by zero");
  EXPECT_EQ(eval_error("3 +"), "Invalid expression");
  EXPECT_EQ(eval_error("mm"), "Unit \"mm\" needs a number before it");
  EXPECT_EQ(eval_error("10 deg", ExprUnit::Length), "\"deg\" is not a unit of length");
  EXPECT_EQ(eval_error("10 mm", ExprUnit::Angle), "\"mm\" is not a unit of angle");
  EXPECT_EQ(eval_error("sqrt(-1)"), "Math error");
  EXPECT_EQ(eval_error("2 \xe2\x82\xac"), "Unexpected character");
}

TEST(cad_expr, rename)
{
  EXPECT_EQ(expr_rename_variable("width*2 + widths + 3 m", "width", "w"), "w*2 + widths + 3 m");
  /* "m" after a number is a unit, not the variable. */
  EXPECT_EQ(expr_rename_variable("m_1 + 3 m", "m_1", "a"), "a + 3 m");
}

TEST(cad_expr, names)
{
  EXPECT_TRUE(expr_is_valid_variable_name("width"));
  EXPECT_TRUE(expr_is_valid_variable_name("_d2"));
  EXPECT_FALSE(expr_is_valid_variable_name(""));
  EXPECT_FALSE(expr_is_valid_variable_name("2d"));
  EXPECT_FALSE(expr_is_valid_variable_name("a b"));
  EXPECT_FALSE(expr_is_valid_variable_name("mm"));
  EXPECT_FALSE(expr_is_valid_variable_name("in"));
  EXPECT_FALSE(expr_is_valid_variable_name("max"));
  EXPECT_FALSE(expr_is_valid_variable_name("pi"));
  EXPECT_FALSE(expr_is_valid_variable_name("if"));
}

TEST(cad_expr, format)
{
  EXPECT_EQ(expr_format(12.5, ExprUnit::Length), "12.5 mm");
  EXPECT_EQ(expr_format(20.0, ExprUnit::Length), "20 mm");
  EXPECT_EQ(expr_format(-0.0000001, ExprUnit::Length), "0 mm");
  EXPECT_EQ(expr_format(300.000012, ExprUnit::Length), "300 mm");
  EXPECT_EQ(expr_format(30.0, ExprUnit::Angle), "30 deg");
  EXPECT_EQ(expr_format(0.125, ExprUnit::None), "0.125");
  /* Formatted values evaluate back to themselves. */
  EXPECT_DOUBLE_EQ(eval_ok(expr_format(12.3456, ExprUnit::Length)), 12.3456);
}

}  // namespace blender::cad::tests
