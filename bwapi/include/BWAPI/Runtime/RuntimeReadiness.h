#pragma once

#include <BWAPI/Runtime/RuntimeExecutor.h>

#include <string>
#include <vector>

namespace BWAPI::Runtime
{
  enum class RuntimeReadinessSeverity
  {
    Info,
    Warning,
    Error
  };

  struct RuntimeReadinessCheck
  {
    std::string id;
    bool passed = false;
    RuntimeReadinessSeverity severity = RuntimeReadinessSeverity::Error;
    std::string detail;
  };

  struct RuntimeReadinessReport
  {
    bool productionReady = false;
    std::vector<RuntimeReadinessCheck> checks;
  };

  const char* toString(RuntimeReadinessSeverity severity);
  RuntimeReadinessReport evaluateProductionReadiness(
    const RuntimeProbeResult& probe,
    const RuntimeContract& contract,
    const RuntimeExecutorPreflightResult& executorPreflight);
  std::vector<RuntimeReadinessCheck> blockingReadinessGaps(const RuntimeReadinessReport& report);
}
