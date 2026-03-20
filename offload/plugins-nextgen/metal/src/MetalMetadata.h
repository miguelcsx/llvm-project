//===- MetalMetadata.h - Structured metadata for Metal images --*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef OPENMP_LIBOMPTARGET_PLUGINS_NEXTGEN_METAL_METADATA_H
#define OPENMP_LIBOMPTARGET_PLUGINS_NEXTGEN_METAL_METADATA_H

#include "PluginInterface.h"

#include "llvm/ADT/StringMap.h"
#include "llvm/Frontend/OpenMP/KernelEnvironment.h"

#include <optional>
#include <string>

namespace llvm::omp::target::plugin {

static constexpr unsigned MetalDescriptorVersion = 1;

struct MetalKernelMetadata {
  std::string EntryName;
  KernelEnvironmentTy Environment = KernelEnvironmentTy{};
  bool HasEnvironment = false;
};

class MetalImageMetadata final {
public:
  MetalImageMetadata() = default;
  explicit MetalImageMetadata(unsigned Version) : Version(Version) {}

  unsigned getVersion() const { return Version; }

  void addKernel(StringRef Name, MetalKernelMetadata Metadata);

  const MetalKernelMetadata *findKernel(StringRef Name) const;

private:
  unsigned Version = MetalDescriptorVersion;
  StringMap<MetalKernelMetadata> Kernels;
};

Expected<MetalImageMetadata> parseMetalImageMetadata(StringRef Payload);

} // namespace llvm::omp::target::plugin

#endif
