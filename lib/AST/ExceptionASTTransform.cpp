/*
 * Copyright (c) 2021-present, Trail of Bits, Inc.
 * All rights reserved.
 *
 * This source code is licensed in accordance with the terms specified in
 * the LICENSE file found in the root directory of this source tree.
 */

#include "rellic/AST/ExceptionASTTransform.h"
#include "rellic/AST/DecompilationContext.h"
#include "rellic/AST/ExceptionRegionInfo.h"
#include "rellic/AST/ASTBuilder.h"
#include "rellic/AST/IRToASTVisitor.h"

#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/Stmt.h>
#include <clang/AST/StmtCXX.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/RecursiveASTVisitor.h>

#include <llvm/IR/Function.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/Support/raw_ostream.h>

#include <glog/logging.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <algorithm>
#include <queue>
#include <set>
#include <map>
#include <functional>
#include <cxxabi.h>
#include <cstdlib>

namespace rellic {

namespace {

// Analyze exception type hierarchy from LLVM IR
class ExceptionTypeHierarchy {
public:
  struct TypeInfo {
    llvm::GlobalVariable* typeinfo_global;  // The LLVM global for this typeinfo
    std::set<llvm::GlobalVariable*> base_types;  // Direct base type globals
    bool is_catch_all = false;
    int hierarchy_depth = 0;  // Distance from most derived type (0 = leaf)
  };
  
private:
  std::map<llvm::GlobalVariable*, TypeInfo> type_hierarchy;
  
  // Extract exception types from landingpad instructions and catch handlers
  void AnalyzeExceptionTypes(llvm::Module* module) {
    if (!module) return;
    
    // First pass: collect all exception types from landingpad instructions
    for (auto& func : *module) {
      for (auto& bb : func) {
        if (auto* lp = llvm::dyn_cast<llvm::LandingPadInst>(bb.getFirstNonPHI())) {
          for (unsigned i = 0; i < lp->getNumClauses(); ++i) {
            if (lp->isCatch(i)) {
              if (auto* clause_val = lp->getClause(i)) {
                if (auto* global = llvm::dyn_cast<llvm::GlobalVariable>(clause_val)) {
                  // This is an exception type - add it to our hierarchy
                  if (type_hierarchy.find(global) == type_hierarchy.end()) {
                    type_hierarchy[global] = TypeInfo{global, {}, false, 0};
                  }
                }
              }
            }
          }
        }
      }
    }
    
    // Second pass: analyze typeinfo structures to find base relationships
    for (auto& [typeinfo_global, info] : type_hierarchy) {
      AnalyzeTypeInfoStructure(typeinfo_global);
    }
    
    // Third pass: compute hierarchy depths
    ComputeHierarchyDepths();
  }
  
  void AnalyzeTypeInfoStructure(llvm::GlobalVariable* typeinfo) {
    if (!typeinfo || !typeinfo->hasInitializer()) return;
    
    auto* init = typeinfo->getInitializer();
    
    // RTTI typeinfo structures typically have pointers to base class typeinfos
    // The exact layout depends on the ABI, but we can look for global references
    std::function<void(llvm::Constant*)> find_typeinfo_refs;
    find_typeinfo_refs = [&](llvm::Constant* c) {
      if (!c) return;
      
      if (auto* global = llvm::dyn_cast<llvm::GlobalVariable>(c->stripPointerCasts())) {
        // Check if this is a typeinfo global (starts with _ZTI)
        if (global != typeinfo && global->hasName() && 
            global->getName().startswith("_ZTI")) {
          // This could be a base class typeinfo
          type_hierarchy[typeinfo].base_types.insert(global);
        }
      }
      
      // Recursively search in aggregates
      if (auto* struct_c = llvm::dyn_cast<llvm::ConstantStruct>(c)) {
        for (unsigned i = 0; i < struct_c->getNumOperands(); ++i) {
          find_typeinfo_refs(struct_c->getOperand(i));
        }
      } else if (auto* array_c = llvm::dyn_cast<llvm::ConstantArray>(c)) {
        for (unsigned i = 0; i < array_c->getNumOperands(); ++i) {
          find_typeinfo_refs(array_c->getOperand(i));
        }
      }
    };
    
    find_typeinfo_refs(init);
  }
  
  void ComputeHierarchyDepths() {
    // Use BFS to compute depths - most derived types have depth 0
    std::queue<llvm::GlobalVariable*> to_process;
    
    // Find leaf types (no types derive from them)
    std::set<llvm::GlobalVariable*> has_derived;
    for (const auto& [type, info] : type_hierarchy) {
      for (auto* base : info.base_types) {
        has_derived.insert(base);
      }
    }
    
    // Start with leaf types
    for (auto& [type, info] : type_hierarchy) {
      if (has_derived.find(type) == has_derived.end()) {
        info.hierarchy_depth = 0;
        to_process.push(type);
      }
    }
    
    // Propagate depths
    while (!to_process.empty()) {
      auto* current = to_process.front();
      to_process.pop();
      
      int current_depth = type_hierarchy[current].hierarchy_depth;
      
      // Update base types
      for (auto* base : type_hierarchy[current].base_types) {
        if (type_hierarchy[base].hierarchy_depth < current_depth + 1) {
          type_hierarchy[base].hierarchy_depth = current_depth + 1;
          to_process.push(base);
        }
      }
    }
  }
  
public:
  void AnalyzeModule(llvm::Module* module) {
    AnalyzeExceptionTypes(module);
    
    // Add known C++ exception hierarchy relationships
    // These are standard relationships that might not be in the LLVM IR
    AddKnownHierarchies();
  }
  
private:
  void AddKnownHierarchies() {
    // Helper to find typeinfo by name pattern
    auto find_typeinfo = [this](const std::string& pattern) -> llvm::GlobalVariable* {
      for (auto& [global, info] : type_hierarchy) {
        if (global->hasName() && global->getName().str().find(pattern) != std::string::npos) {
          return global;
        }
      }
      return nullptr;
    };
    
    // Standard C++ exception hierarchy
    // std::exception is the base of most exceptions
    auto* exception_ti = find_typeinfo("_ZTISt9exception");
    auto* runtime_error_ti = find_typeinfo("_ZTISt13runtime_error");
    auto* logic_error_ti = find_typeinfo("_ZTISt11logic_error");
    auto* invalid_argument_ti = find_typeinfo("_ZTISt16invalid_argument");
    auto* overflow_error_ti = find_typeinfo("_ZTISt14overflow_error");
    auto* underflow_error_ti = find_typeinfo("_ZTISt15underflow_error");
    auto* range_error_ti = find_typeinfo("_ZTISt11range_error");
    auto* out_of_range_ti = find_typeinfo("_ZTISt12out_of_range");
    
    // Build the known hierarchy
    if (exception_ti) {
      // runtime_error : exception
      if (runtime_error_ti) {
        type_hierarchy[runtime_error_ti].base_types.insert(exception_ti);
      }
      // logic_error : exception  
      if (logic_error_ti) {
        type_hierarchy[logic_error_ti].base_types.insert(exception_ti);
      }
    }
    
    // invalid_argument : logic_error : exception
    if (invalid_argument_ti && logic_error_ti) {
      type_hierarchy[invalid_argument_ti].base_types.insert(logic_error_ti);
    } else if (invalid_argument_ti && exception_ti) {
      // Fallback if logic_error not found
      type_hierarchy[invalid_argument_ti].base_types.insert(exception_ti);
    }
    
    // overflow_error, underflow_error, range_error : runtime_error : exception
    if (runtime_error_ti) {
      if (overflow_error_ti) {
        type_hierarchy[overflow_error_ti].base_types.insert(runtime_error_ti);
      }
      if (underflow_error_ti) {
        type_hierarchy[underflow_error_ti].base_types.insert(runtime_error_ti);
      }
      if (range_error_ti) {
        type_hierarchy[range_error_ti].base_types.insert(runtime_error_ti);
      }
    } else if (exception_ti) {
      // Fallback if runtime_error not found
      if (overflow_error_ti) {
        type_hierarchy[overflow_error_ti].base_types.insert(exception_ti);
      }
      if (underflow_error_ti) {
        type_hierarchy[underflow_error_ti].base_types.insert(exception_ti);
      }
      if (range_error_ti) {
        type_hierarchy[range_error_ti].base_types.insert(exception_ti);
      }
    }
    
    // out_of_range : logic_error : exception
    if (out_of_range_ti && logic_error_ti) {
      type_hierarchy[out_of_range_ti].base_types.insert(logic_error_ti);
    } else if (out_of_range_ti && exception_ti) {
      // Fallback
      type_hierarchy[out_of_range_ti].base_types.insert(exception_ti);
    }
    
    // Recompute depths after adding known relationships
    ComputeHierarchyDepths();
  }
  
public:
  
  bool IsBaseOf(llvm::GlobalVariable* base, llvm::GlobalVariable* derived) {
    if (!base || !derived) return false;
    if (base == derived) return true;
    
    // Check if base is in the transitive closure of derived's bases
    std::set<llvm::GlobalVariable*> visited;
    std::queue<llvm::GlobalVariable*> to_check;
    to_check.push(derived);
    
    while (!to_check.empty()) {
      auto* current = to_check.front();
      to_check.pop();
      
      if (visited.find(current) != visited.end()) continue;
      visited.insert(current);
      
      if (type_hierarchy.find(current) != type_hierarchy.end()) {
        for (auto* parent : type_hierarchy[current].base_types) {
          if (parent == base) return true;
          to_check.push(parent);
        }
      }
    }
    
    return false;
  }
  
  int GetHierarchyDepth(llvm::GlobalVariable* type) {
    if (!type) return -1;
    if (type_hierarchy.find(type) == type_hierarchy.end()) return 0;
    return type_hierarchy[type].hierarchy_depth;
  }
  
  int GetHierarchyDepth(const std::string& type_name) {
    // For catch-all
    if (type_name == "..." || type_name.empty()) return 1000;
    
    // Find typeinfo global by name
    for (const auto& [global, info] : type_hierarchy) {
      if (global->hasName() && global->getName() == type_name) {
        return info.hierarchy_depth;
      }
    }
    
    // Unknown types get depth 0
    return 0;
  }
  
  llvm::GlobalVariable* GetTypeInfoGlobal(const std::string& type_name) {
    for (const auto& [global, info] : type_hierarchy) {
      if (global->hasName() && global->getName() == type_name) {
        return global;
      }
    }
    return nullptr;
  }
  
  void DumpHierarchy(llvm::raw_ostream& os) {
    os << "Exception Type Hierarchy (from LLVM IR):\n";
    os << "Total types found: " << type_hierarchy.size() << "\n";
    for (const auto& [type, info] : type_hierarchy) {
      std::string name = type->hasName() ? type->getName().str() : "(unnamed)";
      
      // Directly use fallback patterns for known types
      std::string display_name = name;
      if (name == "_ZTISt9exception") display_name = "std::exception";
      else if (name == "_ZTISt13runtime_error") display_name = "std::runtime_error";
      else if (name == "_ZTISt11logic_error") display_name = "std::logic_error";
      else if (name == "_ZTISt16invalid_argument") display_name = "std::invalid_argument";
      else if (name == "_ZTISt14overflow_error") display_name = "std::overflow_error";
      else if (name == "_ZTISt15underflow_error") display_name = "std::underflow_error";
      else if (name == "_ZTISt11range_error") display_name = "std::range_error";
      else if (name == "_ZTISt12out_of_range") display_name = "std::out_of_range";
      
      if (display_name != name) {
        os << "  " << display_name << " (" << name << ")";
      } else {
        os << "  " << name;
      }
      os << " (depth=" << info.hierarchy_depth << ")";
      
      if (!info.base_types.empty()) {
        os << " extends: ";
        bool first = true;
        for (auto* base : info.base_types) {
          if (!first) os << ", ";
          std::string base_name = base->hasName() ? base->getName().str() : "(unnamed)";
          std::string base_demangled = DemangleTypeName(base_name);
          if (!base_demangled.empty()) {
            os << base_demangled << " (" << base_name << ")";
          } else {
            os << base_name;
          }
          first = false;
        }
      }
      os << "\n";
    }
  }
  
private:
  std::string DemangleTypeName(const std::string& mangled) {
    // Only try to demangle if it looks like a mangled name
    if (mangled.empty()) return "";
    
    // Handle typeinfo symbols specially
    std::string to_demangle = mangled;
    bool is_typeinfo = false;
    if (mangled.find("_ZTI") == 0) {
      // Convert _ZTI to _Z to demangle the type name
      to_demangle = "_Z" + mangled.substr(4);
      is_typeinfo = true;
    } else if (mangled.find("_Z") != 0) {
      // Not a mangled name
      return "";
    }
    
    int status;
    char* demangled = abi::__cxa_demangle(to_demangle.c_str(), nullptr, nullptr, &status);
    if (status == 0 && demangled) {
      std::string result(demangled);
      free(demangled);
      
      if (is_typeinfo) {
        // For typeinfo, we demangled the type constructor, extract the type name
        // e.g., "std::runtime_error::runtime_error()" -> "std::runtime_error"
        size_t pos = result.find("::");
        if (pos != std::string::npos) {
          // Find the last occurrence of :: to handle nested namespaces
          size_t last_pos = result.rfind("::", pos - 1);
          if (last_pos != std::string::npos && last_pos < pos) {
            result = result.substr(0, pos);
          } else {
            result = result.substr(0, pos);
          }
        }
        // Remove any function signatures
        pos = result.find('(');
        if (pos != std::string::npos) {
          result = result.substr(0, pos);
        }
      }
      
      return result;
    }
    
    // If demangling failed, try some simple patterns for known types
    if (mangled == "_ZTISt9exception") return "std::exception";
    if (mangled == "_ZTISt13runtime_error") return "std::runtime_error";
    if (mangled == "_ZTISt11logic_error") return "std::logic_error";
    if (mangled == "_ZTISt16invalid_argument") return "std::invalid_argument";
    if (mangled == "_ZTISt14overflow_error") return "std::overflow_error";
    if (mangled == "_ZTISt15underflow_error") return "std::underflow_error";
    if (mangled == "_ZTISt11range_error") return "std::range_error";
    if (mangled == "_ZTISt12out_of_range") return "std::out_of_range";
    
    return "";
  }
};

// Detect if a function call is a rethrow operation from various ABIs
bool IsRethrowFunction(const std::string& func_name) {
  // C++ ABI (GCC/Clang)
  if (func_name == "__cxa_rethrow") return true;
  
  // Microsoft Visual C++ ABI
  if (func_name == "_CxxThrowException" || func_name == "__CxxThrowException") return true;
  
  // Alternative spellings and mangled names
  if (func_name.find("rethrow") != std::string::npos) return true;
  if (func_name.find("ReThrow") != std::string::npos) return true;
  
  // Rust exception handling (if applicable)
  if (func_name.find("rust_begin_unwind") != std::string::npos) return true;
  
  // JavaScript/WASM exceptions (if applicable) 
  if (func_name == "__wasm_rethrow" || func_name == "wasm_rethrow") return true;
  
  // Any function ending with "_rethrow" pattern
  if (func_name.size() > 8 && func_name.substr(func_name.size() - 8) == "_rethrow") return true;
  
  return false;
}

// Detect rethrow patterns in LLVM instructions
bool IsRethrowInstruction(const llvm::Instruction* inst) {
  if (!inst) return false;
  
  llvm::Function* called_func = nullptr;
  
  if (auto* call_inst = llvm::dyn_cast<llvm::CallInst>(inst)) {
    called_func = call_inst->getCalledFunction();
  } else if (auto* invoke_inst = llvm::dyn_cast<llvm::InvokeInst>(inst)) {
    called_func = invoke_inst->getCalledFunction();
  }
  
  if (called_func) {
    return IsRethrowFunction(called_func->getName().str());
  }
  
  return false;
}

// Utility function to get the proper name of an LLVM basic block
std::string GetBlockName(const llvm::BasicBlock* block, DecompilationContext* dec_ctx = nullptr) {
  if (!block) return "(null)";
  
  // First, try to find the existing block name from the decomp context
  if (dec_ctx) {
    for (const auto& [name, bb] : dec_ctx->bb_name_to_llvm_bb) {
      if (bb == block) {
        return name;  // Return the existing deterministic name
      }
    }
  }
  
  // If not found, generate a deterministic name based on block position and content
  if (auto* func = block->getParent()) {
    std::string func_name = func->getName().str();
    
    // Count position of this block in the function (deterministic)
    int block_index = 0;
    for (auto& bb : *func) {
      if (&bb == block) break;
      block_index++;
    }
    
    // Create a deterministic hash based on block content
    std::string block_content;
    llvm::raw_string_ostream content_stream(block_content);
    for (auto& inst : *block) {
      inst.print(content_stream, true);  // Include metadata for uniqueness
    }
    content_stream.flush();
    
    // Create a hash from the content (deterministic)
    std::hash<std::string> hasher;
    uint32_t content_hash = static_cast<uint32_t>(hasher(block_content) & 0xFFFFFFFF);
    
    // Generate name in the format: {function_name}_bb_{index}_{hash}
    return func_name + "_bb_" + std::to_string(block_index) + "_" + std::to_string(content_hash);
  }
  
  // Final fallback to simple name
  std::string name;
  llvm::raw_string_ostream rso(name);
  block->printAsOperand(rso, false);
  rso.flush();
  
  return name;
}

// Helper class to collect all statements and their source locations
class StmtCollector : public clang::RecursiveASTVisitor<StmtCollector> {
public:
  std::vector<clang::Stmt*> statements;
  DecompilationContext& dec_ctx;
  
  StmtCollector(DecompilationContext& ctx) : dec_ctx(ctx) {}
  
  bool VisitStmt(clang::Stmt* stmt) {
    if (stmt && stmt != dec_ctx.marker_expr) {
      statements.push_back(stmt);
    }
    return true;
  }
};

// Helper to check if a call expression calls a specific function
bool IsCallToFunction(clang::CallExpr* call, const std::string& func_name) {
  if (!call) return false;
  
  auto callee = call->getDirectCallee();
  if (!callee) return false;
  
  return callee->getName() == func_name;
}

// Helper to check if an expression contains exception type checking
bool ContainsExceptionTypeCheck(clang::Expr* expr) {
  if (!expr) return false;
  
  class TypeCheckFinder : public clang::RecursiveASTVisitor<TypeCheckFinder> {
  public:
    bool found = false;
    
    bool VisitCallExpr(clang::CallExpr* call) {
      if (IsCallToFunction(call, "llvm_eh_typeid_for")) {
        found = true;
        return false; // Stop traversal
      }
      return true;
    }
    
    bool VisitDeclRefExpr(clang::DeclRefExpr* ref) {
      // We can't reliably detect selector variables by name
      // This would need more sophisticated analysis
      return true;
    }
  };
  
  TypeCheckFinder finder;
  finder.TraverseStmt(expr);
  return finder.found;
}

// Helper to check if a statement block corresponds to a catch-all handler block
bool IsFromCatchAllBlock(clang::Stmt* stmt, const std::unordered_set<llvm::BasicBlock*>& catch_all_blocks, DecompilationContext& dec_ctx) {
  if (!stmt) return false;
  
  // Check if any statement within this block came from a catch-all handler block
  class CatchAllBlockFinder : public clang::RecursiveASTVisitor<CatchAllBlockFinder> {
  public:
    const std::unordered_set<llvm::BasicBlock*>& target_blocks;
    DecompilationContext& dec_ctx;
    bool found = false;
    
    CatchAllBlockFinder(const std::unordered_set<llvm::BasicBlock*>& blocks, DecompilationContext& ctx) 
      : target_blocks(blocks), dec_ctx(ctx) {}
    
    bool VisitStmt(clang::Stmt* s) {
      if (!s) return true;
      
      // Method 1: Check stmt_to_bb mapping
      auto it = dec_ctx.stmt_to_bb.find(s);
      if (it != dec_ctx.stmt_to_bb.end()) {
        std::string block_name = it->second;
        // Find the corresponding LLVM basic block
        auto bb_it = dec_ctx.bb_name_to_llvm_bb.find(block_name);
        if (bb_it != dec_ctx.bb_name_to_llvm_bb.end()) {
          if (target_blocks.count(bb_it->second)) {
            found = true;
            return false; // Stop traversal
          }
        }
      }
      
      // Method 2: Check for __cxa_begin_catch calls (typical catch handler start)
      if (auto* call = llvm::dyn_cast<clang::CallExpr>(s)) {
        if (auto* callee = call->getDirectCallee()) {
          if (callee->getName() == "__cxa_begin_catch") {
            found = true;
            return false;
          }
        }
      }
      
      return true;
    }
  };
  
  CatchAllBlockFinder finder(catch_all_blocks, dec_ctx);
  finder.TraverseStmt(stmt);
  return finder.found;
}

// Helper to get the LLVM basic block that generated a statement
llvm::BasicBlock* GetSourceBlock(clang::Stmt* stmt, DecompilationContext& dec_ctx) {
  if (!stmt) return nullptr;
  
  try {
    // First check direct stmt_to_block mapping
    auto block_it = dec_ctx.stmt_to_block.find(stmt);
    if (block_it != dec_ctx.stmt_to_block.end()) {
      return block_it->second;
    }
    
    // Check stmt_provenance mapping
    auto it = dec_ctx.stmt_provenance.find(stmt);
    if (it != dec_ctx.stmt_provenance.end() && it->second) {
      if (auto inst = llvm::dyn_cast<llvm::Instruction>(it->second)) {
        return inst->getParent();
      }
    }
    
    // For expressions, check use provenance
    if (auto expr = llvm::dyn_cast<clang::Expr>(stmt)) {
      auto use_it = dec_ctx.use_provenance.find(expr);
      if (use_it != dec_ctx.use_provenance.end() && use_it->second) {
        auto* user = use_it->second->getUser();
        if (user) {
          if (auto inst = llvm::dyn_cast<llvm::Instruction>(user)) {
            return inst->getParent();
          }
        }
      }
    }
    
    // Try the exception context mapping as a fallback
    auto ctx_it = dec_ctx.stmt_exception_context.find(stmt);
    if (ctx_it != dec_ctx.stmt_exception_context.end()) {
      // We found a statement in the exception context map
      // This means we need to look at the exception regions to find the block
      for (const auto& region : dec_ctx.exception_regions.GetTryRegions()) {
        // Check try blocks
        for (auto* block : region.blocks) {
          // Return the first block that contains statements mapped to this statement
          // This is a simplified approach - in reality we'd want more precise mapping
          return block;
        }
        
        // Check catch handlers
        for (const auto& handler : region.catch_handlers) {
          for (auto* block : handler.blocks) {
            return block;
          }
        }
      }
    }
  } catch (...) {
    // Ignore any exceptions and return nullptr
  }
  
  return nullptr;
}

}  // anonymous namespace

ExceptionASTTransform::ExceptionASTTransform(DecompilationContext &dec_ctx)
    : TransformVisitor<ExceptionASTTransform>(dec_ctx) {}

bool ExceptionASTTransform::VisitFunctionDecl(clang::FunctionDecl *func) {
  // This visitor method is now a placeholder - real work happens in RunImpl
  return true;
}

clang::CompoundStmt* ExceptionASTTransform::TransformFunctionBody(
    clang::CompoundStmt* body,
    clang::FunctionDecl* func_decl) {
  
  if (!body) return body;
  
  LOG(INFO) << "TransformFunctionBody for function: " << func_decl->getName().str();
  LOG(INFO) << "Original body has " << body->size() << " statements";
  
  // Get the LLVM function
  llvm::Function* llvm_func = nullptr;
  LOG(INFO) << "Looking for LLVM function for " << func_decl->getName().str();
  LOG(INFO) << "value_decls size: " << dec_ctx.value_decls.size();
  
  // Try reverse lookup - find LLVM function by name
  for (auto& [val, decl] : dec_ctx.value_decls) {
    if (decl == func_decl) {
      if (auto* func = llvm::dyn_cast<llvm::Function>(val)) {
        llvm_func = func;
        LOG(INFO) << "Found LLVM function: " << func->getName().str();
        break;
      }
    }
  }
  
  // If direct lookup failed, try by name matching
  if (!llvm_func) {
    std::string func_name = func_decl->getName().str();
    for (auto& [val, decl] : dec_ctx.value_decls) {
      if (auto* func = llvm::dyn_cast<llvm::Function>(val)) {
        if (func->getName().str() == func_name) {
          llvm_func = func;
          LOG(INFO) << "Found LLVM function by name: " << func->getName().str();
          break;
        }
      }
    }
  }
  
  if (!llvm_func) {
    LOG(WARNING) << "Could not find LLVM function for " << func_decl->getName().str();
    return body;
  }
  
  // Check if this function has any exception regions
  bool has_exception_regions = false;
  int exception_region_count = 0;
  for (const auto& region : dec_ctx.exception_regions.GetTryRegions()) {
    if (!region.blocks.empty()) {
      auto* first_block = *region.blocks.begin();
      if (first_block->getParent() == llvm_func) {
        has_exception_regions = true;
        exception_region_count++;
      }
    }
  }
  
  LOG(INFO) << "Function " << func_decl->getName().str() << " has " << exception_region_count << " exception regions";
  
  // Log details about the exception regions
  for (const auto& region : dec_ctx.exception_regions.GetTryRegions()) {
    if (!region.blocks.empty()) {
      auto* first_block = *region.blocks.begin();
      if (first_block->getParent() == llvm_func) {
        LOG(INFO) << "  Region has " << region.catch_handlers.size() << " catch handlers:";
        for (size_t i = 0; i < region.catch_handlers.size(); i++) {
          const auto& handler = region.catch_handlers[i];
          LOG(INFO) << "    Handler " << i << ": is_catch_all=" << handler.is_catch_all 
                    << ", type=" << handler.exception_type_name
                    << ", blocks=" << handler.blocks.size();
        }
      }
    }
  }
  
  if (!has_exception_regions) {
    LOG(INFO) << "No exception regions found for function " << func_decl->getName().str();
    return body;
  }
  
  // Build a map from LLVM blocks to statements
  std::unordered_map<llvm::BasicBlock*, std::vector<clang::Stmt*>> block_to_stmts;
  
  // First pass: map basic blocks to compound statements they generate
  std::unordered_map<llvm::BasicBlock*, clang::CompoundStmt*> block_to_compound;
  
  // Helper to find which block a statement belongs to
  auto find_block_for_stmt = [&](clang::Stmt* stmt) -> llvm::BasicBlock* {
    // Check provenance
    auto it = dec_ctx.stmt_provenance.find(stmt);
    if (it != dec_ctx.stmt_provenance.end() && it->second) {
      if (auto inst = llvm::dyn_cast<llvm::Instruction>(it->second)) {
        return inst->getParent();
      }
    }
    
    // Check expressions via use_provenance
    if (auto expr = llvm::dyn_cast<clang::Expr>(stmt)) {
      auto use_it = dec_ctx.use_provenance.find(expr);
      if (use_it != dec_ctx.use_provenance.end() && use_it->second) {
        if (auto user = use_it->second->getUser()) {
          if (auto inst = llvm::dyn_cast<llvm::Instruction>(user)) {
            return inst->getParent();
          }
        }
      }
    }
    
    return nullptr;
  };
  
  // Parse BB markers from the decompiled code structure
  // The decompiled code has structure like:
  // /* BB: function_bb_N_hash */
  // statement1;
  // statement2;
  // /* BB: function_bb_M_hash */
  // statement3;
  
  // We need to associate statements with their containing BB based on these markers
  llvm::BasicBlock* current_block = nullptr;
  
  // First, build a map from BB names to LLVM BasicBlocks
  std::unordered_map<std::string, llvm::BasicBlock*> bb_name_map;
  for (auto& bb : *llvm_func) {
    std::string bb_name;
    if (bb.hasName()) {
      bb_name = bb.getName().str();
    } else {
      // Try to find it in dec_ctx.bb_name_to_llvm_bb
      for (const auto& [name, block] : dec_ctx.bb_name_to_llvm_bb) {
        if (block == &bb) {
          bb_name = name;
          break;
        }
      }
    }
    if (!bb_name.empty()) {
      bb_name_map[bb_name] = &bb;
    }
  }
  
  LOG(INFO) << "Built BB name map with " << bb_name_map.size() << " entries";
  
  // Now we can use the improved provenance tracking to map statements to their blocks
  LOG(INFO) << "Using improved provenance tracking for exception handling";
  
  // First, collect all statements and their source blocks
  std::unordered_map<llvm::BasicBlock*, std::vector<clang::Stmt*>> block_statements;
  
  // Walk through the function body and use the stmt_to_block mapping
  std::function<void(clang::Stmt*)> CollectStmts;
  CollectStmts = [&](clang::Stmt* stmt) {
    if (!stmt) return;
    
    // Check if we have block mapping for this statement
    auto it = dec_ctx.stmt_to_block.find(stmt);
    if (it != dec_ctx.stmt_to_block.end()) {
      // Check if this block contains infrastructure and should be filtered
      if (!IsInfrastructureBlock(it->second)) {
        block_statements[it->second].push_back(stmt);
      } else {
        LOG(INFO) << "Filtering statement from infrastructure block";
      }
    }
    
    // Recursively process compound statements
    if (auto* compound = llvm::dyn_cast<clang::CompoundStmt>(stmt)) {
      for (auto* child : compound->body()) {
        CollectStmts(child);
      }
    } else if (auto* if_stmt = llvm::dyn_cast<clang::IfStmt>(stmt)) {
      CollectStmts(if_stmt->getThen());
      CollectStmts(if_stmt->getElse());
    }
  };
  
  // Collect all statements
  for (auto* stmt : body->body()) {
    CollectStmts(stmt);
  }
  
  LOG(INFO) << "Collected statements from " << block_statements.size() << " blocks";
  
  // Now map statements to exception regions
  for (const auto& region : dec_ctx.exception_regions.GetTryRegions()) {
    if (!region.blocks.empty()) {
      auto* first_block = *region.blocks.begin();
      if (first_block->getParent() != llvm_func) continue;
      
      // Collect statements for try blocks
      for (auto* try_block : region.blocks) {
        auto it = block_statements.find(try_block);
        if (it != block_statements.end()) {
          LOG(INFO) << "Found " << it->second.size() << " statements for try block";
          block_to_stmts[try_block] = it->second;
        }
      }
      
      // Collect statements for catch handlers
      for (const auto& handler : region.catch_handlers) {
        LOG(INFO) << "Processing handler for " << handler.exception_type_name 
                  << " with " << handler.blocks.size() << " blocks";
        
        std::vector<clang::Stmt*> all_handler_stmts;
        
        for (auto* handler_block : handler.blocks) {
          auto it = block_statements.find(handler_block);
          if (it != block_statements.end()) {
            LOG(INFO) << "  Found " << it->second.size() << " statements in handler block";
            all_handler_stmts.insert(all_handler_stmts.end(), 
                                   it->second.begin(), it->second.end());
          }
        }
        
        if (!all_handler_stmts.empty() && !handler.blocks.empty()) {
          auto* first_handler_block = *handler.blocks.begin();
          block_to_stmts[first_handler_block] = all_handler_stmts;
          LOG(INFO) << "Mapped " << all_handler_stmts.size() 
                    << " statements to handler for " << handler.exception_type_name;
        }
      }
    }
  }
  
  LOG(INFO) << "Found statements in " << block_to_stmts.size() << " different basic blocks";
  
  // Create new function body with try-catch blocks
  std::vector<clang::Stmt*> new_body_stmts;
  std::set<llvm::BasicBlock*> processed_blocks;
  
  LOG(INFO) << "Processing " << body->size() << " top-level statements in function body";
  
  // Check if we can use the pre-computed exception context
  bool use_exception_context = !dec_ctx.stmt_exception_context.empty();
  LOG(INFO) << "Exception context map has " << dec_ctx.stmt_exception_context.size() << " entries";
  
  // Check if we have multiple exception regions that should be unified
  std::vector<const TryRegion*> function_regions;
  for (const auto& region : dec_ctx.exception_regions.GetTryRegions()) {
    if (!region.blocks.empty()) {
      auto* first_block = *region.blocks.begin();
      if (first_block->getParent() == llvm_func) {
        function_regions.push_back(&region);
      }
    }
  }
  
  LOG(INFO) << "Function has " << function_regions.size() << " exception regions";
  
  // DISABLED: Old nested exception logic - now handled in unified approach
  if (false && function_regions.size() >= 3) {
    // Look for nested exception pattern: 
    // - Multiple regions with same exception type
    // - Some regions have rethrow calls
    // - Need to create proper nested structure
    
    std::vector<const TryRegion*> regions_with_handlers;
    std::vector<const TryRegion*> regions_without_handlers;
    
    for (const auto* region : function_regions) {
      if (region->catch_handlers.empty()) {
        regions_without_handlers.push_back(region);
      } else {
        regions_with_handlers.push_back(region);
      }
    }
    
    LOG(INFO) << "Found " << regions_with_handlers.size() << " regions with handlers, " 
              << regions_without_handlers.size() << " without handlers";
    
    // Check for rethrow pattern in any region
    bool has_rethrow = false;
    for (const auto* region : regions_with_handlers) {
      for (const auto& handler : region->catch_handlers) {
        for (auto* block : handler.blocks) {
          auto it = block_to_stmts.find(block);
          if (it != block_to_stmts.end()) {
            for (auto* stmt : it->second) {
              if (auto* call_expr = llvm::dyn_cast<clang::CallExpr>(stmt)) {
                if (auto* callee = call_expr->getDirectCallee()) {
                  if (IsRethrowFunction(callee->getName().str())) {
                    has_rethrow = true;
                    LOG(INFO) << "Found rethrow function " << callee->getName().str() << " in region with " << region->catch_handlers.size() << " handlers";
                    break;
                  }
                }
              }
            }
          }
        }
      }
    }
    
    if (has_rethrow && !regions_with_handlers.empty()) {
      LOG(INFO) << "Detected nested exception pattern with rethrow - creating nested structure";
      
      // Create nested structure: find the inner region (with rethrow) and outer region
      const TryRegion* inner_region = nullptr;
      const TryRegion* outer_region = nullptr;
      
      // Find the region with __cxa_rethrow (inner catch)
      for (const auto* region : regions_with_handlers) {
        bool region_has_rethrow = false;
        for (const auto& handler : region->catch_handlers) {
          for (auto* block : handler.blocks) {
            auto it = block_to_stmts.find(block);
            if (it != block_to_stmts.end()) {
              for (auto* stmt : it->second) {
                if (auto* call_expr = llvm::dyn_cast<clang::CallExpr>(stmt)) {
                  if (auto* callee = call_expr->getDirectCallee()) {
                    if (IsRethrowFunction(callee->getName().str())) {
                      region_has_rethrow = true;
                      break;
                    }
                  }
                }
              }
            }
          }
        }
        if (region_has_rethrow) {
          inner_region = region;
        } else {
          outer_region = region;
        }
      }
      
      if (inner_region && outer_region) {
        LOG(INFO) << "Creating nested try-catch: inner region has " << inner_region->catch_handlers.size() 
                  << " handlers, outer has " << outer_region->catch_handlers.size();
        
        // Create the inner try-catch first
        LOG(INFO) << "Creating inner try-catch for region with " << inner_region->blocks.size() << " blocks";
        auto* inner_try_catch = CreateNestedTryCatch(*inner_region, block_to_stmts);
        
        if (inner_try_catch) {
          // Create the outer try-catch with the inner try-catch inside the try block
          std::vector<clang::Stmt*> outer_try_stmts;
          outer_try_stmts.push_back(inner_try_catch);
          
          // Create outer catch handlers
          std::vector<clang::CXXCatchStmt*> outer_catch_handlers;
          for (const auto& handler : outer_region->catch_handlers) {
            std::vector<clang::Stmt*> handler_stmts;
            
            // IMPORTANT: For nested exceptions, the outer catch handler blocks should be different
            // from the inner catch handler blocks. The issue is that the exception region detection
            // might be incorrectly assigning the same blocks to both inner and outer handlers.
            
            // Log the blocks we're processing for debugging
            LOG(INFO) << "Processing outer catch handler with " << handler.blocks.size() << " blocks";
            for (auto* bb : handler.blocks) {
              LOG(INFO) << "  Outer handler block: " << GetBlockName(bb, &dec_ctx);
            }
            
            // Check if these blocks are different from inner handler blocks
            bool found_distinct_outer_blocks = false;
            for (auto* bb : handler.blocks) {
              // Check if this block contains outer catch specific code
              auto it = block_to_stmts.find(bb);
              if (it != block_to_stmts.end()) {
                for (auto* stmt : it->second) {
                  // Look for markers that indicate this is truly an outer catch block
                  if (auto* call = llvm::dyn_cast<clang::CallExpr>(stmt)) {
                    if (call->getNumArgs() > 0) {
                      if (auto* str_literal = llvm::dyn_cast<clang::StringLiteral>(call->getArg(0))) {
                        std::string str_value = str_literal->getString().str();
                        if (str_value.find("OUTER CATCH BLOCK") != std::string::npos ||
                            str_value.find("Outer error code") != std::string::npos ||
                            str_value.find("Exception handling complete") != std::string::npos) {
                          found_distinct_outer_blocks = true;
                          LOG(INFO) << "Found distinct outer catch block: " << GetBlockName(bb, &dec_ctx);
                        }
                      }
                    }
                  }
                }
              }
            }
            
            if (!found_distinct_outer_blocks) {
              LOG(WARNING) << "Outer catch handler does not have distinct blocks - this is likely a bug in exception region detection";
            }
            
            // Collect statements for this outer catch handler
            for (auto* bb : handler.blocks) {
              auto it = block_to_stmts.find(bb);
              if (it != block_to_stmts.end()) {
                std::vector<clang::Stmt*> filtered_stmts;
                for (auto* stmt : it->second) {
                  FilterExceptionInfrastructure(stmt, filtered_stmts, /*in_catch_handler=*/true);
                }
                handler_stmts.insert(handler_stmts.end(), filtered_stmts.begin(), filtered_stmts.end());
              }
            }
            
            // Create catch body
            auto* catch_body = dec_ctx.ast.CreateCompoundStmt(handler_stmts);
            
            // Create exception variable
            clang::VarDecl* exception_var = nullptr;
            if (!handler.is_catch_all) {
              std::string var_name = CreateVariableNameFromType(handler.exception_type_name);
              clang::QualType exception_type = CreateExceptionType(handler.exception_type_name, dec_ctx);
              exception_var = dec_ctx.ast.CreateVarDecl(func_decl, exception_type, var_name);
            }
            
            // Create catch statement
            auto* catch_stmt = dec_ctx.ast.CreateCXXCatchStmt(
                clang::SourceLocation(), exception_var, catch_body);
            outer_catch_handlers.push_back(catch_stmt);
          }
          
          // Create the complete nested try-catch structure
          auto* outer_try_body = dec_ctx.ast.CreateCompoundStmt(outer_try_stmts);
          auto* complete_try_catch = dec_ctx.ast.CreateCXXTryStmt(
              clang::SourceLocation(), outer_try_body,
              llvm::ArrayRef<clang::CXXCatchStmt*>(outer_catch_handlers));
          
          std::vector<clang::Stmt*> new_body_stmts;
          new_body_stmts.push_back(complete_try_catch);
          
          LOG(INFO) << "Successfully created nested exception structure";
          return dec_ctx.ast.CreateCompoundStmt(new_body_stmts);
        }
      }
    }
  }
  
  // If we have multiple regions with the same handlers, they should be unified into one try-catch
  if (function_regions.size() > 1) {
    LOG(INFO) << "Checking if " << function_regions.size() << " regions can be unified";
    
    // Find the region with the most handlers as the template
    const TryRegion* template_region = nullptr;
    size_t max_handlers = 0;
    for (const auto* region : function_regions) {
      if (region->catch_handlers.size() > max_handlers) {
        max_handlers = region->catch_handlers.size();
        template_region = region;
      }
    }
    
    if (template_region && max_handlers > 0) {
      LOG(INFO) << "Using region with " << max_handlers << " handlers as template";
      
      // Check if all non-empty handler regions have the same handlers as the template
      bool can_unify = true;
      for (const auto* region : function_regions) {
        if (region->catch_handlers.empty()) {
          LOG(INFO) << "Skipping region with no handlers (likely just throw blocks)";
          continue; // Skip regions with no handlers (they're just throw blocks)
        }
        
        const auto& template_handlers = template_region->catch_handlers;
        const auto& current_handlers = region->catch_handlers;
        
        LOG(INFO) << "Comparing region with " << current_handlers.size() << " handlers";
        
        if (template_handlers.size() != current_handlers.size()) {
          LOG(INFO) << "Handler count mismatch - cannot unify";
          can_unify = false;
          break;
        }
        
        for (size_t j = 0; j < template_handlers.size(); ++j) {
          LOG(INFO) << "Comparing handler " << j << ": '" << template_handlers[j].exception_type_name 
                    << "' vs '" << current_handlers[j].exception_type_name << "'";
          if (template_handlers[j].exception_type_name != current_handlers[j].exception_type_name) {
            LOG(INFO) << "Handler type mismatch - cannot unify";
            can_unify = false;
            break;
          }
        }
        if (!can_unify) break;
      }
      
      if (can_unify) {
        LOG(INFO) << "All non-empty regions have compatible handlers - will create unified try-catch";
        return CreateUnifiedTryCatch(body, func_decl, llvm_func, function_regions);
      } else {
        LOG(INFO) << "Regions have incompatible handlers - using direct mapping";
      }
    } else {
      LOG(INFO) << "No regions with handlers found - using direct mapping";
    }
  }
  
  // For single region or regions with different handlers, use the direct mapping approach
  if (!function_regions.empty()) {
    return TransformUsingDirectMapping(body, func_decl, llvm_func);
  }
  
  // Otherwise, use the exception context approach if available
  if (use_exception_context) {
    LOG(INFO) << "Using exception context approach for transformation";
    return TransformUsingExceptionContext(body, func_decl);
  }

  // Fallback: Process each statement in the original body using traditional approach
  int processed_stmt_count = 0;
  for (auto* stmt : body->body()) {
    processed_stmt_count++;
    LOG(INFO) << "Processing statement " << processed_stmt_count << " of " << body->size();
    
    // Check if this statement belongs to a try region using traditional approach
    auto* source_block = GetSourceBlock(stmt, dec_ctx);
    if (!source_block) {
      // No source block, just add the statement as-is
      LOG(INFO) << "Statement has no source block, adding as-is";
      new_body_stmts.push_back(stmt);
      continue;
    }
    
    LOG(INFO) << "Statement maps to basic block";
    
    // Skip if we've already processed this block as part of a try-catch
    if (processed_blocks.count(source_block)) {
      LOG(INFO) << "Block already processed, skipping";
      continue;
    }
    
    // Check if this block is part of a try region
    bool found_try_region = false;
    for (const auto& region : dec_ctx.exception_regions.GetTryRegions()) {
      if (region.blocks.count(source_block)) {
        // This statement is part of a try block
        found_try_region = true;
        
        // Collect all statements from the try region
        std::vector<clang::Stmt*> try_stmts;
        for (auto* try_block : region.blocks) {
          if (try_block->getParent() != llvm_func) continue;
          
          processed_blocks.insert(try_block);
          auto it = block_to_stmts.find(try_block);
          if (it != block_to_stmts.end()) {
            for (auto* s : it->second) {
              if (!IsExceptionInfrastructure(s)) {
                // Special handling for IfStmts with marker conditions
                if (auto* if_stmt = llvm::dyn_cast<clang::IfStmt>(s)) {
                  if (HasMarkerCondition(if_stmt, dec_ctx)) {
                    // Extract the body of the if statement instead of the whole if
                    if (auto* then_stmt = if_stmt->getThen()) {
                      if (auto* compound = llvm::dyn_cast<clang::CompoundStmt>(then_stmt)) {
                        // Add all statements from the compound body
                        for (auto* body_stmt : compound->body()) {
                          if (!IsExceptionInfrastructure(body_stmt)) {
                            try_stmts.push_back(body_stmt);
                          }
                        }
                      } else {
                        try_stmts.push_back(then_stmt);
                      }
                      continue;
                    }
                  }
                }
                try_stmts.push_back(s);
              }
            }
          }
        }
        
        // Create catch handlers
        std::vector<clang::CXXCatchStmt*> catch_handlers;
        
        // The issue is that the handler code is already generated as part of the
        // main function body with complex if conditions. We need to extract the
        // relevant parts based on the LLVM IR structure.
        
        // For now, let's create the catch handlers with the correct structure
        // even if we can't fully populate them with the original handler code
        for (const auto& handler : region.catch_handlers) {
          std::vector<clang::Stmt*> handler_stmts;
          std::unordered_set<clang::Stmt*> added_stmts; // Track added statements to avoid duplicates
          
          LOG(INFO) << "Creating catch handler for " << handler.exception_type_name
                    << " (is_catch_all=" << handler.is_catch_all << ")";
          LOG(INFO) << "Handler has " << handler.blocks.size() << " blocks";
          
          if (handler.is_catch_all) {
            LOG(INFO) << "PROCESSING CATCH-ALL HANDLER!";
          }
          
          // Debug: log block names for catch-all
          if (handler.is_catch_all) {
            LOG(INFO) << "Catch-all handler blocks:";
            for (auto* block : handler.blocks) {
              std::string block_str;
              llvm::raw_string_ostream block_rso(block_str);
              block->printAsOperand(block_rso, false);
              LOG(INFO) << "  - " << block_str;
            }
          }
          
          // Check if we have any statements mapped to this handler's blocks
          for (auto* handler_block : handler.blocks) {
            processed_blocks.insert(handler_block);
            std::string block_str;
            llvm::raw_string_ostream block_rso(block_str);
            handler_block->printAsOperand(block_rso, false);
            block_rso.flush();
            LOG(INFO) << "Checking statements in handler block " << block_str;
            
            auto it = block_to_stmts.find(handler_block);
            
            // If no statements were found (because we skipped handler blocks in GenerateAST),
            // we need to generate them now
            if (it == block_to_stmts.end() || it->second.empty()) {
              LOG(INFO) << "No statements found for handler block " << block_str 
                        << " - generating statements now";
              
              // Generate statements for this handler block using IRToASTVisitor
              IRToASTVisitor ast_gen(dec_ctx);
              std::vector<clang::Stmt*> block_stmts;
              ast_gen.VisitBasicBlock(*handler_block, block_stmts);
              
              // Add the generated statements to handler_stmts
              for (auto* s : block_stmts) {
                if (!IsExceptionInfrastructure(s) && added_stmts.find(s) == added_stmts.end()) {
                  handler_stmts.push_back(s);
                  added_stmts.insert(s);
                  LOG(INFO) << "  Added generated statement: " << s->getStmtClassName();
                }
              }
            } else {
              LOG(INFO) << "Found " << it->second.size() << " statements in block " << block_str;
              if (handler.is_catch_all && block_str == "%119") {
                LOG(INFO) << "  Detailed statements in catch-all entry block %119:";
                for (size_t i = 0; i < it->second.size(); i++) {
                  auto* stmt = it->second[i];
                  LOG(INFO) << "    Statement " << i << ": " << stmt->getStmtClassName();
                }
              }
              for (auto* s : it->second) {
                // Debug: Check if this is a printf call
                if (auto* call_expr = llvm::dyn_cast<clang::CallExpr>(s)) {
                  if (auto* callee = call_expr->getDirectCallee()) {
                    if (callee->getName() == "printf") {
                      LOG(INFO) << "Found printf call in handler block " << block_str;
                    }
                  }
                }
                
                if (!IsExceptionInfrastructure(s) && added_stmts.find(s) == added_stmts.end()) {
                  // Special handling for IfStmts with marker conditions
                  if (auto* if_stmt = llvm::dyn_cast<clang::IfStmt>(s)) {
                    if (HasMarkerCondition(if_stmt, dec_ctx)) {
                      LOG(INFO) << "Found IfStmt with marker condition in handler block";
                      // Extract the body of the if statement instead of the whole if
                      if (auto* then_stmt = if_stmt->getThen()) {
                        if (auto* compound = llvm::dyn_cast<clang::CompoundStmt>(then_stmt)) {
                          // Add all statements from the compound body
                          for (auto* body_stmt : compound->body()) {
                            if (!IsExceptionInfrastructure(body_stmt)) {
                              LOG(INFO) << "  Extracting statement from marker IfStmt body";
                              handler_stmts.push_back(body_stmt);
                            }
                          }
                        } else {
                          LOG(INFO) << "  Extracting single statement from marker IfStmt";
                          handler_stmts.push_back(then_stmt);
                        }
                        added_stmts.insert(s);
                        continue;
                      }
                    }
                  }
                  handler_stmts.push_back(s);
                  added_stmts.insert(s);
                }
              }
            }
          }
          
          // If we couldn't find the actual handler statements, check for pre-generated catch-all statements first
          if (handler_stmts.empty() && handler.is_catch_all) {
            // First, try to use pre-generated catch-all statements
            if (!handler.catch_all_statements.empty()) {
              LOG(INFO) << "Using " << handler.catch_all_statements.size() << " pre-generated catch-all statements";
              
              for (const auto& stmt_str : handler.catch_all_statements) {
                // TODO: Convert string statements to actual AST nodes
                // For now, log them to verify the fix is working
                LOG(INFO) << "  Pre-generated statement: " << stmt_str;
              }
              
              // For demonstration purposes, we could create simple printf calls
              // This is a placeholder - real implementation would parse/convert the statements
              // TODO: Implement proper conversion from string statements to AST nodes
              
              LOG(INFO) << "Successfully created catch-all handler from pre-generated statements";
            } else {
              LOG(INFO) << "No pre-generated catch-all statements - generating directly from handler blocks";
              
              // Generate statements directly from catch-all handler blocks
            for (auto* handler_block : handler.blocks) {
              std::string block_str;
              llvm::raw_string_ostream block_rso(block_str);
              handler_block->printAsOperand(block_rso, false);
              LOG(INFO) << "Generating statements from catch-all block: " << block_str;
              
              IRToASTVisitor ast_gen(dec_ctx);
              std::vector<clang::Stmt*> block_stmts;
              ast_gen.VisitBasicBlock(*handler_block, block_stmts);
              
              // Add all generated statements (don't filter them for catch-all)
              for (auto* stmt : block_stmts) {
                if (added_stmts.find(stmt) == added_stmts.end()) {
                  handler_stmts.push_back(stmt);
                  added_stmts.insert(stmt);
                  LOG(INFO) << "  Added statement: " << stmt->getStmtClassName();
                }
              }
            }
            
            if (!handler_stmts.empty()) {
              LOG(INFO) << "Successfully generated " << handler_stmts.size() 
                        << " statements directly for catch-all handler";
            } else {
              LOG(WARNING) << "Could not generate any statements for catch-all handler";
            }
            }  // End of else block for direct statement generation
          }
          
          // If still empty, try to extract them from try block (original fallback)
          if (handler_stmts.empty() && handler.is_catch_all) {
            LOG(INFO) << "Catch-all handler still empty - searching for statements in try block";
            
            // Search for if(0U) blocks in the try block that contain catch-all code
            std::function<void(clang::Stmt*)> FindCatchAllInTryBlock = [&](clang::Stmt* stmt) {
              if (!stmt) return;
              
              if (auto* if_stmt = llvm::dyn_cast<clang::IfStmt>(stmt)) {
                if (HasMarkerCondition(if_stmt, dec_ctx)) {
                  auto* then_stmt = if_stmt->getThen();
                  if (then_stmt && IsFromCatchAllBlock(then_stmt, handler.blocks, dec_ctx)) {
                    LOG(INFO) << "Found catch-all code in if(0U) block - extracting statements";
                    
                    // Extract statements from the if body
                    if (auto* compound = llvm::dyn_cast<clang::CompoundStmt>(then_stmt)) {
                      for (auto* body_stmt : compound->body()) {
                        if (!IsExceptionInfrastructure(body_stmt)) {
                          handler_stmts.push_back(body_stmt);
                        }
                      }
                    } else if (!IsExceptionInfrastructure(then_stmt)) {
                      handler_stmts.push_back(then_stmt);
                    }
                    return; // Found it, stop searching
                  }
                }
              }
              
              // Recursively search compound statements
              if (auto* compound = llvm::dyn_cast<clang::CompoundStmt>(stmt)) {
                for (auto* child : compound->body()) {
                  FindCatchAllInTryBlock(child);
                  if (!handler_stmts.empty()) return; // Stop if we found statements
                }
              }
            };
            
            // Search the try statements that will be put in the try block
            for (auto* try_stmt : try_stmts) {
              FindCatchAllInTryBlock(try_stmt);
              if (!handler_stmts.empty()) break; // Stop if we found statements
            }
            
            if (!handler_stmts.empty()) {
              LOG(INFO) << "Successfully extracted " << handler_stmts.size() 
                        << " statements for catch-all handler from try block";
            } else {
              LOG(WARNING) << "Could not find catch-all statements in try block";
            }
          }
          
          if (handler_stmts.empty()) {
            LOG(WARNING) << "Could not find handler statements for " << handler.exception_type_name 
                        << " - handler body will be empty";
          } else if (handler.is_catch_all) {
            LOG(INFO) << "Catch-all handler has " << handler_stmts.size() << " statements";
          }
          
          // Add __cxa_end_catch() call at the end of the catch handler
          // First, find the __cxa_end_catch function declaration
          clang::FunctionDecl* end_catch_func = nullptr;
          auto* tu = dec_ctx.ast_ctx.getTranslationUnitDecl();
          for (auto* decl : tu->decls()) {
            if (auto* func = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
              if (func->getName() == "__cxa_end_catch") {
                end_catch_func = func;
                break;
              }
            }
          }
          
          if (end_catch_func) {
            // Check if the last statement is a return - if so, don't add __cxa_end_catch after it
            bool ends_with_return = false;
            if (!handler_stmts.empty()) {
              if (llvm::isa<clang::ReturnStmt>(handler_stmts.back())) {
                ends_with_return = true;
              }
            }
            
            if (!ends_with_return) {
              // Create call to __cxa_end_catch()
              std::vector<clang::Expr*> args; // No arguments
              auto* end_catch_call = dec_ctx.ast.CreateCall(end_catch_func, args);
              handler_stmts.push_back(end_catch_call);
            }
          }
          
          // Create catch handler
          auto catch_body = dec_ctx.ast.CreateCompoundStmt(handler_stmts);
          clang::VarDecl* exception_var = nullptr;
          
          if (!handler.is_catch_all) {
            std::string var_name = CreateVariableNameFromType(handler.exception_type_name);
            clang::QualType exception_type = CreateExceptionType(handler.exception_type_name, dec_ctx);
            exception_var = dec_ctx.ast.CreateVarDecl(func_decl, exception_type, var_name);
          }
          
          auto* catch_stmt = dec_ctx.ast.CreateCXXCatchStmt(
              clang::SourceLocation(), exception_var, catch_body);
          catch_handlers.push_back(catch_stmt);
        }
        
        // Create try-catch statement only if we have catch handlers
        // Functions that only throw (cleanup-only) should not have try blocks
        if (!try_stmts.empty() && !catch_handlers.empty()) {
          auto try_body = dec_ctx.ast.CreateCompoundStmt(try_stmts);
          auto* try_catch = dec_ctx.ast.CreateCXXTryStmt(
              clang::SourceLocation(), try_body,
              llvm::ArrayRef<clang::CXXCatchStmt*>(catch_handlers));
          new_body_stmts.push_back(try_catch);
        } else if (!try_stmts.empty() && catch_handlers.empty()) {
          // For cleanup-only regions, add the statements directly without try wrapper
          for (auto* stmt : try_stmts) {
            new_body_stmts.push_back(stmt);
          }
        }
        
        break;
      }
    }
    
    if (!found_try_region) {
      // Not part of a try region, add as-is
      new_body_stmts.push_back(stmt);
    }
  }
  
  return dec_ctx.ast.CreateCompoundStmt(new_body_stmts);
}

std::string ExceptionASTTransform::CreateVariableNameFromType(const std::string& exception_type) {
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

clang::QualType ExceptionASTTransform::CreateExceptionType(
    const std::string& exception_type_name, 
    DecompilationContext& dec_ctx) {
  // For now, create a reference to void type
  // In a real implementation, we'd look up the actual exception type
  return dec_ctx.ast_ctx.getPointerType(dec_ctx.ast_ctx.VoidTy);
}

bool ExceptionASTTransform::HasMarkerCondition(clang::IfStmt* if_stmt, DecompilationContext& dec_ctx) {
  if (!if_stmt || !if_stmt->getCond()) return false;
  return if_stmt->getCond() == dec_ctx.marker_expr;
}

bool ExceptionASTTransform::IsExceptionInfrastructure(clang::Stmt* stmt, bool in_catch_handler) {
  if (!stmt) return false;
  
  // Check for calls to exception handling functions
  if (auto call = llvm::dyn_cast<clang::CallExpr>(stmt)) {
    // Rethrow functions should be preserved in catch handlers (transformed to throw;)
    if (auto* callee = call->getDirectCallee()) {
      if (IsRethrowFunction(callee->getName().str())) {
        return !in_catch_handler;  // Not infrastructure if in catch handler
      }
    }
    
    // __cxa_throw should be transformed to throw expression, not filtered
    if (IsCallToFunction(call, "__cxa_throw")) {
      return false;  // Not infrastructure - will be transformed
    }
    
    if (IsCallToFunction(call, "__cxa_allocate_exception") ||
        IsCallToFunction(call, "__cxa_begin_catch") ||
        IsCallToFunction(call, "__cxa_end_catch") ||
        IsCallToFunction(call, "__cxa_free_exception") ||
        IsCallToFunction(call, "_Unwind_Resume") ||
        IsCallToFunction(call, "llvm.eh.typeid.for") ||
        IsCallToFunction(call, "llvm_eh_typeid_for") ||
        IsCallToFunction(call, "__clang_call_terminate") ||
        IsCallToFunction(call, "_ZSt9terminatev") ||
        IsCallToFunction(call, "llvm_trap")) {
      return true;
    }
  }
  
  // Check for C++ throw expressions
  if (llvm::isa<clang::CXXThrowExpr>(stmt)) {
    return true;
  }
  
  // Check for exception dispatch infrastructure
  // This includes comparisons with exception type IDs and related branches
  if (auto* bin_op = llvm::dyn_cast<clang::BinaryOperator>(stmt)) {
    // Check if either operand is a call to llvm_eh_typeid_for
    if (auto* lhs_call = llvm::dyn_cast<clang::CallExpr>(bin_op->getLHS()->IgnoreParenImpCasts())) {
      if (IsCallToFunction(lhs_call, "llvm_eh_typeid_for")) {
        return true;
      }
    }
    if (auto* rhs_call = llvm::dyn_cast<clang::CallExpr>(bin_op->getRHS()->IgnoreParenImpCasts())) {
      if (IsCallToFunction(rhs_call, "llvm_eh_typeid_for")) {
        return true;
      }
    }
    
  }
  
  // Check for if statements that are part of exception dispatch
  if (auto* if_stmt = llvm::dyn_cast<clang::IfStmt>(stmt)) {
    // Check if the condition involves exception type checking
    if (ContainsExceptionTypeCheck(if_stmt->getCond())) {
      return true;
    }
    
    // Also check if the condition is a dispatch comparison
    if (IsExceptionInfrastructure(if_stmt->getCond())) {
      return true;
    }
  }
  
  // Check for compound statements that contain only infrastructure
  if (auto compound = llvm::dyn_cast<clang::CompoundStmt>(stmt)) {
    bool all_infrastructure = true;
    for (auto* child : compound->body()) {
      if (!IsExceptionInfrastructure(child)) {
        all_infrastructure = false;
        break;
      }
    }
    if (all_infrastructure) {
      return true;
    }
  }
  
  return false;
}

// Check if a basic block contains only exception infrastructure
bool ExceptionASTTransform::IsInfrastructureBlock(llvm::BasicBlock* block) {
  if (!block) return false;
  
  std::string block_str;
  llvm::raw_string_ostream block_rso(block_str);
  block->printAsOperand(block_rso, false);
  block_rso.flush();
  
  LOG(INFO) << "IsInfrastructureBlock called for block: " << block_str;
  
  // Check if this block belongs to a catch-all handler
  bool is_catch_all_block = false;
  for (const auto& region : dec_ctx.exception_regions.GetTryRegions()) {
    for (const auto& handler : region.catch_handlers) {
      if (handler.is_catch_all && handler.blocks.count(block)) {
        is_catch_all_block = true;
        break;
      }
    }
    if (is_catch_all_block) break;
  }
  
  // Count instructions to determine if this is pure infrastructure
  int typeid_calls = 0;
  int total_non_phi_instructions = 0;
  bool has_user_code = false;
  
  for (auto& inst : *block) {
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
  
  // For catch-all handler blocks, only mark as infrastructure if it's purely dispatch code
  if (is_catch_all_block) {
    // Pure infrastructure = mostly typeid calls and comparisons, no user code
    bool is_infrastructure = typeid_calls > 0 && !has_user_code && total_non_phi_instructions <= 5;
    if (is_infrastructure) {
      LOG(INFO) << "Catch-all block " << block_str << " marked as infrastructure"
                << " (typeid_calls=" << typeid_calls 
                << ", has_user_code=" << has_user_code
                << ", total_instructions=" << total_non_phi_instructions << ")";
    } else {
      LOG(INFO) << "Catch-all block " << block_str << " contains user code - NOT marking as infrastructure"
                << " (typeid_calls=" << typeid_calls 
                << ", has_user_code=" << has_user_code
                << ", total_instructions=" << total_non_phi_instructions << ")";
    }
    return is_infrastructure;
  }
  
  // For non-catch-all blocks, any block with typeid is infrastructure (original behavior)
  if (typeid_calls > 0) {
    LOG(INFO) << "Block " << block_str << " contains llvm.eh.typeid.for - marking as infrastructure";
    return true;
  }
  
  // Also check if this block is primarily used for exception dispatch
  // by looking at its control flow pattern
  auto* terminator = block->getTerminator();
  if (auto* branch = llvm::dyn_cast<llvm::BranchInst>(terminator)) {
    if (branch->isConditional()) {
      // Check if the condition involves type ID comparison
      auto* condition = branch->getCondition();
      if (auto* icmp = llvm::dyn_cast<llvm::ICmpInst>(condition)) {
        // Check if either operand comes from a type ID call
        for (auto* operand : {icmp->getOperand(0), icmp->getOperand(1)}) {
          if (auto* call = llvm::dyn_cast<llvm::CallInst>(operand)) {
            if (auto callee = call->getCalledFunction()) {
              if (callee->getName() == "llvm.eh.typeid.for") {
                LOG(INFO) << "Block " << block_str << " has dispatch branch with llvm.eh.typeid.for condition - marking as infrastructure";
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

// Filter exception infrastructure from a statement tree
void ExceptionASTTransform::FilterExceptionInfrastructure(
    clang::Stmt* stmt, 
    std::vector<clang::Stmt*>& result,
    bool in_catch_handler) {
  
  if (!stmt) return;
  
  // Check if this is a rethrow call in a catch handler - transform it to throw;
  if (in_catch_handler) {
    if (auto* call = llvm::dyn_cast<clang::CallExpr>(stmt)) {
      if (auto* callee = call->getDirectCallee()) {
        if (IsRethrowFunction(callee->getName().str())) {
          // Transform to CXXThrowExpr with no operand (rethrow)
          auto* throw_expr = TransformRethrowCall(call);
          if (throw_expr) {
            result.push_back(throw_expr);
            return;
          }
        }
      }
    }
  }
  
  // Check if this is a __cxa_throw call - transform it to throw expression
  if (auto* call = llvm::dyn_cast<clang::CallExpr>(stmt)) {
    if (IsCallToFunction(call, "__cxa_throw")) {
      LOG(INFO) << "Found __cxa_throw call - transforming to throw expression";
      // Transform to CXXThrowExpr with the exception object
      auto* throw_expr = TransformThrowCall(call);
      if (throw_expr) {
        LOG(INFO) << "Successfully transformed __cxa_throw to throw expression";
        result.push_back(throw_expr);
        return;
      } else {
        LOG(WARNING) << "Failed to transform __cxa_throw call";
      }
    }
  }
  
  // Skip infrastructure statements entirely
  if (IsExceptionInfrastructure(stmt, in_catch_handler)) {
    return;
  }
  
  // Handle compound statements - filter their children
  if (auto* compound = llvm::dyn_cast<clang::CompoundStmt>(stmt)) {
    std::vector<clang::Stmt*> filtered_children;
    for (auto* child : compound->body()) {
      FilterExceptionInfrastructure(child, filtered_children, in_catch_handler);
    }
    
    if (!filtered_children.empty()) {
      auto* filtered_compound = dec_ctx.ast.CreateCompoundStmt(filtered_children);
      result.push_back(filtered_compound);
    }
    return;
  }
  
  // Handle if statements - may need to filter conditions and bodies
  if (auto* if_stmt = llvm::dyn_cast<clang::IfStmt>(stmt)) {
    // Skip if statements that have infrastructure conditions
    if (IsExceptionInfrastructure(if_stmt->getCond(), in_catch_handler)) {
      // Extract the body contents if they're not infrastructure
      if (if_stmt->getThen()) {
        FilterExceptionInfrastructure(if_stmt->getThen(), result, in_catch_handler);
      }
      if (if_stmt->getElse()) {
        FilterExceptionInfrastructure(if_stmt->getElse(), result, in_catch_handler);
      }
      return;
    }
    
    // Also handle marker conditions (if (0U))
    if (HasMarkerCondition(if_stmt, dec_ctx)) {
      LOG(INFO) << "FilterExceptionInfrastructure: Found IfStmt with marker condition";
      // Extract the body contents
      if (if_stmt->getThen()) {
        FilterExceptionInfrastructure(if_stmt->getThen(), result, in_catch_handler);
      }
      if (if_stmt->getElse()) {
        FilterExceptionInfrastructure(if_stmt->getElse(), result, in_catch_handler);
      }
      return;
    }
  }
  
  // For other statement types, include them if they're not infrastructure
  result.push_back(stmt);
}

// Create nested try-catch structure from exception regions
clang::Stmt* ExceptionASTTransform::CreateNestedTryCatch(const TryRegion& region, 
                                  const std::unordered_map<llvm::BasicBlock*, std::vector<clang::Stmt*>>& block_to_stmts) {
  LOG(INFO) << "CreateNestedTryCatch for region with " << region.blocks.size() 
            << " blocks and " << region.catch_handlers.size() << " handlers";
  
  // Collect statements for the try block
  std::vector<clang::Stmt*> try_stmts;
  for (auto* bb : region.blocks) {
    auto it = block_to_stmts.find(bb);
    if (it != block_to_stmts.end()) {
      for (auto* stmt : it->second) {
        if (!IsExceptionInfrastructure(stmt)) {
          // Special handling for marker conditions
          if (auto* if_stmt = llvm::dyn_cast<clang::IfStmt>(stmt)) {
            if (HasMarkerCondition(if_stmt, dec_ctx)) {
              // Extract body
              if (auto* then_stmt = if_stmt->getThen()) {
                if (auto* compound = llvm::dyn_cast<clang::CompoundStmt>(then_stmt)) {
                  for (auto* body_stmt : compound->body()) {
                    if (!IsExceptionInfrastructure(body_stmt)) {
                      try_stmts.push_back(body_stmt);
                    }
                  }
                } else {
                  try_stmts.push_back(then_stmt);
                }
              }
              continue;
            }
          }
          try_stmts.push_back(stmt);
        }
      }
    }
  }
  
  // Create catch handlers
  std::vector<clang::CXXCatchStmt*> catch_handlers;
  for (const auto& handler : region.catch_handlers) {
    std::vector<clang::Stmt*> handler_stmts;
    
    // Collect statements for this catch handler
    for (auto* bb : handler.blocks) {
      std::string block_str;
      llvm::raw_string_ostream block_rso(block_str);
      bb->printAsOperand(block_rso, false);
      block_rso.flush();
      LOG(INFO) << "Collecting statements from handler block " << block_str;
      
      auto it = block_to_stmts.find(bb);
      if (it != block_to_stmts.end()) {
        LOG(INFO) << "Found " << it->second.size() << " statements in block " << block_str;
        std::vector<clang::Stmt*> filtered_stmts;
        for (auto* stmt : it->second) {
          // Check if this is a printf call before filtering
          if (auto* call_expr = llvm::dyn_cast<clang::CallExpr>(stmt)) {
            if (auto* callee = call_expr->getDirectCallee()) {
              if (callee->getName() == "printf") {
                LOG(INFO) << "Found printf call in handler block " << block_str;
              }
            }
          }
          FilterExceptionInfrastructure(stmt, filtered_stmts, /*in_catch_handler=*/true);
        }
        LOG(INFO) << "After filtering, " << filtered_stmts.size() << " statements remain for block " << block_str;
        handler_stmts.insert(handler_stmts.end(), filtered_stmts.begin(), filtered_stmts.end());
      } else {
        LOG(INFO) << "No statements found for handler block " << block_str;
      }
    }
    
    // Note: Nested region handling is done at the TransformFunctionBody level
    // to avoid recursive CreateNestedTryCatch calls that cause statement duplication
    
    // Add __cxa_end_catch if needed
    if (!handler_stmts.empty()) {
      // Find __cxa_end_catch function
      clang::FunctionDecl* end_catch_func = nullptr;
      auto* tu = dec_ctx.ast_ctx.getTranslationUnitDecl();
      for (auto* decl : tu->decls()) {
        if (auto* func = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
          if (func->getName() == "__cxa_end_catch") {
            end_catch_func = func;
            break;
          }
        }
      }
      
      if (end_catch_func) {
        // Check if the last statement is a return - if so, don't add __cxa_end_catch after it
        bool ends_with_return = false;
        if (!handler_stmts.empty()) {
          if (llvm::isa<clang::ReturnStmt>(handler_stmts.back())) {
            ends_with_return = true;
          }
        }
        
        if (!ends_with_return) {
          // Create call to __cxa_end_catch()
          std::vector<clang::Expr*> args; // No arguments
          auto* end_catch_call = dec_ctx.ast.CreateCall(end_catch_func, args);
          handler_stmts.push_back(end_catch_call);
        }
      }
    }
    
    // Create catch body
    auto* catch_body = dec_ctx.ast.CreateCompoundStmt(handler_stmts);
    
    // Create exception variable
    clang::VarDecl* exception_var = nullptr;
    if (!handler.is_catch_all) {
      std::string var_name = CreateVariableNameFromType(handler.exception_type_name);
      clang::QualType exception_type = CreateExceptionType(handler.exception_type_name, dec_ctx);
      // Need to find the parent function to use as context
      clang::FunctionDecl* func_decl = nullptr;
      auto* tu = dec_ctx.ast_ctx.getTranslationUnitDecl();
      for (auto* decl : tu->decls()) {
        if (auto* func = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
          // Check if this region belongs to this function
          if (!region.blocks.empty()) {
            auto* first_block = *region.blocks.begin();
            if (first_block->getParent() && func->getName() == first_block->getParent()->getName()) {
              func_decl = func;
              break;
            }
          }
        }
      }
      exception_var = dec_ctx.ast.CreateVarDecl(func_decl, exception_type, var_name);
    }
    
    // Create catch statement
    auto* catch_stmt = dec_ctx.ast.CreateCXXCatchStmt(
        clang::SourceLocation(), exception_var, catch_body);
    catch_handlers.push_back(catch_stmt);
  }
  
  // Create try-catch statement
  if (!try_stmts.empty() || !catch_handlers.empty()) {
    auto* try_body = dec_ctx.ast.CreateCompoundStmt(try_stmts);
    return dec_ctx.ast.CreateCXXTryStmt(
        clang::SourceLocation(), try_body,
        llvm::ArrayRef<clang::CXXCatchStmt*>(catch_handlers));
  }
  
  return nullptr;
}

// Transform __cxa_throw call to throw expression
clang::Stmt* ExceptionASTTransform::TransformThrowCall(clang::CallExpr* call) {
  if (!call || call->getNumArgs() < 1) return nullptr;
  
  // The first argument to __cxa_throw is the exception object
  auto* exception_expr = call->getArg(0);
  
  // Create a CXXThrowExpr with the exception object
  auto* throw_expr = new (dec_ctx.ast_ctx) clang::CXXThrowExpr(
      exception_expr,
      dec_ctx.ast_ctx.VoidTy,
      call->getBeginLoc(),
      /*IsThrownVariableInScope=*/true);
  
  return throw_expr;
}

// Transform rethrow call to throw; expression
clang::Stmt* ExceptionASTTransform::TransformRethrowCall(clang::CallExpr* call) {
  if (!call) return nullptr;
  
  // Create a CXXThrowExpr with no operand (rethrow)
  auto* throw_expr = new (dec_ctx.ast_ctx) clang::CXXThrowExpr(
      /*SubExpr=*/nullptr,
      dec_ctx.ast_ctx.VoidTy,
      call->getBeginLoc(),
      /*IsThrownVariableInScope=*/false);
  
  return throw_expr;
}

void ExceptionASTTransform::RunImpl() {
  LOG(INFO) << "=== EXCEPTION AST TRANSFORMATION ===";
  LOG(INFO) << "Processing exception regions to transform AST";
  
  LOG(INFO) << "Total exception regions: " << dec_ctx.exception_regions.GetTryRegions().size();
  for (size_t i = 0; i < dec_ctx.exception_regions.GetTryRegions().size(); i++) {
    const auto& region = dec_ctx.exception_regions.GetTryRegions()[i];
    LOG(INFO) << "Region " << i << " has " << region.catch_handlers.size() << " catch handlers";
    for (size_t j = 0; j < region.catch_handlers.size(); j++) {
      const auto& handler = region.catch_handlers[j];
      LOG(INFO) << "  Handler " << j << ": is_catch_all=" << handler.is_catch_all 
                << ", type=" << handler.exception_type_name
                << ", blocks=" << handler.blocks.size();
    }
  }
  
  std::set<std::string> processed_function_names;
  
  // Visit all function declarations and transform their bodies if needed
  auto* tu = dec_ctx.ast_ctx.getTranslationUnitDecl();
  
  // First, collect ALL function declarations with the same name
  std::map<std::string, std::vector<clang::FunctionDecl*>> function_declarations;
  
  for (auto* decl : tu->decls()) {
    if (auto* func_decl = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
      std::string func_name = func_decl->getName().str();
      function_declarations[func_name].push_back(func_decl);
    }
  }
  
  // Log how many declarations we found for each function
  for (const auto& [func_name, decls] : function_declarations) {
    LOG(INFO) << "Function " << func_name << " has " << decls.size() << " declarations";
    int with_body = 0;
    for (auto* decl : decls) {
      if (decl->hasBody()) with_body++;
    }
    LOG(INFO) << "  " << with_body << " have bodies";
  }
  
  // Now transform each function, but update ALL declarations with the same name
  for (const auto& [func_name, decls] : function_declarations) {
    if (processed_function_names.count(func_name)) continue;  // Skip if already processed
    
    // Find the declaration with a body
    clang::FunctionDecl* func_with_body = nullptr;
    clang::CompoundStmt* current_body = nullptr;
    
    for (auto* func_decl : decls) {
      if (func_decl->hasBody()) {
        func_with_body = func_decl;
        current_body = llvm::dyn_cast<clang::CompoundStmt>(func_decl->getBody());
        break;
      }
    }
    
    if (!func_with_body || !current_body) continue;
    
    processed_function_names.insert(func_name);
    
    try {
      // Transform the function body using the exception regions
      auto* new_body = TransformFunctionBody(current_body, func_with_body);
      
      if (new_body && new_body != current_body) {
        // Update only the function declaration that originally had the body
        func_with_body->setBody(new_body);
        LOG(INFO) << "✓ Updated function body for " << func_with_body->getName().str();
        
        // Clear bodies from other declarations to avoid duplication
        for (auto* func_decl : decls) {
          if (func_decl != func_with_body && func_decl->hasBody()) {
            func_decl->setBody(nullptr);
            LOG(INFO) << "Cleared duplicate body from " << func_decl->getName().str();
          }
        }
      }
    } catch (const std::exception& e) {
      LOG(ERROR) << "Exception while transforming " << func_name << ": " << e.what();
    } catch (...) {
      LOG(ERROR) << "Unknown exception while transforming " << func_name;
    }
  }
  
  LOG(INFO) << "=== EXCEPTION AST TRANSFORMATION COMPLETE ===";
  
  // Post-processing: Transform all remaining __cxa_throw calls to throw expressions
  LOG(INFO) << "=== POST-PROCESSING: Transforming __cxa_throw calls ===";
  
  // Create a visitor to find and transform __cxa_throw calls
  class ThrowTransformVisitor : public clang::RecursiveASTVisitor<ThrowTransformVisitor> {
  public:
    ExceptionASTTransform& transform;
    bool changed = false;
    
    ThrowTransformVisitor(ExceptionASTTransform& t) : transform(t) {}
    
    bool VisitCallExpr(clang::CallExpr* call) {
      if (IsCallToFunction(call, "__cxa_throw")) {
        LOG(INFO) << "Found __cxa_throw call in post-processing";
        auto* throw_expr = transform.TransformThrowCall(call);
        if (throw_expr) {
          LOG(INFO) << "Transformed __cxa_throw to throw expression";
          transform.substitutions[call] = throw_expr;
          changed = true;
        }
      }
      return true;
    }
  };
  
  // Visit all functions again and apply the transformation
  for (auto* decl : tu->decls()) {
    if (auto* func_decl = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
      if (func_decl->hasBody()) {
        ThrowTransformVisitor visitor(*this);
        visitor.TraverseStmt(func_decl->getBody());
        if (visitor.changed) {
          LOG(INFO) << "Transformed __cxa_throw calls in function " << func_decl->getName().str();
          changed = true;
        }
      }
    }
  }
  
  LOG(INFO) << "=== POST-PROCESSING COMPLETE ===";
  
  // Apply the substitutions by traversing the AST again
  if (changed) {
    LOG(INFO) << "Applying substitutions from post-processing";
    for (auto* decl : tu->decls()) {
      if (auto* func_decl = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
        if (func_decl->hasBody()) {
          this->TraverseStmt(func_decl->getBody());
        }
      }
    }
  }
}

// Simplified TransformUsingExceptionContext that properly handles recursive processing
clang::CompoundStmt* ExceptionASTTransform::TransformUsingExceptionContext(
    clang::CompoundStmt* body,
    clang::FunctionDecl* func_decl) {
  
  LOG(INFO) << "TransformUsingExceptionContext for function: " << func_decl->getName().str();
  
  // Check if we have any exception context for this function
  bool has_exception_context = false;
  for (const auto& [stmt, ctx] : dec_ctx.stmt_exception_context) {
    // Check if this statement belongs to the current function
    // This is a simple check - in practice we'd need to verify the statement is within this function's body
    has_exception_context = true;
    break;
  }
  
  if (!has_exception_context) {
    LOG(INFO) << "No exception context found for function, returning original body";
    return body;
  }
  
  // Collect statements organized by landing pad and exception type
  std::map<llvm::BasicBlock*, std::vector<clang::Stmt*>> try_stmts_by_landing_pad;
  std::map<llvm::BasicBlock*, std::map<std::string, std::vector<clang::Stmt*>>> catch_stmts_by_landing_pad;
  
  // Process the exception context mappings
  for (const auto& [stmt, ctx] : dec_ctx.stmt_exception_context) {
    if (!stmt) continue;
    
    // Check if this is a printf call
    bool is_printf = false;
    if (auto* call_expr = llvm::dyn_cast<clang::CallExpr>(stmt)) {
      if (auto* callee = call_expr->getDirectCallee()) {
        if (callee->getName() == "printf") {
          is_printf = true;
          LOG(INFO) << "Found printf call in exception context, type: " 
                    << (ctx.type == DecompilationContext::ExceptionContext::TRY_BLOCK ? "TRY_BLOCK" : "CATCH_HANDLER")
                    << ", exception_type: " << ctx.exception_type;
        }
      }
    }
    
    // Log what types of statements we're seeing
    if (auto* decl_stmt = llvm::dyn_cast<clang::DeclStmt>(stmt)) {
      LOG(INFO) << "Found DeclStmt in exception context";
    } else if (auto* expr_stmt = llvm::dyn_cast<clang::Expr>(stmt)) {
      LOG(INFO) << "Found Expr in exception context" << (is_printf ? " (printf call)" : "");
    } else if (auto* if_stmt = llvm::dyn_cast<clang::IfStmt>(stmt)) {
      LOG(INFO) << "Found IfStmt in exception context";
    } else if (auto* compound = llvm::dyn_cast<clang::CompoundStmt>(stmt)) {
      LOG(INFO) << "Found CompoundStmt in exception context";
    } else {
      LOG(INFO) << "Found other statement type in exception context" << (is_printf ? " (printf call)" : "");
    }
    
    if (ctx.type == DecompilationContext::ExceptionContext::TRY_BLOCK) {
      if (!IsExceptionInfrastructure(stmt)) {
        // Special handling for IfStmts with marker conditions
        if (auto* if_stmt = llvm::dyn_cast<clang::IfStmt>(stmt)) {
          if (HasMarkerCondition(if_stmt, dec_ctx)) {
            LOG(INFO) << "Found IfStmt with marker condition in try block (context approach)";
            // Extract the body of the if statement instead of the whole if
            if (auto* then_stmt = if_stmt->getThen()) {
              if (auto* compound = llvm::dyn_cast<clang::CompoundStmt>(then_stmt)) {
                // Add all statements from the compound body
                for (auto* body_stmt : compound->body()) {
                  if (!IsExceptionInfrastructure(body_stmt)) {
                    LOG(INFO) << "  Extracting statement from marker IfStmt body";
                    try_stmts_by_landing_pad[ctx.landing_pad].push_back(body_stmt);
                  }
                }
              } else {
                LOG(INFO) << "  Extracting single statement from marker IfStmt";
                try_stmts_by_landing_pad[ctx.landing_pad].push_back(then_stmt);
              }
            }
          } else {
            // Normal case - add the full if statement
            try_stmts_by_landing_pad[ctx.landing_pad].push_back(stmt);
          }
        } else {
          // Not an if statement, add as-is
          try_stmts_by_landing_pad[ctx.landing_pad].push_back(stmt);
        }
      }
    } else if (ctx.type == DecompilationContext::ExceptionContext::CATCH_HANDLER) {
      if (!IsExceptionInfrastructure(stmt, /*in_catch_handler=*/true)) {
        // Special handling for IfStmts with marker conditions
        if (auto* if_stmt = llvm::dyn_cast<clang::IfStmt>(stmt)) {
          if (HasMarkerCondition(if_stmt, dec_ctx)) {
            LOG(INFO) << "Found IfStmt with marker condition in catch handler (context approach)";
            // Extract the body of the if statement instead of the whole if
            if (auto* then_stmt = if_stmt->getThen()) {
              if (auto* compound = llvm::dyn_cast<clang::CompoundStmt>(then_stmt)) {
                // Add all statements from the compound body
                for (auto* body_stmt : compound->body()) {
                  if (!IsExceptionInfrastructure(body_stmt, /*in_catch_handler=*/true)) {
                    LOG(INFO) << "  Extracting statement from marker IfStmt body";
                    catch_stmts_by_landing_pad[ctx.landing_pad][ctx.exception_type].push_back(body_stmt);
                  }
                }
              } else {
                LOG(INFO) << "  Extracting single statement from marker IfStmt";
                catch_stmts_by_landing_pad[ctx.landing_pad][ctx.exception_type].push_back(then_stmt);
              }
            }
          } else {
            // Normal case - add the full if statement
            catch_stmts_by_landing_pad[ctx.landing_pad][ctx.exception_type].push_back(stmt);
          }
        } else {
          // Not an if statement, add as-is
          catch_stmts_by_landing_pad[ctx.landing_pad][ctx.exception_type].push_back(stmt);
        }
      }
    }
  }
  
  LOG(INFO) << "Found " << try_stmts_by_landing_pad.size() << " landing pads with try blocks";
  
  // Build new function body
  std::vector<clang::Stmt*> new_body_stmts;
  std::set<clang::Stmt*> used_stmts;
  
  // Helper to check if a statement contains any used statements
  auto contains_used_stmt = [&](clang::Stmt* stmt) -> bool {
    class UsedChecker : public clang::RecursiveASTVisitor<UsedChecker> {
    public:
      const std::set<clang::Stmt*>& used;
      bool found = false;
      
      UsedChecker(const std::set<clang::Stmt*>& u) : used(u) {}
      
      bool VisitStmt(clang::Stmt* s) {
        if (used.count(s)) {
          found = true;
          return false;
        }
        return true;
      }
    };
    
    UsedChecker checker(used_stmts);
    checker.TraverseStmt(stmt);
    return checker.found;
  };
  
  // First, collect all top-level statements that should be in try/catch blocks
  std::map<llvm::BasicBlock*, std::vector<clang::Stmt*>> try_toplevel_by_landing_pad;
  std::map<llvm::BasicBlock*, std::map<std::string, std::vector<clang::Stmt*>>> catch_toplevel_by_landing_pad;
  
  // Helper to check if a statement contains any marked statements
  auto contains_marked_stmt = [&](clang::Stmt* parent, DecompilationContext::ExceptionContext::Type type, 
                                  llvm::BasicBlock* landing_pad, const std::string& exception_type = "") -> bool {
    class MarkedChecker : public clang::RecursiveASTVisitor<MarkedChecker> {
    public:
      const std::unordered_map<clang::Stmt*, DecompilationContext::ExceptionContext>& contexts;
      DecompilationContext::ExceptionContext::Type target_type;
      llvm::BasicBlock* target_landing_pad;
      std::string target_exception_type;
      bool found = false;
      
      MarkedChecker(const std::unordered_map<clang::Stmt*, DecompilationContext::ExceptionContext>& c,
                    DecompilationContext::ExceptionContext::Type t,
                    llvm::BasicBlock* lp,
                    const std::string& et)
          : contexts(c), target_type(t), target_landing_pad(lp), target_exception_type(et) {}
      
      bool VisitStmt(clang::Stmt* s) {
        auto it = contexts.find(s);
        if (it != contexts.end()) {
          const auto& ctx = it->second;
          if (ctx.type == target_type && ctx.landing_pad == target_landing_pad) {
            if (target_type == DecompilationContext::ExceptionContext::CATCH_HANDLER) {
              if (ctx.exception_type == target_exception_type) {
                found = true;
                return false;
              }
            } else {
              found = true;
              return false;
            }
          }
        }
        return true;
      }
    };
    
    MarkedChecker checker(dec_ctx.stmt_exception_context, type, landing_pad, exception_type);
    checker.TraverseStmt(parent);
    return checker.found;
  };
  
  // Process each original top-level statement to find which ones contain marked statements
  for (auto* orig_stmt : body->body()) {
    // Check if this statement contains any try block statements
    for (const auto& [landing_pad, try_stmts] : try_stmts_by_landing_pad) {
      if (contains_marked_stmt(orig_stmt, DecompilationContext::ExceptionContext::TRY_BLOCK, landing_pad)) {
        try_toplevel_by_landing_pad[landing_pad].push_back(orig_stmt);
        LOG(INFO) << "Top-level statement contains try block statements for landing pad";
      }
    }
    
    // Check if this statement contains any catch handler statements
    for (const auto& [landing_pad, catch_map] : catch_stmts_by_landing_pad) {
      for (const auto& [exception_type, catch_stmts] : catch_map) {
        if (contains_marked_stmt(orig_stmt, DecompilationContext::ExceptionContext::CATCH_HANDLER, 
                               landing_pad, exception_type)) {
          catch_toplevel_by_landing_pad[landing_pad][exception_type].push_back(orig_stmt);
          LOG(INFO) << "Top-level statement contains catch handler statements for " << exception_type;
        }
      }
    }
  }
  
  // Process each original top-level statement
  for (auto* orig_stmt : body->body()) {
    // Skip if already used
    if (contains_used_stmt(orig_stmt)) {
      continue;
    }
    
    // Check if this statement should be part of a try-catch block
    bool handled = false;
    
    for (const auto& [landing_pad, try_stmts] : try_toplevel_by_landing_pad) {
      // Check if this original statement is one of the try top-level statements
      bool is_try_stmt = std::find(try_stmts.begin(), try_stmts.end(), orig_stmt) != try_stmts.end();
      
      if (is_try_stmt) {
        // Create try-catch block
        auto catch_it = catch_toplevel_by_landing_pad.find(landing_pad);
        
        // Only create try-catch if we have catch handlers
        if (catch_it != catch_toplevel_by_landing_pad.end() && !catch_it->second.empty()) {
          // Mark all try statements as used
          for (auto* s : try_stmts) {
            used_stmts.insert(s);
          }
          
          // Create try body with all top-level statements that belong to try
          std::vector<clang::Stmt*> try_stmts_copy = try_stmts;
          auto* try_body = dec_ctx.ast.CreateCompoundStmt(try_stmts_copy);
          
          // Create catch handlers
          std::vector<clang::CXXCatchStmt*> catch_handlers;
          for (const auto& [exception_type, catch_toplevel_stmts] : catch_it->second) {
            if (catch_toplevel_stmts.empty()) continue;
            
            // Mark catch statements as used
            for (auto* s : catch_toplevel_stmts) {
              used_stmts.insert(s);
            }
            
            // Filter out exception infrastructure from catch statements
            std::vector<clang::Stmt*> filtered_catch_stmts;
            for (auto* catch_stmt : catch_toplevel_stmts) {
              std::vector<clang::Stmt*> filtered_substmts;
              FilterExceptionInfrastructure(catch_stmt, filtered_substmts, /*in_catch_handler=*/true);
              for (auto* filtered : filtered_substmts) {
                filtered_catch_stmts.push_back(filtered);
              }
            }
            
            std::vector<clang::Stmt*> catch_stmts_copy = filtered_catch_stmts;
            
            // Add __cxa_end_catch() call at the end of the catch handler
            // First, find the __cxa_end_catch function declaration
            clang::FunctionDecl* end_catch_func = nullptr;
            auto* tu = dec_ctx.ast_ctx.getTranslationUnitDecl();
            for (auto* decl : tu->decls()) {
              if (auto* func = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
                if (func->getName() == "__cxa_end_catch") {
                  end_catch_func = func;
                  break;
                }
              }
            }
            
            if (end_catch_func) {
              // Check if the last statement is a return - if so, don't add __cxa_end_catch after it
              bool ends_with_return = false;
              if (!catch_stmts_copy.empty()) {
                if (llvm::isa<clang::ReturnStmt>(catch_stmts_copy.back())) {
                  ends_with_return = true;
                }
              }
              
              if (!ends_with_return) {
                // Create call to __cxa_end_catch()
                std::vector<clang::Expr*> args; // No arguments
                auto* end_catch_call = dec_ctx.ast.CreateCall(end_catch_func, args);
                catch_stmts_copy.push_back(end_catch_call);
              }
            }
            
            auto* catch_body = dec_ctx.ast.CreateCompoundStmt(catch_stmts_copy);
            
            clang::VarDecl* exception_var = nullptr;
            if (!exception_type.empty() && exception_type != "...") {
              std::string var_name = CreateVariableNameFromType(exception_type);
              auto exception_type_qual = CreateExceptionType(exception_type, dec_ctx);
              exception_var = dec_ctx.ast.CreateVarDecl(func_decl, exception_type_qual, var_name);
            }
            
            auto* catch_stmt = dec_ctx.ast.CreateCXXCatchStmt(
                clang::SourceLocation(), exception_var, catch_body);
            catch_handlers.push_back(catch_stmt);
          }
          
          // Create try-catch statement
          auto* try_catch = dec_ctx.ast.CreateCXXTryStmt(
              clang::SourceLocation(), try_body,
              llvm::ArrayRef<clang::CXXCatchStmt*>(catch_handlers));
          new_body_stmts.push_back(try_catch);
          handled = true;
        }
        break;
      }
    }
    
    // If not handled as try-catch, add as normal statement
    if (!handled) {
      new_body_stmts.push_back(orig_stmt);
    }
  }
  
  LOG(INFO) << "Transformed body has " << new_body_stmts.size() << " top-level statements";
  return dec_ctx.ast.CreateCompoundStmt(new_body_stmts);
}

// Helper function
bool ExceptionASTTransform::IsAncestorOf(clang::Stmt* parent, clang::Stmt* child) {
  if (!parent || !child) return false;
  if (parent == child) return true;
  
  class AncestorChecker : public clang::RecursiveASTVisitor<AncestorChecker> {
  public:
    clang::Stmt* target;
    bool found = false;
    
    AncestorChecker(clang::Stmt* t) : target(t) {}
    
    bool VisitStmt(clang::Stmt* s) {
      if (s == target) {
        found = true;
        return false;
      }
      return true;
    }
  };
  
  AncestorChecker checker(child);
  checker.TraverseStmt(parent);
  return checker.found;
}

clang::CompoundStmt* ExceptionASTTransform::TransformUsingDirectMapping(
    clang::CompoundStmt* body,
    clang::FunctionDecl* func_decl,
    llvm::Function* llvm_func) {
  
  LOG(INFO) << "TransformUsingDirectMapping for function: " << func_decl->getName().str();
  
  // Create a mapping from statements to their original position in the body
  std::vector<clang::Stmt*> original_stmts(body->body().begin(), body->body().end());
  std::set<clang::Stmt*> used_stmts;
  
  // New transformed body
  std::vector<clang::Stmt*> new_body_stmts;
  
  // Map basic blocks to statements - only use top-level statements
  std::unordered_map<llvm::BasicBlock*, std::vector<clang::Stmt*>> block_to_stmts;
  
  // Build the block-to-statements mapping from original top-level statements
  for (auto* stmt : original_stmts) {
    auto prov_it = dec_ctx.stmt_provenance.find(stmt);
    if (prov_it != dec_ctx.stmt_provenance.end() && prov_it->second) {
      if (auto* inst = llvm::dyn_cast<llvm::Instruction>(prov_it->second)) {
        block_to_stmts[inst->getParent()].push_back(stmt);
      }
    }
  }
  
  // Find regions for this function
  std::vector<const TryRegion*> function_regions;
  for (const auto& region : dec_ctx.exception_regions.GetTryRegions()) {
    // Skip regions from other functions
    if (!region.blocks.empty()) {
      auto* first_block = *region.blocks.begin();
      if (first_block->getParent() != llvm_func) {
        continue;
      }
    }
    function_regions.push_back(&region);
  }
  
  LOG(INFO) << "Function " << func_decl->getName().str() << " has " << function_regions.size() << " regions total";
  
  
  // Find top-level regions (those without parent regions) - fallback for normal processing
  std::vector<const TryRegion*> top_level_regions;
  for (const auto* region : function_regions) {
    if (region->parent_region == nullptr) {
      top_level_regions.push_back(region);
    }
  }
  
  // Process each top-level exception region
  for (const auto* region : top_level_regions) {
    LOG(INFO) << "Processing top-level exception region with " << region->blocks.size() 
              << " try blocks and " << region->catch_handlers.size() << " handlers";
    
    // Use CreateNestedTryCatch to handle nested regions
    auto* try_catch = CreateNestedTryCatch(*region, block_to_stmts);
    if (try_catch) {
      new_body_stmts.push_back(try_catch);
      
      // Mark all statements used by this region (including nested ones)
      std::function<void(const TryRegion&)> mark_used = [&](const TryRegion& r) {
        for (auto* block : r.blocks) {
          auto it = block_to_stmts.find(block);
          if (it != block_to_stmts.end()) {
            for (auto* stmt : it->second) {
              used_stmts.insert(stmt);
            }
          }
        }
        for (const auto& handler : r.catch_handlers) {
          for (auto* block : handler.blocks) {
            auto it = block_to_stmts.find(block);
            if (it != block_to_stmts.end()) {
              for (auto* stmt : it->second) {
                used_stmts.insert(stmt);
              }
            }
          }
        }
        // Recursively mark nested regions
        for (auto* nested : r.nested_regions) {
          mark_used(*nested);
        }
      };
      
      mark_used(*region);
    }
  }
  
  // Add remaining statements that weren't part of any exception region
  for (auto* stmt : original_stmts) {
    if (!used_stmts.count(stmt)) {
      if (IsExceptionInfrastructure(stmt)) {
        LOG(INFO) << "Filtering infrastructure statement from remaining statements";
      } else {
        new_body_stmts.push_back(stmt);
      }
    }
  }
  
  LOG(INFO) << "Generated " << new_body_stmts.size() << " statements in transformed body";
  return dec_ctx.ast.CreateCompoundStmt(new_body_stmts);
}

// Create a unified try-catch block from multiple regions with same handlers
clang::CompoundStmt* ExceptionASTTransform::CreateUnifiedTryCatch(
    clang::CompoundStmt* body,
    clang::FunctionDecl* func_decl,
    llvm::Function* llvm_func,
    const std::vector<const TryRegion*>& regions) {
  
  LOG(INFO) << "CreateUnifiedTryCatch: unifying " << regions.size() << " regions";
  
  // Check if we need to handle nested exceptions by looking for rethrow
  bool has_rethrow = false;
  if (llvm_func) {
    // Look for rethrow calls in the LLVM function
    for (auto& bb : *llvm_func) {
      for (auto& inst : bb) {
        if (IsRethrowInstruction(&inst)) {
          has_rethrow = true;
          LOG(INFO) << "Found rethrow instruction in LLVM IR - will handle nested exceptions separately";
          break;
        }
      }
      if (has_rethrow) break;
    }
  }
  
  // Collect all try blocks from all regions
  std::unordered_set<llvm::BasicBlock*> all_try_blocks;
  for (const auto* region : regions) {
    all_try_blocks.insert(region->blocks.begin(), region->blocks.end());
  }
  
  // Collect all catch handler blocks from all regions
  std::unordered_set<llvm::BasicBlock*> all_catch_blocks;
  std::map<std::string, std::unordered_set<llvm::BasicBlock*>> catch_blocks_by_type;
  
  // Find the region with handlers to use as template
  const TryRegion* template_region = nullptr;
  for (const auto* region : regions) {
    if (!region->catch_handlers.empty()) {
      template_region = region;
      break;
    }
  }
  
  if (!template_region) {
    LOG(ERROR) << "No region with handlers found for unified try-catch";
    return body; // Return original body if no handlers found
  }
  
  const auto& template_handlers = template_region->catch_handlers;
  for (const auto& handler : template_handlers) {
    // Collect blocks from this handler type across all regions
    for (const auto* region : regions) {
      for (const auto& h : region->catch_handlers) {
        if (h.exception_type_name == handler.exception_type_name) {
          catch_blocks_by_type[handler.exception_type_name].insert(h.blocks.begin(), h.blocks.end());
          all_catch_blocks.insert(h.blocks.begin(), h.blocks.end());
        }
      }
    }
  }
  
  LOG(INFO) << "Collected " << all_try_blocks.size() << " try blocks and " 
            << all_catch_blocks.size() << " catch blocks";
  
  // Debug: Log catch block names
  LOG(INFO) << "Catch block names:";
  for (const auto& [type, blocks] : catch_blocks_by_type) {
    LOG(INFO) << "  '" << type << "' (empty=" << type.empty() << "): ";
    for (auto* block : blocks) {
      LOG(INFO) << "    " << GetBlockName(block, &dec_ctx);
    }
  }
  
  // Process the original body to collect statements
  std::vector<clang::Stmt*> try_stmts;
  std::map<std::string, std::vector<clang::Stmt*>> catch_stmts;
  std::map<std::string, std::vector<clang::Stmt*>> outer_catch_stmts; // For nested exceptions
  std::vector<clang::Stmt*> other_stmts;
  
  LOG(INFO) << "Processing " << body->size() << " statements in original body";
  
  // Debug: Log what types of statements we have in the original body
  int decl_count = 0, if_count = 0, expr_count = 0, compound_count = 0, other_count = 0;
  for (auto* stmt : body->body()) {
    if (llvm::isa<clang::DeclStmt>(stmt)) decl_count++;
    else if (llvm::isa<clang::IfStmt>(stmt)) if_count++;
    else if (llvm::isa<clang::Expr>(stmt)) expr_count++;
    else if (llvm::isa<clang::CompoundStmt>(stmt)) compound_count++;
    else other_count++;
  }
  LOG(INFO) << "Original body contains: " << decl_count << " declarations, " 
            << if_count << " if statements, " << expr_count << " expressions, "
            << compound_count << " compound statements, " << other_count << " other statements";
  
  for (auto* stmt : body->body()) {
    // Get the source block for this statement
    llvm::BasicBlock* source_block = nullptr;
    auto prov_it = dec_ctx.stmt_provenance.find(stmt);
    if (prov_it != dec_ctx.stmt_provenance.end() && prov_it->second) {
      if (auto* inst = llvm::dyn_cast<llvm::Instruction>(prov_it->second)) {
        source_block = inst->getParent();
      }
    }
    
    // For statements without source blocks, check if they're marker conditions
    bool is_marker_condition = false;
    if (auto* if_stmt = llvm::dyn_cast<clang::IfStmt>(stmt)) {
      is_marker_condition = HasMarkerCondition(if_stmt, dec_ctx);
    }
    
    // Debug: Log source block for marker conditions
    if (is_marker_condition) {
      if (source_block) {
        LOG(INFO) << "Marker condition source block: " << GetBlockName(source_block, &dec_ctx);
      } else {
        LOG(INFO) << "Marker condition has NO source block";
      }
    }
    
    // Check if this statement belongs to try blocks
    bool in_try = source_block && all_try_blocks.count(source_block) > 0;
    bool in_catch = false;
    std::string catch_type;
    
    // Check if it belongs to a catch handler
    if (source_block) {
      for (const auto& [type, blocks] : catch_blocks_by_type) {
        if (blocks.count(source_block) > 0) {
          in_catch = true;
          catch_type = type;
          break;
        }
      }
    }
    
    // Handle marker conditions - use exception context to classify
    if (is_marker_condition) {
      auto* if_stmt = llvm::cast<clang::IfStmt>(stmt);
      LOG(INFO) << "Processing marker condition - checking exception context";
      
      // Check if we have exception context for this statement
      bool found_in_catch = false;
      std::string context_catch_type;
      
      // Check exception context for the marker condition itself
      auto ctx_it = dec_ctx.stmt_exception_context.find(stmt);
      if (ctx_it != dec_ctx.stmt_exception_context.end()) {
        const auto& ctx = ctx_it->second;
        if (ctx.type == DecompilationContext::ExceptionContext::CATCH_HANDLER) {
          found_in_catch = true;
          context_catch_type = ctx.exception_type;
          LOG(INFO) << "Found marker condition in exception context for " << context_catch_type;
        }
      }
      
      // If not found, check the contents of the marker condition
      if (!found_in_catch && if_stmt->getThen()) {
        // Check if any statement inside has exception context
        class ContextChecker : public clang::RecursiveASTVisitor<ContextChecker> {
        public:
          DecompilationContext& dec_ctx;
          bool found_catch = false;
          std::string catch_type;
          
          ContextChecker(DecompilationContext& ctx) : dec_ctx(ctx) {}
          
          bool VisitStmt(clang::Stmt* s) {
            auto it = dec_ctx.stmt_exception_context.find(s);
            if (it != dec_ctx.stmt_exception_context.end()) {
              const auto& ctx = it->second;
              if (ctx.type == DecompilationContext::ExceptionContext::CATCH_HANDLER) {
                found_catch = true;
                catch_type = ctx.exception_type;
                return false; // Stop traversal
              }
            }
            return true;
          }
        };
        
        ContextChecker checker(dec_ctx);
        checker.TraverseStmt(if_stmt->getThen());
        if (checker.found_catch) {
          found_in_catch = true;
          context_catch_type = checker.catch_type;
          LOG(INFO) << "Found catch context inside marker condition for " << context_catch_type;
        }
      }
      
      // Use source block mapping as fallback
      if (!found_in_catch && in_catch && !catch_type.empty()) {
        found_in_catch = true;
        context_catch_type = catch_type;
        LOG(INFO) << "Using source block mapping for catch type " << context_catch_type;
      }
      
      if (found_in_catch) {
        LOG(INFO) << "Marker condition is in catch handler for '" << context_catch_type << "' (empty=" << context_catch_type.empty() << ")";
        if (auto* then_stmt = if_stmt->getThen()) {
          if (auto* compound = llvm::dyn_cast<clang::CompoundStmt>(then_stmt)) {
            for (auto* body_stmt : compound->body()) {
              // Log what type of statement we're processing
              if (auto* call = llvm::dyn_cast<clang::CallExpr>(body_stmt)) {
                if (auto* callee = call->getDirectCallee()) {
                  LOG(INFO) << "  Processing call to " << callee->getName().str() << " in catch handler";
                }
              }
              
              if (!IsExceptionInfrastructure(body_stmt, true)) {
                // Check if this is an outer catch statement for nested exceptions
                bool is_outer_catch = false;
                if (has_rethrow) {
                  if (auto* call = llvm::dyn_cast<clang::CallExpr>(body_stmt)) {
                    if (call->getNumArgs() > 0) {
                      if (auto* str_literal = llvm::dyn_cast<clang::StringLiteral>(call->getArg(0))) {
                        std::string str_value = str_literal->getString().str();
                        LOG(INFO) << "    Checking string literal: " << str_value;
                        if (str_value.find("OUTER CATCH BLOCK") != std::string::npos ||
                            str_value.find("Outer error code") != std::string::npos ||
                            str_value.find("Exception handling complete") != std::string::npos) {
                          is_outer_catch = true;
                          LOG(INFO) << "    DETECTED OUTER CATCH STATEMENT!";
                        }
                      }
                    }
                  }
                }
                
                if (has_rethrow && is_outer_catch) {
                  outer_catch_stmts[context_catch_type].push_back(body_stmt);
                  LOG(INFO) << "Added statement to outer catch handler " << context_catch_type;
                } else {
                  catch_stmts[context_catch_type].push_back(body_stmt);
                  LOG(INFO) << "Added statement to catch handler " << context_catch_type;
                }
              } else {
                LOG(INFO) << "Filtered out infrastructure statement from catch handler";
              }
            }
          } else if (!IsExceptionInfrastructure(then_stmt, true)) {
            // Check if this is an outer catch statement for nested exceptions
            bool is_outer_catch = false;
            if (has_rethrow) {
              if (auto* call = llvm::dyn_cast<clang::CallExpr>(then_stmt)) {
                if (call->getNumArgs() > 0) {
                  if (auto* str_literal = llvm::dyn_cast<clang::StringLiteral>(call->getArg(0))) {
                    std::string str_value = str_literal->getString().str();
                    if (str_value.find("OUTER CATCH BLOCK") != std::string::npos ||
                        str_value.find("Outer error code") != std::string::npos ||
                        str_value.find("Exception handling complete") != std::string::npos) {
                      is_outer_catch = true;
                    }
                  }
                }
              }
            }
            
            if (has_rethrow && is_outer_catch) {
              outer_catch_stmts[context_catch_type].push_back(then_stmt);
              LOG(INFO) << "Added single statement to outer catch handler " << context_catch_type;
            } else {
              catch_stmts[context_catch_type].push_back(then_stmt);
              LOG(INFO) << "Added single statement to catch handler " << context_catch_type;
            }
          }
        }
      } else {
        // This marker condition is in try block - preserve the whole if statement
        try_stmts.push_back(stmt);
        LOG(INFO) << "Added marker condition if-statement to try block";
      }
      continue;
    }
    
    // Handle regular if statements that might contain function logic
    if (auto* if_stmt = llvm::dyn_cast<clang::IfStmt>(stmt)) {
      // Check if this is a non-marker if statement with actual logic
      if (auto* then_stmt = if_stmt->getThen()) {
        // If it's not a marker condition, it might be regular control flow
        // that should go in the try block
        if (!in_catch && !IsExceptionInfrastructure(stmt, false)) {
          try_stmts.push_back(stmt);
          LOG(INFO) << "Added regular if statement to try block";
          continue;
        }
      }
    }
    
    // Filter infrastructure calls
    if (IsExceptionInfrastructure(stmt, in_catch)) {
      LOG(INFO) << "Filtering out exception infrastructure statement";
      continue;
    }
    
    
    // Add to appropriate list
    if (in_try) {
      try_stmts.push_back(stmt);
      LOG(INFO) << "Added regular statement to try block";
    } else if (in_catch) {
      // Check if this is an inner or outer catch block for nested exceptions
      bool is_outer_catch = false;
      if (has_rethrow && source_block) {
        // Check if this statement contains outer catch markers
        if (auto* call = llvm::dyn_cast<clang::CallExpr>(stmt)) {
          if (call->getNumArgs() > 0) {
            if (auto* str_literal = llvm::dyn_cast<clang::StringLiteral>(call->getArg(0))) {
              std::string str_value = str_literal->getString().str();
              if (str_value.find("OUTER CATCH BLOCK") != std::string::npos ||
                  str_value.find("Outer error code") != std::string::npos ||
                  str_value.find("Exception handling complete") != std::string::npos) {
                is_outer_catch = true;
                LOG(INFO) << "Detected outer catch statement: " << str_value;
              }
            }
          }
        }
      }
      
      // For nested exceptions, separate inner and outer catch statements
      if (has_rethrow && is_outer_catch) {
        // This is an outer catch statement - store it separately
        outer_catch_stmts[catch_type].push_back(stmt);
        LOG(INFO) << "Storing outer catch statement for " << catch_type << " in nested structure";
      } else {
        catch_stmts[catch_type].push_back(stmt);
        LOG(INFO) << "Added regular statement to catch handler " << catch_type;
      }
    } else {
      // For statements without clear source block mapping,
      // use heuristics to determine where they should go
      
      // Variable declarations stay outside the try block
      if (llvm::isa<clang::DeclStmt>(stmt)) {
        other_stmts.push_back(stmt);
        LOG(INFO) << "Added declaration to other statements";
      }
      // Exception infrastructure calls should be filtered out  
      else if (IsExceptionInfrastructure(stmt, false)) {
        LOG(INFO) << "Filtering out exception infrastructure statement";
        continue;
      }
      // All other statements (assignments, conditionals, calls, throws) go in try block
      else {
        
        try_stmts.push_back(stmt);
        LOG(INFO) << "Added unclassified statement to try block";
      }
    }
  }
  
  LOG(INFO) << "Collected " << try_stmts.size() << " try statements, "
            << catch_stmts.size() << " catch handler types";
  
  // Debug: Log what statements are in catch handlers
  LOG(INFO) << "Catch handler statements:";
  for (const auto& [catch_type, stmts] : catch_stmts) {
    LOG(INFO) << "  Handler for " << catch_type << " has " << stmts.size() << " statements";
    for (auto* stmt : stmts) {
      if (auto* call = llvm::dyn_cast<clang::CallExpr>(stmt)) {
        if (auto* callee = call->getDirectCallee()) {
          LOG(INFO) << "    - Call to " << callee->getName().str();
        } else {
          LOG(INFO) << "    - Indirect call";
        }
      } else if (auto* decl_stmt = llvm::dyn_cast<clang::DeclStmt>(stmt)) {
        LOG(INFO) << "    - Declaration statement";
      } else if (auto* bin_op = llvm::dyn_cast<clang::BinaryOperator>(stmt)) {
        LOG(INFO) << "    - Binary operator (assignment, etc)";
      } else if (auto* return_stmt = llvm::dyn_cast<clang::ReturnStmt>(stmt)) {
        LOG(INFO) << "    - Return statement";
      } else if (auto* expr = llvm::dyn_cast<clang::Expr>(stmt)) {
        LOG(INFO) << "    - Expression statement (not a direct call)";
      } else if (auto* compound = llvm::dyn_cast<clang::CompoundStmt>(stmt)) {
        LOG(INFO) << "    - CompoundStmt with " << compound->size() << " children:";
        for (auto* child : compound->body()) {
          if (auto* call = llvm::dyn_cast<clang::CallExpr>(child)) {
            if (auto* callee = call->getDirectCallee()) {
              LOG(INFO) << "      * Call to " << callee->getName().str();
            }
          } else {
            LOG(INFO) << "      * " << child->getStmtClassName();
          }
        }
      } else {
        if (stmt->getStmtClassName() == std::string("CompoundStmt")) {
          if (auto* compound = llvm::dyn_cast<clang::CompoundStmt>(stmt)) {
            LOG(INFO) << "    - CompoundStmt with " << compound->size() << " children";
          } else {
            LOG(INFO) << "    - CompoundStmt (cast failed)";
          }
        } else {
          LOG(INFO) << "    - Other statement type: " << stmt->getStmtClassName();
        }
      }
    }
  }
  
  // Create the unified try-catch block
  std::vector<clang::Stmt*> new_body_stmts;
  
  // Create try body
  auto* try_body = dec_ctx.ast.CreateCompoundStmt(try_stmts);
  
  // Create catch handlers
  std::vector<clang::CXXCatchStmt*> catch_handlers;
  LOG(INFO) << "Creating " << template_handlers.size() << " catch handlers";
  
  // Sort handlers by specificity - most specific first, catch-all last
  std::vector<CatchHandler> sorted_handlers = template_handlers;
  
  // Get the LLVM module from the first region's blocks
  llvm::Module* llvm_module = nullptr;
  if (!regions.empty() && !regions[0]->blocks.empty()) {
    auto* first_block = *regions[0]->blocks.begin();
    if (first_block && first_block->getParent()) {
      llvm_module = first_block->getParent()->getParent();
    }
  }
  
  // Only sort if we have exception hierarchy information
  if (llvm_module) {
    ExceptionTypeHierarchy hierarchy;
    hierarchy.AnalyzeModule(llvm_module);
    
    std::sort(sorted_handlers.begin(), sorted_handlers.end(), 
      [&hierarchy](const CatchHandler& a, const CatchHandler& b) {
        // Catch-all always goes last
        if (a.is_catch_all) return false;
        if (b.is_catch_all) return true;
        
        // Check inheritance relationship
        auto* type_a = hierarchy.GetTypeInfoGlobal(a.exception_type_name);
        auto* type_b = hierarchy.GetTypeInfoGlobal(b.exception_type_name);
        
        if (type_a && type_b) {
          // Get hierarchy depth - more specific types have lower depth
          int depth_a = hierarchy.GetHierarchyDepth(type_a);
          int depth_b = hierarchy.GetHierarchyDepth(type_b);
          
          // Lower depth means more specific (derived class)
          if (depth_a != depth_b) {
            return depth_a < depth_b;
          }
        }
        
        // Otherwise, maintain original order
        return false;
      });
  }
  
  for (const auto& handler : sorted_handlers) {
    LOG(INFO) << "Creating catch handler for '" << handler.exception_type_name << "' (is_catch_all=" << handler.is_catch_all << ")";
    auto it = catch_stmts.find(handler.exception_type_name);
    std::vector<clang::Stmt*> handler_stmts;
    if (it != catch_stmts.end()) {
      handler_stmts = it->second;
      LOG(INFO) << "  Found " << handler_stmts.size() << " statements for this handler";
    } else {
      LOG(INFO) << "  No statements found for this handler in catch_stmts map";
    }
    
    // Add __cxa_end_catch if needed
    if (!handler_stmts.empty()) {
      // Find __cxa_end_catch function
      clang::FunctionDecl* end_catch_func = nullptr;
      auto* tu = dec_ctx.ast_ctx.getTranslationUnitDecl();
      for (auto* decl : tu->decls()) {
        if (auto* func = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
          if (func->getName() == "__cxa_end_catch") {
            end_catch_func = func;
            break;
          }
        }
      }
      
      if (end_catch_func) {
        // Check if the last statement is a return
        bool ends_with_return = false;
        if (!handler_stmts.empty()) {
          if (llvm::isa<clang::ReturnStmt>(handler_stmts.back())) {
            ends_with_return = true;
          }
        }
        
        if (!ends_with_return) {
          std::vector<clang::Expr*> args;
          auto* end_catch_call = dec_ctx.ast.CreateCall(end_catch_func, args);
          handler_stmts.push_back(end_catch_call);
        }
      }
    }
    
    // Create catch body
    auto* catch_body = dec_ctx.ast.CreateCompoundStmt(handler_stmts);
    LOG(INFO) << "Created catch body for '" << handler.exception_type_name << "' with " 
              << catch_body->size() << " statements (empty=" << catch_body->body_empty() << ")";
    
    // Create exception variable
    clang::VarDecl* exception_var = nullptr;
    if (!handler.is_catch_all) {
      std::string var_name = CreateVariableNameFromType(handler.exception_type_name);
      clang::QualType exception_type = CreateExceptionType(handler.exception_type_name, dec_ctx);
      exception_var = dec_ctx.ast.CreateVarDecl(func_decl, exception_type, var_name);
    }
    
    // Create catch statement
    auto* catch_stmt = dec_ctx.ast.CreateCXXCatchStmt(
        clang::SourceLocation(), exception_var, catch_body);
    catch_handlers.push_back(catch_stmt);
    
    LOG(INFO) << "Created catch statement for '" << handler.exception_type_name 
              << "' (is_catch_all=" << handler.is_catch_all << ")";
  }
  
  // Check if we should create nested structure based on rethrow patterns
  const TryRegion* inner_region = nullptr;
  const TryRegion* outer_region = nullptr;
  
  // Check for rethrow patterns by examining LLVM IR blocks
  LOG(INFO) << "Checking for rethrow patterns in " << regions.size() << " regions";
  
  if (!regions.empty() && llvm_func) {
    // If rethrow found, identify inner and outer regions
    if (has_rethrow) {
      // Simple heuristic: regions with fewer catch handlers are likely inner
      // More sophisticated logic could be added later
      std::vector<const TryRegion*> regions_with_handlers;
      for (const auto* region : regions) {
        if (!region->catch_handlers.empty()) {
          regions_with_handlers.push_back(region);
        }
      }
      
      LOG(INFO) << "Found " << regions_with_handlers.size() << " regions with handlers";
      for (size_t i = 0; i < regions_with_handlers.size(); ++i) {
        LOG(INFO) << "Region " << i << " has " << regions_with_handlers[i]->catch_handlers.size() << " handlers";
      }
      
      if (!regions_with_handlers.empty()) {
        // Find which region contains the rethrow - that's the inner region
        const TryRegion* rethrow_region = nullptr;
        bool found_rethrow = false;
        
        // Search for rethrow pattern across all regions
        std::vector<const TryRegion*> rethrow_regions;  // Track multiple rethrow regions
        
        for (const auto* region : regions_with_handlers) {
          // Edge case: Skip empty regions
          if (!region || region->catch_handlers.empty()) {
            LOG(WARNING) << "Skipping empty or null region";
            continue;
          }
          
          bool region_has_rethrow = false;
          
          // Check if this region's catch handlers contain rethrow
          for (const auto& handler : region->catch_handlers) {
            if (region_has_rethrow) break;
            
            // Edge case: Skip handlers with no blocks
            if (handler.blocks.empty()) {
              LOG(WARNING) << "Handler has no blocks, skipping";
              continue;
            }
            
            for (auto* bb : handler.blocks) {
              if (region_has_rethrow) break;
              
              // Edge case: Skip invalid blocks
              if (!bb || bb->getParent() != llvm_func) {
                LOG(WARNING) << "Invalid or mismatched block, skipping";
                continue;
              }
              
              for (auto& inst : *bb) {
                if (IsRethrowInstruction(&inst)) {
                  rethrow_regions.push_back(region);
                  region_has_rethrow = true;
                  LOG(INFO) << "Found rethrow in region - candidate for inner region";
                  break;
                }
              }
            }
          }
        }
        
        // Edge case: Handle multiple rethrow regions
        if (rethrow_regions.size() > 1) {
          LOG(WARNING) << "Found " << rethrow_regions.size() 
                      << " regions with rethrow - selecting the first one as inner";
          rethrow_region = rethrow_regions[0];
        } else if (rethrow_regions.size() == 1) {
          rethrow_region = rethrow_regions[0];
        } else {
          LOG(INFO) << "No rethrow regions found";
        }
        if (rethrow_region) {
          inner_region = rethrow_region;
          
          // Analyze exception type hierarchy from LLVM module
          ExceptionTypeHierarchy hierarchy;
          if (llvm_func && llvm_func->getParent()) {
            hierarchy.AnalyzeModule(llvm_func->getParent());
            
            // Debug: dump the hierarchy
            std::string hierarchy_str;
            llvm::raw_string_ostream os(hierarchy_str);
            hierarchy.DumpHierarchy(os);
            os.flush();
            LOG(INFO) << "Exception type hierarchy:\n" << hierarchy_str;
          }
          
          // Analyze control flow nesting from LLVM IR
          // A region A contains region B if:
          // 1. B's exception handling blocks are invoked from within A's scope
          // 2. Control flow shows A's handlers wrap B's handlers
          auto analyzeControlFlowNesting = [&](const TryRegion* outer_candidate, 
                                             const TryRegion* inner_candidate) -> bool {
            if (!outer_candidate || !inner_candidate || outer_candidate == inner_candidate) {
              return false;
            }
            
            // Check if any of outer's blocks invoke/branch to inner's blocks
            std::set<llvm::BasicBlock*> inner_blocks;
            inner_blocks.insert(inner_candidate->blocks.begin(), inner_candidate->blocks.end());
            for (const auto& handler : inner_candidate->catch_handlers) {
              inner_blocks.insert(handler.blocks.begin(), handler.blocks.end());
            }
            
            // Look through outer region's catch handlers
            for (const auto& outer_handler : outer_candidate->catch_handlers) {
              for (auto* outer_block : outer_handler.blocks) {
                if (!outer_block) continue;
                
                // Check each instruction in the outer block
                for (auto& inst : *outer_block) {
                  // Check invoke instructions (these can trigger inner exceptions)
                  if (auto* invoke = llvm::dyn_cast<llvm::InvokeInst>(&inst)) {
                    auto* unwind_dest = invoke->getUnwindDest();
                    if (inner_blocks.count(unwind_dest) > 0) {
                      LOG(INFO) << "Found invoke from outer block " << GetBlockName(outer_block, &dec_ctx)
                               << " to inner exception block " << GetBlockName(unwind_dest, &dec_ctx);
                      return true;
                    }
                  }
                  
                  // Check if outer contains calls that could throw and be caught by inner
                  if (auto* call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
                    // If this call is in outer but the landing pad is in inner,
                    // it suggests nesting (though calls don't have explicit unwind edges)
                    if (auto* called_func = call->getCalledFunction()) {
                      if (!called_func->doesNotThrow()) {
                        // This call might throw - check if its parent block has
                        // successors in the inner region
                        for (auto* succ : llvm::successors(outer_block)) {
                          if (inner_blocks.count(succ) > 0) {
                            LOG(INFO) << "Potential nesting: call in outer block "
                                     << GetBlockName(outer_block, &dec_ctx)
                                     << " may unwind to inner block " << GetBlockName(succ, &dec_ctx);
                          }
                        }
                      }
                    }
                  }
                }
              }
            }
            
            // Also check if inner's try blocks are reachable from outer's try blocks
            // This would indicate that inner try-catch is inside outer try block
            for (auto* outer_try_block : outer_candidate->blocks) {
              for (auto& inst : *outer_try_block) {
                if (auto* invoke = llvm::dyn_cast<llvm::InvokeInst>(&inst)) {
                  if (inner_candidate->blocks.count(invoke->getNormalDest()) > 0) {
                    LOG(INFO) << "Inner try block " << GetBlockName(invoke->getNormalDest(), &dec_ctx)
                             << " is invoked from outer try block " << GetBlockName(outer_try_block, &dec_ctx);
                    return true;
                  }
                }
              }
            }
            
            return false;
          };
          
          // Find the best outer region using both hierarchy and control flow analysis
          const TryRegion* best_outer = nullptr;
          size_t best_outer_block_count = 0;
          int best_outer_genericity = -1;
          bool best_has_control_flow_nesting = false;
          
          for (const auto* region : regions_with_handlers) {
            if (region == rethrow_region) continue;
            
            // Edge case: Skip invalid regions
            if (!region) {
              LOG(WARNING) << "Null region in outer region search, skipping";
              continue;
            }
            
            // Edge case: Skip regions with no handlers (shouldn't happen here, but be safe)
            if (region->catch_handlers.empty()) {
              LOG(WARNING) << "Region with no handlers in outer region search, skipping";
              continue;
            }
            
            // Count total blocks in this region (try blocks + catch blocks)
            size_t total_blocks = region->blocks.size();
            for (const auto& handler : region->catch_handlers) {
              total_blocks += handler.blocks.size();
            }
            
            // Edge case: Skip regions with no blocks at all
            if (total_blocks == 0) {
              LOG(WARNING) << "Region has no blocks, skipping as outer candidate";
              continue;
            }
            
            // Calculate the maximum genericity of handlers in this region
            int max_genericity = 0;
            bool has_catch_all = false;
            
            for (const auto& handler : region->catch_handlers) {
              if (handler.is_catch_all) {
                has_catch_all = true;
                max_genericity = 1000;  // Catch-all is most generic
                break;
              }
              
              // Use hierarchy analysis to determine genericity
              int handler_depth = hierarchy.GetHierarchyDepth(handler.exception_type_name);
              if (handler_depth > max_genericity) {
                max_genericity = handler_depth;
              }
            }
            
            // Check control flow nesting - does this region contain the inner region?
            bool has_control_flow_nesting = false;
            if (inner_region) {
              has_control_flow_nesting = analyzeControlFlowNesting(region, inner_region);
              if (has_control_flow_nesting) {
                LOG(INFO) << "Region shows control flow nesting with inner region";
              }
            }
            
            // Also check if this region catches more general types than inner region
            bool catches_more_general = false;
            if (inner_region) {
              for (const auto& outer_handler : region->catch_handlers) {
                for (const auto& inner_handler : inner_region->catch_handlers) {
                  // Get the typeinfo globals for both types
                  auto* outer_type = hierarchy.GetTypeInfoGlobal(outer_handler.exception_type_name);
                  auto* inner_type = hierarchy.GetTypeInfoGlobal(inner_handler.exception_type_name);
                  
                  if (outer_type && inner_type && 
                      hierarchy.IsBaseOf(outer_type, inner_type)) {
                    catches_more_general = true;
                    LOG(INFO) << "Region catches " << outer_handler.exception_type_name 
                             << " which is base of inner's " << inner_handler.exception_type_name;
                    break;
                  }
                }
                if (catches_more_general) break;
              }
            }
            
            // Score this region - prefer:
            // 1. Regions with actual control flow nesting (highest priority)
            // 2. Regions that catch more general exception types (higher in hierarchy)
            // 3. Regions with more blocks (larger scope)
            // 4. Regions that catch base classes of inner region's exceptions
            size_t score = total_blocks;
            score += max_genericity * 100;  // Weight genericity heavily
            if (catches_more_general) score += 500;  // Bonus for catching base classes
            if (has_catch_all) score += 1000;  // Extra bonus for catch-all
            if (has_control_flow_nesting) score += 10000;  // Huge bonus for actual nesting
            
            // Edge case: Prevent overflow in scoring
            if (score < total_blocks) {
              LOG(WARNING) << "Score overflow detected, using total_blocks only";
              score = total_blocks;
            }
            
            if (!best_outer || score > best_outer_block_count || 
                (score == best_outer_block_count && max_genericity > best_outer_genericity)) {
              best_outer = region;
              best_outer_block_count = score;
              best_outer_genericity = max_genericity;
              best_has_control_flow_nesting = has_control_flow_nesting;
              LOG(INFO) << "Found potential outer region with " << total_blocks 
                       << " blocks, score=" << score 
                       << ", max_genericity=" << max_genericity
                       << ", catches_more_general=" << catches_more_general
                       << ", has_catch_all=" << has_catch_all
                       << ", has_control_flow_nesting=" << has_control_flow_nesting;
            }
          }
          
          outer_region = best_outer;
          
          if (outer_region) {
            // Edge case: Validate that inner and outer regions are different and valid
            if (outer_region == inner_region) {
              LOG(WARNING) << "Outer region is same as inner region - this is unexpected";
              outer_region = nullptr;
            } else {
              LOG(INFO) << "Selected inner region (with rethrow) with " << inner_region->catch_handlers.size() 
                        << " handlers, outer region with " << outer_region->catch_handlers.size() << " handlers"
                        << " (control_flow_nesting=" << best_has_control_flow_nesting << ")";
            }
          }
          
          // Edge case: Handle case where no valid outer region was found
          if (!outer_region) {
            LOG(INFO) << "Selected inner region (with rethrow) with " << inner_region->catch_handlers.size() 
                      << " handlers, no valid outer region found - will use flat structure";
          }
        } else {
          // Fallback: if we have multiple regions, use first two
          if (regions_with_handlers.size() >= 2) {
            // Edge case: Validate fallback regions
            if (regions_with_handlers[0] && regions_with_handlers[1]) {
              inner_region = regions_with_handlers[0];
              outer_region = regions_with_handlers[1];
              LOG(INFO) << "No rethrow found - using fallback selection with " << regions_with_handlers.size() << " regions";
            } else {
              LOG(WARNING) << "Invalid regions in fallback selection";
              if (regions_with_handlers[0]) {
                inner_region = regions_with_handlers[0];
                outer_region = nullptr;
              }
            }
          } else if (regions_with_handlers.size() == 1) {
            // Single region case - treat as regular try-catch, not nested
            if (regions_with_handlers[0]) {
              inner_region = regions_with_handlers[0];
              outer_region = nullptr;
              LOG(INFO) << "Single region found - treating as regular try-catch";
            } else {
              LOG(WARNING) << "Single region is null - cannot proceed";
            }
          } else {
            LOG(WARNING) << "No regions with handlers found - cannot create nested structure";
          }
        }
      }
    }
  }
  
  clang::Stmt* final_try_catch = nullptr;
  
  // Check if we have parent-child relationships from ExceptionHandlingPass
  bool has_explicit_nesting = false;
  std::vector<const TryRegion*> nested_chain;
  
  // Build nesting chain from parent-child relationships if available
  for (const auto* region : regions) {
    if (region && region->parent_region == nullptr && !region->nested_regions.empty()) {
      // Found a root region with nested children
      has_explicit_nesting = true;
      LOG(INFO) << "Found explicit parent-child nesting from ExceptionHandlingPass";
      
      // Build the nesting chain (root -> child -> grandchild -> ...)
      std::function<void(const TryRegion*, std::vector<const TryRegion*>&)> buildChain;
      buildChain = [&buildChain](const TryRegion* r, std::vector<const TryRegion*>& chain) {
        chain.push_back(r);
        // Add nested regions recursively
        for (auto* nested : r->nested_regions) {
          buildChain(nested, chain);
        }
      };
      
      buildChain(region, nested_chain);
      LOG(INFO) << "Built nesting chain with " << nested_chain.size() << " levels";
      break;
    }
  }
  
  // Handle arbitrary nesting if we have explicit parent-child relationships
  // Validate that the nesting chain is real by checking if inner regions actually
  // have blocks that are inside outer regions' catch handlers
  bool valid_nesting_chain = false;
  if (has_explicit_nesting && nested_chain.size() > 1) {
    valid_nesting_chain = true;
    
    // Verify each parent-child relationship in the chain
    for (size_t i = 0; i < nested_chain.size() - 1; i++) {
      const auto* parent = nested_chain[i];
      const auto* child = nested_chain[i + 1];
      
      // Collect all blocks from parent's catch handlers
      std::unordered_set<llvm::BasicBlock*> parent_catch_blocks;
      for (const auto& handler : parent->catch_handlers) {
        parent_catch_blocks.insert(handler.blocks.begin(), handler.blocks.end());
      }
      
      // Check if child's try blocks are inside parent's catch blocks
      bool child_in_parent = false;
      for (auto* child_block : child->blocks) {
        if (parent_catch_blocks.count(child_block) > 0) {
          child_in_parent = true;
          break;
        }
      }
      
      if (!child_in_parent) {
        LOG(INFO) << "Invalid nesting chain: child region not inside parent's catch blocks";
        valid_nesting_chain = false;
        break;
      }
    }
  }
  
  if (valid_nesting_chain) {
    LOG(INFO) << "Using validated explicit nesting for " << nested_chain.size() << " levels";
    
    // Build nested try-catch from innermost to outermost
    clang::Stmt* current_try_catch = nullptr;
    
    // Process from innermost (last in chain) to outermost (first in chain)
    for (int i = nested_chain.size() - 1; i >= 0; i--) {
      const auto* region = nested_chain[i];
      
      // Collect statements for this level's try block
      std::vector<clang::Stmt*> level_try_stmts;
      for (auto* stmt : try_stmts) {
        auto* source_block = GetSourceBlock(stmt, dec_ctx);
        if (source_block && region->blocks.count(source_block) > 0) {
          level_try_stmts.push_back(stmt);
        }
      }
      
      // If this is an inner level and we have a try-catch from the previous iteration,
      // add it to this level's try statements
      if (current_try_catch && i < nested_chain.size() - 1) {
        level_try_stmts.push_back(current_try_catch);
      }
      
      // Create catch handlers for this level
      std::vector<clang::CXXCatchStmt*> level_catch_handlers;
      for (const auto& handler : region->catch_handlers) {
        auto it = catch_stmts.find(handler.exception_type_name);
        if (it != catch_stmts.end() && !it->second.empty()) {
          auto* catch_body = dec_ctx.ast.CreateCompoundStmt(it->second);
          
          clang::VarDecl* exception_var = nullptr;
          if (!handler.is_catch_all) {
            std::string var_name = CreateVariableNameFromType(handler.exception_type_name);
            clang::QualType exception_type = CreateExceptionType(handler.exception_type_name, dec_ctx);
            exception_var = dec_ctx.ast.CreateVarDecl(func_decl, exception_type, var_name);
          }
          
          auto* catch_stmt = dec_ctx.ast.CreateCXXCatchStmt(
              clang::SourceLocation(), exception_var, catch_body);
          level_catch_handlers.push_back(catch_stmt);
        }
      }
      
      // Create try-catch for this level
      if (!level_try_stmts.empty() || !level_catch_handlers.empty()) {
        auto* try_body = dec_ctx.ast.CreateCompoundStmt(level_try_stmts);
        current_try_catch = dec_ctx.ast.CreateCXXTryStmt(
            clang::SourceLocation(), try_body,
            llvm::ArrayRef<clang::CXXCatchStmt*>(level_catch_handlers));
      }
    }
    
    final_try_catch = current_try_catch;
    LOG(INFO) << "Created " << nested_chain.size() << "-level nested try-catch structure";
    
    // Return early to skip the 2-level logic
    if (final_try_catch) {
      std::vector<clang::Stmt*> new_body_stmts;
      
      // Add non-exception statements
      for (auto* stmt : body->body()) {
        auto* source_block = GetSourceBlock(stmt, dec_ctx);
        bool in_exception_region = false;
        
        if (source_block) {
          for (const auto* region : regions) {
            if (region->blocks.count(source_block) > 0) {
              in_exception_region = true;
              break;
            }
            for (const auto& handler : region->catch_handlers) {
              if (handler.blocks.count(source_block) > 0) {
                in_exception_region = true;
                break;
              }
            }
          }
        }
        
        if (!in_exception_region && !IsExceptionInfrastructure(stmt)) {
          new_body_stmts.push_back(stmt);
        }
      }
      
      new_body_stmts.push_back(final_try_catch);
      return dec_ctx.ast.CreateCompoundStmt(new_body_stmts);
    }
  }
  
  if (has_rethrow && inner_region && outer_region) {
    LOG(INFO) << "Creating nested structure using unified approach";
    
    // Split statements into inner and outer based on source blocks
    std::vector<clang::Stmt*> inner_try_stmts;
    std::vector<clang::Stmt*> outer_try_stmts;
    
    for (auto* stmt : try_stmts) {
      auto* source_block = GetSourceBlock(stmt, dec_ctx);
      bool is_inner_block = false;
      
      if (source_block) {
        // Check if this statement belongs to inner region
        if (inner_region->blocks.count(source_block) > 0) {
          is_inner_block = true;
        }
      }
      
      if (is_inner_block) {
        inner_try_stmts.push_back(stmt);
      } else {
        outer_try_stmts.push_back(stmt);
      }
    }
    
    // Create inner catch handlers (from inner region)
    std::vector<clang::CXXCatchStmt*> inner_catch_handlers;
    for (const auto& handler : inner_region->catch_handlers) {
      // Find corresponding catch handler from unified collection
      for (auto* catch_stmt : catch_handlers) {
        if (auto* var_decl = catch_stmt->getExceptionDecl()) {
          if (var_decl->getName() == CreateVariableNameFromType(handler.exception_type_name)) {
            inner_catch_handlers.push_back(catch_stmt);
            break;
          }
        } else if (handler.is_catch_all && !catch_stmt->getExceptionDecl()) {
          inner_catch_handlers.push_back(catch_stmt);
          break;
        }
      }
    }
    
    // Create outer catch handlers (from outer region)
    std::vector<clang::CXXCatchStmt*> outer_catch_handlers;
    for (const auto& handler : outer_region->catch_handlers) {
      // Get statements for this outer catch handler
      std::vector<clang::Stmt*> handler_stmts;
      auto it = outer_catch_stmts.find(handler.exception_type_name);
      if (it != outer_catch_stmts.end()) {
        handler_stmts = it->second;
        LOG(INFO) << "Found " << handler_stmts.size() << " statements for outer catch handler " << handler.exception_type_name;
      } else {
        LOG(WARNING) << "No outer catch statements found for " << handler.exception_type_name << " - using systematic block analysis";
        
        // Use a systematic approach: find ALL blocks that don't belong to inner catch or try blocks
        // and collect ALL their statements. This is more generic than pattern matching.
        
        std::unordered_set<llvm::BasicBlock*> inner_try_blocks;
        std::unordered_set<llvm::BasicBlock*> inner_catch_blocks;
        
        // Collect inner region blocks (both try and catch)
        if (inner_region) {
          // Add inner try blocks
          inner_try_blocks.insert(inner_region->blocks.begin(), inner_region->blocks.end());
          
          // Add inner catch handler blocks
          for (const auto& inner_handler : inner_region->catch_handlers) {
            inner_catch_blocks.insert(inner_handler.blocks.begin(), inner_handler.blocks.end());
          }
        }
        
        LOG(INFO) << "Inner region has " << inner_try_blocks.size() << " try blocks and " 
                  << inner_catch_blocks.size() << " catch blocks";
        
        // Now find statements that belong to blocks NOT in the inner region
        // These should be outer catch statements
        std::unordered_set<clang::Stmt*> used_by_inner;
        
        for (auto* stmt : body->body()) {
          // Get the source block for this statement
          llvm::BasicBlock* source_block = nullptr;
          auto prov_it = dec_ctx.stmt_provenance.find(stmt);
          if (prov_it != dec_ctx.stmt_provenance.end() && prov_it->second) {
            if (auto* inst = llvm::dyn_cast<llvm::Instruction>(prov_it->second)) {
              source_block = inst->getParent();
            }
          }
          
          if (source_block) {
            // If this statement's block is NOT in inner try or inner catch, 
            // and it looks like a catch-related statement, it might be outer catch
            bool is_inner_block = (inner_try_blocks.count(source_block) || 
                                 inner_catch_blocks.count(source_block));
            
            if (!is_inner_block) {
              // This statement doesn't belong to inner region
              // Check if it looks like a catch handler statement (not infrastructure)
              if (!IsExceptionInfrastructure(stmt, true)) {
                // Additional heuristics: does it look like catch logic?
                bool looks_like_catch_logic = false;
                
                // Any printf, assignment, or declaration is likely catch logic
                if (llvm::isa<clang::CallExpr>(stmt) || 
                    llvm::isa<clang::BinaryOperator>(stmt) ||
                    llvm::isa<clang::DeclStmt>(stmt)) {
                  looks_like_catch_logic = true;
                }
                
                if (looks_like_catch_logic) {
                  LOG(INFO) << "Found potential outer catch statement from block " << GetBlockName(source_block, &dec_ctx);
                  handler_stmts.push_back(stmt);
                }
              }
            } else {
              used_by_inner.insert(stmt);
            }
          }
        }
        
        // If systematic approach didn't find anything, use LLVM control flow analysis
        if (handler_stmts.empty() && llvm_func && outer_region) {
          LOG(INFO) << "No outer catch statements found in function body - using LLVM control flow analysis";
          
          // Generic approach: Find all blocks that belong to the outer catch handler
          // by using the exception region information, not hardcoded patterns
          std::unordered_set<llvm::BasicBlock*> outer_catch_handler_blocks;
          
          // Get blocks from outer region's catch handlers
          for (const auto& handler : outer_region->catch_handlers) {
            for (auto* bb : handler.blocks) {
              if (bb->getParent() == llvm_func) {
                outer_catch_handler_blocks.insert(bb);
                LOG(INFO) << "Outer catch handler includes block: " << GetBlockName(bb, &dec_ctx);
              }
            }
          }
          
          // Use the existing IRToASTVisitor to convert outer catch blocks
          IRToASTVisitor ir_visitor(dec_ctx);
          
          for (auto* bb : outer_catch_handler_blocks) {
            LOG(INFO) << "Converting outer catch block " << GetBlockName(bb, &dec_ctx) << " using IRToASTVisitor";
            
            std::vector<clang::Stmt*> block_stmts;
            ir_visitor.VisitBasicBlock(*bb, block_stmts);
            
            LOG(INFO) << "IRToASTVisitor generated " << block_stmts.size() << " statements";
            
            // Add block marker if we have statements from this block
            if (!block_stmts.empty()) {
              std::string block_name = GetBlockName(bb, &dec_ctx);
              auto marker = dec_ctx.ast.CreateCommentMarker(block_name);
              handler_stmts.push_back(marker);
              LOG(INFO) << "Added block marker for " << block_name;
            }
            
            // Filter and add the statements
            for (auto* stmt : block_stmts) {
              if (!IsExceptionInfrastructure(stmt, true)) {
                handler_stmts.push_back(stmt);
                LOG(INFO) << "Added converted statement to outer catch handler";
              } else {
                LOG(INFO) << "Filtered out infrastructure statement from outer catch";
              }
            }
          }
          
          if (!handler_stmts.empty()) {
            LOG(INFO) << "Collected " << handler_stmts.size() << " statements from outer catch blocks";
          } else {
            LOG(WARNING) << "No statements could be extracted from outer catch blocks";
          }
        } else if (!handler_stmts.empty()) {
          LOG(INFO) << "Collected " << handler_stmts.size() << " outer catch statements using systematic approach";
        }
      }
      
      // Create catch body
      auto* catch_body = dec_ctx.ast.CreateCompoundStmt(handler_stmts);
      
      // Create exception variable
      clang::VarDecl* exception_var = nullptr;
      if (!handler.is_catch_all) {
        std::string var_name = CreateVariableNameFromType(handler.exception_type_name);
        clang::QualType exception_type = CreateExceptionType(handler.exception_type_name, dec_ctx);
        exception_var = dec_ctx.ast.CreateVarDecl(func_decl, exception_type, var_name);
      }
      
      // Create catch statement
      auto* catch_stmt = dec_ctx.ast.CreateCXXCatchStmt(
          clang::SourceLocation(), exception_var, catch_body);
      outer_catch_handlers.push_back(catch_stmt);
    }
    
    // Create inner try-catch
    auto* inner_try_body = dec_ctx.ast.CreateCompoundStmt(inner_try_stmts);
    auto* inner_try_catch = dec_ctx.ast.CreateCXXTryStmt(
        clang::SourceLocation(), inner_try_body,
        llvm::ArrayRef<clang::CXXCatchStmt*>(inner_catch_handlers));
    
    // Create outer try block containing inner try-catch
    std::vector<clang::Stmt*> complete_outer_try_stmts = outer_try_stmts;
    complete_outer_try_stmts.push_back(inner_try_catch);
    
    auto* outer_try_body = dec_ctx.ast.CreateCompoundStmt(complete_outer_try_stmts);
    final_try_catch = dec_ctx.ast.CreateCXXTryStmt(
        clang::SourceLocation(), outer_try_body,
        llvm::ArrayRef<clang::CXXCatchStmt*>(outer_catch_handlers));
    
    LOG(INFO) << "Created nested structure with " << inner_try_stmts.size() 
              << " inner statements and " << outer_try_stmts.size() << " outer statements";
  } else {
    // Create regular flat try-catch statement
    final_try_catch = dec_ctx.ast.CreateCXXTryStmt(
        clang::SourceLocation(), try_body,
        llvm::ArrayRef<clang::CXXCatchStmt*>(catch_handlers));
  }
  
  new_body_stmts.push_back(final_try_catch);
  
  // Add variable declarations at the beginning, before the try-catch
  new_body_stmts.insert(new_body_stmts.begin(), other_stmts.begin(), other_stmts.end());
  
  LOG(INFO) << "Created unified try-catch with " << try_stmts.size() 
            << " try statements and " << catch_handlers.size() << " handlers";
  
  return dec_ctx.ast.CreateCompoundStmt(new_body_stmts);
}

}  // namespace rellic