#include <BWAPI/Runtime/RuntimeBackend.h>
#include <BWAPI/Runtime/RuntimeExecutor.h>
#include <BWAPI/Runtime/RuntimeManifest.h>
#include <BWAPI/Runtime/RuntimeProcessMemory.h>
#include <BWAPI/Runtime/RuntimeReadiness.h>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

using namespace BWAPI::Runtime;

namespace
{
  std::string fixturePath(const std::string& name)
  {
    return std::string(STARCRAFT_API_TEST_FIXTURE_DIR) + "/" + name;
  }

  std::filesystem::path makeBridgePath()
  {
    std::filesystem::path path = std::filesystem::temp_directory_path() / "starcraft-api-production-bridge-test";
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
  }

  void writeReadyFile(const std::filesystem::path& bridgePath)
  {
    std::ofstream ready(bridgePath / RuntimeExecutorBridgeReadyFile);
    ready << "protocol=" << RuntimeExecutorBridgeProtocol << '\n';
    ready << "product=starcraft-remastered\n";
    ready << "version=test-build\n";
  }
}

int main()
{
  RuntimeManifestLoadResult manifest = loadRuntimeManifestFile(fixturePath("remastered-complete.manifest"));
  assert(manifest.loaded);

  std::filesystem::path bridgePath = makeBridgePath();
  writeReadyFile(bridgePath);

  RuntimeEnvironment environment = RuntimeEnvironment::detectHost();
  environment.product = Product::StarCraftRemastered;
  environment.version = "test-build";
  environment.processId = currentProcessId();
  environment.executablePath = fixturePath("remastered-complete.manifest");
  environment.manifestPath = fixturePath("remastered-complete.manifest");
  environment.executorBridgePath = bridgePath.string();

  std::unique_ptr<RuntimeBackend> backend = createRuntimeBackend(environment);
  RuntimeProbeResult probe = backend->probe();
  RuntimeExecutorPreflightResult preflight = preflightRuntimeExecutor(environment, manifest.manifest.contract);
  RuntimeReadinessReport readiness = evaluateProductionReadiness(probe, manifest.manifest.contract, preflight);

  assert(probe.supported);
  assert(preflight.executorAvailable);
  assert(preflight.errors.empty());
  assert(readiness.productionReady);
  assert(blockingReadinessGaps(readiness).empty());

  std::filesystem::remove_all(bridgePath);
  return 0;
}
