# Gather all .cpp sources in this directory tree
file(GLOB_RECURSE LLVM_ADVISOR_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp
)

set(LLVM_ADVISOR_PYTHON_PACKAGE_DIR
  "${LLVM_MAIN_SRC_DIR}/tools/llvm-advisor/tools"
)
set(LLVM_ADVISOR_NATIVE_LIBRARY_BASENAME "libLLVMAdvisorNative${CMAKE_SHARED_LIBRARY_SUFFIX}")

configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/AdvisorRuntimeConfig.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/AdvisorRuntimeConfig.h
  @ONLY
)

# Define the executable target
add_llvm_tool(llvm-advisor
  ${LLVM_ADVISOR_SOURCES}
)

add_library(LLVMAdvisorNative SHARED
  Core/CapabilityRunner.cpp
  Core/CapabilityRunnerAnalysis.cpp
  Core/CapabilityRunnerCompare.cpp
  Core/CapabilityRunnerManifest.cpp
  Core/CapabilityRunnerRequests.cpp
  Core/CapabilityRunnerSignals.cpp
  Core/CapabilityRunnerSummaries.cpp
  Core/CapabilityRuntime.cpp
)

set_target_properties(LLVMAdvisorNative PROPERTIES
  OUTPUT_NAME LLVMAdvisorNative
  LIBRARY_OUTPUT_DIRECTORY "${LLVM_RUNTIME_OUTPUT_INTDIR}"
)

set(LLVM_ADVISOR_EXTERNAL_SOURCES
  ${LLVM_MAIN_SRC_DIR}/tools/llvm-cov/CoverageExporterJson.cpp
  ${LLVM_MAIN_SRC_DIR}/tools/llvm-cov/CoverageFilters.cpp
  ${LLVM_MAIN_SRC_DIR}/tools/llvm-cov/CoverageReport.cpp
  ${LLVM_MAIN_SRC_DIR}/tools/llvm-cov/CoverageSummaryInfo.cpp
)

target_sources(llvm-advisor PRIVATE ${LLVM_ADVISOR_EXTERNAL_SOURCES})

# Link required LLVM libraries
target_link_libraries(llvm-advisor PRIVATE
  LLVMSupport
  LLVMDebugInfoDWARF
  LLVMObject
  LLVMCore
  LLVMIRReader
  LLVMBitWriter
  LLVMRemarks
  LLVMProfileData
  clangOptions
  clangAST
  clangBasic
  clangCodeGen
  clangDriver
  clangFrontend
  clangSerialization
  clangTooling
  clangStaticAnalyzerFrontend
  clangStaticAnalyzerCheckers
  clangStaticAnalyzerCore
)

if(LLVM_NATIVE_ARCH STREQUAL "X86")
  target_link_libraries(llvm-advisor PRIVATE
    LLVMX86CodeGen
    LLVMX86AsmParser
    LLVMX86Desc
    LLVMX86Info
    LLVMX86Disassembler
  )
elseif(LLVM_NATIVE_ARCH STREQUAL "AArch64")
  target_link_libraries(llvm-advisor PRIVATE
    LLVMAArch64CodeGen
    LLVMAArch64AsmParser
    LLVMAArch64Desc
    LLVMAArch64Info
    LLVMAArch64Disassembler
  )
endif()

target_link_libraries(LLVMAdvisorNative PRIVATE
  LLVMSupport
  LLVMCore
  LLVMIRReader
  LLVMRemarks
)

# Set include directories
set(LLVM_ADVISOR_CLANG_INCLUDE_DIRS
  ${LLVM_BINARY_DIR}/tools/clang/include
)

if(LLVM_EXTERNAL_CLANG_SOURCE_DIR)
  list(APPEND LLVM_ADVISOR_CLANG_INCLUDE_DIRS
    ${LLVM_EXTERNAL_CLANG_SOURCE_DIR}/include
  )
endif()

target_include_directories(llvm-advisor PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_BINARY_DIR}
  ${LLVM_MAIN_SRC_DIR}/tools/llvm-cov
  ${LLVM_ADVISOR_CLANG_INCLUDE_DIRS}
)

target_include_directories(LLVMAdvisorNative PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_BINARY_DIR}
)

# Install the Python webview module alongside the binary
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../tools/
  DESTINATION share/llvm-advisor/tools
  FILES_MATCHING
  PATTERN "*.py"
  PATTERN "*.html"
  PATTERN "*.css"
  PATTERN "*.js"
  PATTERN "*.md"
)

install(TARGETS LLVMAdvisorNative
  LIBRARY DESTINATION bin
  RUNTIME DESTINATION bin
  COMPONENT llvm-advisor)
