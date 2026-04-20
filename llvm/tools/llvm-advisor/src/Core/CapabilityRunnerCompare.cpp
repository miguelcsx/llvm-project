//===------------- CapabilityRunnerCompare.cpp - LLVM Advisor ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "CapabilityRunnerInternal.h"
#include "llvm/ADT/StringSet.h"

using namespace llvm;

namespace llvm::advisor::detail {

void collectNumericMetrics(StringRef Prefix, const json::Object &Object,
                           std::map<std::string, double> &Metrics) {
  for (const auto &Entry : Object) {
    std::string Key =
        Prefix.empty() ? Entry.first.str()
                       : (Twine(Prefix) + "." + Entry.first.str()).str();
    collectNumericMetrics(Key, Entry.second, Metrics);
  }
}

void collectNumericMetrics(StringRef Prefix, const json::Array &Array,
                           std::map<std::string, double> &Metrics) {
  for (size_t I = 0; I < Array.size(); ++I)
    collectNumericMetrics((Twine(Prefix) + "[" + Twine(I) + "]").str(), Array[I],
                          Metrics);
}

void collectNumericMetrics(StringRef Prefix, const json::Value &Value,
                           std::map<std::string, double> &Metrics) {
  if (auto Number = Value.getAsNumber()) {
    Metrics[Prefix.str()] = *Number;
    return;
  }
  if (const auto *Object = Value.getAsObject())
    return collectNumericMetrics(Prefix, *Object, Metrics);
  if (const auto *Array = Value.getAsArray())
    collectNumericMetrics(Prefix, *Array, Metrics);
}

Expected<json::Object> compareCapabilities(const CompareRequest &Request) {
  std::vector<std::string> MatchedUnits;
  if (!Request.UnitName.empty()) {
    MatchedUnits.push_back(Request.UnitName);
  } else {
    auto BaseUnitsOrErr = listUnitNames(Request.BaseDataDir);
    if (!BaseUnitsOrErr)
      return BaseUnitsOrErr.takeError();
    auto CandidateUnitsOrErr = listUnitNames(Request.CandidateDataDir);
    if (!CandidateUnitsOrErr)
      return CandidateUnitsOrErr.takeError();

    StringSet<> CandidateUnitSet;
    for (const std::string &UnitName : *CandidateUnitsOrErr)
      CandidateUnitSet.insert(UnitName);
    for (const std::string &UnitName : *BaseUnitsOrErr)
      if (CandidateUnitSet.contains(UnitName))
        MatchedUnits.push_back(UnitName);
  }

  if (MatchedUnits.empty())
    return createStringError(std::make_error_code(std::errc::no_such_file_or_directory),
                             "No matching units found for compare request");

  json::Array UnitMatches;
  json::Array Changes;
  std::vector<std::pair<std::string, int64_t>> UnitChangeCounts;
  int64_t ChangeCount = 0;

  for (const std::string &UnitName : MatchedUnits) {
    RunnerRequest BaseRunner{Request.BaseDataDir, Request.CapabilityId, UnitName};
    RunnerRequest CandidateRunner{Request.CandidateDataDir, Request.CapabilityId,
                                  UnitName};
    auto BasePayloadOrErr = executeCapability(BaseRunner);
    if (!BasePayloadOrErr)
      return BasePayloadOrErr.takeError();
    auto CandidatePayloadOrErr = executeCapability(CandidateRunner);
    if (!CandidatePayloadOrErr)
      return CandidatePayloadOrErr.takeError();

    std::map<std::string, double> BaseMetrics;
    std::map<std::string, double> CandidateMetrics;
    collectNumericMetrics("", *BasePayloadOrErr, BaseMetrics);
    collectNumericMetrics("", *CandidatePayloadOrErr, CandidateMetrics);

    StringSet<> MetricKeys;
    for (const auto &Entry : BaseMetrics)
      MetricKeys.insert(Entry.first);
    for (const auto &Entry : CandidateMetrics)
      MetricKeys.insert(Entry.first);

    int64_t UnitChangedMetrics = 0;
    for (const auto &MetricKey : MetricKeys) {
      auto BaseIt = BaseMetrics.find(MetricKey.getKey().str());
      auto CandidateIt = CandidateMetrics.find(MetricKey.getKey().str());
      double BaseValue = BaseIt == BaseMetrics.end() ? 0.0 : BaseIt->second;
      double CandidateValue =
          CandidateIt == CandidateMetrics.end() ? 0.0 : CandidateIt->second;
      double AbsoluteDelta = CandidateValue - BaseValue;
      if (AbsoluteDelta == 0.0)
        continue;

      ++ChangeCount;
      ++UnitChangedMetrics;
      double RelativeDelta = AbsoluteDelta / std::max(std::abs(BaseValue), 1.0);
      double Magnitude = std::abs(RelativeDelta);
      StringRef ChangeMagnitude =
          Magnitude >= 1.0 ? "very_high"
          : Magnitude >= 0.5 ? "high"
          : Magnitude >= 0.2 ? "medium"
          : Magnitude >= 0.05 ? "low"
                              : "stable";

      json::Object Change;
      Change["unit_name"] = UnitName;
      Change["metric_key"] = MetricKey.getKey().str();
      Change["base_value"] = BaseValue;
      Change["candidate_value"] = CandidateValue;
      Change["absolute_delta"] = AbsoluteDelta;
      Change["relative_delta"] = RelativeDelta;
      Change["delta_sign"] =
          AbsoluteDelta > 0.0 ? "increase"
                              : (AbsoluteDelta < 0.0 ? "decrease" : "stable");
      Change["change_magnitude"] = ChangeMagnitude;
      Changes.push_back(std::move(Change));
    }

    json::Object Match;
    Match["unit_name"] = UnitName;
    Match["change_count"] = UnitChangedMetrics;
    UnitMatches.push_back(std::move(Match));
    UnitChangeCounts.emplace_back(UnitName, UnitChangedMetrics);
  }

  llvm::sort(UnitChangeCounts, [](const auto &Left, const auto &Right) {
    if (Left.second != Right.second)
      return Left.second > Right.second;
    return Left.first < Right.first;
  });

  json::Array UnitChangeLeaderboard;
  for (const auto &Entry : UnitChangeCounts) {
    if (Entry.second == 0)
      continue;
    json::Object UnitChange;
    UnitChange["unit_name"] = Entry.first;
    UnitChange["change_count"] = Entry.second;
    UnitChangeLeaderboard.push_back(std::move(UnitChange));
  }

  json::Object Summary;
  Summary["matched_unit_count"] = static_cast<int64_t>(MatchedUnits.size());
  Summary["change_count"] = ChangeCount;

  json::Object Result;
  Result["base_data_dir"] = Request.BaseDataDir;
  Result["candidate_data_dir"] = Request.CandidateDataDir;
  Result["capability_id"] = Request.CapabilityId;
  Result["unit_matches"] = std::move(UnitMatches);
  Result["unit_change_leaderboard"] = std::move(UnitChangeLeaderboard);
  Result["summary"] = std::move(Summary);
  Result["changes"] = std::move(Changes);
  return Result;
}

} // namespace llvm::advisor::detail
