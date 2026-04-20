//===-------------- llvm-advisor.cpp - LLVM Advisor -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This is the llvm-advisor code generator driver. It provides a convenient
// command-line interface for generating an assembly file or a relocatable file,
// given LLVM bitcode.
//
//===----------------------------------------------------------------------===//

#include "Config/AdvisorConfig.h"
#include "Core/CapabilityRunner.h"
#include "Core/CompilationManager.h"
#include "Core/ViewerLauncher.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"

namespace {

llvm::cl::OptionCategory AdvisorCategory("llvm-advisor options");

llvm::cl::opt<std::string> GConfigFile("config",
                                       llvm::cl::desc("Configuration file"),
                                       llvm::cl::value_desc("filename"),
                                       llvm::cl::cat(AdvisorCategory));
llvm::cl::opt<std::string> GOutputDir("output-dir",
                                      llvm::cl::desc("Output directory"),
                                      llvm::cl::value_desc("directory"),
                                      llvm::cl::cat(AdvisorCategory));
llvm::cl::opt<bool> GVerbose("verbose", llvm::cl::desc("Verbose output"),
                             llvm::cl::cat(AdvisorCategory));
llvm::cl::opt<bool> GKeepTemps("keep-temps",
                               llvm::cl::desc("Keep temporary files"),
                               llvm::cl::cat(AdvisorCategory));
llvm::cl::opt<bool> GNoProfiler("no-profiler",
                                llvm::cl::desc("Disable profiler"),
                                llvm::cl::cat(AdvisorCategory));

llvm::cl::SubCommand ViewCmd("view", "Compile and launch web viewer");
llvm::cl::SubCommand UnitsCmd("units", "List captured compilation units");
llvm::cl::SubCommand QueryCmd("query", "Query a native capability summary");
llvm::cl::SubCommand MaterializeCmd(
    "materialize", "Materialize one captured representation");
llvm::cl::SubCommand SignalsCmd(
    "signals", "Correlate source and representation signals");
llvm::cl::SubCommand CompareCmd("compare", "Compare native capability results");
llvm::cl::SubCommand RepresentationCapsCmd(
    "representation-capabilities",
    "List supported representation capabilities");

llvm::cl::opt<int> GPort("port",
                         llvm::cl::desc("Web server port (for view command)"),
                         llvm::cl::value_desc("port"), llvm::cl::init(8000),
                         llvm::cl::sub(ViewCmd));
llvm::cl::opt<std::string> GUnitsDataDir(
    "data-dir", llvm::cl::desc("Captured data directory"), llvm::cl::init(".llvm-advisor"),
    llvm::cl::sub(UnitsCmd));
llvm::cl::opt<std::string> GQueryDataDir(
    "data-dir", llvm::cl::desc("Captured data directory"), llvm::cl::init(".llvm-advisor"),
    llvm::cl::sub(QueryCmd));
llvm::cl::opt<std::string> GQueryUnit(
    "unit", llvm::cl::desc("Compilation unit name"), llvm::cl::Required,
    llvm::cl::sub(QueryCmd));
llvm::cl::opt<std::string> GQueryCapability(
    "capability", llvm::cl::desc("Capability identifier"), llvm::cl::Required,
    llvm::cl::sub(QueryCmd));
llvm::cl::opt<std::string> GMaterializeDataDir(
    "data-dir", llvm::cl::desc("Captured data directory"), llvm::cl::init(".llvm-advisor"),
    llvm::cl::sub(MaterializeCmd));
llvm::cl::opt<std::string> GMaterializeUnit(
    "unit", llvm::cl::desc("Compilation unit name"), llvm::cl::Required,
    llvm::cl::sub(MaterializeCmd));
llvm::cl::opt<std::string> GMaterializeSourceRef(
    "source-ref", llvm::cl::desc("Source reference from manifest"),
    llvm::cl::Required, llvm::cl::sub(MaterializeCmd));
llvm::cl::opt<std::string> GMaterializeKind(
    "kind", llvm::cl::desc("Representation kind"), llvm::cl::Required,
    llvm::cl::sub(MaterializeCmd));
llvm::cl::opt<std::string> GCompareBaseDataDir(
    "base-data-dir", llvm::cl::desc("Base captured data directory"),
    llvm::cl::Required, llvm::cl::sub(CompareCmd));
llvm::cl::opt<std::string> GCompareCandidateDataDir(
    "candidate-data-dir", llvm::cl::desc("Candidate captured data directory"),
    llvm::cl::Required, llvm::cl::sub(CompareCmd));
llvm::cl::opt<std::string> GCompareCapability(
    "capability", llvm::cl::desc("Capability identifier"), llvm::cl::Required,
    llvm::cl::sub(CompareCmd));
llvm::cl::opt<std::string> GCompareUnit(
    "unit", llvm::cl::desc("Optional unit name to compare"),
    llvm::cl::init(""), llvm::cl::sub(CompareCmd));
llvm::cl::opt<std::string> GSignalsDataDir(
    "data-dir", llvm::cl::desc("Captured data directory"), llvm::cl::init(".llvm-advisor"),
    llvm::cl::sub(SignalsCmd));
llvm::cl::opt<std::string> GSignalsUnit(
    "unit", llvm::cl::desc("Compilation unit name"), llvm::cl::Required,
    llvm::cl::sub(SignalsCmd));
llvm::cl::opt<std::string> GSignalsSourceRef(
    "source-ref", llvm::cl::desc("Source reference from manifest"),
    llvm::cl::Required, llvm::cl::sub(SignalsCmd));
llvm::cl::opt<std::string> GSignalsKind(
    "kind", llvm::cl::desc("Representation kind or 'source'"), llvm::cl::init("source"),
    llvm::cl::sub(SignalsCmd));

llvm::cl::opt<std::string> GDefaultCompiler(
    llvm::cl::Positional, llvm::cl::desc("<compiler>"), llvm::cl::Required,
    llvm::cl::cat(AdvisorCategory));
llvm::cl::list<std::string> GDefaultCompileArgs(
    llvm::cl::ConsumeAfter, llvm::cl::desc("[compiler-args...]"),
    llvm::cl::cat(AdvisorCategory));
llvm::cl::opt<std::string> GViewCompiler(
    llvm::cl::Positional, llvm::cl::desc("<compiler>"), llvm::cl::Required,
    llvm::cl::sub(ViewCmd));
llvm::cl::list<std::string> GViewCompileArgs(
    llvm::cl::ConsumeAfter, llvm::cl::desc("[compiler-args...]"),
    llvm::cl::sub(ViewCmd));

} // namespace

auto main(int argc, char **argv) -> int {
  llvm::InitLLVM X(argc, argv);
  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();
  llvm::InitializeNativeTargetAsmParser();
  llvm::InitializeNativeTargetDisassembler();

  llvm::cl::HideUnrelatedOptions({&AdvisorCategory});
  llvm::cl::ParseCommandLineOptions(argc, argv, "LLVM Compilation Advisor");

  const bool IsViewCommand = static_cast<bool>(ViewCmd);
  const bool IsUnitsCommand = static_cast<bool>(UnitsCmd);
  const bool IsQueryCommand = static_cast<bool>(QueryCmd);
  const bool IsMaterializeCommand = static_cast<bool>(MaterializeCmd);
  const bool IsSignalsCommand = static_cast<bool>(SignalsCmd);
  const bool IsCompareCommand = static_cast<bool>(CompareCmd);
  const bool IsRepresentationCapsCommand =
      static_cast<bool>(RepresentationCapsCmd);

  auto printNativeJson = [](llvm::Expected<std::string> PayloadOrErr) -> int {
    if (!PayloadOrErr) {
      llvm::errs() << "Error: " << llvm::toString(PayloadOrErr.takeError())
                   << "\n";
      return 1;
    }
    llvm::outs() << *PayloadOrErr << "\n";
    return 0;
  };

  if (IsUnitsCommand)
    return printNativeJson(
        llvm::advisor::CapabilityRunner::listUnitsAsJson(GUnitsDataDir));

  if (IsRepresentationCapsCommand)
    return printNativeJson(
        llvm::advisor::CapabilityRunner::listRepresentationCapabilitiesAsJson());

  if (IsQueryCommand) {
    llvm::json::Object Request;
    Request["data_dir"] = GQueryDataDir;
    Request["capability_id"] = GQueryCapability;
    Request["unit_name"] = GQueryUnit;
    return printNativeJson(llvm::advisor::CapabilityRunner::executeFromJson(
        llvm::formatv("{0}", llvm::json::Value(std::move(Request))).str()));
  }

  if (IsMaterializeCommand) {
    llvm::json::Object Request;
    Request["data_dir"] = GMaterializeDataDir;
    Request["unit_name"] = GMaterializeUnit;
    Request["source_ref"] = GMaterializeSourceRef;
    Request["kind"] = GMaterializeKind;
    return printNativeJson(
        llvm::advisor::CapabilityRunner::materializeRepresentationFromJson(
            llvm::formatv("{0}", llvm::json::Value(std::move(Request))).str()));
  }

  if (IsSignalsCommand) {
    llvm::json::Object Request;
    Request["data_dir"] = GSignalsDataDir;
    Request["unit_name"] = GSignalsUnit;
    Request["source_ref"] = GSignalsSourceRef;
    Request["kind"] = GSignalsKind;
    return printNativeJson(llvm::advisor::CapabilityRunner::correlateSignalsFromJson(
        llvm::formatv("{0}", llvm::json::Value(std::move(Request))).str()));
  }

  if (IsCompareCommand) {
    llvm::json::Object Request;
    Request["base_data_dir"] = GCompareBaseDataDir;
    Request["candidate_data_dir"] = GCompareCandidateDataDir;
    Request["capability_id"] = GCompareCapability;
    if (!GCompareUnit.empty())
      Request["unit_name"] = GCompareUnit;
    return printNativeJson(
        llvm::advisor::CapabilityRunner::compareCapabilitiesFromJson(
            llvm::formatv("{0}", llvm::json::Value(std::move(Request))).str()));
  }

  std::string Compiler = IsViewCommand ? GViewCompiler : GDefaultCompiler;
  const auto &CommandArgs =
      IsViewCommand ? GViewCompileArgs : GDefaultCompileArgs;

  if (Compiler.empty()) {
    llvm::errs() << "error: missing compiler command\n";
    llvm::cl::PrintHelpMessage();
    return 1;
  }

  llvm::SmallVector<std::string, 8> CompilerArgs;
  CompilerArgs.reserve(CommandArgs.size());
  for (const std::string &Arg : CommandArgs)
    CompilerArgs.push_back(Arg);

  // Configure advisor
  llvm::advisor::AdvisorConfig Config;
  if (!GConfigFile.empty()) {
    if (auto Err = Config.loadFromFile(GConfigFile).takeError()) {
      llvm::errs() << "Error loading config: " << llvm::toString(std::move(Err))
                   << "\n";
      return 1;
    }
  }

  if (!GOutputDir.empty()) {
    Config.setOutputDir(GOutputDir);
  } else {
    Config.setOutputDir(".llvm-advisor"); // Default hidden directory
  }

  Config.setVerbose(GVerbose);
  Config.setKeepTemps(GKeepTemps ||
                      IsViewCommand); // Keep temps for view command
  Config.setRunProfiler(!GNoProfiler);

  // Create output directory
  if (auto EC = llvm::sys::fs::create_directories(Config.getOutputDir())) {
    llvm::errs() << "Error creating output directory: " << EC.message() << "\n";
    return 1;
  }

  if (Config.getVerbose()) {
    llvm::outs() << "LLVM Compilation Advisor\n";
    llvm::outs() << "Compiler: " << Compiler << "\n";
    llvm::outs() << "Output: " << Config.getOutputDir() << "\n";
    if (IsViewCommand)
      llvm::outs() << "Mode: Compile and launch web viewer\n";
  }

  // Execute with data collection
  llvm::advisor::CompilationManager Manager(Config);
  auto Result = Manager.executeWithDataCollection(Compiler, CompilerArgs);

  if (!Result) {
    llvm::errs() << "Error: " << llvm::toString(Result.takeError()) << "\n";
    return 1;
  }

  if (Config.getVerbose())
    llvm::outs() << "Compilation completed (exit code: " << *Result << ")\n";

  // If this is a view command and compilation succeeded, launch the web viewer
  if (IsViewCommand && *Result == 0) {
    if (Config.getVerbose())
      llvm::outs() << "Launching web viewer...\n";

    // Convert output directory to absolute path for web viewer
    llvm::SmallString<256> AbsoluteOutputDir;
    if (llvm::sys::path::is_absolute(Config.getOutputDir())) {
      AbsoluteOutputDir = Config.getOutputDir();
    } else {
      llvm::sys::fs::current_path(AbsoluteOutputDir);
      llvm::sys::path::append(AbsoluteOutputDir, Config.getOutputDir());
    }

    auto ViewerResult = llvm::advisor::ViewerLauncher::launch(
        std::string(AbsoluteOutputDir.str()), GPort);
    if (!ViewerResult) {
      llvm::errs() << "Error launching web viewer: "
                   << llvm::toString(ViewerResult.takeError()) << "\n";
      llvm::errs() << "Compilation data is still available in: "
                   << Config.getOutputDir() << "\n";
      return 1;
    }

    return *ViewerResult;
  }

  return *Result;
}
