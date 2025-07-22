/*
 * Copyright (c) 2021-present, Trail of Bits, Inc.
 * All rights reserved.
 *
 * This source code is licensed in accordance with the terms specified in
 * the LICENSE file found in the root directory of this source tree.
 */

#pragma once

#include "rellic/AST/ASTPass.h"
#include "rellic/AST/IRToASTVisitor.h"
#include "rellic/AST/TransformVisitor.h"

namespace rellic {

/*
 * This pass eliminates statements that have no effect
 */
class DeadStmtElim : public TransformVisitor<DeadStmtElim> {
 private:
  bool eliminate_zero_patterns;  // Flag to enable elimination of 0+0 patterns
  
 protected:
  void RunImpl() override;

 public:
  DeadStmtElim(DecompilationContext &dec_ctx, bool eliminate_zero_patterns = false);

  bool VisitIfStmt(clang::IfStmt *ifstmt);
  bool VisitCompoundStmt(clang::CompoundStmt *compound);
};

}  // namespace rellic