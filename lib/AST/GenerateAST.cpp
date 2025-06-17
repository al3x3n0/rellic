/*
 * Copyright (c) 2021-present, Trail of Bits, Inc.
 * All rights reserved.
 *
 * This source code is licensed in accordance with the terms specified in
 * the LICENSE file found in the root directory of this source tree.
 */

#include "rellic/AST/GenerateAST.h"

#include <clang/AST/Expr.h>
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <llvm/ADT/DepthFirstIterator.h>
#include <llvm/ADT/PostOrderIterator.h>
#include <llvm/Analysis/CFG.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/RegionInfo.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <vector>

#include "rellic/AST/ASTBuilder.h"
#include "rellic/AST/Util.h"
#include "rellic/BC/Util.h"
#include "rellic/Exception.h"

namespace rellic {

namespace {

using BBEdge = std::pair<llvm::BasicBlock *, llvm::BasicBlock *>;
using StmtVec = std::vector<clang::Stmt *>;
// using BBGraph =
//     std::unordered_map<llvm::BasicBlock *, std::vector<llvm::BasicBlock *>>;

// static void CFGSlice(llvm::BasicBlock *source, llvm::BasicBlock *sink,
//                      BBGraph &result) {
//   // Clear the output container
//   result.clear();
//   // Adds a path to the result slice BBGraph
//   auto AddPath = [&result](std::vector<llvm::BasicBlock *> &path) {
//     for (unsigned i = 1; i < path.size(); ++i) {
//       result[path[i - 1]].push_back(path[i]);
//       result[path[i]];
//     }
//   };
//   // DFS walk the CFG from `source` to `sink`
//   for (auto it = llvm::df_begin(source); it != llvm::df_end(source); ++it) {
//     for (auto succ : llvm::successors(*it)) {
//       // Construct the path up to this node while
//       // checking if `succ` is already on the path
//       std::vector<llvm::BasicBlock *> path;
//       bool on_path = false;
//       for (unsigned i = 0; i < it.getPathLength(); ++i) {
//         auto node = it.getPath(i);
//         on_path = node == succ;
//         path.push_back(node);
//       }
//       // Check if the path leads to `sink`
//       path.push_back(succ);
//       if (!it.nodeVisited(succ)) {
//         if (succ == sink) {
//           AddPath(path);
//         }
//       } else if (result.count(succ) && !on_path) {
//         AddPath(path);
//       }
//     }
//   }
// }

static bool IsRegionBlock(llvm::Region *region, llvm::BasicBlock *block) {
  return region->getRegionInfo()->getRegionFor(block) == region;
}

static bool IsSubregionEntry(llvm::Region *region, llvm::BasicBlock *block) {
  for (auto &subregion : *region) {
    if (subregion->getEntry() == block) {
      return true;
    }
  }
  return false;
}

// static bool IsSubregionExit(llvm::Region *region, llvm::BasicBlock *block) {
//   for (auto &subregion : *region) {
//     if (subregion->getExit() == block) {
//       return true;
//     }
//   }
//   return false;
// }

static llvm::Region *GetSubregion(llvm::Region *region,
                                  llvm::BasicBlock *block) {
  if (!region->contains(block)) {
    return nullptr;
  } else {
    return region->getSubRegionNode(block);
  }
}

std::string GetRegionNameStr(llvm::Region *region) {
  std::string exit_name;
  std::string entry_name;

  if (region->getEntry()->getName().empty()) {
    llvm::raw_string_ostream os(entry_name);
    region->getEntry()->printAsOperand(os, false);
  } else
    entry_name = region->getEntry()->getName().str();

  if (region->getExit()) {
    if (region->getExit()->getName().empty()) {
      llvm::raw_string_ostream os(exit_name);
      region->getExit()->printAsOperand(os, false);
    } else
      exit_name = region->getExit()->getName().str();
  } else
    exit_name = "<Function Return>";

  return entry_name + " => " + exit_name;
}

}  // namespace

static std::string GetName(llvm::Value *v) {
  std::string s{"h"};
  llvm::raw_string_ostream os(s);
  os.write_hex((unsigned long long)v);
  return s;
}

z3::expr GenerateAST::ToExpr(unsigned idx) {
  if (idx == poison_idx) {
    return dec_ctx.z3_ctx.bool_val(false);
  }
  return dec_ctx.z3_exprs[idx];
}

unsigned GenerateAST::GetOrCreateEdgeForBranch(llvm::BranchInst *inst,
                                               bool cond) {
  if (dec_ctx.z3_br_edges.find({inst, cond}) != dec_ctx.z3_br_edges.end()) {
    return dec_ctx.z3_br_edges[{inst, cond}];
  }

  if (auto constant = llvm::dyn_cast<llvm::ConstantInt>(inst->getCondition())) {
    // This is a conditional branch with a constant condition, so just emit
    // whether the condition matches the wanted value
    auto edge{dec_ctx.z3_ctx.bool_val(constant->isOne() == cond)};
    dec_ctx.z3_br_edges[{inst, cond}] = dec_ctx.InsertZExpr(edge);
    dec_ctx.z3_br_edges_inv[edge.id()] = {inst, true};
  } else if (cond) {
    // This is a conditional branch, so the expression that is true when the
    // branch is going to be taken is just a new variable.
    auto name{GetName(inst)};
    auto edge{dec_ctx.z3_ctx.bool_const(name.c_str())};
    dec_ctx.z3_br_edges[{inst, cond}] = dec_ctx.InsertZExpr(edge);
    dec_ctx.z3_br_edges_inv[edge.id()] = {inst, true};
  } else {
    // Like the previous case, but in this case we want to know the expression
    // that will be true when the branch is not going to be taken
    auto edge{!(ToExpr(GetOrCreateEdgeForBranch(inst, true)))};
    dec_ctx.z3_br_edges[{inst, cond}] = dec_ctx.InsertZExpr(edge);
  }

  return dec_ctx.z3_br_edges[{inst, cond}];
}

unsigned GenerateAST::GetOrCreateVarForSwitch(llvm::SwitchInst *inst) {
  // To aide simplification, switch instructions actually produce numerical
  // variables instead of boolean ones, but are always compared against a
  // constant value.
  if (dec_ctx.z3_sw_vars.find(inst) != dec_ctx.z3_sw_vars.end()) {
    return dec_ctx.z3_sw_vars[inst];
  }

  auto name{GetName(inst)};
  auto var{dec_ctx.z3_ctx.int_const(name.c_str())};
  dec_ctx.z3_sw_vars[inst] = dec_ctx.InsertZExpr(var);
  dec_ctx.z3_sw_vars_inv[var.id()] = inst;
  return dec_ctx.z3_sw_vars[inst];
}

unsigned GenerateAST::GetOrCreateEdgeForSwitch(llvm::SwitchInst *inst,
                                               llvm::ConstantInt *c) {
  if (dec_ctx.z3_sw_edges.find({inst, c}) != dec_ctx.z3_sw_edges.end()) {
    return dec_ctx.z3_sw_edges[{inst, c}];
  }

  unsigned idx;
  if (c) {
    auto sw_case{inst->findCaseValue(c)};
    auto var{ToExpr(GetOrCreateVarForSwitch(inst))};
    auto expr{var == dec_ctx.z3_ctx.int_val(sw_case->getCaseIndex())};

    idx = dec_ctx.InsertZExpr(expr);
  } else {
    // Default case
    z3::expr_vector vec{dec_ctx.z3_ctx};
    for (auto sw_case : inst->cases()) {
      vec.push_back(
          !ToExpr(GetOrCreateEdgeForSwitch(inst, sw_case.getCaseValue())));
    }
    idx = dec_ctx.InsertZExpr(z3::mk_and(vec));
  }
  dec_ctx.z3_sw_edges[{inst, c}] = idx;
  return idx;
}

unsigned GenerateAST::GetOrCreateEdgeCond(llvm::BasicBlock *from,
                                          llvm::BasicBlock *to) {
  if (dec_ctx.z3_edges.find({from, to}) != dec_ctx.z3_edges.end()) {
    return dec_ctx.z3_edges[{from, to}];
  }

  // Construct the edge condition for CFG edge `(from, to)`
  auto result{dec_ctx.z3_ctx.bool_val(true)};
  auto term = from->getTerminator();
  switch (term->getOpcode()) {
    // Conditional branches
    case llvm::Instruction::Br: {
      auto br = llvm::cast<llvm::BranchInst>(term);
      if (br->isConditional()) {
        result =
            ToExpr(GetOrCreateEdgeForBranch(br, to == br->getSuccessor(0)));
      }
    } break;
    // Switches
    case llvm::Instruction::Switch: {
      auto sw{llvm::cast<llvm::SwitchInst>(term)};
      if (to == sw->getDefaultDest()) {
        result = ToExpr(GetOrCreateEdgeForSwitch(sw, nullptr));
      } else {
        z3::expr_vector or_vec{dec_ctx.z3_ctx};
        for (auto sw_case : sw->cases()) {
          if (sw_case.getCaseSuccessor() == to) {
            or_vec.push_back(
                ToExpr(GetOrCreateEdgeForSwitch(sw, sw_case.getCaseValue())));
          }
        }
        result = HeavySimplify(z3::mk_or(or_vec));
      }
    } break;
    // Returns
    case llvm::Instruction::Ret:
      break;
    // Exceptions
    case llvm::Instruction::Invoke:
    case llvm::Instruction::Resume:
    case llvm::Instruction::CatchSwitch:
    case llvm::Instruction::CatchRet:
    case llvm::Instruction::CleanupRet:
      THROW() << "Exception terminator '" << term->getOpcodeName()
              << "' is not supported yet";
      break;
    // Unknown
    default:
      THROW() << "Unsupported terminator instruction: "
              << term->getOpcodeName();
      break;
  }

  dec_ctx.z3_edges[{from, to}] = dec_ctx.InsertZExpr(result.simplify());
  return dec_ctx.z3_edges[{from, to}];
}

unsigned GenerateAST::GetReachingCond(llvm::BasicBlock *block) {
  if (dec_ctx.reaching_conds.find(block) == dec_ctx.reaching_conds.end()) {
    return poison_idx;
  }

  return dec_ctx.reaching_conds[block];
}

void GenerateAST::CreateReachingCond(llvm::BasicBlock *block) {
  auto old_cond_idx{GetReachingCond(block)};
  auto old_cond{ToExpr(old_cond_idx)};
  if (block->hasNPredecessorsOrMore(1)) {
    // Gather reaching conditions from predecessors of the block
    z3::expr_vector conds{dec_ctx.z3_ctx};
    for (auto pred : llvm::predecessors(block)) {
      auto pred_cond{ToExpr(GetReachingCond(pred))};
      auto edge_cond{ToExpr(GetOrCreateEdgeCond(pred, block))};
      // Construct reaching condition from `pred` to `block` as
      // `reach_cond[pred] && edge_cond(pred, block)` or one of
      // the two if the other one is missing.
      auto conj_cond{HeavySimplify(pred_cond && edge_cond)};
      // Append `conj_cond` to reaching conditions of other
      // predecessors via an `||`. Use `conj_cond` if there
      // is no `cond` yet.
      conds.push_back(conj_cond);
    }

    auto cond{HeavySimplify(z3::mk_or(conds))};
    if (old_cond_idx == poison_idx || !Prove(old_cond == cond)) {
      dec_ctx.reaching_conds[block] = dec_ctx.InsertZExpr(cond);
      reaching_conds_changed = true;
    }
  } else if (dec_ctx.reaching_conds.find(block) ==
             dec_ctx.reaching_conds.end()) {
    dec_ctx.reaching_conds[block] =
        dec_ctx.InsertZExpr(dec_ctx.z3_ctx.bool_val(true));
    reaching_conds_changed = true;
  }
}

StmtVec GenerateAST::CreateBasicBlockStmts(llvm::BasicBlock *block) {
  StmtVec result;
  
  // Use a sophisticated naming scheme based on IR characteristics
  std::string block_name;
  if (block->hasName()) {
    block_name = block->getName().str();
  } else if (block_names.count(block)) {
    block_name = block_names[block];
  } else {
    // Generate a unique name based on IR hash and characteristics
    auto func = block->getParent();
    
    // Create a hash based on the block's IR content
    std::string ir_content;
    llvm::raw_string_ostream stream(ir_content);
    block->print(stream);
    stream.flush();
    
    // Simple hash of the IR content
    std::hash<std::string> hasher;
    size_t ir_hash = hasher(ir_content);
    
    // Get block index for additional uniqueness
    unsigned block_idx = 0;
    for (auto &bb : *func) {
      if (&bb == block) break;
      block_idx++;
    }
    
    // Create sophisticated name: function_bb_<index>_<hash_suffix>
    std::string hash_suffix = std::to_string(ir_hash).substr(0, 8);
    block_name = func->getName().str() + "_bb_" + std::to_string(block_idx) + "_" + hash_suffix;
    
    block_names[block] = block_name;
  }

  // No need for line mapping with hash-based naming
  
  // Store the LLVM BB reference and function mapping
  dec_ctx.bb_name_to_llvm_bb[block_name] = block;
  dec_ctx.bb_to_func[block_name] = block->getParent();
  
  // printf("[DEBUG] BB '%s' from function '%s'\n", 
  //        block_name.c_str(), block->getParent()->getName().str().c_str()); fflush(stdout);
  
  // Visit the basic block using IRToASTVisitor
  ast_gen.VisitBasicBlock(*block, result);
  
  // Insert a BB marker at the beginning of the block
  // This will appear as a comment in the output
  if (!result.empty()) {
    auto marker = dec_ctx.ast.CreateCommentMarker(block_name);
    result.insert(result.begin(), marker);
  }

  // Store BB info even if it has no statements
  dec_ctx.bb_is_entry[block_name] = (block == &block->getParent()->getEntryBlock());
  
  if (!result.empty()) {
    if (dec_ctx.bb_first_stmt_map.find(block_name) == dec_ctx.bb_first_stmt_map.end()) {
      dec_ctx.bb_first_stmt_map[block_name] = result.front();
    }
    
    // Map all statements in this BB (preserve order)
    for (auto stmt : result) {
      dec_ctx.stmt_to_bb[stmt] = block_name;
      dec_ctx.bb_to_stmts[block_name].push_back(stmt);
    }
  }
  
  // No need for line tracking with hash-based naming
  
  return result;
}

StmtVec GenerateAST::CreateRegionStmts(llvm::Region *region) {
  StmtVec result;
  for (auto block : rpo_walk) {
    // Check if the block is a subregion entry
    auto subregion = GetSubregion(region, block);
    // Ignore blocks that are neither a subregion or a region block
    if (!subregion && !IsRegionBlock(region, block)) {
      continue;
    }
    // If the block is a head of a subregion, get the compound statement of
    // the subregion otherwise create a new compound and gate it behind a
    // reaching condition.
    clang::CompoundStmt *compound = nullptr;
    StmtVec epi_body;
    if (subregion) {
      CHECK(compound = region_stmts[subregion]);
    } else {
      // Create a compound, wrapping the block
      auto block_body = CreateBasicBlockStmts(block);
      compound = ast.CreateCompoundStmt(block_body);
    }
    // Gate the compound behind a reaching condition
    auto z_expr{GetReachingCond(block)};
    block_stmts[block] = ast.CreateIf(dec_ctx.marker_expr, compound);
    dec_ctx.conds[block_stmts[block]] =
        dec_ctx.InsertZExpr(dec_ctx.z3_exprs[z_expr]);
    // Store the compound
    result.push_back(block_stmts[block]);
  }
  return result;
}

void GenerateAST::RefineLoopSuccessors(llvm::Loop *loop, BBSet &members,
                                       BBSet &successors) {
  // Initialize loop members
  members.insert(loop->block_begin(), loop->block_end());
  // Initialize loop successors
  llvm::SmallVector<llvm::BasicBlock *, 1> exits;
  loop->getExitBlocks(exits);
  successors.insert(exits.begin(), exits.end());
  auto header = loop->getHeader();
  auto region = regions->getRegionFor(header);
  auto exit = region->getExit();
  // Loop membership test
  auto IsLoopMember = [&members](llvm::BasicBlock *block) {
    return members.count(block) > 0;
  };
  // Refinement
  auto new_blocks = successors;
  while (successors.size() > 1 && !new_blocks.empty()) {
    new_blocks.clear();
    for (auto block : BBSet(successors)) {
      if (block == exit) {
        // Don't remove this block from the list of successors if it is the
        // direct exit of the region
        continue;
      }

      // Check if all predecessors of `block` are loop members
      if (std::all_of(llvm::pred_begin(block), llvm::pred_end(block),
                      IsLoopMember)) {
        // Add `block` as a loop member
        members.insert(block);
        // Remove it as a loop successor
        successors.erase(block);
        // Add a successor of `block` to the set of discovered blocks if
        // if it is a region member, if it is NOT a loop member and if
        // the loop header dominates it.
        for (auto succ : llvm::successors(block)) {
          if (IsRegionBlock(region, succ) && !IsLoopMember(succ) &&
              domtree->dominates(header, succ)) {
            new_blocks.insert(succ);
          }
        }
      }
    }
    successors.insert(new_blocks.begin(), new_blocks.end());
  }
}

clang::CompoundStmt *GenerateAST::StructureAcyclicRegion(llvm::Region *region) {
  DLOG(INFO) << "Region " << GetRegionNameStr(region) << " is acyclic";
  auto region_body = CreateRegionStmts(region);
  return ast.CreateCompoundStmt(region_body);
}

clang::CompoundStmt *GenerateAST::StructureCyclicRegion(llvm::Region *region) {
  DLOG(INFO) << "Region " << GetRegionNameStr(region) << " is cyclic";
  auto region_body = CreateRegionStmts(region);
  // Get the loop for which the entry block of the region is a header
  // loops->getLoopFor(region->getEntry())->print(llvm::errs());
  auto loop = region->outermostLoopInRegion(loops, region->getEntry());
  // Only add loop specific control-flow to regions which contain
  // a recognized natural loop. Cyclic regions may only be fragments
  // of a larger loop structure.
  if (!loop) {
    return ast.CreateCompoundStmt(region_body);
  }
  // Refine loop members and successors without invalidating LoopInfo
  BBSet members, successors;
  RefineLoopSuccessors(loop, members, successors);
  // Construct the initial loop body
  StmtVec loop_body;
  for (auto block : rpo_walk) {
    if (members.count(block)) {
      if (IsRegionBlock(region, block) || IsSubregionEntry(region, block)) {
        auto stmt = block_stmts[block];
        auto it = std::find(region_body.begin(), region_body.end(), stmt);
        region_body.erase(it);
        loop_body.push_back(stmt);
      }
    }
  }
  // Get loop exit edges
  std::vector<BBEdge> exits;
  for (auto succ : successors) {
    for (auto pred : llvm::predecessors(succ)) {
      if (members.count(pred)) {
        exits.push_back({pred, succ});
      }
    }
  }
  // Insert `break` statements
  for (auto edge : exits) {
    auto from = edge.first;
    auto to = edge.second;
    // Find the statement corresponding to the exiting block
    auto it = std::find(loop_body.begin(), loop_body.end(), block_stmts[from]);
    CHECK(it != loop_body.end());
    // Create a loop exiting `break` statement
    StmtVec break_stmt({ast.CreateBreak()});
    auto exit_stmt =
        ast.CreateIf(dec_ctx.marker_expr, ast.CreateCompoundStmt(break_stmt));
    // Create edge condition
    dec_ctx.conds[exit_stmt] = dec_ctx.InsertZExpr(
        (ToExpr(GetReachingCond(from)) && ToExpr(GetOrCreateEdgeCond(from, to)))
            .simplify());
    // Insert it after the exiting block statement
    loop_body.insert(std::next(it), exit_stmt);
  }
  // Create the loop statement
  auto loop_stmt =
      ast.CreateWhile(ast.CreateTrue(), ast.CreateCompoundStmt(loop_body));
  // Insert it at the beginning of the region body
  region_body.insert(region_body.begin(), loop_stmt);
  // Structure the rest of the loop body as a acyclic region
  return ast.CreateCompoundStmt(region_body);
}

clang::CompoundStmt *GenerateAST::StructureSwitchRegion(llvm::Region *region) {
  DLOG(INFO) << "Region " << GetRegionNameStr(region)
             << " has a switch instruction";
  // TODO(frabert): find a way to do this in a refinement pass.
  // See "No More Gotos": Condition-aware refinement
  auto body{CreateBasicBlockStmts(region->getEntry())};
  auto sw_inst{
      llvm::cast<llvm::SwitchInst>(region->getEntry()->getTerminator())};
  auto cond{ast_gen.CreateOperandExpr(sw_inst->getOperandUse(0))};
  auto sw_stmt{ast.CreateSwitchStmt(cond)};
  StmtVec sw_body;

  auto default_dest{sw_inst->getDefaultDest()};
  if (default_dest != region->getExit()) {
    auto default_body{CreateBasicBlockStmts(default_dest)};
    sw_body.push_back(
        ast.CreateDefaultStmt(ast.CreateCompoundStmt(default_body)));
    sw_body.push_back(ast.CreateBreak());
  }

  std::vector<llvm::SwitchInst::CaseHandle> cases{sw_inst->case_begin(),
                                                  sw_inst->case_end()};
  for (auto i{0U}; i < cases.size(); ++i) {
    auto sw_case{cases[i]};
    auto successor{sw_case.getCaseSuccessor()};
    if (i + 1 < cases.size() && cases[i + 1].getCaseSuccessor() == successor) {
      continue;
    }

    auto value{ast_gen.CreateConstantExpr(sw_case.getCaseValue())};
    auto case_stmt{ast.CreateCaseStmt(value)};
    if (successor != region->getExit()) {
      auto case_body{CreateBasicBlockStmts(successor)};
      case_stmt->setSubStmt(ast.CreateCompoundStmt(case_body));
      sw_body.push_back(case_stmt);
      sw_body.push_back(ast.CreateBreak());
    } else {
      case_stmt->setSubStmt(ast.CreateBreak());
      sw_body.push_back(case_stmt);
    }
  }
  sw_stmt->setBody(ast.CreateCompoundStmt(sw_body));
  body.push_back(sw_stmt);
  return ast.CreateCompoundStmt(body);
}

clang::CompoundStmt *GenerateAST::StructureRegion(llvm::Region *region) {
  DLOG(INFO) << "Structuring region " << GetRegionNameStr(region);
  auto &region_stmt = region_stmts[region];
  if (region_stmt) {
    LOG(WARNING) << "Asking to re-structure region: "
                 << GetRegionNameStr(region)
                 << "; returning current region instead";
    return region_stmt;
  }
  bool is_cyclic{loops->isLoopHeader(region->getEntry())};
  if (llvm::isa<llvm::SwitchInst>(region->getEntry()->getTerminator()) &&
      !GetSubregion(region, region->getEntry()) && !is_cyclic) {
    region_stmt = StructureSwitchRegion(region);
    return region_stmt;
  }

  // Structure
  region_stmt = is_cyclic ? StructureCyclicRegion(region)
                          : StructureAcyclicRegion(region);
  return region_stmt;
}

llvm::AnalysisKey GenerateAST::Key;

GenerateAST::GenerateAST(DecompilationContext &dec_ctx)
    : dec_ctx(dec_ctx), ast(dec_ctx.ast), ast_gen(dec_ctx) {}

GenerateAST::Result GenerateAST::run(llvm::Module &module,
                                    llvm::ModuleAnalysisManager &MAM) {
  // First process all struct declarations
  ProcessStructs(module);

  // Then process global variables
  for (auto &var : module.globals()) {
    ast_gen.VisitGlobalVar(var);
  }

  // Finally process function declarations
  for (auto &func : module.functions()) {
    ast_gen.VisitFunctionDecl(func);
  }

  return llvm::PreservedAnalyses::all();
}

GenerateAST::Result GenerateAST::run(llvm::Function &func,
                                     llvm::FunctionAnalysisManager &FAM) {
  if (func.isDeclaration()) {
    return llvm::PreservedAnalyses::all();
  }



  // Clear the region statements from previous functions
  region_stmts.clear();
  
  // Get analysis results
  domtree = &FAM.getResult<llvm::DominatorTreeAnalysis>(func);
  regions = &FAM.getResult<llvm::RegionInfoAnalysis>(func);
  loops = &FAM.getResult<llvm::LoopAnalysis>(func);

  // Get blocks in reverse post-order for reaching conditions
  rpo_walk.clear();
  for (auto block : llvm::ReversePostOrderTraversal<llvm::Function *>(&func)) {
    rpo_walk.push_back(block);
  }

  // Process each block once - both for statements and reaching conditions
  std::unordered_map<llvm::BasicBlock*, StmtVec> block_stmts;
  for (auto &block : func) {
    // Generate statements and track line numbers
    block_stmts[&block] = CreateBasicBlockStmts(&block);
    
    // Compute reaching conditions for this block
    CreateReachingCond(&block);
  }

  // Iterate only if reaching conditions need more passes
  while (reaching_conds_changed) {
    reaching_conds_changed = false;
    for (auto block : rpo_walk) {
      CreateReachingCond(block);
    }
  }

  // Recursively walk regions in post-order and structure
  std::function<void(llvm::Region *)> POWalkSubRegions;
  POWalkSubRegions = [&](llvm::Region *region) {
    for (auto &subregion : *region) {
      POWalkSubRegions(&*subregion);
    }
    StructureRegion(region);
  };
  POWalkSubRegions(regions->getTopLevelRegion());

  // Get the function declaration AST node for `func`
  auto fdecl = clang::cast<clang::FunctionDecl>(dec_ctx.value_decls[&func]);
  // Create a redeclaration of `fdecl` that will serve as a definition
  auto tudecl = dec_ctx.ast_ctx.getTranslationUnitDecl();
  auto fdefn =
      ast.CreateFunctionDecl(tudecl, fdecl->getType(), fdecl->getIdentifier());
  fdefn->setPreviousDecl(fdecl);
  dec_ctx.value_decls[&func] = fdefn;
  tudecl->addDecl(fdefn);
  
  // Track where this function definition starts
  dec_ctx.function_start_lines[&func] = dec_ctx.current_line;
  printf("[DEBUG] Processing function body for '%s' at current_line=%u\n", 
         func.getName().str().c_str(), dec_ctx.current_line); fflush(stdout);
  
  // Set parameters to the same as the previous declaration
  fdefn->setParams(fdecl->parameters());
  
  // Create body of the function
  StmtVec fbody;
  // Add declarations of local variables
  for (auto decl : fdecl->decls()) {
    if (clang::isa<clang::VarDecl>(decl)) {
      fbody.push_back(ast.CreateDeclStmt(decl));
    }
  }
  
  // Add statements of the top-level region compound
  for (auto stmt : region_stmts[regions->getTopLevelRegion()]->body()) {
    fbody.push_back(stmt);
  }
  
  // Set body to a new compound
  fdefn->setBody(ast.CreateCompoundStmt(fbody));

  return llvm::PreservedAnalyses::all();
}

void GenerateAST::run(llvm::Module &module, DecompilationContext &dec_ctx) {
  llvm::ModulePassManager mpm;
  llvm::ModuleAnalysisManager mam;
  llvm::PassBuilder pb;
  mam.registerPass([&] { return rellic::GenerateAST(dec_ctx); });
  mpm.addPass(rellic::GenerateAST(dec_ctx));
  pb.registerModuleAnalyses(mam);
  mpm.run(module, mam);

  llvm::FunctionPassManager fpm;
  llvm::FunctionAnalysisManager fam;
  fam.registerPass([&] { return rellic::GenerateAST(dec_ctx); });
  fpm.addPass(rellic::GenerateAST(dec_ctx));
  pb.registerFunctionAnalyses(fam);
  for (auto &func : module.functions()) {
    fpm.run(func, fam);
  }
}

void GenerateAST::ProcessStructs(llvm::Module &module) {
  // Reset line counter since we're starting with structs
  dec_ctx.current_line = 1;

  // First pass: collect all struct types from the module
  std::unordered_set<llvm::StructType*> struct_types;

  // Check global variables
  for (auto &global : module.globals()) {
    if (auto struct_ty = llvm::dyn_cast<llvm::StructType>(global.getValueType())) {
      struct_types.insert(struct_ty);
    }
  }

  // Check functions and their bodies
  for (auto &func : module) {
    // Check function arguments
    for (auto &arg : func.args()) {
      if (auto struct_ty = llvm::dyn_cast<llvm::StructType>(arg.getType())) {
        struct_types.insert(struct_ty);
      }
    }

    // Skip declarations
    if (func.isDeclaration()) {
      continue;
    }

    // Check function body
    for (auto &block : func) {
      for (auto &inst : block) {
        // Check instruction type
        if (auto struct_ty = llvm::dyn_cast<llvm::StructType>(inst.getType())) {
          struct_types.insert(struct_ty);
        }

        // Check operand types
        for (auto &op : inst.operands()) {
          if (auto struct_ty = llvm::dyn_cast<llvm::StructType>(op->getType())) {
            struct_types.insert(struct_ty);
          }
        }
      }
    }
  }

  // Second pass: force declaration and emission of all collected struct types
  auto tudecl = dec_ctx.ast_ctx.getTranslationUnitDecl();
  
  // Sort struct types by name for consistent ordering
  std::vector<llvm::StructType*> sorted_types(struct_types.begin(), struct_types.end());
  std::sort(sorted_types.begin(), sorted_types.end(), 
    [](llvm::StructType* a, llvm::StructType* b) {
      return a->getName() < b->getName();
    });

  // Force struct declarations at the start
  for (auto struct_ty : sorted_types) {
    // Get the struct declaration
    auto qual_type = dec_ctx.GetQualType(struct_ty);
    if (auto tag_type = qual_type->getAs<clang::TagType>()) {
      if (auto decl = tag_type->getDecl()) {
        // Create a new declaration at the translation unit level
        auto new_decl = clang::RecordDecl::Create(
          dec_ctx.ast_ctx, 
          clang::TTK_Struct,
          tudecl,  // New parent context
          decl->getBeginLoc(),
          decl->getLocation(),
          decl->getIdentifier());

        // Copy the fields from the original declaration
        if (auto record_decl = llvm::dyn_cast<clang::RecordDecl>(decl)) {
          for (auto field : record_decl->fields()) {
            auto new_field = clang::FieldDecl::Create(
              dec_ctx.ast_ctx,
              new_decl,
              field->getBeginLoc(),
              field->getLocation(),
              field->getIdentifier(),
              field->getType(),
              field->getTypeSourceInfo(),
              field->getBitWidth(),
              field->isMutable(),
              field->getInClassInitStyle());
            new_decl->addDecl(new_field);
          }
        }

        // Complete the definition if original was complete
        if (auto record_decl = llvm::dyn_cast<clang::RecordDecl>(decl)) {
          if (record_decl->isCompleteDefinition()) {
            new_decl->completeDefinition();
          }
        }

        // Add to translation unit
        tudecl->addDecl(new_decl);
      }
    }
  }
}

}  // namespace rellic
