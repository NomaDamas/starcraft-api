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

  void writeRuntimeIdentity(std::ofstream& ready, int processId, const std::string& executable)
  {
    ready << "process_id=" << processId << '\n';
    ready << "executable=" << executable << '\n';
  }

  void writeRuntimeCommandQueueSink(std::ofstream& ready)
  {
    ready << RuntimeExecutorBridgeActiveCommandReceiverLine << '\n';
    ready << RuntimeExecutorBridgeRuntimeCommandQueueSinkLine << '\n';
    ready << "contract.binding.BW::BWDATA::sgdwBytesInCmdQueue=command-queue|unit-test:bytes-in-command-queue\n";
    ready << "contract.binding.BW::BWDATA::TurnBuffer=command-queue|unit-test:turn-buffer\n";
  }

  void writeBootstrapReadyFile(
    const std::filesystem::path& bridgePath,
    int processId,
    const std::string& executable)
  {
    std::ofstream ready(bridgePath / RuntimeExecutorBridgeReadyFile);
    ready << "protocol=" << RuntimeExecutorBridgeProtocol << '\n';
    ready << "product=starcraft-remastered\n";
    ready << "version=test-build\n";
    ready << "mode=" << RuntimeExecutorBridgeBootstrapMode << '\n';
    writeRuntimeIdentity(ready, processId, executable);
  }

  void writeValidatedAdapterReadyFile(
    const std::filesystem::path& bridgePath,
    int processId,
    const std::string& executable)
  {
    std::ofstream ready(bridgePath / RuntimeExecutorBridgeReadyFile);
    ready << "protocol=" << RuntimeExecutorBridgeProtocol << '\n';
    ready << "product=starcraft-remastered\n";
    ready << "version=test-build\n";
    ready << "mode=" << RuntimeExecutorBridgeValidatedAdapterMode << '\n';
    writeRuntimeIdentity(ready, processId, executable);
    writeRuntimeCommandQueueSink(ready);
    for (const RuntimeExecutorBehaviorProof& proof : requiredRuntimeExecutorBehaviorProofs())
      ready << proof.readyFileLine << '\n';
  }

  void writePartialValidatedAdapterReadyFile(
    const std::filesystem::path& bridgePath,
    int processId,
    const std::string& executable)
  {
    std::ofstream ready(bridgePath / RuntimeExecutorBridgeReadyFile);
    ready << "protocol=" << RuntimeExecutorBridgeProtocol << '\n';
    ready << "product=starcraft-remastered\n";
    ready << "version=test-build\n";
    ready << "mode=" << RuntimeExecutorBridgeValidatedAdapterMode << '\n';
    writeRuntimeIdentity(ready, processId, executable);
    writeRuntimeCommandQueueSink(ready);
    for (const RuntimeExecutorBehaviorProof& proof : requiredRuntimeExecutorBehaviorProofs())
    {
      if (std::string(proof.id) != "multiplayer-sync")
        ready << proof.readyFileLine << '\n';
    }
  }
}

int main(int argc, char** argv)
{
  assert(argc > 0);
  const std::string selfExecutable = std::filesystem::absolute(argv[0]).lexically_normal().string();

  RuntimeManifestLoadResult manifest = loadRuntimeManifestFile(fixturePath("remastered-complete.manifest"));
  assert(manifest.loaded);

  std::filesystem::path bridgePath = makeBridgePath();

  RuntimeEnvironment environment = RuntimeEnvironment::detectHost();
  environment.product = Product::StarCraftRemastered;
  environment.version = "test-build";
  environment.processId = currentProcessId();
  environment.executablePath = selfExecutable;
  environment.manifestPath = fixturePath("remastered-complete.manifest");
  environment.executorBridgePath = bridgePath.string();

  writeBootstrapReadyFile(bridgePath, environment.processId, environment.executablePath);

  std::unique_ptr<RuntimeBackend> backend = createRuntimeBackend(environment);
  RuntimeProbeResult probe = backend->probe();
  RuntimeExecutorPreflightResult preflight = preflightRuntimeExecutor(environment, manifest.manifest.contract);
  RuntimeReadinessReport readiness = evaluateProductionReadiness(probe, manifest.manifest.contract, preflight);

  assert(!probe.supported);
  assert(!preflight.executorAvailable);
  assert(!preflight.errors.empty());
  assert(!readiness.productionReady);
  assert(!blockingReadinessGaps(readiness).empty());

  writePartialValidatedAdapterReadyFile(bridgePath, environment.processId, environment.executablePath);
  backend = createRuntimeBackend(environment);
  probe = backend->probe();
  preflight = preflightRuntimeExecutor(environment, manifest.manifest.contract);
  readiness = evaluateProductionReadiness(probe, manifest.manifest.contract, preflight);

  assert(!probe.supported);
  assert(preflight.executorAvailable);
  assert(preflight.executorName == "filesystem-bridge-validated-runtime-adapter");
  assert(preflight.executorBridgeMode == RuntimeExecutorBridgeValidatedAdapterMode);
  assert(preflight.missingBehaviorProofs.size() == 1);
  assert(!preflight.errors.empty());
  assert(!readiness.productionReady);
  assert(!blockingReadinessGaps(readiness).empty());

  writeValidatedAdapterReadyFile(bridgePath, environment.processId, environment.executablePath);
  backend = createRuntimeBackend(environment);
  probe = backend->probe();
  preflight = preflightRuntimeExecutor(environment, manifest.manifest.contract);
  readiness = evaluateProductionReadiness(probe, manifest.manifest.contract, preflight);

  assert(!probe.supported);
  assert(probe.reason.find("fixture validation evidence") != std::string::npos);
  assert(preflight.executorAvailable);
  assert(preflight.errors.empty());
  assert(!readiness.productionReady);
  assert(!blockingReadinessGaps(readiness).empty());

  RuntimeOpenResult opened = backend->open();
  assert(!opened.opened);
  assert(opened.state == RuntimeSessionState::Failed);
  assert(backend->state() == RuntimeSessionState::Failed);

  std::filesystem::remove_all(bridgePath);
  return 0;
}
