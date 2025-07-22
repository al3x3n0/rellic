/*
 * Copyright (c) 2021-present, Trail of Bits, Inc.
 * All rights reserved.
 *
 * This source code is licensed in accordance with the terms specified in
 * the LICENSE file found in the root directory of this source tree.
 */
#include "rellic/AST/DeadStmtElim.h"

#include <gflags/gflags.h>
#include <glog/logging.h>

namespace rellic {

DeadStmtElim::DeadStmtElim(DecompilationContext &dec_ctx, bool eliminate_zero_patterns)
    : TransformVisitor<DeadStmtElim>(dec_ctx), eliminate_zero_patterns(eliminate_zero_patterns) {}

bool DeadStmtElim::VisitIfStmt(clang::IfStmt *ifstmt) {
  LOG(INFO) << "DeadStmtElim VisitIfStmt";
  bool can_delete = false;

  std::string cond_str;
  llvm::raw_string_ostream stream(cond_str);
  ifstmt->printPretty(stream, nullptr, dec_ctx.ast_ctx.getPrintingPolicy());
  stream.flush();
  LOG(INFO) << "DeadStmtElim: IfStmt: eliminate_zero_patterns=" << eliminate_zero_patterns << " " << cond_str;

  // Check for if(0U) pattern
  if (auto cond = ifstmt->getCond()) {
    // Debug: print the condition details
    LOG(INFO) << "  Condition type: " << cond->getStmtClassName();
    
    // Print the raw condition
    std::string raw_cond_str;
    llvm::raw_string_ostream raw_stream(raw_cond_str);
    cond->printPretty(raw_stream, nullptr, dec_ctx.ast_ctx.getPrintingPolicy());
    raw_stream.flush();
    LOG(INFO) << "  Raw condition: " << raw_cond_str;
    
    auto cond_no_casts = cond->IgnoreParenImpCasts();
    LOG(INFO) << "  After IgnoreParenImpCasts: " << cond_no_casts->getStmtClassName();
    
    // Print the condition after ignoring casts
    std::string no_cast_str;
    llvm::raw_string_ostream no_cast_stream(no_cast_str);
    cond_no_casts->printPretty(no_cast_stream, nullptr, dec_ctx.ast_ctx.getPrintingPolicy());
    no_cast_stream.flush();
    LOG(INFO) << "  Condition after IgnoreCasts: " << no_cast_str;

    if (auto int_lit = llvm::dyn_cast<clang::IntegerLiteral>(cond_no_casts)) {
      LOG(INFO) << "  Integer literal value: " << int_lit->getValue().getZExtValue();
      if (int_lit->getValue() == 0) {
        can_delete = true;
        LOG(INFO) << "  -> Found if(0) - marking for deletion";
      }
    } else if (eliminate_zero_patterns) {
      // When flag is enabled, check for patterns like "0U + 0U"
      if (auto binop = llvm::dyn_cast<clang::BinaryOperator>(cond_no_casts)) {
        LOG(INFO) << "  Binary operator opcode: " << binop->getOpcodeStr().str();
        if (binop->getOpcode() == clang::BO_Add) {
          auto lhs = binop->getLHS()->IgnoreParenImpCasts();
          auto rhs = binop->getRHS()->IgnoreParenImpCasts();
          LOG(INFO) << "  LHS type: " << lhs->getStmtClassName();
          LOG(INFO) << "  RHS type: " << rhs->getStmtClassName();
          
          if (auto lhs_int = llvm::dyn_cast<clang::IntegerLiteral>(lhs)) {
            if (auto rhs_int = llvm::dyn_cast<clang::IntegerLiteral>(rhs)) {
              LOG(INFO) << "  Both operands are integer literals: " 
                        << lhs_int->getValue().getZExtValue() 
                        << " + " << rhs_int->getValue().getZExtValue();
              // Only eliminate if BOTH operands are literal zero
              if (lhs_int->getValue() == 0 && rhs_int->getValue() == 0) {
                can_delete = true;
                LOG(INFO) << "  -> Found if(0U + 0U) pattern - marking for deletion";
              } else {
                LOG(INFO) << "  -> Binary add with non-zero literals, keeping";
              }
            } else {
              LOG(INFO) << "  -> RHS not an integer literal";
            }
          } else {
            LOG(INFO) << "  -> LHS not an integer literal";
          }
        } else if (binop->getOpcode() == clang::BO_EQ) {
          LOG(INFO) << "  -> This is an equality comparison (==), not addition";
        } else {
          LOG(INFO) << "  -> Binary operator but not addition or equality";
        }
      } else {
        LOG(INFO) << "  -> Not an integer literal or binary operator";
      }
    } else {
      LOG(INFO) << "  -> Not an integer literal";
    }
  } else {
    LOG(INFO) << "  -> No condition found";
  }

  if (ifstmt->getCond() == dec_ctx.marker_expr) {
    can_delete = Prove(!dec_ctx.z3_exprs[dec_ctx.conds[ifstmt]]);
  }

  auto compound = clang::dyn_cast<clang::CompoundStmt>(ifstmt->getThen());
  bool is_empty = compound ? compound->body_empty() : false;
  if (can_delete || is_empty) {
    LOG(INFO) << "DeadStmtElim: Marking IfStmt for deletion (can_delete=" << can_delete 
              << ", is_empty=" << is_empty << ")";
    substitutions[ifstmt] = nullptr;
  }
  return true;
}

bool DeadStmtElim::VisitCompoundStmt(clang::CompoundStmt *compound) {
  // DLOG(INFO) << "VisitCompoundStmt";
  std::vector<clang::Stmt *> new_body;
  for (auto stmt : compound->body()) {
    // Filter out nullptr statements
    if (!stmt) {
      continue;
    }
    // Add only necessary statements
    if (auto expr = clang::dyn_cast<clang::Expr>(stmt)) {
      if (expr->HasSideEffects(dec_ctx.ast_ctx)) {
        new_body.push_back(stmt);
      }
    } else if (!clang::dyn_cast<clang::NullStmt>(stmt)) {
      new_body.push_back(stmt);
    }
  }
  // Create the a new compound
  if (changed || new_body.size() < compound->size()) {
    substitutions[compound] = dec_ctx.ast.CreateCompoundStmt(new_body);
  }
  return !Stopped();
}

void DeadStmtElim::RunImpl() {
  LOG(INFO) << "========== DeadStmtElim Starting (eliminate_zero_patterns=" 
            << (eliminate_zero_patterns ? "true" : "false") << ") ==========";
  
  // Clear substitutions from base class
  TransformVisitor<DeadStmtElim>::RunImpl();
  
  // Traverse the AST to collect substitutions
  TraverseDecl(dec_ctx.ast_ctx.getTranslationUnitDecl());
  
  LOG(INFO) << "DeadStmtElim collected " << substitutions.size() << " substitutions";
  for (const auto& [stmt, replacement] : substitutions) {
    LOG(INFO) << "  Substitution: " << stmt->getStmtClassName() 
              << " -> " << (replacement ? replacement->getStmtClassName() : "nullptr");
  }
  LOG(INFO) << "========== DeadStmtElim Completed ==========";
}

}  // namespace rellic
