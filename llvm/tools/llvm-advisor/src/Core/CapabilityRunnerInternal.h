//===------------- CapabilityRunnerInternal.h - LLVM Advisor -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_ADVISOR_SRC_CORE_CAPABILITYRUNNERINTERNAL_H
#define LLVM_TOOLS_LLVM_ADVISOR_SRC_CORE_CAPABILITYRUNNERINTERNAL_H

#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace llvm::advisor::detail {

struct RunnerRequest {
  std::string DataDir;
  std::string CapabilityId;
  std::string UnitName;
};

struct RepresentationRequest {
  std::string DataDir;
  std::string UnitName;
  std::string SourceRef;
  std::string Kind;
};

struct CompareRequest {
  std::string BaseDataDir;
  std::string CandidateDataDir;
  std::string CapabilityId;
  std::string UnitName;
};

struct SourceDescriptor {
  std::string SourceRef;
  std::string SourcePath;
  std::string SourceBaseName;
  std::string Language;
};

struct RepresentationCapabilityDescriptor {
  StringRef Kind;
  StringRef Label;
  StringRef Syntax;
  StringRef Category;
  int DisplayRank;
  StringRef MaterializationMode;
  StringRef CachePolicy;
  StringRef CostClass;
  bool SupportsTimeCompare;
  bool SupportsRangeQueries;
  bool SupportsLineMapping;
  StringRef DiffBaseKind;
  StringRef DefaultVariant;
};

extern const RepresentationCapabilityDescriptor RepresentationCapabilities[];

class UnitManifestIndex {
public:
  explicit UnitManifestIndex(StringRef DataDir);

  auto load(StringRef UnitName) -> Error;
  auto getLatestRunDir() const -> StringRef;
  auto listSources() const -> json::Array;
  auto listRepresentationDescriptors() const -> json::Array;
  auto readSource(StringRef SourceRef) const -> Expected<json::Object>;
  auto readRepresentation(StringRef SourceRef, StringRef Kind) const
      -> Expected<json::Object>;
  auto correlateSignals(StringRef SourceRef, StringRef Kind) const
      -> Expected<json::Object>;
  auto listRepresentationFilesByCategory(StringRef Category) const
      -> Expected<std::vector<std::string>>;
  auto countRepresentationKinds() const -> json::Object;
  auto countRepresentationCategories() const -> json::Object;

  static auto representationKindToCategory(StringRef Kind) -> StringRef;
  static auto representationKindToSyntax(StringRef Kind) -> StringRef;
  static auto categoryToRepresentationKind(StringRef Category) -> StringRef;

private:
  auto resolveSourceDescriptor(StringRef SourceRef) const
      -> Expected<SourceDescriptor>;
  auto buildSourceSignals(const SourceDescriptor &Source, StringRef Kind) const
      -> json::Object;
  auto buildRepresentationSignals(const SourceDescriptor &Source, StringRef Kind,
                                  const json::Object &RepresentationPayload,
                                  const json::Object &SourceSignals) const
      -> json::Object;
  auto resolveRepresentationPath(StringRef RelativePath) const
      -> Expected<std::string>;

  static auto jsonLineNumber(const json::Object &Object, StringRef Key)
      -> std::optional<int64_t>;
  static auto locationMatchesSource(StringRef LocationFile,
                                    const SourceDescriptor &Source) -> bool;
  static auto extractRepresentationFunctionLineMap(StringRef Content,
                                                   StringRef Kind)
      -> StringMap<int64_t>;
  static auto mappedRepresentationLine(const json::Value *LineMapping,
                                       int64_t SourceLine)
      -> std::optional<int64_t>;
  static auto findLatestRunDir(StringRef UnitRoot) -> Expected<std::string>;

  std::string DataDir;
  std::string LatestRunDir;
  std::string LatestRunRealDir;
  json::Object Manifest;
};

auto parseRequest(StringRef RequestJson) -> Expected<RunnerRequest>;
auto parseRepresentationRequest(StringRef RequestJson)
    -> Expected<RepresentationRequest>;
auto parseCompareRequest(StringRef RequestJson) -> Expected<CompareRequest>;
auto serializeJson(json::Value Value) -> Expected<std::string>;
auto buildRepresentationCapability(
    const RepresentationCapabilityDescriptor &Descriptor) -> json::Object;
auto buildRepresentationCapabilities() -> json::Array;
auto listUnits(StringRef DataDir) -> Expected<json::Array>;
auto listUnitNames(StringRef DataDir) -> Expected<std::vector<std::string>>;
auto executeCapability(const RunnerRequest &Request) -> Expected<json::Object>;
auto compareCapabilities(const CompareRequest &Request)
    -> Expected<json::Object>;
auto buildASTSummary(const UnitManifestIndex &Index) -> Expected<json::Object>;
auto buildPassStats(const UnitManifestIndex &Index) -> Expected<json::Object>;
auto buildObjectSummary(const UnitManifestIndex &Index)
    -> Expected<json::Object>;
auto buildDebugSummary(const UnitManifestIndex &Index) -> Expected<json::Object>;
auto buildTopCounts(const StringMap<int64_t> &Counts, size_t Limit = 12)
    -> json::Object;
void collectASTStats(const json::Value &Value, StringMap<int64_t> &DeclKinds,
                     int64_t &DeclCount, int64_t &FunctionCount,
                     int64_t &RecordCount, int64_t &NamespaceCount,
                     int64_t &TemplateCount, int64_t &VariableCount);

void collectNumericMetrics(StringRef Prefix, const json::Value &Value,
                           std::map<std::string, double> &Metrics);
void collectNumericMetrics(StringRef Prefix, const json::Object &Object,
                           std::map<std::string, double> &Metrics);
void collectNumericMetrics(StringRef Prefix, const json::Array &Array,
                           std::map<std::string, double> &Metrics);

} // namespace llvm::advisor::detail

#endif // LLVM_TOOLS_LLVM_ADVISOR_SRC_CORE_CAPABILITYRUNNERINTERNAL_H
