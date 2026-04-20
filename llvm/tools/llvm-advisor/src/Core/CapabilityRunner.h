//===----------------- CapabilityRunner.h - LLVM Advisor ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_ADVISOR_SRC_CORE_CAPABILITYRUNNER_H
#define LLVM_TOOLS_LLVM_ADVISOR_SRC_CORE_CAPABILITYRUNNER_H

#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"
#include <string>

namespace llvm::advisor {

class CapabilityRunner {
public:
  static auto executeFromJson(StringRef RequestJson)
      -> llvm::Expected<std::string>;
  static auto listUnitsAsJson(StringRef DataDir) -> llvm::Expected<std::string>;
  static auto listRepresentationCapabilitiesAsJson()
      -> llvm::Expected<std::string>;
  static auto materializeRepresentationFromJson(StringRef RequestJson)
      -> llvm::Expected<std::string>;
  static auto correlateSignalsFromJson(StringRef RequestJson)
      -> llvm::Expected<std::string>;
  static auto compareCapabilitiesFromJson(StringRef RequestJson)
      -> llvm::Expected<std::string>;
};

} // namespace llvm::advisor

#endif // LLVM_TOOLS_LLVM_ADVISOR_SRC_CORE_CAPABILITYRUNNER_H
