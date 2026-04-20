//===------------ CapabilityRunnerSummaries.cpp - LLVM Advisor -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "CapabilityRunnerInternal.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Remarks/Remark.h"
#include "llvm/Remarks/RemarkFormat.h"
#include "llvm/Remarks/RemarkParser.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Regex.h"
#include "llvm/Support/SourceMgr.h"

using namespace llvm;

namespace llvm::advisor::detail {

static Expected<json::Object> buildBuildCommandMeta(const RunnerRequest &Request,
                                                    const UnitManifestIndex &Index) {
  json::Object Result;
  Result["unit_name"] = Request.UnitName;
  Result["unit_path"] = std::string(Index.getLatestRunDir());
  Result["representation_counts"] = Index.countRepresentationKinds();
  Result["representation_categories"] = Index.countRepresentationCategories();
  return Result;
}

static Expected<json::Object> buildDiagnosticsSummary(const UnitManifestIndex &Index) {
  Regex DiagnosticRegex("^([^:]+):(\\d+):(\\d+):\\s*([A-Za-z]+):\\s*(.+)$");
  int64_t WarningCount = 0;
  int64_t ErrorCount = 0;
  int64_t NoteCount = 0;
  int64_t InfoCount = 0;
  StringMap<int64_t> FileCounts;

  auto FilesOrErr = Index.listRepresentationFilesByCategory("diagnostics");
  if (!FilesOrErr)
    return FilesOrErr.takeError();
  for (const std::string &FilePath : *FilesOrErr) {
    auto BufferOrErr = MemoryBuffer::getFile(FilePath);
    if (!BufferOrErr)
      continue;
    SmallVector<StringRef, 8> Lines;
    BufferOrErr->get()->getBuffer().split(Lines, '\n');
    for (StringRef Line : Lines) {
      SmallVector<StringRef, 6> Matches;
      if (!DiagnosticRegex.match(Line.trim(), &Matches))
        continue;
      StringRef Level = Matches[4].lower();
      if (Level == "warning")
        ++WarningCount;
      else if (Level == "error")
        ++ErrorCount;
      else if (Level == "note")
        ++NoteCount;
      else if (Level == "info")
        ++InfoCount;
      FileCounts[Matches[1]] += 1;
    }
  }

  json::Object Result;
  Result["warning_count"] = WarningCount;
  Result["error_count"] = ErrorCount;
  Result["note_count"] = NoteCount;
  Result["info_count"] = InfoCount;
  Result["total"] = WarningCount + ErrorCount + NoteCount + InfoCount;
  Result["top_files"] = buildTopCounts(FileCounts);
  return Result;
}

static Expected<json::Object> buildRemarksSummary(const UnitManifestIndex &Index) {
  int64_t Total = 0;
  StringMap<int64_t> PassCounts;
  StringMap<int64_t> FunctionCounts;
  StringMap<int64_t> FileCounts;

  auto FilesOrErr = Index.listRepresentationFilesByCategory("remarks");
  if (!FilesOrErr)
    return FilesOrErr.takeError();
  for (const std::string &FilePath : *FilesOrErr) {
    auto BufferOrErr = MemoryBuffer::getFile(FilePath);
    if (!BufferOrErr)
      continue;
    auto ParserOrErr =
        remarks::createRemarkParser(remarks::Format::YAML,
                                    BufferOrErr->get()->getBuffer());
    if (!ParserOrErr)
      continue;

    while (true) {
      auto RemarkOrErr = (*ParserOrErr)->next();
      if (!RemarkOrErr) {
        Error Err = RemarkOrErr.takeError();
        bool IsEndOfFile = false;
        handleAllErrors(std::move(Err),
                        [&](const remarks::EndOfFileError &) {
                          IsEndOfFile = true;
                        },
                        [&](const ErrorInfoBase &) {});
        if (IsEndOfFile)
          break;
        break;
      }

      const remarks::Remark &Remark = **RemarkOrErr;
      ++Total;
      PassCounts[Remark.PassName] += 1;
      FunctionCounts[Remark.FunctionName] += 1;
      if (Remark.Loc)
        FileCounts[Remark.Loc->SourceFilePath] += 1;
    }
  }

  json::Object Result;
  Result["total"] = Total;
  Result["top_passes"] = buildTopCounts(PassCounts);
  Result["top_functions"] = buildTopCounts(FunctionCounts);
  Result["top_files"] = buildTopCounts(FileCounts);
  return Result;
}

static Expected<json::Object> buildIRSummary(const UnitManifestIndex &Index) {
  int64_t IRFileCount = 0;
  int64_t FunctionCount = 0;
  int64_t GlobalCount = 0;
  int64_t TypeCount = 0;
  int64_t InstructionCount = 0;

  auto FilesOrErr = Index.listRepresentationFilesByCategory("ir");
  if (!FilesOrErr)
    return FilesOrErr.takeError();

  LLVMContext Context;
  SMDiagnostic Diagnostic;
  for (const std::string &FilePath : *FilesOrErr) {
    auto Module = parseIRFile(FilePath, Diagnostic, Context);
    if (!Module)
      continue;
    ++IRFileCount;
    for (const Function &Fn : *Module) {
      if (Fn.isDeclaration())
        continue;
      ++FunctionCount;
      for (const BasicBlock &BB : Fn)
        InstructionCount += static_cast<int64_t>(BB.size());
    }
    for (const GlobalVariable &GV : Module->globals())
      if (!GV.isDeclaration())
        ++GlobalCount;
    TypeCount += static_cast<int64_t>(Module->getIdentifiedStructTypes().size());
  }

  json::Object Result;
  Result["ir_file_count"] = IRFileCount;
  Result["function_count"] = FunctionCount;
  Result["global_count"] = GlobalCount;
  Result["type_count"] = TypeCount;
  Result["instruction_count"] = InstructionCount;
  return Result;
}

json::Object buildTopCounts(const StringMap<int64_t> &Counts, size_t Limit) {
  std::vector<std::pair<std::string, int64_t>> Entries;
  Entries.reserve(Counts.size());
  for (const auto &Entry : Counts)
    Entries.emplace_back(Entry.getKey().str(), Entry.getValue());

  llvm::sort(Entries, [](const auto &Left, const auto &Right) {
    if (Left.second != Right.second)
      return Left.second > Right.second;
    return Left.first < Right.first;
  });
  if (Entries.size() > Limit)
    Entries.resize(Limit);

  json::Object Result;
  for (const auto &Entry : Entries)
    Result[Entry.first] = Entry.second;
  return Result;
}

Expected<json::Object> executeCapability(const RunnerRequest &Request) {
  UnitManifestIndex Index(Request.DataDir);
  if (auto Err = Index.load(Request.UnitName))
    return std::move(Err);

  if (Request.CapabilityId == "build.command.meta")
    return buildBuildCommandMeta(Request, Index);
  if (Request.CapabilityId == "clang.diag.summary")
    return buildDiagnosticsSummary(Index);
  if (Request.CapabilityId == "llvm.remarks.summary")
    return buildRemarksSummary(Index);
  if (Request.CapabilityId == "llvm.ir.summary")
    return buildIRSummary(Index);
  if (Request.CapabilityId == "clang.ast.summary")
    return buildASTSummary(Index);
  if (Request.CapabilityId == "llvm.pass.stats")
    return buildPassStats(Index);
  if (Request.CapabilityId == "llvm.obj.summary")
    return buildObjectSummary(Index);
  if (Request.CapabilityId == "llvm.debug.summary")
    return buildDebugSummary(Index);
  return createStringError(std::make_error_code(std::errc::not_supported),
                           "Unsupported capability: " + Request.CapabilityId);
}

} // namespace llvm::advisor::detail
