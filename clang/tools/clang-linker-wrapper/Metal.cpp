//===- Metal.cpp - Metal-specific linker wrapper helpers ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Metal.h"

#include "llvm/BinaryFormat/Magic.h"
#include "llvm/Frontend/Offloading/TargetInfo.h"
#include "llvm/Frontend/OpenMP/KernelEnvironment.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/StringSaver.h"

#include <cstdlib>
#include <map>
#include <optional>

using namespace llvm;
using namespace llvm::object;

namespace {

enum ID {
  OPT_INVALID = 0,
#define OPTION(...) LLVM_MAKE_OPT_ID(__VA_ARGS__),
#include "LinkerWrapperOpts.inc"
  LastOption
#undef OPTION
};

Expected<int64_t> getConstantInt(const Constant *C, const Twine &Context) {
  if (const auto *CI = dyn_cast_or_null<ConstantInt>(C))
    return CI->getSExtValue();

  return createStringError(Twine(Context) + " is not a constant integer");
}

Expected<const ConstantStruct *> getConstantStruct(const Constant *C,
                                                   const Twine &Context) {
  if (const auto *CS = dyn_cast_or_null<ConstantStruct>(C))
    return CS;

  return createStringError(Twine(Context) + " is not a constant struct");
}

Expected<const ConstantStruct *>
getRequiredStructField(const ConstantStruct &Struct, unsigned Index,
                       const Twine &Context) {
  if (Struct.getNumOperands() <= Index)
    return createStringError(Twine(Context) + " is missing field " +
                             Twine(Index));

  return getConstantStruct(Struct.getAggregateElement(Index), Context);
}

Expected<int64_t> getRequiredIntField(const ConstantStruct &Struct,
                                      unsigned Index, const Twine &Context) {
  if (Struct.getNumOperands() <= Index)
    return createStringError(Twine(Context) + " is missing field " +
                             Twine(Index));

  return getConstantInt(Struct.getAggregateElement(Index), Context);
}

Error appendKernelEnvironment(StringRef KernelName, const GlobalVariable &GV,
                              json::Object &Kernel) {
  using namespace omp::kernel_environment;
  using namespace omp::kernel_environment::configuration;

  if (!GV.hasInitializer())
    return createStringError(Twine("kernel environment global for '") +
                             KernelName + "' does not have an initializer");

  auto KernelEnvOrErr =
      getConstantStruct(GV.getInitializer(),
                        Twine("kernel environment for '") + KernelName + "'");
  if (!KernelEnvOrErr)
    return KernelEnvOrErr.takeError();

  const ConstantStruct *KernelEnv = *KernelEnvOrErr;
  if (KernelEnv->getNumOperands() < NumRequiredKernelEnvironmentFields)
    return createStringError(Twine("kernel environment for '") + KernelName +
                             "' does not match the required ABI shape");

  auto ConfigOrErr = getRequiredStructField(
      *KernelEnv, ConfigurationIndex,
      Twine("kernel environment configuration for '") + KernelName + "'");
  if (!ConfigOrErr)
    return ConfigOrErr.takeError();

  const ConstantStruct *Config = *ConfigOrErr;
  if (Config->getNumOperands() < NumRequiredFields)
    return createStringError(Twine("kernel environment configuration for '") +
                             KernelName +
                             "' is older than the required metadata ABI");

  json::Object Environment;
  struct FieldSpec {
    unsigned Index;
    const char *Key;
  };
  static constexpr FieldSpec Fields[] = {
      {UseGenericStateMachineIndex, "use_generic_state_machine"},
      {MayUseNestedParallelismIndex, "may_use_nested_parallelism"},
      {ExecModeIndex, "exec_mode"},
      {MinThreadsIndex, "min_threads"},
      {MaxThreadsIndex, "max_threads"},
      {MinTeamsIndex, "min_teams"},
      {MaxTeamsIndex, "max_teams"},
      {ReductionDataSizeIndex, "reduction_data_size"},
      {ReductionBufferLengthIndex, "reduction_buffer_length"},
  };
  for (auto &F : Fields) {
    auto ValOrErr = getRequiredIntField(*Config, F.Index,
                                        Twine("field '") + F.Key +
                                            "' in kernel environment for '" +
                                            KernelName + "'");
    if (!ValOrErr)
      return ValOrErr.takeError();
    Environment[F.Key] = *ValOrErr;
  }

  Kernel["entry"] = KernelName.str();
  Kernel["environment"] = std::move(Environment);
  return Error::success();
}

Expected<std::optional<std::string>>
getExistingMetalDescriptor(ArrayRef<OffloadFile> Input) {
  std::optional<std::string> Descriptor;
  for (const OffloadFile &File : Input) {
    StringRef Existing =
        File.getBinary()->getString(omp::offload::MetalDescriptorKey);
    if (Existing.empty())
      continue;

    if (!Descriptor)
      Descriptor = Existing.str();
    else if (*Descriptor != Existing)
      return createStringError(
          "conflicting Metal descriptor metadata found in device inputs");
  }

  return Descriptor;
}

Expected<std::optional<std::string>>
buildMetalDescriptorFromBitcode(ArrayRef<OffloadFile> Input) {
  std::map<std::string, json::Object> Kernels;
  size_t NumBitcodeInputs = 0;

  for (const OffloadFile &File : Input) {
    const OffloadBinary &Binary = *File.getBinary();
    if (identify_magic(Binary.getImage()) != file_magic::bitcode)
      continue;

    ++NumBitcodeInputs;

    LLVMContext Context;
    SMDiagnostic Err;
    std::unique_ptr<MemoryBuffer> Buffer = MemoryBuffer::getMemBufferCopy(
        Binary.getImage(), Binary.getTriple() + "-" + Binary.getArch());
    std::unique_ptr<Module> M =
        parseIR(Buffer->getMemBufferRef(), Err, Context);
    if (!M)
      return createStringError(Twine("failed to parse device bitcode for '") +
                               Binary.getTriple() +
                               "' while emitting Metal "
                               "metadata");

    for (const GlobalVariable &GV : M->globals()) {
      StringRef Name = GV.getName();
      if (!Name.ends_with(omp::kernel_environment::GlobalSuffix))
        continue;

      StringRef KernelName =
          Name.drop_back(omp::kernel_environment::GlobalSuffix.size());
      if (KernelName.empty())
        return createStringError("encountered an empty OpenMP kernel name");

      json::Object KernelObject;
      if (Error Err = appendKernelEnvironment(KernelName, GV, KernelObject))
        return Err;

      auto [It, Inserted] = Kernels.try_emplace(KernelName.str());
      if (!Inserted) {
        if (It->second != KernelObject)
          return createStringError(
              Twine("conflicting OpenMP kernel environment for '") +
              KernelName + "' while emitting Metal metadata");
        continue;
      }

      It->second = std::move(KernelObject);
    }
  }

  if (!NumBitcodeInputs)
    return std::optional<std::string>();

  if (Kernels.empty())
    return createStringError(
        "Metal OpenMP offload requires kernel metadata, but no "
        "'*_kernel_environment' globals were found in the device bitcode");

  json::Object Root;
  Root["version"] = 1;
  json::Object KernelMap;
  for (auto &[Name, Kernel] : Kernels)
    KernelMap[Name] = std::move(Kernel);
  Root["kernels"] = std::move(KernelMap);

  return formatv("{0}", json::Value(std::move(Root))).str();
}

std::optional<std::string> getMetalSDKRoot(const opt::ArgList &Args) {
  if (StringRef SDK = Args.getLastArgValue(OPT_sysroot_EQ); !SDK.empty())
    return SDK.str();
  if (StringRef SDK = Args.getLastArgValue(OPT_syslibroot); !SDK.empty())
    return SDK.str();
  if (const char *SDKRootEnv = ::getenv("SDKROOT"); SDKRootEnv && *SDKRootEnv)
    return std::string(SDKRootEnv);
  if (StringRef SDK = Args.getLastArgValue(OPT_metal_sdk_EQ);
      !SDK.empty() && sys::fs::is_directory(SDK))
    return SDK.str();
  return std::nullopt;
}

std::optional<std::string> getMetalSDKName(const opt::ArgList &Args) {
  if (StringRef SDK = Args.getLastArgValue(OPT_metal_sdk_EQ);
      !SDK.empty() && !sys::fs::is_directory(SDK))
    return SDK.str();
  return std::nullopt;
}

SmallVector<std::string> buildMetalEnvironment(const opt::ArgList &Args) {
  SmallVector<std::string> EnvStorage;
  if (std::optional<std::string> SDKRoot = getMetalSDKRoot(Args))
    EnvStorage.push_back("SDKROOT=" + *SDKRoot);
  return EnvStorage;
}

Error executeWithEnvironment(
    StringRef ExecutablePath, ArrayRef<StringRef> CommandArgs,
    ArrayRef<std::string> EnvStorage,
    function_ref<Error(StringRef ExecutablePath, ArrayRef<StringRef> Args,
                       std::optional<ArrayRef<StringRef>>)>
        ExecuteCommand) {
  if (EnvStorage.empty())
    return ExecuteCommand(ExecutablePath, CommandArgs, std::nullopt);

  SmallVector<StringRef, 16> WrappedArgs;
  WrappedArgs.reserve(1 + EnvStorage.size() + CommandArgs.size());
  WrappedArgs.push_back("/usr/bin/env");
  for (const std::string &Entry : EnvStorage)
    WrappedArgs.push_back(Entry);
  WrappedArgs.append(CommandArgs.begin(), CommandArgs.end());
  return ExecuteCommand("/usr/bin/env", WrappedArgs, std::nullopt);
}

Expected<std::string>
getToolPath(const opt::ArgList &Args, opt::OptSpecifier OverrideOpt,
            StringRef ToolName,
            function_ref<Expected<std::string>(StringRef Name,
                                               ArrayRef<StringRef> Paths)>
                FindProgram) {
  if (StringRef Override = Args.getLastArgValue(OverrideOpt); !Override.empty())
    return Override.str();

  Expected<std::string> ToolPath = FindProgram(ToolName, {});
  if (!ToolPath) {
    // Provide actionable diagnostic when external tools are missing.
    std::string Diagnostic = ("cannot find '" + ToolName +
                              "' in PATH, which is required for Metal offload")
                                 .str();
    if (ToolName == "spirv-cross") {
      Diagnostic += "\n  Install spirv-cross from "
                    "https://github.com/KhronosGroup/SPIRV-Cross";
      Diagnostic += "\n  Or specify the path explicitly with "
                    "'--metal-translation-path=/path/to/spirv-cross'";
    } else if (ToolName == "xcrun") {
      Diagnostic += "\n  Install Xcode Command Line Tools: xcode-select "
                    "--install";
      Diagnostic += "\n  Or specify Metal tools explicitly with "
                    "'--metal-compiler-path' and '--metallib-path'";
    }
    return createStringError(inconvertibleErrorCode(), Diagnostic);
  }

  return ToolPath;
}

// Return the Metal Shading Language version number (MAJOR*10000 + MINOR*100)
// that corresponds to the deployment target in Triple.  The version is passed
// to spirv-cross so it emits MSL features appropriate for the target.
static uint32_t getMSLVersion(const Triple &Triple) {
  VersionTuple OS = Triple.getOSVersion();
  unsigned Major = OS.getMajor();
  unsigned Minor = OS.getMinor().value_or(0);

  if (Triple.isMacOSX()) {
    // macOS 10.x: MSL version tracks the minor.
    if (Major == 10) {
      if (Minor >= 15)
        return 21000; // MSL 2.1
      if (Minor >= 14)
        return 20000; // MSL 2.0
      return 10200;   // MSL 1.2 (10.11+)
    }
    // macOS 11+ maps linearly to MSL 2.3, 2.4, 3.0, 3.1, 3.2 ...
    static constexpr struct {
      unsigned MacOS;
      uint32_t MSL;
    } kMacTable[] = {
        {15, 32000}, {14, 31000}, {13, 30000}, {12, 24000}, {11, 23000},
    };
    for (auto &E : kMacTable)
      if (Major >= E.MacOS)
        return E.MSL;
    return 20000;
  }
  if (Triple.isiOS() || Triple.isTvOS()) {
    static constexpr struct {
      unsigned iOS;
      uint32_t MSL;
    } kIOSTable[] = {
        {17, 31000}, {16, 30000}, {15, 24000}, {14, 23000}, {13, 22000},
    };
    for (auto &E : kIOSTable)
      if (Major >= E.iOS)
        return E.MSL;
    return 20000;
  }
  return 20000; // Safe baseline: MSL 2.0
}

void appendMetalDeploymentTarget(SmallVectorImpl<StringRef> &CmdArgs,
                                 const opt::ArgList &Args,
                                 const Triple &Triple) {
  VersionTuple Version = Triple.getOSVersion();
  if (Version.empty())
    return;

  // Metal is available on all Apple platforms. Map the triple's OS to the
  // appropriate deployment target flag for the Apple toolchain.
  StringRef Flag;
  if (Triple.isMacOSX())
    Flag = "-mmacosx-version-min=";
  else if (Triple.isiOS())
    Flag = Triple.isTvOS() ? "-mtvos-version-min=" : "-mios-version-min=";
  else if (Triple.isWatchOS())
    Flag = "-mwatchos-version-min=";
  else if (Triple.isXROS())
    Flag = "-mxros-version-min=";
  else
    // Unknown Apple platform; skip deployment target to avoid breaking the
    // compilation. The downstream tools will use their defaults.
    return;

  CmdArgs.push_back(Args.MakeArgString(Flag + Version.getAsString()));
}

void buildXCRunPrefix(const std::string &XCRunPath, const opt::ArgList &Args,
                      StringRef ToolName, SmallVectorImpl<StringRef> &Storage) {
  Storage.push_back(XCRunPath);
  if (std::optional<std::string> SDKName = getMetalSDKName(Args)) {
    Storage.push_back("--sdk");
    Storage.push_back(*SDKName);
  }
  Storage.push_back(ToolName);
}

Error runMetalTool(
    const opt::ArgList &Args, opt::OptSpecifier OverrideOpt, StringRef ToolName,
    ArrayRef<StringRef> ToolArgs, ArrayRef<std::string> EnvStorage,
    function_ref<Expected<std::string>(StringRef Name,
                                       ArrayRef<StringRef> Paths)>
        FindProgram,
    function_ref<Error(StringRef ExecutablePath, ArrayRef<StringRef> Args,
                       std::optional<ArrayRef<StringRef>>)>
        ExecuteCommand) {
  if (StringRef ToolPath = Args.getLastArgValue(OverrideOpt);
      !ToolPath.empty()) {
    SmallVector<StringRef, 16> CmdArgs = {ToolPath};
    CmdArgs.append(ToolArgs.begin(), ToolArgs.end());
    return executeWithEnvironment(ToolPath, CmdArgs, EnvStorage,
                                  ExecuteCommand);
  }

  auto XCRunPathOrErr = getToolPath(Args, OPT_INVALID, "xcrun", FindProgram);
  if (!XCRunPathOrErr)
    return XCRunPathOrErr.takeError();

  SmallVector<StringRef, 16> CmdArgs;
  buildXCRunPrefix(*XCRunPathOrErr, Args, ToolName, CmdArgs);
  CmdArgs.append(ToolArgs.begin(), ToolArgs.end());
  return executeWithEnvironment(*XCRunPathOrErr, CmdArgs, EnvStorage,
                                ExecuteCommand);
}

// ---------------------------------------------------------------------------
// SPIR-V binary helpers
// ---------------------------------------------------------------------------

// LLVM does not currently expose a public SPIR-V binary-format enum surface
// that clang-linker-wrapper can depend on cleanly. Keep this local subset
// intentionally small and fail on unsupported constructs instead of encoding a
// broad private SPIR-V schema here.
namespace spv {
enum Op : uint16_t {
  OpUndef = 1,
  OpSource = 3,
  OpName = 5,
  OpExtInstImport = 11,
  OpExtInst = 12,
  OpMemoryModel = 14,
  OpEntryPoint = 15,
  OpExecutionMode = 16,
  OpCapability = 17,
  OpTypeVoid = 19,
  OpTypeBool = 20,
  OpTypeInt = 21,
  OpTypeFloat = 22,
  OpTypeVector = 23,
  OpTypeArray = 28,
  OpTypeRuntimeArray = 29,
  OpTypeStruct = 30,
  OpTypePointer = 32,
  OpTypeFunction = 33,
  OpConstant = 43,
  OpConstantComposite = 44,
  OpConstantNull = 46,
  OpSpecConstant = 50,
  OpSpecConstantOp = 52,
  OpFunction = 54,
  OpFunctionParameter = 55,
  OpFunctionEnd = 56,
  OpFunctionCall = 57,
  OpVariable = 59,
  OpLoad = 61,
  OpStore = 62,
  OpAccessChain = 65,
  OpInBoundsAccessChain = 113,
  OpDecorate = 71,
  OpMemberDecorate = 72,
  OpCompositeExtract = 77,
  OpCompositeInsert = 78,
  OpCopyObject = 79,
  OpSelect = 169,
  OpPhi = 245,
  OpBranch = 249,
  OpBranchConditional = 250,
  OpLabel = 248,
  OpReturn = 253,
  OpReturnValue = 254,
  OpLifetimeStart = 256,
  OpLifetimeStop = 257,
  OpPtrCastToGeneric = 121,
  OpGenericCastToPtr = 122,
  OpConvertPtrToU = 117,
  OpConvertUToPtr = 120,
  OpBitcast = 124,
};

// Capabilities.
constexpr uint32_t CapShader = 1;
constexpr uint32_t CapAddresses = 4;
constexpr uint32_t CapLinkage = 5;
constexpr uint32_t CapKernel = 6;
constexpr uint32_t CapGenericPointer = 38;

// Addressing / memory models.
constexpr uint32_t AddrLogical = 0;
constexpr uint32_t MemGLSL450 = 1;
// Execution models.
constexpr uint32_t ExecGLCompute = 5;

// Execution modes.
constexpr uint32_t EMLocalSize = 17;
constexpr uint32_t EMContractionOff = 31;

// Storage classes.
constexpr uint32_t SCCrossWorkgroup = 5;
constexpr uint32_t SCPrivate = 6;
constexpr uint32_t SCFunction = 7;
constexpr uint32_t SCGeneric = 8;
constexpr uint32_t SCStorageBuffer = 12;

// Decorations.
constexpr uint32_t DecBlock = 2;
constexpr uint32_t DecBinding = 33;
constexpr uint32_t DecDescriptorSet = 34;
constexpr uint32_t DecOffset = 35;
constexpr uint32_t DecFuncParamAttr = 38;
constexpr uint32_t DecMaxByteOffset = 45;
constexpr uint32_t DecLinkageAttributes = 41;
constexpr uint32_t DecAlignment = 44;
constexpr uint32_t DecConstant = 22;

namespace OpenCL {
// Rounding / classification
constexpr uint32_t ceil = 12;
constexpr uint32_t floor = 25;
constexpr uint32_t rint = 53;
constexpr uint32_t round = 55;
constexpr uint32_t trunc = 66;
constexpr uint32_t fract = 30;
// Absolute value
constexpr uint32_t fabs = 23;
// Trigonometric
constexpr uint32_t acos = 0;
constexpr uint32_t acosh = 1;
constexpr uint32_t asin = 3;
constexpr uint32_t asinh = 4;
constexpr uint32_t atan = 6;
constexpr uint32_t atan2 = 7;
constexpr uint32_t atanh = 8;
constexpr uint32_t cos = 14;
constexpr uint32_t cosh = 15;
constexpr uint32_t sin = 57;
constexpr uint32_t sinh = 59;
constexpr uint32_t tan = 62;
constexpr uint32_t tanh = 63;
// Exponential / logarithm
constexpr uint32_t exp = 19;
constexpr uint32_t exp2 = 20;
constexpr uint32_t log = 37;
constexpr uint32_t log2 = 38;
constexpr uint32_t pow = 48;
constexpr uint32_t powr = 50;
// Roots
constexpr uint32_t sqrt = 61;
constexpr uint32_t rsqrt = 56;
// FMA / ldexp
constexpr uint32_t fma = 26;
constexpr uint32_t ldexp = 34;
// min / max / clamp (float, signed, unsigned)
constexpr uint32_t fmax = 27;
constexpr uint32_t fmin = 28;
constexpr uint32_t fclamp = 95;
constexpr uint32_t s_clamp = 149;
constexpr uint32_t u_clamp = 150;
constexpr uint32_t s_max = 156;
constexpr uint32_t u_max = 157;
constexpr uint32_t s_min = 158;
constexpr uint32_t u_min = 159;
// Half-precision approximations → same GLSL ops
constexpr uint32_t half_cos = 67;
constexpr uint32_t half_exp = 69;
constexpr uint32_t half_exp2 = 70;
constexpr uint32_t half_log = 72;
constexpr uint32_t half_log2 = 73;
constexpr uint32_t half_rsqrt = 77;
constexpr uint32_t half_sin = 78;
constexpr uint32_t half_sqrt = 79;
constexpr uint32_t half_tan = 80;
// Native approximations → same GLSL ops
constexpr uint32_t native_cos = 81;
constexpr uint32_t native_exp = 83;
constexpr uint32_t native_exp2 = 84;
constexpr uint32_t native_log = 86;
constexpr uint32_t native_log2 = 87;
constexpr uint32_t native_rsqrt = 91;
constexpr uint32_t native_sin = 92;
constexpr uint32_t native_sqrt = 93;
constexpr uint32_t native_tan = 94;
// Pointer-output math (lowered via GLSL struct-result equivalents in pass 2)
constexpr uint32_t frexp = 31; // (x, int* exp) → fraction; cf. GLSL FrexpStruct
constexpr uint32_t modf = 45;  // (x, T* iptr)  → fraction; cf. GLSL ModfStruct
constexpr uint32_t sincos =
    58; // (x, T* cosval) → sin; lowered as Sin + Cos + Store
} // namespace OpenCL

namespace GLSL {
// Rounding
constexpr uint32_t Round = 1;
constexpr uint32_t RoundEven = 2;
constexpr uint32_t Trunc = 3;
constexpr uint32_t FAbs = 4;
constexpr uint32_t Floor = 8;
constexpr uint32_t Ceil = 9;
constexpr uint32_t Fract = 10;
// Trigonometric
constexpr uint32_t Sin = 13;
constexpr uint32_t Cos = 14;
constexpr uint32_t Tan = 15;
constexpr uint32_t Asin = 16;
constexpr uint32_t Acos = 17;
constexpr uint32_t Atan = 18;
constexpr uint32_t Sinh = 19;
constexpr uint32_t Cosh = 20;
constexpr uint32_t Tanh = 21;
constexpr uint32_t Asinh = 22;
constexpr uint32_t Acosh = 23;
constexpr uint32_t Atanh = 24;
constexpr uint32_t Atan2 = 25;
// Exponential / logarithm
constexpr uint32_t Pow = 26;
constexpr uint32_t Exp = 27;
constexpr uint32_t Log = 28;
constexpr uint32_t Exp2 = 29;
constexpr uint32_t Log2 = 30;
constexpr uint32_t Sqrt = 31;
constexpr uint32_t InverseSqrt = 32;
// min / max / clamp (signed, unsigned, NaN-propagating float)
constexpr uint32_t UMin = 38;
constexpr uint32_t SMin = 39;
constexpr uint32_t UMax = 41;
constexpr uint32_t SMax = 42;
constexpr uint32_t UClamp = 44;
constexpr uint32_t SClamp = 45;
constexpr uint32_t ModfStruct = 36; // (x) → struct { T frac; T intpart; }
constexpr uint32_t Fma = 50;
constexpr uint32_t FrexpStruct = 52; // (x) → struct { T sig; int32 exp; }
constexpr uint32_t Ldexp = 53;
// NaN-propagating float variants (used for OpenCL fmin/fmax/fclamp)
constexpr uint32_t NMin = 79;
constexpr uint32_t NMax = 80;
constexpr uint32_t NClamp = 81;
} // namespace GLSL

} // namespace spv

/// Encode one SPIR-V instruction: (WordCount << 16) | Opcode, followed by Ops.
static void emitInst(SmallVectorImpl<uint32_t> &Out, uint16_t Opcode,
                     ArrayRef<uint32_t> Ops) {
  uint32_t WordCount = static_cast<uint32_t>(1 + Ops.size());
  Out.push_back((WordCount << 16) | Opcode);
  Out.append(Ops.begin(), Ops.end());
}

/// Encode a null-terminated string as SPIR-V literal words.
static void appendStringWords(SmallVectorImpl<uint32_t> &Out, StringRef S) {
  size_t TotalBytes = S.size() + 1; // include NUL
  size_t NumWords = (TotalBytes + 3) / 4;
  size_t Base = Out.size();
  Out.resize(Base + NumWords, 0);
  std::memcpy(reinterpret_cast<char *>(Out.data() + Base), S.data(), S.size());
  // NUL + padding are already zero from resize.
}

/// Read a null-terminated string starting at Words[Pos], return it and
/// advance Pos past the string words.
static StringRef readStringWords(const uint32_t *Words, unsigned &Pos,
                                 unsigned Limit) {
  const char *Start = reinterpret_cast<const char *>(Words + Pos);
  size_t MaxBytes = static_cast<size_t>(Limit - Pos) * 4;
  size_t Len = ::strnlen(Start, MaxBytes);
  size_t NumWords = (Len + 4) / 4; // includes NUL byte rounded up
  Pos += NumWords;
  return StringRef(Start, Len);
}

} // namespace

// ---------------------------------------------------------------------------
// transformOpenCLToVulkanSPIRV — rewrite OpenCL SPIR-V to Vulkan SPIR-V so
// spirv-cross can consume it.
//
// Architecture: two-pass.
//   Pass 1 — scan: collect entry points, function signatures, pointer types,
//            and build a remap table for eliminated pointer casts.
//   Pass 2 — emit: write the transformed binary, inserting StorageBuffer
//            descriptor variables and rewriting capabilities, memory model,
//            entry-point metadata, and storage classes.
//
// ID remapping is instruction-format-aware: only operand positions that hold
// SPIR-V <id> references are rewritten.  Literal constants, memory-access
// flags, and result-id definition sites are never touched.
// ---------------------------------------------------------------------------

/// Resolve an ID through a remap chain (A→B→C becomes A→C).
static uint32_t resolveRemap(const DenseMap<uint32_t, uint32_t> &Remap,
                             uint32_t ID) {
  for (unsigned Depth = 0; Depth < 64; ++Depth) {
    auto It = Remap.find(ID);
    if (It == Remap.end())
      return ID;
    ID = It->second;
  }
  return ID; // Safety: avoid infinite loops on malformed input.
}

/// Remap a single word only if it appears in the table.
static uint32_t remapWord(const DenseMap<uint32_t, uint32_t> &Remap,
                          uint32_t W) {
  auto It = Remap.find(W);
  return It != Remap.end() ? It->second : W;
}

/// Copy an instruction to Out, applying ID remapping to specific operand
/// positions only.  `IDPositions` lists the word indices (0-based within the
/// instruction) that are <id> references and should be remapped.
static void emitRemapped(SmallVectorImpl<uint32_t> &Out,
                         ArrayRef<uint32_t> Inst,
                         const DenseMap<uint32_t, uint32_t> &Remap,
                         ArrayRef<unsigned> IDPositions) {
  size_t Base = Out.size();
  Out.append(Inst.begin(), Inst.end());
  for (unsigned Idx : IDPositions) {
    if (Idx < Inst.size())
      Out[Base + Idx] = remapWord(Remap, Inst[Idx]);
  }
}

// Map OpenCL.std extended instruction opcodes to GLSL.std.450 equivalents.
// Only opcodes with structurally identical signatures are handled here.
// Opcodes writing results through pointer arguments (frexp, modf, sincos) are
// handled separately in the OpExtInst case of pass 2 via struct-result
// lowering. lgamma_r has no GLSL equivalent and remains unsupported.
static std::optional<uint32_t> mapOpenCLExtInstToGLSL(uint32_t Op) {
  namespace OCL = spv::OpenCL;
  namespace G = spv::GLSL;
  switch (Op) {
  // Rounding / classification
  case OCL::ceil:
    return G::Ceil;
  case OCL::floor:
    return G::Floor;
  case OCL::fract:
    return G::Fract;
  case OCL::rint:
    return G::RoundEven;
  case OCL::round:
    return G::Round;
  case OCL::trunc:
    return G::Trunc;
  // Absolute value
  case OCL::fabs:
    return G::FAbs;
  // Trigonometric
  case OCL::acos:
    return G::Acos;
  case OCL::acosh:
    return G::Acosh;
  case OCL::asin:
    return G::Asin;
  case OCL::asinh:
    return G::Asinh;
  case OCL::atan:
    return G::Atan;
  case OCL::atan2:
    return G::Atan2;
  case OCL::atanh:
    return G::Atanh;
  case OCL::cos:
    return G::Cos;
  case OCL::cosh:
    return G::Cosh;
  case OCL::sin:
    return G::Sin;
  case OCL::sinh:
    return G::Sinh;
  case OCL::tan:
    return G::Tan;
  case OCL::tanh:
    return G::Tanh;
  // Exponential / logarithm
  case OCL::exp:
    return G::Exp;
  case OCL::exp2:
    return G::Exp2;
  case OCL::log:
    return G::Log;
  case OCL::log2:
    return G::Log2;
  case OCL::pow:
    return G::Pow;
  case OCL::powr:
    return G::Pow; // powr requires positive base; Pow is correct for GPU
  // Roots
  case OCL::sqrt:
    return G::Sqrt;
  case OCL::rsqrt:
    return G::InverseSqrt;
  // FMA / ldexp
  case OCL::fma:
    return G::Fma;
  case OCL::ldexp:
    return G::Ldexp;
  // min / max / clamp — float (NaN-propagating), signed, unsigned
  case OCL::fmin:
    return G::NMin;
  case OCL::fmax:
    return G::NMax;
  case OCL::fclamp:
    return G::NClamp;
  case OCL::s_min:
    return G::SMin;
  case OCL::u_min:
    return G::UMin;
  case OCL::s_max:
    return G::SMax;
  case OCL::u_max:
    return G::UMax;
  case OCL::s_clamp:
    return G::SClamp;
  case OCL::u_clamp:
    return G::UClamp;
  // Half-precision approximations
  case OCL::half_cos:
    return G::Cos;
  case OCL::half_exp:
    return G::Exp;
  case OCL::half_exp2:
    return G::Exp2;
  case OCL::half_log:
    return G::Log;
  case OCL::half_log2:
    return G::Log2;
  case OCL::half_rsqrt:
    return G::InverseSqrt;
  case OCL::half_sin:
    return G::Sin;
  case OCL::half_sqrt:
    return G::Sqrt;
  case OCL::half_tan:
    return G::Tan;
  // Native approximations
  case OCL::native_cos:
    return G::Cos;
  case OCL::native_exp:
    return G::Exp;
  case OCL::native_exp2:
    return G::Exp2;
  case OCL::native_log:
    return G::Log;
  case OCL::native_log2:
    return G::Log2;
  case OCL::native_rsqrt:
    return G::InverseSqrt;
  case OCL::native_sin:
    return G::Sin;
  case OCL::native_sqrt:
    return G::Sqrt;
  case OCL::native_tan:
    return G::Tan;
  default:
    return std::nullopt;
  }
}

static Expected<StringRef> transformOpenCLToVulkanSPIRV(
    StringRef InputFile,
    function_ref<Expected<StringRef>(const Twine &Prefix, StringRef Extension)>
        CreateOutputFile) {

  auto BufOrErr = MemoryBuffer::getFile(InputFile);
  if (!BufOrErr)
    return createStringError("cannot read SPIR-V file '" + InputFile +
                             "': " + BufOrErr.getError().message());

  StringRef Blob = (*BufOrErr)->getBuffer();
  if (Blob.size() < 20 || Blob.size() % 4 != 0)
    return createStringError("invalid SPIR-V binary (too small or misaligned)");

  ArrayRef<uint32_t> Words(reinterpret_cast<const uint32_t *>(Blob.data()),
                           Blob.size() / 4);

  if (Words[0] != 0x07230203)
    return createStringError("not a SPIR-V binary (bad magic)");

  // =======================================================================
  // Pass 1 — scan
  // =======================================================================

  bool HasKernelCap = false;
  bool HasShaderCap = false;

  struct EntryPointInfo {
    uint32_t FuncID = 0;
    std::string Name;
  };
  SmallVector<EntryPointInfo, 4> EntryPoints;

  DenseMap<uint32_t, SmallVector<uint32_t, 4>> FuncParams;
  DenseMap<uint32_t, uint32_t> FuncTypeMap;
  DenseMap<uint32_t, SmallVector<uint32_t, 4>> TypeFuncOps;
  DenseMap<uint32_t, std::pair<uint32_t, uint32_t>> PtrTypeInfo;
  DenseSet<uint32_t> OpenCLStdImportIDs;
  DenseSet<uint32_t> EntryPointIDs;
  DenseSet<uint32_t> CrossWorkgroupGlobals;
  DenseSet<uint32_t> KernelParamPointerTypeIDs;
  DenseSet<uint32_t> FunctionTypeIDs; // IDs defined by OpTypeFunction

  // Pointer-output OpenCL.std instructions that need struct-result lowering.
  // Maps OpExtInst ResultID → {OCLOpcode, ResultTypeID}.
  struct PtrOutputInfo {
    uint32_t OCLOpcode;
    uint32_t ResultTypeID;
    uint32_t TempID = 0;    // sincos: ID for cos_tmp; frexp/modf: struct result
    uint32_t ExtractID = 0; // frexp/modf: extracted pointer-output component
    uint32_t StructTypeID = 0; // frexp/modf: OpTypeStruct {T, U} type
  };
  DenseMap<uint32_t, PtrOutputInfo> PtrOutputInsts;

  uint32_t CurrentFunc = 0;
  bool OrigHadTypeVoid = false;
  bool OrigHadTypeIntU32 = false;
  bool OrigHadConstZero = false;
  uint32_t TypeVoidID = 0;
  uint32_t TypeIntU32ID = 0;
  uint32_t ConstZeroID = 0;

  unsigned Pos = 5;
  while (Pos < Words.size()) {
    uint32_t W0 = Words[Pos];
    uint16_t Opcode = W0 & 0xFFFF;
    uint16_t WC = W0 >> 16;
    if (WC == 0 || Pos + WC > Words.size())
      break;

    switch (Opcode) {
    case spv::OpCapability:
      if (WC >= 2) {
        if (Words[Pos + 1] == spv::CapKernel)
          HasKernelCap = true;
        if (Words[Pos + 1] == spv::CapShader)
          HasShaderCap = true;
      }
      break;

    case spv::OpEntryPoint:
      if (WC >= 4) {
        EntryPointInfo EP;
        EP.FuncID = Words[Pos + 2];
        unsigned NP = 3;
        EP.Name = readStringWords(Words.data() + Pos, NP, WC).str();
        EntryPoints.push_back(EP);
        EntryPointIDs.insert(EP.FuncID);
      }
      break;

    case spv::OpExtInstImport:
      if (WC >= 3) {
        unsigned NP = 2;
        if (readStringWords(Words.data() + Pos, NP, WC) == "OpenCL.std")
          OpenCLStdImportIDs.insert(Words[Pos + 1]);
      }
      break;

    case spv::OpTypeVoid:
      if (WC >= 2) {
        TypeVoidID = Words[Pos + 1];
        OrigHadTypeVoid = true;
      }
      break;

    case spv::OpTypeInt:
      if (WC >= 4 && Words[Pos + 2] == 32 && Words[Pos + 3] == 0) {
        TypeIntU32ID = Words[Pos + 1];
        OrigHadTypeIntU32 = true;
      }
      break;

    case spv::OpTypePointer:
      if (WC == 4)
        PtrTypeInfo[Words[Pos + 1]] = {Words[Pos + 2], Words[Pos + 3]};
      break;

    case spv::OpTypeFunction:
      if (WC >= 3) {
        uint32_t TID = Words[Pos + 1];
        FunctionTypeIDs.insert(TID);
        SmallVector<uint32_t, 4> Ops;
        for (unsigned I = 2; I < WC; ++I)
          Ops.push_back(Words[Pos + I]);
        TypeFuncOps[TID] = std::move(Ops);
      }
      break;

    case spv::OpConstant:
      if (WC >= 4 && TypeIntU32ID && Words[Pos + 1] == TypeIntU32ID &&
          Words[Pos + 3] == 0) {
        ConstZeroID = Words[Pos + 2];
        OrigHadConstZero = true;
      }
      break;

    case spv::OpFunction:
      if (WC >= 5) {
        CurrentFunc = Words[Pos + 2];
        FuncTypeMap[CurrentFunc] = Words[Pos + 4];
      }
      break;

    case spv::OpFunctionParameter:
      if (WC >= 3 && CurrentFunc)
        FuncParams[CurrentFunc].push_back(Words[Pos + 2]);
      break;

    case spv::OpFunctionEnd:
      CurrentFunc = 0;
      break;

    case spv::OpVariable:
      if (WC >= 4 && Words[Pos + 3] == spv::SCCrossWorkgroup)
        CrossWorkgroupGlobals.insert(Words[Pos + 2]);
      break;

    case spv::OpExtInst:
      // Record pointer-output instructions for struct-result lowering in
      // pass 2.
      if (WC >= 6 && OpenCLStdImportIDs.count(Words[Pos + 3])) {
        uint32_t OCLOp = Words[Pos + 4];
        if (OCLOp == spv::OpenCL::frexp || OCLOp == spv::OpenCL::modf ||
            OCLOp == spv::OpenCL::sincos) {
          PtrOutputInsts[Words[Pos + 2]] = {OCLOp, Words[Pos + 1]};
        }
      }
      break;

    default:
      break;
    }
    Pos += WC;
  }

  // Passthrough: already Vulkan SPIR-V.
  if (HasShaderCap && !HasKernelCap)
    return InputFile;
  if (!HasKernelCap)
    return createStringError(
        "SPIR-V binary has neither Kernel nor Shader capability");
  if (EntryPoints.empty())
    return createStringError("no entry points found in SPIR-V binary");

  // =======================================================================
  // Allocate new IDs and build transforms
  // =======================================================================

  uint32_t NextID = Words[3];
  auto AllocID = [&]() { return NextID++; };

  if (!TypeVoidID)
    TypeVoidID = AllocID();
  bool NeedSynthTypeVoid = !OrigHadTypeVoid;
  if (!ConstZeroID) {
    if (!TypeIntU32ID)
      TypeIntU32ID = AllocID();
    ConstZeroID = AllocID();
  }
  bool NeedSynthTypeIntU32 = !OrigHadTypeIntU32;
  bool NeedSynthConstZero = !OrigHadConstZero;

  struct BufferBinding {
    uint32_t ParamID;
    uint32_t PointeeTypeID;
    uint32_t Binding;
    uint32_t WrapStructID;
    uint32_t PtrSBWrapID;
    uint32_t PtrSBInnerID;
    uint32_t VarID;
    uint32_t AccessChainID;
  };

  struct EPTransform {
    uint32_t FuncID;
    SmallVector<BufferBinding, 4> Bindings;
    uint32_t NewFuncTypeID;
  };

  SmallVector<EPTransform, 4> EPTransforms;
  uint32_t GlobalBindingIdx = 0;

  for (auto &EP : EntryPoints) {
    EPTransform EPT;
    EPT.FuncID = EP.FuncID;

    auto ParamIt = FuncParams.find(EP.FuncID);
    if (ParamIt != FuncParams.end()) {
      auto FTIt = FuncTypeMap.find(EP.FuncID);
      uint32_t FTID = FTIt != FuncTypeMap.end() ? FTIt->second : 0;
      auto TOIt = FTID ? TypeFuncOps.find(FTID) : TypeFuncOps.end();

      for (unsigned I = 0; I < ParamIt->second.size(); ++I) {
        uint32_t ParamID = ParamIt->second[I];
        uint32_t ParamTypeID = 0;
        if (TOIt != TypeFuncOps.end() && I + 1 < TOIt->second.size())
          ParamTypeID = TOIt->second[I + 1];

        auto PtrIt = PtrTypeInfo.find(ParamTypeID);
        if (PtrIt != PtrTypeInfo.end() &&
            PtrIt->second.first == spv::SCCrossWorkgroup) {
          KernelParamPointerTypeIDs.insert(ParamTypeID);
          BufferBinding BB;
          BB.ParamID = ParamID;
          BB.PointeeTypeID = PtrIt->second.second;
          BB.Binding = GlobalBindingIdx++;
          BB.WrapStructID = AllocID();
          BB.PtrSBWrapID = AllocID();
          BB.PtrSBInnerID = AllocID();
          BB.VarID = AllocID();
          BB.AccessChainID = AllocID();
          EPT.Bindings.push_back(BB);
        }
      }
    }
    EPT.NewFuncTypeID = AllocID();
    EPTransforms.push_back(std::move(EPT));
  }

  // Allocate IDs for pointer-output instruction lowering.
  // (frexp/modf need a struct type, struct result, and extracted component;
  //  sincos needs only one extra ID for the cos temporary.)
  DenseMap<std::pair<uint32_t, uint32_t>, uint32_t> StructTypeDedup;
  for (auto &[ResultID, Info] : PtrOutputInsts) {
    Info.TempID = AllocID();
    if (Info.OCLOpcode != spv::OpenCL::sincos) {
      Info.ExtractID = AllocID();
      uint32_t SecondType = (Info.OCLOpcode == spv::OpenCL::frexp)
                                ? TypeIntU32ID
                                : Info.ResultTypeID;
      auto [It, Inserted] =
          StructTypeDedup.try_emplace({Info.ResultTypeID, SecondType}, 0u);
      if (Inserted)
        It->second = AllocID();
      Info.StructTypeID = It->second;
    }
  }

  // Build remap: old param ID → access-chain ID.
  DenseMap<uint32_t, uint32_t> IDRemap;
  for (auto &EPT : EPTransforms)
    for (auto &BB : EPT.Bindings)
      IDRemap[BB.ParamID] = BB.AccessChainID;

  DenseMap<uint32_t, unsigned> EPTIndexMap;
  for (unsigned I = 0; I < EPTransforms.size(); ++I)
    EPTIndexMap[EPTransforms[I].FuncID] = I;

  DenseSet<uint32_t> SkippedTypeIDs;
  for (const auto &[TypeID, StorageAndPointee] : PtrTypeInfo)
    if (StorageAndPointee.first == spv::SCFunction &&
        FunctionTypeIDs.count(StorageAndPointee.second))
      SkippedTypeIDs.insert(TypeID);

  // Set of parameter IDs we're skipping (for quick lookup in
  // OpFunctionParameter).
  DenseSet<uint32_t> SkippedParamIDs;
  for (auto &[K, V] : IDRemap)
    SkippedParamIDs.insert(K);

  // =======================================================================
  // Pass 2 — emit transformed binary
  // =======================================================================

  SmallVector<uint32_t, 4096> Out;
  Out.append(Words.begin(), Words.begin() + 5); // header

  bool EmittedNewAnnotations = false;
  bool EmittedNewTypes = false;
  uint32_t ActiveEPFunc = 0;
  bool EmittedAccessChains = false;

  auto emitAnnotations = [&]() {
    if (EmittedNewAnnotations)
      return;
    EmittedNewAnnotations = true;
    for (auto &EPT : EPTransforms) {
      for (auto &BB : EPT.Bindings) {
        emitInst(Out, spv::OpDecorate, {BB.WrapStructID, spv::DecBlock});
        emitInst(Out, spv::OpDecorate, {BB.VarID, spv::DecDescriptorSet, 0});
        emitInst(Out, spv::OpDecorate, {BB.VarID, spv::DecBinding, BB.Binding});
        emitInst(Out, spv::OpMemberDecorate,
                 {BB.WrapStructID, 0, spv::DecOffset, 0});
      }
    }
  };

  auto emitNewTypes = [&]() {
    if (EmittedNewTypes)
      return;
    EmittedNewTypes = true;

    if (NeedSynthTypeVoid)
      emitInst(Out, spv::OpTypeVoid, {TypeVoidID});
    if (NeedSynthTypeIntU32)
      emitInst(Out, spv::OpTypeInt, {TypeIntU32ID, 32, 0});
    if (NeedSynthConstZero)
      emitInst(Out, spv::OpConstant, {TypeIntU32ID, ConstZeroID, 0});

    for (auto &EPT : EPTransforms)
      emitInst(Out, spv::OpTypeFunction, {EPT.NewFuncTypeID, TypeVoidID});

    // Struct types for frexp/modf pointer-output lowering.
    for (auto &[TypePair, StructID] : StructTypeDedup)
      emitInst(Out, spv::OpTypeStruct,
               {StructID, TypePair.first, TypePair.second});

    for (auto &EPT : EPTransforms) {
      for (auto &BB : EPT.Bindings) {
        emitInst(Out, spv::OpTypeStruct, {BB.WrapStructID, BB.PointeeTypeID});
        emitInst(Out, spv::OpTypePointer,
                 {BB.PtrSBWrapID, spv::SCStorageBuffer, BB.WrapStructID});
        emitInst(Out, spv::OpTypePointer,
                 {BB.PtrSBInnerID, spv::SCStorageBuffer, BB.PointeeTypeID});
        emitInst(Out, spv::OpVariable,
                 {BB.PtrSBWrapID, BB.VarID, spv::SCStorageBuffer});
      }
    }
  };

  auto isTypeConstGlobalOp = [](uint16_t Op) -> bool {
    if (Op >= 19 && Op <= 33)
      return true;
    return Op == spv::OpConstant || Op == spv::OpConstantComposite ||
           Op == spv::OpConstantNull || Op == spv::OpSpecConstant ||
           Op == spv::OpSpecConstantOp || Op == spv::OpVariable ||
           Op == spv::OpUndef || Op == spv::OpTypeArray ||
           Op == spv::OpTypeRuntimeArray;
  };

  Pos = 5;
  while (Pos < Words.size()) {
    uint32_t W0 = Words[Pos];
    uint16_t Opcode = W0 & 0xFFFF;
    uint16_t WC = W0 >> 16;
    if (WC == 0 || Pos + WC > Words.size())
      break;

    ArrayRef<uint32_t> Inst(Words.data() + Pos, WC);

    // --- Insertion-point detection ---
    // Annotations must stay ahead of the type section, but the synthesized
    // wrapper types must come after the original pointee types they reference.
    if (!EmittedNewAnnotations && isTypeConstGlobalOp(Opcode))
      emitAnnotations();
    if (Opcode == spv::OpFunction) {
      emitAnnotations();
      emitNewTypes();
    }

    // --- Per-instruction transformation ---
    switch (Opcode) {

    // ----- Capabilities -----
    case spv::OpCapability: {
      uint32_t Cap = Inst[1];
      if (Cap == spv::CapKernel)
        emitInst(Out, spv::OpCapability, {spv::CapShader});
      else if (Cap == spv::CapAddresses || Cap == spv::CapGenericPointer ||
               Cap == spv::CapLinkage)
        ; // skip
      else
        Out.append(Inst.begin(), Inst.end());
      break;
    }

    // ----- ExtInstImport -----
    case spv::OpExtInstImport: {
      uint32_t RID = Inst[1];
      unsigned NP = 2;
      StringRef Name = readStringWords(Inst.data(), NP, WC);
      if (Name == "OpenCL.std") {
        SmallVector<uint32_t, 8> Ops;
        Ops.push_back(RID);
        appendStringWords(Ops, "GLSL.std.450");
        emitInst(Out, spv::OpExtInstImport, Ops);
      } else {
        Out.append(Inst.begin(), Inst.end());
      }
      break;
    }

    // ----- Memory model -----
    case spv::OpMemoryModel:
      emitInst(Out, spv::OpMemoryModel, {spv::AddrLogical, spv::MemGLSL450});
      break;

    // ----- Entry point -----
    case spv::OpEntryPoint: {
      uint32_t FID = Inst[2];
      unsigned NP = 3;
      StringRef Name = readStringWords(Inst.data(), NP, WC);
      SmallVector<uint32_t, 16> Ops;
      Ops.push_back(spv::ExecGLCompute);
      Ops.push_back(FID);
      appendStringWords(Ops, Name);
      auto It = EPTIndexMap.find(FID);
      if (It != EPTIndexMap.end())
        for (auto &BB : EPTransforms[It->second].Bindings)
          Ops.push_back(BB.VarID);
      emitInst(Out, spv::OpEntryPoint, Ops);
      break;
    }

    // ----- Execution mode -----
    case spv::OpExecutionMode: {
      uint32_t FID = Inst[1];
      uint32_t Mode = Inst[2];
      if (Mode == spv::EMContractionOff)
        emitInst(Out, spv::OpExecutionMode, {FID, spv::EMLocalSize, 1, 1, 1});
      else
        Out.append(Inst.begin(), Inst.end());
      break;
    }

    // ----- Skip OpSource -----
    case spv::OpSource:
      break;

    // ----- Lifetime markers are Kernel-only -----
    case spv::OpLifetimeStart:
    case spv::OpLifetimeStop:
      break;

    // ----- Decorations -----
    case spv::OpDecorate: {
      if (WC < 3) {
        Out.append(Inst.begin(), Inst.end());
        break;
      }
      uint32_t Target = Inst[1];
      uint32_t Dec = Inst[2];
      if (Dec == spv::DecFuncParamAttr || Dec == spv::DecLinkageAttributes ||
          Dec == spv::DecAlignment || Dec == spv::DecMaxByteOffset)
        break; // skip
      if (Dec == spv::DecConstant && CrossWorkgroupGlobals.count(Target))
        break; // skip
      Out.append(Inst.begin(), Inst.end());
      break;
    }

    // ----- Type: pointer -----
    case spv::OpTypePointer: {
      if (WC < 4) {
        Out.append(Inst.begin(), Inst.end());
        break;
      }
      uint32_t RID = Inst[1];
      uint32_t SC = Inst[2];
      uint32_t Pointee = Inst[3];

      if (SkippedTypeIDs.count(RID))
        break;

      if (SC == spv::SCGeneric)
        emitInst(Out, spv::OpTypePointer, {RID, spv::SCFunction, Pointee});
      else if (SC == spv::SCCrossWorkgroup)
        emitInst(Out, spv::OpTypePointer,
                 {RID,
                  KernelParamPointerTypeIDs.count(RID) ? spv::SCStorageBuffer
                                                       : spv::SCPrivate,
                  Pointee});
      else
        Out.append(Inst.begin(), Inst.end());
      break;
    }

    case spv::OpConstant:
      Out.append(Inst.begin(), Inst.end());
      break;

    case spv::OpConstantNull:
      if (WC >= 3 && SkippedTypeIDs.count(Inst[1]))
        break;
      Out.append(Inst.begin(), Inst.end());
      break;

    // ----- Type: function (always emit) -----
    case spv::OpTypeFunction:
      Out.append(Inst.begin(), Inst.end());
      break;

    // ----- Function header -----
    case spv::OpFunction: {
      if (WC < 5) {
        Out.append(Inst.begin(), Inst.end());
        break;
      }
      uint32_t FID = Inst[2];
      auto It = EPTIndexMap.find(FID);
      if (It != EPTIndexMap.end()) {
        ActiveEPFunc = FID;
        EmittedAccessChains = false;
        emitInst(
            Out, spv::OpFunction,
            {TypeVoidID, FID, Inst[3], EPTransforms[It->second].NewFuncTypeID});
      } else {
        ActiveEPFunc = 0;
        Out.append(Inst.begin(), Inst.end());
      }
      break;
    }

    // ----- Function parameter -----
    case spv::OpFunctionParameter:
      if (WC >= 3 && SkippedParamIDs.count(Inst[2]))
        break; // replaced by StorageBuffer access chain
      Out.append(Inst.begin(), Inst.end());
      break;

    // ----- Function end -----
    case spv::OpFunctionEnd:
      ActiveEPFunc = 0;
      Out.append(Inst.begin(), Inst.end());
      break;

    // ----- Label (insert access chains after first label in EP) -----
    case spv::OpLabel:
      Out.append(Inst.begin(), Inst.end());
      if (ActiveEPFunc && !EmittedAccessChains) {
        EmittedAccessChains = true;
        auto It = EPTIndexMap.find(ActiveEPFunc);
        if (It != EPTIndexMap.end())
          for (auto &BB : EPTransforms[It->second].Bindings)
            emitInst(
                Out, spv::OpAccessChain,
                {BB.PtrSBInnerID, BB.AccessChainID, BB.VarID, ConstZeroID});
      }
      break;

    // ----- Pointer cast elimination (these become identity) -----
    case spv::OpPtrCastToGeneric:
    case spv::OpGenericCastToPtr:
    case spv::OpConvertPtrToU:
    case spv::OpConvertUToPtr: {
      if (WC < 4)
        break;
      uint32_t RID = Inst[2];
      uint32_t SrcID = resolveRemap(IDRemap, Inst[3]);
      IDRemap[RID] = SrcID;
      break;
    }

    // ----- Module-scope variables -----
    case spv::OpVariable:
      if (WC >= 4 && Inst[3] == spv::SCCrossWorkgroup) {
        SmallVector<uint32_t, 4> Ops = {Inst[1], Inst[2], spv::SCPrivate};
        if (WC >= 5)
          Ops.push_back(Inst[4]);
        emitInst(Out, spv::OpVariable, Ops);
        break;
      }
      Out.append(Inst.begin(), Inst.end());
      break;

    // =====================================================================
    // Instructions that reference value IDs in known operand positions.
    // We remap ONLY those positions — never literals, never definitions.
    // =====================================================================

    // OpStore: [opcode|wc, Pointer, Object, optional MemAccess...]
    case spv::OpStore:
      emitRemapped(Out, Inst, IDRemap, {1, 2});
      break;

    // OpLoad: [opcode|wc, ResultType, ResultID, Pointer, optional MemAccess...]
    case spv::OpLoad:
      emitRemapped(Out, Inst, IDRemap, {3});
      break;

    // OpAccessChain / OpInBoundsAccessChain:
    //   [opcode|wc, ResultType, ResultID, Base, Index0, Index1, ...]
    case spv::OpAccessChain:
    case spv::OpInBoundsAccessChain: {
      SmallVector<unsigned, 8> Positions;
      for (unsigned I = 3; I < WC; ++I)
        Positions.push_back(I);
      emitRemapped(Out, Inst, IDRemap, Positions);
      break;
    }

    // OpBitcast: [opcode|wc, ResultType, ResultID, Operand]
    case spv::OpBitcast:
      emitRemapped(Out, Inst, IDRemap, {3});
      break;

    // OpFunctionCall: [opcode|wc, ResultType, ResultID, Function, Arg0, ...]
    case spv::OpFunctionCall: {
      SmallVector<unsigned, 8> Positions;
      for (unsigned I = 4; I < WC; ++I)
        Positions.push_back(I);
      emitRemapped(Out, Inst, IDRemap, Positions);
      break;
    }

    // OpPhi: [opcode|wc, ResultType, ResultID, (Value, Parent), ...]
    case spv::OpPhi: {
      SmallVector<unsigned, 8> Positions;
      for (unsigned I = 3; I < WC; I += 2) // values at 3,5,7,...
        Positions.push_back(I);
      emitRemapped(Out, Inst, IDRemap, Positions);
      break;
    }

    // OpSelect: [opcode|wc, ResultType, ResultID, Condition, Obj1, Obj2]
    case spv::OpSelect:
      emitRemapped(Out, Inst, IDRemap, {4, 5});
      break;

    // OpCompositeExtract / OpCompositeInsert — operand at word 3
    case spv::OpCompositeExtract:
    case spv::OpCompositeInsert:
      emitRemapped(Out, Inst, IDRemap, {3});
      break;

    // OpCopyObject: [opcode|wc, ResultType, ResultID, Operand]
    case spv::OpCopyObject:
      emitRemapped(Out, Inst, IDRemap, {3});
      break;

    // OpReturnValue: [opcode|wc, Value]
    case spv::OpReturnValue:
      emitRemapped(Out, Inst, IDRemap, {1});
      break;

    // Arithmetic / comparison — operands start at word 3.
    // These are all "ResultType ResultID Op1 Op2" or "... Op1".
    case 128:
    case 129:
    case 130:
    case 131:
    case 132:
    case 133: // IAdd..FDiv
    case 134:
    case 135:
    case 136:
    case 137:
    case 138:
    case 139:
    case 164:
    case 166:
    case 170:
    case 171: // comparisons
    case 180:
    case 181:
    case 182:
    case 183:
    case 184:
    case 185: // logical
    case 186:
    case 187:
    case 188:
    case 189:
    case 190:
    case 191:
    case 194:
    case 195:
    case 196:
    case 197: // bitwise
    case 199:
    case 200:
    case 201:
    case 202:
    case 203:
    case 204: { // shifts etc
      SmallVector<unsigned, 4> Positions;
      for (unsigned I = 3; I < WC; ++I)
        Positions.push_back(I);
      emitRemapped(Out, Inst, IDRemap, Positions);
      break;
    }

    // OpExtInst: [opcode|wc, ResultType, ResultID, Set, Instruction, Op0, ...]
    case spv::OpExtInst: {
      if (WC < 5) {
        Out.append(Inst.begin(), Inst.end());
        break;
      }

      if (!OpenCLStdImportIDs.count(Inst[3])) {
        SmallVector<unsigned, 8> Positions;
        for (unsigned I = 5; I < WC; ++I)
          Positions.push_back(I);
        emitRemapped(Out, Inst, IDRemap, Positions);
        break;
      }

      // Pointer-output instructions: frexp, modf, sincos.
      // These write a second result through a pointer argument and have no
      // direct GLSL equivalent. Lower them using struct-result instructions.
      if (auto It = PtrOutputInsts.find(Inst[2]); It != PtrOutputInsts.end()) {
        const PtrOutputInfo &Info = It->second;
        uint32_t SetID = Inst[3]; // reused as GLSL.std.450 import ID
        uint32_t X = remapWord(IDRemap, Inst[5]);
        uint32_t Ptr = remapWord(IDRemap, Inst[6]);
        if (Info.OCLOpcode == spv::OpenCL::sincos) {
          // %result = OpExtInst T GLSL Sin %x
          emitInst(Out, spv::OpExtInst,
                   {Inst[1], Inst[2], SetID, spv::GLSL::Sin, X});
          // %cos_tmp = OpExtInst T GLSL Cos %x
          emitInst(Out, spv::OpExtInst,
                   {Inst[1], Info.TempID, SetID, spv::GLSL::Cos, X});
          // OpStore %cos_ptr %cos_tmp
          emitInst(Out, spv::OpStore, {Ptr, Info.TempID});
        } else if (Info.OCLOpcode == spv::OpenCL::modf) {
          // %struct = OpExtInst StructType GLSL ModfStruct %x
          emitInst(Out, spv::OpExtInst,
                   {Info.StructTypeID, Info.TempID, SetID,
                    spv::GLSL::ModfStruct, X});
          // %result = OpCompositeExtract T %struct 0
          emitInst(Out, spv::OpCompositeExtract,
                   {Inst[1], Inst[2], Info.TempID, 0});
          // %intpart = OpCompositeExtract T %struct 1
          emitInst(Out, spv::OpCompositeExtract,
                   {Inst[1], Info.ExtractID, Info.TempID, 1});
          // OpStore %iptr %intpart
          emitInst(Out, spv::OpStore, {Ptr, Info.ExtractID});
        } else { // frexp
          // %struct = OpExtInst StructType GLSL FrexpStruct %x
          emitInst(Out, spv::OpExtInst,
                   {Info.StructTypeID, Info.TempID, SetID,
                    spv::GLSL::FrexpStruct, X});
          // %result = OpCompositeExtract T %struct 0
          emitInst(Out, spv::OpCompositeExtract,
                   {Inst[1], Inst[2], Info.TempID, 0});
          // %exp = OpCompositeExtract int32 %struct 1
          emitInst(Out, spv::OpCompositeExtract,
                   {TypeIntU32ID, Info.ExtractID, Info.TempID, 1});
          // OpStore %exp_ptr %exp
          emitInst(Out, spv::OpStore, {Ptr, Info.ExtractID});
        }
        break;
      }

      std::optional<uint32_t> GLSLOpcode = mapOpenCLExtInstToGLSL(Inst[4]);
      if (!GLSLOpcode)
        return createStringError(
            "unsupported OpenCL.std extended instruction opcode " +
            Twine(Inst[4]) + " in Metal SPIR-V rewrite");

      SmallVector<uint32_t, 8> Ops = {Inst[1], Inst[2], Inst[3], *GLSLOpcode};
      for (unsigned I = 5; I < WC; ++I)
        Ops.push_back(remapWord(IDRemap, Inst[I]));
      emitInst(Out, spv::OpExtInst, Ops);
      break;
    }

    // ----- Default: copy as-is (no remapping) -----
    default:
      Out.append(Inst.begin(), Inst.end());
      break;
    }

    Pos += WC;
  }

  // Ensure annotations/types were emitted even if the module had an unusual
  // layout but still parsed successfully.
  emitAnnotations();
  emitNewTypes();

  Out[3] = NextID; // Fix up bound.

  // Write output.
  auto OutFileOrErr =
      CreateOutputFile(sys::path::filename(InputFile), "vulkan.spv");
  if (!OutFileOrErr)
    return OutFileOrErr.takeError();

  std::error_code EC;
  raw_fd_ostream OS(OutFileOrErr->str(), EC, sys::fs::OF_None);
  if (EC)
    return createStringError("cannot write transformed SPIR-V to '" +
                             *OutFileOrErr + "': " + EC.message());

  OS.write(reinterpret_cast<const char *>(Out.data()),
           Out.size() * sizeof(uint32_t));
  OS.close();

  return *OutFileOrErr;
}

bool llvm::isMetalOpenMPTarget(const Triple &Triple, StringRef Arch,
                               OffloadKind Kind) {
  return offloading::isMetalOpenMPOffloadTarget(Triple, Arch, Kind);
}

Triple llvm::getMetalOpenMPToolchainTriple(const Triple &Triple) {
  return offloading::getMetalOpenMPToolchainTriple(Triple);
}

Expected<std::optional<std::string>>
llvm::createMetalDescriptor(ArrayRef<OffloadFile> Input) {
  auto ExistingDescriptorOrErr = getExistingMetalDescriptor(Input);
  if (!ExistingDescriptorOrErr)
    return ExistingDescriptorOrErr.takeError();

  auto GeneratedDescriptorOrErr = buildMetalDescriptorFromBitcode(Input);
  if (!GeneratedDescriptorOrErr)
    return GeneratedDescriptorOrErr.takeError();

  if (!*GeneratedDescriptorOrErr)
    return *ExistingDescriptorOrErr;

  if (*ExistingDescriptorOrErr &&
      *ExistingDescriptorOrErr != **GeneratedDescriptorOrErr)
    return createStringError(
        "conflicting Metal descriptor metadata found while linking "
        "Metal OpenMP device images");

  return std::move(*GeneratedDescriptorOrErr);
}

Expected<FinalizedImageFile> llvm::finalizeMetalOpenMPImage(
    StringRef InputFile, const opt::ArgList &Args, const Triple &Triple,
    function_ref<Expected<StringRef>(const Twine &Prefix, StringRef Extension)>
        CreateOutputFile,
    function_ref<Expected<std::string>(StringRef Name,
                                       ArrayRef<StringRef> Paths)>
        FindProgram,
    function_ref<Error(StringRef ExecutablePath, ArrayRef<StringRef> Args,
                       std::optional<ArrayRef<StringRef>>)>
        ExecuteCommand) {
  if (Args.hasArg(OPT_embed_bitcode))
    return createStringError(
        "Metal OpenMP offload does not support embedding LLVM bitcode");

  // Transform OpenCL SPIR-V to Vulkan SPIR-V for spirv-cross compatibility.
  auto VulkanSPVOrErr =
      transformOpenCLToVulkanSPIRV(InputFile, CreateOutputFile);
  if (!VulkanSPVOrErr)
    return VulkanSPVOrErr.takeError();
  StringRef SPVFile = *VulkanSPVOrErr;

  auto TranslatorPathOrErr = getToolPath(Args, OPT_metal_translation_path_EQ,
                                         "spirv-cross", FindProgram);
  if (!TranslatorPathOrErr)
    return TranslatorPathOrErr.takeError();

  auto MSLFileOrErr = CreateOutputFile(sys::path::filename(SPVFile), "metal");
  if (!MSLFileOrErr)
    return MSLFileOrErr.takeError();

  std::string MSLVersionStr = std::to_string(getMSLVersion(Triple));
  SmallVector<StringRef, 10> TranslateArgs = {
      *TranslatorPathOrErr, SPVFile,    "--msl",      "--msl-version",
      MSLVersionStr,        "--output", *MSLFileOrErr};
  if (Error Err =
          ExecuteCommand(*TranslatorPathOrErr, TranslateArgs, std::nullopt))
    return std::move(Err);

  auto AIRFileOrErr =
      CreateOutputFile(sys::path::filename(*MSLFileOrErr), "air");
  if (!AIRFileOrErr)
    return AIRFileOrErr.takeError();

  SmallVector<std::string> EnvStorage = buildMetalEnvironment(Args);
  SmallVector<StringRef, 8> MetalCompilerArgs = {"-c", *MSLFileOrErr, "-o",
                                                 *AIRFileOrErr};
  appendMetalDeploymentTarget(MetalCompilerArgs, Args, Triple);
  if (Error Err = runMetalTool(Args, OPT_metal_compiler_path_EQ, "metal",
                               MetalCompilerArgs, EnvStorage, FindProgram,
                               ExecuteCommand))
    return std::move(Err);

  auto MetallibFileOrErr =
      CreateOutputFile(sys::path::filename(*AIRFileOrErr), "metallib");
  if (!MetallibFileOrErr)
    return MetallibFileOrErr.takeError();

  SmallVector<StringRef, 4> MetallibArgs = {*AIRFileOrErr, "-o",
                                            *MetallibFileOrErr};
  if (Error Err =
          runMetalTool(Args, OPT_metallib_path_EQ, "metallib", MetallibArgs,
                       EnvStorage, FindProgram, ExecuteCommand))
    return std::move(Err);

  return FinalizedImageFile{*MetallibFileOrErr, IMG_Metallib,
                            /*NeedsContainerization=*/false};
}
