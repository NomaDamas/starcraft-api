#pragma once

#include <BWAPI/Runtime/RuntimeExecutor.h>

#include <cstddef>
#include <string>
#include <vector>

namespace BWAPI::Runtime
{
  struct RuntimeImplementationGap
  {
    std::string category;
    std::string id;
    std::string detail;
  };

  struct RuntimeImplementationGapCategoryCount
  {
    std::string category;
    std::size_t count = 0;
  };

  std::vector<RuntimeImplementationGap> collectRuntimeImplementationGaps(
    const RuntimeProbeResult& probe,
    const RuntimeContract& contract,
    const RuntimeExecutorPreflightResult& preflight);

  std::vector<RuntimeImplementationGapCategoryCount> summarizeRuntimeImplementationGapsByCategory(
    const std::vector<RuntimeImplementationGap>& gaps);

  std::size_t countRuntimeImplementationGapsByCategory(
    const std::vector<RuntimeImplementationGap>& gaps,
    const std::string& category);
}
