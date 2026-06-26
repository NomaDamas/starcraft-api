#include <BWAPI/Runtime/RuntimeBackend.h>
#include <BWAPI/Runtime/RuntimeExecutor.h>
#include <BWAPI/Runtime/RuntimeManifest.h>
#include <BWAPI/Runtime/RuntimeProcessMemory.h>
#include <BWAPI/Runtime/RuntimeReadiness.h>
#include <BWAPI/Runtime/RuntimeResidentBridge.h>

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace BWAPI::Runtime;

namespace
{
  std::atomic<std::uint32_t> residentFrameCounter{ 802 };
  std::array<unsigned char, 64> activeUnitEvidence = {
    0x42, 0x57, 0x41, 0x50, 0x49, 0x2d, 0x55, 0x4e
  };

  struct ResidentFrameCounterTicker
  {
    std::atomic<bool> running{ true };
    std::thread thread;

    ResidentFrameCounterTicker()
      : thread([this]()
        {
          while (running.load(std::memory_order_relaxed))
          {
            residentFrameCounter.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
          }
        })
    {
    }

    ~ResidentFrameCounterTicker()
    {
      running.store(false, std::memory_order_relaxed);
      if (thread.joinable())
        thread.join();
    }
  };

  std::string fixturePath(const std::string& name)
  {
    return std::string(STARCRAFT_API_TEST_FIXTURE_DIR) + "/" + name;
  }

  void writeResidentSnapshotPayload(std::ofstream& snapshot, const std::string& proof)
  {
    if (proof == "read_units")
    {
      snapshot << "index\tnode\tsecondary\tsprite\tid\tx\ty\ttarget_x\ttarget_y\torder\tstate\tplayer\ttype_hint\thit_points\n";
      for (int i = 0; i < 6; ++i)
      {
        snapshot << i << "\t0x" << (1000 + i) << "\t0x" << (2000 + i)
                 << "\t0x" << (3000 + i) << '\t' << (400 + i)
                 << '\t' << (64 + i) << '\t' << (80 + i)
                 << '\t' << (96 + i) << '\t' << (112 + i)
                 << "\t0\t0\t" << (i % 2) << "\t0\t40\n";
      }
      return;
    }
    if (proof == "read_player_data")
    {
      snapshot << "player\tstorm_id\trace\trace_inferred\tobserved_unit_count\tminerals\tgas\tsupply_used\tsupply_total\talliance_mask\n"
               << "0\t100\tTerran\ttrue\t2\t50\t0\t4\t18\t0x1\n"
               << "1\t101\tZerg\ttrue\t2\t50\t0\t4\t18\t0x2\n";
      return;
    }
    if (proof == "read_map_data")
    {
      snapshot << "map_name\tmap_name_address\tmap_tile_array_address\ttile_count\tmap_path\tmap_file_size\tsource\treplay_path\treplay_file_size\n"
               << "UnitTest\t0x1600\t0x1700\t256\t/tmp/UnitTest.scx\t4096\tlive-sc-r-map-tile-array\t/tmp/UnitTest.rep\t8192\n";
      return;
    }
    if (proof == "read_bullet_data")
    {
      snapshot << "index\taddress\tsprite\tsource_unit\ttarget_unit\ttype\tx\ty\tvelocity_x\tvelocity_y\tplayer\tremove_timer\n"
               << "0\t0x1800\t0x3000\t0x1000\t0x2000\t1\t64\t80\t2\t0\t0\t12\n"
               << "1\t0x1880\t0x3010\t0x1010\t0x2010\t2\t96\t112\t0\t-2\t1\t16\n";
      return;
    }
    if (proof == "read_region_data")
    {
      snapshot << "id\tcenter_x\tcenter_y\tleft\ttop\tright\tbottom\tobserved_units\taccessible\n"
               << "0\t32\t32\t0\t0\t64\t64\t1\ttrue\n"
               << "1\t96\t32\t64\t0\t128\t64\t2\ttrue\n"
               << "2\t32\t96\t0\t64\t64\t128\t1\ttrue\n";
      return;
    }
    if (proof == "replay_analysis")
    {
      snapshot << "source\tcurrent_process_replay\tactive_match_metadata\tmap_name\tfirst_frame\tlast_frame\tobserved_player_count\n"
               << "active-match-live-metadata\tfalse\ttrue\tUnitTest\t800\t802\t2\n";
      return;
    }
    snapshot << "field\tvalue\n"
             << "passed\ttrue\n";
  }

  void writeResidentProofSnapshot(
    const std::filesystem::path& path,
    const std::string& proof,
    int processId,
    std::uint64_t heartbeat,
    std::uint64_t frameId,
    bool activeMatchCorrelated)
  {
    std::ofstream snapshot(path);
    snapshot << "# schema=starcraft-api.resident-snapshot.v1\n"
             << "# proof=" << proof << '\n'
             << "# source_identity=resident-adapter\n"
             << "# process_id=" << processId << '\n'
             << "# heartbeat=" << heartbeat << '\n'
             << "# frame_id=" << frameId << '\n'
             << "# active_match_correlated="
             << (activeMatchCorrelated ? "true" : "false") << '\n';
    writeResidentSnapshotPayload(snapshot, proof);
  }

  void writeResidentProofSnapshots(
    const std::filesystem::path& path,
    int processId,
    std::uint64_t heartbeat,
    std::uint64_t frameId,
    bool activeMatchCorrelated)
  {
    const std::vector<std::pair<std::string, std::string>> snapshots = {
      { "units.snapshot.tsv", "read_units" },
      { "issue_commands.snapshot.tsv", "issue_commands" },
      { "draw_overlays.snapshot.tsv", "draw_overlays" },
      { "events.snapshot.tsv", "dispatch_events" },
      { "replay.snapshot.tsv", "replay_analysis" },
      { "multiplayer_sync.snapshot.tsv", "multiplayer_sync" },
      { "ai_module_load.snapshot.tsv", "load_ai_modules" },
      { "map.snapshot.tsv", "read_map_data" },
      { "players.snapshot.tsv", "read_player_data" },
      { "bullets.snapshot.tsv", "read_bullet_data" },
      { "regions.snapshot.tsv", "read_region_data" }
    };
    for (const auto& snapshotSpec : snapshots)
    {
      writeResidentProofSnapshot(
        path / snapshotSpec.first,
        snapshotSpec.second,
        processId,
        heartbeat,
        frameId,
        activeMatchCorrelated);
    }
  }

  std::filesystem::path makeBridgePath()
  {
    std::filesystem::path path = std::filesystem::temp_directory_path() / "starcraft-api-production-bridge-test";
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    writeResidentProofSnapshots(path, currentProcessId(), 30, 802, true);
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
    ready << "proof.issue_commands.command=pauseGame\n";
    ready << "proof.issue_commands.source=live-sc-r-command-path\n";
    ready << "proof.issue_commands.delivery_checked=true\n";
    ready << "proof.issue_commands.behavior_checked=true\n";
    ready << "proof.issue_commands.self_fixture=false\n";
    ready << "proof.issue_commands.pause_frame_counter_matched=true\n";
    ready << "proof.issue_commands.vector_address=0x1000\n";
    ready << "proof.issue_commands.storage_kind=live-sc-r-command-queue-v1\n";
    ready << "proof.issue_commands.bytes_in_queue_address=0x1100\n";
    ready << "proof.issue_commands.frame_counter_address=0x1200\n";
    ready << "proof.issue_commands.encoded_bytes=10\n";
    ready << "proof.issue_commands.stale_proof_bytes_cleared=true\n";
    ready << "proof.issue_commands.snapshot=issue_commands.snapshot.tsv\n";
  }

  void writeValidatedProductionProofMetadata(std::ofstream& ready)
  {
    ready << "proof.attach.source=resident-adapter\n";
    ready << "proof.draw_overlays.source=live-render-hook\n";
    ready << "proof.draw_overlays.hook_address=0x1300\n";
    ready << "proof.draw_overlays.snapshot=draw_overlays.snapshot.tsv\n";
    ready << "proof.dispatch_events.frame_events=3\n";
    ready << "proof.dispatch_events.unit_discover_events=2\n";
    ready << "proof.dispatch_events.unit_update_events=4\n";
    ready << "proof.dispatch_events.unique_players=2\n";
    ready << "proof.dispatch_events.snapshot=events.snapshot.tsv\n";
    ready << "proof.replay_analysis.source=active-match-live-metadata\n";
    ready << "proof.replay_analysis.current_process_replay=false\n";
    ready << "proof.replay_analysis.active_match_metadata=true\n";
    ready << "proof.replay_analysis.map_name=UnitTest\n";
    ready << "proof.replay_analysis.first_frame=800\n";
    ready << "proof.replay_analysis.last_frame=802\n";
    ready << "proof.replay_analysis.player_count=2\n";
    ready << "proof.replay_analysis.snapshot=replay.snapshot.tsv\n";
    ready << "proof.multiplayer_sync.source=live-snet-turn-hooks\n";
    ready << "proof.multiplayer_sync.send_turn_address=0x1400\n";
    ready << "proof.multiplayer_sync.receive_message_address=0x1500\n";
    ready << "proof.multiplayer_sync.snapshot=multiplayer_sync.snapshot.tsv\n";
    ready << "proof.battle_net_policy.status=runtime-process-visible\n";
    ready << "proof.battle_net_policy.game_process_count=1\n";
    ready << "proof.battle_net_policy.blocker_count=0\n";
    ready << "proof.load_ai_modules.loader=dlopen\n";
    ready << "proof.load_ai_modules.module_extension=.dylib\n";
    ready << "proof.load_ai_modules.self_process_smoke=false\n";
    ready << "proof.load_ai_modules.module_path=/tmp/starcraft-api-test-ai-module.dylib\n";
    ready << "proof.load_ai_modules.snapshot=ai_module_load.snapshot.tsv\n";
    ready << "proof.read_map_data.source=live-sc-r-map-tile-array\n";
    ready << "proof.read_map_data.map_name_address=0x1600\n";
    ready << "proof.read_map_data.map_tile_array_address=0x1700\n";
    ready << "proof.read_map_data.tile_count=256\n";
    ready << "proof.read_map_data.snapshot=map.snapshot.tsv\n";
    ready << "proof.read_player_data.player_count=2\n";
    ready << "proof.read_player_data.observed_units=4\n";
    ready << "proof.read_player_data.player_info_projection=true\n";
    ready << "proof.read_player_data.player_info_record_size=128\n";
    ready << "proof.read_player_data.alliance_projection=true\n";
    ready << "proof.read_player_data.projection_source=compat-player-projection-v1:unit-snapshot-derived\n";
    ready << "proof.read_player_data.snapshot=players.snapshot.tsv\n";
    ready << "proof.read_bullet_data.source=live-sc-r-bullet-table\n";
    ready << "proof.read_bullet_data.address=0x1800\n";
    ready << "proof.read_bullet_data.record_size=128\n";
    ready << "proof.read_bullet_data.active_records=2\n";
    ready << "proof.read_bullet_data.unit_correlated_records=2\n";
    ready << "proof.read_bullet_data.snapshot=bullets.snapshot.tsv\n";
    ready << "proof.read_region_data.source=live-bwapi-region-graph\n";
    ready << "proof.read_region_data.region_count=3\n";
    ready << "proof.read_region_data.snapshot=regions.snapshot.tsv\n";
  }

  std::uint64_t nextResidentProofHeartbeat()
  {
    static std::uint64_t heartbeat = 30;
    return heartbeat++;
  }

  std::uint64_t writeResidentStateProofs(
    std::ofstream& ready,
    int processId,
    const std::string& executable,
    const std::filesystem::path& bridgePath = {})
  {
    RuntimeEnvironment environment = RuntimeEnvironment::detectHost();
    environment.product = Product::StarCraftRemastered;
    environment.version = "test-build";
    environment.processId = processId;
    environment.executablePath = executable;
    const std::uint64_t heartbeat = nextResidentProofHeartbeat();
    const std::vector<RuntimeResidentGameStateSample> samples = {
      { 800, 40000 },
      { 801, 40016 },
      { 802, 40032 }
    };
    for (const std::string& line : makeRuntimeResidentAdapterReadyLines(environment, heartbeat))
      ready << line << '\n';
    for (const std::string& line : makeRuntimeResidentReadGameStateProofReadyLines(
           environment,
           heartbeat,
           samples))
      ready << line << '\n';
    ready << "proof.read_game_state.address="
          << reinterpret_cast<std::uintptr_t>(&residentFrameCounter) << '\n';
    for (const std::string& line : makeRuntimeResidentActiveMatchProofReadyLines(
           environment,
           heartbeat,
           6,
           "match"))
      ready << line << '\n';
    ready << "proof.read_units=passed\n";
    ready << "proof.read_units.address="
          << reinterpret_cast<std::uintptr_t>(activeUnitEvidence.data()) << '\n';
    ready << "proof.read_units.record_size=64\n";
    ready << "proof.read_units.active_records=6\n";
    ready << "proof.read_units.snapshot=units.snapshot.tsv\n";
    ready << "proof.active_match_state.evidence=active-unit-node-snapshot\n";
    ready << "proof.active_match_state.active_records=6\n";
    ready << "proof.active_match_state.unit_node_address="
          << reinterpret_cast<std::uintptr_t>(activeUnitEvidence.data()) << '\n';
    ready << "proof.active_match_state.unit_node_record_size=64\n";
    if (!bridgePath.empty())
      writeResidentProofSnapshots(bridgePath, processId, heartbeat, 802, true);
    return heartbeat;
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
  }

  bool residentStateProofAlreadyWritesBehaviorProof(const RuntimeExecutorBehaviorProof& proof)
  {
    return std::string(proof.id) == "read-game-state"
      || std::string(proof.id) == "active-match-state"
      || std::string(proof.id) == "read-units";
  }

  void writeBehaviorProofLines(
    std::ofstream& ready,
    const std::string& omittedBehaviorProof = {})
  {
    for (const RuntimeExecutorBehaviorProof& proof : requiredRuntimeExecutorBehaviorProofs())
    {
      if (residentStateProofAlreadyWritesBehaviorProof(proof))
        continue;
      if (proof.id != omittedBehaviorProof)
        ready << proof.readyFileLine << '\n';
    }
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
    bool includeLiveContractProofs = false,
    const std::string& omittedBehaviorProof = {})
  {
    std::ofstream ready(bridgePath / RuntimeExecutorBridgeReadyFile);
    ready << "protocol=" << RuntimeExecutorBridgeProtocol << '\n';
    ready << "product=starcraft-remastered\n";
    ready << "version=test-build\n";
    ready << "mode=" << RuntimeExecutorBridgeValidatedAdapterMode << '\n';
    writeRuntimeIdentity(ready, processId, executable);
    writeRuntimeCommandQueueSink(ready);
    writeResidentStateProofs(ready, processId, executable, bridgePath);
    writeValidatedProductionProofMetadata(ready);
    if (includeLiveContractProofs)
      writeLiveContractProofs(ready);
    writeBehaviorProofLines(ready, omittedBehaviorProof);
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
    writeResidentStateProofs(ready, processId, executable, bridgePath);
    writeValidatedProductionProofMetadata(ready);
    writeBehaviorProofLines(ready, "multiplayer-sync");
  }
}

int main(int argc, char** argv)
{
  assert(argc > 0);
  ResidentFrameCounterTicker ticker;
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
    true,
    "read-region-data");
  backend = createRuntimeBackend(environment);
  probe = backend->probe();
  preflight = preflightRuntimeExecutor(environment, manifest.manifest.contract);
  readinessContract = applyRuntimeExecutorBridgeContractProofs(environment, manifest.manifest.contract);
  readiness = evaluateProductionReadiness(probe, readinessContract, preflight);

  assert(!probe.supported);
  assert(preflight.executorAvailable);
  assert(preflight.missingBehaviorProofs.size() == 1);
  assert(preflight.missingBehaviorProofs.front() == "proof.read_region_data=passed");
  assert(!preflight.errors.empty());
  assert(!readiness.productionReady);
  assert(!blockingReadinessGaps(readiness).empty());

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
