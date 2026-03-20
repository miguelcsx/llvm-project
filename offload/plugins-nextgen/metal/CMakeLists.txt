if(NOT APPLE)
  message(FATAL_ERROR "Metal plugin is only supported on Apple hosts")
endif()

if(NOT LIBOMPTARGET_DEP_METAL_FOUND)
  message(FATAL_ERROR "Metal plugin requested, but required Apple frameworks were not found")
endif()

include(CheckLanguage)
check_language(OBJCXX)
if(NOT CMAKE_OBJCXX_COMPILER)
  message(FATAL_ERROR "Metal plugin requested, but no Objective-C++ compiler was found")
endif()
enable_language(OBJCXX)

add_target_library(omptarget.rtl.metal METAL)

target_sources(omptarget.rtl.metal PRIVATE
  src/MetalMetadata.cpp
  src/MetalRuntime.mm
  src/rtl.cpp
)

target_include_directories(omptarget.rtl.metal PRIVATE
  ${LIBOMPTARGET_INCLUDE_DIR}
  ${LIBOMPTARGET_LLVM_INCLUDE_DIRS}
)

target_link_libraries(omptarget.rtl.metal PRIVATE
  ${LIBOMPTARGET_DEP_METAL_FRAMEWORK}
  ${LIBOMPTARGET_DEP_FOUNDATION_FRAMEWORK}
)

set(LIBOMPTARGET_SYSTEM_TARGETS
    "${LIBOMPTARGET_SYSTEM_TARGETS} arm64-apple-darwin" PARENT_SCOPE)
