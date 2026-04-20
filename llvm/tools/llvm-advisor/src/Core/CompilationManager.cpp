//===---------------- CompilationManager.cpp - LLVM Advisor ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This is the CompilationManager code generator driver. It provides a
// convenient command-line interface for generating an assembly file or a
// relocatable file, given LLVM bitcode.
//
//===----------------------------------------------------------------------===//

#include "CompilationManager.h"
#include "../Detection/UnitDetector.h"
#include "../Utils/FileClassifier.h"
#include "../Utils/FileManager.h"
#include "../Utils/UnitMetadata.h"
#include "CommandAnalyzer.h"
#include "DataExtractor.h"
#include "UnitIdentity.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <optional>
#include <unordered_set>

namespace llvm {
namespace advisor {

namespace {

bool hasSourceInputs(const BuildContext &Context) {
  for (const auto &InputFile : Context.inputFiles) {
    llvm::StringRef InputRef(InputFile);
    if (InputRef.ends_with_insensitive(".c") ||
        InputRef.ends_with_insensitive(".cc") ||
        InputRef.ends_with_insensitive(".cpp") ||
        InputRef.ends_with_insensitive(".cxx") ||
        InputRef.ends_with_insensitive(".m") ||
        InputRef.ends_with_insensitive(".mm"))
      return true;
  }
  return false;
}

bool shouldForwardInvocation(const BuildContext &Context) {
  if (Context.phase == BuildPhase::Archiving)
    return true;

  if (Context.phase != BuildPhase::Linking)
    return false;

  return !hasSourceInputs(Context);
}

} // namespace

CompilationManager::CompilationManager(const AdvisorConfig &Config)
    : config(Config), buildExecutor(Config) {

  // Get current working directory first
  llvm::SmallString<256> CurrentDir;
  llvm::sys::fs::current_path(CurrentDir);
  initialWorkingDir = std::string(CurrentDir.str());

  // Create temp directory with proper error handling
  llvm::SmallString<128> TempDirPath;
  if (auto EC =
          llvm::sys::fs::createUniqueDirectory("llvm-advisor", TempDirPath)) {
    // Use timestamp for temp folder naming
    auto Now = std::chrono::system_clock::now();
    auto Timestamp =
        std::chrono::duration_cast<std::chrono::seconds>(Now.time_since_epoch())
            .count();
    tempDir = "/tmp/llvm-advisor-" + std::to_string(Timestamp);
    llvm::sys::fs::create_directories(tempDir);
  } else
    tempDir = std::string(TempDirPath.str());

  // Ensure the directory actually exists
  if (!llvm::sys::fs::exists(tempDir))
    llvm::sys::fs::create_directories(tempDir);

  if (Config.getVerbose())
    llvm::outs() << "Using temporary directory: " << tempDir << "\n";

  // Initialize unit metadata tracking
  llvm::SmallString<256> OutputDirPath;
  if (llvm::sys::path::is_absolute(Config.getOutputDir())) {
    OutputDirPath = Config.getOutputDir();
  } else {
    OutputDirPath = initialWorkingDir;
    llvm::sys::path::append(OutputDirPath, Config.getOutputDir());
  }

  unitMetadata = std::make_unique<utils::UnitMetadata>(OutputDirPath.str());
  if (auto Err = unitMetadata->loadMetadata()) {
    if (Config.getVerbose()) {
      llvm::errs() << "Failed to load metadata: "
                   << llvm::toString(std::move(Err)) << "\n";
    }
  }
}

CompilationManager::~CompilationManager() {
  if (!config.getKeepTemps() && llvm::sys::fs::exists(tempDir))
    llvm::sys::fs::remove_directories(tempDir);
}

llvm::Expected<int> CompilationManager::executeWithDataCollection(
    const std::string &Compiler,
    const llvm::SmallVectorImpl<std::string> &Args) {

  // Analyze the build command
  CommandAnalyzer Analyzer(Compiler, Args);
  BuildContext BuildContext = Analyzer.analyze();

  if (config.getVerbose())
    llvm::outs() << "Build phase: " << static_cast<int>(BuildContext.phase)
                 << "\n";

  auto writeForwardedInvocationSummary =
      [&](llvm::StringRef Status, int ExitCode) -> llvm::Error {
    llvm::SmallString<256> OutputDirPath;
    if (llvm::sys::path::is_absolute(config.getOutputDir())) {
      OutputDirPath = config.getOutputDir();
    } else {
      OutputDirPath = initialWorkingDir;
      llvm::sys::path::append(OutputDirPath, config.getOutputDir());
    }

    if (auto EC = llvm::sys::fs::create_directories(OutputDirPath))
      return llvm::createStringError(
          EC, "Failed to create output directory for run summary: " +
                  OutputDirPath.str());

    llvm::json::Array CommandArgs;
    for (const auto &Arg : Args)
      CommandArgs.emplace_back(Arg);

    llvm::json::Object Summary;
    Summary["schema_version"] = 1;
    Summary["status"] = Status.str();
    Summary["compiler"] = Compiler;
    Summary["exit_code"] = static_cast<int64_t>(ExitCode);
    Summary["output_dir"] = std::string(OutputDirPath.str());
    Summary["working_directory"] = initialWorkingDir;
    Summary["build_phase"] = static_cast<int64_t>(BuildContext.phase);
    Summary["forwarded_invocation"] = true;
    Summary["representation_count"] = static_cast<int64_t>(0);
    Summary["unit_count"] = static_cast<int64_t>(0);
    Summary["organization_failed"] = false;
    Summary["units"] = llvm::json::Array();
    Summary["command_args"] = std::move(CommandArgs);

    llvm::SmallString<256> ManifestPath(OutputDirPath);
    llvm::sys::path::append(ManifestPath, "run-summary.json");

    std::error_code EC;
    llvm::raw_fd_ostream ManifestFile(ManifestPath, EC);
    if (EC)
      return llvm::createStringError(
          EC, "Failed to create run summary manifest: " + ManifestPath.str());

    ManifestFile << llvm::formatv("{0:2}", llvm::json::Value(std::move(Summary)))
                 << "\n";
    return llvm::Error::success();
  };

  // Forward pure link/archive steps unchanged. Invocations like
  // "clang++ foo.cpp" still contain real compile work and should collect data.
  if (shouldForwardInvocation(BuildContext)) {
    auto ExecResult = buildExecutor.execute(Compiler, Args, BuildContext, tempDir);
    if (!ExecResult)
      return ExecResult;

    if (auto Err = writeForwardedInvocationSummary("success", *ExecResult)) {
      if (config.getVerbose())
        llvm::errs() << "Failed to write run summary manifest: "
                     << llvm::toString(std::move(Err)) << "\n";
    } else if (config.getVerbose()) {
      llvm::SmallString<256> ManifestPath;
      if (llvm::sys::path::is_absolute(config.getOutputDir())) {
        ManifestPath = config.getOutputDir();
      } else {
        ManifestPath = initialWorkingDir;
        llvm::sys::path::append(ManifestPath, config.getOutputDir());
      }
      llvm::sys::path::append(ManifestPath, "run-summary.json");
      llvm::outs() << "Run summary: " << ManifestPath << "\n";
    }

    return *ExecResult;
  }

  std::string ArtifactRoot = tempDir;

  auto PreparedBuildOrErr = buildExecutor.prepareBuild(
      Compiler, Args, BuildContext, tempDir, ArtifactRoot);
  if (!PreparedBuildOrErr)
    return PreparedBuildOrErr.takeError();

  auto PreparedBuild = std::move(*PreparedBuildOrErr);

  // ── Analysis phase (pre-instrumentation)
  // ──────────────────────────────────── Build a non-instrumented compilation
  // from the original user args so that phase refinement and unit detection see
  // clean, unmodified flags.  The result is used for analysis only and must
  // never be executed.
  std::optional<BuildExecutor::PreparedBuild> OriginalBuild;
  {
    auto OrErr = buildExecutor.buildOriginalCompilation(Compiler, Args);
    if (OrErr)
      OriginalBuild = std::move(*OrErr);
    else
      llvm::consumeError(OrErr.takeError()); // non-fatal; fall back to nullptr
  }
  const clang::driver::Compilation *OriginalCompilation =
      (OriginalBuild && OriginalBuild->UsesDriver)
          ? OriginalBuild->Compilation.get()
          : nullptr;

  Analyzer.refineWithCompilation(BuildContext, OriginalCompilation);
  if (!BuildContext.outputFiles.empty()) {
    const std::string &PrimaryOutput = BuildContext.outputFiles.front();
    for (auto &Site : BuildContext.coverageSites)
      if (Site.instrumentedBinary.empty())
        Site.instrumentedBinary = PrimaryOutput;
  }
  if (!BuildContext.coverageSites.empty())
    coverageIngestion.registerSites(BuildContext.coverageSites);

  UnitDetector Detector(config);
  auto DetectedUnits =
      Detector.detectUnits(Compiler, Args, OriginalCompilation);

  // The original compilation is no longer needed — release it before executing
  // the (potentially large) instrumented build.
  OriginalBuild.reset();

  if (!DetectedUnits)
    return DetectedUnits.takeError();

  llvm::SmallVector<std::unique_ptr<CompilationUnit>, 4> Units;
  for (auto &UnitInfo : *DetectedUnits) {
    Units.push_back(std::make_unique<CompilationUnit>(UnitInfo, tempDir));

    unitMetadata->registerUnit(UnitInfo.name);
  }

  auto ExecResult = buildExecutor.executePreparedBuild(PreparedBuild);
  if (!ExecResult)
    return ExecResult;
  int ExitCode = *ExecResult;

  coverageIngestion.processOnce();
  coverageIngestion.startWatching();

  registerBuildArtifacts(BuildContext, Units);

  // Extract additional data
  DataExtractor Extractor(config);
  for (auto &Unit : Units) {
    if (auto Err = Extractor.extractAllData(*Unit, tempDir)) {
      if (config.getVerbose())
        llvm::errs() << "Data extraction failed: "
                     << llvm::toString(std::move(Err)) << "\n";
      // Mark unit as failed if data extraction fails
      unitMetadata->updateUnitStatus(Unit->getName(), "failed");
    } else {
      // Update unit metadata with file counts and artifact types
      const auto &GeneratedFiles = Unit->getAllGeneratedFiles();
      size_t TotalFiles = 0;
      for (const auto &Category : GeneratedFiles) {
        TotalFiles += Category.second.size();
        unitMetadata->addArtifactType(Unit->getName(), Category.first);
      }
      unitMetadata->updateUnitFileCount(Unit->getName(), TotalFiles);
    }
  }

  auto writeRunSummaryManifest =
      [&](llvm::StringRef Status, bool HadOrganizationFailure) -> llvm::Error {
    llvm::SmallString<256> OutputDirPath;
    if (llvm::sys::path::is_absolute(config.getOutputDir())) {
      OutputDirPath = config.getOutputDir();
    } else {
      OutputDirPath = initialWorkingDir;
      llvm::sys::path::append(OutputDirPath, config.getOutputDir());
    }

    llvm::json::Array UnitSummaries;
    size_t TotalCollectedFiles = 0;
    for (const auto &Unit : Units) {
      llvm::json::Object Categories;
      size_t UnitFileCount = 0;
      for (const auto &Category : Unit->getAllGeneratedFiles()) {
        Categories[Category.first] =
            static_cast<int64_t>(Category.second.size());
        UnitFileCount += Category.second.size();
      }

      llvm::json::Array Sources;
      for (const auto &Source : Unit->getInfo().sources)
        Sources.emplace_back(Source.path);

      llvm::json::Object UnitSummary;
      UnitSummary["name"] = Unit->getName();
      UnitSummary["primary_source"] = Unit->getPrimarySource();
      UnitSummary["source_count"] =
          static_cast<int64_t>(Unit->getInfo().sources.size());
      UnitSummary["sources"] = std::move(Sources);
      UnitSummary["representation_count"] = static_cast<int64_t>(UnitFileCount);
      UnitSummary["representation_categories"] = std::move(Categories);
      UnitSummary["output_object"] = Unit->getInfo().outputObject;
      UnitSummary["output_executable"] = Unit->getInfo().outputExecutable;
      UnitSummaries.emplace_back(std::move(UnitSummary));
      TotalCollectedFiles += UnitFileCount;
    }

    llvm::json::Object Summary;
    Summary["schema_version"] = 1;
    Summary["status"] = Status.str();
    Summary["compiler"] = Compiler;
    Summary["exit_code"] = static_cast<int64_t>(ExitCode);
    Summary["output_dir"] = std::string(OutputDirPath.str());
    Summary["unit_count"] = static_cast<int64_t>(Units.size());
    Summary["representation_count"] = static_cast<int64_t>(TotalCollectedFiles);
    Summary["organization_failed"] = HadOrganizationFailure;
    Summary["working_directory"] = initialWorkingDir;
    Summary["units"] = std::move(UnitSummaries);

    llvm::SmallString<256> ManifestPath(OutputDirPath);
    llvm::sys::path::append(ManifestPath, "run-summary.json");

    std::error_code EC;
    llvm::raw_fd_ostream ManifestFile(ManifestPath, EC);
    if (EC)
      return llvm::createStringError(
          EC, "Failed to create run summary manifest: " + ManifestPath.str());

    ManifestFile << llvm::formatv("{0:2}", llvm::json::Value(std::move(Summary)))
                 << "\n";
    return llvm::Error::success();
  };

  bool HadOrganizationFailure = false;
  if (auto Err = organizeOutput(Units)) {
    HadOrganizationFailure = true;
    if (config.getVerbose())
      llvm::errs() << "Output organization failed: "
                   << llvm::toString(std::move(Err)) << "\n";
    // Mark units as failed if output organization fails
    for (auto &Unit : Units)
      unitMetadata->updateUnitStatus(Unit->getName(), "failed");
  } else {
    // Mark units as completed on successful organization
    for (auto &Unit : Units)
      unitMetadata->updateUnitStatus(Unit->getName(), "completed");
  }

  if (auto Err = unitMetadata->saveMetadata()) {
    if (config.getVerbose())
      llvm::errs() << "Failed to save metadata: "
                   << llvm::toString(std::move(Err)) << "\n";
  }

  if (auto Err = writeRunSummaryManifest(
          HadOrganizationFailure ? "partial-success" : "success",
          HadOrganizationFailure)) {
    if (config.getVerbose())
      llvm::errs() << "Failed to write run summary manifest: "
                   << llvm::toString(std::move(Err)) << "\n";
  } else if (config.getVerbose()) {
    llvm::SmallString<256> ManifestPath;
    if (llvm::sys::path::is_absolute(config.getOutputDir())) {
      ManifestPath = config.getOutputDir();
    } else {
      ManifestPath = initialWorkingDir;
      llvm::sys::path::append(ManifestPath, config.getOutputDir());
    }
    llvm::sys::path::append(ManifestPath, "run-summary.json");
    llvm::outs() << "Run summary: " << ManifestPath << "\n";
  }

  // Clean up leaked files from source directory
  cleanupLeakedFiles();

  return ExitCode;
}

void CompilationManager::registerBuildArtifacts(
    const BuildContext &Ctx,
    llvm::SmallVectorImpl<std::unique_ptr<CompilationUnit>> &Units) {
  if (Units.empty())
    return;

  FileClassifier Classifier;
  auto *Unit = Units.front().get();

  for (const auto &Path : Ctx.expectedGeneratedFiles) {
    if (!llvm::sys::fs::exists(Path))
      continue;
    if (!Classifier.shouldCollect(Path))
      continue;

    auto Classification = Classifier.classifyFile(Path);
    Unit->addGeneratedFile(Classification.category, Path);
  }
}

llvm::Error CompilationManager::organizeOutput(
    const llvm::SmallVectorImpl<std::unique_ptr<CompilationUnit>> &Units) {
  // Resolve output directory as absolute path from initial working directory
  llvm::SmallString<256> OutputDirPath;
  if (llvm::sys::path::is_absolute(config.getOutputDir())) {
    OutputDirPath = config.getOutputDir();
  } else {
    OutputDirPath = initialWorkingDir;
    llvm::sys::path::append(OutputDirPath, config.getOutputDir());
  }

  std::string OutputDir = std::string(OutputDirPath.str());

  if (config.getVerbose())
    llvm::outs() << "Output directory: " << OutputDir << "\n";

  auto normalizeSourcePath = [&](llvm::StringRef SourcePath) -> std::string {
    if (SourcePath.empty())
      return "";
    llvm::SmallString<256> AbsolutePath(SourcePath);
    if (!llvm::sys::path::is_absolute(AbsolutePath)) {
      llvm::SmallString<256> WorkingDir(initialWorkingDir);
      llvm::sys::path::append(WorkingDir, AbsolutePath);
      AbsolutePath = WorkingDir;
    }
    llvm::sys::path::remove_dots(AbsolutePath, /*remove_dot_dot=*/true);
    return std::string(AbsolutePath.str());
  };

  auto makeSourceRef = [&](llvm::StringRef SourcePath) -> std::string {
    std::string Absolute = normalizeSourcePath(SourcePath);
    llvm::StringRef Root(initialWorkingDir);
    llvm::StringRef PathRef(Absolute);
    if (PathRef.starts_with(Root) &&
        (PathRef.size() == Root.size() || PathRef[Root.size()] == '/')) {
      llvm::StringRef Relative = PathRef.drop_front(Root.size());
      return Relative.trim("/").str();
    }
    return Absolute;
  };

  // Generate timestamp for this compilation run
  auto Now = std::chrono::system_clock::now();
  auto TimeT = std::chrono::system_clock::to_time_t(Now);
  auto Tm = *std::localtime(&TimeT);

  char TimestampStr[20];
  std::strftime(TimestampStr, sizeof(TimestampStr), "%Y%m%d_%H%M%S", &Tm);

  // Move collected files to organized structure
  for (const auto &Unit : Units) {
    // Create base unit directory if it doesn't exist
    std::string BaseUnitDir = OutputDir + "/" + Unit->getName();
    llvm::sys::fs::create_directories(BaseUnitDir);

    // Create timestamped run directory
    std::string RunDirName = Unit->getName() + "_" + std::string(TimestampStr);
    std::string UnitDir = BaseUnitDir + "/" + RunDirName;

    if (config.getVerbose())
      llvm::outs() << "Creating run directory: " << UnitDir << "\n";

    // Create timestamped run directory
    if (auto EC = llvm::sys::fs::create_directories(UnitDir)) {
      if (config.getVerbose()) {
        llvm::errs() << "Warning: Could not create run directory: " << UnitDir
                     << "\n";
      }
      continue; // Skip if we can't create the directory
    }

    const auto &GeneratedFiles = Unit->getAllGeneratedFiles();
    llvm::json::Array ManifestRepresentations;
    auto inferRepresentationKind = [](llvm::StringRef Category) -> llvm::StringRef {
      if (Category == "ir")
        return "llvm-ir";
      if (Category == "assembly")
        return "assembly";
      if (Category == "objdump")
        return "objdump";
      if (Category == "ast-json")
        return "clang-ast";
      if (Category == "preprocessed")
        return "preprocessed-source";
      if (Category == "macro-expansion")
        return "macro-state";
      if (Category == "remarks")
        return "optimization-remarks";
      if (Category == "diagnostics")
        return "diagnostics";
      if (Category == "time-trace")
        return "time-trace";
      if (Category == "runtime-trace")
        return "runtime-trace";
      if (Category == "debug")
        return "debug-info";
      if (Category == "sources")
        return "source-copy";
      return "derived-artifact";
    };
    auto inferRepresentationVariant = [](llvm::StringRef Category) -> llvm::StringRef {
      if (Category == "ir")
        return "frontend";
      if (Category == "assembly")
        return "target-codegen";
      if (Category == "objdump")
        return "object-view";
      if (Category == "debug")
        return "dwarf-text";
      return "default";
    };
    auto inferContentType = [](llvm::StringRef Category,
                               llvm::StringRef FileName) -> llvm::StringRef {
      if (Category == "remarks")
        return "application/yaml";
      if (Category == "time-trace" || Category == "runtime-trace" ||
          FileName.ends_with(".json"))
        return "application/json";
      return "text/plain";
    };
    for (const auto &Category : GeneratedFiles) {
      std::string CategoryDir = UnitDir + "/" + Category.first;
      llvm::sys::fs::create_directories(CategoryDir);

      for (const auto &File : Category.second) {
        std::string FileName = llvm::sys::path::filename(File).str();
        std::string DestFile = CategoryDir + "/" + FileName;
        if (auto Err = FileManager::copyFile(File, DestFile)) {
          if (config.getVerbose()) {
            llvm::errs() << "Failed to copy " << File << " to " << DestFile
                         << "\n";
          }
          continue;
        }

        llvm::json::Object Representation;
        Representation["representation_id"] =
            (llvm::Twine(Unit->getName()) + "::" + Category.first + "::" +
             FileName)
                .str();
        Representation["kind"] = inferRepresentationKind(Category.first);
        Representation["variant"] = inferRepresentationVariant(Category.first);
        Representation["materialization_policy"] = "eager";
        Representation["storage"] = "blob";
        Representation["content_type"] =
            inferContentType(Category.first, FileName);
        Representation["file_name"] = FileName;
        Representation["relative_path"] = Category.first + "/" + FileName;

        std::string MappingSourceFile = File + ".map.json";
        if (llvm::sys::fs::exists(MappingSourceFile)) {
          std::string MappingDir = UnitDir + "/mappings/" + Category.first;
          llvm::sys::fs::create_directories(MappingDir);
          std::string MappingFileName = FileName + ".map.json";
          std::string MappingDestFile = MappingDir + "/" + MappingFileName;
          if (auto MappingErr =
                  FileManager::copyFile(MappingSourceFile, MappingDestFile)) {
            if (config.getVerbose()) {
              llvm::errs() << "Failed to copy mapping sidecar "
                           << MappingSourceFile << " to " << MappingDestFile
                           << ": " << llvm::toString(std::move(MappingErr))
                           << "\n";
            }
          } else {
            llvm::json::Object Mapping;
            Mapping["format"] = "llvm-advisor.line-mapping.v1";
            Mapping["relative_path"] =
                "mappings/" + Category.first + "/" + MappingFileName;
            Representation["mapping"] = std::move(Mapping);
          }
        }

        std::string SourcePath = Unit->findSourceForArtifactPath(File);
        if (!SourcePath.empty()) {
          Representation["source_path"] = normalizeSourcePath(SourcePath);
          Representation["source_ref"] = makeSourceRef(SourcePath);
        } else {
          Representation["source_path"] = nullptr;
          Representation["source_ref"] = nullptr;
        }

        llvm::json::Object Provenance;
        Provenance["producer"] = "llvm-advisor-native";
        Provenance["capture_mode"] = "library-first";
        Provenance["category"] = Category.first;
        Representation["provenance"] = std::move(Provenance);

        ManifestRepresentations.emplace_back(std::move(Representation));
      }
    }

    llvm::json::Array ManifestSources;
    for (const auto &Source : Unit->getInfo().sources) {
      llvm::json::Object SourceObject;
      SourceObject["source_id"] =
          (llvm::Twine(Unit->getName()) + "::src::" + makeSourceRef(Source.path))
              .str();
      SourceObject["source_path"] = normalizeSourcePath(Source.path);
      SourceObject["source_ref"] = makeSourceRef(Source.path);
      SourceObject["language"] = Source.language;
      SourceObject["is_header"] = Source.isHeader;
      ManifestSources.emplace_back(std::move(SourceObject));
    }

    UnitIdentity Identity = UnitIdentityBuilder::build(
        Unit->getInfo(), Unit->getPrimarySource(), initialWorkingDir);

    llvm::json::Array CompileFlags;
    for (const auto &Flag : Unit->getInfo().compileFlags)
      CompileFlags.emplace_back(Flag);

    llvm::json::Array CC1Args;
    for (const auto &Arg : Unit->getInfo().cc1Args)
      CC1Args.emplace_back(Arg);

    llvm::json::Array CommandNormalized;
    for (const auto &Entry : Identity.commandNormalized)
      CommandNormalized.emplace_back(Entry);

    llvm::json::Object Command;
    Command["compiler_path"] = Unit->getInfo().compilerPath;
    Command["compile_flags"] = std::move(CompileFlags);
    Command["cc1_args"] = std::move(CC1Args);
    Command["command_normalized"] = std::move(CommandNormalized);
    Command["command_fingerprint"] = Identity.commandFingerprint;
    Command["target_arch"] = Unit->getInfo().targetArch;
    Command["target_triple"] = Identity.targetTriple;
    Command["output_object"] = Unit->getInfo().outputObject;
    Command["output_executable"] = Unit->getInfo().outputExecutable;

    llvm::json::Object Manifest;
    Manifest["schema_version"] = "2.0";
    Manifest["unit_id"] = Identity.unitId;
    Manifest["unit_name"] = Unit->getName();
    Manifest["run_dir"] = UnitDir;
    Manifest["primary_source_path"] =
        normalizeSourcePath(Unit->getPrimarySource());
    Manifest["primary_source_ref"] = makeSourceRef(Unit->getPrimarySource());
    Manifest["language"] = Identity.language;
    Manifest["target_triple"] = Identity.targetTriple;
    Manifest["command_fingerprint"] = Identity.commandFingerprint;
    Manifest["source_fingerprint"] = Identity.sourceFingerprint;
    Manifest["sources"] = std::move(ManifestSources);
    Manifest["representations"] = std::move(ManifestRepresentations);
    Manifest["command"] = std::move(Command);

    llvm::SmallString<256> ManifestPath(UnitDir);
    llvm::sys::path::append(ManifestPath, "unit-manifest.json");

    std::error_code ManifestEC;
    llvm::raw_fd_ostream ManifestFile(ManifestPath, ManifestEC);
    if (ManifestEC) {
      return llvm::createStringError(
          ManifestEC,
          "Failed to create unit manifest: " + std::string(ManifestPath.str()));
    }
    ManifestFile << llvm::formatv("{0:2}", llvm::json::Value(std::move(Manifest)))
                 << "\n";
  }

  return llvm::Error::success();
}

void CompilationManager::cleanupLeakedFiles() {
  std::error_code EC;
  for (llvm::sys::fs::directory_iterator I(initialWorkingDir, EC), E;
       I != E && !EC; I.increment(EC)) {
    if (I->type() != llvm::sys::fs::file_type::regular_file)
      continue;

    llvm::StringRef Filename = llvm::sys::path::filename(I->path());
    if (Filename.ends_with(".opt.yaml") || Filename.ends_with(".opt.yml") ||
        Filename.ends_with(".profraw") || Filename.ends_with(".profdata")) {
      llvm::sys::fs::remove(I->path());
      if (config.getVerbose())
        llvm::outs() << "Cleaned up leaked file: " << I->path() << "\n";
    }
  }
}

} // namespace advisor
} // namespace llvm
