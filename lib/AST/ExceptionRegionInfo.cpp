#include "rellic/AST/ExceptionRegionInfo.h"
#include <glog/logging.h>
#include <llvm/Support/raw_ostream.h>

namespace rellic {

void ExceptionRegionInfo::AddTryRegion(
    llvm::BasicBlock *landingpad,
    const std::unordered_set<llvm::BasicBlock *> &try_blocks) {
  
  std::string landingpad_str;
  llvm::raw_string_ostream landingpad_stream(landingpad_str);
  landingpad->printAsOperand(landingpad_stream, false);
  landingpad_stream.flush();
  
  LOG(INFO) << "ExceptionRegionInfo::AddTryRegion called for landingpad " << landingpad_str 
            << " with " << try_blocks.size() << " try blocks";
  
  try_regions_.emplace_back();
  auto &region = try_regions_.back();
  region.landingpad_block = landingpad;
  region.blocks = try_blocks;
  
  landingpad_to_region_[landingpad] = &region;
  
  for (auto *bb : try_blocks) {
    block_to_try_region_[bb] = &region;
  }
  
  LOG(INFO) << "Total try regions now: " << try_regions_.size();
}

void ExceptionRegionInfo::AddCatchHandler(
    llvm::BasicBlock *landingpad,
    llvm::BasicBlock *handler,
    llvm::Type *exception_type,
    const std::string &type_name,
    llvm::Value *type_info,
    int selector,
    bool is_catch_all,
    const std::unordered_set<llvm::BasicBlock *> &catch_blocks,
    const std::unordered_set<llvm::BasicBlock *> &filtered_blocks,
    const std::vector<std::string> &catch_all_statements) {
  
  auto it = landingpad_to_region_.find(landingpad);
  if (it == landingpad_to_region_.end()) {
    return;
  }
  
  auto *region = it->second;
  region->catch_handlers.emplace_back();
  auto &catch_handler = region->catch_handlers.back();
  catch_handler.handler_block = handler;
  catch_handler.exception_type = exception_type;
  catch_handler.exception_type_name = type_name;
  catch_handler.type_info = type_info;
  catch_handler.selector_value = selector;
  catch_handler.is_catch_all = is_catch_all;
  catch_handler.blocks = catch_blocks;
  catch_handler.filtered_infrastructure_blocks = filtered_blocks;
  catch_handler.catch_all_statements = catch_all_statements;
  
  for (auto *bb : catch_blocks) {
    block_to_catch_handler_[bb] = &catch_handler;
  }
}

bool ExceptionRegionInfo::IsInTryBlock(llvm::BasicBlock *bb) const {
  return block_to_try_region_.find(bb) != block_to_try_region_.end();
}

bool ExceptionRegionInfo::IsInCatchBlock(llvm::BasicBlock *bb) const {
  return block_to_catch_handler_.find(bb) != block_to_catch_handler_.end();
}

TryRegion *ExceptionRegionInfo::GetTryRegion(llvm::BasicBlock *bb) {
  auto it = block_to_try_region_.find(bb);
  if (it != block_to_try_region_.end()) {
    return it->second;
  }
  
  auto catch_it = block_to_catch_handler_.find(bb);
  if (catch_it != block_to_catch_handler_.end()) {
    for (auto &region : try_regions_) {
      for (auto &handler : region.catch_handlers) {
        if (&handler == catch_it->second) {
          return &region;
        }
      }
    }
  }
  
  return nullptr;
}

}  // namespace rellic