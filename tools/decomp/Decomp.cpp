/*
 * Copyright (c) 2021-present, Trail of Bits, Inc.
 * All rights reserved.
 *
 * This source code is licensed in accordance with the terms specified in
 * the LICENSE file found in the root directory of this source tree.
 */

#include <clang/Tooling/Tooling.h>
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/raw_ostream.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <clang/AST/PrettyPrinter.h>
#include <clang/AST/StmtVisitor.h>
#include <clang/AST/ASTContext.h>

#include <iostream>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <sstream>
#include <regex>

#include "rellic/BC/Util.h"
#include "rellic/Decompiler.h"
#include "rellic/Version.h"

#ifndef LLVM_VERSION_STRING
#define LLVM_VERSION_STRING LLVM_VERSION_MAJOR << "." << LLVM_VERSION_MINOR
#endif

DEFINE_string(input, "", "Input LLVM bitcode file.");
DEFINE_string(output, "", "Output file.");
DEFINE_string(mapping_output, "test_mapping.json", "Output file for basic block to line mapping.");
DEFINE_bool(disable_z3, false, "Disable Z3 based AST tranformations.");
DEFINE_bool(remove_phi_nodes, false,
            "Remove PHINodes from input bitcode before decompilation.");
DEFINE_bool(lower_switch, false,
            "Remove SwitchInst by lowering them to branches.");
DEFINE_bool(output_mapping, false, "Output basic block to line mapping information.");
DEFINE_bool(enable_exception_try_catch, false,
            "Transform exception handling patterns into C++ try-catch blocks.");

DECLARE_bool(version);

namespace {
static llvm::Optional<llvm::APInt> GetPCMetadata(llvm::Value* value) {
  auto inst{llvm::dyn_cast<llvm::Instruction>(value)};
  if (!inst) {
    return llvm::Optional<llvm::APInt>();
  }

  auto pc{inst->getMetadata("pc")};
  if (!pc) {
    return llvm::Optional<llvm::APInt>();
  }

  auto& cop{pc->getOperand(0U)};
  auto cval{llvm::cast<llvm::ConstantAsMetadata>(cop)->getValue()};
  return llvm::cast<llvm::ConstantInt>(cval)->getValue();
}

void WriteMapping(const rellic::DecompilationResult &result, llvm::raw_ostream &os) {
  // Helper function to escape JSON strings
  auto EscapeJSON = [](const std::string &s) {
    std::string out;
    for (char c : s) {
      switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
          if ('\x00' <= c && c <= '\x1f') {
            char buf[8];
            snprintf(buf, sizeof(buf), "\\u%04x", (int)c);
            out += buf;
          } else {
            out += c;
          }
      }
    }
    return out;
  };

  os << "{\n";
  
  // Write basic block information with IR content
  os << "  \"basic_blocks\": [\n";
  bool first = true;
  for (const auto &[bb_name, llvm_bb] : result.bb_name_to_llvm_bb) {
    if (!first) {
      os << ",\n";
    }
    first = false;
    
    // Get the IR content for this basic block
    std::string ir_content;
    llvm::raw_string_ostream ir_stream(ir_content);
    llvm_bb->print(ir_stream);
    ir_stream.flush();
    
    // Get function name
    std::string func_name = llvm_bb->getParent() ? llvm_bb->getParent()->getName().str() : "unknown";
    
    // Format IR content as an array of lines for better readability
    os << "    {\n"
       << "      \"name\": \"" << EscapeJSON(bb_name) << "\",\n"
       << "      \"function\": \"" << EscapeJSON(func_name) << "\",\n"
       << "      \"ir_content\": [\n";
    
    std::istringstream ir_lines(ir_content);
    std::string line;
    bool first_line = true;
    while (std::getline(ir_lines, line)) {
      if (!first_line) {
        os << ",\n";
      }
      os << "        \"" << EscapeJSON(line) << "\"";
      first_line = false;
    }
    
    os << "\n      ],\n"
       << "      \"is_entry\": " << (result.bb_is_entry.count(bb_name) && result.bb_is_entry.at(bb_name) ? "true" : "false") << "\n"
       << "    }";
  }
  os << "\n  ],\n";

  // Write expression positions
  os << "  \"expressions\": [\n";
  first = true;
  for (const auto &[expr_id, info] : result.expr_positions) {
    if (!first) {
      os << ",\n";
    }
    first = false;
    
    os << "    {\n"
       << "      \"id\": \"" << EscapeJSON(expr_id) << "\",\n"
       << "      \"type\": \"" << EscapeJSON(info.type) << "\",\n"
       << "      \"llvm_ir\": \"" << EscapeJSON(info.llvm_ir) << "\",\n"
       << "      \"start_line\": " << info.start_line << ",\n"
       << "      \"start_col\": " << info.start_col << ",\n" 
       << "      \"end_line\": " << info.end_line << ",\n"
       << "      \"end_col\": " << info.end_col << "\n"
       << "    }";
  }
  os << "\n  ]\n}\n";
}

// Transform BB marker calls into comments
std::string TransformBBMarkers(const std::string& line) {
  // Look for pattern: __builtin_rellic_bb_marker("bb_name");
  std::regex marker_regex(R"(__builtin_rellic_bb_marker\(\"([^\"]+)\"\);)");
  std::smatch match;
  
  if (std::regex_search(line, match, marker_regex)) {
    // Extract the BB name
    std::string bb_name = match[1].str();
    // Replace the entire line with a comment
    return "  /* BB: " + bb_name + " */";
  }
  
  return line;
}
}  // namespace

static void SetVersion(void) {
  std::stringstream version;

  auto vs = rellic::Version::GetVersionString();
  if (0 == vs.size()) {
    vs = "unknown";
  }
  version << vs << "\n";
  if (!rellic::Version::HasVersionData()) {
    version << "No extended version information found!\n";
  } else {
    version << "Commit Hash: " << rellic::Version::GetCommitHash() << "\n";
    version << "Commit Date: " << rellic::Version::GetCommitDate() << "\n";
    version << "Last commit by: " << rellic::Version::GetAuthorName() << " ["
            << rellic::Version::GetAuthorEmail() << "]\n";
    version << "Commit Subject: [" << rellic::Version::GetCommitSubject()
            << "]\n";
    version << "\n";
    if (rellic::Version::HasUncommittedChanges()) {
      version << "Uncommitted changes were present during build.\n";
    } else {
      version << "All changes were committed prior to building.\n";
    }
  }
  version << "Using LLVM " << LLVM_VERSION_STRING << std::endl;

  google::SetVersionString(version.str());
}

int main(int argc, char* argv[]) {
  std::stringstream usage;
  usage << std::endl
        << std::endl
        << "  " << argv[0] << " \\" << std::endl
        << "    --input INPUT_BC_FILE \\" << std::endl
        << "    --output OUTPUT_C_FILE \\" << std::endl
        << "    [--output-mapping] \\" << std::endl
        << "    [--mapping-output MAPPING_JSON_FILE] \\" << std::endl
        << std::endl
        // Print the version and exit.
        << "    [--version]" << std::endl
        << std::endl;

  google::InitGoogleLogging(argv[0]);
  google::InstallFailureSignalHandler();
  google::SetUsageMessage(usage.str());
  SetVersion();
  google::ParseCommandLineFlags(&argc, &argv, true);

  LOG_IF(ERROR, FLAGS_input.empty())
      << "Must specify the path to an input LLVM bitcode file.";

  LOG_IF(ERROR, FLAGS_output.empty())
      << "Must specify the path to an output C file.";

  if (FLAGS_input.empty() || FLAGS_output.empty()) {
    std::cerr << google::ProgramUsage();
    return EXIT_FAILURE;
  }

  std::unique_ptr<llvm::LLVMContext> llvm_ctx(new llvm::LLVMContext);
  auto module{std::unique_ptr<llvm::Module>(
      rellic::LoadModuleFromFile(llvm_ctx.get(), FLAGS_input))};

  std::error_code ec;
  llvm::raw_fd_ostream output(FLAGS_output, ec);
  CHECK(!ec) << "Failed to create output file: " << ec.message();

  rellic::DecompilationOptions opts{};
  opts.lower_switches = FLAGS_lower_switch;
  opts.remove_phi_nodes = FLAGS_remove_phi_nodes;
  opts.enable_exception_try_catch = FLAGS_enable_exception_try_catch;

  auto result{rellic::Decompile(std::move(module), std::move(opts))};
  if (result.Succeeded()) {
    auto value{result.TakeValue()};
    if (FLAGS_output_mapping) {
      llvm::raw_fd_ostream map_output(FLAGS_mapping_output, ec, llvm::sys::fs::OF_None);
      if (ec) {
        LOG(ERROR) << "Failed to create mapping file: " << ec.message();
        return EXIT_FAILURE;
      }
      WriteMapping(value, map_output);
    }

    // Print AST to a buffer first
    std::string buffer;
    llvm::raw_string_ostream temp_output(buffer);
    value.ast->getASTContext().getTranslationUnitDecl()->print(temp_output);
    temp_output.flush();
    
    // Process output line by line, transforming BB markers
    std::istringstream reader(buffer);
    std::string line;
    while (std::getline(reader, line)) {
      // Transform BB marker calls into comments
      std::string transformed = TransformBBMarkers(line);
      output << transformed << "\n";
    }
  } else {
    LOG(FATAL) << result.TakeError().message;
  }

  google::ShutDownCommandLineFlags();
  google::ShutdownGoogleLogging();

  return EXIT_SUCCESS;
}

