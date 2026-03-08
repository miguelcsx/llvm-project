//==- SPIRVOpenMP.cpp - SPIR-V OpenMP Tool Implementations --------*- C++ -*==//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//==------------------------------------------------------------------------==//
#include "SPIRVOpenMP.h"
#include "clang/Driver/CommonArgs.h"
#include "llvm/Frontend/Offloading/TargetInfo.h"

using namespace clang::driver;
using namespace clang::driver::toolchains;
using namespace clang::driver::tools;
using namespace llvm::opt;

namespace clang::driver::toolchains {
SPIRVOpenMPToolChain::SPIRVOpenMPToolChain(const Driver &D,
                                           const llvm::Triple &Triple,
                                           const ToolChain &HostToolchain,
                                           const ArgList &Args)
    : SPIRVToolChain(D, Triple, Args), HostTC(HostToolchain) {}

void SPIRVOpenMPToolChain::addClangTargetOptions(
    const llvm::opt::ArgList &DriverArgs, llvm::opt::ArgStringList &CC1Args,
    Action::OffloadKind DeviceOffloadingKind) const {

  if (DeviceOffloadingKind != Action::OFK_OpenMP)
    return;

  if (!DriverArgs.hasFlag(options::OPT_offloadlib, options::OPT_no_offloadlib,
                          true))
    return;
  addOpenMPDeviceRTL(getDriver(), DriverArgs, CC1Args, "", getTriple(), HostTC);
}

std::string SPIRVOpenMPToolChain::ComputeLLVMTriple(const ArgList &Args,
                                                    types::ID InputType) const {
  llvm::Triple Triple(SPIRVToolChain::ComputeLLVMTriple(Args, InputType));
  if (!Triple.isSPIRV())
    return Triple.getTriple();

  if (Triple.getEnvironmentName() != "applegpu" && !Triple.isOSDarwin())
    return Triple.getTriple();
  return llvm::offloading::getMetalOpenMPToolchainTriple(Triple).getTriple();
}

llvm::Expected<llvm::SmallVector<std::string>>
SPIRVOpenMPToolChain::getSystemGPUArchs(const llvm::opt::ArgList &) const {
  llvm::SmallVector<std::string> Archs;
  if (getTriple().isMacOSX())
    Archs.emplace_back("applegpu");
  return Archs;
}
} // namespace clang::driver::toolchains
