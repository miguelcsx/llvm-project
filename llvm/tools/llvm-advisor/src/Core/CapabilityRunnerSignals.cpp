//===------------- CapabilityRunnerSignals.cpp - LLVM Advisor ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "CapabilityRunnerInternal.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Remarks/Remark.h"
#include "llvm/Remarks/RemarkFormat.h"
#include "llvm/Remarks/RemarkParser.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Regex.h"

using namespace llvm;

namespace llvm::advisor::detail {

Expected<SourceDescriptor>
UnitManifestIndex::resolveSourceDescriptor(StringRef SourceRef) const {
  if (auto *Sources = Manifest.getArray("sources")) {
    for (const auto &Entry : *Sources) {
      const auto *Object = Entry.getAsObject();
      if (!Object || Object->getString("source_ref").value_or("") != SourceRef)
        continue;

      SourceDescriptor Descriptor;
      Descriptor.SourceRef = SourceRef.str();
      Descriptor.SourcePath =
          std::string(Object->getString("source_path").value_or(""));
      Descriptor.SourceBaseName = sys::path::filename(Descriptor.SourcePath).str();
      Descriptor.Language =
          std::string(Object->getString("language").value_or("text"));
      return Descriptor;
    }
  }

  return createStringError(
      std::make_error_code(std::errc::no_such_file_or_directory),
      "Source ref not found in manifest: " + SourceRef.str());
}

std::optional<int64_t> UnitManifestIndex::jsonLineNumber(const json::Object &Object,
                                                         StringRef Key) {
  if (auto Value = Object.getInteger(Key))
    return *Value;
  if (auto Value = Object.getNumber(Key))
    return static_cast<int64_t>(*Value);
  return std::nullopt;
}

bool UnitManifestIndex::locationMatchesSource(StringRef LocationFile,
                                              const SourceDescriptor &Source) {
  if (LocationFile.empty())
    return false;
  if (LocationFile == Source.SourceRef || LocationFile == Source.SourcePath)
    return true;
  return sys::path::filename(LocationFile) == Source.SourceBaseName;
}

StringMap<int64_t>
UnitManifestIndex::extractRepresentationFunctionLineMap(StringRef Content,
                                                        StringRef Kind) {
  StringMap<int64_t> LineMap;
  SmallVector<StringRef, 256> Lines;
  Content.split(Lines, '\n');
  for (size_t I = 0; I < Lines.size(); ++I) {
    StringRef Line = Lines[I].trim();
    StringRef CandidateName;
    if (Kind == "assembly") {
      StringRef Label = Line.split('#').first.trim();
      if (Label.ends_with(":") && !Label.starts_with(".L") &&
          !Label.starts_with("#"))
        CandidateName = Label.drop_back().trim();
    } else if ((Kind == "ir" || Kind == "optimized-ir") &&
               Line.starts_with("define ") && Line.contains('@') &&
               Line.contains('(')) {
      CandidateName = Line.split('@').second.split('(').first.trim();
    } else if (Kind == "object") {
      SmallVector<StringRef, 8> Parts;
      Line.split(Parts, ' ', -1, false);
      if (Parts.size() >= 4 && Parts[1] == "FUNC")
        CandidateName = Parts.back();
    }

    if (!CandidateName.empty() && !LineMap.contains(CandidateName))
      LineMap[CandidateName] = static_cast<int64_t>(I + 1);
  }
  return LineMap;
}

std::optional<int64_t>
UnitManifestIndex::mappedRepresentationLine(const json::Value *LineMapping,
                                            int64_t SourceLine) {
  if (!LineMapping)
    return std::nullopt;
  const auto *MappingObject = LineMapping->getAsObject();
  if (!MappingObject)
    return std::nullopt;
  const auto *Entries = MappingObject->getArray("entries");
  if (!Entries)
    return std::nullopt;

  for (const auto &Entry : *Entries) {
    const auto *Object = Entry.getAsObject();
    if (!Object)
      continue;
    auto EntrySourceLine = jsonLineNumber(*Object, "source_line");
    auto RepresentationLine = jsonLineNumber(*Object, "representation_line");
    if (EntrySourceLine && RepresentationLine && *EntrySourceLine == SourceLine)
      return *RepresentationLine;
  }
  return std::nullopt;
}

json::Object UnitManifestIndex::buildSourceSignals(const SourceDescriptor &Source,
                                                   StringRef Kind) const {
  json::Array Diagnostics;
  json::Array Remarks;
  Regex DiagnosticRegex("^([^:]+):(\\d+):(\\d+):\\s*([A-Za-z]+):\\s*(.+)$");

  auto DiagnosticFilesOrErr = listRepresentationFilesByCategory("diagnostics");
  if (DiagnosticFilesOrErr) {
    for (const std::string &FilePath : *DiagnosticFilesOrErr) {
      auto BufferOrErr = MemoryBuffer::getFile(FilePath);
      if (!BufferOrErr)
        continue;
      SmallVector<StringRef, 64> Lines;
      BufferOrErr->get()->getBuffer().split(Lines, '\n');
      for (StringRef Line : Lines) {
        SmallVector<StringRef, 6> Matches;
        if (!DiagnosticRegex.match(Line.trim(), &Matches) ||
            !locationMatchesSource(Matches[1], Source))
          continue;
        int64_t LineNumber = 0;
        int64_t ColumnNumber = 0;
        Matches[2].getAsInteger(10, LineNumber);
        Matches[3].getAsInteger(10, ColumnNumber);
        json::Object Diagnostic;
        Diagnostic["line"] = LineNumber;
        Diagnostic["column"] = ColumnNumber;
        Diagnostic["level"] = Matches[4].lower();
        Diagnostic["message"] = Matches[5].str();
        if (!Kind.empty())
          Diagnostic["representation_kind"] = std::move(Kind);
        Diagnostics.push_back(std::move(Diagnostic));
      }
    }
  }

  auto RemarkFilesOrErr = listRepresentationFilesByCategory("remarks");
  if (RemarkFilesOrErr) {
    for (const std::string &FilePath : *RemarkFilesOrErr) {
      auto BufferOrErr = MemoryBuffer::getFile(FilePath);
      if (!BufferOrErr)
        continue;
      auto ParserOrErr = remarks::createRemarkParser(
          remarks::Format::YAML, BufferOrErr->get()->getBuffer());
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
        if (!Remark.Loc ||
            !locationMatchesSource(Remark.Loc->SourceFilePath, Source))
          continue;
        json::Object RemarkSignal;
        RemarkSignal["line"] = static_cast<int64_t>(Remark.Loc->SourceLine);
        RemarkSignal["column"] = static_cast<int64_t>(Remark.Loc->SourceColumn);
        RemarkSignal["pass_name"] = Remark.PassName.str();
        RemarkSignal["function"] = Remark.FunctionName.str();
        RemarkSignal["message"] = Remark.getArgsAsMsg();
        if (!Kind.empty())
          RemarkSignal["representation_kind"] = Kind;
        Remarks.push_back(std::move(RemarkSignal));
      }
    }
  }

  json::Object Result;
  Result["diagnostics"] = std::move(Diagnostics);
  Result["remarks"] = std::move(Remarks);
  return Result;
}

json::Object UnitManifestIndex::buildRepresentationSignals(
    const SourceDescriptor &Source, StringRef Kind,
    const json::Object &RepresentationPayload,
    const json::Object &SourceSignals) const {
  json::Array Diagnostics;
  json::Array Remarks;
  const json::Value *LineMapping = RepresentationPayload.get("line_mapping");
  StringMap<int64_t> FunctionLineMap;
  if (auto Content = RepresentationPayload.getString("content"))
    FunctionLineMap = extractRepresentationFunctionLineMap(*Content, Kind);

  if (const auto *SourceDiagnostics = SourceSignals.getArray("diagnostics")) {
    for (const auto &Entry : *SourceDiagnostics) {
      const auto *Object = Entry.getAsObject();
      if (!Object)
        continue;
      auto SourceLine = jsonLineNumber(*Object, "line");
      if (!SourceLine)
        continue;
      auto RepresentationLine = mappedRepresentationLine(LineMapping, *SourceLine);
      if (!RepresentationLine)
        continue;
      json::Object Diagnostic(*Object);
      Diagnostic["line"] = *RepresentationLine;
      Diagnostic["source_line"] = *SourceLine;
      Diagnostic["correlation_origin"] = "line_mapping";
      Diagnostics.push_back(std::move(Diagnostic));
    }
  }

  if (const auto *SourceRemarks = SourceSignals.getArray("remarks")) {
    for (const auto &Entry : *SourceRemarks) {
      const auto *Object = Entry.getAsObject();
      if (!Object)
        continue;
      auto SourceLine = jsonLineNumber(*Object, "line");
      std::optional<int64_t> RepresentationLine;
      StringRef CorrelationOrigin = "unmapped";
      if (SourceLine)
        RepresentationLine = mappedRepresentationLine(LineMapping, *SourceLine);
      if (RepresentationLine) {
        CorrelationOrigin = "line_mapping";
      } else if (auto Function = Object->getString("function")) {
        auto It = FunctionLineMap.find(*Function);
        if (It != FunctionLineMap.end()) {
          RepresentationLine = It->second;
          CorrelationOrigin = "function_fallback";
        }
      }
      if (!RepresentationLine)
        continue;

      json::Object RemarkSignal(*Object);
      RemarkSignal["line"] = *RepresentationLine;
      if (SourceLine)
        RemarkSignal["source_line"] = *SourceLine;
      RemarkSignal["correlation_origin"] = CorrelationOrigin.str();
      Remarks.push_back(std::move(RemarkSignal));
    }
  }

  json::Object Result;
  Result["diagnostics"] = std::move(Diagnostics);
  Result["remarks"] = std::move(Remarks);
  return Result;
}

Expected<json::Object> UnitManifestIndex::correlateSignals(StringRef SourceRef,
                                                           StringRef Kind) const {
  auto SourceOrErr = resolveSourceDescriptor(SourceRef);
  if (!SourceOrErr)
    return SourceOrErr.takeError();

  SourceDescriptor Source = *SourceOrErr;
  json::Object SourceSignals =
      buildSourceSignals(Source, Kind == "source" ? StringRef("") : Kind);
  json::Object RepresentationSignals;
  RepresentationSignals["diagnostics"] = json::Array();
  RepresentationSignals["remarks"] = json::Array();

  if (Kind != "source") {
    auto RepresentationOrErr = readRepresentation(SourceRef, Kind);
    if (!RepresentationOrErr)
      return RepresentationOrErr.takeError();
    RepresentationSignals =
        buildRepresentationSignals(Source, Kind, *RepresentationOrErr, SourceSignals);
  }

  json::Object Result;
  Result["source_signals"] = std::move(SourceSignals);
  Result["representation_signals"] = std::move(RepresentationSignals);
  return Result;
}

} // namespace llvm::advisor::detail
