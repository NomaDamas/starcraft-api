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
    ready << "contract.binding.BW::BWDATA::sgdwBytesInCmdQueue=command-queue|proof.issue_commands=passed:bytes-in-command-queue\n";
    ready << "contract.binding.BW::BWDATA::TurnBuffer=command-queue|proof.issue_commands=passed:turn-buffer\n";
  }

  void writeLiveContractProofs(std::ofstream& ready)
  {
    ready << "contract.binding.BW::BWDATA::Game=data-address|proof.read_game_state=passed:game\n";
    ready << "contract.binding.BW::BWDATA::Players=data-address|proof.read_player_data=passed:players\n";
    ready << "contract.binding.BW::BWDATA::UnitNodeTable=data-address|proof.read_units=passed:unit-node-table\n";
    ready << "contract.binding.BW::BWDATA::BulletNodeTable=data-address|proof.read_bullet_data=passed:bullet-node-table\n";
    ready << "contract.binding.BW::BWDATA::MapTileArray=data-address|proof.read_map_data=passed:map-tile-array\n";
    ready << "contract.binding.BW::BWFXN_ExecuteGameTriggers=function-address|proof.dispatch_events=passed:execute-game-triggers\n";
    ready << "contract.binding.Storm::SNetReceiveMessage=imported-function|proof.multiplayer_sync=passed:snet-receive-message\n";
    ready << "contract.binding.Storm::SNetSendTurn=imported-function|proof.multiplayer_sync=passed:snet-send-turn\n";
    ready << "contract.binding.draw-game-layer-hook=hook-point|proof.draw_overlays=passed:draw-game-layer-hook\n";
    ready << "contract.binding.ai-module-loader=transport|proof.load_ai_modules=passed:ai-module-loader\n";
    ready << "contract.binding.shared-memory-client-transport=transport|proof.attach=passed:shared-memory-client\n";
    ready << "contract.structure.BW::BWGame=256|proof.read_game_state=passed:bwgame-layout\n";
    ready << "contract.field.BW::BWGame.players=0x00|4|proof.read_player_data=passed:bwgame-players\n";
    ready << "contract.field.BW::BWGame.alliance=0x04|4|proof.read_player_data=passed:bwgame-alliance\n";
    ready << "contract.field.BW::BWGame.elapsedFrames=0x08|4|proof.read_game_state=passed:bwgame-elapsed-frames\n";
    ready << "contract.structure.BW::CUnit=512|proof.read_units=passed:cunit-layout\n";
    ready << "contract.field.BW::CUnit.id=0x00|4|proof.read_units=passed:cunit-id\n";
    ready << "contract.field.BW::CUnit.position=0x04|8|proof.read_units=passed:cunit-position\n";
    ready << "contract.field.BW::CUnit.hitPoints=0x0c|4|proof.read_units=passed:cunit-hit-points\n";
    ready << "contract.field.BW::CUnit.order=0x10|4|proof.read_units=passed:cunit-order\n";
    ready << "contract.field.BW::CUnit.player=0x14|4|proof.read_units=passed:cunit-player\n";
    ready << "contract.structure.BW::CBullet=128|proof.read_bullet_data=passed:cbullet-layout\n";
    ready << "contract.field.BW::CBullet.position=0x00|8|proof.read_bullet_data=passed:cbullet-position\n";
    ready << "contract.field.BW::CBullet.velocity=0x08|8|proof.read_bullet_data=passed:cbullet-velocity\n";
    ready << "contract.field.BW::CBullet.sourceUnit=0x10|8|proof.read_bullet_data=passed:cbullet-source-unit\n";
    ready << "contract.field.BW::CBullet.target=0x18|8|proof.read_bullet_data=passed:cbullet-target\n";
    ready << "contract.structure.BW::PlayerInfo=128|proof.read_player_data=passed:player-info-layout\n";
    ready << "contract.field.BW::PlayerInfo.stormId=0x00|4|proof.read_player_data=passed:player-info-storm-id\n";
    ready << "contract.field.BW::PlayerInfo.race=0x04|4|proof.read_player_data=passed:player-info-race\n";
    ready << "contract.field.BW::PlayerInfo.resources=0x08|8|proof.read_player_data=passed:player-info-resources\n";
    ready << "contract.field.BW::PlayerInfo.supply=0x10|8|proof.read_player_data=passed:player-info-supply\n";
    ready << "contract.structure.BW::ReplayHeader=256|proof.replay_analysis=passed:replay-header-layout\n";
    ready << "contract.field.BW::ReplayHeader.mapName=0x00|32|proof.replay_analysis=passed:replay-header-map-name\n";
    ready << "contract.field.BW::ReplayHeader.frameCount=0x20|4|proof.replay_analysis=passed:replay-header-frame-count\n";
    ready << "contract.field.BW::ReplayHeader.playerCount=0x24|4|proof.replay_analysis=passed:replay-header-player-count\n";
    ready << "proof.read_map_data=passed\n";
    ready << "proof.read_player_data=passed\n";
    ready << "proof.read_bullet_data=passed\n";
    ready << "proof.read_region_data=passed\n";
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
    const std::string& executable,
    bool includeLiveContractProofs = false)
  {
    std::ofstream ready(bridgePath / RuntimeExecutorBridgeReadyFile);
    ready << "protocol=" << RuntimeExecutorBridgeProtocol << '\n';
    ready << "product=starcraft-remastered\n";
    ready << "version=test-build\n";
    ready << "mode=" << RuntimeExecutorBridgeValidatedAdapterMode << '\n';
    writeRuntimeIdentity(ready, processId, executable);
    writeRuntimeCommandQueueSink(ready);
    if (includeLiveContractProofs)
      writeLiveContractProofs(ready);
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
  RuntimeContract readinessContract =
    applyRuntimeExecutorBridgeContractProofs(environment, manifest.manifest.contract);
  RuntimeReadinessReport readiness = evaluateProductionReadiness(probe, readinessContract, preflight);

  assert(!probe.supported);
  assert(!preflight.executorAvailable);
  assert(!preflight.errors.empty());
  assert(!readiness.productionReady);
  assert(!blockingReadinessGaps(readiness).empty());

  writePartialValidatedAdapterReadyFile(bridgePath, environment.processId, environment.executablePath);
  backend = createRuntimeBackend(environment);
  probe = backend->probe();
  preflight = preflightRuntimeExecutor(environment, manifest.manifest.contract);
  readinessContract = applyRuntimeExecutorBridgeContractProofs(environment, manifest.manifest.contract);
  readiness = evaluateProductionReadiness(probe, readinessContract, preflight);

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
  readinessContract = applyRuntimeExecutorBridgeContractProofs(environment, manifest.manifest.contract);
  readiness = evaluateProductionReadiness(probe, readinessContract, preflight);

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

  writeValidatedAdapterReadyFile(
    bridgePath,
    environment.processId,
    environment.executablePath,
    true);
  backend = createRuntimeBackend(environment);
  probe = backend->probe();
  preflight = preflightRuntimeExecutor(environment, manifest.manifest.contract);
  readinessContract = applyRuntimeExecutorBridgeContractProofs(environment, manifest.manifest.contract);
  readiness = evaluateProductionReadiness(probe, readinessContract, preflight);

  assert(probe.supported);
  assert(preflight.executorAvailable);
  assert(preflight.errors.empty());
  assert(readiness.productionReady);
  assert(blockingReadinessGaps(readiness).empty());

  opened = backend->open();
  assert(opened.opened);
  assert(opened.state == RuntimeSessionState::Open);
  assert(backend->state() == RuntimeSessionState::Open);
  backend->close();
  assert(backend->state() == RuntimeSessionState::Closed);

  std::filesystem::remove_all(bridgePath);
  return 0;
}
