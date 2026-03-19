//===- MetalRuntime.h - Apple Metal runtime bridge --------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef OPENMP_LIBOMPTARGET_PLUGINS_NEXTGEN_METAL_RUNTIME_H
#define OPENMP_LIBOMPTARGET_PLUGINS_NEXTGEN_METAL_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct OMPMetalDevice OMPMetalDevice;
typedef struct OMPMetalBuffer OMPMetalBuffer;
typedef struct OMPMetalLibrary OMPMetalLibrary;
typedef struct OMPMetalPipeline OMPMetalPipeline;
typedef struct OMPMetalCommandQueue OMPMetalCommandQueue;
typedef struct OMPMetalCommandBuffer OMPMetalCommandBuffer;

enum { OMP_METAL_DEVICE_NAME_MAX = 256 };

typedef struct OMPMetalDeviceInfo {
  uint64_t RegistryID;
  uint64_t RecommendedMaxWorkingSetSize;
  uint64_t MaxBufferLength;
  bool HasUnifiedMemory;
  bool LowPower;
  bool Removable;
  char Name[OMP_METAL_DEVICE_NAME_MAX];
} OMPMetalDeviceInfo;

int omp_metal_get_device_count(uint32_t *Count, char *ErrMsg,
                               size_t ErrMsgCapacity);
int omp_metal_create_device(uint32_t DeviceIndex, OMPMetalDevice **Device,
                            char *ErrMsg, size_t ErrMsgCapacity);
void omp_metal_destroy_device(OMPMetalDevice *Device);
int omp_metal_query_device(OMPMetalDevice *Device, OMPMetalDeviceInfo *Info,
                           char *ErrMsg, size_t ErrMsgCapacity);

int omp_metal_allocate_shared_buffer(OMPMetalDevice *Device, size_t Size,
                                     const char *Label, OMPMetalBuffer **Buffer,
                                     char *ErrMsg, size_t ErrMsgCapacity);
void omp_metal_destroy_buffer(OMPMetalBuffer *Buffer);
void *omp_metal_get_buffer_contents(OMPMetalBuffer *Buffer);
size_t omp_metal_get_buffer_size(OMPMetalBuffer *Buffer);

int omp_metal_create_library(OMPMetalDevice *Device, const void *Data,
                             size_t Size, OMPMetalLibrary **Library,
                             char *ErrMsg, size_t ErrMsgCapacity);
void omp_metal_destroy_library(OMPMetalLibrary *Library);
int omp_metal_library_has_function(OMPMetalLibrary *Library, const char *Name,
                                   bool *HasFunction, char *ErrMsg,
                                   size_t ErrMsgCapacity);

int omp_metal_create_compute_pipeline(OMPMetalDevice *Device,
                                      OMPMetalLibrary *Library,
                                      const char *FunctionName,
                                      OMPMetalPipeline **Pipeline, char *ErrMsg,
                                      size_t ErrMsgCapacity);
void omp_metal_destroy_pipeline(OMPMetalPipeline *Pipeline);
int omp_metal_get_pipeline_max_total_threads(OMPMetalPipeline *Pipeline,
                                             uint32_t *MaxThreads, char *ErrMsg,
                                             size_t ErrMsgCapacity);

int omp_metal_create_command_queue(OMPMetalDevice *Device,
                                   OMPMetalCommandQueue **Queue, char *ErrMsg,
                                   size_t ErrMsgCapacity);
void omp_metal_destroy_command_queue(OMPMetalCommandQueue *Queue);

int omp_metal_launch_compute(
    OMPMetalCommandQueue *Queue, OMPMetalPipeline *Pipeline,
    OMPMetalBuffer *const *Buffers, const uint64_t *Offsets,
    const void *const *InlineArgData, const uint64_t *InlineArgSizes,
    const uint8_t *UseInlineArgs, size_t NumBuffers,
    uint32_t ThreadgroupMemSize, uint32_t NumThreads[3], uint32_t NumBlocks[3],
    bool WaitForCompletion, OMPMetalCommandBuffer **OutCommandBuffer,
    char *ErrMsg, size_t ErrMsgCapacity);

void omp_metal_command_buffer_retain(OMPMetalCommandBuffer *CommandBuffer);
void omp_metal_command_buffer_release(OMPMetalCommandBuffer *CommandBuffer);
int omp_metal_command_buffer_wait(OMPMetalCommandBuffer *CommandBuffer,
                                  char *ErrMsg, size_t ErrMsgCapacity);
int omp_metal_command_buffer_is_complete(OMPMetalCommandBuffer *CommandBuffer,
                                         bool *IsComplete, char *ErrMsg,
                                         size_t ErrMsgCapacity);

#ifdef __cplusplus
}
#endif

#endif
