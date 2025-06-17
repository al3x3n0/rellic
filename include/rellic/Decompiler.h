/*
 * Copyright (c) 2021-present, Trail of Bits, Inc.
 * All rights reserved.
 *
 * This source code is licensed in accordance with the terms specified in
 * the LICENSE file found in the root directory of this source tree.
 */

#pragma once

#include <clang/Frontend/ASTUnit.h>
#include <llvm/IR/Module.h>

#include <memory>
#include <unordered_map>
#include <vector>

#include "Result.h"
#include "rellic/AST/TypeProvider.h"

namespace rellic {

// Expression position info struct
struct ExpressionInfo {
  std::string type;  // Expression type (e.g. "Binary Add", "Unary Minus")
  std::string llvm_ir;  // LLVM IR instruction that generated this expression
  unsigned start_line;
  unsigned start_col;
  unsigned end_line;
  unsigned end_col;
};

/* This additional level of indirection is needed to alleviate the users from
 * the burden of having to instantiate custom TypeProviders before the actual
 * DecompilationContext has been created */
class TypeProviderFactory {
 public:
  virtual ~TypeProviderFactory() = default;
  virtual std::unique_ptr<TypeProvider> create(DecompilationContext& ctx) = 0;
};

template <typename T>
class SimpleTypeProviderFactory final : public TypeProviderFactory {
 public:
  std::unique_ptr<TypeProvider> create(DecompilationContext& ctx) override {
    return std::make_unique<T>(ctx);
  }
};

struct DecompilationOptions {
  using TypeProviderFactoryPtr = std::unique_ptr<TypeProviderFactory>;

  bool lower_switches = false;
  bool remove_phi_nodes = false;

  // Additional type providers to be used during code generation.
  // Providers added later will have higher priority.
  std::vector<TypeProviderFactoryPtr> additional_providers;
};

struct DecompilationResult {
  using StmtToIRMap =
      std::unordered_map<const clang::Stmt*, const llvm::Value*>;
  using DeclToIRMap =
      std::unordered_map<const clang::ValueDecl*, const llvm::Value*>;
  using TypeDeclToIRMap =
      std::unordered_map<const clang::TypeDecl*, const llvm::Type*>;
  using ExprToUseMap = std::unordered_map<const clang::Expr*, const llvm::Use*>;
  using IRToStmtMap =
      std::unordered_map<const llvm::Value*, const clang::Stmt*>;
  using IRToDeclMap =
      std::unordered_map<const llvm::Value*, const clang::ValueDecl*>;
  using IRToTypeDeclMap =
      std::unordered_map<const llvm::Type*, const clang::TypeDecl*>;
  using UseToExprMap = std::unordered_map<const llvm::Use*, const clang::Expr*>;
  using BBToLineMap = std::unordered_map<std::string, unsigned>;
  using ExprPositionsMap = std::unordered_map<std::string, ExpressionInfo>;

  std::unique_ptr<llvm::Module> module;
  std::unique_ptr<clang::ASTUnit> ast;
  StmtToIRMap stmt_provenance_map;
  IRToStmtMap value_to_stmt_map;
  DeclToIRMap decl_provenance_map;
  IRToDeclMap value_to_decl_map;
  TypeDeclToIRMap type_provenance_map;
  IRToTypeDeclMap type_to_decl_map;
  ExprToUseMap expr_use_map;
  UseToExprMap use_expr_map;
  BBToLineMap bb_to_line_map;
  ExprPositionsMap expr_positions;
  std::unordered_map<const llvm::Function*, unsigned> function_start_lines;
  std::unordered_map<std::string, clang::Stmt*> bb_first_stmt_map;
  std::unordered_map<clang::Stmt*, std::string> stmt_to_bb;
  std::unordered_map<std::string, bool> bb_is_entry;
  std::unordered_map<std::string, std::vector<clang::Stmt*>> bb_to_stmts;
  std::unordered_map<clang::Stmt*, std::string> control_flow_to_bb;
  std::unordered_map<std::string, llvm::BasicBlock*> bb_name_to_llvm_bb;
  std::unordered_map<clang::Stmt*, llvm::Function*> stmt_to_func;
  std::unordered_map<std::string, llvm::Function*> bb_to_func;
};

struct DecompilationError {
  std::unique_ptr<llvm::Module> module;
  std::unique_ptr<clang::ASTUnit> ast;
  std::string message;
};

Result<DecompilationResult, DecompilationError> Decompile(
    std::unique_ptr<llvm::Module> module, DecompilationOptions options = {});
}  // namespace rellic
