#include <BWAPI/Runtime/RuntimeBackend.h>
#include <BWAPI/Runtime/RuntimeCommandSurface.h>
#include <BWAPI/Runtime/RuntimeContract.h>
#include <BWAPI/Runtime/RuntimeExecutor.h>
#include <BWAPI/Runtime/RuntimeProcessMemory.h>
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
#include <vector>

using namespace BWAPI::Runtime;

namespace
{
  std::atomic<std::uint32_t> residentFrameCounter{ 102 };
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

  void writeResidentStateProofs(std::ofstream& ready, RuntimeEnvironment environment)
  {
    constexpr std::uint64_t heartbeat = 20;
    const std::vector<RuntimeResidentGameStateSample> samples = {
      { 100, 1000 },
      { 101, 1016 },
      { 102, 1032 }
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
           4,
           "match"))
      ready << line << '\n';
    ready << "proof.read_units=passed\n";
    ready << "proof.read_units.address="
          << reinterpret_cast<std::uintptr_t>(activeUnitEvidence.data()) << '\n';
    ready << "proof.read_units.record_size=64\n";
    ready << "proof.read_units.active_records=4\n";
    ready << "proof.active_match_state.evidence=active-unit-node-snapshot\n";
    ready << "proof.active_match_state.active_records=4\n";
    ready << "proof.active_match_state.unit_node_address="
          << reinterpret_cast<std::uintptr_t>(activeUnitEvidence.data()) << '\n';
    ready << "proof.active_match_state.unit_node_record_size=64\n";
  }

  void writeProofSnapshot(const std::filesystem::path& path, const std::string& proof)
  {
    std::ofstream snapshot(path);
    snapshot << "field\tvalue\n"
             << "proof\t" << proof << '\n'
             << "passed\ttrue\n";
  }

  bool residentStateProofAlreadyWritesBehaviorProof(const RuntimeExecutorBehaviorProof& proof)
  {
    return std::string(proof.id) == "read-game-state"
      || std::string(proof.id) == "active-match-state"
      || std::string(proof.id) == "read-units";
  }

  void writeBehaviorProofLines(std::ofstream& ready)
  {
    for (const RuntimeExecutorBehaviorProof& proof : requiredRuntimeExecutorBehaviorProofs())
    {
      if (residentStateProofAlreadyWritesBehaviorProof(proof))
        continue;
      ready << proof.readyFileLine << '\n';
    }
  }
}

int main()
{
  ResidentFrameCounterTicker ticker;
  RuntimeEnvironment detected = RuntimeEnvironment::detectHost();
  assert(detected.platform != Platform::Unknown);
  assert(std::string(toString(detected.platform)) != "unknown");
  assert(parsePlatform("darwin") == Platform::MacOS);
  assert(parsePlatform("linux") == Platform::Linux);
  assert(parseProduct("scr") == Product::StarCraftRemastered);
  assert(parseProduct("bw_1_16_1") == Product::StarCraftBroodWar1161);

  RuntimeEnvironment remastered = detected;
  remastered.product = Product::StarCraftRemastered;
  remastered.version = "unknown";
  remastered.processId = 0;
  remastered.executablePath.clear();

  std::unique_ptr<RuntimeBackend> remasteredBackend = createRuntimeBackend(remastered);
  RuntimeProbeResult remasteredProbe = remasteredBackend->probe();
  assert(std::string(remasteredBackend->name()) == "starcraft-remastered-runtime");
  assert(!remasteredProbe.supported);
  assert(!remasteredProbe.reason.empty());
  assert(remasteredProbe.capabilities.empty());
  RuntimeCommandSurface remasteredSurface = makeBWAPICommandSurface();
  assert(remasteredProbe.implementedApiSurfaceMethods == makeRemasteredParityContract("unknown").requiredApiSurfaceMethods);
  assert(remasteredProbe.implementedCommandSurfaceEntries == remasteredSurface.totalEntries());
  assert(remasteredProbe.implementedUnitCommands == remasteredSurface.unitCommands);
  assert(remasteredProbe.implementedGameActions == remasteredSurface.gameActions);
  assert(remasteredBackend->state() == RuntimeSessionState::Closed);
  RuntimeOpenResult remasteredOpen = remasteredBackend->open();
  assert(!remasteredOpen.opened);
  assert(remasteredOpen.state == RuntimeSessionState::Failed);
  assert(remasteredBackend->state() == RuntimeSessionState::Failed);
  remasteredBackend->close();
  assert(remasteredBackend->state() == RuntimeSessionState::Closed);

  RuntimeEnvironment attachableRemastered = remastered;
  attachableRemastered.processId = currentProcessId();
  std::unique_ptr<RuntimeBackend> attachableRemasteredBackend = createRuntimeBackend(attachableRemastered);
  RuntimeProbeResult attachableRemasteredProbe = attachableRemasteredBackend->probe();
  assert(!attachableRemasteredProbe.supported);
  assert(hasCapability(attachableRemasteredProbe, Capability::SharedMemoryClient));

  const std::filesystem::path bridgeDir =
    std::filesystem::temp_directory_path() / "starcraft-api-runtime-backend-proof-test";
  std::filesystem::remove_all(bridgeDir);
  std::filesystem::create_directories(bridgeDir);
  {
    std::ofstream issueCommands(bridgeDir / "issue_commands.snapshot.tsv");
    issueCommands << "field\tvalue\n"
                  << "passed\ttrue\n"
                  << "source\tlive-sc-r-command-path\n"
                  << "behavior_checked\ttrue\n";
  }
  writeProofSnapshot(bridgeDir / "draw_overlays.snapshot.tsv", "draw_overlays");
  writeProofSnapshot(bridgeDir / "events.snapshot.tsv", "dispatch_events");
  writeProofSnapshot(bridgeDir / "replay.snapshot.tsv", "replay_analysis");
  writeProofSnapshot(bridgeDir / "multiplayer_sync.snapshot.tsv", "multiplayer_sync");
  writeProofSnapshot(bridgeDir / "ai_module_load.snapshot.tsv", "load_ai_modules");
  writeProofSnapshot(bridgeDir / "map.snapshot.tsv", "read_map_data");
  writeProofSnapshot(bridgeDir / "players.snapshot.tsv", "read_player_data");
  writeProofSnapshot(bridgeDir / "bullets.snapshot.tsv", "read_bullet_data");
  writeProofSnapshot(bridgeDir / "regions.snapshot.tsv", "read_region_data");
  {
    std::ofstream ready(bridgeDir / RuntimeExecutorBridgeReadyFile);
    ready << "protocol=" << RuntimeExecutorBridgeProtocol << '\n';
    ready << "product=starcraft-remastered\n";
    ready << "version=unknown\n";
    ready << "process_id=" << currentProcessId() << '\n';
    ready << "executor=unit-test\n";
    ready << "mode=" << RuntimeExecutorBridgeValidatedAdapterMode << '\n';
    ready << "proof.attach=passed\n";
    RuntimeEnvironment proofEnvironment = attachableRemastered;
    proofEnvironment.executorBridgePath = bridgeDir.string();
    writeResidentStateProofs(ready, proofEnvironment);
  }

  RuntimeEnvironment proofBackedRemastered = attachableRemastered;
  proofBackedRemastered.executorBridgePath = bridgeDir.string();
  std::unique_ptr<RuntimeBackend> proofBackedRemasteredBackend = createRuntimeBackend(proofBackedRemastered);
  RuntimeProbeResult proofBackedRemasteredProbe = proofBackedRemasteredBackend->probe();
  assert(hasCapability(proofBackedRemasteredProbe, Capability::SharedMemoryClient));
  assert(hasCapability(proofBackedRemasteredProbe, Capability::ReadGameState));
  assert(hasCapability(proofBackedRemasteredProbe, Capability::ReadUnitData));
  assert(proofBackedRemasteredProbe.implementedCommandSurfaceEntries == remasteredSurface.totalEntries());
  assert(proofBackedRemasteredProbe.implementedUnitCommands == remasteredSurface.unitCommands);
  assert(proofBackedRemasteredProbe.implementedGameActions == remasteredSurface.gameActions);

  {
    std::ofstream ready(bridgeDir / RuntimeExecutorBridgeReadyFile);
    ready << "protocol=" << RuntimeExecutorBridgeProtocol << '\n';
    ready << "product=starcraft-remastered\n";
    ready << "version=unknown\n";
    ready << "process_id=" << currentProcessId() << '\n';
    ready << "executor=unit-test\n";
    ready << "mode=" << RuntimeExecutorBridgeValidatedAdapterMode << '\n';
    ready << "proof.attach=passed\n";
    ready << RuntimeExecutorBridgeCommandSurfaceLine << '\n';
  }

  RuntimeProbeResult commandSurfaceOnlyProbe = proofBackedRemasteredBackend->probe();
  assert(!hasCapability(commandSurfaceOnlyProbe, Capability::IssueCommands));
  assert(commandSurfaceOnlyProbe.implementedCommandSurfaceEntries == remasteredSurface.totalEntries());
  assert(commandSurfaceOnlyProbe.implementedUnitCommands == remasteredSurface.unitCommands);
  assert(commandSurfaceOnlyProbe.implementedGameActions == remasteredSurface.gameActions);

  {
    std::ofstream ready(bridgeDir / RuntimeExecutorBridgeReadyFile);
    ready << "protocol=" << RuntimeExecutorBridgeProtocol << '\n';
    ready << "product=starcraft-remastered\n";
    ready << "version=unknown\n";
    ready << "process_id=" << currentProcessId() << '\n';
    ready << "executor=unit-test\n";
    ready << "mode=" << RuntimeExecutorBridgeValidatedAdapterMode << '\n';
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
    ready << "proof.attach=passed\n";
    ready << "proof.issue_commands=passed\n";
  }

  RuntimeProbeResult issueCommandOnlyProbe = proofBackedRemasteredBackend->probe();
  assert(hasCapability(issueCommandOnlyProbe, Capability::IssueCommands));
  assert(!hasCapability(issueCommandOnlyProbe, Capability::DrawOverlays));
  assert(issueCommandOnlyProbe.implementedCommandSurfaceEntries == remasteredSurface.totalEntries());
  assert(issueCommandOnlyProbe.implementedUnitCommands == remasteredSurface.unitCommands);
  assert(issueCommandOnlyProbe.implementedGameActions == remasteredSurface.gameActions);

  {
    std::ofstream ready(bridgeDir / RuntimeExecutorBridgeReadyFile);
    ready << "protocol=" << RuntimeExecutorBridgeProtocol << '\n';
    ready << "product=starcraft-remastered\n";
    ready << "version=unknown\n";
    ready << "process_id=" << currentProcessId() << '\n';
    ready << "executor=unit-test\n";
    ready << "mode=" << RuntimeExecutorBridgeValidatedAdapterMode << '\n';
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
    ready << "proof.replay_analysis.first_frame=100\n";
    ready << "proof.replay_analysis.last_frame=140\n";
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
    ready << "proof.read_bullet_data.snapshot=bullets.snapshot.tsv\n";
    ready << "proof.read_region_data.source=live-bwapi-region-graph\n";
    ready << "proof.read_region_data.region_count=3\n";
    ready << "proof.read_region_data.snapshot=regions.snapshot.tsv\n";
    writeBehaviorProofLines(ready);
    ready << "proof.read_map_data=passed\n";
    ready << "proof.read_player_data=passed\n";
    ready << "proof.read_bullet_data=passed\n";
  }

  RuntimeProbeResult commandProofRemasteredProbe = proofBackedRemasteredBackend->probe();
  assert(hasCapability(commandProofRemasteredProbe, Capability::IssueCommands));
  assert(hasCapability(commandProofRemasteredProbe, Capability::DrawOverlays));
  assert(hasCapability(commandProofRemasteredProbe, Capability::ReadMapData));
  assert(hasCapability(commandProofRemasteredProbe, Capability::ReadPlayerData));
  assert(hasCapability(commandProofRemasteredProbe, Capability::ReadBulletData));
  assert(hasCapability(commandProofRemasteredProbe, Capability::ReadRegionData));
  assert(hasCapability(commandProofRemasteredProbe, Capability::LoadAIModules));
  assert(commandProofRemasteredProbe.implementedCommandSurfaceEntries == remasteredSurface.totalEntries());
  assert(commandProofRemasteredProbe.implementedUnitCommands == remasteredSurface.unitCommands);
  assert(commandProofRemasteredProbe.implementedGameActions == remasteredSurface.gameActions);
  std::filesystem::remove_all(bridgeDir);

  RuntimeEnvironment legacy = detected;
  legacy.product = Product::StarCraftBroodWar1161;
  legacy.version = "1.16.1";

  std::unique_ptr<RuntimeBackend> legacyBackend = createRuntimeBackend(legacy);
  RuntimeProbeResult legacyProbe = legacyBackend->probe();
  assert(std::string(legacyBackend->name()) == "legacy-bwapi-1.16.1-runtime");
  assert(!legacyProbe.supported);
  assert(!legacyProbe.reason.empty());
  assert(std::string(toString(RuntimeSessionState::Closed)) == "closed");

  return 0;
}
