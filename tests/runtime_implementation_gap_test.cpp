#include <BWAPI/Runtime/RuntimeCommandSurface.h>
#include <BWAPI/Runtime/RuntimeImplementationGap.h>

#include <cassert>

using namespace BWAPI::Runtime;

namespace
{
  RuntimeContract resolvedContract(const std::string& evidence = "proof.read_game_state=passed")
  {
    RuntimeContract contract = makeRemasteredParityContract("test-build");

    for (RuntimeBinding& binding : contract.bindings)
    {
      binding.resolved = true;
      binding.evidence = evidence;
    }

    for (StructureLayout& structure : contract.structures)
    {
      structure.size = 1;
      structure.evidence = evidence;
      for (StructureField& field : structure.fields)
      {
        field.resolved = true;
        field.offset = 0;
        field.size = 1;
        field.evidence = evidence;
      }
    }

    return contract;
  }

  RuntimeProbeResult fullProbe(const RuntimeContract& contract)
  {
    RuntimeCommandSurface surface = makeBWAPICommandSurface();

    RuntimeProbeResult probe;
    probe.supported = true;
    probe.capabilities = contract.requiredCapabilities;
    probe.implementedUnitCommands = surface.unitCommands;
    probe.implementedGameActions = surface.gameActions;
    probe.implementedApiSurfaceMethods = contract.requiredApiSurfaceMethods;
    probe.implementedCommandSurfaceEntries = contract.requiredCommandSurfaceEntries;
    return probe;
  }

  RuntimeExecutorPreflightResult cleanPreflight()
  {
    RuntimeExecutorPreflightResult preflight;
    preflight.contractValid = true;
    preflight.processIdentified = true;
    preflight.memoryAccessible = true;
    preflight.targetLocated = true;
    preflight.executorAvailable = true;
    return preflight;
  }
}

int main()
{
  RuntimeContract incompleteContract = makeRemasteredParityContract("test-build");
  RuntimeProbeResult emptyProbe;
  emptyProbe.supported = false;
  RuntimeExecutorPreflightResult blockedPreflight;
  ContractValidationResult validation = validateRuntimeContract(incompleteContract);
  assert(!validation.errors.empty());
  blockedPreflight.errors.push_back(validation.errors.front());
  blockedPreflight.errors.push_back("executor unavailable");

  std::vector<RuntimeImplementationGap> gaps =
    collectRuntimeImplementationGaps(emptyProbe, incompleteContract, blockedPreflight);
  assert(!gaps.empty());
  assert(countRuntimeImplementationGapsByCategory(gaps, "backend") == 1);
  assert(countRuntimeImplementationGapsByCategory(gaps, "api-surface") == 1);
  assert(countRuntimeImplementationGapsByCategory(gaps, "command-surface") == 1);
  assert(countRuntimeImplementationGapsByCategory(gaps, "unit-command") == makeBWAPICommandSurface().unitCommands.size());
  assert(countRuntimeImplementationGapsByCategory(gaps, "game-action") == makeBWAPICommandSurface().gameActions.size());
  assert(countRuntimeImplementationGapsByCategory(gaps, "capability") == incompleteContract.requiredCapabilities.size());
  assert(countRuntimeImplementationGapsByCategory(gaps, "memory-access") == 1);
  assert(countRuntimeImplementationGapsByCategory(gaps, "data-address") > 0);
  assert(countRuntimeImplementationGapsByCategory(gaps, "structure-layout") == incompleteContract.structures.size());
  assert(countRuntimeImplementationGapsByCategory(gaps, "structure-field") > 0);
  assert(countRuntimeImplementationGapsByCategory(gaps, "executor-preflight") == 3);
  assert(countRuntimeImplementationGapsByCategory(gaps, "executor-preflight-error") == 1);

  std::vector<RuntimeImplementationGapCategoryCount> categories =
    summarizeRuntimeImplementationGapsByCategory(gaps);
  assert(!categories.empty());
  assert(categories.front().category == "backend");
  assert(categories.front().count == 1);

  RuntimeContract completeContract = resolvedContract();
  std::vector<RuntimeImplementationGap> noGaps =
    collectRuntimeImplementationGaps(fullProbe(completeContract), completeContract, cleanPreflight());
  assert(noGaps.empty());
  assert(summarizeRuntimeImplementationGapsByCategory(noGaps).empty());
  assert(countRuntimeImplementationGapsByCategory(noGaps, "backend") == 0);

  RuntimeExecutorPreflightResult proofBlockedPreflight = cleanPreflight();
  proofBlockedPreflight.executorAvailable = false;
  proofBlockedPreflight.executorBridgeMode = RuntimeExecutorBridgeBootstrapMode;
  proofBlockedPreflight.missingBehaviorProofs.push_back("proof.attach=passed");
  std::vector<RuntimeImplementationGap> proofGaps =
    collectRuntimeImplementationGaps(fullProbe(completeContract), completeContract, proofBlockedPreflight);
  assert(countRuntimeImplementationGapsByCategory(proofGaps, "executor-preflight") == 1);
  assert(countRuntimeImplementationGapsByCategory(proofGaps, "executor-bridge-mode") == 1);
  assert(countRuntimeImplementationGapsByCategory(proofGaps, "executor-behavior-proof") == 1);

  RuntimeExecutorPreflightResult memoryBlockedPreflight = cleanPreflight();
  memoryBlockedPreflight.memoryAccessible = false;
  memoryBlockedPreflight.memoryAccessReason = "unit-test denial";
  std::vector<RuntimeImplementationGap> memoryGaps =
    collectRuntimeImplementationGaps(fullProbe(completeContract), completeContract, memoryBlockedPreflight);
  assert(countRuntimeImplementationGapsByCategory(memoryGaps, "memory-access") == 1);

  return 0;
}
