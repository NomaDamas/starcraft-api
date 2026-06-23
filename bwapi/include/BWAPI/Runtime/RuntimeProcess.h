#pragma once

#include <BWAPI/Runtime/RuntimeBackend.h>

#include <string>
#include <vector>

namespace BWAPI::Runtime
{
  struct RuntimeProcessOpenResult
  {
    bool opened = false;
    int processId = 0;
    std::string executablePath;
    std::string reason;
  };

  struct RuntimeProcessStateResult
  {
    bool inspected = false;
    bool suspended = false;
    unsigned status = 0;
    int threadCount = -1;
    std::string reason;
  };

  struct RuntimeProcessCommandLineResult
  {
    bool inspected = false;
    std::vector<std::string> arguments;
    std::string commandLine;
    std::string reason;
  };

  bool runtimeProcessExists(int processId);
  std::string runtimeProcessExecutablePath(int processId);
  RuntimeProcessStateResult inspectRuntimeProcessState(int processId);
  RuntimeProcessCommandLineResult inspectRuntimeProcessCommandLine(int processId);
  RuntimeProcessOpenResult openRuntimeProcess(const RuntimeEnvironment& environment);
}
