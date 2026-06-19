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

  ContractValidationResult completeValidation = validateRuntimeContract(complete.manifest.contract);
  assert(completeValidation.valid);
  assert(completeValidation.errors.empty());

  RuntimeProbeResult completeProbe;
  completeProbe.supported = true;
  completeProbe.capabilities = complete.manifest.capabilities;
  completeProbe.implementedApiSurfaceMethods = complete.manifest.implementedApiSurfaceMethods;
  completeProbe.implementedCommandSurfaceEntries = complete.manifest.implementedCommandSurfaceEntries;
  assert(canClaimProductionSupport(completeProbe, complete.manifest.contract));

  RuntimeManifestLoadResult incomplete = loadRuntimeManifestFile(fixturePath("remastered-incomplete.manifest"));
  assert(incomplete.loaded);
  ContractValidationResult incompleteValidation = validateRuntimeContract(incomplete.manifest.contract);
  assert(!incompleteValidation.valid);
  assert(!canClaimProductionSupport(completeProbe, incomplete.manifest.contract));

  std::istringstream malformed("product unknown-product\n");
  RuntimeManifestLoadResult malformedResult = loadRuntimeManifest(malformed, "inline");
  assert(!malformedResult.loaded);
  assert(!malformedResult.errors.empty());

  return 0;
}
