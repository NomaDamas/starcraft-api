#include <BWAPI/Runtime/RuntimeManifest.h>

#include <cassert>
#include <sstream>
#include <string>

using namespace BWAPI::Runtime;

namespace
{
  std::string fixturePath(const std::string& name)
  {
    return std::string(STARCRAFT_API_TEST_FIXTURE_DIR) + "/" + name;
  }

  bool hasErrorContaining(const RuntimeManifestLoadResult& result, const std::string& value)
  {
    for (const std::string& error : result.errors)
    {
      if (error.find(value) != std::string::npos)
        return true;
    }
    return false;
  }
}

int main()
{
  RuntimeManifestLoadResult complete = loadRuntimeManifestFile(fixturePath("remastered-complete.manifest"));
  assert(complete.loaded);
  assert(complete.errors.empty());
  assert(complete.manifest.contract.product == Product::StarCraftRemastered);
  assert(complete.manifest.contract.version == "test-build");
  assert(complete.manifest.implementedApiSurfaceMethods == complete.manifest.contract.requiredApiSurfaceMethods);
  assert(complete.manifest.implementedCommandSurfaceEntries == complete.manifest.contract.requiredCommandSurfaceEntries);
  assert(complete.manifest.unitCommands.size() == 44);
  assert(complete.manifest.gameActions.size() == 28);
  assert(complete.manifest.unitCommandEvidence.size() == complete.manifest.unitCommands.size());
  assert(complete.manifest.gameActionEvidence.size() == complete.manifest.gameActions.size());
  assert(commandEvidenceStatusFor(complete.manifest.unitCommandEvidence, "Attack_Move") == RuntimeCommandEvidenceStatus::MockTested);
  assert(commandEvidenceStatusFor(complete.manifest.gameActionEvidence, "drawBox") == RuntimeCommandEvidenceStatus::AdapterLocal);

  ContractValidationResult completeValidation = validateRuntimeContract(complete.manifest.contract);
  assert(completeValidation.valid);
  assert(completeValidation.errors.empty());
  assert(!completeValidation.warnings.empty());
  assert(contractContainsFixtureEvidence(complete.manifest.contract));

  RuntimeProbeResult completeProbe;
  completeProbe.supported = true;
  completeProbe.capabilities = complete.manifest.capabilities;
  completeProbe.implementedUnitCommands = complete.manifest.unitCommands;
  completeProbe.implementedGameActions = complete.manifest.gameActions;
  completeProbe.implementedUnitCommandEvidence = complete.manifest.unitCommandEvidence;
  completeProbe.implementedGameActionEvidence = complete.manifest.gameActionEvidence;
  completeProbe.implementedApiSurfaceMethods = complete.manifest.implementedApiSurfaceMethods;
  completeProbe.implementedCommandSurfaceEntries = complete.manifest.implementedCommandSurfaceEntries;
  assert(!canClaimProductionSupport(completeProbe, complete.manifest.contract));

  RuntimeManifestLoadResult incomplete = loadRuntimeManifestFile(fixturePath("remastered-incomplete.manifest"));
  assert(!incomplete.loaded);
  assert(!incomplete.errors.empty());
  assert(incomplete.manifest.contract.product == Product::StarCraftRemastered);
  assert(incomplete.manifest.contract.version == "test-build");
  assert(incomplete.manifest.implementedApiSurfaceMethods == 384);
  assert(incomplete.manifest.implementedCommandSurfaceEntries == 1);
  assert(incomplete.manifest.unitCommands.size() == 1);

  std::istringstream bootstrap(
    "product starcraft-remastered\n"
    "version test-build\n"
    "api-surface-methods 0\n"
    "command-surface-entries 0\n");
  RuntimeManifestLoadResult bootstrapResult = loadRuntimeManifest(bootstrap, "bootstrap");
  assert(bootstrapResult.loaded);
  assert(bootstrapResult.errors.empty());
  assert(bootstrapResult.manifest.contract.product == Product::StarCraftRemastered);
  assert(bootstrapResult.manifest.contract.version == "test-build");
  assert(bootstrapResult.manifest.implementedApiSurfaceMethods == 0);
  assert(bootstrapResult.manifest.implementedCommandSurfaceEntries == 0);
  assert(!hasErrorContaining(bootstrapResult, "manifest API surface method count is missing"));
  assert(!hasErrorContaining(bootstrapResult, "manifest command surface entry count is missing"));
  assert(!hasErrorContaining(bootstrapResult, "manifest is missing required unit command"));
  assert(!validateRuntimeContract(bootstrapResult.manifest.contract).valid);

  std::istringstream proofBackedManifest(
    "product starcraft-remastered\n"
    "version test-build\n"
    "api-surface-methods 0\n"
    "command-surface-entries 0\n"
    "binding BW::BWDATA::Game data-address required proof.read_game_state=passed\n");
  RuntimeManifestLoadResult proofBackedResult = loadRuntimeManifest(proofBackedManifest, "proof-backed");
  assert(!proofBackedResult.loaded);
  assert(hasErrorContaining(proofBackedResult, "proof.* is only accepted from a validated ready file"));

  std::istringstream missingCommandEvidence(
    "product starcraft-remastered\n"
    "version test-build\n"
    "api-surface-methods 0\n"
    "command-surface-entries 1\n"
    "unit-command Attack_Move\n");
  RuntimeManifestLoadResult missingEvidenceResult = loadRuntimeManifest(missingCommandEvidence, "missing-command-evidence");
  assert(!missingEvidenceResult.loaded);
  assert(hasErrorContaining(missingEvidenceResult, "unit-command directive expects"));

  std::istringstream unknownCommandEvidence(
    "product starcraft-remastered\n"
    "version test-build\n"
    "api-surface-methods 0\n"
    "command-surface-entries 1\n"
    "unit-command Attack_Move fixture\n");
  RuntimeManifestLoadResult unknownEvidenceResult = loadRuntimeManifest(unknownCommandEvidence, "unknown-command-evidence");
  assert(!unknownEvidenceResult.loaded);
  assert(hasErrorContaining(unknownEvidenceResult, "unknown unit-command evidence status"));

  std::istringstream duplicateCommand(
    "product starcraft-remastered\n"
    "version test-build\n"
    "api-surface-methods 0\n"
    "command-surface-entries 2\n"
    "unit-command Attack_Move mock-tested encoder\n"
    "unit-command Attack_Move mock-tested encoder\n");
  RuntimeManifestLoadResult duplicateCommandResult = loadRuntimeManifest(duplicateCommand, "duplicate-command");
  assert(!duplicateCommandResult.loaded);
  assert(hasErrorContaining(duplicateCommandResult, "manifest declares duplicate unit command: Attack_Move"));

  std::istringstream malformed("product unknown-product\n");
  RuntimeManifestLoadResult malformedResult = loadRuntimeManifest(malformed, "inline");
  assert(!malformedResult.loaded);
  assert(!malformedResult.errors.empty());

  return 0;
}
