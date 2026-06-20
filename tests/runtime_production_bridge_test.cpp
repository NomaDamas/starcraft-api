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

  void writeBootstrapReadyFile(const std::filesystem::path& bridgePath)
  {
    std::ofstream ready(bridgePath / RuntimeExecutorBridgeReadyFile);
    ready << "protocol=" << RuntimeExecutorBridgeProtocol << '\n';
    ready << "product=starcraft-remastered\n";
    ready << "version=test-build\n";
    ready << "mode=" << RuntimeExecutorBridgeBootstrapMode << '\n';
  }

  void writeValidatedAdapterReadyFile(const std::filesystem::path& bridgePath)
  {
    std::ofstream ready(bridgePath / RuntimeExecutorBridgeReadyFile);
    ready << "protocol=" << RuntimeExecutorBridgeProtocol << '\n';
    ready << "product=starcraft-remastered\n";
    ready << "version=test-build\n";
    ready << "mode=" << RuntimeExecutorBridgeValidatedAdapterMode << '\n';
    ready << "proof.attach=passed\n";
    ready << "proof.read_game_state=passed\n";
    ready << "proof.read_units=passed\n";
    ready << "proof.issue_commands=passed\n";
    ready << "proof.draw_overlays=passed\n";
    ready << "proof.dispatch_events=passed\n";
    ready << "proof.replay_analysis=passed\n";
    ready << "proof.multiplayer_sync=passed\n";
    ready << "proof.battle_net_policy=passed\n";
  }

  void writePartialValidatedAdapterReadyFile(const std::filesystem::path& bridgePath)
  {
    std::ofstream ready(bridgePath / RuntimeExecutorBridgeReadyFile);
    ready << "protocol=" << RuntimeExecutorBridgeProtocol << '\n';
    ready << "product=starcraft-remastered\n";
    ready << "version=test-build\n";
    ready << "mode=" << RuntimeExecutorBridgeValidatedAdapterMode << '\n';
    ready << "proof.attach=passed\n";
    ready << "proof.read_game_state=passed\n";
    ready << "proof.read_units=passed\n";
    ready << "proof.issue_commands=passed\n";
    ready << "proof.draw_overlays=passed\n";
    ready << "proof.dispatch_events=passed\n";
    ready << "proof.replay_analysis=passed\n";
    ready << "proof.battle_net_policy=passed\n";
  }
}

int main()
{
  RuntimeManifestLoadResult manifest = loadRuntimeManifestFile(fixturePath("remastered-complete.manifest"));
  assert(manifest.loaded);

  std::filesystem::path bridgePath = makeBridgePath();
  writeBootstrapReadyFile(bridgePath);

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

  assert(!probe.supported);
  assert(!preflight.executorAvailable);
  assert(!preflight.errors.empty());
  assert(!readiness.productionReady);
  assert(!blockingReadinessGaps(readiness).empty());

  writePartialValidatedAdapterReadyFile(bridgePath);
  backend = createRuntimeBackend(environment);
  probe = backend->probe();
  preflight = preflightRuntimeExecutor(environment, manifest.manifest.contract);
  readiness = evaluateProductionReadiness(probe, manifest.manifest.contract, preflight);

  assert(!probe.supported);
  assert(!preflight.executorAvailable);
  assert(preflight.executorBridgeMode == RuntimeExecutorBridgeValidatedAdapterMode);
  assert(preflight.missingBehaviorProofs.size() == 1);
  assert(!preflight.errors.empty());
  assert(!readiness.productionReady);
  assert(!blockingReadinessGaps(readiness).empty());

  writeValidatedAdapterReadyFile(bridgePath);
  backend = createRuntimeBackend(environment);
  probe = backend->probe();
  preflight = preflightRuntimeExecutor(environment, manifest.manifest.contract);
  readiness = evaluateProductionReadiness(probe, manifest.manifest.contract, preflight);

  assert(probe.supported);
  assert(preflight.executorAvailable);
  assert(preflight.errors.empty());
  assert(readiness.productionReady);
  assert(blockingReadinessGaps(readiness).empty());

  RuntimeOpenResult opened = backend->open();
  assert(opened.opened);
  assert(opened.state == RuntimeSessionState::Open);
  assert(backend->state() == RuntimeSessionState::Open);
  backend->close();
  assert(backend->state() == RuntimeSessionState::Closed);

  std::filesystem::remove_all(bridgePath);
  return 0;
}
