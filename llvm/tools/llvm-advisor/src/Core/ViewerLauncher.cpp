//===---------------- ViewerLauncher.cpp - LLVM Advisor ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "ViewerLauncher.h"
#include "AdvisorRuntimeConfig.h"
#include "../Utils/ProcessRunner.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"
#include <cstdlib>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace llvm;
using namespace llvm::advisor;

namespace {

constexpr unsigned LaunchTimeoutMs = 5000;
constexpr unsigned ProbeIntervalMs = 200;

struct ViewerRuntimeRecord {
  std::string Host;
  int Port = 0;
  std::string DataDir;
};

std::string quotePythonString(StringRef Value) {
  std::string Quoted;
  Quoted.reserve(Value.size() + 8);
  Quoted.push_back('\'');

  for (char C : Value) {
    switch (C) {
    case '\\':
      Quoted += "\\\\";
      break;
    case '\'':
      Quoted += "\\'";
      break;
    case '\n':
      Quoted += "\\n";
      break;
    case '\r':
      Quoted += "\\r";
      break;
    case '\t':
      Quoted += "\\t";
      break;
    default:
      Quoted.push_back(C);
      break;
    }
  }

  Quoted.push_back('\'');
  return Quoted;
}

std::string buildLaunchBootstrap(StringRef PackageRoot, StringRef OutputDir,
                                 int Port) {
  return ("import runpy, sys; "
          "sys.path.insert(0, " +
          quotePythonString(PackageRoot) +
          "); "
          "sys.argv = ['webserver.server', '--data-dir', " +
          quotePythonString(OutputDir) + ", '--port', '" + std::to_string(Port) +
          "']; "
          "runpy.run_module('webserver.server', run_name='__main__')");
}

std::vector<std::string> buildViewerEnvironment(StringRef ExecutablePath) {
  std::vector<std::string> Environment;

  if (const char *CurrentPath = std::getenv("PATH"))
    Environment.emplace_back(std::string("PATH=") + CurrentPath);

  if (const char *PythonPath = std::getenv("PYTHONPATH"))
    Environment.emplace_back(std::string("PYTHONPATH=") + PythonPath);

  Environment.emplace_back("LLVM_ADVISOR_EXECUTABLE=" + ExecutablePath.str());

  SmallString<256> NativeLibraryPath(sys::path::parent_path(ExecutablePath));
  sys::path::append(NativeLibraryPath, LLVM_ADVISOR_NATIVE_LIBRARY_BASENAME);
  Environment.emplace_back("LLVM_ADVISOR_NATIVE_LIBRARY=" +
                           std::string(NativeLibraryPath.str()));
  return Environment;
}

std::string buildHealthProbeScript(int Port) {
  return ("import json, sys, urllib.request; "
          "response = urllib.request.urlopen("
          "'http://127.0.0.1:" +
          std::to_string(Port) +
          "/api/health', timeout=1); "
          "payload = json.load(response); "
          "sys.exit(0 if payload.get('success') else 1)");
}

std::string buildStoreProbeScript(StringRef Host, int Port,
                                  StringRef ExpectedOutputDir) {
  return ("import json, sys, urllib.request; "
          "response = urllib.request.urlopen('http://" +
          Host.str() + ":" + std::to_string(Port) +
          "/api/health', timeout=1); "
          "payload = json.load(response); "
          "data = payload.get('data') or {}; "
          "sys.exit(0 if payload.get('success') and data.get('data_dir') == " +
          quotePythonString(ExpectedOutputDir) + " else 1)");
}

std::string readLogFile(StringRef LogPath) {
  auto BufferOrErr = MemoryBuffer::getFile(LogPath);
  if (!BufferOrErr)
    return {};

  StringRef Contents = (*BufferOrErr)->getBuffer();
  constexpr size_t MaxLogBytes = 8192;
  if (Contents.size() > MaxLogBytes)
    Contents = Contents.take_back(MaxLogBytes);

  return Contents.str();
}

Error waitForServerReady(StringRef PythonExecutable, int Port,
                         const sys::ProcessInfo &Process,
                         StringRef LogPath) {
  SmallVector<std::string, 8> ProbeArgs = {"-c", buildHealthProbeScript(Port)};

  const auto Deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(LaunchTimeoutMs);

  while (std::chrono::steady_clock::now() < Deadline) {
    std::string WaitError;
    auto WaitResult = sys::Wait(Process, 0, &WaitError, nullptr, true);
    if (WaitResult.Pid == Process.Pid) {
      std::string Message = "Web server exited before becoming ready";
      if (!WaitError.empty())
        Message += ": " + WaitError;

      std::string LogOutput = readLogFile(LogPath);
      if (!LogOutput.empty())
        Message += "\nViewer log:\n" + LogOutput;

      return createStringError(std::make_error_code(std::errc::io_error),
                               Message);
    }

    auto ProbeResult = ProcessRunner::run(PythonExecutable, ProbeArgs, 2);
    if (ProbeResult && ProbeResult->exitCode == 0)
      return Error::success();

    std::this_thread::sleep_for(std::chrono::milliseconds(ProbeIntervalMs));
  }

  std::string Message = "Timed out waiting for web server readiness";
  std::string LogOutput = readLogFile(LogPath);
  if (!LogOutput.empty())
    Message += "\nViewer log:\n" + LogOutput;

  return createStringError(std::make_error_code(std::errc::timed_out), Message);
}

std::string getRuntimeMetadataPath(StringRef OutputDir) {
  SmallString<256> RuntimePath(OutputDir);
  sys::path::append(RuntimePath, ".llvm-advisor-store", "runtime",
                    "server.json");
  return std::string(RuntimePath.str());
}

Expected<ViewerRuntimeRecord> readRuntimeMetadata(StringRef OutputDir) {
  auto BufferOrErr = MemoryBuffer::getFile(getRuntimeMetadataPath(OutputDir));
  if (!BufferOrErr) {
    return createStringError(
        std::make_error_code(std::errc::no_such_file_or_directory),
        "Viewer runtime metadata not found");
  }

  auto JsonOrErr = json::parse((*BufferOrErr)->getBuffer());
  if (!JsonOrErr)
    return createStringError(std::make_error_code(std::errc::invalid_argument),
                             "Failed to parse viewer runtime metadata");

  auto *Object = JsonOrErr->getAsObject();
  if (!Object)
    return createStringError(std::make_error_code(std::errc::invalid_argument),
                             "Viewer runtime metadata is not a JSON object");

  ViewerRuntimeRecord Record;
  if (auto Host = Object->getString("host"))
    Record.Host = std::string(*Host);
  if (auto Port = Object->getInteger("port"))
    Record.Port = static_cast<int>(*Port);
  if (auto DataDir = Object->getString("data_dir"))
    Record.DataDir = std::string(*DataDir);

  if (Record.Host.empty() || Record.Port <= 0 || Record.DataDir.empty())
    return createStringError(std::make_error_code(std::errc::invalid_argument),
                             "Viewer runtime metadata is incomplete");

  return Record;
}

bool probeExistingViewer(StringRef PythonExecutable, StringRef Host, int Port,
                         StringRef OutputDir) {
  SmallVector<std::string, 8> ProbeArgs = {
      "-c", buildStoreProbeScript(Host, Port, OutputDir)};
  auto ProbeResult = ProcessRunner::run(PythonExecutable, ProbeArgs, 2);
  return ProbeResult && ProbeResult->exitCode == 0;
}

Expected<int> findAvailablePort(int PreferredPort) {
#ifdef _WIN32
  WSADATA WsaData;
  if (WSAStartup(MAKEWORD(2, 2), &WsaData) != 0)
    return createStringError(std::make_error_code(std::errc::io_error),
                             "WSAStartup failed while selecting viewer port");
#endif

  auto TryBindPort = [](int CandidatePort) -> Expected<int> {
    int SocketFd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (SocketFd < 0)
      return createStringError(std::make_error_code(std::errc::io_error),
                               "Failed to create socket for viewer port selection");

    int ReuseAddr = 1;
    ::setsockopt(SocketFd, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char *>(&ReuseAddr), sizeof(ReuseAddr));

    sockaddr_in Address{};
    Address.sin_family = AF_INET;
    Address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    Address.sin_port = htons(static_cast<uint16_t>(CandidatePort));

    if (::bind(SocketFd, reinterpret_cast<sockaddr *>(&Address),
               sizeof(Address)) != 0) {
#ifdef _WIN32
      ::closesocket(SocketFd);
#else
      ::close(SocketFd);
#endif
      return createStringError(std::make_error_code(std::errc::address_in_use),
                               "Requested port is not available");
    }

    if (CandidatePort == 0) {
      socklen_t AddressLength = sizeof(Address);
      if (::getsockname(SocketFd, reinterpret_cast<sockaddr *>(&Address),
                        &AddressLength) != 0) {
#ifdef _WIN32
        ::closesocket(SocketFd);
#else
        ::close(SocketFd);
#endif
        return createStringError(std::make_error_code(std::errc::io_error),
                                 "Failed to inspect selected viewer port");
      }
      CandidatePort = ntohs(Address.sin_port);
    }

#ifdef _WIN32
    ::closesocket(SocketFd);
#else
    ::close(SocketFd);
#endif
    return CandidatePort;
  };

  if (PreferredPort > 0) {
    if (auto PortOrErr = TryBindPort(PreferredPort)) {
#ifdef _WIN32
      WSACleanup();
#endif
      return *PortOrErr;
    }
  }

  auto FallbackPortOrErr = TryBindPort(0);
#ifdef _WIN32
  WSACleanup();
#endif
  if (!FallbackPortOrErr)
    return FallbackPortOrErr.takeError();
  return *FallbackPortOrErr;
}

} // namespace

Expected<std::string> ViewerLauncher::findPythonExecutable() {
  std::vector<std::string> Candidates = {"python3", "python"};

  for (const auto &Candidate : Candidates) {
    if (auto Path = sys::findProgramByName(Candidate))
      return *Path;
  }

  return createStringError(
      std::make_error_code(std::errc::no_such_file_or_directory),
      "Python executable not found. Please install Python 3.");
}

Expected<std::string> ViewerLauncher::getPythonPackageRoot() {
  SmallString<256> PackageRoot(LLVM_ADVISOR_PYTHON_PACKAGE_DIR);
  if (sys::fs::exists(PackageRoot))
    return std::string(PackageRoot.str());

  auto MainExecutable = sys::fs::getMainExecutable(nullptr, nullptr);
  if (MainExecutable.empty()) {
    return createStringError(
        std::make_error_code(std::errc::no_such_file_or_directory),
        "Cannot determine executable path");
  }

  SmallString<256> InstallRoot(sys::path::parent_path(MainExecutable));
  sys::path::append(InstallRoot, "..", "share", "llvm-advisor", "tools");
  sys::path::remove_dots(InstallRoot, /*remove_dot_dot=*/true);

  if (sys::fs::exists(InstallRoot))
    return std::string(InstallRoot.str());

  return createStringError(
      std::make_error_code(std::errc::no_such_file_or_directory),
      "Bundled viewer package not found. Expected llvm-advisor Python resources");
}

Expected<int> ViewerLauncher::launch(const std::string &OutputDir, int Port) {
  auto PythonOrErr = findPythonExecutable();
  if (!PythonOrErr)
    return PythonOrErr.takeError();

  auto PackageRootOrErr = getPythonPackageRoot();
  if (!PackageRootOrErr)
    return PackageRootOrErr.takeError();

  if (!sys::fs::exists(OutputDir))
    return createStringError(
        std::make_error_code(std::errc::no_such_file_or_directory),
        "Output directory does not exist: " + OutputDir);

  if (auto RuntimeRecordOrErr = readRuntimeMetadata(OutputDir)) {
    if (probeExistingViewer(*PythonOrErr, RuntimeRecordOrErr->Host,
                            RuntimeRecordOrErr->Port, OutputDir)) {
      outs() << "View available at http://" << RuntimeRecordOrErr->Host << ":"
             << RuntimeRecordOrErr->Port << "\n";
      return 0;
    }
  } else {
    consumeError(RuntimeRecordOrErr.takeError());
  }

  auto SelectedPortOrErr = findAvailablePort(Port);
  if (!SelectedPortOrErr)
    return SelectedPortOrErr.takeError();
  int SelectedPort = *SelectedPortOrErr;

  std::vector<std::string> OwnedArgs = {
      *PythonOrErr, "-c",
      buildLaunchBootstrap(*PackageRootOrErr, OutputDir, SelectedPort)};
  auto MainExecutable = sys::fs::getMainExecutable(nullptr, nullptr);
  auto OwnedEnv = buildViewerEnvironment(MainExecutable);
  SmallVector<StringRef, 8> Args;
  Args.reserve(OwnedArgs.size());
  for (const auto &Arg : OwnedArgs)
    Args.push_back(Arg);
  SmallVector<StringRef, 8> Env;
  Env.reserve(OwnedEnv.size());
  for (const auto &Entry : OwnedEnv)
    Env.push_back(Entry);

  SmallString<128> LogPath;
  if (auto TempEC =
          sys::fs::createTemporaryFile("llvm-advisor-view", "log", LogPath))
    return createStringError(
        TempEC, "Failed to create temporary log file for detached viewer");

  std::optional<StringRef> Redirects[] = {
      StringRef(""), StringRef(LogPath), StringRef(LogPath)};

  std::string LaunchError;
  bool ExecutionFailed = false;
  auto Process = sys::ExecuteNoWait(*PythonOrErr, Args, Env, Redirects,
                                    0, &LaunchError, &ExecutionFailed, nullptr,
                                    true);
  if (ExecutionFailed || !Process.Pid) {
    std::string Message = "Failed to launch detached web server";
    if (!LaunchError.empty())
      Message += ": " + LaunchError;
    return createStringError(std::make_error_code(std::errc::io_error),
                             Message);
  }

  if (auto ReadyErr =
          waitForServerReady(*PythonOrErr, SelectedPort, Process, LogPath))
    return std::move(ReadyErr);

  outs() << "View available at http://localhost:" << SelectedPort << "\n";
  return 0;
}
