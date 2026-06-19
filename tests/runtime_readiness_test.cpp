#include <BWAPI/Runtime/RuntimeCommandSurface.h>
#include <BWAPI/Runtime/RuntimeReadiness.h>

#include <cassert>
#include <string>

using namespace BWAPI::Runtime;

namespace
{
  RuntimeContract resolvedContract()
  {
    RuntimeContract contract = makeRemasteredParityContract("test-build");

    for (RuntimeBinding& binding : contract.bindings)
    {
      binding.resolved = true;
      binding.evidence = "unit-test";
    }

    for (StructureLayout& structure : contract.structures)
    {
      structure.size = 1;
      for (StructureField& field : structure.fields)
      {
        field.resolved = true;
        field.offset = 0;
        field.size = 1;
      }
    }

    return contract;
  }

  RuntimeProbeResult fullProbe(const RuntimeContract& contract)
  {
    RuntimeCommandSurface commandSurface = makeBWAPICommandSurface();

    RuntimeProbeResult probe;
    probe.supported = true;
    probe.capabilities = contract.requiredCapabilities;
    probe.implementedApiSurfaceMethods = contract.requiredApiSurfaceMethods;
    probe.implementedCommandSurfaceEntries = contract.requiredCommandSurfaceEntries;
    probe.implementedUnitCommands = commandSurface.unitCommands;
    probe.implementedGameActions = commandSurface.gameActions;
    return probe;
  }

  RuntimeExecutorPreflightResult readyPreflight()
  {
    RuntimeExecutorPreflightResult preflight;
    preflight.contractValid = true;
    preflight.processIdentified = true;
    preflight.targetLocated = true;
    preflight.executorAvailable = true;
    return preflight;
  }

  bool hasBlockingGap(const RuntimeReadinessReport& report, const std::string& id)
  {
    for (const RuntimeReadinessCheck& gap : blockingReadinessGaps(report))
    {
      if (gap.id == id)
        return true;
    }
    return false;
  }
}

int main()
{
  RuntimeContract contract = resolvedContract();
  RuntimeProbeResult probe = fullProbe(contract);
  RuntimeExecutorPreflightResult preflight = readyPreflight();

  RuntimeReadinessReport ready = evaluateProductionReadiness(probe, contract, preflight);
  assert(ready.productionReady);
  assert(blockingReadinessGaps(ready).empty());

  RuntimeExecutorPreflightResult unavailableExecutor = preflight;
  unavailableExecutor.executorAvailable = false;
  unavailableExecutor.warnings.push_back("authorized runtime executor is not implemented for this product/platform");
  RuntimeReadinessReport missingExecutor = evaluateProductionReadiness(probe, contract, unavailableExecutor);
  assert(!missingExecutor.productionReady);
  assert(hasBlockingGap(missingExecutor, "executor-available"));

  RuntimeProbeResult missingCapability = probe;
  missingCapability.capabilities.pop_back();
  RuntimeReadinessReport capabilityGap = evaluateProductionReadiness(missingCapability, contract, preflight);
  assert(!capabilityGap.productionReady);
  assert(hasBlockingGap(capabilityGap, "required-capabilities-present"));

  RuntimeProbeResult missingGameAction = probe;
  missingGameAction.implementedGameActions.pop_back();
  RuntimeReadinessReport actionGap = evaluateProductionReadiness(missingGameAction, contract, preflight);
  assert(!actionGap.productionReady);
  assert(hasBlockingGap(actionGap, "game-action-surface-complete"));

  RuntimeContract unresolved = makeRemasteredParityContract("test-build");
  RuntimeReadinessReport contractGap = evaluateProductionReadiness(probe, unresolved, preflight);
  assert(!contractGap.productionReady);
  assert(hasBlockingGap(contractGap, "contract-valid"));

  assert(std::string(toString(RuntimeReadinessSeverity::Warning)) == "warning");

  return 0;
}
