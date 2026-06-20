#pragma once

#include <BWAPI/Runtime/RuntimeBackend.h>

#include <string>

namespace BWAPI::Runtime
{
  struct RuntimeProcessOpenResult
  {
    bool opened = false;
    int processId = 0;
    std::string executablePath;
    std::string reason;
  };

  bool runtimeProcessExists(int processId);
  std::string runtimeProcessExecutablePath(int processId);
  RuntimeProcessOpenResult openRuntimeProcess(const RuntimeEnvironment& environment);
}
