#pragma once

#include <BWAPI/Runtime/RuntimeBackend.h>

#include <string>
#include <vector>

namespace BWAPI::Runtime
{
  struct RuntimeResidentLoadRequest
  {
    RuntimeEnvironment environment;
    std::string adapterPath;
  };

  struct RuntimeResidentLoadPlan
  {
    bool valid = false;
    std::string loaderMode;
    std::vector<std::string> errors;
  };

  RuntimeResidentLoadPlan planRuntimeResidentAdapterLoad(
    const RuntimeResidentLoadRequest& request);
}
