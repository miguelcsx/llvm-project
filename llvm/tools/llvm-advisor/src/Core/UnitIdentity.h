//===--------------------- UnitIdentity.h - LLVM Advisor ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_ADVISOR_SRC_CORE_UNITIDENTITY_H
#define LLVM_TOOLS_LLVM_ADVISOR_SRC_CORE_UNITIDENTITY_H

#include "CompilationUnit.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include <string>

namespace llvm::advisor {

struct UnitIdentity {
  std::string unitId;
  std::string commandFingerprint;
  std::string sourceFingerprint;
  std::string targetTriple;
  std::string language;
  llvm::SmallVector<std::string, 16> commandNormalized;
};

class UnitIdentityBuilder {
public:
  static auto build(const CompilationUnitInfo &UnitInfo,
                    llvm::StringRef PrimarySourcePath,
                    llvm::StringRef WorkingDirectory) -> UnitIdentity;
};

} // namespace llvm::advisor

#endif // LLVM_TOOLS_LLVM_ADVISOR_SRC_CORE_UNITIDENTITY_H
