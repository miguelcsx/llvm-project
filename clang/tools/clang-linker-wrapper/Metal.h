//===- Metal.h - Metal-specific linker wrapper helpers ----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_CLANG_LINKER_WRAPPER_METAL_H
#define LLVM_CLANG_TOOLS_CLANG_LINKER_WRAPPER_METAL_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Object/OffloadBinary.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Support/Error.h"
#include "llvm/TargetParser/Triple.h"

#include <optional>

namespace llvm {

struct FinalizedImageFile {
  StringRef Filename;
  object::ImageKind Kind = object::IMG_None;
  bool NeedsContainerization = true;
};

bool isMetalOpenMPTarget(const Triple &Triple, StringRef Arch,
                         object::OffloadKind Kind);

Triple getMetalOpenMPToolchainTriple(const Triple &Triple);

Expected<std::optional<std::string>>
createMetalDescriptor(ArrayRef<object::OffloadFile> Input);

Expected<FinalizedImageFile> finalizeMetalOpenMPImage(
    StringRef InputFile, const opt::ArgList &Args, const Triple &Triple,
    function_ref<Expected<StringRef>(const Twine &Prefix, StringRef Extension)>
        CreateOutputFile,
    function_ref<Expected<std::string>(StringRef Name,
                                       ArrayRef<StringRef> Paths)>
        FindProgram,
    function_ref<Error(StringRef ExecutablePath, ArrayRef<StringRef> Args,
                       std::optional<ArrayRef<StringRef>>)>
        ExecuteCommand);

} // namespace llvm

#endif // LLVM_CLANG_TOOLS_CLANG_LINKER_WRAPPER_METAL_H
