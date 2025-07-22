#pragma once

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Type.h>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rellic {

struct CatchHandler {
  llvm::BasicBlock *handler_block;
  llvm::Type *exception_type;
  std::string exception_type_name;  // Demangled C++ type name
  llvm::Value *type_info;           // Global typeinfo variable
  int selector_value;               // Selector value for this handler
  bool is_catch_all;               // True for catch(...)
  std::unordered_set<llvm::BasicBlock *> blocks;
  
  // Store blocks that were filtered out as infrastructure/dispatch
  std::unordered_set<llvm::BasicBlock *> filtered_infrastructure_blocks;
  
  // Store pre-generated statements for catch-all handlers
  std::vector<std::string> catch_all_statements;
};

struct TryRegion {
  std::unordered_set<llvm::BasicBlock *> blocks;
  std::vector<CatchHandler> catch_handlers;
  llvm::BasicBlock *landingpad_block;
  
  // For nested exceptions: parent region that contains this region
  TryRegion* parent_region = nullptr;
  // Child regions nested inside this region's catch handlers
  std::vector<TryRegion*> nested_regions;
};

class ExceptionRegionInfo {
 public:
  void AddTryRegion(llvm::BasicBlock *landingpad, 
                    const std::unordered_set<llvm::BasicBlock *> &try_blocks);
  
  void AddCatchHandler(llvm::BasicBlock *landingpad,
                       llvm::BasicBlock *handler,
                       llvm::Type *exception_type,
                       const std::string &type_name,
                       llvm::Value *type_info,
                       int selector,
                       bool is_catch_all,
                       const std::unordered_set<llvm::BasicBlock *> &catch_blocks,
                       const std::unordered_set<llvm::BasicBlock *> &filtered_blocks = {},
                       const std::vector<std::string> &catch_all_statements = {});
  
  const std::vector<TryRegion> &GetTryRegions() const { return try_regions_; }
  std::vector<TryRegion> &GetMutableTryRegions() { return try_regions_; }
  
  bool IsInTryBlock(llvm::BasicBlock *bb) const;
  bool IsInCatchBlock(llvm::BasicBlock *bb) const;
  TryRegion *GetTryRegion(llvm::BasicBlock *bb);
  
 private:
  std::vector<TryRegion> try_regions_;
  std::unordered_map<llvm::BasicBlock *, TryRegion *> landingpad_to_region_;
  std::unordered_map<llvm::BasicBlock *, TryRegion *> block_to_try_region_;
  std::unordered_map<llvm::BasicBlock *, CatchHandler *> block_to_catch_handler_;
};

}  // namespace rellic