//===------------- CapabilityRunnerRequests.cpp - LLVM Advisor -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "CapabilityRunnerInternal.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace llvm::advisor::detail {

const RepresentationCapabilityDescriptor RepresentationCapabilities[] = {
    {"source", "Source", "cpp", "source", 0, "eager", "snapshot", "low", true,
     true, true, "", "default"},
    {"assembly", "Assembly", "assembly", "explorer", 10, "lazy", "snapshot",
     "moderate", true, true, true, "", "default"},
    {"ir", "LLVM IR", "llvm-ir", "explorer", 20, "lazy", "snapshot",
     "moderate", true, true, true, "", "default"},
    {"optimized-ir", "Optimized IR", "llvm-ir", "explorer", 30, "lazy",
     "snapshot", "high", true, true, true, "", "default"},
    {"diff", "IR Diff", "diff", "explorer", 40, "derived", "request",
     "moderate", true, false, false, "ir", "default"},
    {"ast-json", "AST JSON", "json", "explorer", 50, "lazy", "snapshot",
     "high", true, true, false, "", "default"},
    {"object", "Object Code", "text", "explorer", 60, "lazy", "snapshot",
     "high", true, false, false, "", "default"},
    {"preprocessed", "Preprocessed", "cpp", "explorer", 70, "lazy", "snapshot",
     "moderate", true, true, false, "", "default"},
    {"macro-expansion", "Macro Expansion", "cpp", "explorer", 80, "lazy",
     "snapshot", "moderate", true, true, false, "", "default"},
};

Expected<RunnerRequest> parseRequest(StringRef RequestJson) {
  auto JsonOrErr = json::parse(RequestJson);
  if (!JsonOrErr)
    return JsonOrErr.takeError();

  const auto *Object = JsonOrErr->getAsObject();
  if (!Object)
    return createStringError(std::make_error_code(std::errc::invalid_argument),
                             "Capability request must be a JSON object");

  RunnerRequest Request;
  Request.DataDir = std::string(Object->getString("data_dir").value_or(""));
  Request.CapabilityId =
      std::string(Object->getString("capability_id").value_or(""));
  Request.UnitName = std::string(Object->getString("unit_name").value_or(""));
  if (Request.DataDir.empty() || Request.CapabilityId.empty() ||
      Request.UnitName.empty())
    return createStringError(std::make_error_code(std::errc::invalid_argument),
                             "data_dir, capability_id, and unit_name are required");
  return Request;
}

Expected<RepresentationRequest> parseRepresentationRequest(StringRef RequestJson) {
  auto JsonOrErr = json::parse(RequestJson);
  if (!JsonOrErr)
    return JsonOrErr.takeError();

  const auto *Object = JsonOrErr->getAsObject();
  if (!Object)
    return createStringError(std::make_error_code(std::errc::invalid_argument),
                             "Representation request must be a JSON object");

  RepresentationRequest Request;
  Request.DataDir = std::string(Object->getString("data_dir").value_or(""));
  Request.UnitName = std::string(Object->getString("unit_name").value_or(""));
  Request.SourceRef = std::string(Object->getString("source_ref").value_or(""));
  Request.Kind = std::string(Object->getString("kind").value_or(""));
  if (Request.DataDir.empty() || Request.UnitName.empty() ||
      Request.SourceRef.empty() || Request.Kind.empty())
    return createStringError(std::make_error_code(std::errc::invalid_argument),
                             "data_dir, unit_name, source_ref, and kind are required");
  return Request;
}

Expected<CompareRequest> parseCompareRequest(StringRef RequestJson) {
  auto JsonOrErr = json::parse(RequestJson);
  if (!JsonOrErr)
    return JsonOrErr.takeError();

  const auto *Object = JsonOrErr->getAsObject();
  if (!Object)
    return createStringError(std::make_error_code(std::errc::invalid_argument),
                             "Compare request must be a JSON object");

  CompareRequest Request;
  Request.BaseDataDir =
      std::string(Object->getString("base_data_dir").value_or(""));
  Request.CandidateDataDir =
      std::string(Object->getString("candidate_data_dir").value_or(""));
  Request.CapabilityId =
      std::string(Object->getString("capability_id").value_or(""));
  Request.UnitName = std::string(Object->getString("unit_name").value_or(""));
  if (Request.BaseDataDir.empty() || Request.CandidateDataDir.empty() ||
      Request.CapabilityId.empty())
    return createStringError(
        std::make_error_code(std::errc::invalid_argument),
        "base_data_dir, candidate_data_dir, and capability_id are required");
  return Request;
}

Expected<std::string> serializeJson(json::Value Value) {
  std::string Serialized;
  raw_string_ostream OS(Serialized);
  OS << Value;
  return Serialized;
}

json::Object buildRepresentationCapability(
    const RepresentationCapabilityDescriptor &Descriptor) {
  json::Object Object;
  Object["kind"] = Descriptor.Kind;
  Object["label"] = Descriptor.Label;
  Object["syntax"] = Descriptor.Syntax;
  Object["category"] = Descriptor.Category;
  Object["display_rank"] = Descriptor.DisplayRank;
  Object["materialization_mode"] = Descriptor.MaterializationMode;
  Object["cache_policy"] = Descriptor.CachePolicy;
  Object["cost_class"] = Descriptor.CostClass;
  Object["supports_time_compare"] = Descriptor.SupportsTimeCompare;
  Object["supports_range_queries"] = Descriptor.SupportsRangeQueries;
  Object["supports_line_mapping"] = Descriptor.SupportsLineMapping;
  Object["diff_base_kind"] =
      Descriptor.DiffBaseKind.empty() ? json::Value(nullptr)
                                      : json::Value(Descriptor.DiffBaseKind.str());
  Object["default_variant"] = Descriptor.DefaultVariant;
  return Object;
}

json::Array buildRepresentationCapabilities() {
  json::Array Result;
  for (const auto &Descriptor : RepresentationCapabilities)
    Result.push_back(buildRepresentationCapability(Descriptor));
  return Result;
}

Expected<json::Array> listUnits(StringRef DataDir) {
  json::Array Units;
  std::error_code EC;
  for (sys::fs::directory_iterator It(DataDir, EC), End; It != End && !EC;
       It.increment(EC)) {
    if (!sys::fs::is_directory(It->path()))
      continue;
    StringRef UnitName = sys::path::filename(It->path());
    if (UnitName.starts_with(".llvm-advisor-store"))
      continue;
    json::Object Unit;
    Unit["unit_name"] = UnitName.str();
    Unit["unit_path"] = std::string(It->path());
    Units.push_back(std::move(Unit));
  }
  if (EC)
    return createStringError(EC, "Failed to enumerate compilation units");
  return Units;
}

Expected<std::vector<std::string>> listUnitNames(StringRef DataDir) {
  auto UnitsOrErr = listUnits(DataDir);
  if (!UnitsOrErr)
    return UnitsOrErr.takeError();

  std::vector<std::string> Units;
  for (const auto &Entry : *UnitsOrErr) {
    const auto *Object = Entry.getAsObject();
    if (!Object)
      continue;
    std::string UnitName = std::string(Object->getString("unit_name").value_or(""));
    if (!UnitName.empty())
      Units.push_back(UnitName);
  }
  llvm::sort(Units);
  return Units;
}

} // namespace llvm::advisor::detail
