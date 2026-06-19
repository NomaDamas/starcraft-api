#pragma once

#include <BWAPI/Runtime/RuntimeContract.h>

#include <string>
#include <vector>

namespace BWAPI::Runtime
{
  struct RuntimeExecutorPreflightResult
  {
    bool contractValid = false;
    bool targetLocated = false;
    bool executorAvailable = false;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
  };

  RuntimeExecutorPreflightResult preflightRuntimeExecutor(
    const RuntimeEnvironment& environment,
    const RuntimeContract& contract);
}
