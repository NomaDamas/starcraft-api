#include <BWAPI/Runtime/RuntimeContract.h>
#include <BWAPI/Runtime/RuntimeCommandSurface.h>

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
  assert(findRuntimeBinding(resolved, "BW::BWDATA::Game", BindingKind::DataAddress) != nullptr);
  assert(findRuntimeBinding(resolved, "BW::BWDATA::Game", BindingKind::FunctionAddress) == nullptr);
  assert(findStructureLayout(resolved, "BW::CUnit") != nullptr);
  assert(findStructureField(resolved, "BW::CUnit", "position") != nullptr);
  assert(findStructureField(resolved, "BW::CUnit", "missing") == nullptr);

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
