//===------------- CapabilityRunnerManifest.cpp - LLVM Advisor -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "CapabilityRunnerInternal.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"

using namespace llvm;

namespace llvm::advisor::detail {

UnitManifestIndex::UnitManifestIndex(StringRef DataDir) : DataDir(DataDir.str()) {}

Error UnitManifestIndex::load(StringRef UnitName) {
  SmallString<256> UnitRoot(DataDir);
  sys::path::append(UnitRoot, UnitName);
  if (!sys::fs::exists(UnitRoot))
    return createStringError(std::make_error_code(std::errc::no_such_file_or_directory),
                             "Compilation unit directory not found: " +
                                 std::string(UnitRoot.str()));

  auto LatestRunOrErr = findLatestRunDir(UnitRoot);
  if (!LatestRunOrErr)
    return LatestRunOrErr.takeError();
  LatestRunDir = *LatestRunOrErr;

  SmallString<256> ManifestPath(LatestRunDir);
  sys::path::append(ManifestPath, "unit-manifest.json");
  auto BufferOrErr = MemoryBuffer::getFile(ManifestPath);
  if (!BufferOrErr)
    return createStringError(std::make_error_code(std::errc::no_such_file_or_directory),
                             "Unit manifest not found: " +
                                 std::string(ManifestPath.str()));

  auto JsonOrErr = json::parse(BufferOrErr.get()->getBuffer());
  if (!JsonOrErr)
    return JsonOrErr.takeError();
  auto *Object = JsonOrErr->getAsObject();
  if (!Object)
    return createStringError(std::make_error_code(std::errc::invalid_argument),
                             "Unit manifest is not a JSON object");

  Manifest = std::move(*Object);
  SmallString<256> RealPath;
  if (std::error_code EC = sys::fs::real_path(LatestRunDir, RealPath))
    LatestRunRealDir = LatestRunDir;
  else
    LatestRunRealDir = RealPath.str().str();
  return Error::success();
}

StringRef UnitManifestIndex::getLatestRunDir() const { return LatestRunDir; }

json::Array UnitManifestIndex::listSources() const {
  json::Array Result;
  if (auto *Sources = Manifest.getArray("sources"))
    for (const auto &Entry : *Sources)
      Result.push_back(Entry);
  return Result;
}

json::Array UnitManifestIndex::listRepresentationDescriptors() const {
  json::Array Result;
  if (auto *Representations = Manifest.getArray("representations"))
    for (const auto &Entry : *Representations)
      Result.push_back(Entry);
  return Result;
}

Expected<json::Object> UnitManifestIndex::readSource(StringRef SourceRef) const {
  if (auto *Sources = Manifest.getArray("sources")) {
    for (const auto &Entry : *Sources) {
      const auto *Object = Entry.getAsObject();
      if (!Object || Object->getString("source_ref").value_or("") != SourceRef)
        continue;

      std::string SourcePath =
          std::string(Object->getString("source_path").value_or(""));
      auto BufferOrErr = MemoryBuffer::getFile(SourcePath);
      if (!BufferOrErr)
        return createStringError(
            std::make_error_code(std::errc::no_such_file_or_directory),
            "Source file not found: " + SourcePath);

      json::Object Result;
      Result["representation_kind"] = "source";
      Result["file_path"] = SourceRef.str();
      Result["source_path"] = SourcePath;
      Result["language"] =
          std::string(Object->getString("language").value_or("text"));
      Result["content"] = BufferOrErr.get()->getBuffer().str();
      return Result;
    }
  }

  return createStringError(std::make_error_code(std::errc::no_such_file_or_directory),
                           "Source ref not found in manifest: " + SourceRef.str());
}

Expected<json::Object> UnitManifestIndex::readRepresentation(StringRef SourceRef,
                                                             StringRef Kind) const {
  if (Kind == "source")
    return readSource(SourceRef);

  StringRef ExpectedCategory = representationKindToCategory(Kind);
  if (ExpectedCategory.empty())
    return createStringError(std::make_error_code(std::errc::not_supported),
                             "Unsupported representation kind: " + Kind.str());

  if (Kind == "diff") {
    auto BaseOrErr = readRepresentation(SourceRef, "ir");
    if (!BaseOrErr)
      return BaseOrErr.takeError();
    auto OptimizedOrErr = readRepresentation(SourceRef, "optimized-ir");
    if (!OptimizedOrErr)
      return OptimizedOrErr.takeError();

    json::Object Result;
    Result["type"] = "diff";
    Result["representation_kind"] = "diff";
    Result["file_path"] = SourceRef.str();
    Result["language"] = "llvm";
    Result["original"] = std::string(BaseOrErr->getString("content").value_or(""));
    Result["modified"] =
        std::string(OptimizedOrErr->getString("content").value_or(""));
    return Result;
  }

  if (auto *Representations = Manifest.getArray("representations")) {
    for (const auto &Entry : *Representations) {
      const auto *Object = Entry.getAsObject();
      if (!Object || Object->getString("source_ref").value_or("") != SourceRef)
        continue;
      const auto *Provenance = Object->getObject("provenance");
      if (!Provenance ||
          Provenance->getString("category").value_or("") != ExpectedCategory)
        continue;

      std::string RelativePath =
          std::string(Object->getString("relative_path").value_or(""));
      auto AbsolutePathOrErr = resolveRepresentationPath(RelativePath);
      if (!AbsolutePathOrErr)
        return AbsolutePathOrErr.takeError();
      auto BufferOrErr = MemoryBuffer::getFile(*AbsolutePathOrErr);
      if (!BufferOrErr)
        return createStringError(
            std::make_error_code(std::errc::no_such_file_or_directory),
            "Representation blob not found: " + *AbsolutePathOrErr);

      json::Object Result;
      Result["representation_kind"] = Kind.str();
      Result["file_path"] = SourceRef.str();
      Result["relative_path"] = RelativePath;
      Result["content_type"] =
          std::string(Object->getString("content_type").value_or("text/plain"));
      Result["language"] = representationKindToSyntax(Kind).str();
      Result["content"] = BufferOrErr.get()->getBuffer().str();
      if (const auto *Mapping = Object->getObject("mapping")) {
        std::string MappingRelativePath =
            std::string(Mapping->getString("relative_path").value_or(""));
        if (!MappingRelativePath.empty()) {
          auto MappingPathOrErr = resolveRepresentationPath(MappingRelativePath);
          if (MappingPathOrErr) {
            if (auto MappingBufferOrErr = MemoryBuffer::getFile(*MappingPathOrErr)) {
              if (auto MappingJsonOrErr =
                      json::parse(MappingBufferOrErr.get()->getBuffer())) {
                Result["line_mapping"] = std::move(*MappingJsonOrErr);
              }
            }
          }
        }
        if (auto MappingFormat = Mapping->getString("format"))
          Result["mapping_format"] = MappingFormat->str();
      }
      return Result;
    }
  }

  return createStringError(std::make_error_code(std::errc::no_such_file_or_directory),
                           "Representation not found for source '" +
                               SourceRef.str() + "' and kind '" + Kind.str() + "'");
}

Expected<std::vector<std::string>>
UnitManifestIndex::listRepresentationFilesByCategory(StringRef Category) const {
  std::vector<std::string> Files;
  if (auto *Representations = Manifest.getArray("representations")) {
    for (const auto &Entry : *Representations) {
      const auto *Object = Entry.getAsObject();
      if (!Object)
        continue;
      const auto *Provenance = Object->getObject("provenance");
      if (!Provenance || Provenance->getString("category").value_or("") != Category)
        continue;
      std::string RelativePath =
          std::string(Object->getString("relative_path").value_or(""));
      if (RelativePath.empty())
        continue;
      auto AbsolutePathOrErr = resolveRepresentationPath(RelativePath);
      if (!AbsolutePathOrErr)
        return AbsolutePathOrErr.takeError();
      Files.push_back(*AbsolutePathOrErr);
    }
  }
  return Files;
}

json::Object UnitManifestIndex::countRepresentationKinds() const {
  StringMap<int64_t> Counts;
  if (auto *Sources = Manifest.getArray("sources"))
    Counts["source"] = static_cast<int64_t>(Sources->size());
  if (auto *Representations = Manifest.getArray("representations")) {
    for (const auto &Entry : *Representations) {
      const auto *Object = Entry.getAsObject();
      if (!Object)
        continue;
      const auto *Provenance = Object->getObject("provenance");
      StringRef Category =
          Provenance ? Provenance->getString("category").value_or("") : "";
      StringRef Kind = categoryToRepresentationKind(Category);
      if (!Kind.empty())
        Counts[Kind] += 1;
    }
  }

  json::Object Result;
  for (const auto &Entry : Counts)
    Result[Entry.getKey().str()] = Entry.getValue();
  return Result;
}

json::Object UnitManifestIndex::countRepresentationCategories() const {
  StringMap<int64_t> Counts;
  if (auto *Sources = Manifest.getArray("sources"))
    Counts["sources"] = static_cast<int64_t>(Sources->size());
  if (auto *Representations = Manifest.getArray("representations")) {
    for (const auto &Entry : *Representations) {
      const auto *Object = Entry.getAsObject();
      if (!Object)
        continue;
      const auto *Provenance = Object->getObject("provenance");
      if (!Provenance)
        continue;
      StringRef Category = Provenance->getString("category").value_or("");
      if (!Category.empty())
        Counts[Category] += 1;
    }
  }

  json::Object Result;
  for (const auto &Entry : Counts)
    Result[Entry.getKey().str()] = Entry.getValue();
  return Result;
}

Expected<std::string>
UnitManifestIndex::resolveRepresentationPath(StringRef RelativePath) const {
  if (RelativePath.empty() || sys::path::is_absolute(RelativePath))
    return createStringError(std::make_error_code(std::errc::invalid_argument),
                             "Manifest relative_path must be relative");

  SmallString<256> Candidate(LatestRunDir);
  sys::path::append(Candidate, RelativePath);
  SmallString<256> CanonicalPath;
  if (std::error_code EC = sys::fs::real_path(Candidate, CanonicalPath))
    return createStringError(EC, "Failed to resolve representation path: " +
                                     Candidate.str());

  StringRef Canonical = CanonicalPath;
  StringRef Root = LatestRunRealDir.empty() ? StringRef(LatestRunDir)
                                            : StringRef(LatestRunRealDir);
  if (!Canonical.starts_with(Root))
    return createStringError(std::make_error_code(std::errc::permission_denied),
                             "Manifest path escapes unit run directory: " +
                                 Canonical.str());
  return std::string(Canonical);
}

Expected<std::string> UnitManifestIndex::findLatestRunDir(StringRef UnitRoot) {
  std::string LatestRun;
  std::error_code EC;
  for (sys::fs::directory_iterator It(UnitRoot, EC), End; It != End && !EC;
       It.increment(EC)) {
    if (!sys::fs::is_directory(It->path()))
      continue;
    StringRef Name = sys::path::filename(It->path());
    if (!Name.starts_with(sys::path::filename(UnitRoot)))
      continue;
    std::string Candidate = std::string(It->path());
    if (LatestRun.empty() || Candidate > LatestRun)
      LatestRun = Candidate;
  }
  if (EC)
    return createStringError(EC, "Failed to scan unit run directories");
  if (LatestRun.empty())
    return createStringError(std::make_error_code(std::errc::no_such_file_or_directory),
                             "No unit runs found under " + UnitRoot.str());
  return LatestRun;
}

StringRef UnitManifestIndex::representationKindToCategory(StringRef Kind) {
  if (Kind == "assembly")
    return "assembly";
  if (Kind == "ir")
    return "ir";
  if (Kind == "optimized-ir")
    return "optimized-ir";
  if (Kind == "object")
    return "objdump";
  if (Kind == "ast-json")
    return "ast-json";
  if (Kind == "preprocessed")
    return "preprocessed";
  if (Kind == "macro-expansion")
    return "macro-expansion";
  return "";
}

StringRef UnitManifestIndex::representationKindToSyntax(StringRef Kind) {
  if (Kind == "assembly")
    return "assembly";
  if (Kind == "ir" || Kind == "optimized-ir")
    return "llvm-ir";
  if (Kind == "ast-json")
    return "json";
  if (Kind == "preprocessed" || Kind == "macro-expansion" || Kind == "source")
    return "cpp";
  return "text";
}

StringRef UnitManifestIndex::categoryToRepresentationKind(StringRef Category) {
  if (Category == "sources")
    return "source";
  if (Category == "assembly")
    return "assembly";
  if (Category == "ir")
    return "ir";
  if (Category == "optimized-ir")
    return "optimized-ir";
  if (Category == "objdump")
    return "object";
  if (Category == "ast-json")
    return "ast-json";
  if (Category == "preprocessed")
    return "preprocessed";
  if (Category == "macro-expansion")
    return "macro-expansion";
  if (Category == "remarks")
    return "remarks";
  if (Category == "diagnostics")
    return "diagnostics";
  if (Category == "time-trace")
    return "time-trace";
  if (Category == "runtime-trace")
    return "runtime-trace";
  if (Category == "debug")
    return "debug";
  return "";
}

} // namespace llvm::advisor::detail
