#include <BWAPI/Runtime/RuntimeCommandSurface.h>
#include <BWAPI/Runtime/RuntimeReadiness.h>

#include <cassert>
#include <string>
#include <vector>

using namespace BWAPI::Runtime;

namespace
{
  std::vector<RuntimeCommandEvidence> liveCommandEvidence(const std::vector<std::string>& names)
  {
    std::vector<RuntimeCommandEvidence> evidence;
    for (const std::string& name : names)
      evidence.push_back({ name, RuntimeCommandEvidenceStatus::LiveProven, "unit-test-live-proof" });
    return evidence;
  }

  std::string productionEvidenceForBinding(const RuntimeBinding& binding)
  {
    if (binding.name == "BW::BWDATA::Game")
      return "proof.read_game_state=passed";
    if (binding.name == "BW::BWDATA::Players")
      return "proof.read_player_data=passed";
    if (binding.name == "BW::BWDATA::UnitNodeTable")
      return "proof.read_units=passed";
    if (binding.name == "BW::BWDATA::BulletNodeTable")
      return "proof.read_bullet_data=passed";
    if (binding.name == "BW::BWDATA::MapTileArray")
      return "proof.read_map_data=passed";
    if (binding.name == "BW::BWFXN_ExecuteGameTriggers")
      return "proof.dispatch_events=passed";
    if (binding.name == "Storm::SNetReceiveMessage" || binding.name == "Storm::SNetSendTurn")
      return "proof.multiplayer_sync=passed";
    if (binding.name == "draw-game-layer-hook")
      return "proof.draw_overlays=passed";
    if (binding.name == "BW::BWDATA::sgdwBytesInCmdQueue" || binding.name == "BW::BWDATA::TurnBuffer")
      return "proof.issue_commands=passed";
    if (binding.name == "ai-module-loader")
      return "proof.load_ai_modules=passed";
    if (binding.name == "shared-memory-client-transport")
      return "proof.attach=passed";
    return "proof.unknown=passed";
  }

  std::string productionEvidenceForStructure(const std::string& name)
  {
    if (name == "BW::BWGame")
      return "proof.read_game_state=passed";
    if (name == "BW::CUnit")
      return "proof.read_units=passed";
    if (name == "BW::CBullet")
      return "proof.read_bullet_data=passed";
    if (name == "BW::PlayerInfo")
      return "proof.read_player_data=passed";
    if (name == "BW::ReplayHeader")
      return "proof.replay_analysis=passed";
    return "proof.unknown=passed";
  }

  std::string productionEvidenceForField(const std::string& structureName, const std::string& fieldName)
  {
    if (structureName == "BW::BWGame")
    {
      if (fieldName == "players" || fieldName == "alliance")
        return "proof.read_player_data=passed";
      if (fieldName == "elapsedFrames")
        return "proof.read_game_state=passed";
    }
    return productionEvidenceForStructure(structureName);
  }

  RuntimeContract resolvedContract()
  {
    RuntimeContract contract = makeRemasteredParityContract("test-build");

    for (RuntimeBinding& binding : contract.bindings)
    {
      binding.resolved = true;
      binding.evidence = productionEvidenceForBinding(binding);
    }

    for (StructureLayout& structure : contract.structures)
    {
      structure.size = 1;
      structure.evidence = productionEvidenceForStructure(structure.name);
      for (StructureField& field : structure.fields)
      {
        field.resolved = true;
        field.offset = 0;
        field.size = 1;
        field.evidence = productionEvidenceForField(structure.name, field.name);
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
    probe.implementedUnitCommandEvidence = liveCommandEvidence(commandSurface.unitCommands);
    probe.implementedGameActionEvidence = liveCommandEvidence(commandSurface.gameActions);
    return probe;
  }

  RuntimeExecutorPreflightResult readyPreflight()
  {
    RuntimeExecutorPreflightResult preflight;
    preflight.contractValid = true;
    preflight.processIdentified = true;
    preflight.memoryAccessible = true;
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

  RuntimeContract nonProductionEvidence = contract;
  nonProductionEvidence.bindings.front().evidence = "unit-test";
  RuntimeReadinessReport evidenceGap = evaluateProductionReadiness(probe, nonProductionEvidence, preflight);
  assert(!evidenceGap.productionReady);
  assert(hasBlockingGap(evidenceGap, "contract-production-evidence"));

  RuntimeContract missingEvidence = contract;
  missingEvidence.structures.front().evidence.clear();
  RuntimeReadinessReport missingEvidenceGap = evaluateProductionReadiness(probe, missingEvidence, preflight);
  assert(!missingEvidenceGap.productionReady);
  assert(hasBlockingGap(missingEvidenceGap, "contract-production-evidence"));

  RuntimeExecutorPreflightResult unavailableExecutor = preflight;
  unavailableExecutor.executorAvailable = false;
  unavailableExecutor.warnings.push_back("authorized runtime executor is not implemented for this product/platform");
  RuntimeReadinessReport missingExecutor = evaluateProductionReadiness(probe, contract, unavailableExecutor);
  assert(!missingExecutor.productionReady);
  assert(hasBlockingGap(missingExecutor, "executor-available"));
  assert(hasBlockingGap(missingExecutor, "executor-behavior-proof-complete"));

  RuntimeExecutorPreflightResult memoryBlocked = preflight;
  memoryBlocked.memoryAccessible = false;
  memoryBlocked.memoryAccessReason = "unit-test denial";
  RuntimeReadinessReport memoryGap = evaluateProductionReadiness(probe, contract, memoryBlocked);
  assert(!memoryGap.productionReady);
  assert(hasBlockingGap(memoryGap, "runtime-memory-accessible"));

  RuntimeExecutorPreflightResult bootstrapBridge = preflight;
  bootstrapBridge.executorAvailable = false;
  bootstrapBridge.executorBridgeMode = RuntimeExecutorBridgeBootstrapMode;
  bootstrapBridge.missingBehaviorProofs.push_back("proof.attach=passed");
  bootstrapBridge.errors.push_back("runtime executor bridge is launch/attach bootstrap only");
  RuntimeReadinessReport bootstrapGap = evaluateProductionReadiness(probe, contract, bootstrapBridge);
  assert(!bootstrapGap.productionReady);
  assert(hasBlockingGap(bootstrapGap, "executor-available"));
  assert(hasBlockingGap(bootstrapGap, "executor-bridge-mode-valid"));
  assert(hasBlockingGap(bootstrapGap, "executor-behavior-proof-complete"));
  assert(hasBlockingGap(bootstrapGap, "executor-preflight-clean"));

  RuntimeExecutorPreflightResult partialProofBridge = preflight;
  partialProofBridge.executorAvailable = true;
  partialProofBridge.executorBridgeMode = RuntimeExecutorBridgeValidatedAdapterMode;
  partialProofBridge.missingBehaviorProofs.push_back("proof.multiplayer_sync=passed");
  partialProofBridge.errors.push_back(
    "runtime executor bridge ready file is missing behavior proof: proof.multiplayer_sync=passed");
  RuntimeReadinessReport proofGap = evaluateProductionReadiness(probe, contract, partialProofBridge);
  assert(!proofGap.productionReady);
  assert(!hasBlockingGap(proofGap, "executor-bridge-mode-valid"));
  assert(hasBlockingGap(proofGap, "executor-behavior-proof-complete"));

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

  RuntimeProbeResult mockCommandEvidence = probe;
  mockCommandEvidence.implementedUnitCommandEvidence.front().status = RuntimeCommandEvidenceStatus::MockTested;
  RuntimeReadinessReport commandEvidenceGap = evaluateProductionReadiness(mockCommandEvidence, contract, preflight);
  assert(!commandEvidenceGap.productionReady);
  assert(hasBlockingGap(commandEvidenceGap, "unit-command-evidence-live"));

  RuntimeProbeResult adapterLocalActionEvidence = probe;
  adapterLocalActionEvidence.implementedGameActionEvidence.front().status = RuntimeCommandEvidenceStatus::AdapterLocal;
  RuntimeReadinessReport actionEvidenceGap = evaluateProductionReadiness(adapterLocalActionEvidence, contract, preflight);
  assert(!actionEvidenceGap.productionReady);
  assert(hasBlockingGap(actionEvidenceGap, "game-action-evidence-live"));

  RuntimeContract unresolved = makeRemasteredParityContract("test-build");
  RuntimeReadinessReport contractGap = evaluateProductionReadiness(probe, unresolved, preflight);
  assert(!contractGap.productionReady);
  assert(hasBlockingGap(contractGap, "contract-valid"));

  assert(std::string(toString(RuntimeReadinessSeverity::Warning)) == "warning");

  return 0;
}
