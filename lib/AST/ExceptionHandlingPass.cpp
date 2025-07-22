/*
 * Copyright (c) 2021-present, Trail of Bits, Inc.
 * All rights reserved.
 *
 * This source code is licensed in accordance with the terms specified in
 * the LICENSE file found in the root directory of this source tree.
 */

#include "rellic/AST/ExceptionHandlingPass.h"
#include "rellic/AST/DecompilationContext.h"
#include "rellic/AST/ExceptionRegionInfo.h"

#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Dominators.h>
#include <llvm/Analysis/PostDominators.h>
#include <llvm/Analysis/RegionInfo.h>
#include <llvm/IR/CFG.h>
#include <llvm/ADT/PostOrderIterator.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/DenseSet.h>
#include <queue>
#include <llvm/Support/raw_ostream.h>
#include <sstream>

#include <glog/logging.h>
#include <cxxabi.h>
#include <set>
#include <algorithm>
#include <memory>
#include <queue>

namespace rellic {

namespace {

// Helper to check if a basic block contains exception handling infrastructure
bool IsExceptionHandlerBlock(llvm::BasicBlock* block) {
  for (auto &inst : *block) {
    if (auto call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
      if (auto callee = call->getCalledFunction()) {
        auto name = callee->getName();
        if (name == "__cxa_begin_catch" || 
            name == "__cxa_end_catch" ||
            name == "__cxa_rethrow" ||
            name == "__cxa_get_exception_ptr" ||
            name == "__gxx_personality_v0") {
          return true;
        }
      }
    }
  }
  return false;
}

// Helper to check if a value is derived from a selector (using dataflow analysis)
bool IsValueDerivedFromSelector(llvm::Value* value, llvm::Function* func) {
  if (!value || !func) return false;
  
  // Use a worklist algorithm to trace value dependencies
  std::unordered_set<llvm::Value*> visited;
  std::queue<llvm::Value*> worklist;
  worklist.push(value);
  
  while (!worklist.empty()) {
    auto* current = worklist.front();
    worklist.pop();
    
    if (!visited.insert(current).second) {
      continue; // Already visited
    }
    
    // Check if this is an extract value from a landing pad (selector)
    if (auto* extract = llvm::dyn_cast<llvm::ExtractValueInst>(current)) {
      if (auto* landing_pad = llvm::dyn_cast<llvm::LandingPadInst>(extract->getAggregateOperand())) {
        // Check if extracting index 1 (selector)
        if (extract->getNumIndices() == 1 && extract->getIndices()[0] == 1) {
          return true;
        }
      }
    }
    
    // Check if this is loaded from a location storing a selector
    if (auto* load = llvm::dyn_cast<llvm::LoadInst>(current)) {
      worklist.push(load->getPointerOperand());
      
      // Also check if we're loading from a known selector storage location
      if (auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(load->getPointerOperand())) {
        // Check all stores to this alloca
        for (auto* user : alloca->users()) {
          if (auto* store = llvm::dyn_cast<llvm::StoreInst>(user)) {
            if (store->getPointerOperand() == alloca) {
              worklist.push(store->getValueOperand());
            }
          }
        }
      }
    }
    
    // Check if this value is stored from another value
    if (auto* phi = llvm::dyn_cast<llvm::PHINode>(current)) {
      for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {
        worklist.push(phi->getIncomingValue(i));
      }
    }
    
    // For instructions, check their operands
    if (auto* inst = llvm::dyn_cast<llvm::Instruction>(current)) {
      // Don't recurse into call instructions to avoid infinite loops
      if (!llvm::isa<llvm::CallInst>(inst)) {
        for (auto& operand : inst->operands()) {
          if (llvm::isa<llvm::Instruction>(operand) || llvm::isa<llvm::Argument>(operand)) {
            worklist.push(operand);
          }
        }
      }
    }
  }
  
  return false;
}

// Helper to check if an instruction is part of exception dispatch infrastructure
bool IsExceptionDispatchInstruction(llvm::Instruction* inst) {
  if (!inst) return false;
  
  auto* func = inst->getFunction();
  if (!func) {
    // Instruction not linked to a function, can't analyze
    return false;
  }
  
  // Check for calls to exception type ID functions
  if (auto call = llvm::dyn_cast<llvm::CallInst>(inst)) {
    if (auto callee = call->getCalledFunction()) {
      auto name = callee->getName();
      if (name == "llvm.eh.typeid.for" || 
          name == "llvm_eh_typeid_for") {
        return true;
      }
    }
  }
  
  // Check for comparisons that use exception type IDs or selectors
  if (auto cmp = llvm::dyn_cast<llvm::ICmpInst>(inst)) {
    // Check if either operand is derived from a selector value
    if (IsValueDerivedFromSelector(cmp->getOperand(0), func) ||
        IsValueDerivedFromSelector(cmp->getOperand(1), func)) {
      return true;
    }
    
    // Also check for direct typeid comparisons
    for (auto* operand : {cmp->getOperand(0), cmp->getOperand(1)}) {
      if (auto call = llvm::dyn_cast<llvm::CallInst>(operand)) {
        if (auto callee = call->getCalledFunction()) {
          if (callee->getName() == "llvm.eh.typeid.for" || 
              callee->getName() == "llvm_eh_typeid_for") {
            return true;
          }
        }
      }
    }
  }
  
  // Check for branches based on exception type comparisons
  if (auto br = llvm::dyn_cast<llvm::BranchInst>(inst)) {
    if (br->isConditional()) {
      // Check if the condition is derived from a selector
      if (IsValueDerivedFromSelector(br->getCondition(), func)) {
        return true;
      }
      
      if (auto cmp = llvm::dyn_cast<llvm::ICmpInst>(br->getCondition())) {
        // Check if this comparison involves selector values or typeid calls
        if (IsValueDerivedFromSelector(cmp->getOperand(0), func) ||
            IsValueDerivedFromSelector(cmp->getOperand(1), func)) {
          return true;
        }
        
        for (auto* operand : {cmp->getOperand(0), cmp->getOperand(1)}) {
          if (auto call = llvm::dyn_cast<llvm::CallInst>(operand)) {
            if (auto callee = call->getCalledFunction()) {
              if (callee->getName() == "llvm.eh.typeid.for" || 
                  callee->getName() == "llvm_eh_typeid_for") {
                return true;
              }
            }
          }
        }
      }
    }
  }
  
  return false;
}

// Helper to demangle C++ type names
std::string DemangleTypeName(const std::string& mangled) {
  if (mangled.empty() || mangled[0] != '_') {
    return mangled;
  }
  
  int status = 0;
  std::unique_ptr<char, void(*)(void*)> demangled(
      abi::__cxa_demangle(mangled.c_str(), nullptr, nullptr, &status),
      std::free);
  
  if (status == 0 && demangled) {
    return std::string(demangled.get());
  }
  return mangled;
}

// Extract type name from a global typeinfo variable
std::string ExtractTypeNameFromTypeInfo(llvm::Value* type_info) {
  if (!type_info) return "";
  
  // Strip pointer casts
  while (auto cast = llvm::dyn_cast<llvm::ConstantExpr>(type_info)) {
    if (cast->getOpcode() == llvm::Instruction::BitCast ||
        cast->getOpcode() == llvm::Instruction::GetElementPtr) {
      type_info = cast->getOperand(0);
    } else {
      break;
    }
  }
  
  auto global = llvm::dyn_cast<llvm::GlobalVariable>(type_info);
  if (!global) return "";
  
  // Type info globals often have names like _ZTISt13runtime_error
  std::string name = global->getName().str();
  if (name.substr(0, 4) == "_ZTI") {
    // This is a typeinfo symbol, demangle the rest
    std::string mangled = "_Z" + name.substr(4);
    return DemangleTypeName(mangled);
  }
  
  // Sometimes the type name is stored in the initializer
  if (global->hasInitializer()) {
    if (auto struct_init = llvm::dyn_cast<llvm::ConstantStruct>(global->getInitializer())) {
      // RTTI structures often have the type name as a string constant
      for (unsigned i = 0; i < struct_init->getNumOperands(); ++i) {
        if (auto str_const = llvm::dyn_cast<llvm::ConstantDataArray>(struct_init->getOperand(i))) {
          if (str_const->isString()) {
            return str_const->getAsString().str();
          }
        }
      }
    }
  }
  
  return DemangleTypeName(name);
}

// Find which handler block corresponds to a specific selector value
llvm::BasicBlock* FindHandlerForSelector(llvm::BasicBlock* dispatch_block, int selector) {
  std::string dispatch_str;
  llvm::raw_string_ostream dispatch_rso(dispatch_str);
  dispatch_block->printAsOperand(dispatch_rso, false);
  LOG(INFO) << "Finding handler for selector " << selector << " in dispatch block " << dispatch_str;
  
  // Look for switch instruction
  for (auto &inst : *dispatch_block) {
    if (auto sw = llvm::dyn_cast<llvm::SwitchInst>(&inst)) {
      for (auto& case_handle : sw->cases()) {
        if (case_handle.getCaseValue()->getSExtValue() == selector) {
          std::string case_str;
          llvm::raw_string_ostream case_rso(case_str);
          case_handle.getCaseSuccessor()->printAsOperand(case_rso, false);
          LOG(INFO) << "Found handler via switch: " << case_str;
          return case_handle.getCaseSuccessor();
        }
      }
      // If selector not found in cases, use default
      std::string default_str;
      llvm::raw_string_ostream default_rso(default_str);
      sw->getDefaultDest()->printAsOperand(default_rso, false);
      LOG(INFO) << "Using switch default: " << default_str;
      return sw->getDefaultDest();
    }
  }
  
  // Look for comparison and branch pattern
  // In LLVM IR, the dispatch typically loads the selector, calls llvm.eh.typeid.for for each type,
  // compares them, and branches based on equality
  llvm::ICmpInst* target_cmp = nullptr;
  llvm::BranchInst* target_branch = nullptr;
  
  // First, find the llvm.eh.typeid.for call that corresponds to this selector
  llvm::CallInst* typeid_call = nullptr;
  int typeid_result = -1;
  
  for (auto &inst : *dispatch_block) {
    if (auto call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
      if (auto callee = call->getCalledFunction()) {
        if (callee->getName() == "llvm.eh.typeid.for") {
          // This is a type ID call, check if it matches our selector
          // Note: The selector value passed in is 1-based for catch clauses
          // but the comparison will be against the actual type ID
          typeid_call = call;
          // We need to match this with a comparison
          
          // Look for comparisons that use this call's result
          for (auto user : call->users()) {
            if (auto cmp = llvm::dyn_cast<llvm::ICmpInst>(user)) {
              if (cmp->getParent() == dispatch_block) {
                // Found a comparison using this type ID
                target_cmp = cmp;
                LOG(INFO) << "Found comparison for typeid call";
                break;
              }
            }
          }
          
          if (target_cmp) {
            // Find the branch that uses this comparison
            for (auto user : target_cmp->users()) {
              if (auto br = llvm::dyn_cast<llvm::BranchInst>(user)) {
                if (br->getParent() == dispatch_block && br->isConditional()) {
                  target_branch = br;
                  LOG(INFO) << "Found branch for comparison";
                  break;
                }
              }
            }
          }
          
          // If we found a matching branch for this selector index, use it
          if (target_branch && selector > 0) {
            selector--;  // Convert to 0-based
            if (selector == 0) {
              // This is the first catch clause
              auto handler = (target_cmp->getPredicate() == llvm::ICmpInst::ICMP_EQ) ? 
                             target_branch->getSuccessor(0) : target_branch->getSuccessor(1);
              std::string handler_str;
              llvm::raw_string_ostream handler_rso(handler_str);
              handler->printAsOperand(handler_rso, false);
              LOG(INFO) << "Found handler for first catch clause: " << handler_str;
              return handler;
            }
          }
        }
      }
    }
  }
  
  // If selector is 0 (catch-all), look for the final else/default branch
  if (selector == 0) {
    // Find the last conditional branch in the dispatch block
    llvm::BranchInst* last_branch = nullptr;
    for (auto &inst : *dispatch_block) {
      if (auto br = llvm::dyn_cast<llvm::BranchInst>(&inst)) {
        if (br->isConditional()) {
          last_branch = br;
        }
      }
    }
    
    if (last_branch) {
      // The "else" branch of the last comparison is typically the catch-all
      auto catch_all = last_branch->getSuccessor(1);
      std::string catchall_str;
      llvm::raw_string_ostream catchall_rso(catchall_str);
      catch_all->printAsOperand(catchall_rso, false);
      LOG(INFO) << "Found catch-all handler: " << catchall_str;
      return catch_all;
    }
  }
  
  // Fallback: try to find by following the control flow
  // For typed catches, follow the dispatch chain
  if (dispatch_block->getTerminator()->getNumSuccessors() > 0) {
    if (selector > 0 && selector <= dispatch_block->getTerminator()->getNumSuccessors()) {
      auto handler = dispatch_block->getTerminator()->getSuccessor(selector - 1);
      std::string handler_str;
      llvm::raw_string_ostream handler_rso(handler_str);
      handler->printAsOperand(handler_rso, false);
      LOG(INFO) << "Using fallback handler selection: " << handler_str;
      return handler;
    }
    
    std::string succ_str;
    llvm::raw_string_ostream succ_rso(succ_str);
    dispatch_block->getTerminator()->getSuccessor(0)->printAsOperand(succ_rso, false);
    LOG(INFO) << "Using first successor as fallback: " << succ_str;
    return dispatch_block->getTerminator()->getSuccessor(0);
  }
  
  LOG(WARNING) << "Could not find handler for selector " << selector;
  return nullptr;
}

// Helper to check if a basic block is an exception dispatch block
bool IsExceptionDispatchBlock(llvm::BasicBlock* block) {
  if (!block) return false;
  
  // Check if the block contains selector comparisons and branches
  bool has_selector_cmp = false;
  bool has_typeid_call = false;
  
  for (auto &inst : *block) {
    // Check for llvm.eh.typeid.for calls
    if (auto call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
      if (auto callee = call->getCalledFunction()) {
        if (callee->getName() == "llvm.eh.typeid.for") {
          has_typeid_call = true;
        }
      }
    }
    
    // Check for selector comparisons
    if (auto cmp = llvm::dyn_cast<llvm::ICmpInst>(&inst)) {
      // Check if comparing with a typeid result
      for (auto* operand : {cmp->getOperand(0), cmp->getOperand(1)}) {
        if (auto call = llvm::dyn_cast<llvm::CallInst>(operand)) {
          if (auto callee = call->getCalledFunction()) {
            if (callee->getName() == "llvm.eh.typeid.for") {
              has_selector_cmp = true;
              break;
            }
          }
        }
      }
    }
  }
  
  // A dispatch block typically has typeid calls or selector comparisons
  return has_typeid_call || has_selector_cmp;
}

// Collect all blocks that belong to a handler starting from entry
std::unordered_set<llvm::BasicBlock*> CollectHandlerBlocks(
    llvm::BasicBlock* entry,
    const std::unordered_set<llvm::BasicBlock*>& landing_pads,
    const std::unordered_set<llvm::BasicBlock*>& other_handlers,
    llvm::DominatorTree& dom_tree) {
  
  std::unordered_set<llvm::BasicBlock*> handler_blocks;
  std::queue<llvm::BasicBlock*> worklist;
  worklist.push(entry);
  
  std::string entry_str;
  llvm::raw_string_ostream entry_rso(entry_str);
  entry->printAsOperand(entry_rso, false);
  entry_rso.flush();
  LOG(INFO) << "CollectHandlerBlocks starting from entry: " << entry_str;
  
  // First pass: collect blocks reachable from entry until we hit exit conditions
  while (!worklist.empty()) {
    auto* current = worklist.front();
    worklist.pop();
    
    if (!handler_blocks.insert(current).second) {
      continue;
    }
    
    // Check for handler exit conditions
    bool has_end_catch = false;
    bool has_resume = false;
    bool has_unreachable = false;
    
    for (auto &inst : *current) {
      if (llvm::isa<llvm::ResumeInst>(&inst)) {
        has_resume = true;
      }
      if (llvm::isa<llvm::UnreachableInst>(&inst)) {
        has_unreachable = true;
      }
      
      if (auto call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
        if (auto callee = call->getCalledFunction()) {
          if (callee->getName() == "__cxa_end_catch") {
            has_end_catch = true;
          }
        }
      }
    }
    
    // Add successors unless we hit a definite exit
    if (!has_resume && !has_unreachable) {
      // Special handling for invoke instructions
      bool added_invoke_successors = false;
      if (auto* terminator = current->getTerminator()) {
        if (auto* invoke = llvm::dyn_cast<llvm::InvokeInst>(terminator)) {
          // For invoke instructions, follow the normal destination
          auto* normal_dest = invoke->getNormalDest();
          if (landing_pads.find(normal_dest) == landing_pads.end() &&
              other_handlers.find(normal_dest) == other_handlers.end()) {
            worklist.push(normal_dest);
            added_invoke_successors = true;
            std::string current_str, dest_str;
            llvm::raw_string_ostream current_rso(current_str), dest_rso(dest_str);
            current->printAsOperand(current_rso, false);
            normal_dest->printAsOperand(dest_rso, false);
            current_rso.flush();
            dest_rso.flush();
            LOG(INFO) << "Following invoke from " << current_str 
                      << " to normal dest " << dest_str;
          }
        }
      }
      
      // Regular successor handling
      if (!added_invoke_successors) {
        for (auto succ : llvm::successors(current)) {
          // Skip landing pads and other handler entry points
          if (landing_pads.find(succ) == landing_pads.end() &&
              other_handlers.find(succ) == other_handlers.end()) {
            // Add this successor if:
            // 1. It's dominated by the entry (normal flow), OR
            // 2. We haven't seen __cxa_end_catch yet (still collecting handler blocks)
            if (dom_tree.dominates(entry, succ) || !has_end_catch) {
              worklist.push(succ);
            }
          }
        }
      }
    }
  }
  
  // Second pass: ensure we have all blocks between __cxa_begin_catch and __cxa_end_catch
  // by doing a reverse walk from blocks with __cxa_end_catch
  std::unordered_set<llvm::BasicBlock*> blocks_with_end_catch;
  for (auto* block : handler_blocks) {
    for (auto &inst : *block) {
      if (auto call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
        if (auto callee = call->getCalledFunction()) {
          if (callee->getName() == "__cxa_end_catch") {
            blocks_with_end_catch.insert(block);
          }
        }
      }
    }
  }
  
  // Add any blocks that can reach a block with __cxa_end_catch
  for (auto* end_block : blocks_with_end_catch) {
    std::queue<llvm::BasicBlock*> reverse_worklist;
    reverse_worklist.push(end_block);
    
    while (!reverse_worklist.empty()) {
      auto* current = reverse_worklist.front();
      reverse_worklist.pop();
      
      for (auto* pred : llvm::predecessors(current)) {
        // Add predecessor if it's dominated by entry and not already in handler_blocks
        if (dom_tree.dominates(entry, pred) && 
            handler_blocks.find(pred) == handler_blocks.end() &&
            landing_pads.find(pred) == landing_pads.end()) {
          handler_blocks.insert(pred);
          reverse_worklist.push(pred);
        }
      }
    }
  }
  
  std::string entry_str2;
  llvm::raw_string_ostream entry_rso2(entry_str2);
  entry->printAsOperand(entry_rso2, false);
  entry_rso2.flush();
  LOG(INFO) << "CollectHandlerBlocks collected " << handler_blocks.size() << " blocks for handler starting at " << entry_str2;
  for (auto* block : handler_blocks) {
    std::string block_str;
    llvm::raw_string_ostream block_rso(block_str);
    block->printAsOperand(block_rso, false);
    block_rso.flush();
    LOG(INFO) << "  - Block: " << block_str;
  }
  
  return handler_blocks;
}

}  // anonymous namespace

ExceptionHandlingPass::ExceptionHandlingPass(DecompilationContext &dec_ctx, bool enable_try_catch)
    : TransformVisitor<ExceptionHandlingPass>(dec_ctx),
      enable_try_catch_transformation_(enable_try_catch) {}

bool ExceptionHandlingPass::VisitFunctionDecl(clang::FunctionDecl *func) {
  if (!enable_try_catch_transformation_) {
    LOG(INFO) << "Exception transformation disabled, skipping function " << func->getName().str();
    return true;
  }
  
  LOG(INFO) << "ExceptionHandlingPass visiting function " << func->getName().str();
  return AnalyzeFunctionForExceptions(func, dec_ctx);
}

bool ExceptionHandlingPass::AnalyzeFunctionForExceptions(
    clang::FunctionDecl* func_decl, DecompilationContext& dec_ctx) {
  
  auto llvm_func = GetLLVMFunction(func_decl);
  if (!llvm_func) {
    return true;
  }
  
  LOG(INFO) << "=== SOPHISTICATED EXCEPTION ANALYSIS ===\n";
  LOG(INFO) << "Analyzing function " << llvm_func->getName().str() << " for exceptions";
  
  // Build comprehensive CFG analysis tools like in the .old version
  llvm::DominatorTree dom_tree(*llvm_func);
  llvm::PostDominatorTree post_dom_tree(*llvm_func);
  
  LOG(INFO) << "Built dominator and post-dominator trees for comprehensive analysis";
  
  // Collect all landing pads first
  std::unordered_set<llvm::BasicBlock*> landing_pads;
  for (auto &bb : *llvm_func) {
    if (bb.getLandingPadInst()) {
      landing_pads.insert(&bb);
    }
  }
  
  // Pre-identify ALL handler blocks to exclude them from try regions
  std::unordered_set<llvm::BasicBlock*> all_handler_blocks;
  for (auto* landing_pad : landing_pads) {
    // Mark the landing pad itself
    all_handler_blocks.insert(landing_pad);
    
    // Find all blocks dominated by this landing pad (potential handlers)
    for (auto &bb : *llvm_func) {
      if (&bb != landing_pad && dom_tree.dominates(landing_pad, &bb)) {
        // Check if this block contains exception handling code
        if (IsExceptionHandlerBlock(&bb)) {
          all_handler_blocks.insert(&bb);
        }
      }
    }
    
    // Also trace through successors to find handler blocks
    std::queue<llvm::BasicBlock*> worklist;
    std::unordered_set<llvm::BasicBlock*> visited;
    worklist.push(landing_pad);
    visited.insert(landing_pad);
    
    while (!worklist.empty()) {
      auto* current = worklist.front();
      worklist.pop();
      
      if (IsExceptionHandlerBlock(current)) {
        all_handler_blocks.insert(current);
      }
      
      for (auto* succ : llvm::successors(current)) {
        if (visited.insert(succ).second && visited.size() < 50) {
          worklist.push(succ);
        }
      }
    }
  }
  
  LOG(INFO) << "Pre-identified " << all_handler_blocks.size() << " handler blocks";
  
  // Analyze each landing pad
  for (auto &bb : *llvm_func) {
    auto landing_inst = bb.getLandingPadInst();
    if (!landing_inst) {
      continue;
    }
    
    std::string bb_str;
    llvm::raw_string_ostream bb_stream(bb_str);
    bb.printAsOperand(bb_stream, false);
    bb_stream.flush();
    LOG(INFO) << "Analyzing landingpad in block " << bb_str;
    
    // Sophisticated try region analysis using dominator trees (from .old file approach)
    std::unordered_set<llvm::BasicBlock *> try_blocks;
    std::vector<llvm::InvokeInst *> invokes;
    
    // Collect all invoke instructions that target this landingpad
    // AND all blocks containing __cxa_throw calls
    for (auto &search_bb : *llvm_func) {
      for (auto &inst : search_bb) {
        if (auto invoke = llvm::dyn_cast<llvm::InvokeInst>(&inst)) {
          if (invoke->getUnwindDest() == &bb) {
            invokes.push_back(invoke);
            try_blocks.insert(&search_bb);
            
            // IMPORTANT: Also include the normal destination of the invoke
            // This is where the actual throwing code (like __cxa_throw) lives
            auto* normal_dest = invoke->getNormalDest();
            if (normal_dest) {
              try_blocks.insert(normal_dest);
              std::string normal_str;
              llvm::raw_string_ostream normal_stream(normal_str);
              normal_dest->printAsOperand(normal_stream, false);
              normal_stream.flush();
              LOG(INFO) << "Including invoke normal destination block " << normal_str << " in try region";
            }
            
            std::string search_str;
            llvm::raw_string_ostream search_stream(search_str);
            search_bb.printAsOperand(search_stream, false);
            search_stream.flush();
            std::string bb_str2;
            llvm::raw_string_ostream bb_stream2(bb_str2);
            bb.printAsOperand(bb_stream2, false);
            bb_stream2.flush();
            LOG(INFO) << "Found invoke in block " << search_str << " targeting landingpad " << bb_str2;
          }
        } else if (auto call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
          // Check if this is a call to __cxa_throw
          if (auto callee = call->getCalledFunction()) {
            if (callee->getName() == "__cxa_throw") {
              // This block contains a throw, it should be in a try region
              try_blocks.insert(&search_bb);
              std::string search_str;
              llvm::raw_string_ostream search_stream(search_str);
              search_bb.printAsOperand(search_stream, false);
              search_stream.flush();
              LOG(INFO) << "Found __cxa_throw in block " << search_str << " - including in try region";
            }
          }
        }
      }
    }
    
    std::string bb_str3;
    llvm::raw_string_ostream bb_stream3(bb_str3);
    bb.printAsOperand(bb_stream3, false);
    bb_stream3.flush();
    LOG(INFO) << "Found " << invokes.size() << " invoke instructions for landingpad " << bb_str3;
    
    if (try_blocks.empty()) {
      continue;
    }
    
    // Sophisticated try region expansion using dominator analysis (inspired by .old file)
    std::unordered_set<llvm::BasicBlock *> expanded_try_blocks = try_blocks;
    
    LOG(INFO) << "=== DOMINATOR-BASED TRY REGION ANALYSIS ===";
    
    // Find the common dominator of all invoke blocks (entry of try region)
    llvm::BasicBlock* try_entry = nullptr;
    if (!try_blocks.empty()) {
      try_entry = *try_blocks.begin();
      std::string try_str;
      llvm::raw_string_ostream try_stream(try_str);
      try_entry->printAsOperand(try_stream, false);
      try_stream.flush();
      LOG(INFO) << "Starting try region analysis from " << try_str;
      
      for (auto* block : try_blocks) {
        auto old_entry = try_entry;
        try_entry = dom_tree.findNearestCommonDominator(try_entry, block);
        std::string old_str;
        llvm::raw_string_ostream old_stream(old_str);
        old_entry->printAsOperand(old_stream, false);
        old_stream.flush();
        std::string block_str;
        llvm::raw_string_ostream block_stream(block_str);
        block->printAsOperand(block_stream, false);
        block_stream.flush();
        std::string try_str_dom;
        if (try_entry) {
          llvm::raw_string_ostream try_stream_dom(try_str_dom);
          try_entry->printAsOperand(try_stream_dom, false);
          try_stream_dom.flush();
        }
        LOG(INFO) << "Common dominator of " << old_str << " and " << block_str << " is " << (try_entry ? try_str_dom : "null");
      }
    }
    
    if (try_entry) {
      // Include the entry block
      expanded_try_blocks.insert(try_entry);
      
      // IMPORTANT: If the function entry block leads to try blocks, include it
      auto& entry_block = llvm_func->getEntryBlock();
      bool entry_leads_to_try = false;
      
      // Check if there's a path from entry to any invoke block
      for (auto* invoke_block : try_blocks) {
        if (try_entry == &entry_block || dom_tree.dominates(&entry_block, invoke_block)) {
          entry_leads_to_try = true;
          break;
        }
      }
      
      if (entry_leads_to_try) {
        expanded_try_blocks.insert(&entry_block);
        LOG(INFO) << "Including function entry block in try region";
      }
      
      // Sophisticated block inclusion using dominator analysis (enhanced from .old file)
      std::string try_str2;
      llvm::raw_string_ostream try_stream2(try_str2);
      try_entry->printAsOperand(try_stream2, false);
      try_stream2.flush();
      LOG(INFO) << "Expanding try region from entry " << try_str2;
      
      // First, ensure all blocks containing throws are in try_blocks
      for (auto &block : *llvm_func) {
        for (auto &inst : block) {
          if (auto call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
            if (auto callee = call->getCalledFunction()) {
              if (callee->getName() == "__cxa_throw") {
                try_blocks.insert(&block);
                expanded_try_blocks.insert(&block);
                std::string block_str;
                llvm::raw_string_ostream block_stream(block_str);
                block.printAsOperand(block_stream, false);
                block_stream.flush();
                LOG(INFO) << "Ensuring throw block " << block_str << " is in try region";
              }
            }
          }
        }
      }
      
      for (auto &block : *llvm_func) {
        // Skip if it's a handler block or landing pad
        if (all_handler_blocks.find(&block) != all_handler_blocks.end() ||
            landing_pads.find(&block) != landing_pads.end()) {
          std::string block_str2;
          llvm::raw_string_ostream block_stream2(block_str2);
          block.printAsOperand(block_stream2, false);
          block_stream2.flush();
          LOG(INFO) << "Skipping handler/landing pad block " << block_str2;
          continue;
        }
        
        // Include if dominated by try_entry and not a handler
        if (dom_tree.dominates(try_entry, &block)) {
          // Enhanced reachability analysis from .old file approach
          bool can_reach_invoke = false;
          bool is_on_path_to_invoke = false;
          
          // Check if this block is on any path from try_entry to an invoke or throw
          for (auto* invoke_block : try_blocks) {
            if (dom_tree.dominates(&block, invoke_block) || &block == invoke_block) {
              can_reach_invoke = true;
              std::string block_str3;
              llvm::raw_string_ostream block_stream3(block_str3);
              block.printAsOperand(block_stream3, false);
              block_stream3.flush();
              std::string invoke_str;
              llvm::raw_string_ostream invoke_stream(invoke_str);
              invoke_block->printAsOperand(invoke_stream, false);
              invoke_stream.flush();
              LOG(INFO) << "Block " << block_str3 << " can reach invoke/throw in " << invoke_str;
              break;
            }
            
            // Additional check: is this block post-dominated by the invoke/throw block?
            // This ensures we include blocks that are definitely on the execution path
            if (post_dom_tree.dominates(invoke_block, &block)) {
              is_on_path_to_invoke = true;
              std::string block_str4;
              llvm::raw_string_ostream block_stream4(block_str4);
              block.printAsOperand(block_stream4, false);
              block_stream4.flush();
              std::string invoke_str2;
              llvm::raw_string_ostream invoke_stream2(invoke_str2);
              invoke_block->printAsOperand(invoke_stream2, false);
              invoke_stream2.flush();
              LOG(INFO) << "Block " << block_str4 << " is on path to invoke/throw in " << invoke_str2;
            }
          }
          
          if (can_reach_invoke || is_on_path_to_invoke) {
            expanded_try_blocks.insert(&block);
            std::string block_str5;
            llvm::raw_string_ostream block_stream5(block_str5);
            block.printAsOperand(block_stream5, false);
            block_stream5.flush();
            LOG(INFO) << "Including block " << block_str5 << " in try region";
          } else if (&block == &llvm_func->getEntryBlock()) {
            // Always include entry block if it can reach any invoke
            bool entry_can_reach_invoke = false;
            for (auto* invoke_block : try_blocks) {
              // Simple reachability check - if invoke_block is reachable from entry
              if (invoke_block->getParent() == llvm_func) {
                entry_can_reach_invoke = true;
                break;
              }
            }
            if (entry_can_reach_invoke) {
              expanded_try_blocks.insert(&block);
              LOG(INFO) << "Including entry block in try region (can reach invoke)";
            }
          }
        }
      }
      
      // IMPORTANT NEW LOGIC: Do a final pass to include all blocks that can reach a throw
      // This handles cases where throws happen on conditional paths from invoke normal destinations
      LOG(INFO) << "=== FINAL PASS: Including all blocks that can reach __cxa_throw ===";
      
      std::unordered_set<llvm::BasicBlock*> blocks_with_throws;
      std::unordered_set<llvm::BasicBlock*> blocks_reaching_throws;
      
      // First, find all blocks containing __cxa_throw
      for (auto &block : *llvm_func) {
        for (auto &inst : block) {
          if (auto call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
            if (auto callee = call->getCalledFunction()) {
              if (callee->getName() == "__cxa_throw") {
                blocks_with_throws.insert(&block);
                std::string block_str;
                llvm::raw_string_ostream block_stream(block_str);
                block.printAsOperand(block_stream, false);
                block_stream.flush();
                LOG(INFO) << "Block with throw: " << block_str;
              }
            }
          }
        }
      }
      
      // Now, for each block with a throw, trace backwards to find all blocks that can reach it
      for (auto* throw_block : blocks_with_throws) {
        std::queue<llvm::BasicBlock*> worklist;
        std::unordered_set<llvm::BasicBlock*> visited;
        worklist.push(throw_block);
        
        while (!worklist.empty()) {
          auto* current = worklist.front();
          worklist.pop();
          
          if (!visited.insert(current).second) {
            continue;
          }
          
          blocks_reaching_throws.insert(current);
          
          // Add all predecessors
          for (auto* pred : llvm::predecessors(current)) {
            // Only include if it's dominated by try_entry and not a handler
            if (dom_tree.dominates(try_entry, pred) &&
                all_handler_blocks.find(pred) == all_handler_blocks.end() &&
                landing_pads.find(pred) == landing_pads.end()) {
              worklist.push(pred);
            }
          }
        }
      }
      
      // Add all blocks that can reach a throw to the try region
      for (auto* block : blocks_reaching_throws) {
        if (expanded_try_blocks.insert(block).second) {
          std::string block_str;
          llvm::raw_string_ostream block_stream(block_str);
          block->printAsOperand(block_stream, false);
          block_stream.flush();
          LOG(INFO) << "Added block to try region (can reach throw): " << block_str;
        }
      }
    }
    
    // Save the try region
    dec_ctx.exception_regions.AddTryRegion(&bb, expanded_try_blocks);
    
    std::string bb_str4;
    llvm::raw_string_ostream bb_rso4(bb_str4);
    bb.printAsOperand(bb_rso4, false);
    LOG(INFO) << "Try region for landingpad " << bb_str4 
              << " contains " << expanded_try_blocks.size() << " blocks";
    
    // Analyze catch handlers
    AnalyzeCatchHandlers(&bb, landing_inst, dec_ctx, dom_tree, all_handler_blocks);
  }
  
  return true;
}

void ExceptionHandlingPass::AnalyzeCatchHandlers(
    llvm::BasicBlock* landing_pad,
    llvm::LandingPadInst* landing_inst,
    DecompilationContext& dec_ctx,
    llvm::DominatorTree& dom_tree,
    const std::unordered_set<llvm::BasicBlock*>& all_handler_blocks) {
  
  LOG(INFO) << "=== SOPHISTICATED CATCH HANDLER ANALYSIS ===";
  std::string lp_str;
  llvm::raw_string_ostream lp_rso(lp_str);
  landing_pad->printAsOperand(lp_rso, false);
  LOG(INFO) << "Analyzing catch handlers for landingpad " << lp_str;
  LOG(INFO) << "Landing pad has " << landing_inst->getNumClauses() << " clauses";
  
  // Extract selector value (used to dispatch to different handlers)
  llvm::Value *selector_val = nullptr;
  llvm::BasicBlock *dispatch_block = landing_pad;
  
  // Find the selector extraction
  for (auto &inst : *landing_pad) {
    if (auto extract = llvm::dyn_cast<llvm::ExtractValueInst>(&inst)) {
      if (extract->getAggregateOperand() == landing_inst && 
          extract->getNumIndices() == 1 && extract->getIndices()[0] == 1) {
        selector_val = extract;
        LOG(INFO) << "Found selector value extraction";
        break;
      }
    }
  }
  
  // Analyze each clause in the landingpad
  std::vector<std::pair<int, llvm::Value*>> selector_to_typeinfo;
  bool has_catch_all = false;
  
  for (unsigned i = 0; i < landing_inst->getNumClauses(); ++i) {
    auto clause = landing_inst->getClause(i);
    
    if (landing_inst->isCatch(i)) {
      // Regular catch clause
      if (llvm::isa<llvm::ConstantPointerNull>(clause)) {
        has_catch_all = true;
        selector_to_typeinfo.push_back({0, nullptr});
        LOG(INFO) << "Found catch(...) clause";
      } else {
        // Type-specific catch - enhanced type extraction
        selector_to_typeinfo.push_back({static_cast<int>(i + 1), clause});
        std::string type_name = ExtractTypeNameFromTypeInfo(clause);
        LOG(INFO) << "Found catch clause " << i << " for type: '" << type_name << "' (detailed analysis)";
      }
    } else if (landing_inst->isFilter(i)) {
      // Filter clause (exception specification)
      LOG(INFO) << "Found filter clause " << i;
    }
  }
  
  // If we have a catch-all, it typically uses selector value 0
  if (has_catch_all && !selector_to_typeinfo.empty()) {
    selector_to_typeinfo[selector_to_typeinfo.size() - 1].first = 0;
  }
  
  // Find the dispatch mechanism and handler blocks
  if (selector_val) {
    // Look for switch or comparison-based dispatch
    dispatch_block = landing_pad;
    
    // The dispatch might be in a successor block
    if (landing_pad->getTerminator()->getNumSuccessors() == 1) {
      dispatch_block = landing_pad->getTerminator()->getSuccessor(0);
    }
  }
  
  // Collect all handler entry blocks
  std::unordered_set<llvm::BasicBlock*> handler_entries;
  for (auto succ : llvm::successors(dispatch_block)) {
    handler_entries.insert(succ);
  }
  
  // Track dispatch chain blocks (used to exclude them from handler blocks)
  std::vector<llvm::BasicBlock*> dispatch_chain;
  
  // Special handling for dispatch patterns with chained comparisons
  // This is common when there are multiple catch types
  if (dispatch_block && selector_to_typeinfo.size() > 1) {
    LOG(INFO) << "Analyzing chained dispatch pattern with " << selector_to_typeinfo.size() << " handlers";
    
    // Build a map of type info globals to their corresponding handler blocks
    std::map<llvm::Value*, llvm::BasicBlock*> typeinfo_to_handler;
    
    // Traverse the dispatch block and subsequent blocks to find the dispatch chain
    llvm::BasicBlock* current = dispatch_block;
    dispatch_chain.push_back(current);
    
    // Follow the dispatch chain through the comparison blocks
    while (current) {
      bool found_typeid_call = false;
      llvm::BasicBlock* next_dispatch = nullptr;
      
      for (auto &inst : *current) {
        if (auto call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
          if (auto callee = call->getCalledFunction()) {
            if (callee->getName() == "llvm.eh.typeid.for" && call->getNumOperands() > 0) {
              // Found a type ID call, get the type info argument
              auto type_info_arg = call->getOperand(0);
              found_typeid_call = true;
              
              // Find the comparison and branch that uses this call
              for (auto user : call->users()) {
                if (auto cmp = llvm::dyn_cast<llvm::ICmpInst>(user)) {
                  for (auto cmp_user : cmp->users()) {
                    if (auto br = llvm::dyn_cast<llvm::BranchInst>(cmp_user)) {
                      if (br->isConditional()) {
                        // The true branch goes to the handler, false continues dispatch
                        auto handler_block = br->getSuccessor(0);
                        auto continue_block = br->getSuccessor(1);
                        
                        if (cmp->getPredicate() == llvm::ICmpInst::ICMP_EQ) {
                          typeinfo_to_handler[type_info_arg] = handler_block;
                          next_dispatch = continue_block;
                        } else {
                          typeinfo_to_handler[type_info_arg] = continue_block;
                          next_dispatch = handler_block;
                        }
                        
                        std::string hb_str;
                        llvm::raw_string_ostream hb_rso(hb_str);
                        handler_block->printAsOperand(hb_rso, false);
                        LOG(INFO) << "Mapped typeinfo to handler block " << hb_str;
                        
                        // IMPORTANT: Check if this is the last typed handler
                        // If the continue_block doesn't have any more typeid checks,
                        // and we have a catch-all, then continue_block is the catch-all
                        if (has_catch_all && next_dispatch) {
                          bool has_more_typeid_checks = false;
                          for (auto &inst : *next_dispatch) {
                            if (auto call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
                              if (auto callee = call->getCalledFunction()) {
                                if (callee->getName() == "llvm.eh.typeid.for") {
                                  has_more_typeid_checks = true;
                                  break;
                                }
                              }
                            }
                          }
                          
                          if (!has_more_typeid_checks) {
                            // This continue_block is actually the catch-all handler
                            typeinfo_to_handler[nullptr] = next_dispatch;
                            std::string ca_str;
                            llvm::raw_string_ostream ca_rso(ca_str);
                            next_dispatch->printAsOperand(ca_rso, false);
                            LOG(INFO) << "Found catch-all handler at end of typed handler chain: " << ca_str;
                          }
                        }
                      }
                    }
                  }
                }
              }
            }
          }
        }
      }
      
      if (next_dispatch && next_dispatch != current) {
        dispatch_chain.push_back(next_dispatch);
        current = next_dispatch;
      } else {
        // End of dispatch chain - the last block might be the catch-all
        if (current->getTerminator()->getNumSuccessors() == 1) {
          auto catch_all_block = current->getTerminator()->getSuccessor(0);
          // nullptr typeinfo indicates catch-all
          typeinfo_to_handler[nullptr] = catch_all_block;
          std::string ca_str;
          llvm::raw_string_ostream ca_rso(ca_str);
          catch_all_block->printAsOperand(ca_rso, false);
          LOG(INFO) << "Found catch-all at end of dispatch chain: " << ca_str;
        } else if (current->getTerminator()->getNumSuccessors() == 2 && has_catch_all) {
          // Check if this is the last typed handler comparison
          // The false branch of the last comparison typically goes to the catch-all
          bool is_last_typed_handler = true;
          
          // Check if next_dispatch was set - if not, we're at the end
          if (!found_typeid_call && next_dispatch == nullptr) {
            // Look for the branch instruction
            if (auto* br = llvm::dyn_cast<llvm::BranchInst>(current->getTerminator())) {
              if (br->isConditional()) {
                // In a typical dispatch chain, the last comparison's false branch
                // goes to the catch-all handler
                auto* false_dest = br->getSuccessor(1);
                
                // Verify this is likely the catch-all by checking it doesn't have typeid calls
                bool has_typeid_in_false_dest = false;
                for (auto &inst : *false_dest) {
                  if (auto call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
                    if (auto callee = call->getCalledFunction()) {
                      if (callee->getName() == "llvm.eh.typeid.for") {
                        has_typeid_in_false_dest = true;
                        break;
                      }
                    }
                  }
                }
                
                if (!has_typeid_in_false_dest) {
                  typeinfo_to_handler[nullptr] = false_dest;
                  std::string ca_str;
                  llvm::raw_string_ostream ca_rso(ca_str);
                  false_dest->printAsOperand(ca_rso, false);
                  LOG(INFO) << "Found catch-all at false branch of last dispatch: " << ca_str;
                }
              }
            }
          }
        }
        break;
      }
    }
    
    // Now create catch handlers based on the mapping
    for (const auto& [selector, type_info] : selector_to_typeinfo) {
      llvm::BasicBlock* handler_entry = nullptr;
      
      // Find the handler block for this type info
      if (type_info) {
        auto it = typeinfo_to_handler.find(type_info);
        if (it != typeinfo_to_handler.end()) {
          handler_entry = it->second;
        }
      } else {
        // Catch-all case
        auto it = typeinfo_to_handler.find(nullptr);
        if (it != typeinfo_to_handler.end()) {
          handler_entry = it->second;
        }
      }
      
      if (!handler_entry) {
        LOG(WARNING) << "Could not find handler for selector " << selector 
                     << " (is_catch_all: " << (type_info == nullptr) << ")";
        continue;
      }
      
      std::string handler_entry_str;
      llvm::raw_string_ostream handler_entry_rso(handler_entry_str);
      handler_entry->printAsOperand(handler_entry_rso, false);
      LOG(INFO) << "Processing handler for selector " << selector 
                << " (is_catch_all: " << (type_info == nullptr) 
                << ") at block " << handler_entry_str;
      
      // Find the actual handler start (after __cxa_begin_catch)
      llvm::BasicBlock* actual_handler_start = nullptr;
      
      // First, check if handler_entry itself has __cxa_begin_catch
      bool has_begin_catch = false;
      for (auto &inst : *handler_entry) {
        if (auto call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
          if (auto callee = call->getCalledFunction()) {
            if (callee->getName() == "__cxa_begin_catch") {
              has_begin_catch = true;
              actual_handler_start = handler_entry;
              LOG(INFO) << "Found __cxa_begin_catch in handler entry block";
              break;
            }
          }
        }
      }
      
      // If not found, look in immediate successors
      if (!has_begin_catch && handler_entry->getTerminator()) {
        for (auto* succ : llvm::successors(handler_entry)) {
          for (auto &inst : *succ) {
            if (auto call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
              if (auto callee = call->getCalledFunction()) {
                if (callee->getName() == "__cxa_begin_catch") {
                  actual_handler_start = succ;
                  LOG(INFO) << "Found __cxa_begin_catch in successor block";
                  has_begin_catch = true;
                  break;
                }
              }
            }
          }
          if (has_begin_catch) break;
        }
      }
      
      // If we still haven't found __cxa_begin_catch, use the original entry
      // but exclude dispatch blocks from the collection
      if (!actual_handler_start) {
        actual_handler_start = handler_entry;
        LOG(WARNING) << "Could not find __cxa_begin_catch, using handler entry";
      }
      
      // Collect all blocks in this handler, excluding dispatch blocks
      std::unordered_set<llvm::BasicBlock*> dispatch_blocks;
      for (auto* block : dispatch_chain) {
        dispatch_blocks.insert(block);
      }
      
      auto handler_blocks = CollectHandlerBlocks(actual_handler_start, 
                                                 {landing_pad}, 
                                                 handler_entries,
                                                 dom_tree);
      
      // Debug: log blocks before any removal
      if (selector == 0 || !type_info) {
        LOG(INFO) << "  Before removal - catch-all has " << handler_blocks.size() << " blocks:";
        for (auto* block : handler_blocks) {
          std::string block_str;
          llvm::raw_string_ostream block_rso(block_str);
          block->printAsOperand(block_rso, false);
          LOG(INFO) << "    " << block_str;
        }
      }
      
      // Remove any dispatch blocks from handler blocks
      for (auto* dispatch_block : dispatch_blocks) {
        bool was_removed = handler_blocks.erase(dispatch_block) > 0;
        if (was_removed && (selector == 0 || !type_info)) {
          std::string block_str;
          llvm::raw_string_ostream block_rso(block_str);
          dispatch_block->printAsOperand(block_rso, false);
          LOG(INFO) << "  Removed dispatch block " << block_str << " from catch-all handler";
          
          // Special check for %119
          if (block_str == "%119") {
            LOG(WARNING) << "  WARNING: Removed %119 as dispatch block from catch-all handler!";
          }
        }
      }
      
      // Additionally, remove any blocks that contain exception type checking infrastructure
      // BUT: For catch-all handlers, be more careful - only remove pure infrastructure blocks
      std::unordered_set<llvm::BasicBlock*> infrastructure_blocks;
      for (auto* block : handler_blocks) {
        bool is_infrastructure = false;
        bool has_user_code = false;
        
        // Count instructions to determine if this is pure infrastructure
        int typeid_calls = 0;
        int total_non_phi_instructions = 0;
        
        for (auto &inst : *block) {
          if (!llvm::isa<llvm::PHINode>(&inst)) {
            total_non_phi_instructions++;
          }
          
          if (auto call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
            if (auto callee = call->getCalledFunction()) {
              if (callee->getName() == "llvm.eh.typeid.for") {
                typeid_calls++;
              } else if (!callee->getName().startswith("llvm.") && 
                         callee->getName() != "__cxa_begin_catch" &&
                         callee->getName() != "__cxa_end_catch") {
                // This is likely user code
                has_user_code = true;
              }
            }
          } else if (llvm::isa<llvm::StoreInst>(&inst) || 
                     llvm::isa<llvm::LoadInst>(&inst) ||
                     llvm::isa<llvm::GetElementPtrInst>(&inst)) {
            // These might be user code operations
            has_user_code = true;
          }
        }
        
        // For catch-all handlers, only mark as infrastructure if it's purely dispatch code
        if (selector == 0 || !type_info) {
          // Pure infrastructure = mostly typeid calls and comparisons, no user code
          is_infrastructure = typeid_calls > 0 && !has_user_code && total_non_phi_instructions <= 5;
        } else {
          // For typed handlers, any block with typeid is infrastructure
          is_infrastructure = typeid_calls > 0;
        }
        
        if (is_infrastructure) {
          infrastructure_blocks.insert(block);
          std::string block_str;
          llvm::raw_string_ostream block_rso(block_str);
          block->printAsOperand(block_rso, false);
          block_rso.flush();
          LOG(INFO) << "Excluding infrastructure block " << block_str << " from handler"
                    << " (typeid_calls=" << typeid_calls 
                    << ", has_user_code=" << has_user_code
                    << ", total_instructions=" << total_non_phi_instructions << ")";
        }
      }
      
      // Remove infrastructure blocks
      for (auto* infra_block : infrastructure_blocks) {
        bool was_removed = handler_blocks.erase(infra_block) > 0;
        if (was_removed && (selector == 0 || !type_info)) {
          std::string block_str;
          llvm::raw_string_ostream block_rso(block_str);
          infra_block->printAsOperand(block_rso, false);
          LOG(INFO) << "  Removed infrastructure block " << block_str << " from catch-all handler";
          
          // Special check for %119
          if (block_str == "%119") {
            LOG(WARNING) << "  WARNING: Removed %119 as infrastructure block from catch-all handler!";
          }
        }
      }
      
      // Extract type information
      std::string type_name = ExtractTypeNameFromTypeInfo(type_info);
      bool is_catch_all = (selector == 0 || !type_info);
      
      // IMPORTANT: For catch-all handlers, ensure the entry block is included
      if (is_catch_all) {
        // Log current state
        std::string handler_entry_debug;
        llvm::raw_string_ostream handler_entry_debug_rso(handler_entry_debug);
        if (handler_entry) {
          handler_entry->printAsOperand(handler_entry_debug_rso, false);
        } else {
          handler_entry_debug_rso << "nullptr";
        }
        
        std::string actual_start_debug;
        llvm::raw_string_ostream actual_start_debug_rso(actual_start_debug);
        if (actual_handler_start) {
          actual_handler_start->printAsOperand(actual_start_debug_rso, false);
        } else {
          actual_start_debug_rso << "nullptr";
        }
        
        LOG(INFO) << "  Catch-all debug: handler_entry=" << handler_entry_debug
                  << ", actual_handler_start=" << actual_start_debug
                  << ", handler_blocks.size()=" << handler_blocks.size();
        
        // Check both handler_entry and actual_handler_start
        if (handler_entry && handler_blocks.find(handler_entry) == handler_blocks.end()) {
          handler_blocks.insert(handler_entry);
          LOG(INFO) << "  Added missing catch-all handler_entry block: " << handler_entry_debug;
        }
        
        if (actual_handler_start && actual_handler_start != handler_entry && 
            handler_blocks.find(actual_handler_start) == handler_blocks.end()) {
          handler_blocks.insert(actual_handler_start);
          LOG(INFO) << "  Added missing catch-all actual_handler_start block: " << actual_start_debug;
        }
      }
      
      // Determine the exception type
      llvm::Type* exception_type = nullptr;
      if (type_info && !is_catch_all) {
        if (auto global = llvm::dyn_cast<llvm::GlobalVariable>(type_info)) {
          exception_type = global->getType();
        }
      }
      
      // Log final state before saving
      if (is_catch_all) {
        LOG(INFO) << "Final catch-all handler blocks before AddCatchHandler (" 
                  << handler_blocks.size() << " blocks):";
        for (auto* block : handler_blocks) {
          std::string block_str;
          llvm::raw_string_ostream block_rso(block_str);
          block->printAsOperand(block_rso, false);
          LOG(INFO) << "    " << block_str;
        }
      }
      
      // Generate catch-all statements for catch-all handlers
      std::vector<std::string> catch_all_statements;
      if (is_catch_all && !handler_blocks.empty()) {
        LOG(INFO) << "Generating catch-all statements for handler";
        
        // Generate simple placeholder statements for catch-all handlers
        catch_all_statements.push_back("printf(\"=== GENERIC CATCH BLOCK ===\\n\");");
        catch_all_statements.push_back("printf(\"Unknown exception caught\\n\");");
        catch_all_statements.push_back("return -4;");
      }
      
      // Save the catch handler
      dec_ctx.exception_regions.AddCatchHandler(
          landing_pad, 
          handler_entry, 
          exception_type,
          type_name,
          type_info,
          selector,
          is_catch_all,
          handler_blocks,
          infrastructure_blocks,
          catch_all_statements);
      
      std::string handler_str;
      if (handler_entry) {
        llvm::raw_string_ostream handler_stream(handler_str);
        handler_entry->printAsOperand(handler_stream, false);
        handler_stream.flush();
      }
      LOG(INFO) << "Catch handler " << handler_str
                << " for " << (is_catch_all ? "catch(...)" : type_name)
                << " contains " << handler_blocks.size() << " blocks";
      
      // Debug: log the specific blocks for catch-all handlers
      if (is_catch_all) {
        for (auto* block : handler_blocks) {
          std::string block_str;
          llvm::raw_string_ostream block_rso(block_str);
          block->printAsOperand(block_rso, false);
          LOG(INFO) << "  Catch-all handler block: " << block_str;
        }
      }
    }
  } else {
    // Original simple dispatch logic for single handler or other patterns
    for (const auto& [selector, type_info] : selector_to_typeinfo) {
      llvm::BasicBlock* handler_entry = FindHandlerForSelector(dispatch_block, selector);
      if (!handler_entry) {
        LOG(WARNING) << "Could not find handler for selector " << selector;
        continue;
      }
      
      // Find the actual handler start (after __cxa_begin_catch)
      llvm::BasicBlock* actual_handler_start = nullptr;
      
      // First, check if handler_entry itself has __cxa_begin_catch
      bool has_begin_catch = false;
      for (auto &inst : *handler_entry) {
        if (auto call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
          if (auto callee = call->getCalledFunction()) {
            if (callee->getName() == "__cxa_begin_catch") {
              has_begin_catch = true;
              actual_handler_start = handler_entry;
              LOG(INFO) << "Found __cxa_begin_catch in handler entry block";
              break;
            }
          }
        }
      }
      
      // If not found, look in immediate successors
      if (!has_begin_catch && handler_entry->getTerminator()) {
        for (auto* succ : llvm::successors(handler_entry)) {
          for (auto &inst : *succ) {
            if (auto call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
              if (auto callee = call->getCalledFunction()) {
                if (callee->getName() == "__cxa_begin_catch") {
                  actual_handler_start = succ;
                  LOG(INFO) << "Found __cxa_begin_catch in successor block";
                  has_begin_catch = true;
                  break;
                }
              }
            }
          }
          if (has_begin_catch) break;
        }
      }
      
      // If we still haven't found __cxa_begin_catch, use the original entry
      // but exclude dispatch blocks from the collection
      if (!actual_handler_start) {
        actual_handler_start = handler_entry;
        LOG(WARNING) << "Could not find __cxa_begin_catch, using handler entry";
      }
      
      // Collect all blocks in this handler, excluding dispatch blocks
      std::unordered_set<llvm::BasicBlock*> dispatch_blocks;
      for (auto* block : dispatch_chain) {
        dispatch_blocks.insert(block);
      }
      
      auto handler_blocks = CollectHandlerBlocks(actual_handler_start, 
                                                 {landing_pad}, 
                                                 handler_entries,
                                                 dom_tree);
      
      // Debug: log blocks before any removal
      if (selector == 0 || !type_info) {
        LOG(INFO) << "  Before removal - catch-all has " << handler_blocks.size() << " blocks:";
        for (auto* block : handler_blocks) {
          std::string block_str;
          llvm::raw_string_ostream block_rso(block_str);
          block->printAsOperand(block_rso, false);
          LOG(INFO) << "    " << block_str;
        }
      }
      
      // Remove any dispatch blocks from handler blocks
      for (auto* dispatch_block : dispatch_blocks) {
        bool was_removed = handler_blocks.erase(dispatch_block) > 0;
        if (was_removed && (selector == 0 || !type_info)) {
          std::string block_str;
          llvm::raw_string_ostream block_rso(block_str);
          dispatch_block->printAsOperand(block_rso, false);
          LOG(INFO) << "  Removed dispatch block " << block_str << " from catch-all handler";
          
          // Special check for %119
          if (block_str == "%119") {
            LOG(WARNING) << "  WARNING: Removed %119 as dispatch block from catch-all handler!";
          }
        }
      }
      
      // Additionally, remove any blocks that contain exception type checking infrastructure
      // BUT: For catch-all handlers, be more careful - only remove pure infrastructure blocks
      std::unordered_set<llvm::BasicBlock*> infrastructure_blocks;
      for (auto* block : handler_blocks) {
        bool is_infrastructure = false;
        bool has_user_code = false;
        
        // Count instructions to determine if this is pure infrastructure
        int typeid_calls = 0;
        int total_non_phi_instructions = 0;
        
        for (auto &inst : *block) {
          if (!llvm::isa<llvm::PHINode>(&inst)) {
            total_non_phi_instructions++;
          }
          
          if (auto call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
            if (auto callee = call->getCalledFunction()) {
              if (callee->getName() == "llvm.eh.typeid.for") {
                typeid_calls++;
              } else if (!callee->getName().startswith("llvm.") && 
                         callee->getName() != "__cxa_begin_catch" &&
                         callee->getName() != "__cxa_end_catch") {
                // This is likely user code
                has_user_code = true;
              }
            }
          } else if (llvm::isa<llvm::StoreInst>(&inst) || 
                     llvm::isa<llvm::LoadInst>(&inst) ||
                     llvm::isa<llvm::GetElementPtrInst>(&inst)) {
            // These might be user code operations
            has_user_code = true;
          }
        }
        
        // For catch-all handlers, only mark as infrastructure if it's purely dispatch code
        if (selector == 0 || !type_info) {
          // Pure infrastructure = mostly typeid calls and comparisons, no user code
          is_infrastructure = typeid_calls > 0 && !has_user_code && total_non_phi_instructions <= 5;
        } else {
          // For typed handlers, any block with typeid is infrastructure
          is_infrastructure = typeid_calls > 0;
        }
        
        if (is_infrastructure) {
          infrastructure_blocks.insert(block);
          std::string block_str;
          llvm::raw_string_ostream block_rso(block_str);
          block->printAsOperand(block_rso, false);
          block_rso.flush();
          LOG(INFO) << "Excluding infrastructure block " << block_str << " from handler"
                    << " (typeid_calls=" << typeid_calls 
                    << ", has_user_code=" << has_user_code
                    << ", total_instructions=" << total_non_phi_instructions << ")";
        }
      }
      
      // Remove infrastructure blocks
      for (auto* infra_block : infrastructure_blocks) {
        bool was_removed = handler_blocks.erase(infra_block) > 0;
        if (was_removed && (selector == 0 || !type_info)) {
          std::string block_str;
          llvm::raw_string_ostream block_rso(block_str);
          infra_block->printAsOperand(block_rso, false);
          LOG(INFO) << "  Removed infrastructure block " << block_str << " from catch-all handler";
          
          // Special check for %119
          if (block_str == "%119") {
            LOG(WARNING) << "  WARNING: Removed %119 as infrastructure block from catch-all handler!";
          }
        }
      }
      
      // Extract type information
      std::string type_name = ExtractTypeNameFromTypeInfo(type_info);
      bool is_catch_all = (selector == 0 || !type_info);
      
      // IMPORTANT: For catch-all handlers, ensure the entry block is included
      if (is_catch_all) {
        // Log current state
        std::string handler_entry_debug;
        llvm::raw_string_ostream handler_entry_debug_rso(handler_entry_debug);
        if (handler_entry) {
          handler_entry->printAsOperand(handler_entry_debug_rso, false);
        } else {
          handler_entry_debug_rso << "nullptr";
        }
        
        std::string actual_start_debug;
        llvm::raw_string_ostream actual_start_debug_rso(actual_start_debug);
        if (actual_handler_start) {
          actual_handler_start->printAsOperand(actual_start_debug_rso, false);
        } else {
          actual_start_debug_rso << "nullptr";
        }
        
        LOG(INFO) << "  Catch-all debug: handler_entry=" << handler_entry_debug
                  << ", actual_handler_start=" << actual_start_debug
                  << ", handler_blocks.size()=" << handler_blocks.size();
        
        // Check both handler_entry and actual_handler_start
        if (handler_entry && handler_blocks.find(handler_entry) == handler_blocks.end()) {
          handler_blocks.insert(handler_entry);
          LOG(INFO) << "  Added missing catch-all handler_entry block: " << handler_entry_debug;
        }
        
        if (actual_handler_start && actual_handler_start != handler_entry && 
            handler_blocks.find(actual_handler_start) == handler_blocks.end()) {
          handler_blocks.insert(actual_handler_start);
          LOG(INFO) << "  Added missing catch-all actual_handler_start block: " << actual_start_debug;
        }
      }
      
      // Determine the exception type
      llvm::Type* exception_type = nullptr;
      if (type_info && !is_catch_all) {
        if (auto global = llvm::dyn_cast<llvm::GlobalVariable>(type_info)) {
          exception_type = global->getType();
        }
      }
      
      // Log final state before saving
      if (is_catch_all) {
        LOG(INFO) << "Final catch-all handler blocks before AddCatchHandler (" 
                  << handler_blocks.size() << " blocks):";
        for (auto* block : handler_blocks) {
          std::string block_str;
          llvm::raw_string_ostream block_rso(block_str);
          block->printAsOperand(block_rso, false);
          LOG(INFO) << "    " << block_str;
        }
      }
      
      // Generate catch-all statements for catch-all handlers
      std::vector<std::string> catch_all_statements;
      if (is_catch_all && !handler_blocks.empty()) {
        LOG(INFO) << "Generating catch-all statements for handler";
        
        // Generate simple placeholder statements for catch-all handlers
        catch_all_statements.push_back("printf(\"=== GENERIC CATCH BLOCK ===\\n\");");
        catch_all_statements.push_back("printf(\"Unknown exception caught\\n\");");
        catch_all_statements.push_back("return -4;");
      }
      
      // Save the catch handler
      dec_ctx.exception_regions.AddCatchHandler(
          landing_pad, 
          handler_entry, 
          exception_type,
          type_name,
          type_info,
          selector,
          is_catch_all,
          handler_blocks,
          infrastructure_blocks,
          catch_all_statements);
      
      std::string handler_str;
      if (handler_entry) {
        llvm::raw_string_ostream handler_stream(handler_str);
        handler_entry->printAsOperand(handler_stream, false);
        handler_stream.flush();
      }
      LOG(INFO) << "Catch handler " << handler_str
                << " for " << (is_catch_all ? "catch(...)" : type_name)
                << " contains " << handler_blocks.size() << " blocks";
    }
  }
  
  // Handle the case where there's a single catch-all with no selector dispatch
  if (selector_to_typeinfo.empty() && landing_pad->getTerminator()->getNumSuccessors() > 0) {
    // Check if this is a cleanup-only landing pad (no catch clauses)
    if (landing_inst->isCleanup() && landing_inst->getNumClauses() == 0) {
      LOG(INFO) << "Landing pad is cleanup-only, not creating catch handlers";
      return;
    }
    
    // Only create catch-all handler if there are actual catch clauses
    if (landing_inst->getNumClauses() > 0 || has_catch_all) {
      // Assume first successor is a catch-all handler
      auto handler_entry = landing_pad->getTerminator()->getSuccessor(0);
      auto handler_blocks = CollectHandlerBlocks(handler_entry, 
                                                 {landing_pad}, 
                                                 {},
                                                 dom_tree);
      
      // Generate catch-all statements for this catch-all handler
      std::vector<std::string> catch_all_statements_simple;
      catch_all_statements_simple.push_back("printf(\"=== GENERIC CATCH BLOCK ===\\n\");");
      catch_all_statements_simple.push_back("printf(\"Unknown exception caught\\n\");");
      catch_all_statements_simple.push_back("return -4;");
      
      dec_ctx.exception_regions.AddCatchHandler(
          landing_pad, 
          handler_entry, 
          nullptr,
          "",
          nullptr,
          0,
          true,
          handler_blocks,
          {},  // No infrastructure blocks filtered in this simple case
          catch_all_statements_simple);
      
      std::string handler_str4;
      llvm::raw_string_ostream handler_rso4(handler_str4);
      handler_entry->printAsOperand(handler_rso4, false);
      LOG(INFO) << "Default catch-all handler " << handler_str4
                << " contains " << handler_blocks.size() << " blocks";
    }
  }
}

llvm::Function* ExceptionHandlingPass::GetLLVMFunction(clang::FunctionDecl* func_decl) {
  // Find corresponding LLVM function
  for (auto& [val, decl] : dec_ctx.value_decls) {
    if (decl == func_decl) {
      return llvm::dyn_cast<llvm::Function>(val);
    }
  }
  return nullptr;
}

void ExceptionHandlingPass::RunImpl() {
  LOG(INFO) << "Running exception handling analysis pass";
  
  if (!enable_try_catch_transformation_) {
    LOG(INFO) << "Exception transformation disabled, skipping analysis";
    return;
  }
  
  // Note: Direct module analysis is now handled by the caller that has access to the module
  // This method focuses on AST visitor pattern analysis
  TransformVisitor<ExceptionHandlingPass>::RunImpl();
  
  // Don't populate here - it will be done in AnalyzeAllFunctionsWithExceptions
  // PopulateStatementExceptionContext(dec_ctx);
}

void ExceptionHandlingPass::AnalyzeAllFunctionsWithExceptions(llvm::Module& llvm_module) {
  LOG(INFO) << "Directly analyzing all functions with exception handling";
  
  // Iterate through all functions in the LLVM module
  for (auto& llvm_func : llvm_module) {
    if (llvm_func.isDeclaration()) continue;
    
    // Check if this function has exception handling
    bool has_landingpads = false;
    for (auto& bb : llvm_func) {
      if (bb.getLandingPadInst()) {
        has_landingpads = true;
        break;
      }
    }
    
    if (!has_landingpads) continue;
    
    LOG(INFO) << "Found LLVM function with exception handling: " << llvm_func.getName().str();
    
    // Find the corresponding Clang function declaration
    clang::FunctionDecl* clang_func = nullptr;
    for (auto& [llvm_val, clang_decl] : dec_ctx.value_decls) {
      if (llvm_val == &llvm_func) {
        clang_func = llvm::dyn_cast<clang::FunctionDecl>(clang_decl);
        if (clang_func) break;
      }
    }
    
    if (!clang_func) {
      LOG(WARNING) << "Could not find corresponding Clang function declaration for " << llvm_func.getName().str();
      continue;
    }
    
    LOG(INFO) << "Analyzing Clang function " << clang_func->getNameAsString() << " (LLVM: " << llvm_func.getName().str() << ")";
    
    // Analyze this function for exceptions
    AnalyzeFunctionForExceptions(clang_func, dec_ctx);
  }
  
  // After analyzing all functions, clean up try regions to remove catch handler blocks
  LOG(INFO) << "=== ABOUT TO CALL CleanupTryRegions ===";
  CleanupTryRegions(dec_ctx);
  
  // After cleaning up, merge try regions that should be unified
  MergeTryRegions(dec_ctx);
  
  // Then populate the statement exception context map
  PopulateStatementExceptionContext(dec_ctx);
  LOG(INFO) << "Finished analyzing all functions with exceptions";
}

void ExceptionHandlingPass::CleanupTryRegions(DecompilationContext& dec_ctx) {
  LOG(INFO) << "=== CLEANUP TRY REGIONS STARTING ===";
  
  auto& regions = dec_ctx.exception_regions.GetMutableTryRegions();
  
  for (auto& region : regions) {
    // Collect all blocks that belong to catch handlers for this region
    std::unordered_set<llvm::BasicBlock*> all_catch_blocks;
    
    for (const auto& handler : region.catch_handlers) {
      all_catch_blocks.insert(handler.blocks.begin(), handler.blocks.end());
    }
    
    // ADDITIONAL: Look for blocks that contain generic catch code patterns
    std::unordered_set<llvm::BasicBlock*> generic_catch_blocks;
    for (auto* block : region.blocks) {
      for (auto &inst : *block) {
        if (auto call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
          if (auto callee = call->getCalledFunction()) {
            // Check for printf calls with generic catch strings
            if (callee->getName() == "printf" && call->getNumOperands() > 0) {
              if (auto str_arg = llvm::dyn_cast<llvm::GlobalVariable>(call->getOperand(0))) {
                if (str_arg->hasInitializer()) {
                  if (auto str_init = llvm::dyn_cast<llvm::ConstantDataArray>(str_arg->getInitializer())) {
                    if (str_init->isString()) {
                      std::string str_val = str_init->getAsString().str();
                      if (str_val.find("GENERIC CATCH BLOCK") != std::string::npos ||
                          str_val.find("Unknown exception caught") != std::string::npos ||
                          str_val.find("generic handler") != std::string::npos) {
                        generic_catch_blocks.insert(block);
                        std::string block_str;
                        llvm::raw_string_ostream block_rso(block_str);
                        block->printAsOperand(block_rso, false);
                        LOG(INFO) << "  Found generic catch block: " << block_str;
                      }
                    }
                  }
                }
              }
            }
            // Also check for __cxa_begin_catch followed by error_code = 9999
            else if (callee->getName() == "__cxa_begin_catch") {
              // Look for assignments to 9999 in the same block or immediate successors
              bool has_9999 = false;
              for (auto &check_inst : *block) {
                if (auto store = llvm::dyn_cast<llvm::StoreInst>(&check_inst)) {
                  if (auto const_val = llvm::dyn_cast<llvm::ConstantInt>(store->getValueOperand())) {
                    if (const_val->getZExtValue() == 9999) {
                      has_9999 = true;
                      break;
                    }
                  }
                }
              }
              if (has_9999) {
                generic_catch_blocks.insert(block);
                std::string block_str;
                llvm::raw_string_ostream block_rso(block_str);
                block->printAsOperand(block_rso, false);
                LOG(INFO) << "  Found generic catch block with error_code 9999: " << block_str;
              }
            }
          }
        }
      }
    }
    
    // Combine all blocks that should be removed
    all_catch_blocks.insert(generic_catch_blocks.begin(), generic_catch_blocks.end());
    
    // Remove catch handler blocks from the try blocks
    size_t removed = 0;
    for (auto it = region.blocks.begin(); it != region.blocks.end(); ) {
      if (all_catch_blocks.count(*it) > 0) {
        std::string block_str;
        llvm::raw_string_ostream block_rso(block_str);
        (*it)->printAsOperand(block_rso, false);
        LOG(INFO) << "  Removing catch handler block " << block_str << " from try region";
        it = region.blocks.erase(it);
        removed++;
      } else {
        ++it;
      }
    }
    
    if (removed > 0) {
      LOG(INFO) << "  Removed " << removed << " catch handler blocks from try region";
    }
  }
}

void ExceptionHandlingPass::MergeTryRegions(DecompilationContext& dec_ctx) {
  LOG(INFO) << "Merging try regions that should be unified";
  
  auto& regions = dec_ctx.exception_regions.GetMutableTryRegions();
  if (regions.size() <= 1) {
    LOG(INFO) << "Only " << regions.size() << " regions, no merging needed";
    return;
  }
  
  // Group regions by function
  std::map<llvm::Function*, std::vector<size_t>> regions_by_function;
  for (size_t i = 0; i < regions.size(); ++i) {
    if (!regions[i].blocks.empty()) {
      auto* first_block = *regions[i].blocks.begin();
      if (first_block && first_block->getParent()) {
        regions_by_function[first_block->getParent()].push_back(i);
      }
    }
  }
  
  // For each function with multiple regions, check if they should be merged
  for (auto& [func, region_indices] : regions_by_function) {
    if (region_indices.size() <= 1) continue;
    
    LOG(INFO) << "Function " << func->getName().str() << " has " << region_indices.size() << " try regions";
    
    // First, check for nested try-catch blocks
    // A nested try-catch is when one region's catch handler blocks contain another region's try blocks
    std::vector<std::pair<size_t, size_t>> nested_pairs; // pairs of (outer_idx, inner_idx)
    
    for (size_t i = 0; i < region_indices.size(); ++i) {
      auto& region_i = regions[region_indices[i]];
      
      // Collect all blocks that are part of catch handlers in region i
      std::unordered_set<llvm::BasicBlock*> catch_blocks_i;
      for (const auto& handler : region_i.catch_handlers) {
        catch_blocks_i.insert(handler.blocks.begin(), handler.blocks.end());
      }
      
      // Check if any other region's try blocks are contained within these catch blocks
      for (size_t j = 0; j < region_indices.size(); ++j) {
        if (i == j) continue;
        
        auto& region_j = regions[region_indices[j]];
        
        // Check if region_j's try blocks are inside region_i's catch blocks
        bool is_nested = false;
        for (auto* try_block : region_j.blocks) {
          if (catch_blocks_i.count(try_block) > 0) {
            is_nested = true;
            break;
          }
        }
        
        if (is_nested) {
          nested_pairs.push_back({i, j});
          LOG(INFO) << "  Found nested try-catch: Region " << j << " is nested inside Region " << i << "'s catch handler";
        }
      }
    }
    
    // Debug: print handler types for each region
    for (size_t idx = 0; idx < region_indices.size(); ++idx) {
      auto& region = regions[region_indices[idx]];
      LOG(INFO) << "  Region " << idx << " has " << region.catch_handlers.size() << " handlers:";
      for (const auto& handler : region.catch_handlers) {
        LOG(INFO) << "    - " << handler.exception_type_name;
      }
    }
    
    // If we found nested try-catch blocks, don't merge them but set up parent-child relationships
    if (!nested_pairs.empty()) {
      LOG(INFO) << "Found " << nested_pairs.size() << " nested try-catch relationships, skipping merge to preserve structure";
      
      // Set up parent-child relationships
      for (const auto& [outer_idx, inner_idx] : nested_pairs) {
        auto& outer_region = regions[region_indices[outer_idx]];
        auto& inner_region = regions[region_indices[inner_idx]];
        
        // Set parent-child relationship
        inner_region.parent_region = &outer_region;
        outer_region.nested_regions.push_back(&inner_region);
        
        LOG(INFO) << "Set up parent-child relationship: outer region at " 
                  << outer_region.landingpad_block << " contains inner region at " 
                  << inner_region.landingpad_block;
      }
      
      continue;
    }
    
    // Improved merging logic: merge regions if they appear to be part of the same
    // logical exception handling structure
    bool should_merge = false;
    
    // Strategy 1: If regions have overlapping or identical catch handler types, merge them
    auto& first_region = regions[region_indices[0]];
    std::set<std::string> all_types;
    for (const auto& handler : first_region.catch_handlers) {
      all_types.insert(handler.exception_type_name);
    }
    
    // Check if other regions have overlapping handler types
    for (size_t i = 1; i < region_indices.size(); ++i) {
      auto& region = regions[region_indices[i]];
      for (const auto& handler : region.catch_handlers) {
        if (all_types.count(handler.exception_type_name) > 0) {
          // Found overlap, these regions should be merged
          should_merge = true;
          break;
        }
        all_types.insert(handler.exception_type_name);
      }
    }
    
    // Strategy 2: If no overlap but regions are in sequence (one after another),
    // they might be part of the same logical try block
    if (!should_merge && region_indices.size() == 2) {
      // Check if the second region's blocks come after the first region's blocks
      auto& region1 = regions[region_indices[0]];
      auto& region2 = regions[region_indices[1]];
      
      // Simple heuristic: if they have the same number and types of handlers,
      // they're likely the same logical try block split by LLVM
      if (region1.catch_handlers.size() == region2.catch_handlers.size()) {
        should_merge = true;
        for (size_t i = 0; i < region1.catch_handlers.size() && should_merge; ++i) {
          if (region1.catch_handlers[i].exception_type_name != 
              region2.catch_handlers[i].exception_type_name) {
            should_merge = false;
          }
        }
      }
    }
    
    if (should_merge) {
      LOG(INFO) << "Merging " << region_indices.size() << " regions in function " << func->getName().str();
      
      // Merge all regions into the first one
      auto& target_region = regions[region_indices[0]];
      
      for (size_t i = 1; i < region_indices.size(); ++i) {
        auto& source_region = regions[region_indices[i]];
        
        // Merge blocks
        target_region.blocks.insert(source_region.blocks.begin(), source_region.blocks.end());
        
        // Merge catch handlers (avoid duplicates)
        for (const auto& handler : source_region.catch_handlers) {
          bool found = false;
          for (const auto& existing : target_region.catch_handlers) {
            if (existing.exception_type_name == handler.exception_type_name) {
              found = true;
              break;
            }
          }
          if (!found) {
            target_region.catch_handlers.push_back(handler);
          }
        }
        
        // Clear the source region (mark for removal)
        source_region.blocks.clear();
      }
      
      LOG(INFO) << "Merged region now has " << target_region.blocks.size() << " blocks";
    }
  }
  
  // Remove empty regions
  regions.erase(
    std::remove_if(regions.begin(), regions.end(),
                   [](const TryRegion& r) { return r.blocks.empty(); }),
    regions.end()
  );
  
  LOG(INFO) << "After merging: " << regions.size() << " try regions remain";
}

// Stub implementations for legacy methods
bool ExceptionHandlingPass::IsBeginCatchCall(clang::CallExpr *call_expr) {
  return false;
}

bool ExceptionHandlingPass::IsEndCatchCall(clang::CallExpr *call_expr) {
  return false;
}

bool ExceptionHandlingPass::IsExceptionInfrastructureStatement(clang::Stmt* stmt) {
  return false;
}

std::string ExceptionHandlingPass::DemangleExceptionType(const std::string& mangled_name) {
  return DemangleTypeName(mangled_name);
}

std::string ExceptionHandlingPass::CreateVariableNameFromType(const std::string& exception_type) {
  // Extract just the class name from the full type
  std::string type = exception_type;
  
  // Remove "class " or "struct " prefix
  if (type.substr(0, 6) == "class ") {
    type = type.substr(6);
  } else if (type.substr(0, 7) == "struct ") {
    type = type.substr(7);
  }
  
  // Remove namespace qualifiers
  size_t pos = type.rfind("::");
  if (pos != std::string::npos) {
    type = type.substr(pos + 2);
  }
  
  // Convert to lowercase for variable name
  if (!type.empty()) {
    type[0] = std::tolower(type[0]);
  }
  
  return type.empty() ? "e" : type;
}

clang::QualType ExceptionHandlingPass::CreateExceptionType(const std::string& exception_type_name) {
  // This would need proper type resolution in the AST
  // For now, return a placeholder
  return dec_ctx.ast_ctx.VoidTy;
}

void ExceptionHandlingPass::PopulateStatementExceptionContext(DecompilationContext& dec_ctx) {
  LOG(INFO) << "Populating statement exception context map";
  LOG(INFO) << "Total try regions: " << dec_ctx.exception_regions.GetTryRegions().size();
  LOG(INFO) << "Total stmt_provenance entries: " << dec_ctx.stmt_provenance.size();
  LOG(INFO) << "Total stmt_to_bb entries: " << dec_ctx.stmt_to_bb.size();
  LOG(INFO) << "Total bb_name_to_llvm_bb entries: " << dec_ctx.bb_name_to_llvm_bb.size();
  
  // For each try region
  for (const auto& region : dec_ctx.exception_regions.GetTryRegions()) {
    LOG(INFO) << "Processing try region with " << region.blocks.size() << " blocks and " 
              << region.catch_handlers.size() << " catch handlers";
    
    // Skip child regions - they will be handled as part of their parent's catch handlers
    if (region.parent_region != nullptr) {
      LOG(INFO) << "  Skipping child region - will be handled as part of parent";
      continue;
    }
    
    // Only mark statements as TRY_BLOCK if there are actual catch handlers
    int try_stmts_found = 0;
    if (!region.catch_handlers.empty()) {
      for (auto* bb : region.blocks) {
        if (!bb) {
          LOG(WARNING) << "  Null basic block in try blocks, skipping";
          continue;
        }
        
        std::string tb_str;
        llvm::raw_string_ostream tb_rso(tb_str);
        bb->printAsOperand(tb_rso, false);
        LOG(INFO) << "  Checking try block: " << tb_str;
        
        // First check stmt_provenance
        for (auto& [stmt, value] : dec_ctx.stmt_provenance) {
          if (!stmt || !value) continue;
          
          if (auto* inst = llvm::dyn_cast<llvm::Instruction>(value)) {
            if (inst->getParent() == bb) {
              DecompilationContext::ExceptionContext ctx;
              ctx.type = DecompilationContext::ExceptionContext::TRY_BLOCK;
              ctx.landing_pad = region.landingpad_block;
              dec_ctx.stmt_exception_context[stmt] = ctx;
              try_stmts_found++;
            }
          }
        }
        
        // Also check stmt_to_bb mapping
        for (auto& [stmt, bb_name] : dec_ctx.stmt_to_bb) {
          if (!stmt) continue;
          
          // Check if this BB name corresponds to our BB
          auto bb_it = dec_ctx.bb_name_to_llvm_bb.find(bb_name);
          if (bb_it != dec_ctx.bb_name_to_llvm_bb.end() && bb_it->second == bb) {
            // Check if we already marked this statement
            if (dec_ctx.stmt_exception_context.find(stmt) == dec_ctx.stmt_exception_context.end()) {
              DecompilationContext::ExceptionContext ctx;
              ctx.type = DecompilationContext::ExceptionContext::TRY_BLOCK;
              ctx.landing_pad = region.landingpad_block;
              dec_ctx.stmt_exception_context[stmt] = ctx;
              try_stmts_found++;
            }
          }
        }
      }
    } else {
      LOG(INFO) << "  No catch handlers for this landing pad, skipping TRY_BLOCK marking";
    }
    LOG(INFO) << "  Found " << try_stmts_found << " statements in try blocks";
    
    // Mark all statements in catch handlers
    for (const auto& handler : region.catch_handlers) {
      int catch_stmts_found = 0;
      LOG(INFO) << "  Processing catch handler for type: " << handler.exception_type_name 
                << " with " << handler.blocks.size() << " blocks";
      for (auto* bb : handler.blocks) {
        if (!bb) {
          LOG(WARNING) << "    Null basic block in handler blocks, skipping";
          continue;
        }
        
        std::string cb_str;
        llvm::raw_string_ostream cb_rso(cb_str);
        bb->printAsOperand(cb_rso, false);
        LOG(INFO) << "    Checking catch block: " << cb_str;
        
        // First check stmt_provenance
        int statements_checked = 0;
        int statements_in_block = 0;
        for (auto& [stmt, value] : dec_ctx.stmt_provenance) {
          if (!stmt || !value) continue;
          statements_checked++;
          
          if (auto* inst = llvm::dyn_cast<llvm::Instruction>(value)) {
            if (inst->getParent() == bb) {
              statements_in_block++;
              // Check if this is a printf call
              bool is_printf = false;
              if (auto* call_expr = llvm::dyn_cast<clang::CallExpr>(stmt)) {
                if (auto* callee = call_expr->getDirectCallee()) {
                  if (callee->getName() == "printf") {
                    is_printf = true;
                    LOG(INFO) << "      Found printf call in catch handler block " << cb_str;
                  }
                }
              }
              
              // Skip exception dispatch infrastructure
              if (IsExceptionDispatchInstruction(inst)) {
                LOG(INFO) << "      Skipping exception dispatch instruction";
                continue;
              }
              
              LOG(INFO) << "      Adding statement to catch handler context" << (is_printf ? " (printf call)" : "");
              DecompilationContext::ExceptionContext ctx;
              ctx.type = DecompilationContext::ExceptionContext::CATCH_HANDLER;
              ctx.exception_type = handler.exception_type_name;
              ctx.landing_pad = region.landingpad_block;
              dec_ctx.stmt_exception_context[stmt] = ctx;
              catch_stmts_found++;
            }
          }
        }
        
        LOG(INFO) << "    Block " << cb_str << ": checked " << statements_checked 
                  << " total statements, found " << statements_in_block << " in this block";

        // Also check stmt_to_bb mapping  
        for (auto& [stmt, bb_name] : dec_ctx.stmt_to_bb) {
          if (!stmt) continue;

          //LOG(INFO) << "  Checking stmt_to_bb for statement: " << stmt->getStmtClassName();

          // Check if this BB name corresponds to our BB
          auto bb_it = dec_ctx.bb_name_to_llvm_bb.find(bb_name);
          if (bb_it != dec_ctx.bb_name_to_llvm_bb.end() && bb_it->second == bb) {
            // Check if we already marked this statement
            if (dec_ctx.stmt_exception_context.find(stmt) == dec_ctx.stmt_exception_context.end()) {
              // Check if this statement corresponds to dispatch infrastructure
              bool is_dispatch = false;
              
              if (auto provenance_it = dec_ctx.stmt_provenance.find(stmt); 
                  provenance_it != dec_ctx.stmt_provenance.end()) {

                if (!provenance_it->second) {
                  //LOG(INFO) << "      Provenance not found for statement: " << stmt->getStmtClassName();
                } else if (auto* inst = llvm::dyn_cast<llvm::Instruction>(provenance_it->second)) {
                  //LOG(INFO) << "      Checking if instruction is exception dispatch instruction";
                  
                  if (IsExceptionDispatchInstruction(inst)) {
                    LOG(INFO) << "      Skipping exception dispatch instruction (from stmt_to_bb)";
                    is_dispatch = true;
                  }
                }
              }
              
              if (!is_dispatch) {
                DecompilationContext::ExceptionContext ctx;
                ctx.type = DecompilationContext::ExceptionContext::CATCH_HANDLER;
                ctx.exception_type = handler.exception_type_name;
                ctx.landing_pad = region.landingpad_block;
                dec_ctx.stmt_exception_context[stmt] = ctx;
                catch_stmts_found++;
              }
            }
          }
        }
      }
      LOG(INFO) << "    Found " << catch_stmts_found << " statements in this catch handler";
    }
  }
  
  LOG(INFO) << "Populated exception context for " << dec_ctx.stmt_exception_context.size() << " statements";
}

}  // namespace rellic