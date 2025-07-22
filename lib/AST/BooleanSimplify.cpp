/*
 * Copyright (c) 2021-present, Trail of Bits, Inc.
 * All rights reserved.
 *
 * This source code is licensed in accordance with the terms specified in
 * the LICENSE file found in the root directory of this source tree.
 */

#include "rellic/AST/BooleanSimplify.h"
#include <glog/logging.h>

namespace rellic {

BooleanSimplify::BooleanSimplify(DecompilationContext &dec_ctx)
    : TransformVisitor<BooleanSimplify>(dec_ctx) {}

void BooleanSimplify::RunImpl() {
  //LOG(INFO) << "BooleanSimplify::RunImpl() - Starting boolean expression simplification";
  TransformVisitor<BooleanSimplify>::RunImpl();
  
  // Run multiple passes to handle nested simplifications
  int max_iterations = 5;
  for (int i = 0; i < max_iterations; ++i) {
    changed = false;
    substitutions.clear();
    //LOG(INFO) << "BooleanSimplify: Starting iteration " << i;
    TraverseDecl(dec_ctx.ast_ctx.getTranslationUnitDecl());
    //LOG(INFO) << "BooleanSimplify: Iteration " << i << " completed with " << substitutions.size() << " substitutions, changed=" << changed;
    if (!changed) break;
  }
  //LOG(INFO) << "BooleanSimplify::RunImpl() - Completed";
}

bool BooleanSimplify::IsConstantTrue(clang::Expr *expr) {
  if (!expr) return false;
  
  // Strip any implicit casts and parentheses
  expr = expr->IgnoreParenImpCasts();
  
  // Check for integer literal 1 (including 1U)
  if (auto int_lit = llvm::dyn_cast<clang::IntegerLiteral>(expr)) {
    return int_lit->getValue() == 1;
  }

  // Check for boolean literal true
  if (auto bool_lit = llvm::dyn_cast<clang::CXXBoolLiteralExpr>(expr)) {
    return bool_lit->getValue();
  }
  
  // Check for logical negation of false constant (!0U)
  if (auto unary_op = llvm::dyn_cast<clang::UnaryOperator>(expr)) {
    if (unary_op->getOpcode() == clang::UO_LNot) {
      return IsConstantFalse(unary_op->getSubExpr());
    }
  }
  
  return false;
}

bool BooleanSimplify::IsConstantFalse(clang::Expr *expr) {
  if (!expr) return false;
  
  // Strip any implicit casts and parentheses
  expr = expr->IgnoreParenImpCasts();
  
  // Check for integer literal 0 (including 0U)
  if (auto int_lit = llvm::dyn_cast<clang::IntegerLiteral>(expr)) {
    return int_lit->getValue() == 0;
  }
  
  // Check for boolean literal false
  if (auto bool_lit = llvm::dyn_cast<clang::CXXBoolLiteralExpr>(expr)) {
    return !bool_lit->getValue();
  }
  
  // Check for logical negation of true constant (!1U)
  if (auto unary_op = llvm::dyn_cast<clang::UnaryOperator>(expr)) {
    if (unary_op->getOpcode() == clang::UO_LNot) {
      return IsConstantTrue(unary_op->getSubExpr());
    }
  }
  
  return false;
}

bool BooleanSimplify::VisitIfStmt(clang::IfStmt *stmt) {
  // The condition will be simplified through the visitor pattern
  // by visiting BinaryOperator and UnaryOperator nodes
  //LOG(INFO) << "BooleanSimplify: Visiting IfStmt";
  if (stmt->getCond()) {
    //LOG(INFO) << "  Condition type: " << stmt->getCond()->getStmtClassName();
    
    // Print the condition for debugging
    std::string cond_str;
    llvm::raw_string_ostream stream(cond_str);
    stmt->printPretty(stream, nullptr, clang::PrintingPolicy(dec_ctx.ast_ctx.getLangOpts()));
    stream.flush();
    //LOG(INFO) << "  Condition text: " << cond_str;
    
    // Try to simplify the condition directly
    if (auto simplified = SimplifyBooleanExpr(stmt->getCond())) {
      if (simplified != stmt->getCond()) {
        stmt->setCond(simplified);
        changed = true;
        //LOG(INFO) << "  Simplified IfStmt condition";
      }
    }
  }
  return true;
}

bool BooleanSimplify::VisitWhileStmt(clang::WhileStmt *stmt) {
  // The condition will be simplified through the visitor pattern
  // by visiting BinaryOperator and UnaryOperator nodes
  return true;
}

bool BooleanSimplify::VisitBinaryOperator(clang::BinaryOperator *binop) {
  //LOG(INFO) << "BooleanSimplify: VisitBinaryOperator called with opcode " << binop->getOpcode();
  if (binop->getOpcode() == clang::BO_LAnd || binop->getOpcode() == clang::BO_LOr) {
    // Only handle simple cases where operands are constants
    auto lhs = binop->getLHS();
    auto rhs = binop->getRHS();
    
    // Debug: log what we're seeing
    //LOG(INFO) << "BooleanSimplify: Found " << (binop->getOpcode() == clang::BO_LAnd ? "AND" : "OR") << " operation";
    //LOG(INFO) << "  LHS type: " << lhs->getStmtClassName();
    //LOG(INFO) << "  RHS type: " << rhs->getStmtClassName();
    if (auto int_lit = llvm::dyn_cast<clang::IntegerLiteral>(lhs->IgnoreImpCasts())) {
      //LOG(INFO) << "  LHS is IntegerLiteral: " << int_lit->getValue().getZExtValue();
    }
    if (auto int_lit = llvm::dyn_cast<clang::IntegerLiteral>(rhs->IgnoreImpCasts())) {
      //LOG(INFO) << "  RHS is IntegerLiteral: " << int_lit->getValue().getZExtValue();
    }
    //LOG(INFO) << "  IsConstantTrue(lhs): " << IsConstantTrue(lhs);
    //LOG(INFO) << "  IsConstantFalse(lhs): " << IsConstantFalse(lhs);
    //LOG(INFO) << "  IsConstantTrue(rhs): " << IsConstantTrue(rhs);
    //LOG(INFO) << "  IsConstantFalse(rhs): " << IsConstantFalse(rhs);
    
    if (binop->getOpcode() == clang::BO_LAnd) {
      // Handle logical AND with constants
      if (IsConstantTrue(lhs) && !IsConstantFalse(rhs)) {
        // `1U && expr` -> `expr`
        //DLOG(INFO) << "  Simplifying: 1U && expr -> expr";
        substitutions[binop] = rhs;
        changed = true;
      } else if (IsConstantTrue(rhs) && !IsConstantFalse(lhs)) {
        // `expr && 1U` -> `expr`
        //DLOG(INFO) << "  Simplifying: expr && 1U -> expr";
        substitutions[binop] = lhs;
        changed = true;
      } else if (IsConstantFalse(lhs) || IsConstantFalse(rhs)) {
        // `0U && expr` or `expr && 0U` -> `false`
        //DLOG(INFO) << "  Simplifying: false && expr -> false";
        substitutions[binop] = dec_ctx.ast.CreateFalse();
        changed = true;
      }
    } else if (binop->getOpcode() == clang::BO_LOr) {
      //DLOG(INFO) << " OR";
      // Handle logical OR with constants
      if (IsConstantTrue(lhs) || IsConstantTrue(rhs)) {
        // `1U || expr` or `expr || 1U` -> `true`
        //DLOG(INFO) << "  Simplifying: true || expr -> true";
        substitutions[binop] = dec_ctx.ast.CreateTrue();
        changed = true;
      } else if (IsConstantFalse(lhs)) {
        // `0U || expr` -> `expr`
        //DLOG(INFO) << "  Simplifying: 0U || expr -> expr";
        substitutions[binop] = rhs;
        changed = true;
      } else if (IsConstantFalse(rhs)) {
        // `expr || 0U` -> `expr`
        //DLOG(INFO) << "  Simplifying: expr || 0U -> expr";
        substitutions[binop] = lhs;
        changed = true;
      }
    }
  }
  return true;
}

bool BooleanSimplify::VisitUnaryOperator(clang::UnaryOperator *unop) {
  if (unop->getOpcode() == clang::UO_LNot) {
    auto sub_expr = unop->getSubExpr();
    
    // Debug: log what we're seeing
    //LOG(INFO) << "BooleanSimplify: Found NOT operation";
    //LOG(INFO) << "  SubExpr type: " << sub_expr->getStmtClassName();
    if (auto int_lit = llvm::dyn_cast<clang::IntegerLiteral>(sub_expr->IgnoreImpCasts())) {
      //LOG(INFO) << "  SubExpr is IntegerLiteral: " << int_lit->getValue().getZExtValue();
    }
    //LOG(INFO) << "  IsConstantTrue(sub_expr): " << IsConstantTrue(sub_expr);
    //LOG(INFO) << "  IsConstantFalse(sub_expr): " << IsConstantFalse(sub_expr);
    
    // Only handle constants to avoid infinite loops
    if (IsConstantTrue(sub_expr)) {
      // `!1U` -> `false`
      //DLOG(INFO) << "  Simplifying: !1U -> false";
      substitutions[unop] = dec_ctx.ast.CreateFalse();
      changed = true;
    } else if (IsConstantFalse(sub_expr)) {
      // `!0U` -> `true`
      //DLOG(INFO) << "  Simplifying: !0U -> true";
      substitutions[unop] = dec_ctx.ast.CreateTrue();
      changed = true;
    }
  }
  return true;
}

bool BooleanSimplify::VisitConditionalOperator(clang::ConditionalOperator *cond_op) {
  //LOG(INFO) << "BooleanSimplify: Found ConditionalOperator";
  
  // Try to simplify the entire conditional expression
  if (auto simplified = SimplifyBooleanExpr(cond_op)) {
    if (simplified != cond_op) {
      substitutions[cond_op] = simplified;
      changed = true;
      //LOG(INFO) << "  Simplified ConditionalOperator";
    }
  }
  
  return true;
}

clang::Expr *BooleanSimplify::SimplifyBooleanExpr(clang::Expr *expr) {
  if (!expr) return expr;
  
  // Handle conditional operator (? :)
  if (auto cond_op = llvm::dyn_cast<clang::ConditionalOperator>(expr)) {
    // First recursively simplify all parts
    auto cond = SimplifyBooleanExpr(cond_op->getCond());
    auto true_expr = SimplifyBooleanExpr(cond_op->getTrueExpr());
    auto false_expr = SimplifyBooleanExpr(cond_op->getFalseExpr());
    
    // If the condition is constant, we can eliminate the conditional
    if (IsConstantTrue(cond)) {
      changed = true;
      return true_expr;
    } else if (IsConstantFalse(cond)) {
      changed = true;
      return false_expr;
    }
    
    // If both branches return the same constant, simplify to that constant
    // This handles cases like (cond ? 1U : 1U) -> 1U
    if (IsConstantTrue(true_expr) && IsConstantTrue(false_expr)) {
      changed = true;
      return dec_ctx.ast.CreateTrue();
    } else if (IsConstantFalse(true_expr) && IsConstantFalse(false_expr)) {
      changed = true;
      return dec_ctx.ast.CreateFalse();
    }
    
    // Check if both branches are identical integer literals
    if (auto true_int = llvm::dyn_cast<clang::IntegerLiteral>(true_expr->IgnoreParenImpCasts())) {
      if (auto false_int = llvm::dyn_cast<clang::IntegerLiteral>(false_expr->IgnoreParenImpCasts())) {
        if (true_int->getValue() == false_int->getValue()) {
          // Both branches return the same integer literal
          changed = true;
          return true_expr; // Return one of them
        }
      }
    }
    
    // If any part changed, create a new conditional
    if (cond != cond_op->getCond() || true_expr != cond_op->getTrueExpr() || false_expr != cond_op->getFalseExpr()) {
      changed = true;
      return dec_ctx.ast.CreateConditional(cond, true_expr, false_expr);
    }
  }
  
  // Handle binary operations (AND, OR)
  if (auto binop = llvm::dyn_cast<clang::BinaryOperator>(expr)) {
    auto lhs = SimplifyBooleanExpr(binop->getLHS());
    auto rhs = SimplifyBooleanExpr(binop->getRHS());
    
    if (binop->getOpcode() == clang::BO_LAnd) {
      // Handle logical AND patterns
      if (IsConstantTrue(lhs)) {
        // `true && expr` -> `expr`
        changed = true;
        return rhs;
      } else if (IsConstantTrue(rhs)) {
        // `expr && true` -> `expr`
        changed = true;
        return lhs;
      } else if (IsConstantFalse(lhs) || IsConstantFalse(rhs)) {
        // `false && expr` or `expr && false` -> `false`
        changed = true;
        return dec_ctx.ast.CreateFalse();
      }
    } else if (binop->getOpcode() == clang::BO_LOr) {
      // Handle logical OR patterns
      if (IsConstantTrue(lhs) || IsConstantTrue(rhs)) {
        // `true || expr` or `expr || true` -> `true`
        changed = true;
        return dec_ctx.ast.CreateTrue();
      } else if (IsConstantFalse(lhs)) {
        // `false || expr` -> `expr`
        changed = true;
        return rhs;
      } else if (IsConstantFalse(rhs)) {
        // `expr || false` -> `expr`
        changed = true;
        return lhs;
      }
    }
    
    // If operands changed, create new binary operation
    if (lhs != binop->getLHS() || rhs != binop->getRHS()) {
      changed = true;
      if (binop->getOpcode() == clang::BO_LAnd) {
        return dec_ctx.ast.CreateLAnd(lhs, rhs);
      } else if (binop->getOpcode() == clang::BO_LOr) {
        return dec_ctx.ast.CreateLOr(lhs, rhs);
      }
    }
  }
  
  // Handle unary operations (NOT)
  if (auto unop = llvm::dyn_cast<clang::UnaryOperator>(expr)) {
    if (unop->getOpcode() == clang::UO_LNot) {
      auto sub_expr = SimplifyBooleanExpr(unop->getSubExpr());
      
      if (IsConstantTrue(sub_expr)) {
        // `!true` -> `false`
        changed = true;
        return dec_ctx.ast.CreateFalse();
      } else if (IsConstantFalse(sub_expr)) {
        // `!false` -> `true`
        changed = true;
        return dec_ctx.ast.CreateTrue();
      }
      
      // If sub-expression changed, create new NOT operation
      if (sub_expr != unop->getSubExpr()) {
        changed = true;
        return dec_ctx.ast.CreateLNot(sub_expr);
      }
    }
  }
  
  // Convert standalone integer literals to boolean literals in boolean context
  if (auto int_lit = llvm::dyn_cast<clang::IntegerLiteral>(expr)) {
    if (int_lit->getValue() == 1) {
      changed = true;
      return dec_ctx.ast.CreateTrue();
    } else if (int_lit->getValue() == 0) {
      changed = true;
      return dec_ctx.ast.CreateFalse();
    }
  }
  
  return expr;
}

}  // namespace rellic 