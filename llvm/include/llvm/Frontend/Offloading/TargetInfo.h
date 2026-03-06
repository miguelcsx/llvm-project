//===- TargetInfo.h - Offloading target helpers ----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Helpers shared by the offloading driver and linker wrapper to reason about
// target metadata that is carried partly in the offload triple and partly in
// auxiliary image properties such as the offload architecture.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_FRONTEND_OFFLOADING_TARGETINFO_H
#define LLVM_FRONTEND_OFFLOADING_TARGETINFO_H

#include "llvm/ADT/StringRef.h"
#include "llvm/Object/OffloadBinary.h"
#include "llvm/TargetParser/Triple.h"

namespace llvm::offloading {

inline bool isMetalOpenMPOffloadTarget(const Triple &Triple, StringRef Arch,
                                       object::OffloadKind Kind) {
  if (Kind != object::OFK_OpenMP || !Triple.isSPIRV())
    return false;

  // The driver currently records Metal selection via arch=applegpu while some
  // transitional metadata still uses environment=applegpu. Accept both.
  return Arch == "applegpu" || Triple.getEnvironmentName() == "applegpu";
}

inline Triple getMetalOpenMPToolchainTriple(const Triple &Triple) {
  if (!Triple.isSPIRV())
    return Triple;

  // Keep the user-facing offload triple Apple-specific for packaging and
  // runtime selection, but compile and link Metal-bound SPIR-V with the
  // canonical backend triple understood by the SPIR-V target.
  return llvm::Triple((Triple.getArchName() + "-unknown-unknown").str());
}

} // namespace llvm::offloading

#endif // LLVM_FRONTEND_OFFLOADING_TARGETINFO_H
