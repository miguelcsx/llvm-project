//===--------------- CapabilityRuntime.cpp - LLVM Advisor -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "CapabilityRunner.h"
#include "llvm/Support/Error.h"
#include <cstdlib>
#include <cstring>
#include <string>

using namespace llvm;

namespace {

const char *dupResult(Expected<std::string> ResultOrErr) {
  if (!ResultOrErr) {
    std::string ErrorMessage = toString(ResultOrErr.takeError());
    return ::strdup(ErrorMessage.c_str());
  }

  return ::strdup(ResultOrErr->c_str());
}

} // namespace

extern "C" const char *llvm_advisor_run_capability(const char *RequestJson) {
  if (!RequestJson)
    return nullptr;

  return dupResult(llvm::advisor::CapabilityRunner::executeFromJson(RequestJson));
}

extern "C" const char *
llvm_advisor_materialize_representation(const char *RequestJson) {
  if (!RequestJson)
    return nullptr;

  return dupResult(
      llvm::advisor::CapabilityRunner::materializeRepresentationFromJson(
          RequestJson));
}

extern "C" const char *llvm_advisor_correlate_signals(const char *RequestJson) {
  if (!RequestJson)
    return nullptr;

  return dupResult(
      llvm::advisor::CapabilityRunner::correlateSignalsFromJson(RequestJson));
}

extern "C" const char *llvm_advisor_list_units(const char *DataDir) {
  if (!DataDir)
    return nullptr;

  return dupResult(llvm::advisor::CapabilityRunner::listUnitsAsJson(DataDir));
}

extern "C" const char *llvm_advisor_compare_capabilities(const char *RequestJson) {
  if (!RequestJson)
    return nullptr;

  return dupResult(
      llvm::advisor::CapabilityRunner::compareCapabilitiesFromJson(RequestJson));
}

extern "C" void llvm_advisor_free_string(const char *Value) {
  if (Value)
    ::free(const_cast<char *>(Value));
}
