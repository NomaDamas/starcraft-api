#include <BWAPI/Runtime/RuntimeExecutor.h>
#include <BWAPI/Runtime/RuntimeManifest.h>

#include <cassert>
#include <string>
#include <utility>

using namespace BWAPI::Runtime;

namespace
{
  std::string fixturePath(const std::string& name)
  {
    return std::string(STARCRAFT_API_TEST_FIXTURE_DIR) + "/" + name;
  }

  RuntimeEnvironment remasteredEnvironment(std::string executablePath)
  {
    RuntimeEnvironment environment = RuntimeEnvironment::detectHost();
    environment.product = Product::StarCraftRemastered;
    environment.version = "test-build";
    environment.executablePath = std::move(executablePath);
    environment.manifestPath = fixturePath("remastered-complete.manifest");
    return environment;
  }
}

int main()
{
  RuntimeManifestLoadResult complete = loadRuntimeManifestFile(fixturePath("remastered-complete.manifest"));
  assert(complete.loaded);

  RuntimeExecutorPreflightResult readyPrerequisites =
    preflightRuntimeExecutor(remasteredEnvironment(fixturePath("remastered-complete.manifest")), complete.manifest.contract);
  assert(readyPrerequisites.contractValid);
  assert(readyPrerequisites.targetLocated);
  assert(!readyPrerequisites.executorAvailable);
  assert(!readyPrerequisites.warnings.empty());

  RuntimeExecutorPreflightResult missingTarget =
    preflightRuntimeExecutor(remasteredEnvironment(fixturePath("missing-starcraft")), complete.manifest.contract);
  assert(missingTarget.contractValid);
  assert(!missingTarget.targetLocated);
  assert(!missingTarget.executorAvailable);
  assert(!missingTarget.errors.empty());

  RuntimeContract unresolved = makeRemasteredParityContract("test-build");
  RuntimeExecutorPreflightResult invalidContract =
    preflightRuntimeExecutor(remasteredEnvironment(fixturePath("remastered-complete.manifest")), unresolved);
  assert(!invalidContract.contractValid);
  assert(invalidContract.targetLocated);
  assert(!invalidContract.executorAvailable);
  assert(!invalidContract.errors.empty());

  return 0;
}
