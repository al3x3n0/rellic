/*
 * Copyright (c) 2021-present, Trail of Bits, Inc.
 * All rights reserved.
 *
 * This source code is licensed in accordance with the terms specified in
 * the LICENSE file found in the root directory of this source tree.
 */

#pragma once

#include "rellic/AST/TransformVisitor.h"
#include <llvm/IR/BasicBlock.h>
#include <unordered_map>
#include <vector>
#include <string>

namespace clang {
class FunctionDecl;
class CompoundStmt;
class CXXTryStmt;
class CXXCatchStmt;
class Stmt;
}  // namespace clang

namespace rellic {

class DecompilationContext;
struct TryRegion;

/*
 * AST post-processing pass that transforms the existing AST to add try-catch blocks
 * based on the exception region analysis saved in DecompilationContext.
 * 
 * This pass:
 * 1. Reads the exception region information from DecompilationContext
 * 2. Identifies which existing AST statements belong to try/catch regions
 * 3. Reorganizes the AST to wrap statements in proper try-catch blocks
 * 4. Removes exception infrastructure calls (__cxa_begin_catch, etc.)
 */
class ExceptionASTTransform : public TransformVisitor<ExceptionASTTransform> {
public:
  ExceptionASTTransform(DecompilationContext &dec_ctx);

  bool VisitFunctionDecl(clang::FunctionDecl *func);
  
  void RunImpl();
  
private:
  // Transform the function body to add try-catch blocks
  clang::CompoundStmt* TransformFunctionBody(clang::CompoundStmt* body,
                                            clang::FunctionDecl* func_decl);
  
  // Transform using pre-computed exception context mapping
  clang::CompoundStmt* TransformUsingExceptionContext(clang::CompoundStmt* body,
                                                     clang::FunctionDecl* func_decl);
  
  // Generate nested try-catch structure from exception regions
  clang::Stmt* CreateNestedTryCatch(const TryRegion& region, 
                                    const std::unordered_map<llvm::BasicBlock*, std::vector<clang::Stmt*>>& block_to_stmts);
  
  // Create nested try-catch structure using parent-child relationships
  clang::CompoundStmt* CreateNestedTryCatchFromRegions(const TryRegion* region,
                                                      clang::FunctionDecl* func_decl,
                                                      llvm::Function* llvm_func);
  
  // Transform using direct mapping from exception regions
  clang::CompoundStmt* TransformUsingDirectMapping(clang::CompoundStmt* body,
                                                  clang::FunctionDecl* func_decl,
                                                  llvm::Function* llvm_func);
  
  // Helper methods
  bool IsExceptionInfrastructure(clang::Stmt* stmt, bool in_catch_handler = false);
  bool IsInfrastructureBlock(llvm::BasicBlock* block);
  void FilterExceptionInfrastructure(clang::Stmt* stmt, std::vector<clang::Stmt*>& result, bool in_catch_handler = false);
  clang::Stmt* TransformThrowCall(clang::CallExpr* call);
  clang::Stmt* TransformRethrowCall(clang::CallExpr* call);
  std::string CreateVariableNameFromType(const std::string& exception_type);
  clang::QualType CreateExceptionType(const std::string& exception_type_name,
                                     DecompilationContext& dec_ctx);
  bool IsAncestorOf(clang::Stmt* parent, clang::Stmt* child);
  bool HasMarkerCondition(clang::IfStmt* if_stmt, DecompilationContext& dec_ctx);
  
  // Create a unified try-catch block from multiple regions with same handlers
  clang::CompoundStmt* CreateUnifiedTryCatch(clang::CompoundStmt* body,
                                            clang::FunctionDecl* func_decl,
                                            llvm::Function* llvm_func,
                                            const std::vector<const TryRegion*>& regions);
};

}  // namespace rellic