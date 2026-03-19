//===- MetalRuntime.mm - Apple Metal runtime bridge ------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "MetalRuntime.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <dispatch/dispatch.h>

#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <new>

struct OMPMetalDevice {
  id<MTLDevice> Device;
};

struct OMPMetalBuffer {
  id<MTLBuffer> Buffer;
};

struct OMPMetalLibrary {
  id<MTLLibrary> Library;
};

struct OMPMetalPipeline {
  id<MTLComputePipelineState> Pipeline;
};

struct OMPMetalCommandQueue {
  id<MTLCommandQueue> Queue;
};

struct OMPMetalCommandBuffer {
  id<MTLCommandBuffer> Buffer;
  std::atomic<uint32_t> RefCount;
};

static void setError(char *ErrMsg, size_t ErrMsgCapacity, NSString *Message) {
  if (!ErrMsg || ErrMsgCapacity == 0)
    return;

  const char *Text = Message ? Message.UTF8String : "unknown Metal runtime error";
  std::snprintf(ErrMsg, ErrMsgCapacity, "%s", Text ? Text : "unknown error");
}

static void clearError(char *ErrMsg, size_t ErrMsgCapacity) {
  if (ErrMsg && ErrMsgCapacity)
    ErrMsg[0] = '\0';
}

static NSArray<id<MTLDevice>> *copyDeviceList(void) {
  NSArray<id<MTLDevice>> *Devices = nil;
  if (@available(macOS 10.13, *))
    Devices = [MTLCopyAllDevices() autorelease];

  if ([Devices count] == 0) {
    id<MTLDevice> DefaultDevice = MTLCreateSystemDefaultDevice();
    if (DefaultDevice) {
      Devices = @[ DefaultDevice ];
      [DefaultDevice release];
    }
  }

  return Devices;
}

static id<MTLFunction> copyFunction(OMPMetalLibrary *Library, const char *Name,
                                    char *ErrMsg, size_t ErrMsgCapacity) {
  if (!Library || !Library->Library || !Name || Name[0] == '\0') {
    setError(ErrMsg, ErrMsgCapacity, @"invalid Metal function lookup request");
    return nil;
  }

  NSString *FunctionName = [NSString stringWithUTF8String:Name];
  if (!FunctionName) {
    setError(ErrMsg, ErrMsgCapacity, @"invalid UTF-8 Metal function name");
    return nil;
  }

  id<MTLFunction> Function = [Library->Library newFunctionWithName:FunctionName];
  if (!Function)
    setError(ErrMsg, ErrMsgCapacity,
             [NSString stringWithFormat:@"Metal function '%s' was not found",
                                        Name]);
  return Function;
}

int omp_metal_get_device_count(uint32_t *Count, char *ErrMsg,
                               size_t ErrMsgCapacity) {
  @autoreleasepool {
    clearError(ErrMsg, ErrMsgCapacity);
    if (!Count) {
      setError(ErrMsg, ErrMsgCapacity, @"device-count output is null");
      return 0;
    }

    NSArray<id<MTLDevice>> *Devices = copyDeviceList();
    *Count = static_cast<uint32_t>([Devices count]);
    return 1;
  }
}

int omp_metal_create_device(uint32_t DeviceIndex, OMPMetalDevice **Device,
                            char *ErrMsg, size_t ErrMsgCapacity) {
  @autoreleasepool {
    clearError(ErrMsg, ErrMsgCapacity);
    if (!Device) {
      setError(ErrMsg, ErrMsgCapacity, @"device output is null");
      return 0;
    }

    NSArray<id<MTLDevice>> *Devices = copyDeviceList();
    if (DeviceIndex >= [Devices count]) {
      setError(ErrMsg, ErrMsgCapacity, @"Metal device index is out of range");
      return 0;
    }

    OMPMetalDevice *Handle = new (std::nothrow) OMPMetalDevice{};
    if (!Handle) {
      setError(ErrMsg, ErrMsgCapacity, @"failed to allocate Metal device handle");
      return 0;
    }

    Handle->Device = [[Devices objectAtIndex:DeviceIndex] retain];
    *Device = Handle;
    return 1;
  }
}

void omp_metal_destroy_device(OMPMetalDevice *Device) {
  @autoreleasepool {
    if (!Device)
      return;

    [Device->Device release];
    delete Device;
  }
}

int omp_metal_query_device(OMPMetalDevice *Device, OMPMetalDeviceInfo *Info,
                           char *ErrMsg, size_t ErrMsgCapacity) {
  @autoreleasepool {
    clearError(ErrMsg, ErrMsgCapacity);
    if (!Device || !Device->Device || !Info) {
      setError(ErrMsg, ErrMsgCapacity, @"invalid Metal device query");
      return 0;
    }

    std::memset(Info, 0, sizeof(*Info));

    NSString *Name = [Device->Device name];
    if (Name)
      std::snprintf(Info->Name, sizeof(Info->Name), "%s", Name.UTF8String);

    if ([Device->Device respondsToSelector:@selector(registryID)])
      Info->RegistryID = static_cast<uint64_t>(Device->Device.registryID);
    if ([Device->Device respondsToSelector:@selector(recommendedMaxWorkingSetSize)])
      Info->RecommendedMaxWorkingSetSize =
          static_cast<uint64_t>(Device->Device.recommendedMaxWorkingSetSize);

    Info->MaxBufferLength = static_cast<uint64_t>(Device->Device.maxBufferLength);
    Info->HasUnifiedMemory = Device->Device.hasUnifiedMemory;
    Info->LowPower = Device->Device.lowPower;
    Info->Removable = Device->Device.removable;
    return 1;
  }
}

int omp_metal_allocate_shared_buffer(OMPMetalDevice *Device, size_t Size,
                                     const char *Label,
                                     OMPMetalBuffer **Buffer, char *ErrMsg,
                                     size_t ErrMsgCapacity) {
  @autoreleasepool {
    clearError(ErrMsg, ErrMsgCapacity);
    if (!Device || !Device->Device || !Buffer) {
      setError(ErrMsg, ErrMsgCapacity, @"invalid Metal buffer allocation request");
      return 0;
    }

    id<MTLBuffer> RawBuffer = [Device->Device
        newBufferWithLength:Size
                    options:MTLResourceStorageModeShared];
    if (!RawBuffer) {
      setError(ErrMsg, ErrMsgCapacity, @"failed to allocate Metal shared buffer");
      return 0;
    }

    if (Label && Label[0] != '\0') {
      NSString *LabelString = [NSString stringWithUTF8String:Label];
      if (LabelString)
        RawBuffer.label = LabelString;
    }

    OMPMetalBuffer *Handle = new (std::nothrow) OMPMetalBuffer{};
    if (!Handle) {
      [RawBuffer release];
      setError(ErrMsg, ErrMsgCapacity, @"failed to allocate Metal buffer handle");
      return 0;
    }

    Handle->Buffer = RawBuffer;
    *Buffer = Handle;
    return 1;
  }
}

void omp_metal_destroy_buffer(OMPMetalBuffer *Buffer) {
  @autoreleasepool {
    if (!Buffer)
      return;

    [Buffer->Buffer release];
    delete Buffer;
  }
}

void *omp_metal_get_buffer_contents(OMPMetalBuffer *Buffer) {
  @autoreleasepool {
    if (!Buffer || !Buffer->Buffer)
      return nullptr;

    return [Buffer->Buffer contents];
  }
}

size_t omp_metal_get_buffer_size(OMPMetalBuffer *Buffer) {
  @autoreleasepool {
    if (!Buffer || !Buffer->Buffer)
      return 0;

    return static_cast<size_t>([Buffer->Buffer length]);
  }
}

int omp_metal_create_library(OMPMetalDevice *Device, const void *Data,
                             size_t Size, OMPMetalLibrary **Library,
                             char *ErrMsg, size_t ErrMsgCapacity) {
  @autoreleasepool {
    clearError(ErrMsg, ErrMsgCapacity);
    if (!Device || !Device->Device || !Data || Size == 0 || !Library) {
      setError(ErrMsg, ErrMsgCapacity, @"invalid Metal library creation request");
      return 0;
    }

    void *DataCopy = std::malloc(Size);
    if (!DataCopy) {
      setError(ErrMsg, ErrMsgCapacity, @"failed to allocate metallib staging copy");
      return 0;
    }
    std::memcpy(DataCopy, Data, Size);

    dispatch_data_t DispatchData =
        dispatch_data_create(DataCopy, Size, dispatch_get_global_queue(
                                               QOS_CLASS_USER_INITIATED, 0),
                             DISPATCH_DATA_DESTRUCTOR_FREE);
    if (!DispatchData) {
      std::free(DataCopy);
      setError(ErrMsg, ErrMsgCapacity, @"failed to create metallib dispatch data");
      return 0;
    }

    NSError *Error = nil;
    id<MTLLibrary> RawLibrary =
        [Device->Device newLibraryWithData:DispatchData error:&Error];
#if !OS_OBJECT_USE_OBJC
    dispatch_release(DispatchData);
#endif
    if (!RawLibrary) {
      setError(ErrMsg, ErrMsgCapacity,
               Error.localizedDescription ?: @"failed to load metallib");
      return 0;
    }

    OMPMetalLibrary *Handle = new (std::nothrow) OMPMetalLibrary{};
    if (!Handle) {
      [RawLibrary release];
      setError(ErrMsg, ErrMsgCapacity, @"failed to allocate Metal library handle");
      return 0;
    }

    Handle->Library = RawLibrary;
    *Library = Handle;
    return 1;
  }
}

void omp_metal_destroy_library(OMPMetalLibrary *Library) {
  @autoreleasepool {
    if (!Library)
      return;

    [Library->Library release];
    delete Library;
  }
}

int omp_metal_library_has_function(OMPMetalLibrary *Library, const char *Name,
                                   bool *HasFunction, char *ErrMsg,
                                   size_t ErrMsgCapacity) {
  @autoreleasepool {
    clearError(ErrMsg, ErrMsgCapacity);
    if (!HasFunction) {
      setError(ErrMsg, ErrMsgCapacity, @"function-presence output is null");
      return 0;
    }

    if (!Library || !Library->Library || !Name || Name[0] == '\0') {
      setError(ErrMsg, ErrMsgCapacity, @"invalid Metal library lookup request");
      return 0;
    }

    NSString *FunctionName = [NSString stringWithUTF8String:Name];
    if (!FunctionName) {
      setError(ErrMsg, ErrMsgCapacity, @"invalid UTF-8 Metal function name");
      return 0;
    }

    *HasFunction = false;
    id<MTLFunction> Function = [Library->Library newFunctionWithName:FunctionName];
    if (!Function)
      return 1;

    *HasFunction = true;
    [Function release];
    return 1;
  }
}

int omp_metal_create_compute_pipeline(OMPMetalDevice *Device,
                                      OMPMetalLibrary *Library,
                                      const char *FunctionName,
                                      OMPMetalPipeline **Pipeline,
                                      char *ErrMsg, size_t ErrMsgCapacity) {
  @autoreleasepool {
    clearError(ErrMsg, ErrMsgCapacity);
    if (!Device || !Device->Device || !Pipeline) {
      setError(ErrMsg, ErrMsgCapacity, @"invalid Metal pipeline creation request");
      return 0;
    }

    id<MTLFunction> Function =
        copyFunction(Library, FunctionName, ErrMsg, ErrMsgCapacity);
    if (!Function)
      return 0;

    NSError *Error = nil;
    id<MTLComputePipelineState> RawPipeline =
        [Device->Device newComputePipelineStateWithFunction:Function
                                                      error:&Error];
    [Function release];
    if (!RawPipeline) {
      setError(ErrMsg, ErrMsgCapacity,
               Error.localizedDescription ?: @"failed to create Metal pipeline");
      return 0;
    }

    OMPMetalPipeline *Handle = new (std::nothrow) OMPMetalPipeline{};
    if (!Handle) {
      [RawPipeline release];
      setError(ErrMsg, ErrMsgCapacity, @"failed to allocate Metal pipeline handle");
      return 0;
    }

    Handle->Pipeline = RawPipeline;
    *Pipeline = Handle;
    return 1;
  }
}

void omp_metal_destroy_pipeline(OMPMetalPipeline *Pipeline) {
  @autoreleasepool {
    if (!Pipeline)
      return;

    [Pipeline->Pipeline release];
    delete Pipeline;
  }
}

int omp_metal_get_pipeline_max_total_threads(OMPMetalPipeline *Pipeline,
                                             uint32_t *MaxThreads,
                                             char *ErrMsg,
                                             size_t ErrMsgCapacity) {
  @autoreleasepool {
    clearError(ErrMsg, ErrMsgCapacity);
    if (!Pipeline || !Pipeline->Pipeline || !MaxThreads) {
      setError(ErrMsg, ErrMsgCapacity, @"invalid Metal pipeline query");
      return 0;
    }

    *MaxThreads = static_cast<uint32_t>(
        Pipeline->Pipeline.maxTotalThreadsPerThreadgroup);
    return 1;
  }
}

int omp_metal_create_command_queue(OMPMetalDevice *Device,
                                   OMPMetalCommandQueue **Queue, char *ErrMsg,
                                   size_t ErrMsgCapacity) {
  @autoreleasepool {
    clearError(ErrMsg, ErrMsgCapacity);
    if (!Device || !Device->Device || !Queue) {
      setError(ErrMsg, ErrMsgCapacity, @"invalid Metal queue creation request");
      return 0;
    }

    id<MTLCommandQueue> RawQueue = [Device->Device newCommandQueue];
    if (!RawQueue) {
      setError(ErrMsg, ErrMsgCapacity, @"failed to create Metal command queue");
      return 0;
    }

    OMPMetalCommandQueue *Handle = new (std::nothrow) OMPMetalCommandQueue{};
    if (!Handle) {
      [RawQueue release];
      setError(ErrMsg, ErrMsgCapacity, @"failed to allocate Metal queue handle");
      return 0;
    }

    Handle->Queue = RawQueue;
    *Queue = Handle;
    return 1;
  }
}

void omp_metal_destroy_command_queue(OMPMetalCommandQueue *Queue) {
  @autoreleasepool {
    if (!Queue)
      return;

    [Queue->Queue release];
    delete Queue;
  }
}

int omp_metal_launch_compute(OMPMetalCommandQueue *Queue,
                             OMPMetalPipeline *Pipeline,
                             OMPMetalBuffer *const *Buffers,
                             const uint64_t *Offsets,
                             const void *const *InlineArgData,
                             const uint64_t *InlineArgSizes,
                             const uint8_t *UseInlineArgs, size_t NumBuffers,
                             uint32_t ThreadgroupMemSize,
                             uint32_t NumThreads[3], uint32_t NumBlocks[3],
                             bool WaitForCompletion,
                             OMPMetalCommandBuffer **OutCommandBuffer,
                             char *ErrMsg, size_t ErrMsgCapacity) {
  @autoreleasepool {
    clearError(ErrMsg, ErrMsgCapacity);
    if (!Queue || !Queue->Queue || !Pipeline || !Pipeline->Pipeline ||
        !NumThreads || !NumBlocks) {
      setError(ErrMsg, ErrMsgCapacity, @"invalid Metal compute launch request");
      return 0;
    }

    if (!WaitForCompletion && !OutCommandBuffer) {
      setError(ErrMsg, ErrMsgCapacity,
               @"async Metal launches require a command buffer output");
      return 0;
    }

    if (NumThreads[0] == 0 || NumThreads[1] == 0 || NumThreads[2] == 0 ||
        NumBlocks[0] == 0 || NumBlocks[1] == 0 || NumBlocks[2] == 0) {
      setError(ErrMsg, ErrMsgCapacity,
               @"Metal launches require non-zero thread and block dimensions");
      return 0;
    }

    id<MTLCommandBuffer> CommandBuffer = [Queue->Queue commandBuffer];
    if (!CommandBuffer) {
      setError(ErrMsg, ErrMsgCapacity, @"failed to create Metal command buffer");
      return 0;
    }

    id<MTLComputeCommandEncoder> Encoder = [CommandBuffer computeCommandEncoder];
    if (!Encoder) {
      setError(ErrMsg, ErrMsgCapacity,
               @"failed to create Metal compute command encoder");
      return 0;
    }

    [Encoder setComputePipelineState:Pipeline->Pipeline];
    if (ThreadgroupMemSize > 0)
      [Encoder setThreadgroupMemoryLength:static_cast<NSUInteger>(ThreadgroupMemSize)
                                  atIndex:0];
    for (size_t I = 0; I < NumBuffers; ++I) {
      if (UseInlineArgs && UseInlineArgs[I]) {
        const void *Data = InlineArgData ? InlineArgData[I] : nullptr;
        uint64_t Size = InlineArgSizes ? InlineArgSizes[I] : 0;
        if (!Data || Size == 0) {
          setError(ErrMsg, ErrMsgCapacity, @"invalid inline Metal kernel argument");
          return 0;
        }

        [Encoder setBytes:Data
                   length:static_cast<NSUInteger>(Size)
                  atIndex:I];
        continue;
      }

      id<MTLBuffer> Buffer = nullptr;
      if (Buffers && Buffers[I] && Buffers[I]->Buffer)
        Buffer = Buffers[I]->Buffer;

      NSUInteger Offset = 0;
      if (Offsets)
        Offset = static_cast<NSUInteger>(Offsets[I]);

      [Encoder setBuffer:Buffer offset:Offset atIndex:I];
    }

    MTLSize ThreadsPerThreadgroup =
        MTLSizeMake(NumThreads[0], NumThreads[1], NumThreads[2]);
    MTLSize Threadgroups =
        MTLSizeMake(NumBlocks[0], NumBlocks[1], NumBlocks[2]);
    [Encoder dispatchThreadgroups:Threadgroups
            threadsPerThreadgroup:ThreadsPerThreadgroup];
    [Encoder endEncoding];

    OMPMetalCommandBuffer *Wrapper = nullptr;
    if (OutCommandBuffer) {
      Wrapper = new (std::nothrow) OMPMetalCommandBuffer{};
      if (!Wrapper) {
        setError(ErrMsg, ErrMsgCapacity,
                 @"failed to allocate Metal command buffer handle");
        return 0;
      }
      Wrapper->Buffer = [CommandBuffer retain];
      Wrapper->RefCount.store(1, std::memory_order_relaxed);
      *OutCommandBuffer = Wrapper;
    }

    [CommandBuffer commit];

    if (WaitForCompletion) {
      [CommandBuffer waitUntilCompleted];
      if (CommandBuffer.status == MTLCommandBufferStatusError) {
        NSError *Error = CommandBuffer.error;
        setError(ErrMsg, ErrMsgCapacity,
                 Error.localizedDescription ?: @"Metal compute launch failed");
        if (Wrapper) {
          [Wrapper->Buffer release];
          delete Wrapper;
          *OutCommandBuffer = nullptr;
        }
        return 0;
      }
    }

    return 1;
  }
}

void omp_metal_command_buffer_retain(OMPMetalCommandBuffer *CommandBuffer) {
  if (!CommandBuffer)
    return;
  CommandBuffer->RefCount.fetch_add(1, std::memory_order_relaxed);
}

void omp_metal_command_buffer_release(OMPMetalCommandBuffer *CommandBuffer) {
  if (!CommandBuffer)
    return;
  if (CommandBuffer->RefCount.fetch_sub(1, std::memory_order_acq_rel) != 1)
    return;

  @autoreleasepool {
    [CommandBuffer->Buffer release];
    delete CommandBuffer;
  }
}

int omp_metal_command_buffer_wait(OMPMetalCommandBuffer *CommandBuffer,
                                  char *ErrMsg, size_t ErrMsgCapacity) {
  @autoreleasepool {
    clearError(ErrMsg, ErrMsgCapacity);
    if (!CommandBuffer || !CommandBuffer->Buffer) {
      setError(ErrMsg, ErrMsgCapacity,
               @"invalid Metal command buffer wait request");
      return 0;
    }

    [CommandBuffer->Buffer waitUntilCompleted];
    if (CommandBuffer->Buffer.status == MTLCommandBufferStatusError) {
      NSError *Error = CommandBuffer->Buffer.error;
      setError(ErrMsg, ErrMsgCapacity,
               Error.localizedDescription ?: @"Metal command buffer failed");
      return 0;
    }

    return 1;
  }
}

int omp_metal_command_buffer_is_complete(OMPMetalCommandBuffer *CommandBuffer,
                                         bool *IsComplete, char *ErrMsg,
                                         size_t ErrMsgCapacity) {
  @autoreleasepool {
    clearError(ErrMsg, ErrMsgCapacity);
    if (!CommandBuffer || !CommandBuffer->Buffer || !IsComplete) {
      setError(ErrMsg, ErrMsgCapacity,
               @"invalid Metal command buffer query request");
      return 0;
    }

    MTLCommandBufferStatus Status = CommandBuffer->Buffer.status;
    if (Status == MTLCommandBufferStatusError) {
      NSError *Error = CommandBuffer->Buffer.error;
      setError(ErrMsg, ErrMsgCapacity,
               Error.localizedDescription ?: @"Metal command buffer failed");
      return 0;
    }

    *IsComplete = (Status == MTLCommandBufferStatusCompleted);
    return 1;
  }
}
