/*
 * Copyright (c) 2021-present, Trail of Bits, Inc.
 * All rights reserved.
 *
 * This source code is licensed in accordance with the terms specified in
 * the LICENSE file found in the root directory of this source tree.
 */

#pragma once

#include <clang/AST/Stmt.h>
#include <llvm/Analysis/RegionInfo.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Module.h>
#include <llvm/Passes/PassBuilder.h>
#include <z3++.h>

#include <limits>
#include <map>
#include <unordered_set>

#include "rellic/AST/ASTBuilder.h"
#include "rellic/AST/IRToASTVisitor.h"

namespace rellic {

class DecompilationContext;

class GenerateAST : public llvm::AnalysisInfoMixin<GenerateAST> {
 private:
  friend llvm::AnalysisInfoMixin<GenerateAST>;
  static llvm::AnalysisKey Key;

  constexpr static unsigned poison_idx = std::numeric_limits<unsigned>::max();
  z3::expr ToExpr(unsigned idx);

  rellic::IRToASTVisitor ast_gen;
  DecompilationContext &dec_ctx;
  ASTBuilder &ast;
  bool reaching_conds_changed{true};
  std::unordered_map<llvm::BasicBlock *, clang::IfStmt *> block_stmts;
  std::unordered_map<llvm::Region *, clang::CompoundStmt *> region_stmts;

  llvm::DominatorTree *domtree;
  llvm::RegionInfo *regions;
  llvm::LoopInfo *loops;

  std::vector<llvm::BasicBlock *> rpo_walk;

  // Line number tracking
  unsigned current_line{1};
  std::unordered_map<std::string, unsigned> bb_line_map;

  // GetOrCreateEdgeForBranch(branch, true) will return the index of an
  // expression that is true when branch is taken.
  // Viceversa, GetOrCreateEdgeForBranch(branch, false) is an expression that
  // will be true when branch is not taken
  unsigned GetOrCreateEdgeForBranch(llvm::BranchInst *inst, bool cond);

  // Returns the index of an expression containing a numerical variable that
  // represents the condition of a switch.
  unsigned GetOrCreateVarForSwitch(llvm::SwitchInst *inst);
  // Returns the index of an expression that is true when a particular case of a
  // switch is taken. If c is nullptr, the expression for the default case will
  // be returned.
  unsigned GetOrCreateEdgeForSwitch(llvm::SwitchInst *inst,
                                    llvm::ConstantInt *c);

  unsigned GetOrCreateEdgeCond(llvm::BasicBlock *from, llvm::BasicBlock *to);
  unsigned GetReachingCond(llvm::BasicBlock *block);
  void CreateReachingCond(llvm::BasicBlock *block);

  std::vector<clang::Stmt *> CreateBasicBlockStmts(llvm::BasicBlock *block);
  std::vector<clang::Stmt *> CreateRegionStmts(llvm::Region *region);

  using BBSet = std::unordered_set<llvm::BasicBlock *>;

  void RefineLoopSuccessors(llvm::Loop *loop, BBSet &members,
                            BBSet &successors);

  clang::CompoundStmt *StructureAcyclicRegion(llvm::Region *region);
  clang::CompoundStmt *StructureCyclicRegion(llvm::Region *region);
  clang::CompoundStmt *StructureSwitchRegion(llvm::Region *region);
  clang::CompoundStmt *StructureRegion(llvm::Region *region);

 public:
  using Result = llvm::PreservedAnalyses;
  GenerateAST(DecompilationContext &dec_ctx);

  Result run(llvm::Function &F, llvm::FunctionAnalysisManager &FAM);
  Result run(llvm::Module &M, llvm::ModuleAnalysisManager &MAM);

  static void run(llvm::Module &M, DecompilationContext &dec_ctx);

  // Get the basic block to line number mapping
  const std::unordered_map<std::string, unsigned>& GetBBToLineMap() const {
    return bb_line_map;
  }
};

}  // namespace rellic
