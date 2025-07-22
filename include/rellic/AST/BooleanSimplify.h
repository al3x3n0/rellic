/*
 * Copyright (c) 2021-present, Trail of Bits, Inc.
 * All rights reserved.
 *
 * This source code is licensed in accordance with the terms specified in
 * the LICENSE file found in the root directory of this source tree.
 */

#pragma once

#include <clang/AST/ASTContext.h>
#include <clang/AST/ExprCXX.h>

#include "rellic/AST/TransformVisitor.h"

namespace rellic {

/*
 * Pass to simplify boolean expressions that contain integer literals.
 * Converts patterns like:
 * - `1U && expr` -> `expr`
 * - `0U && expr` -> `false`
 * - `expr && 1U` -> `expr`
 * - `!1U` -> `false`
 * - `!0U` -> `true`
 * - `(cond ? 1U : 1U)` -> `1U`
 * - `(cond ? 0U : 0U)` -> `0U`
 * And eliminates dead if statements with false conditions.
 */
class BooleanSimplify : public TransformVisitor<BooleanSimplify> {
 public:
  BooleanSimplify(DecompilationContext &dec_ctx);
  
  bool VisitIfStmt(clang::IfStmt *stmt);
  bool VisitWhileStmt(clang::WhileStmt *stmt);
  bool VisitBinaryOperator(clang::BinaryOperator *binop);
  bool VisitUnaryOperator(clang::UnaryOperator *unop);
  bool VisitConditionalOperator(clang::ConditionalOperator *cond_op);

 protected:
  void RunImpl() override;

 private:
  clang::Expr *SimplifyBooleanExpr(clang::Expr *expr);
  bool IsConstantTrue(clang::Expr *expr);
  bool IsConstantFalse(clang::Expr *expr);
};

}  // namespace rellic 