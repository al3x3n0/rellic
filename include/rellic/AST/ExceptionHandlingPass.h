/*
 * Copyright (c) 2021-present, Trail of Bits, Inc.
 * All rights reserved.
 *
 * This source code is licensed in accordance with the terms specified in
 * the LICENSE file found in the root directory of this source tree.
 */

#pragma once

#include "rellic/AST/TransformVisitor.h"

#include <unordered_set>

namespace llvm {
class Function;
class BasicBlock;
class LandingPadInst;
class DominatorTree;
}  // namespace llvm

namespace clang {
class CallExpr;
class FunctionDecl;
class Stmt;
}  // namespace clang

namespace rellic {

class DecompilationContext;

/*
 * Exception handling analysis pass that identifies exception regions in LLVM IR
 * and saves the analysis results to DecompilationContext for later use.
 * 
 * This pass:
 * 1. Identifies landingpad blocks and their associated try regions
 * 2. Analyzes catch handlers and their control flow
 * 3. Saves the analysis results to ExceptionRegionInfo in DecompilationContext
 * 
 * The actual AST transformation is done by a separate post-processing pass.
 */
class ExceptionHandlingPass : public TransformVisitor<ExceptionHandlingPass> {
private:
  bool enable_try_catch_transformation_;

public:
  ExceptionHandlingPass(DecompilationContext &dec_ctx, bool enable_try_catch = true);

  bool VisitFunctionDecl(clang::FunctionDecl *func);
  
  // Main analysis method
  bool AnalyzeFunctionForExceptions(clang::FunctionDecl* func_decl, DecompilationContext& dec_ctx);

  // Legacy methods (kept for compatibility)
  bool IsBeginCatchCall(clang::CallExpr *call_expr);
  bool IsEndCatchCall(clang::CallExpr *call_expr);
  bool IsExceptionInfrastructureStatement(clang::Stmt* stmt);
  std::string DemangleExceptionType(const std::string& mangled_name);
  std::string CreateVariableNameFromType(const std::string& exception_type);
  clang::QualType CreateExceptionType(const std::string& exception_type_name);

  void RunImpl();
  void AnalyzeAllFunctionsWithExceptions(llvm::Module& module);
  
private:
  // Helper methods
  void AnalyzeCatchHandlers(llvm::BasicBlock* landing_pad, 
                           llvm::LandingPadInst* landing_inst,
                           DecompilationContext& dec_ctx,
                           llvm::DominatorTree& dom_tree,
                           const std::unordered_set<llvm::BasicBlock*>& all_handler_blocks);
  llvm::Function* GetLLVMFunction(clang::FunctionDecl* func_decl);
  
  // Populate the statement exception context map
  void PopulateStatementExceptionContext(DecompilationContext& dec_ctx);
  
  // Clean up try regions to remove catch handler blocks  
  void CleanupTryRegions(DecompilationContext& dec_ctx);
  
  // Merge try regions that should be unified
  void MergeTryRegions(DecompilationContext& dec_ctx);
};

}  // namespace rellic