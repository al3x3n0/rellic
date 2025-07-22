/*
 * Copyright (c) 2021-present, Trail of Bits, Inc.
 * All rights reserved.
 *
 * This source code is licensed in accordance with the terms specified in
 * the LICENSE file found in the root directory of this source tree.
 */

#include "rellic/Decompiler.h"

#include <clang/Basic/TargetInfo.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/PassManager.h>
#include <llvm/InitializePasses.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/Utils.h>
#include <llvm/Transforms/Utils/Local.h>
#include <llvm/Transforms/Utils/LowerSwitch.h>

#include <memory>

#include <glog/logging.h>

#include "rellic/AST/BooleanSimplify.h"
#include "rellic/AST/CondBasedRefine.h"
#include "rellic/AST/DeadStmtElim.h"
#include "rellic/AST/DebugInfoCollector.h"
#include "rellic/AST/ExceptionHandlingPass.h"
#include "rellic/AST/ExceptionASTTransform.h"
#include "rellic/AST/ExprCombine.h"
#include "rellic/AST/Z3CondSimplify.h"
#include "rellic/AST/GenerateAST.h"
#include "rellic/AST/IRToASTVisitor.h"
#include "rellic/AST/LocalDeclRenamer.h"
#include "rellic/AST/LoopRefine.h"
#include "rellic/AST/MaterializeConds.h"
#include "rellic/AST/NestedCondProp.h"
#include "rellic/AST/NestedScopeCombine.h"
#include "rellic/AST/ReachBasedRefine.h"
#include "rellic/AST/StructFieldRenamer.h"
#include "rellic/AST/Z3CondSimplify.h"
#include "rellic/BC/Util.h"
#include "rellic/Exception.h"

namespace {

static void InitOptPasses(void) {
  auto& pr{*llvm::PassRegistry::getPassRegistry()};
  initializeCore(pr);
  initializeAnalysis(pr);
}
}  // namespace

template <typename TKey, typename TValue>
static void CopyMap(const std::unordered_map<TKey*, TValue*>& from,
                    std::unordered_map<const TKey*, const TValue*>& to,
                    std::unordered_map<const TValue*, const TKey*>& inverse) {
  for (auto [key, value] : from) {
    if (value) {
      to[key] = value;
      inverse[value] = key;
    }
  }
}

namespace rellic {
Result<DecompilationResult, DecompilationError> Decompile(
    std::unique_ptr<llvm::Module> module, DecompilationOptions options) {
  try {
    if (options.remove_phi_nodes) {
      RemovePHINodes(*module);
    }

    if (options.lower_switches) {
      LowerSwitches(*module);
    }

    ConvertArrayArguments(*module);
    RemoveInsertValues(*module);

    InitOptPasses();
    rellic::DebugInfoCollector dic;
    dic.visit(*module);

    std::vector<std::string> args{"-Wno-pointer-to-int-cast",
                                  "-Wno-pointer-sign", "-target",
                                  module->getTargetTriple()};
    // Silence clang warning
    // warning: unknown platform, assumming -mfloat-abi=soft
    const auto& triple{llvm::Triple(module->getTargetTriple())};
    if (triple.isARM()) {
        args.push_back("-mfloat-abi=soft");
    }
    auto ast_unit{clang::tooling::buildASTFromCodeWithArgs("", args, "out.c")};
    rellic::DecompilationContext dec_ctx(*ast_unit);

    for (auto& provider : options.additional_providers) {
      dec_ctx.type_provider->AddProvider(provider->create(dec_ctx));
    }

    rellic::GenerateAST::run(*module, dec_ctx);
    // TODO(surovic): Add llvm::Value* -> clang::Decl* map
    // Especially for llvm::Argument* and llvm::Function*.

    rellic::CompositeASTPass pass_ast(dec_ctx);
    auto& ast_passes{pass_ast.GetPasses()};

    ast_passes.push_back(std::make_unique<rellic::DeadStmtElim>(dec_ctx));
    ast_passes.push_back(std::make_unique<rellic::LocalDeclRenamer>(
        dec_ctx, dic.GetIRToNameMap()));
    ast_passes.push_back(std::make_unique<rellic::StructFieldRenamer>(
        dec_ctx, dic.GetIRTypeToDITypeMap()));
    pass_ast.Run();
    
    // EXCEPTION ANALYSIS: Analyze exception regions after AST generation
    LOG(INFO) << "Checking exception flag: " << (options.enable_exception_try_catch ? "ENABLED" : "DISABLED");
    if (options.enable_exception_try_catch) {
      LOG(INFO) << "Exception handling is ENABLED, starting analysis...";
      auto exception_pass = std::make_unique<rellic::ExceptionHandlingPass>(dec_ctx, options.enable_exception_try_catch);
      // Analyze exception regions after AST generation when Clang declarations exist
      LOG(INFO) << "Starting exception analysis...";
      exception_pass->AnalyzeAllFunctionsWithExceptions(*module);
      LOG(INFO) << "Exception analysis completed, found " << dec_ctx.exception_regions.GetTryRegions().size() << " try regions";
      
      // Also check what's in the exception regions
      auto& try_regions = dec_ctx.exception_regions.GetTryRegions();
      for (size_t i = 0; i < try_regions.size(); ++i) {
        const auto& region = try_regions[i];
        std::string landingpad_str;
        llvm::raw_string_ostream landingpad_stream(landingpad_str);
        region.landingpad_block->printAsOperand(landingpad_stream, false);
        landingpad_stream.flush();
        LOG(INFO) << "Try region " << i << ": landingpad " << landingpad_str 
                  << ", " << region.blocks.size() << " try blocks, "
                  << region.catch_handlers.size() << " catch handlers";
      }
      
      // EXCEPTION AST TRANSFORMATION: Transform the AST based on the analyzed exception regions
      LOG(INFO) << "Starting exception AST transformation...";
      rellic::CompositeASTPass pass_exception_transform{dec_ctx};
      auto& exception_transform_passes{pass_exception_transform.GetPasses()};
      exception_transform_passes.push_back(std::make_unique<rellic::ExceptionASTTransform>(dec_ctx));
      pass_exception_transform.Run();
      LOG(INFO) << "Exception AST transformation completed";
      
      // RUN BOOLEAN SIMPLIFICATION AFTER EXCEPTION TRANSFORMATION
      LOG(INFO) << "Running boolean simplification after exception transformation...";
      rellic::CompositeASTPass pass_exception_cleanup{dec_ctx};
      auto& exception_cleanup_passes{pass_exception_cleanup.GetPasses()};
      exception_cleanup_passes.push_back(std::make_unique<rellic::BooleanSimplify>(dec_ctx));
      exception_cleanup_passes.push_back(std::make_unique<rellic::Z3CondSimplify>(dec_ctx));
      exception_cleanup_passes.push_back(std::make_unique<rellic::BooleanSimplify>(dec_ctx)); // Run again after Z3 simplification
      
      // Run cleanup passes multiple times until no more changes
      int cleanup_iterations = 0;
      while (pass_exception_cleanup.Run() && cleanup_iterations < 5) {
        cleanup_iterations++;
        LOG(INFO) << "Exception cleanup iteration " << cleanup_iterations;
      }
      LOG(INFO) << "Boolean simplification after exception transformation completed (" << cleanup_iterations << " iterations)";
      
      rellic::CompositeASTPass pass_post_exception_cleanup{dec_ctx};
      auto& post_exception_passes{pass_post_exception_cleanup.GetPasses()};
      // Now it's safe to aggressively eliminate if(0+0) patterns since catch blocks are formed
      post_exception_passes.push_back(std::make_unique<rellic::DeadStmtElim>(dec_ctx, true));
      pass_post_exception_cleanup.Run();
      LOG(INFO) << "Aggressive dead statement elimination completed";
      
    } else {
      LOG(INFO) << "Exception handling is DISABLED, skipping analysis";
    }

    rellic::CompositeASTPass pass_cbr(dec_ctx);
    auto& cbr_passes{pass_cbr.GetPasses()};

    cbr_passes.push_back(std::make_unique<rellic::Z3CondSimplify>(dec_ctx));
    cbr_passes.push_back(std::make_unique<rellic::BooleanSimplify>(dec_ctx));
    cbr_passes.push_back(std::make_unique<rellic::NestedCondProp>(dec_ctx));

    cbr_passes.push_back(std::make_unique<rellic::NestedScopeCombine>(dec_ctx));

    cbr_passes.push_back(std::make_unique<rellic::CondBasedRefine>(dec_ctx));
    cbr_passes.push_back(std::make_unique<rellic::ReachBasedRefine>(dec_ctx));
    cbr_passes.push_back(std::make_unique<rellic::DeadStmtElim>(dec_ctx, true));

    pass_cbr.Run();

    rellic::CompositeASTPass pass_loop{dec_ctx};
    auto& loop_passes{pass_loop.GetPasses()};

    loop_passes.push_back(std::make_unique<rellic::LoopRefine>(dec_ctx));
    loop_passes.push_back(std::make_unique<rellic::NestedCondProp>(dec_ctx));
    loop_passes.push_back(
        std::make_unique<rellic::NestedScopeCombine>(dec_ctx));

    pass_loop.Run();

    rellic::CompositeASTPass pass_scope{dec_ctx};
    auto& scope_passes{pass_scope.GetPasses()};
    scope_passes.push_back(std::make_unique<rellic::Z3CondSimplify>(dec_ctx));
    scope_passes.push_back(std::make_unique<rellic::BooleanSimplify>(dec_ctx));
    scope_passes.push_back(std::make_unique<rellic::NestedCondProp>(dec_ctx));

    scope_passes.push_back(
        std::make_unique<rellic::NestedScopeCombine>(dec_ctx));

    pass_scope.Run();
    
    rellic::CompositeASTPass pass_ec{dec_ctx};
    auto& ec_passes{pass_ec.GetPasses()};
    ec_passes.push_back(std::make_unique<rellic::MaterializeConds>(dec_ctx));
    ec_passes.push_back(std::make_unique<rellic::ExprCombine>(dec_ctx));

    pass_ec.Run();
    
    // Run BooleanSimplify again after MaterializeConds to simplify 1U && expr patterns
    rellic::CompositeASTPass pass_final_simplify{dec_ctx};
    auto& final_simplify_passes{pass_final_simplify.GetPasses()};
    final_simplify_passes.push_back(std::make_unique<rellic::BooleanSimplify>(dec_ctx));
    
    pass_final_simplify.Run();
    
    // FINAL AGGRESSIVE DEAD STATEMENT ELIMINATION
    LOG(INFO) << "Running final aggressive dead statement elimination...";
    rellic::CompositeASTPass pass_final_cleanup{dec_ctx};
    auto& final_cleanup_passes{pass_final_cleanup.GetPasses()};
    final_cleanup_passes.push_back(std::make_unique<rellic::DeadStmtElim>(dec_ctx, true));
    pass_final_cleanup.Run();
    LOG(INFO) << "Final aggressive dead statement elimination completed";

    DecompilationResult result{};
    result.ast = std::move(ast_unit);
    result.module = std::move(module);

    // Copy maps using the helper function
    CopyMap(dec_ctx.stmt_provenance, result.stmt_provenance_map,
            result.value_to_stmt_map);
    CopyMap(dec_ctx.value_decls, result.value_to_decl_map,
            result.decl_provenance_map);
    CopyMap(dec_ctx.type_decls, result.type_to_decl_map,
            result.type_provenance_map);
    CopyMap(dec_ctx.use_provenance, result.expr_use_map, result.use_expr_map);

    // Copy the basic block mappings (no line mapping needed with hash-based naming)
    result.bb_first_stmt_map = dec_ctx.bb_first_stmt_map;
    result.stmt_to_bb = dec_ctx.stmt_to_bb;
    result.bb_is_entry = dec_ctx.bb_is_entry;
    result.bb_to_stmts = dec_ctx.bb_to_stmts;
    result.bb_name_to_llvm_bb = dec_ctx.bb_name_to_llvm_bb;
    result.stmt_to_func = dec_ctx.stmt_to_func;
    result.bb_to_func = dec_ctx.bb_to_func;
    
    // Copy expression positions map element by element
    for (const auto &[expr_id, info] : dec_ctx.expr_positions) {
      ExpressionInfo result_info;
      result_info.type = info.type;
      result_info.llvm_ir = info.llvm_ir;  // Copy LLVM IR directly
      result_info.start_line = info.start_line;
      result_info.start_col = info.start_col;
      result_info.end_line = info.end_line;
      result_info.end_col = info.end_col;
      result.expr_positions[expr_id] = result_info;
    }

    return Result<DecompilationResult, DecompilationError>(std::move(result));
  } catch (Exception& ex) {
    DecompilationError error{};
    error.message = ex.what();
    error.module = std::move(module);
    return Result<DecompilationResult, DecompilationError>(std::move(error));
  }
}
}  // namespace rellic
