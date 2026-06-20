#include <BWAPI/Runtime/RuntimeReadiness.h>
#include <BWAPI/Runtime/RuntimeCommandSurface.h>

#include <algorithm>
#include <sstream>
#include <utility>

namespace BWAPI::Runtime
{
  namespace
  {
    void addCheck(
      RuntimeReadinessReport& report,
      std::string id,
      bool passed,
      RuntimeReadinessSeverity severity,
      std::string detail)
    {
      RuntimeReadinessCheck check;
      check.id = std::move(id);
      check.passed = passed;
      check.severity = severity;
      check.detail = std::move(detail);
      report.checks.push_back(std::move(check));
    }

    std::string join(const std::vector<std::string>& values)
    {
      std::ostringstream out;
      for (std::size_t i = 0; i < values.size(); ++i)
      {
        if (i > 0)
          out << "; ";
        out << values[i];
      }
      return out.str();
    }

    bool contains(const std::vector<std::string>& values, const std::string& value)
    {
      return std::find(values.begin(), values.end(), value) != values.end();
    }

    bool requiresCapability(const RuntimeContract& contract, Capability capability)
    {
      return std::find(
        contract.requiredCapabilities.begin(),
        contract.requiredCapabilities.end(),
        capability) != contract.requiredCapabilities.end();
    }

    std::vector<std::string> missingCapabilities(
      const RuntimeProbeResult& probe,
      const RuntimeContract& contract)
    {
      std::vector<std::string> missing;
      for (Capability capability : contract.requiredCapabilities)
      {
        if (!hasCapability(probe, capability))
          missing.push_back(toString(capability));
      }
      return missing;
    }

    std::vector<std::string> missingEntries(
      const std::vector<std::string>& required,
      const std::vector<std::string>& actual)
    {
      std::vector<std::string> missing;
      for (const std::string& entry : required)
      {
        if (!contains(actual, entry))
          missing.push_back(entry);
      }
      return missing;
    }

    std::string countDetail(int actual, int required, const char* label)
    {
      std::ostringstream out;
      out << label << " implemented=" << actual << " required=" << required;
      return out.str();
    }

    bool hasBlockingFailures(const RuntimeReadinessReport& report)
    {
      return std::any_of(
        report.checks.begin(),
        report.checks.end(),
        [](const RuntimeReadinessCheck& check)
        {
          return check.severity == RuntimeReadinessSeverity::Error && !check.passed;
        });
    }
  }

  const char* toString(RuntimeReadinessSeverity severity)
  {
    switch (severity)
    {
    case RuntimeReadinessSeverity::Info: return "info";
    case RuntimeReadinessSeverity::Warning: return "warning";
    case RuntimeReadinessSeverity::Error: return "error";
    }
    return "error";
  }

  RuntimeReadinessReport evaluateProductionReadiness(
    const RuntimeProbeResult& probe,
    const RuntimeContract& contract,
    const RuntimeExecutorPreflightResult& executorPreflight)
  {
    RuntimeReadinessReport report;

    addCheck(
      report,
      "backend-supported",
      probe.supported,
      RuntimeReadinessSeverity::Error,
      probe.supported ? "runtime backend claims support" : probe.reason);

    const ContractValidationResult validation = validateRuntimeContract(contract);
    addCheck(
      report,
      "contract-valid",
      validation.valid,
      RuntimeReadinessSeverity::Error,
      validation.valid ? "runtime contract has resolved required bindings and layouts" : join(validation.errors));

    addCheck(
      report,
      "api-surface-complete",
      probe.implementedApiSurfaceMethods >= contract.requiredApiSurfaceMethods,
      RuntimeReadinessSeverity::Error,
      countDetail(probe.implementedApiSurfaceMethods, contract.requiredApiSurfaceMethods, "BWAPI abstract methods"));

    addCheck(
      report,
      "command-surface-count-complete",
      probe.implementedCommandSurfaceEntries >= contract.requiredCommandSurfaceEntries,
      RuntimeReadinessSeverity::Error,
      countDetail(probe.implementedCommandSurfaceEntries, contract.requiredCommandSurfaceEntries, "BWAPI command/action entries"));

    const RuntimeCommandSurface commandSurface = makeBWAPICommandSurface();
    const std::vector<std::string> missingUnitCommands =
      missingEntries(commandSurface.unitCommands, probe.implementedUnitCommands);
    addCheck(
      report,
      "unit-command-surface-complete",
      missingUnitCommands.empty(),
      RuntimeReadinessSeverity::Error,
      missingUnitCommands.empty() ? "all BWAPI unit commands are implemented by name" : join(missingUnitCommands));

    const std::vector<std::string> missingGameActions =
      missingEntries(commandSurface.gameActions, probe.implementedGameActions);
    addCheck(
      report,
      "game-action-surface-complete",
      missingGameActions.empty(),
      RuntimeReadinessSeverity::Error,
      missingGameActions.empty() ? "all BWAPI game action methods are implemented by name" : join(missingGameActions));

    const std::vector<std::string> missingCapabilityNames = missingCapabilities(probe, contract);
    addCheck(
      report,
      "required-capabilities-present",
      missingCapabilityNames.empty(),
      RuntimeReadinessSeverity::Error,
      missingCapabilityNames.empty() ? "all required runtime capabilities are present" : join(missingCapabilityNames));

    addCheck(
      report,
      "executor-contract-valid",
      executorPreflight.contractValid,
      RuntimeReadinessSeverity::Error,
      executorPreflight.contractValid ? "executor accepted runtime contract" : "executor rejected runtime contract");

    addCheck(
      report,
      "runtime-process-identified",
      executorPreflight.processIdentified,
      RuntimeReadinessSeverity::Error,
      executorPreflight.processIdentified ? "target runtime process is identified" : "target runtime process is not identified");

    const bool requiresMemoryAccess = requiresCapability(contract, Capability::SharedMemoryClient);
    std::string memoryAccessDetail = "process memory access is not required by this runtime contract";
    if (requiresMemoryAccess)
    {
      memoryAccessDetail = executorPreflight.memoryAccessible
        ? "target runtime process memory access is available"
        : "target runtime process memory access is unavailable";
      if (!executorPreflight.memoryAccessReason.empty())
        memoryAccessDetail += ": " + executorPreflight.memoryAccessReason;
    }
    addCheck(
      report,
      "runtime-memory-accessible",
      !requiresMemoryAccess || executorPreflight.memoryAccessible,
      RuntimeReadinessSeverity::Error,
      memoryAccessDetail);

    addCheck(
      report,
      "runtime-target-located",
      executorPreflight.targetLocated,
      RuntimeReadinessSeverity::Error,
      executorPreflight.targetLocated ? "target executable or runtime image is located" : "target executable or runtime image is not located");

    addCheck(
      report,
      "executor-available",
      executorPreflight.executorAvailable,
      RuntimeReadinessSeverity::Error,
      executorPreflight.executorAvailable ? "authorized runtime executor is available" : "authorized runtime executor is not available");

    const bool bridgeModeValid = executorPreflight.executorBridgeMode.empty()
      || executorPreflight.executorBridgeMode == RuntimeExecutorBridgeValidatedAdapterMode;
    std::string bridgeModeDetail = "executor did not report filesystem bridge mode";
    if (!executorPreflight.executorBridgeMode.empty())
    {
      bridgeModeDetail = bridgeModeValid
        ? "filesystem bridge mode is validated-runtime-adapter"
        : "filesystem bridge mode is " + executorPreflight.executorBridgeMode
            + "; expected " + RuntimeExecutorBridgeValidatedAdapterMode;
    }
    addCheck(
      report,
      "executor-bridge-mode-valid",
      bridgeModeValid,
      RuntimeReadinessSeverity::Error,
      bridgeModeDetail);

    addCheck(
      report,
      "executor-behavior-proof-complete",
      executorPreflight.missingBehaviorProofs.empty(),
      RuntimeReadinessSeverity::Error,
      executorPreflight.missingBehaviorProofs.empty()
        ? "all required executor behavior proofs are present"
        : join(executorPreflight.missingBehaviorProofs));

    addCheck(
      report,
      "executor-preflight-clean",
      executorPreflight.errors.empty(),
      RuntimeReadinessSeverity::Error,
      executorPreflight.errors.empty() ? "executor preflight has no blocking errors" : join(executorPreflight.errors));

    addCheck(
      report,
      "executor-preflight-warnings",
      executorPreflight.warnings.empty(),
      RuntimeReadinessSeverity::Warning,
      executorPreflight.warnings.empty() ? "executor preflight has no warnings" : join(executorPreflight.warnings));

    report.productionReady = !hasBlockingFailures(report);
    return report;
  }

  std::vector<RuntimeReadinessCheck> blockingReadinessGaps(const RuntimeReadinessReport& report)
  {
    std::vector<RuntimeReadinessCheck> gaps;
    for (const RuntimeReadinessCheck& check : report.checks)
    {
      if (check.severity == RuntimeReadinessSeverity::Error && !check.passed)
        gaps.push_back(check);
    }
    return gaps;
  }
}
