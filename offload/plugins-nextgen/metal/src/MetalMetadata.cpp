//===- MetalMetadata.cpp - Structured metadata for Metal images ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "MetalMetadata.h"

#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/JSON.h"

#include <limits>
#include <optional>

namespace llvm::omp::target::plugin {

using namespace llvm;
using namespace error;

void MetalImageMetadata::addKernel(StringRef Name,
                                   MetalKernelMetadata Metadata) {
  Kernels[Name] = std::move(Metadata);
}

const MetalKernelMetadata *
MetalImageMetadata::findKernel(StringRef Name) const {
  auto It = Kernels.find(Name);
  if (It == Kernels.end())
    return nullptr;
  return &It->second;
}

static Error metadataError(const Twine &Message) {
  return Plugin::error(ErrorCode::INVALID_BINARY, "%s", Message.str().c_str());
}

static Expected<int64_t> getRequiredInteger(const json::Object &Object,
                                            StringRef Key, StringRef Context) {
  std::optional<int64_t> Value = Object.getInteger(Key);
  if (!Value)
    return metadataError(Twine(Context) + " is missing integer field '" + Key +
                         "'");
  return *Value;
}

static std::optional<StringRef> getOptionalString(const json::Object &Object,
                                                  StringRef Key) {
  if (std::optional<StringRef> Value = Object.getString(Key))
    return *Value;
  return std::nullopt;
}

static Expected<uint8_t> parseTriStateValue(const json::Value &Value,
                                            const Twine &Context) {
  if (std::optional<bool> BoolValue = Value.getAsBoolean())
    return static_cast<uint8_t>(*BoolValue);

  if (std::optional<int64_t> IntValue = Value.getAsInteger()) {
    if (*IntValue < 0 || *IntValue > 2)
      return metadataError(Twine(Context) + " must be in range [0, 2]");
    return static_cast<uint8_t>(*IntValue);
  }

  if (std::optional<StringRef> Text = Value.getAsString()) {
    auto Parsed = StringSwitch<std::optional<uint8_t>>(*Text)
                      .CaseLower("false", uint8_t(0))
                      .CaseLower("true", uint8_t(1))
                      .CaseLower("unknown", uint8_t(2))
                      .Default(std::nullopt);
    if (Parsed)
      return *Parsed;
  }

  return metadataError(Twine(Context) +
                       " must be a boolean, integer, or tri-state string");
}

static Expected<int32_t> parseInt32Value(const json::Value &Value,
                                         const Twine &Context) {
  std::optional<int64_t> IntValue = Value.getAsInteger();
  if (!IntValue)
    return metadataError(Twine(Context) + " must be an integer");
  if (*IntValue < std::numeric_limits<int32_t>::min() ||
      *IntValue > std::numeric_limits<int32_t>::max())
    return metadataError(Twine(Context) + " is out of range");
  return static_cast<int32_t>(*IntValue);
}

static Expected<uint8_t> parseExecutionModeValue(const json::Value &Value,
                                                 const Twine &Context) {
  if (std::optional<int64_t> IntValue = Value.getAsInteger()) {
    if (!GenericKernelTy::isValidExecutionMode(
            static_cast<OMPTgtExecModeFlags>(*IntValue)))
      return metadataError(Twine(Context) + " has an invalid execution mode");
    return static_cast<uint8_t>(*IntValue);
  }

  if (std::optional<StringRef> Text = Value.getAsString()) {
    auto Parsed =
        StringSwitch<std::optional<uint8_t>>(*Text)
            .CaseLower("bare", uint8_t(OMP_TGT_EXEC_MODE_BARE))
            .CaseLower("spmd", uint8_t(OMP_TGT_EXEC_MODE_SPMD))
            .CaseLower("generic", uint8_t(OMP_TGT_EXEC_MODE_GENERIC))
            .CaseLower("generic-spmd", uint8_t(OMP_TGT_EXEC_MODE_GENERIC_SPMD))
            .CaseLower("spmd-no-loop", uint8_t(OMP_TGT_EXEC_MODE_SPMD_NO_LOOP))
            .Default(std::nullopt);
    if (Parsed)
      return *Parsed;
  }

  return metadataError(Twine(Context) +
                       " must be a valid execution mode string or integer");
}

static Error parseKernelEnvironment(const json::Object &Object, StringRef Name,
                                    MetalKernelMetadata &Metadata) {
  Metadata.Environment.Configuration.ExecMode = OMP_TGT_EXEC_MODE_GENERIC;

  auto setIntField = [&](StringRef Key, int32_t &Field) -> Error {
    if (const auto *Value = Object.get(Key)) {
      auto Parsed =
          parseInt32Value(*Value, Twine("kernel '") + Name +
                                      "' environment field '" + Key + "'");
      if (!Parsed)
        return Parsed.takeError();
      Field = *Parsed;
      Metadata.HasEnvironment = true;
    }
    return Plugin::success();
  };

  if (const auto *Value = Object.get("exec_mode")) {
    auto Parsed = parseExecutionModeValue(
        *Value, Twine("kernel '") + Name + "' environment field 'exec_mode'");
    if (!Parsed)
      return Parsed.takeError();
    Metadata.Environment.Configuration.ExecMode = *Parsed;
    Metadata.HasEnvironment = true;
  }

  auto setTriStateField = [&](StringRef Key, uint8_t &Field) -> Error {
    if (const auto *Value = Object.get(Key)) {
      auto Parsed =
          parseTriStateValue(*Value, Twine("kernel '") + Name +
                                         "' environment field '" + Key + "'");
      if (!Parsed)
        return Parsed.takeError();
      Field = *Parsed;
      Metadata.HasEnvironment = true;
    }
    return Plugin::success();
  };

  if (auto Err = setTriStateField(
          "use_generic_state_machine",
          Metadata.Environment.Configuration.UseGenericStateMachine))
    return Err;
  if (auto Err = setTriStateField(
          "may_use_nested_parallelism",
          Metadata.Environment.Configuration.MayUseNestedParallelism))
    return Err;
  if (auto Err = setIntField("min_threads",
                             Metadata.Environment.Configuration.MinThreads))
    return Err;
  if (auto Err = setIntField("max_threads",
                             Metadata.Environment.Configuration.MaxThreads))
    return Err;
  if (auto Err =
          setIntField("min_teams", Metadata.Environment.Configuration.MinTeams))
    return Err;
  if (auto Err =
          setIntField("max_teams", Metadata.Environment.Configuration.MaxTeams))
    return Err;
  if (auto Err =
          setIntField("reduction_data_size",
                      Metadata.Environment.Configuration.ReductionDataSize))
    return Err;
  if (auto Err =
          setIntField("reduction_buffer_length",
                      Metadata.Environment.Configuration.ReductionBufferLength))
    return Err;

  if (Metadata.HasEnvironment &&
      !GenericKernelTy::isValidExecutionMode(static_cast<OMPTgtExecModeFlags>(
          Metadata.Environment.Configuration.ExecMode)))
    return metadataError(Twine("kernel '") + Name +
                         "' contains an invalid execution mode");

  if (Metadata.Environment.Configuration.MaxThreads > 0 &&
      Metadata.Environment.Configuration.MinThreads > 0 &&
      Metadata.Environment.Configuration.MinThreads >
          Metadata.Environment.Configuration.MaxThreads)
    return metadataError(Twine("kernel '") + Name +
                         "' has min_threads greater than max_threads");

  if (Metadata.Environment.Configuration.MaxTeams > 0 &&
      Metadata.Environment.Configuration.MinTeams > 0 &&
      Metadata.Environment.Configuration.MinTeams >
          Metadata.Environment.Configuration.MaxTeams)
    return metadataError(Twine("kernel '") + Name +
                         "' has min_teams greater than max_teams");

  return Plugin::success();
}

Expected<MetalImageMetadata> parseMetalImageMetadata(StringRef Payload) {
  if (Payload.empty())
    return metadataError("missing required Metal descriptor payload");

  Expected<json::Value> Parsed = json::parse(Payload);
  if (!Parsed)
    return metadataError(Twine("failed to parse Metal descriptor JSON: ") +
                         toString(Parsed.takeError()));

  const json::Object *Root = Parsed->getAsObject();
  if (!Root)
    return metadataError("Metal descriptor must be a JSON object");

  auto VersionOrErr = getRequiredInteger(*Root, "version", "Metal descriptor");
  if (!VersionOrErr)
    return VersionOrErr.takeError();
  if (*VersionOrErr <= 0 || static_cast<uint64_t>(*VersionOrErr) >
                                std::numeric_limits<unsigned>::max())
    return metadataError("Metal descriptor version is out of range");

  MetalImageMetadata Metadata(static_cast<unsigned>(*VersionOrErr));
  if (Metadata.getVersion() != MetalDescriptorVersion)
    return metadataError(Twine("unsupported Metal descriptor version '") +
                         Twine(Metadata.getVersion()) + "'");

  const json::Object *Kernels = Root->getObject("kernels");
  if (!Kernels)
    return metadataError("Metal descriptor is missing object field 'kernels'");

  for (const auto &Entry : *Kernels) {
    std::string KernelName = Entry.first.str();
    const json::Object *KernelObject = Entry.second.getAsObject();
    if (!KernelObject)
      return metadataError(Twine("kernel '") + KernelName +
                           "' descriptor must be an object");

    MetalKernelMetadata Kernel;
    Kernel.EntryName =
        getOptionalString(*KernelObject, "entry").value_or(KernelName).str();

    if (const json::Object *Environment =
            KernelObject->getObject("environment"))
      if (auto Err = parseKernelEnvironment(*Environment, KernelName, Kernel))
        return std::move(Err);

    Metadata.addKernel(KernelName, std::move(Kernel));
  }

  return Metadata;
}

} // namespace llvm::omp::target::plugin
