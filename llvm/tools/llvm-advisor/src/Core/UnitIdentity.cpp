//===-------------------- UnitIdentity.cpp - LLVM Advisor ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "UnitIdentity.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SHA256.h"
#include "llvm/TargetParser/Host.h"
#include <algorithm>
#include <cstring>

namespace llvm::advisor {

namespace {

auto normalizePath(llvm::StringRef Path, llvm::StringRef WorkingDirectory)
    -> std::string {
  if (Path.empty())
    return "";

  llvm::SmallString<256> AbsolutePath(Path);
  if (!llvm::sys::path::is_absolute(AbsolutePath)) {
    llvm::SmallString<256> Root(WorkingDirectory);
    llvm::sys::path::append(Root, AbsolutePath);
    AbsolutePath = Root;
  }

  llvm::sys::path::remove_dots(AbsolutePath, /*remove_dot_dot=*/true);
  return std::string(AbsolutePath.str());
}

auto sha256Hex(llvm::StringRef Payload) -> std::string {
  return llvm::toHex(llvm::SHA256::hash(llvm::arrayRefFromStringRef(Payload)),
                     /*LowerCase=*/true);
}

auto sha256Digest(llvm::StringRef Payload) -> std::string {
  return "sha256:" + sha256Hex(Payload);
}

auto detectLanguage(const CompilationUnitInfo &UnitInfo,
                    llvm::StringRef PrimarySourcePath) -> std::string {
  for (const auto &Source : UnitInfo.sources) {
    if (Source.path == PrimarySourcePath && !Source.language.empty())
      return llvm::StringRef(Source.language).lower();
  }

  llvm::StringRef Suffix = llvm::sys::path::extension(PrimarySourcePath).lower();
  if (Suffix == ".c")
    return "c";
  return "c++";
}

auto detectTargetTriple(const CompilationUnitInfo &UnitInfo) -> std::string {
  for (size_t I = 0; I < UnitInfo.compileFlags.size(); ++I) {
    llvm::StringRef Flag(UnitInfo.compileFlags[I]);
    if (Flag.starts_with("--target="))
      return Flag.drop_front(strlen("--target=")).str();
    if (Flag == "-target" && I + 1 < UnitInfo.compileFlags.size())
      return UnitInfo.compileFlags[I + 1];
  }
  return llvm::sys::getDefaultTargetTriple();
}

auto normalizeCommand(const CompilationUnitInfo &UnitInfo,
                      llvm::StringRef PrimarySourcePath,
                      llvm::StringRef WorkingDirectory)
    -> llvm::SmallVector<std::string, 16> {
  llvm::SmallVector<std::string, 16> OrderedFlags;
  llvm::SmallVector<std::string, 16> CommutativeFlags;

  auto appendOrderedFlag = [&](llvm::StringRef Flag, llvm::StringRef Value) {
    OrderedFlags.push_back((Flag + "=" + normalizePath(Value, WorkingDirectory)).str());
  };

  auto pushSimpleFlag = [&](llvm::StringRef Flag) {
    if (Flag.empty())
      return;
    if (Flag == "-c" || Flag == "-S" || Flag == "-E")
      return;
    if (Flag.starts_with("-I") && Flag.size() > 2) {
      appendOrderedFlag("-I", Flag.drop_front(2));
      return;
    }
    if (Flag.starts_with("--sysroot=")) {
      appendOrderedFlag("--sysroot", Flag.drop_front(strlen("--sysroot=")));
      return;
    }
    CommutativeFlags.push_back(Flag.str());
  };

  auto isPairedPathFlag = [](llvm::StringRef Flag) {
    return Flag == "-isystem" || Flag == "-isysroot" || Flag == "-iprefix" ||
           Flag == "-iwithprefix" || Flag == "-iwithprefixbefore" ||
           Flag == "-idirafter" || Flag == "-iquote" || Flag == "-include" ||
           Flag == "-imacros" || Flag == "--sysroot";
  };

  for (size_t I = 0; I < UnitInfo.compileFlags.size(); ++I) {
    llvm::StringRef Flag(UnitInfo.compileFlags[I]);
    if (Flag.empty())
      continue;

    if ((Flag == "-target" || isPairedPathFlag(Flag)) &&
        I + 1 < UnitInfo.compileFlags.size()) {
      llvm::StringRef Value(UnitInfo.compileFlags[I + 1]);
      if (Flag == "-target")
        CommutativeFlags.push_back((Flag + "=" + Value).str());
      else
        appendOrderedFlag(Flag, Value);
      ++I;
      continue;
    }

    pushSimpleFlag(Flag);
  }

  llvm::sort(CommutativeFlags);

  llvm::SmallVector<std::string, 16> Result;
  llvm::StringRef CompilerPath(UnitInfo.compilerPath);
  Result.push_back(llvm::sys::path::filename(CompilerPath).str());
  Result.push_back("language=" + detectLanguage(UnitInfo, PrimarySourcePath));
  Result.push_back("target=" + detectTargetTriple(UnitInfo));
  Result.push_back("source=" + normalizePath(PrimarySourcePath, WorkingDirectory));
  Result.append(OrderedFlags.begin(), OrderedFlags.end());
  Result.append(CommutativeFlags.begin(), CommutativeFlags.end());
  return Result;
}

auto computeSourceFingerprint(llvm::StringRef PrimarySourcePath,
                              llvm::StringRef WorkingDirectory) -> std::string {
  std::string NormalizedPath = normalizePath(PrimarySourcePath, WorkingDirectory);
  auto BufferOrErr = llvm::MemoryBuffer::getFile(NormalizedPath);
  if (!BufferOrErr)
    return sha256Digest(NormalizedPath);
  return sha256Digest((*BufferOrErr)->getBuffer());
}

auto serializeCommand(
    llvm::ArrayRef<std::string> CommandNormalized) -> std::string {
  llvm::json::Array Payload;
  for (const auto &Entry : CommandNormalized)
    Payload.emplace_back(Entry);
  return llvm::formatv("{0}", llvm::json::Value(std::move(Payload))).str();
}

} // namespace

auto UnitIdentityBuilder::build(const CompilationUnitInfo &UnitInfo,
                                llvm::StringRef PrimarySourcePath,
                                llvm::StringRef WorkingDirectory)
    -> UnitIdentity {
  UnitIdentity Identity;
  Identity.language = detectLanguage(UnitInfo, PrimarySourcePath);
  Identity.targetTriple = detectTargetTriple(UnitInfo);
  Identity.commandNormalized =
      normalizeCommand(UnitInfo, PrimarySourcePath, WorkingDirectory);
  Identity.commandFingerprint =
      sha256Digest(serializeCommand(Identity.commandNormalized));
  Identity.sourceFingerprint =
      computeSourceFingerprint(PrimarySourcePath, WorkingDirectory);

  std::string Seed = Identity.commandFingerprint + "|" +
                     Identity.sourceFingerprint + "|" + Identity.targetTriple;
  Identity.unitId =
      "unit_" + llvm::StringRef(sha256Hex(Seed)).take_front(26).upper();
  return Identity;
}

} // namespace llvm::advisor
