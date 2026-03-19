//===-RTLs/metal/src/rtl.cpp - Target RTLs Implementation - C++ ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Experimental nextgen plugin for Apple Metal.
//
//===----------------------------------------------------------------------===//

#include "GlobalHandler.h"
#include "MetalMetadata.h"
#include "MetalRuntime.h"
#include "PluginInterface.h"
#include "Shared/Debug.h"
#include "omptarget.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Frontend/OpenMP/OMPConstants.h"
#include "llvm/Frontend/OpenMP/OMPGridValues.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <shared_mutex>

namespace llvm::omp::target::plugin {

using namespace llvm::omp::target;
using namespace error;

static Error unsupported(StringRef What) {
  return Plugin::error(ErrorCode::UNSUPPORTED,
                       "%s is not implemented for Metal", What.data());
}

static constexpr size_t RuntimeErrorBufferSize = 512;

static Error runtimeError(error::ErrorCode Code, StringRef Context,
                          StringRef Message) {
  return Plugin::error(Code, "%s: %s", Context.data(), Message.data());
}

class RuntimeErrorBuffer final {
public:
  RuntimeErrorBuffer() { Storage[0] = '\0'; }

  char *data() { return Storage; }
  size_t size() const { return sizeof(Storage); }
  StringRef message() const {
    return Storage[0] ? StringRef(Storage) : StringRef("unknown Metal error");
  }

private:
  char Storage[RuntimeErrorBufferSize];
};

static constexpr GV MetalGridValues = {
    8,     // GV_Slot_Size
    32,    // GV_Warp_Size
    65535, // GV_Max_Teams
    128,   // GV_Default_Num_Teams
    1024,  // GV_SimpleBufferSize
    1024,  // GV_Max_WG_Size
    256,   // GV_Default_WG_Size
};

struct MetalAsyncQueue {
  uint64_t QueueId = 0;
  OMPMetalCommandQueue *Queue = nullptr;
  llvm::SmallVector<OMPMetalCommandBuffer *, 4> InFlight;
  std::mutex Mutex;
};

struct MetalEvent {
  OMPMetalCommandBuffer *CommandBuffer = nullptr;
  uint64_t QueueId = 0;
};

static std::atomic<uint64_t> NextMetalQueueId{1};

static void destroyAsyncQueue(MetalAsyncQueue *QueueState) {
  if (!QueueState)
    return;

  if (QueueState->Queue)
    omp_metal_destroy_command_queue(QueueState->Queue);
  delete QueueState;
}

static Error waitUntilIdleLocked(MetalAsyncQueue &QueueState) {
  RuntimeErrorBuffer ErrMsg;
  for (auto *CommandBuffer : QueueState.InFlight) {
    if (!omp_metal_command_buffer_wait(CommandBuffer, ErrMsg.data(),
                                       ErrMsg.size()))
      return runtimeError(ErrorCode::BACKEND_FAILURE,
                          "synchronizing Metal command buffer",
                          ErrMsg.message());
    omp_metal_command_buffer_release(CommandBuffer);
  }
  QueueState.InFlight.clear();
  return Plugin::success();
}

static Expected<bool> pruneCompletedLocked(MetalAsyncQueue &QueueState) {
  RuntimeErrorBuffer ErrMsg;
  bool AllComplete = true;
  auto &InFlight = QueueState.InFlight;
  for (auto It = InFlight.begin(); It != InFlight.end();) {
    bool Complete = false;
    if (!omp_metal_command_buffer_is_complete(*It, &Complete, ErrMsg.data(),
                                              ErrMsg.size()))
      return runtimeError(ErrorCode::BACKEND_FAILURE,
                          "querying Metal command buffer", ErrMsg.message());
    if (Complete) {
      omp_metal_command_buffer_release(*It);
      It = InFlight.erase(It);
    } else {
      AllComplete = false;
      ++It;
    }
  }
  return AllComplete;
}

class MetalDeviceImageTy final : public DeviceImageTy {
public:
  MetalDeviceImageTy(int32_t ImageId, GenericDeviceTy &Device,
                     std::unique_ptr<MemoryBuffer> &&Image,
                     OMPMetalLibrary *Library)
      : DeviceImageTy(ImageId, Device, std::move(Image)), Library(Library) {}

  OMPMetalLibrary *getLibrary() const { return Library; }

  Expected<void *> getOrCreateGlobal(GenericDeviceTy &Device, StringRef Name,
                                     uint64_t Size) {
    if (!Size ||
        Size > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
      return Plugin::error(ErrorCode::INVALID_ARGUMENT,
                           "invalid Metal global size for '%s'", Name.data());

    {
      std::lock_guard<std::mutex> Lock(GlobalsMutex);
      auto It = DeviceGlobals.find(Name);
      if (It != DeviceGlobals.end())
        return It->second;
    }

    auto PtrOrErr =
        Device.dataAlloc(static_cast<int64_t>(Size), /*HostPtr=*/nullptr,
                         TargetAllocTy::TARGET_ALLOC_DEVICE);
    if (!PtrOrErr)
      return PtrOrErr.takeError();

    std::lock_guard<std::mutex> Lock(GlobalsMutex);
    auto [It, Inserted] = DeviceGlobals.try_emplace(Name, *PtrOrErr);
    if (!Inserted) {
      if (auto Err =
              Device.dataDelete(*PtrOrErr, TargetAllocTy::TARGET_ALLOC_DEVICE))
        return Err;
      return It->second;
    }
    return It->second;
  }

  Error releaseGlobals(GenericDeviceTy &Device) {
    SmallVector<void *, 8> Globals;
    {
      std::lock_guard<std::mutex> Lock(GlobalsMutex);
      Globals.reserve(DeviceGlobals.size());
      for (auto &Entry : DeviceGlobals)
        Globals.push_back(Entry.second);
      DeviceGlobals.clear();
    }

    for (void *Ptr : Globals)
      if (auto Err = Device.dataDelete(Ptr, TargetAllocTy::TARGET_ALLOC_DEVICE))
        return Err;

    return Plugin::success();
  }

  Expected<const MetalImageMetadata &> getMetadata() {
    if (ParsedMetadata)
      return *ParsedMetadata;

    StringRef Descriptor = getString(omp::offload::MetalDescriptorKey);
    if (Descriptor.empty())
      return Plugin::error(ErrorCode::INVALID_BINARY,
                           "Metal image is missing required '%s' metadata",
                           omp::offload::MetalDescriptorKey.data());

    auto MetadataOrErr = parseMetalImageMetadata(Descriptor);
    if (!MetadataOrErr)
      return MetadataOrErr.takeError();
    ParsedMetadata = std::move(*MetadataOrErr);
    return *ParsedMetadata;
  }

private:
  OMPMetalLibrary *Library;
  std::mutex GlobalsMutex;
  StringMap<void *> DeviceGlobals;
  std::optional<MetalImageMetadata> ParsedMetadata;
};

class MetalKernelTy final : public GenericKernelTy {
public:
  MetalKernelTy(const char *Name) : GenericKernelTy(Name), Pipeline(nullptr) {}

  ~MetalKernelTy() override {
    if (Pipeline)
      omp_metal_destroy_pipeline(Pipeline);
  }

  Error initImpl(GenericDeviceTy &GenericDevice, DeviceImageTy &Image) override;

  Error launchImpl(GenericDeviceTy &, uint32_t[3], uint32_t[3], KernelArgsTy &,
                   KernelLaunchParamsTy, AsyncInfoWrapperTy &) const override;

  Expected<uint64_t> maxGroupSize(GenericDeviceTy &, uint64_t) const override {
    if (!Pipeline)
      return Plugin::error(ErrorCode::UNSUPPORTED,
                           "kernel pipeline is not initialized for Metal");

    RuntimeErrorBuffer ErrMsg;
    uint32_t MaxThreads = 0;
    if (!omp_metal_get_pipeline_max_total_threads(Pipeline, &MaxThreads,
                                                  ErrMsg.data(), ErrMsg.size()))
      return runtimeError(ErrorCode::UNSUPPORTED, "querying Metal occupancy",
                          ErrMsg.message());

    return MaxThreads;
  }

private:
  OMPMetalPipeline *Pipeline;
};

class MetalGlobalHandlerTy final : public GenericGlobalHandlerTy {
public:
  Error getGlobalMetadataFromDevice(GenericDeviceTy &, DeviceImageTy &,
                                    GlobalTy &) override;
};

Error MetalGlobalHandlerTy::getGlobalMetadataFromDevice(
    GenericDeviceTy &Device, DeviceImageTy &Image, GlobalTy &DeviceGlobal) {
  auto &MetalImage = static_cast<MetalDeviceImageTy &>(Image);
  auto PtrOrErr = MetalImage.getOrCreateGlobal(Device, DeviceGlobal.getName(),
                                               DeviceGlobal.getSize());
  if (!PtrOrErr)
    return PtrOrErr.takeError();

  DeviceGlobal.setPtr(*PtrOrErr);
  return Plugin::success();
}

class MetalDeviceTy final : public GenericDeviceTy {
public:
  MetalDeviceTy(GenericPluginTy &Plugin, int32_t DeviceId, int32_t NumDevices)
      : GenericDeviceTy(Plugin, DeviceId, NumDevices, MetalGridValues),
        Device(nullptr), Queue(nullptr), Info{} {}

  ~MetalDeviceTy() override = default;

  Error setContext() override { return Plugin::success(); }

  Error initImpl(GenericPluginTy &) override;

  Error deinitImpl() override;

  Expected<DeviceImageTy *>
  loadBinaryImpl(std::unique_ptr<MemoryBuffer> &&Image,
                 int32_t ImageId) override;

  Error unloadBinaryImpl(DeviceImageTy *Image) override {
    auto *MetalImage = static_cast<MetalDeviceImageTy *>(Image);
    if (auto Err = MetalImage->releaseGlobals(*this))
      return Err;
    omp_metal_destroy_library(MetalImage->getLibrary());
    Plugin.free(MetalImage);
    return Plugin::success();
  }

  Expected<GenericKernelTy &> constructKernel(const char *Name) override {
    auto *Kernel = Plugin.allocate<MetalKernelTy>();
    if (!Kernel)
      return Plugin::error(ErrorCode::OUT_OF_RESOURCES,
                           "failed to allocate memory for Metal kernel");
    new (Kernel) MetalKernelTy(Name);
    return *Kernel;
  }

  std::string getComputeUnitKind() const override { return "simdgroup"; }

  Expected<void *> allocate(size_t Size, void *, TargetAllocTy Kind) override;

  Error free(void *TgtPtr, TargetAllocTy Kind) override;

  Expected<void *> dataLockImpl(void *HstPtr, int64_t) override {
    return HstPtr;
  }

  Error dataUnlockImpl(void *) override { return Plugin::success(); }

  Expected<bool> isPinnedPtrImpl(void *, void *&, void *&,
                                 size_t &) const override {
    return false;
  }

  Error dataSubmitImpl(void *TgtPtr, const void *HstPtr, int64_t Size,
                       AsyncInfoWrapperTy &) override;

  Error dataRetrieveImpl(void *HstPtr, const void *TgtPtr, int64_t Size,
                         AsyncInfoWrapperTy &) override;

  Error dataFence(__tgt_async_info *AsyncInfo) override {
    if (!AsyncInfo)
      return Plugin::success();
    return synchronize(AsyncInfo, /*ReleaseQueue=*/false);
  }

  Error dataExchangeImpl(const void *SrcPtr, GenericDeviceTy &DstDevice,
                         void *DstPtr, int64_t Size,
                         AsyncInfoWrapperTy &) override;

  Error dataFillImpl(void *TgtPtr, const void *PatternPtr, int64_t PatternSize,
                     int64_t Size, AsyncInfoWrapperTy &) override;

  Error initAsyncInfoImpl(AsyncInfoWrapperTy &AsyncInfoWrapper) override {
    auto QueueOrErr = getOrCreateAsyncQueue(AsyncInfoWrapper);
    if (!QueueOrErr)
      return QueueOrErr.takeError();
    return Plugin::success();
  }

  Error enqueueHostCallImpl(void (*Callback)(void *), void *UserData,
                            AsyncInfoWrapperTy &AsyncInfoWrapper) override {
    if (AsyncInfoWrapper.usesLocalAsyncInfo()) {
      Callback(UserData);
      return Plugin::success();
    }

    auto QueueOrErr = getOrCreateAsyncQueue(AsyncInfoWrapper);
    if (!QueueOrErr)
      return QueueOrErr.takeError();

    auto *QueueState = *QueueOrErr;
    std::lock_guard<std::mutex> Lock(QueueState->Mutex);
    if (auto Err = waitUntilIdleLocked(*QueueState))
      return Err;

    // Keep the queue mutex held while running the callback so no later work can
    // be committed to this queue before the callback returns.
    Callback(UserData);
    return Plugin::success();
  }

  Error synchronizeImpl(__tgt_async_info &AsyncInfo,
                        bool ReleaseQueue) override {
    if (!AsyncInfo.Queue)
      return Plugin::success();

    auto *QueueState = static_cast<MetalAsyncQueue *>(AsyncInfo.Queue);
    {
      std::lock_guard<std::mutex> Lock(QueueState->Mutex);
      if (auto Err = waitUntilIdleLocked(*QueueState))
        return Err;
    }

    if (ReleaseQueue) {
      AsyncInfo.Queue = nullptr;
      destroyAsyncQueue(QueueState);
      return Plugin::success();
    }

    return Plugin::success();
  }

  Error queryAsyncImpl(__tgt_async_info &AsyncInfo, bool ReleaseQueue,
                       bool *IsQueueWorkCompleted) override {
    if (!AsyncInfo.Queue) {
      if (IsQueueWorkCompleted)
        *IsQueueWorkCompleted = true;
      return Plugin::success();
    }

    auto *QueueState = static_cast<MetalAsyncQueue *>(AsyncInfo.Queue);
    bool AllComplete = false;
    {
      std::lock_guard<std::mutex> Lock(QueueState->Mutex);
      auto AllCompleteOrErr = pruneCompletedLocked(*QueueState);
      if (!AllCompleteOrErr)
        return AllCompleteOrErr.takeError();
      AllComplete = *AllCompleteOrErr;
    }

    if (IsQueueWorkCompleted)
      *IsQueueWorkCompleted = AllComplete;

    if (AllComplete && ReleaseQueue) {
      AsyncInfo.Queue = nullptr;
      destroyAsyncQueue(QueueState);
      return Plugin::success();
    }

    return Plugin::success();
  }

  Expected<bool>
  hasPendingWorkImpl(AsyncInfoWrapperTy &AsyncInfoWrapper) override {
    if (!AsyncInfoWrapper.hasQueue())
      return false;

    auto *QueueState = AsyncInfoWrapper.getQueueAs<MetalAsyncQueue *>();
    if (!QueueState)
      return Plugin::error(ErrorCode::INVALID_ARGUMENT,
                           "invalid Metal async queue");

    std::lock_guard<std::mutex> Lock(QueueState->Mutex);
    return !QueueState->InFlight.empty();
  }

  Error createEventImpl(void **EventPtrStorage) override {
    if (!EventPtrStorage)
      return Plugin::error(ErrorCode::INVALID_ARGUMENT,
                           "null event storage for Metal");

    auto *Event = new (std::nothrow) MetalEvent();
    if (!Event)
      return Plugin::error(ErrorCode::OUT_OF_RESOURCES,
                           "failed to allocate Metal event");
    *EventPtrStorage = Event;
    return Plugin::success();
  }

  Error destroyEventImpl(void *EventPtr) override {
    if (!EventPtr)
      return Plugin::success();

    auto *Event = static_cast<MetalEvent *>(EventPtr);
    if (Event->CommandBuffer)
      omp_metal_command_buffer_release(Event->CommandBuffer);
    delete Event;
    return Plugin::success();
  }

  Error recordEventImpl(void *EventPtr,
                        AsyncInfoWrapperTy &AsyncInfoWrapper) override {
    if (!EventPtr)
      return Plugin::error(ErrorCode::INVALID_ARGUMENT, "null Metal event");

    auto *Event = static_cast<MetalEvent *>(EventPtr);
    if (Event->CommandBuffer) {
      omp_metal_command_buffer_release(Event->CommandBuffer);
      Event->CommandBuffer = nullptr;
      Event->QueueId = 0;
    }

    if (!AsyncInfoWrapper.hasQueue())
      return Plugin::success();

    auto *QueueState = AsyncInfoWrapper.getQueueAs<MetalAsyncQueue *>();
    if (!QueueState)
      return Plugin::error(ErrorCode::INVALID_ARGUMENT,
                           "invalid Metal async queue");

    std::lock_guard<std::mutex> Lock(QueueState->Mutex);
    if (!QueueState->InFlight.empty()) {
      auto *CommandBuffer = QueueState->InFlight.back();
      omp_metal_command_buffer_retain(CommandBuffer);
      Event->CommandBuffer = CommandBuffer;
      Event->QueueId = QueueState->QueueId;
    }

    return Plugin::success();
  }

  Error waitEventImpl(void *EventPtr,
                      AsyncInfoWrapperTy &AsyncInfoWrapper) override {
    if (!EventPtr)
      return Plugin::error(ErrorCode::INVALID_ARGUMENT, "null Metal event");

    auto *Event = static_cast<MetalEvent *>(EventPtr);
    if (!Event->CommandBuffer)
      return Plugin::success();

    if (AsyncInfoWrapper.hasQueue()) {
      auto *QueueState = AsyncInfoWrapper.getQueueAs<MetalAsyncQueue *>();
      if (!QueueState)
        return Plugin::error(ErrorCode::INVALID_ARGUMENT,
                             "invalid Metal async queue");
      if (QueueState->QueueId == Event->QueueId)
        return Plugin::success();

      // Metal does not expose a lightweight cross-queue wait primitive here, so
      // serialize by waiting on the producer before more work is committed.
      std::lock_guard<std::mutex> Lock(QueueState->Mutex);
      RuntimeErrorBuffer ErrMsg;
      if (!omp_metal_command_buffer_wait(Event->CommandBuffer, ErrMsg.data(),
                                         ErrMsg.size()))
        return runtimeError(ErrorCode::BACKEND_FAILURE,
                            "waiting on Metal event", ErrMsg.message());
      return Plugin::success();
    }

    RuntimeErrorBuffer ErrMsg;
    if (!omp_metal_command_buffer_wait(Event->CommandBuffer, ErrMsg.data(),
                                       ErrMsg.size()))
      return runtimeError(ErrorCode::BACKEND_FAILURE, "waiting on Metal event",
                          ErrMsg.message());

    return Plugin::success();
  }

  Expected<bool> isEventCompleteImpl(void *EventPtr,
                                     AsyncInfoWrapperTy &) override {
    if (!EventPtr)
      return Plugin::error(ErrorCode::INVALID_ARGUMENT, "null Metal event");

    auto *Event = static_cast<MetalEvent *>(EventPtr);
    if (!Event->CommandBuffer)
      return true;

    RuntimeErrorBuffer ErrMsg;
    bool Complete = false;
    if (!omp_metal_command_buffer_is_complete(Event->CommandBuffer, &Complete,
                                              ErrMsg.data(), ErrMsg.size()))
      return runtimeError(ErrorCode::BACKEND_FAILURE, "querying Metal event",
                          ErrMsg.message());

    return Complete;
  }

  Error syncEventImpl(void *EventPtr) override {
    if (!EventPtr)
      return Plugin::error(ErrorCode::INVALID_ARGUMENT, "null Metal event");

    auto *Event = static_cast<MetalEvent *>(EventPtr);
    if (!Event->CommandBuffer)
      return Plugin::success();

    RuntimeErrorBuffer ErrMsg;
    if (!omp_metal_command_buffer_wait(Event->CommandBuffer, ErrMsg.data(),
                                       ErrMsg.size()))
      return runtimeError(ErrorCode::BACKEND_FAILURE,
                          "synchronizing Metal event", ErrMsg.message());

    return Plugin::success();
  }

  Expected<InfoTreeNode> obtainInfoImpl() override;

  Error getDeviceStackSize(uint64_t &Value) override {
    Value = 0;
    return Plugin::success();
  }

  Error setDeviceStackSize(uint64_t) override { return Plugin::success(); }

  bool useAutoZeroCopyImpl() override { return Info.HasUnifiedMemory; }

  Expected<bool> isAccessiblePtrImpl(const void *Ptr, size_t Size) override;

  Error getDeviceMemorySize(uint64_t &Size) override {
    Size = Info.RecommendedMaxWorkingSetSize ? Info.RecommendedMaxWorkingSetSize
                                             : Info.MaxBufferLength;
    return Plugin::success();
  }

  OMPMetalDevice *getDeviceHandle() const { return Device; }
  OMPMetalCommandQueue *getQueueHandle() const { return Queue; }

private:
  friend class MetalKernelTy;

  struct AllocationEntry {
    OMPMetalBuffer *Buffer;
    size_t Size;
    TargetAllocTy Kind;
  };

  struct AllocationView {
    OMPMetalBuffer *Buffer;
    void *BasePtr;
    size_t Size;
    size_t Offset;
    TargetAllocTy Kind;
  };

  Error synchronizeForHostAccess(AsyncInfoWrapperTy &AsyncInfoWrapper) {
    if (!AsyncInfoWrapper.hasQueue())
      return Plugin::success();
    return synchronize(static_cast<__tgt_async_info *>(AsyncInfoWrapper),
                       /*ReleaseQueue=*/false);
  }

  Expected<MetalAsyncQueue *>
  getOrCreateAsyncQueue(AsyncInfoWrapperTy &AsyncInfoWrapper);
  Expected<AllocationView> getAllocationView(const void *Ptr,
                                             size_t Size) const;
  Error releaseAllocations();

  mutable std::shared_mutex AllocationMutex;
  std::map<uintptr_t, AllocationEntry> Allocations;
  OMPMetalDevice *Device;
  OMPMetalCommandQueue *Queue;
  OMPMetalDeviceInfo Info;
};

Error MetalKernelTy::initImpl(GenericDeviceTy &GenericDevice,
                              DeviceImageTy &Image) {
  auto &MetalDevice = static_cast<MetalDeviceTy &>(GenericDevice);
  auto &MetalImage = static_cast<MetalDeviceImageTy &>(Image);
  auto MetadataOrErr = MetalImage.getMetadata();
  if (!MetadataOrErr)
    return MetadataOrErr.takeError();
  const MetalImageMetadata &ImageMetadata = *MetadataOrErr;
  const MetalKernelMetadata *Metadata = ImageMetadata.findKernel(getName());
  if (!Metadata)
    return Plugin::error(ErrorCode::INVALID_BINARY,
                         "Metal descriptor is missing kernel metadata for '%s'",
                         getName());
  if (Metadata->HasEnvironment)
    KernelEnvironment = Metadata->Environment;

  auto HasFunction = [&](StringRef Name) -> Expected<bool> {
    RuntimeErrorBuffer ErrMsg;
    bool Present = false;
    if (!omp_metal_library_has_function(MetalImage.getLibrary(), Name.data(),
                                        &Present, ErrMsg.data(), ErrMsg.size()))
      return runtimeError(ErrorCode::INVALID_BINARY,
                          "querying Metal kernel symbol", ErrMsg.message());
    return Present;
  };

  std::string EntryName =
      Metadata->EntryName.empty() ? getName() : Metadata->EntryName;
  auto HasEntryOrErr = HasFunction(EntryName);
  if (!HasEntryOrErr)
    return HasEntryOrErr.takeError();
  bool Found = *HasEntryOrErr;
  if (!Found && EntryName != getName()) {
    // If the configured entry name is absent, try the raw OpenMP kernel name
    // as a fallback (spirv-cross may have preserved it unchanged).
    auto HasKernelOrErr = HasFunction(getName());
    if (!HasKernelOrErr)
      return HasKernelOrErr.takeError();
    if (*HasKernelOrErr) {
      EntryName = getName();
      Found = true;
    }
  }
  if (!Found)
    return Plugin::error(
        ErrorCode::INVALID_BINARY,
        "unable to find Metal function '%s' for OpenMP kernel '%s'",
        EntryName.c_str(), getName());

  RuntimeErrorBuffer ErrMsg;
  if (!omp_metal_create_compute_pipeline(
          MetalDevice.getDeviceHandle(), MetalImage.getLibrary(),
          EntryName.c_str(), &Pipeline, ErrMsg.data(), ErrMsg.size()))
    return runtimeError(ErrorCode::INVALID_BINARY,
                        "creating Metal compute pipeline", ErrMsg.message());

  uint32_t MaxThreads = 0;
  if (!omp_metal_get_pipeline_max_total_threads(Pipeline, &MaxThreads,
                                                ErrMsg.data(), ErrMsg.size()))
    return runtimeError(ErrorCode::INVALID_BINARY,
                        "querying Metal pipeline limits", ErrMsg.message());

  MaxNumThreads =
      KernelEnvironment.Configuration.MaxThreads > 0
          ? std::min<uint32_t>(KernelEnvironment.Configuration.MaxThreads,
                               MaxThreads)
          : MaxThreads;
  PreferredNumThreads =
      KernelEnvironment.Configuration.MinThreads > 0
          ? std::max<uint32_t>(KernelEnvironment.Configuration.MinThreads,
                               GenericDevice.getDefaultNumThreads())
          : GenericDevice.getDefaultNumThreads();
  PreferredNumThreads = std::min(PreferredNumThreads, MaxNumThreads);
  return Plugin::success();
}

Error MetalKernelTy::launchImpl(GenericDeviceTy &GenericDevice,
                                uint32_t NumThreads[3], uint32_t NumBlocks[3],
                                KernelArgsTy &KernelArgs,
                                KernelLaunchParamsTy LaunchParams,
                                AsyncInfoWrapperTy &AsyncInfoWrapper) const {
  if (!Pipeline)
    return Plugin::error(ErrorCode::UNINITIALIZED,
                         "Metal kernel pipeline is not initialized");
  // DynCGroupMem is forwarded to Metal as threadgroup memory at index 0,
  // matching the [[threadgroup(0)]] attribute emitted by spirv-cross.
  if (LaunchParams.Size % sizeof(void *) != 0)
    return Plugin::error(ErrorCode::INVALID_ARGUMENT,
                         "Metal kernel argument buffer size is not aligned");
  if (LaunchParams.Size && !LaunchParams.Data)
    return Plugin::error(ErrorCode::INVALID_ARGUMENT,
                         "Metal kernel argument buffer is null");
  if (LaunchParams.Data &&
      (reinterpret_cast<uintptr_t>(LaunchParams.Data) % alignof(void *) != 0))
    return Plugin::error(ErrorCode::INVALID_ARGUMENT,
                         "Metal kernel argument buffer is not aligned");

  auto &MetalDevice = static_cast<MetalDeviceTy &>(GenericDevice);
  size_t NumArgs = LaunchParams.Size / sizeof(void *);
  auto **ArgValues = reinterpret_cast<void **>(LaunchParams.Data);

  SmallVector<OMPMetalBuffer *, 16> Buffers(NumArgs, nullptr);
  SmallVector<uint64_t, 16> Offsets(NumArgs, 0);
  SmallVector<uintptr_t, 16> InlinePointerValues(NumArgs, 0);
  SmallVector<const void *, 16> InlineArgData(NumArgs, nullptr);
  SmallVector<uint64_t, 16> InlineArgSizes(NumArgs, 0);
  SmallVector<uint8_t, 16> UseInlineArgs(NumArgs, 0);
  const bool HasKernelLaunchEnvironment =
      KernelArgs.Version >= OMP_KERNEL_ARG_MIN_VERSION_WITH_DYN_PTR;
  if (HasKernelLaunchEnvironment && NumArgs == 0)
    return Plugin::error(ErrorCode::INVALID_ARGUMENT,
                         "missing Metal kernel launch environment argument");

  const size_t UserArgOffset = HasKernelLaunchEnvironment ? 1u : 0u;
  const size_t NumUserArgs = NumArgs - UserArgOffset;
  for (size_t I = 0; I < NumArgs; ++I) {
    if (!ArgValues[I])
      continue;

    auto ViewOrErr = MetalDevice.getAllocationView(ArgValues[I], 0);
    if (!ViewOrErr) {
      consumeError(ViewOrErr.takeError());
      uintptr_t RawArg = reinterpret_cast<uintptr_t>(ArgValues[I]);
      if (HasKernelLaunchEnvironment && I == 0) {
        InlinePointerValues[I] = RawArg;
        InlineArgData[I] = &InlinePointerValues[I];
        InlineArgSizes[I] = sizeof(uintptr_t);
        UseInlineArgs[I] = 1;
        continue;
      }

      const size_t UserArgIndex = I - UserArgOffset;
      int64_t ArgType = (KernelArgs.ArgTypes && UserArgIndex < NumUserArgs)
                            ? KernelArgs.ArgTypes[UserArgIndex]
                            : int64_t(0);
      int64_t ArgSize = (KernelArgs.ArgSizes && UserArgIndex < NumUserArgs)
                            ? KernelArgs.ArgSizes[UserArgIndex]
                            : int64_t(0);
      const bool IsLiteralArg = ArgType & OMP_TGT_MAPTYPE_LITERAL;
      if (IsLiteralArg) {
        if (ArgSize < 0)
          return Plugin::error(ErrorCode::INVALID_ARGUMENT,
                               "Metal literal kernel argument %zu has "
                               "negative size %" PRId64,
                               I, ArgSize);

        const uint64_t LiteralSize = static_cast<uint64_t>(ArgSize);
        if (LiteralSize == 0 || LiteralSize <= sizeof(uintptr_t)) {
          InlinePointerValues[I] = RawArg;
          InlineArgData[I] = &InlinePointerValues[I];
          InlineArgSizes[I] = LiteralSize
                                  ? LiteralSize
                                  : static_cast<uint64_t>(sizeof(uintptr_t));
          UseInlineArgs[I] = 1;
          continue;
        }

        InlineArgData[I] = ArgValues[I];
        InlineArgSizes[I] = LiteralSize;
        UseInlineArgs[I] = 1;
        continue;
      }

      InlinePointerValues[I] = RawArg;
      InlineArgData[I] = &InlinePointerValues[I];
      InlineArgSizes[I] = sizeof(uintptr_t);
      UseInlineArgs[I] = 1;
      continue;
    }

    Buffers[I] = ViewOrErr->Buffer;
    Offsets[I] = ViewOrErr->Offset;
  }

  const bool IsAsync = !AsyncInfoWrapper.usesLocalAsyncInfo();
  MetalAsyncQueue *AsyncQueue = nullptr;
  OMPMetalCommandQueue *Queue = MetalDevice.getQueueHandle();
  if (IsAsync) {
    auto QueueOrErr = MetalDevice.getOrCreateAsyncQueue(AsyncInfoWrapper);
    if (!QueueOrErr)
      return QueueOrErr.takeError();
    AsyncQueue = *QueueOrErr;
    Queue = AsyncQueue->Queue;
  }
  if (!Queue)
    return Plugin::error(ErrorCode::UNINITIALIZED,
                         "Metal command queue is not initialized");

  OMPMetalCommandBuffer *CommandBuffer = nullptr;
  RuntimeErrorBuffer ErrMsg;
  if (IsAsync) {
    std::lock_guard<std::mutex> Lock(AsyncQueue->Mutex);
    if (!omp_metal_launch_compute(
            Queue, Pipeline, Buffers.data(),
            Offsets.empty() ? nullptr : Offsets.data(),
            InlineArgData.empty() ? nullptr : InlineArgData.data(),
            InlineArgSizes.empty() ? nullptr : InlineArgSizes.data(),
            UseInlineArgs.empty() ? nullptr : UseInlineArgs.data(), NumArgs,
            KernelArgs.DynCGroupMem, NumThreads, NumBlocks,
            /*WaitForCompletion=*/false, &CommandBuffer, ErrMsg.data(),
            ErrMsg.size()))
      return runtimeError(ErrorCode::UNKNOWN, "launching Metal kernel",
                          ErrMsg.message());
    if (CommandBuffer)
      AsyncQueue->InFlight.push_back(CommandBuffer);
    return Plugin::success();
  }

  if (!omp_metal_launch_compute(
          Queue, Pipeline, Buffers.data(),
          Offsets.empty() ? nullptr : Offsets.data(),
          InlineArgData.empty() ? nullptr : InlineArgData.data(),
          InlineArgSizes.empty() ? nullptr : InlineArgSizes.data(),
          UseInlineArgs.empty() ? nullptr : UseInlineArgs.data(), NumArgs,
          KernelArgs.DynCGroupMem, NumThreads, NumBlocks,
          /*WaitForCompletion=*/true,
          /*OutCommandBuffer=*/nullptr, ErrMsg.data(), ErrMsg.size()))
    return runtimeError(ErrorCode::UNKNOWN, "launching Metal kernel",
                        ErrMsg.message());

  return Plugin::success();
}

Error MetalDeviceTy::initImpl(GenericPluginTy &) {
  RuntimeErrorBuffer ErrMsg;
  if (!omp_metal_create_device(getDeviceId(), &Device, ErrMsg.data(),
                               ErrMsg.size()))
    return runtimeError(ErrorCode::BACKEND_FAILURE, "creating Metal device",
                        ErrMsg.message());

  if (!omp_metal_query_device(Device, &Info, ErrMsg.data(), ErrMsg.size())) {
    omp_metal_destroy_device(Device);
    Device = nullptr;
    return runtimeError(ErrorCode::BACKEND_FAILURE, "querying Metal device",
                        ErrMsg.message());
  }

  if (!omp_metal_create_command_queue(Device, &Queue, ErrMsg.data(),
                                      ErrMsg.size())) {
    omp_metal_destroy_device(Device);
    Device = nullptr;
    return runtimeError(ErrorCode::BACKEND_FAILURE,
                        "creating Metal command queue", ErrMsg.message());
  }

  if (Info.RegistryID)
    setDeviceUidFromVendorUid(std::to_string(Info.RegistryID));
  else if (Info.Name[0] != '\0')
    setDeviceUidFromVendorUid(Info.Name);

  return Plugin::success();
}

Error MetalDeviceTy::deinitImpl() {
  if (auto Err = releaseAllocations())
    return Err;

  if (Queue) {
    omp_metal_destroy_command_queue(Queue);
    Queue = nullptr;
  }

  if (Device) {
    omp_metal_destroy_device(Device);
    Device = nullptr;
  }
  std::memset(&Info, 0, sizeof(Info));
  return Plugin::success();
}

Expected<MetalAsyncQueue *>
MetalDeviceTy::getOrCreateAsyncQueue(AsyncInfoWrapperTy &AsyncInfoWrapper) {
  __tgt_async_info *AsyncInfo = AsyncInfoWrapper;
  std::lock_guard<std::mutex> Lock(AsyncInfo->Mutex);

  if (AsyncInfo->Queue) {
    auto *QueueState = static_cast<MetalAsyncQueue *>(AsyncInfo->Queue);
    if (!QueueState || !QueueState->Queue)
      return Plugin::error(ErrorCode::INVALID_ARGUMENT,
                           "invalid Metal async queue");
    return QueueState;
  }

  if (!Device)
    return Plugin::error(ErrorCode::UNINITIALIZED,
                         "Metal device is not initialized");

  auto *QueueState = new (std::nothrow) MetalAsyncQueue();
  if (!QueueState)
    return Plugin::error(ErrorCode::OUT_OF_RESOURCES,
                         "failed to allocate Metal async queue");

  // External async handles need distinct Metal command queues so completion and
  // host-visible synchronization are scoped to the handle that issued the work.
  RuntimeErrorBuffer ErrMsg;
  if (!omp_metal_create_command_queue(Device, &QueueState->Queue, ErrMsg.data(),
                                      ErrMsg.size())) {
    destroyAsyncQueue(QueueState);
    return runtimeError(ErrorCode::BACKEND_FAILURE,
                        "creating Metal async queue", ErrMsg.message());
  }

  QueueState->QueueId =
      NextMetalQueueId.fetch_add(1, std::memory_order_relaxed);
  AsyncInfo->Queue = QueueState;
  return QueueState;
}

Expected<DeviceImageTy *>
MetalDeviceTy::loadBinaryImpl(std::unique_ptr<MemoryBuffer> &&Image,
                              int32_t ImageId) {
  if (!Image || Image->getBufferSize() == 0)
    return Plugin::error(ErrorCode::INVALID_BINARY,
                         "cannot load an empty Metal image");
  if (!Device)
    return Plugin::error(ErrorCode::UNINITIALIZED,
                         "cannot load Metal image before device init");

  RuntimeErrorBuffer ErrMsg;
  OMPMetalLibrary *Library = nullptr;
  if (!omp_metal_create_library(Device, Image->getBufferStart(),
                                Image->getBufferSize(), &Library, ErrMsg.data(),
                                ErrMsg.size()))
    return runtimeError(ErrorCode::INVALID_BINARY, "loading Metal library",
                        ErrMsg.message());

  auto *MetalImage = Plugin.allocate<MetalDeviceImageTy>();
  if (!MetalImage) {
    omp_metal_destroy_library(Library);
    return Plugin::error(ErrorCode::OUT_OF_RESOURCES,
                         "failed to allocate memory for Metal image");
  }

  new (MetalImage)
      MetalDeviceImageTy(ImageId, *this, std::move(Image), Library);
  return MetalImage;
}

Expected<void *> MetalDeviceTy::allocate(size_t Size, void *,
                                         TargetAllocTy Kind) {
  if (Size == 0)
    return nullptr;
  if (!Device)
    return Plugin::error(ErrorCode::UNINITIALIZED,
                         "Metal device is not initialized");

  RuntimeErrorBuffer ErrMsg;
  OMPMetalBuffer *Buffer = nullptr;
  if (!omp_metal_allocate_shared_buffer(Device, Size, "libomptarget-metal",
                                        &Buffer, ErrMsg.data(), ErrMsg.size()))
    return runtimeError(ErrorCode::OUT_OF_RESOURCES,
                        "allocating Metal shared buffer", ErrMsg.message());

  void *BasePtr = omp_metal_get_buffer_contents(Buffer);
  size_t BufferSize = omp_metal_get_buffer_size(Buffer);
  if (!BasePtr || BufferSize < Size) {
    omp_metal_destroy_buffer(Buffer);
    return Plugin::error(ErrorCode::OUT_OF_RESOURCES,
                         "failed to get CPU-visible storage for Metal buffer");
  }

  std::lock_guard<std::shared_mutex> Lock(AllocationMutex);
  auto [It, Inserted] =
      Allocations.try_emplace(reinterpret_cast<uintptr_t>(BasePtr),
                              AllocationEntry{Buffer, BufferSize, Kind});
  if (!Inserted) {
    omp_metal_destroy_buffer(Buffer);
    return Plugin::error(ErrorCode::INVALID_ARGUMENT,
                         "duplicate Metal allocation base pointer");
  }

  return BasePtr;
}

Error MetalDeviceTy::free(void *TgtPtr, TargetAllocTy) {
  if (!TgtPtr)
    return Plugin::success();

  OMPMetalBuffer *Buffer = nullptr;
  {
    std::lock_guard<std::shared_mutex> Lock(AllocationMutex);
    auto It = Allocations.find(reinterpret_cast<uintptr_t>(TgtPtr));
    if (It == Allocations.end())
      return Plugin::error(ErrorCode::INVALID_ARGUMENT,
                           "invalid Metal allocation pointer");
    Buffer = It->second.Buffer;
    Allocations.erase(It);
  }

  omp_metal_destroy_buffer(Buffer);
  return Plugin::success();
}

Expected<MetalDeviceTy::AllocationView>
MetalDeviceTy::getAllocationView(const void *Ptr, size_t Size) const {
  if (!Ptr)
    return Plugin::error(ErrorCode::INVALID_ARGUMENT,
                         "null Metal allocation pointer");

  const uintptr_t Address = reinterpret_cast<uintptr_t>(Ptr);
  std::shared_lock<std::shared_mutex> Lock(AllocationMutex);
  auto It = Allocations.upper_bound(Address);
  if (It == Allocations.begin())
    return Plugin::error(ErrorCode::INVALID_ARGUMENT,
                         "pointer is not owned by the Metal device");

  --It;
  const uintptr_t Base = It->first;
  const AllocationEntry &Entry = It->second;
  const uintptr_t End = Base + Entry.Size;
  if (Address < Base || Address >= End)
    return Plugin::error(ErrorCode::INVALID_ARGUMENT,
                         "pointer is not owned by the Metal device");

  const size_t Offset = Address - Base;
  if (Offset > Entry.Size || Size > Entry.Size - Offset)
    return Plugin::error(ErrorCode::INVALID_ARGUMENT,
                         "requested range exceeds Metal allocation bounds");

  return AllocationView{Entry.Buffer, reinterpret_cast<void *>(Base),
                        Entry.Size, Offset, Entry.Kind};
}

Error MetalDeviceTy::dataSubmitImpl(void *TgtPtr, const void *HstPtr,
                                    int64_t Size,
                                    AsyncInfoWrapperTy &AsyncInfoWrapper) {
  if (Size < 0)
    return Plugin::error(ErrorCode::INVALID_ARGUMENT,
                         "negative transfer size is invalid");
  if (Size == 0)
    return Plugin::success();
  if (!HstPtr)
    return Plugin::error(ErrorCode::INVALID_ARGUMENT,
                         "null host source pointer is invalid");

  auto ViewOrErr = getAllocationView(TgtPtr, static_cast<size_t>(Size));
  if (!ViewOrErr)
    return ViewOrErr.takeError();
  if (auto Err = synchronizeForHostAccess(AsyncInfoWrapper))
    return Err;

  std::memcpy(TgtPtr, HstPtr, Size);
  return Plugin::success();
}

Error MetalDeviceTy::dataRetrieveImpl(void *HstPtr, const void *TgtPtr,
                                      int64_t Size,
                                      AsyncInfoWrapperTy &AsyncInfoWrapper) {
  if (Size < 0)
    return Plugin::error(ErrorCode::INVALID_ARGUMENT,
                         "negative transfer size is invalid");
  if (Size == 0)
    return Plugin::success();
  if (!HstPtr)
    return Plugin::error(ErrorCode::INVALID_ARGUMENT,
                         "null host destination pointer is invalid");

  auto ViewOrErr = getAllocationView(TgtPtr, static_cast<size_t>(Size));
  if (!ViewOrErr)
    return ViewOrErr.takeError();
  if (auto Err = synchronizeForHostAccess(AsyncInfoWrapper))
    return Err;

  std::memcpy(HstPtr, TgtPtr, Size);
  return Plugin::success();
}

Error MetalDeviceTy::dataExchangeImpl(const void *SrcPtr,
                                      GenericDeviceTy &DstDevice, void *DstPtr,
                                      int64_t Size,
                                      AsyncInfoWrapperTy &AsyncInfoWrapper) {
  if (Size < 0)
    return Plugin::error(ErrorCode::INVALID_ARGUMENT,
                         "negative transfer size is invalid");
  if (Size == 0)
    return Plugin::success();

  auto SrcViewOrErr = getAllocationView(SrcPtr, static_cast<size_t>(Size));
  if (!SrcViewOrErr)
    return SrcViewOrErr.takeError();

  if (&DstDevice.Plugin != &Plugin)
    return unsupported("peer data exchange");
  auto &DstMetalDevice = static_cast<MetalDeviceTy &>(DstDevice);

  auto DstViewOrErr =
      DstMetalDevice.getAllocationView(DstPtr, static_cast<size_t>(Size));
  if (!DstViewOrErr)
    return DstViewOrErr.takeError();
  if (auto Err = synchronizeForHostAccess(AsyncInfoWrapper))
    return Err;

  std::memmove(DstPtr, SrcPtr, Size);
  return Plugin::success();
}

Error MetalDeviceTy::dataFillImpl(void *TgtPtr, const void *PatternPtr,
                                  int64_t PatternSize, int64_t Size,
                                  AsyncInfoWrapperTy &AsyncInfoWrapper) {
  if (PatternSize <= 0 || Size < 0)
    return Plugin::error(ErrorCode::INVALID_ARGUMENT,
                         "invalid Metal fill request");
  if (Size == 0)
    return Plugin::success();
  if (!PatternPtr)
    return Plugin::error(ErrorCode::INVALID_ARGUMENT,
                         "null fill pattern pointer is invalid");

  auto ViewOrErr = getAllocationView(TgtPtr, static_cast<size_t>(Size));
  if (!ViewOrErr)
    return ViewOrErr.takeError();
  if (auto Err = synchronizeForHostAccess(AsyncInfoWrapper))
    return Err;

  if (PatternSize == 1) {
    std::memset(TgtPtr, *static_cast<const unsigned char *>(PatternPtr), Size);
    return Plugin::success();
  }

  for (int64_t Offset = 0; Offset < Size; Offset += PatternSize) {
    int64_t Bytes = std::min<int64_t>(PatternSize, Size - Offset);
    std::memcpy(static_cast<char *>(TgtPtr) + Offset, PatternPtr, Bytes);
  }

  return Plugin::success();
}

Expected<bool> MetalDeviceTy::isAccessiblePtrImpl(const void *Ptr,
                                                  size_t Size) {
  auto ViewOrErr = getAllocationView(Ptr, Size);
  if (!ViewOrErr) {
    consumeError(ViewOrErr.takeError());
    return false;
  }
  return true;
}

Error MetalDeviceTy::releaseAllocations() {
  std::map<uintptr_t, AllocationEntry> Pending;
  {
    std::lock_guard<std::shared_mutex> Lock(AllocationMutex);
    Pending.swap(Allocations);
  }

  for (auto &Entry : Pending)
    omp_metal_destroy_buffer(Entry.second.Buffer);

  return Plugin::success();
}

Expected<InfoTreeNode> MetalDeviceTy::obtainInfoImpl() {
  InfoTreeNode Root("metal", std::monostate{}, "");
  Root.add("plugin", std::string("nextgen-metal"));
  Root.add("device", std::string(Info.Name[0] ? Info.Name : "unknown"));
  Root.add("image kind", std::string("metallib"));
  Root.add("registry id", Info.RegistryID);
  Root.add("unified memory", Info.HasUnifiedMemory);
  Root.add("low power", Info.LowPower);
  Root.add("removable", Info.Removable);
  Root.add("max buffer length", Info.MaxBufferLength, "bytes");
  Root.add("recommended working set", Info.RecommendedMaxWorkingSetSize,
           "bytes");
  return Root;
}

class MetalPluginTy final : public GenericPluginTy {
public:
  MetalPluginTy() : GenericPluginTy(Triple::aarch64) {}

  Expected<int32_t> initImpl() override {
    RuntimeErrorBuffer ErrMsg;
    uint32_t NumDevices = 0;
    if (!omp_metal_get_device_count(&NumDevices, ErrMsg.data(), ErrMsg.size()))
      return runtimeError(ErrorCode::BACKEND_FAILURE,
                          "enumerating Metal devices", ErrMsg.message());

    ODBG(OLDT_Init) << "Metal plugin discovered " << NumDevices << " device(s)";
    return static_cast<int32_t>(NumDevices);
  }

  Error deinitImpl() override { return Plugin::success(); }

  GenericDeviceTy *createDevice(GenericPluginTy &Plugin, int32_t DeviceId,
                                int32_t NumDevices) override {
    return new MetalDeviceTy(Plugin, DeviceId, NumDevices);
  }

  GenericGlobalHandlerTy *createGlobalHandler() override {
    return new MetalGlobalHandlerTy();
  }

  Expected<bool> isELFCompatible(uint32_t, StringRef) const override {
    return false;
  }

  Expected<bool>
  isImageCompatible(StringRef,
                    const object::OffloadBinary &Binary) const override {
    if (Binary.getImageKind() != object::IMG_Metallib ||
        Binary.getOffloadKind() != object::OFK_OpenMP)
      return false;

    // Check triple/arch before the more expensive JSON descriptor parse.
    Triple T(Binary.getTriple());
    if (!T.isOSDarwin() || T.getVendor() != Triple::Apple)
      return false;
    if (!(T.isSPIRV() || T.getArch() == Triple::aarch64))
      return false;
    StringRef Arch = Binary.getArch();
    if (!Arch.empty() && Arch != "applegpu")
      return false;

    StringRef Descriptor = Binary.getString(omp::offload::MetalDescriptorKey);
    if (Descriptor.empty())
      return Plugin::error(ErrorCode::INVALID_BINARY,
                           "Metal image is missing required '%s' metadata",
                           omp::offload::MetalDescriptorKey.data());

    auto MetadataOrErr = parseMetalImageMetadata(Descriptor);
    if (!MetadataOrErr)
      return MetadataOrErr.takeError();

    return true;
  }

  // Metal images are not ELF; isELFCompatible() always returns false so this
  // value is never consulted, but satisfy the interface with a neutral
  // sentinel.
  uint16_t getMagicElfBits() const override { return 0; }

  Triple::ArchType getTripleArch() const override { return Triple::aarch64; }

  const char *getName() const override { return GETNAME(TARGET_NAME); }
};

} // namespace llvm::omp::target::plugin

extern "C" {
llvm::omp::target::plugin::GenericPluginTy *createPlugin_metal() {
  return new llvm::omp::target::plugin::MetalPluginTy();
}
}
