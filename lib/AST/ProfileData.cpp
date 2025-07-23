/*
 * Copyright (c) 2021-present, Trail of Bits, Inc.
 * All rights reserved.
 *
 */

#include "rellic/AST/ProfileData.h"

#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/IR/Instructions.h>
#include <llvm/ADT/StringRef.h>
#include <glog/logging.h>
#include <fstream>
#include <regex>

namespace rellic {

bool ProfileData::LoadProfileData(const std::string& profdata_path) {
  if (profdata_path.empty()) {
    LOG(INFO) << "No profile data path provided";
    return false;
  }

  LOG(INFO) << "Loading profile data from: " << profdata_path;
  
  // Try to parse the actual profdata file
  if (!ParseProfileDataFile(profdata_path)) {
    LOG(ERROR) << "Failed to parse profile data file: " << profdata_path;
    return false;
  }
  
  profile_loaded_ = true;
  LOG(INFO) << "Successfully loaded profile data";
  LOG(INFO) << "Found execution counts for " << block_execution_counts_.size() << " blocks";
  LOG(INFO) << "Found execution counts for " << function_execution_counts_.size() << " functions";
  
  return true;
}

void ProfileData::ExtractProfileDataFromReader(llvm::IndexedInstrProfReader& prof_reader) {
  // Iterate through all profile records in the reader
  // In LLVM 16, the iterator returns NamedInstrProfRecord directly
  for (auto& record : prof_reader) {
    std::string function_name = record.Name.str();
    
    // Store function execution count (entry block count)
    if (!record.Counts.empty()) {
      uint64_t function_count = record.Counts[0]; // Entry block count
      function_execution_counts_[function_name] = function_count;
    }
    
    // Process basic block execution counts
    for (size_t i = 0; i < record.Counts.size(); ++i) {
      uint64_t count = record.Counts[i];
      
      // Create block identifier: function_name_bb_index
      std::string block_id = function_name + "_bb_" + std::to_string(i);
      block_execution_counts_[block_id] = count;
    }
  }
  
  LOG(INFO) << "Loaded profile data for " << function_execution_counts_.size() 
            << " functions and " << block_execution_counts_.size() << " basic blocks";
}

bool ProfileData::ParseProfileDataFile(const std::string& profdata_path) {
  // Load the profile data file using LLVM APIs
  auto buffer_or_error = llvm::MemoryBuffer::getFile(profdata_path);
  if (auto error = buffer_or_error.getError()) {
    LOG(ERROR) << "Failed to read profile data file: " << profdata_path 
               << " - " << error.message();
    return false;
  }

  auto buffer = std::move(buffer_or_error.get());
  LOG(INFO) << "Loaded profile data file (" << buffer->getBufferSize() << " bytes)";
  
  // Try to create profile reader using LLVM 16 APIs
  auto reader_or_error = llvm::IndexedInstrProfReader::create(std::move(buffer));
  if (!reader_or_error) {
    // Handle error - convert to string for logging
    std::string error_str;
    llvm::raw_string_ostream error_stream(error_str);
    error_stream << reader_or_error.takeError();
    LOG(ERROR) << "Failed to create profile reader: " << error_str;
    return false;
  }

  auto prof_reader = std::move(reader_or_error.get());
  LOG(INFO) << "Successfully created profile reader";
  
  // Extract profile data from reader
  ExtractProfileDataFromReader(*prof_reader);
  
  return true;
}

std::string ProfileData::GetBlockIdentifier(const llvm::BasicBlock* block) const {
  if (!block || !block->getParent()) {
    return "";
  }
  
  const llvm::Function* func = block->getParent();
  std::string func_name = func->getName().str();
  
  // Find block index within function
  size_t block_index = 0;
  for (const auto& bb : *func) {
    if (&bb == block) {
      break;
    }
    block_index++;
  }
  
  return func_name + "_bb_" + std::to_string(block_index);
}

uint64_t ProfileData::GetBlockExecutionCount(const llvm::BasicBlock* block) const {
  if (!profile_loaded_ || !block) {
    return 0;
  }
  
  std::string block_id = GetBlockIdentifier(block);
  
  // Try exact match first
  auto it = block_execution_counts_.find(block_id);
  if (it != block_execution_counts_.end()) {
    return it->second;
  }
  
  // Try to match by function name and block pattern
  std::string func_name = block->getParent()->getName().str();
  
  // Try matching with function name - improved logic for block number mismatches
  std::string pattern_id = func_name + "_bb_";
  std::vector<std::pair<std::string, uint64_t>> function_blocks;
  
  // Collect all blocks for this function
  for (const auto& [key, count] : block_execution_counts_) {
    if (key.find(pattern_id) == 0) {
      function_blocks.push_back({key, count});
    }
  }
  
  if (!function_blocks.empty()) {
    // Sort by block number to get consistent mapping
    std::sort(function_blocks.begin(), function_blocks.end());
    
    // Extract requested block index from Rellic's block_id
    std::regex rellic_regex(pattern_id + "(\\d+)");
    std::smatch rellic_match;
    
    if (std::regex_search(block_id, rellic_match, rellic_regex)) {
      int requested_index = std::stoi(rellic_match[1].str());
      
      // Try direct mapping first
      for (const auto& [key, count] : function_blocks) {
        std::regex profile_regex(pattern_id + "(\\d+)");
        std::smatch profile_match;
        if (std::regex_search(key, profile_match, profile_regex)) {
          int profile_index = std::stoi(profile_match[1].str());
          if (profile_index == requested_index) {
            return count;
          }
        }
      }
      
      // If no direct mapping, try heuristic mapping based on execution patterns
      // For simple cases, map high-execution blocks to likely loop bodies
      if (function_blocks.size() >= 2) {
        // Find the highest execution count block (likely a loop body)
        auto max_block = std::max_element(function_blocks.begin(), function_blocks.end(),
          [](const auto& a, const auto& b) { return a.second < b.second; });
        
        // If Rellic is asking for blocks 2,3,4 but we have 0,1, map the non-entry blocks to the loop body
        if (requested_index > 0 && max_block->second > function_blocks[0].second) {
          return max_block->second;
        }
      }
    }
  }
  
  // Default to a small execution count for unmapped blocks
  return 1;
}

uint64_t ProfileData::GetFunctionExecutionCount(const llvm::Function* function) const {
  if (!profile_loaded_ || !function) {
    return 0;
  }
  
  std::string func_name = function->getName().str();
  auto it = function_execution_counts_.find(func_name);
  
  if (it != function_execution_counts_.end()) {
    return it->second;
  }
  
  // Try pattern matching for mangled names
  for (const auto& [key, count] : function_execution_counts_) {
    if (func_name.find(key) != std::string::npos) {
      return count;
    }
  }
  
  return 0;
}

} // namespace rellic