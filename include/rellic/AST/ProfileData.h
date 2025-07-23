/*
 * Copyright (c) 2021-present, Trail of Bits, Inc.
 * All rights reserved.
 *
 */

#pragma once

#include <unordered_map>
#include <string>
#include <llvm/ProfileData/InstrProfReader.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>

namespace rellic {

class ProfileData {
public:
  ProfileData() = default;
  ~ProfileData() = default;

  // Load profile data from .profdata file (simplified for LLVM 16)
  bool LoadProfileData(const std::string& profdata_path);
  
  // Get execution count for a basic block (simplified implementation)
  uint64_t GetBlockExecutionCount(const llvm::BasicBlock* block) const;
  
  // Get execution count for a function
  uint64_t GetFunctionExecutionCount(const llvm::Function* function) const;
  
  // Check if profile data is available
  bool HasProfileData() const { return profile_loaded_; }
  
  // Get all available execution counts for debugging
  const std::unordered_map<std::string, uint64_t>& GetAllBlockCounts() const {
    return block_execution_counts_;
  }

private:
  bool profile_loaded_ = false;
  
  // Simplified storage - maps block/function identifiers to execution counts
  std::unordered_map<std::string, uint64_t> block_execution_counts_;
  std::unordered_map<std::string, uint64_t> function_execution_counts_;
  
  // Helper to create unique block identifier
  std::string GetBlockIdentifier(const llvm::BasicBlock* block) const;
  
  // Parse profile data file using LLVM APIs
  bool ParseProfileDataFile(const std::string& profdata_path);
  
  // Extract profile data from LLVM reader
  void ExtractProfileDataFromReader(llvm::IndexedInstrProfReader& prof_reader);
};

} // namespace rellic