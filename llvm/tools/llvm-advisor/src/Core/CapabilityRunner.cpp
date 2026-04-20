//===----------------- CapabilityRunner.cpp - LLVM Advisor ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "CapabilityRunner.h"
#include "CapabilityRunnerInternal.h"

using namespace llvm;

namespace llvm::advisor {

Expected<std::string> CapabilityRunner::executeFromJson(StringRef RequestJson) {
  auto RequestOrErr = detail::parseRequest(RequestJson);
  if (!RequestOrErr)
    return RequestOrErr.takeError();

  auto ResultOrErr = detail::executeCapability(*RequestOrErr);
  if (!ResultOrErr)
    return ResultOrErr.takeError();

  json::Object Response;
  Response["success"] = true;
  Response["capability_id"] = RequestOrErr->CapabilityId;
  Response["unit_name"] = RequestOrErr->UnitName;
  Response["data"] = std::move(*ResultOrErr);
  return detail::serializeJson(json::Value(std::move(Response)));
}

Expected<std::string> CapabilityRunner::listUnitsAsJson(StringRef DataDir) {
  auto UnitsOrErr = detail::listUnits(DataDir);
  if (!UnitsOrErr)
    return UnitsOrErr.takeError();

  json::Object Response;
  Response["success"] = true;
  Response["data_dir"] = DataDir.str();
  Response["units"] = std::move(*UnitsOrErr);
  return detail::serializeJson(json::Value(std::move(Response)));
}

Expected<std::string> CapabilityRunner::listRepresentationCapabilitiesAsJson() {
  json::Object Response;
  Response["success"] = true;
  Response["capabilities"] = detail::buildRepresentationCapabilities();
  return detail::serializeJson(json::Value(std::move(Response)));
}

Expected<std::string>
CapabilityRunner::materializeRepresentationFromJson(StringRef RequestJson) {
  auto RequestOrErr = detail::parseRepresentationRequest(RequestJson);
  if (!RequestOrErr)
    return RequestOrErr.takeError();

  detail::UnitManifestIndex Index(RequestOrErr->DataDir);
  if (auto Err = Index.load(RequestOrErr->UnitName))
    return std::move(Err);

  auto PayloadOrErr =
      Index.readRepresentation(RequestOrErr->SourceRef, RequestOrErr->Kind);
  if (!PayloadOrErr)
    return PayloadOrErr.takeError();

  json::Object Response;
  Response["success"] = true;
  Response["unit_name"] = RequestOrErr->UnitName;
  Response["source_ref"] = RequestOrErr->SourceRef;
  Response["kind"] = RequestOrErr->Kind;
  Response["data"] = std::move(*PayloadOrErr);
  return detail::serializeJson(json::Value(std::move(Response)));
}

Expected<std::string>
CapabilityRunner::correlateSignalsFromJson(StringRef RequestJson) {
  auto RequestOrErr = detail::parseRepresentationRequest(RequestJson);
  if (!RequestOrErr)
    return RequestOrErr.takeError();

  detail::UnitManifestIndex Index(RequestOrErr->DataDir);
  if (auto Err = Index.load(RequestOrErr->UnitName))
    return std::move(Err);

  auto PayloadOrErr =
      Index.correlateSignals(RequestOrErr->SourceRef, RequestOrErr->Kind);
  if (!PayloadOrErr)
    return PayloadOrErr.takeError();

  json::Object Response;
  Response["success"] = true;
  Response["unit_name"] = RequestOrErr->UnitName;
  Response["source_ref"] = RequestOrErr->SourceRef;
  Response["kind"] = RequestOrErr->Kind;
  Response["data"] = std::move(*PayloadOrErr);
  return detail::serializeJson(json::Value(std::move(Response)));
}

Expected<std::string>
CapabilityRunner::compareCapabilitiesFromJson(StringRef RequestJson) {
  auto RequestOrErr = detail::parseCompareRequest(RequestJson);
  if (!RequestOrErr)
    return RequestOrErr.takeError();

  auto ResultOrErr = detail::compareCapabilities(*RequestOrErr);
  if (!ResultOrErr)
    return ResultOrErr.takeError();

  json::Object Response;
  Response["success"] = true;
  Response["data"] = std::move(*ResultOrErr);
  return detail::serializeJson(json::Value(std::move(Response)));
}

} // namespace llvm::advisor
