//===- KernelEnvironment.h - OpenMP kernel environment constants-*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Canonical constants for the OpenMP device kernel environment ABI and common
// offload metadata keys.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_FRONTEND_OPENMP_KERNELENVIRONMENT_H
#define LLVM_FRONTEND_OPENMP_KERNELENVIRONMENT_H

#include "llvm/ADT/StringRef.h"

namespace llvm::omp {

namespace offload {
inline constexpr StringLiteral TripleKey = "triple";
inline constexpr StringLiteral ArchKey = "arch";
inline constexpr StringLiteral MetalDescriptorKey = "metal.descriptor";
} // namespace offload

namespace kernel_environment {
inline constexpr StringLiteral GlobalSuffix = "_kernel_environment";

inline constexpr unsigned ConfigurationIndex = 0;
inline constexpr unsigned IdentIndex = 1;
inline constexpr unsigned DynamicEnvironmentIndex = 2;
inline constexpr unsigned NumRequiredKernelEnvironmentFields = 3;

namespace configuration {
inline constexpr unsigned UseGenericStateMachineIndex = 0;
inline constexpr unsigned MayUseNestedParallelismIndex = 1;
inline constexpr unsigned ExecModeIndex = 2;
inline constexpr unsigned MinThreadsIndex = 3;
inline constexpr unsigned MaxThreadsIndex = 4;
inline constexpr unsigned MinTeamsIndex = 5;
inline constexpr unsigned MaxTeamsIndex = 6;
inline constexpr unsigned ReductionDataSizeIndex = 7;
inline constexpr unsigned ReductionBufferLengthIndex = 8;
inline constexpr unsigned NumRequiredFields = 9;
} // namespace configuration
} // namespace kernel_environment

} // namespace llvm::omp

#endif // LLVM_FRONTEND_OPENMP_KERNELENVIRONMENT_H
