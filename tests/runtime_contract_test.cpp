#include <BWAPI/Runtime/RuntimeContract.h>
#include <BWAPI/Runtime/RuntimeCommandSurface.h>

#include <cassert>
#include <string>

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
}

int main()
{
  RuntimeContract remastered = makeRemasteredParityContract("unknown");
  ContractValidationResult remasteredValidation = validateRuntimeContract(remastered);
  assert(!remasteredValidation.valid);
  assert(!remasteredValidation.errors.empty());

  RuntimeProbeResult unsupportedProbe;
  unsupportedProbe.supported = false;
  assert(!canClaimProductionSupport(unsupportedProbe, remastered));

  RuntimeContract resolved = resolvedContract();
  ContractValidationResult resolvedValidation = validateRuntimeContract(resolved);
  assert(resolvedValidation.valid);
  assert(resolvedValidation.errors.empty());
  assert(contractProductionEvidenceErrors(resolved).empty());
  assert(findRuntimeBinding(resolved, "BW::BWDATA::Game", BindingKind::DataAddress) != nullptr);
  assert(findRuntimeBinding(resolved, "BW::BWDATA::Game", BindingKind::FunctionAddress) == nullptr);
  assert(findStructureLayout(resolved, "BW::CUnit") != nullptr);
  assert(findStructureField(resolved, "BW::CUnit", "position") != nullptr);
  assert(findStructureField(resolved, "BW::CUnit", "missing") == nullptr);

  RuntimeContract fixtureLayoutEvidence = resolved;
  fixtureLayoutEvidence.structures.front().evidence = "fixture:bwgame-layout";
  assert(contractContainsFixtureEvidence(fixtureLayoutEvidence));
  fixtureLayoutEvidence.structures.front().evidence.clear();
  fixtureLayoutEvidence.structures.front().fields.front().evidence = "static-layout:bwgame-players";
  assert(contractContainsFixtureEvidence(fixtureLayoutEvidence));

  RuntimeContract bareUnitTestEvidence = resolved;
  bareUnitTestEvidence.bindings.front().evidence = "unit-test";
  assert(contractContainsFixtureEvidence(bareUnitTestEvidence));
  assert(!contractProductionEvidenceErrors(bareUnitTestEvidence).empty());

  RuntimeContract arbitraryEvidence = resolved;
  arbitraryEvidence.bindings.front().evidence = "manual-review";
  assert(!contractContainsFixtureEvidence(arbitraryEvidence));
  assert(!contractProductionEvidenceErrors(arbitraryEvidence).empty());

  RuntimeContract missingLayoutEvidence = resolved;
  missingLayoutEvidence.structures.front().evidence.clear();
  assert(!contractProductionEvidenceErrors(missingLayoutEvidence).empty());

  RuntimeProbeResult incompleteProbe;
  incompleteProbe.supported = true;
  incompleteProbe.capabilities = {
    Capability::ReadGameState,
    Capability::ReadMapData
  };
  assert(!canClaimProductionSupport(incompleteProbe, resolved));

  RuntimeProbeResult fullProbe;
  fullProbe.supported = true;
  fullProbe.capabilities = resolved.requiredCapabilities;
  fullProbe.implementedApiSurfaceMethods = resolved.requiredApiSurfaceMethods;
  fullProbe.implementedCommandSurfaceEntries = resolved.requiredCommandSurfaceEntries;
  RuntimeCommandSurface commandSurface = makeBWAPICommandSurface();
  fullProbe.implementedUnitCommands = commandSurface.unitCommands;
  fullProbe.implementedGameActions = commandSurface.gameActions;
  assert(canClaimProductionSupport(fullProbe, resolved));
  assert(!canClaimProductionSupport(fullProbe, bareUnitTestEvidence));
  assert(!canClaimProductionSupport(fullProbe, arbitraryEvidence));
  assert(!canClaimProductionSupport(fullProbe, missingLayoutEvidence));

  RuntimeProbeResult missingApiSurfaceProbe = fullProbe;
  missingApiSurfaceProbe.implementedApiSurfaceMethods = resolved.requiredApiSurfaceMethods - 1;
  assert(!canClaimProductionSupport(missingApiSurfaceProbe, resolved));

  RuntimeProbeResult missingCommandSurfaceProbe = fullProbe;
  missingCommandSurfaceProbe.implementedCommandSurfaceEntries = resolved.requiredCommandSurfaceEntries - 1;
  assert(!canClaimProductionSupport(missingCommandSurfaceProbe, resolved));

  RuntimeProbeResult missingCommandNameProbe = fullProbe;
  missingCommandNameProbe.implementedUnitCommands.pop_back();
  assert(!canClaimProductionSupport(missingCommandNameProbe, resolved));

  assert(std::string(toString(BindingKind::CommandQueue)) == "command-queue");
  assert(std::string(toString(BindingRequirement::Required)) == "required");

  return 0;
}
