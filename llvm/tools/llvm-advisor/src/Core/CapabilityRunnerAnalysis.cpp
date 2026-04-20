//===------------- CapabilityRunnerAnalysis.cpp - LLVM Advisor -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "CapabilityRunnerInternal.h"
#include "llvm/Remarks/Remark.h"
#include "llvm/Remarks/RemarkFormat.h"
#include "llvm/Remarks/RemarkParser.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Regex.h"

using namespace llvm;

namespace llvm::advisor::detail {

void collectASTStats(const json::Value &Value, StringMap<int64_t> &DeclKinds,
                     int64_t &DeclCount, int64_t &FunctionCount,
                     int64_t &RecordCount, int64_t &NamespaceCount,
                     int64_t &TemplateCount, int64_t &VariableCount) {
  if (const auto *Object = Value.getAsObject()) {
    if (auto Kind = Object->getString("kind")) {
      if (Kind->ends_with("Decl")) {
        ++DeclCount;
        DeclKinds[*Kind] += 1;
      }
      if (*Kind == "FunctionDecl" || *Kind == "CXXMethodDecl" ||
          *Kind == "CXXConstructorDecl" || *Kind == "CXXDestructorDecl" ||
          *Kind == "ConversionFunctionDecl")
        ++FunctionCount;
      if (*Kind == "CXXRecordDecl" || *Kind == "RecordDecl")
        ++RecordCount;
      if (*Kind == "NamespaceDecl")
        ++NamespaceCount;
      if (Kind->contains("Template"))
        ++TemplateCount;
      if (Kind->ends_with("VarDecl"))
        ++VariableCount;
    }

    for (const auto &Entry : *Object)
      collectASTStats(Entry.second, DeclKinds, DeclCount, FunctionCount,
                      RecordCount, NamespaceCount, TemplateCount, VariableCount);
    return;
  }

  if (const auto *Array = Value.getAsArray()) {
    for (const auto &Entry : *Array)
      collectASTStats(Entry, DeclKinds, DeclCount, FunctionCount, RecordCount,
                      NamespaceCount, TemplateCount, VariableCount);
  }
}

Expected<json::Object> buildASTSummary(const UnitManifestIndex &Index) {
  int64_t ASTFileCount = 0;
  int64_t DeclCount = 0;
  int64_t FunctionCount = 0;
  int64_t RecordCount = 0;
  int64_t NamespaceCount = 0;
  int64_t TemplateCount = 0;
  int64_t VariableCount = 0;
  StringMap<int64_t> DeclKinds;

  auto FilesOrErr = Index.listRepresentationFilesByCategory("ast-json");
  if (!FilesOrErr)
    return FilesOrErr.takeError();
  for (const std::string &FilePath : *FilesOrErr) {
    auto BufferOrErr = MemoryBuffer::getFile(FilePath);
    if (!BufferOrErr)
      continue;
    auto JsonOrErr = json::parse(BufferOrErr->get()->getBuffer());
    if (!JsonOrErr)
      continue;
    ++ASTFileCount;
    collectASTStats(*JsonOrErr, DeclKinds, DeclCount, FunctionCount, RecordCount,
                    NamespaceCount, TemplateCount, VariableCount);
  }

  json::Object Result;
  Result["ast_file_count"] = ASTFileCount;
  Result["decl_count"] = DeclCount;
  Result["function_count"] = FunctionCount;
  Result["record_count"] = RecordCount;
  Result["namespace_count"] = NamespaceCount;
  Result["template_count"] = TemplateCount;
  Result["variable_count"] = VariableCount;
  Result["top_decl_kinds"] = buildTopCounts(DeclKinds);
  return Result;
}

Expected<json::Object> buildPassStats(const UnitManifestIndex &Index) {
  int64_t RemarkCount = 0;
  StringMap<int64_t> PassCounts;
  StringMap<int64_t> FunctionCounts;

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
      ++RemarkCount;
      PassCounts[Remark.PassName] += 1;
      FunctionCounts[Remark.FunctionName] += 1;
    }
  }

  int64_t HottestPassCount = 0;
  for (const auto &Entry : PassCounts)
    HottestPassCount = std::max(HottestPassCount, Entry.getValue());
  int64_t HottestFunctionCount = 0;
  for (const auto &Entry : FunctionCounts)
    HottestFunctionCount = std::max(HottestFunctionCount, Entry.getValue());

  json::Object Result;
  Result["remark_count"] = RemarkCount;
  Result["unique_pass_count"] = static_cast<int64_t>(PassCounts.size());
  Result["unique_function_count"] = static_cast<int64_t>(FunctionCounts.size());
  Result["hottest_pass_count"] = HottestPassCount;
  Result["hottest_function_count"] = HottestFunctionCount;
  Result["pass_counts"] = buildTopCounts(PassCounts);
  Result["function_counts"] = buildTopCounts(FunctionCounts);
  return Result;
}

Expected<json::Object> buildObjectSummary(const UnitManifestIndex &Index) {
  Regex SymbolRegex("^([0-9A-Fa-f]+)\\s+([A-Z]+)(?:\\s+([A-Z]))?\\s+(.+)$");
  Regex SectionRegex("^([.A-Za-z0-9_\\-]+)\\s+(\\d+)$");

  int64_t SymbolCount = 0;
  int64_t FunctionSymbolCount = 0;
  int64_t ExternalSymbolCount = 0;
  int64_t SectionCount = 0;
  int64_t TotalBytes = 0;
  int64_t TextBytes = 0;
  StringMap<int64_t> Sections;

  auto SymbolFilesOrErr = Index.listRepresentationFilesByCategory("symbols");
  if (!SymbolFilesOrErr)
    return SymbolFilesOrErr.takeError();
  for (const std::string &FilePath : *SymbolFilesOrErr) {
    auto BufferOrErr = MemoryBuffer::getFile(FilePath);
    if (!BufferOrErr)
      continue;
    SmallVector<StringRef, 64> Lines;
    BufferOrErr->get()->getBuffer().split(Lines, '\n');
    for (StringRef Line : Lines) {
      SmallVector<StringRef, 5> Matches;
      if (!SymbolRegex.match(Line.trim(), &Matches))
        continue;
      ++SymbolCount;
      if (Matches[2] == "FUNC")
        ++FunctionSymbolCount;
      if (Matches.size() > 3 && Matches[3] == "G")
        ++ExternalSymbolCount;
    }
  }

  auto SizeFilesOrErr = Index.listRepresentationFilesByCategory("binary-size");
  if (!SizeFilesOrErr)
    return SizeFilesOrErr.takeError();
  for (const std::string &FilePath : *SizeFilesOrErr) {
    auto BufferOrErr = MemoryBuffer::getFile(FilePath);
    if (!BufferOrErr)
      continue;
    SmallVector<StringRef, 64> Lines;
    BufferOrErr->get()->getBuffer().split(Lines, '\n');
    for (StringRef Line : Lines) {
      SmallVector<StringRef, 3> Matches;
      if (!SectionRegex.match(Line.trim(), &Matches))
        continue;
      StringRef SectionName = Matches[1];
      int64_t ByteCount = 0;
      Matches[2].getAsInteger(10, ByteCount);
      if (SectionName == "TOTAL") {
        TotalBytes += ByteCount;
        continue;
      }
      ++SectionCount;
      Sections[SectionName] += ByteCount;
      if (SectionName == ".text")
        TextBytes += ByteCount;
    }
  }

  json::Object Result;
  Result["symbol_count"] = SymbolCount;
  Result["function_symbol_count"] = FunctionSymbolCount;
  Result["external_symbol_count"] = ExternalSymbolCount;
  Result["section_count"] = SectionCount;
  Result["total_bytes"] = TotalBytes;
  Result["text_bytes"] = TextBytes;
  Result["sections"] = buildTopCounts(Sections);
  return Result;
}

Expected<json::Object> buildDebugSummary(const UnitManifestIndex &Index) {
  Regex DebugSectionRegex("^\\.debug_[A-Za-z0-9_]+ contents:$");
  Regex DebugTagRegex("(DW_TAG_[A-Za-z0-9_]+)");

  int64_t DebugSectionCount = 0;
  int64_t TagCount = 0;
  int64_t CompileUnitCount = 0;
  int64_t SubprogramCount = 0;
  int64_t VariableCount = 0;
  StringMap<int64_t> TagCounts;

  auto FilesOrErr = Index.listRepresentationFilesByCategory("debug");
  if (!FilesOrErr)
    return FilesOrErr.takeError();
  for (const std::string &FilePath : *FilesOrErr) {
    auto BufferOrErr = MemoryBuffer::getFile(FilePath);
    if (!BufferOrErr)
      continue;
    SmallVector<StringRef, 256> Lines;
    BufferOrErr->get()->getBuffer().split(Lines, '\n');
    for (StringRef Line : Lines) {
      Line = Line.trim();
      if (DebugSectionRegex.match(Line))
        ++DebugSectionCount;

      SmallVector<StringRef, 4> Matches;
      if (!DebugTagRegex.match(Line, &Matches))
        continue;
      StringRef Tag = Matches[1];
      ++TagCount;
      TagCounts[Tag] += 1;
      if (Tag == "DW_TAG_compile_unit")
        ++CompileUnitCount;
      else if (Tag == "DW_TAG_subprogram")
        ++SubprogramCount;
      else if (Tag == "DW_TAG_variable")
        ++VariableCount;
    }
  }

  json::Object Result;
  Result["debug_section_count"] = DebugSectionCount;
  Result["tag_count"] = TagCount;
  Result["compile_unit_count"] = CompileUnitCount;
  Result["subprogram_count"] = SubprogramCount;
  Result["variable_count"] = VariableCount;
  Result["top_tags"] = buildTopCounts(TagCounts);
  return Result;
}

} // namespace llvm::advisor::detail
