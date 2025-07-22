/*
 * Copyright (c) 2021-present, Trail of Bits, Inc.
 * All rights reserved.
 *
 * This source code is licensed in accordance with the terms specified in
 * the LICENSE file found in the root directory of this source tree.
 */

#pragma once

#include <clang/AST/ASTContext.h>
#include <clang/Frontend/ASTUnit.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Value.h>
#include <z3++.h>

#include <unordered_map>

#include "rellic/AST/ASTBuilder.h"
#include "rellic/AST/ExceptionRegionInfo.h"
#include "rellic/AST/TypeProvider.h"

namespace rellic {

// Forward declaration
struct ExceptionCatchHandler;

struct DecompilationContext {
  using StmtToIRMap = std::unordered_map<clang::Stmt *, llvm::Value *>;
  using ExprToUseMap = std::unordered_map<clang::Expr *, llvm::Use *>;
  using IRToTypeDeclMap = std::unordered_map<llvm::Type *, clang::TypeDecl *>;
  using IRToValDeclMap = std::unordered_map<llvm::Value *, clang::ValueDecl *>;
  using IRToStmtMap = std::unordered_map<llvm::Value *, clang::Stmt *>;
  using ArgToTempMap = std::unordered_map<llvm::Argument *, clang::VarDecl *>;
  using BlockToUsesMap =
      std::unordered_map<llvm::BasicBlock *, std::vector<llvm::Use *>>;
  using Z3CondMap = std::unordered_map<clang::Stmt *, unsigned>;
  using BBToLineMap = std::unordered_map<std::string, unsigned>;
  
  // Expression position info struct
  struct ExpressionInfo {
    std::string type;  // Expression type (e.g. "Binary Add", "Unary Minus")
    std::string llvm_ir;  // LLVM IR instruction that generated this expression
    unsigned start_line;
    unsigned start_col;
    unsigned end_line;
    unsigned end_col;
  };
  
  // Map using expression ID as key
  using ExprPositionsMap = std::unordered_map<std::string, ExpressionInfo>;

  using BBEdge = std::pair<llvm::BasicBlock *, llvm::BasicBlock *>;
  using BrEdge = std::pair<llvm::BranchInst *, bool>;
  using SwEdge = std::pair<llvm::SwitchInst *, llvm::ConstantInt *>;
  using InvokeEdge = std::pair<llvm::InvokeInst *, bool>;

  DecompilationContext(clang::ASTUnit &ast_unit);

  // Helper function to generate a unique ID for an expression
  std::string GenerateExpressionId(clang::Expr *expr);

  clang::ASTUnit &ast_unit;
  clang::ASTContext &ast_ctx;
  ASTBuilder ast;

  std::unique_ptr<TypeProviderCombiner> type_provider;

  StmtToIRMap stmt_provenance;
  ExprToUseMap use_provenance;
  IRToTypeDeclMap type_decls;
  IRToValDeclMap value_decls;
  ArgToTempMap temp_decls;
  BlockToUsesMap outgoing_uses;
  z3::context z3_ctx;
  z3::expr_vector z3_exprs{z3_ctx};
  Z3CondMap conds;
  BBToLineMap bb_line_map;
  ExprPositionsMap expr_positions;
  std::unordered_map<const llvm::Function*, unsigned> function_start_lines;
  std::unordered_map<std::string, clang::Stmt*> bb_first_stmt_map;
  std::unordered_map<std::string, bool> bb_is_entry;
  std::unordered_map<clang::Stmt*, std::string> stmt_to_bb;
  std::unordered_map<std::string, std::vector<clang::Stmt*>> bb_to_stmts;
  
  // Map control flow statements to their corresponding BBs
  std::unordered_map<clang::Stmt*, std::string> control_flow_to_bb;
  // Map BBs to their LLVM BasicBlock for better tracking
  std::unordered_map<std::string, llvm::BasicBlock*> bb_name_to_llvm_bb;
  // Map statements to their containing function for proper scoping
  std::unordered_map<clang::Stmt*, llvm::Function*> stmt_to_func;
  // Track which BBs belong to which functions
  std::unordered_map<std::string, llvm::Function*> bb_to_func;

  // Current line number during decompilation
  unsigned current_line{1};

  clang::Expr *marker_expr;

  std::unordered_map<unsigned, BrEdge> z3_br_edges_inv;

  // Pairs do not have a std::hash specialization so we can't use unordered maps
  // here. If this turns out to be a performance issue, investigate adding hash
  // specializations for these specifically
  std::map<BrEdge, unsigned> z3_br_edges;

  std::unordered_map<llvm::SwitchInst *, unsigned> z3_sw_vars;
  std::unordered_map<unsigned, llvm::SwitchInst *> z3_sw_vars_inv;
  std::map<SwEdge, unsigned> z3_sw_edges;
  
  std::unordered_map<unsigned, InvokeEdge> z3_invoke_edges_inv;
  std::map<InvokeEdge, unsigned> z3_invoke_edges;

  std::map<BBEdge, unsigned> z3_edges;
  std::unordered_map<llvm::BasicBlock *, unsigned> reaching_conds;

  size_t num_literal_structs = 0;
  size_t num_declared_structs = 0;

  // Exception region analysis results
  ExceptionRegionInfo exception_regions;
  
  // Map statements to their exception context
  // Key: Clang Stmt*, Value: Exception context (try block, catch handler type, etc.)
  struct ExceptionContext {
    enum Type { NONE, TRY_BLOCK, CATCH_HANDLER } type = NONE;
    std::string exception_type;  // For catch handlers, the exception type being caught
    llvm::BasicBlock* landing_pad = nullptr;  // Associated landing pad
  };
  std::unordered_map<clang::Stmt*, ExceptionContext> stmt_exception_context;
  
  // Map basic blocks to their exception handler info
  std::unordered_map<llvm::BasicBlock*, const ExceptionCatchHandler*> block_exception_info;
  
  // Map statements to their source basic block (improved provenance)
  std::unordered_map<clang::Stmt*, llvm::BasicBlock*> stmt_to_block;

  // Inserts an expression into z3_exprs and returns its index
  unsigned InsertZExpr(const z3::expr &e);

  clang::QualType GetQualType(llvm::Type *type);
};

}  // namespace rellic