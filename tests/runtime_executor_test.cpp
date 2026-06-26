#include <BWAPI/Runtime/RuntimeExecutor.h>
#include <BWAPI/Runtime/RuntimeManifest.h>
#include <BWAPI/Runtime/RuntimeProcessMemory.h>
#include <BWAPI/Runtime/RuntimeResidentBridge.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace BWAPI::Runtime;

namespace
{
  std::atomic<std::uint32_t> residentFrameCounter{ 102 };
  std::array<unsigned char, 64> activeUnitEvidence = {
    0x42, 0x57, 0x41, 0x50, 0x49, 0x2d, 0x55, 0x4e
  };
  std::uint32_t issueCommandBytesInQueue = 0;
  std::array<unsigned char, 64> issueCommandVectorEvidence = {
    0x42, 0x57, 0x41, 0x50, 0x49, 0x2d, 0x43, 0x4d
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

  RuntimeEnvironment remasteredEnvironment(std::string executablePath)
  {
    RuntimeEnvironment environment = RuntimeEnvironment::detectHost();
    environment.product = Product::StarCraftRemastered;
    environment.version = "test-build";
    environment.processId = currentProcessId();
    environment.executablePath = std::move(executablePath);
    environment.manifestPath = fixturePath("remastered-complete.manifest");
    return environment;
  }

  void writeResidentSnapshotPayload(
    std::ofstream& snapshot,
    const std::string& proof,
    std::uint64_t replayLastFrame = 140)
  {
    if (proof == "read_units")
    {
      snapshot << "index\tnode\tsecondary\tsprite\tid\tx\ty\ttarget_x\ttarget_y\torder\tstate\tplayer\ttype_hint\thit_points\n";
      for (int i = 0; i < 4; ++i)
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
               << "active-match-live-metadata\tfalse\ttrue\tUnitTest\t100\t"
               << replayLastFrame << "\t2\n";
      return;
    }
    if (proof == "issue_commands")
    {
      snapshot << "field\tvalue\n"
               << "passed\ttrue\n"
               << "delivery_checked\ttrue\n"
               << "behavior_checked\ttrue\n"
               << "live_behavior_witness\tstarcraft-runtime-adapter-proof-live-write-v1\n"
               << "self_fixture\tfalse\n"
               << "receiver_active\ttrue\n"
               << "stale_proof_bytes_cleared\ttrue\n"
               << "pause_frame_counter_sampled\ttrue\n"
               << "pause_frame_counter_matched\ttrue\n"
               << "frame_counter_candidate_count\t1\n"
               << "issue_commands_required_adapter_abi\tstarcraft-api-resident-adapter-v1\n"
               << "issue_commands_required_adapter_location\tin-process-target-runtime\n"
               << "issue_commands_required_adapter_thread_policy\texecute-on-target-runtime-thread\n"
               << "issue_commands_required_adapter_behavior\tencoded-bwapi-command-reaches-live-scr-command-path-and-changes-frame-behavior\n"
               << "issue_commands_required_adapter_promotion_rule\tdo-not-emit-production-proof-until-live-behavior-is-observed\n"
               << "command\tpauseGame/resumeGame\n"
               << "encoded_bytes\t10 / 11\n"
               << "attempt_count\t1\n"
               << "storage_kind\tlive-sc-r-command-queue-v1\n"
               << "vector_address\t"
               << reinterpret_cast<std::uintptr_t>(issueCommandVectorEvidence.data()) << '\n'
               << "bytes_in_queue_address\t"
               << reinterpret_cast<std::uintptr_t>(&issueCommandBytesInQueue) << '\n'
               << "buffer_begin\t"
               << reinterpret_cast<std::uintptr_t>(issueCommandVectorEvidence.data()) << '\n'
               << "frame_counter_address\t"
               << reinterpret_cast<std::uintptr_t>(&residentFrameCounter) << '\n'
               << "original_used_bytes\t0\n"
               << "appended_bytes\t1\n"
               << "baseline_delta\t12\n"
               << "paused_delta\t0\n"
               << "resumed_delta\t12\n";
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

  std::filesystem::path makeBridgePath(
    const std::string& name = "starcraft-api-runtime-executor-test")
  {
    std::filesystem::path path = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    writeResidentProofSnapshots(path, currentProcessId(), 20, 102, true);
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
    ready << "proof.issue_commands.command=pauseGame/resumeGame\n";
    ready << "proof.issue_commands.source=live-sc-r-command-path\n";
    ready << "proof.issue_commands.delivery_checked=true\n";
    ready << "proof.issue_commands.behavior_checked=true\n";
    ready << "proof.issue_commands.live_behavior_witness=starcraft-runtime-adapter-proof-live-write-v1\n";
    ready << "proof.issue_commands.self_fixture=false\n";
    ready << "proof.issue_commands.pause_frame_counter_matched=true\n";
    ready << "proof.issue_commands.vector_address="
          << reinterpret_cast<std::uintptr_t>(issueCommandVectorEvidence.data()) << '\n';
    ready << "proof.issue_commands.storage_kind=live-sc-r-command-queue-v1\n";
    ready << "proof.issue_commands.bytes_in_queue_address="
          << reinterpret_cast<std::uintptr_t>(&issueCommandBytesInQueue) << '\n';
    ready << "proof.issue_commands.frame_counter_address="
          << reinterpret_cast<std::uintptr_t>(&residentFrameCounter) << '\n';
    ready << "proof.issue_commands.encoded_bytes=10 / 11\n";
    ready << "proof.issue_commands.stale_proof_bytes_cleared=true\n";
    ready << "proof.issue_commands.snapshot=issue_commands.snapshot.tsv\n";
  }

  std::uint64_t nextResidentProofHeartbeat()
  {
    static std::uint64_t heartbeat = 20;
    return heartbeat++;
  }

  std::uint64_t writeResidentStateProofs(
    std::ofstream& ready,
    int processId,
    const std::string& executable,
    const std::filesystem::path& bridgePath = {})
  {
    RuntimeEnvironment residentEnvironment = remasteredEnvironment(executable);
    residentEnvironment.processId = processId;
    const std::uint64_t heartbeat = nextResidentProofHeartbeat();
    const std::vector<RuntimeResidentGameStateSample> samples = {
      { 100, 1000 },
      { 101, 1016 },
      { 102, 1032 }
    };
    for (const std::string& line : makeRuntimeResidentAdapterReadyLines(residentEnvironment, heartbeat))
      ready << line << '\n';
    for (const std::string& line : makeRuntimeResidentReadGameStateProofReadyLines(
           residentEnvironment,
           heartbeat,
           samples))
      ready << line << '\n';
    ready << "proof.read_game_state.address="
          << reinterpret_cast<std::uintptr_t>(&residentFrameCounter) << '\n';
    for (const std::string& line : makeRuntimeResidentActiveMatchProofReadyLines(
           residentEnvironment,
           heartbeat,
           4,
           "match"))
      ready << line << '\n';
    ready << "proof.read_units=passed\n";
    ready << "proof.read_units.address="
          << reinterpret_cast<std::uintptr_t>(activeUnitEvidence.data()) << '\n';
    ready << "proof.read_units.record_size=64\n";
    ready << "proof.read_units.active_records=4\n";
    ready << "proof.read_units.snapshot=units.snapshot.tsv\n";
    ready << "proof.active_match_state.evidence=active-unit-node-snapshot\n";
    ready << "proof.active_match_state.active_records=4\n";
    ready << "proof.active_match_state.unit_node_address="
          << reinterpret_cast<std::uintptr_t>(activeUnitEvidence.data()) << '\n';
    ready << "proof.active_match_state.unit_node_record_size=64\n";
    if (!bridgePath.empty())
      writeResidentProofSnapshots(bridgePath, processId, heartbeat, 102, true);
    return heartbeat;
  }

  void writeValidatedProductionProofMetadata(std::ofstream& ready)
  {
    ready << "proof.attach.source=resident-adapter\n";
    ready << "proof.attach.queue=resident-proof.queue\n";
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
    ready << "proof.read_bullet_data.active_records=2\n";
    ready << "proof.read_bullet_data.unit_correlated_records=2\n";
    ready << "proof.read_bullet_data.snapshot=bullets.snapshot.tsv\n";
    ready << "proof.read_region_data.source=live-bwapi-region-graph\n";
    ready << "proof.read_region_data.region_count=3\n";
    ready << "proof.read_region_data.snapshot=regions.snapshot.tsv\n";
  }

  void writeResidentProofQueueReadyLines(
    std::ofstream& ready,
    const std::filesystem::path& bridgePath,
    std::uint64_t heartbeat)
  {
    const std::filesystem::path proofQueuePath = bridgePath / RuntimeResidentProofQueueFile;
    const RuntimeResidentQueueHeader desiredQueue =
      makeRuntimeResidentQueueHeader(
        RuntimeResidentQueueKind::Proof,
        sizeof(RuntimeResidentRecordHeader),
        64,
        heartbeat);
    RuntimeResidentQueueHeader actualQueue;
    RuntimeResidentQueueValidationResult ensuredQueue =
      ensureRuntimeResidentQueueFile(proofQueuePath, desiredQueue, actualQueue);
    if (!ensuredQueue.valid)
    {
      for (const std::string& error : ensuredQueue.errors)
        std::cerr << "resident proof queue fixture error: " << error << '\n';
    }
    assert(ensuredQueue.valid);
    for (const std::string& line : makeRuntimeResidentQueueReadyLines(
           RuntimeResidentQueueKind::Proof,
           RuntimeResidentProofQueueFile,
           actualQueue))
    {
      ready << line << '\n';
    }
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
    const std::string& executable)
  {
    std::ofstream ready(bridgePath / RuntimeExecutorBridgeReadyFile);
    ready << "protocol=" << RuntimeExecutorBridgeProtocol << '\n';
    ready << "product=starcraft-remastered\n";
    ready << "version=test-build\n";
    ready << "executor=starcraft-api-resident-adapter\n";
    ready << "mode=" << RuntimeExecutorBridgeValidatedAdapterMode << '\n';
    writeRuntimeIdentity(ready, processId, executable);
    writeRuntimeCommandQueueSink(ready);
    const std::uint64_t heartbeat =
      writeResidentStateProofs(ready, processId, executable, bridgePath);
    writeResidentProofQueueReadyLines(ready, bridgePath, heartbeat);
    writeValidatedProductionProofMetadata(ready);
    writeBehaviorProofLines(ready);
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
    ready << "executor=starcraft-api-resident-adapter\n";
    ready << "mode=" << RuntimeExecutorBridgeValidatedAdapterMode << '\n';
    writeRuntimeIdentity(ready, processId, executable);
    writeRuntimeCommandQueueSink(ready);
    const std::uint64_t heartbeat =
      writeResidentStateProofs(ready, processId, executable, bridgePath);
    writeResidentProofQueueReadyLines(ready, bridgePath, heartbeat);
    writeValidatedProductionProofMetadata(ready);
    writeBehaviorProofLines(ready, "multiplayer-sync");
  }

  void writeDirectValidatedAdapterReadyFile(
    const std::filesystem::path& bridgePath,
    int processId,
    const std::string& executable,
    std::uintptr_t bytesInQueueAddress,
    std::uintptr_t turnBufferAddress,
    const std::string& evidenceProof = "proof.issue_commands=passed")
  {
    std::ofstream ready(bridgePath / RuntimeExecutorBridgeReadyFile);
    ready << "protocol=" << RuntimeExecutorBridgeProtocol << '\n';
    ready << "product=starcraft-remastered\n";
    ready << "version=test-build\n";
    ready << "executor=starcraft-api-resident-adapter\n";
    ready << "mode=" << RuntimeExecutorBridgeValidatedAdapterMode << '\n';
    writeRuntimeIdentity(ready, processId, executable);
    ready << RuntimeExecutorBridgeActiveCommandReceiverLine << '\n';
    ready << RuntimeExecutorBridgeRuntimeCommandQueueSinkLine << '\n';
    ready << "contract.binding.BW::BWDATA::sgdwBytesInCmdQueue=command-queue|" << evidenceProof << ':'
          << bytesInQueueAddress << '\n';
    ready << "contract.binding.BW::BWDATA::TurnBuffer=command-queue|" << evidenceProof << ':'
          << turnBufferAddress << '\n';
    ready << "proof.issue_commands.command=pauseGame/resumeGame\n";
    ready << "proof.issue_commands.source=live-sc-r-command-path\n";
    ready << "proof.issue_commands.delivery_checked=true\n";
    ready << "proof.issue_commands.behavior_checked=true\n";
    ready << "proof.issue_commands.self_fixture=false\n";
    ready << "proof.issue_commands.pause_frame_counter_matched=true\n";
    ready << "proof.issue_commands.vector_address=" << turnBufferAddress << '\n';
    ready << "proof.issue_commands.storage_kind=live-sc-r-direct-turn-buffer-v1\n";
    ready << "proof.issue_commands.bytes_in_queue_address=" << bytesInQueueAddress << '\n';
    ready << "proof.issue_commands.frame_counter_address=0x1200\n";
    ready << "proof.issue_commands.encoded_bytes=10 / 11\n";
    ready << "proof.issue_commands.stale_proof_bytes_cleared=true\n";
    ready << "proof.issue_commands.snapshot=issue_commands.snapshot.tsv\n";
    const std::uint64_t heartbeat =
      writeResidentStateProofs(ready, processId, executable, bridgePath);
    writeResidentProofQueueReadyLines(ready, bridgePath, heartbeat);
    writeValidatedProductionProofMetadata(ready);
    writeBehaviorProofLines(ready);
  }

  void writeMismatchedRuntimeIdentityReadyFile(
    const std::filesystem::path& bridgePath,
    int processId,
    const std::string& executable)
  {
    std::ofstream ready(bridgePath / RuntimeExecutorBridgeReadyFile);
    ready << "protocol=" << RuntimeExecutorBridgeProtocol << '\n';
    ready << "product=starcraft-remastered\n";
    ready << "version=test-build\n";
    ready << "executor=starcraft-api-resident-adapter\n";
    ready << "mode=" << RuntimeExecutorBridgeValidatedAdapterMode << '\n';
    writeRuntimeIdentity(ready, processId + 100000, executable);
    writeRuntimeCommandQueueSink(ready);
    const std::uint64_t heartbeat =
      writeResidentStateProofs(ready, processId + 100000, executable, bridgePath);
    writeResidentProofQueueReadyLines(ready, bridgePath, heartbeat);
    writeValidatedProductionProofMetadata(ready);
    writeBehaviorProofLines(ready);
  }

  bool hasErrorContaining(
    const std::vector<std::string>& errors,
    const std::string& needle)
  {
    for (const std::string& error : errors)
    {
      if (error.find(needle) != std::string::npos)
        return true;
    }
    return false;
  }

  bool hasWarningContaining(
    const std::vector<std::string>& warnings,
    const std::string& needle)
  {
    for (const std::string& warning : warnings)
    {
      if (warning.find(needle) != std::string::npos)
        return true;
    }
    return false;
  }

  bool hasMissingProof(
    const RuntimeExecutorPreflightResult& preflight,
    const std::string& proof)
  {
    return std::find(
      preflight.missingBehaviorProofs.begin(),
      preflight.missingBehaviorProofs.end(),
      proof) != preflight.missingBehaviorProofs.end();
  }
}

int main(int argc, char** argv)
{
  assert(argc > 0);
  ResidentFrameCounterTicker ticker;
  const std::string selfExecutable = std::filesystem::absolute(argv[0]).lexically_normal().string();

  const std::vector<RuntimeExecutorBehaviorProof>& proofs = requiredRuntimeExecutorBehaviorProofs();
  assert(proofs.size() == 12);
  assert(std::string(proofs.front().readyFileLine) == "proof.attach=passed");
  assert(std::string(proofs[10].readyFileLine) == "proof.battle_net_policy=passed");
  assert(std::string(proofs.back().readyFileLine) == "proof.load_ai_modules=passed");

  RuntimeManifestLoadResult complete = loadRuntimeManifestFile(fixturePath("remastered-complete.manifest"));
  assert(complete.loaded);

  RuntimeExecutorPreflightResult readyPrerequisites =
    preflightRuntimeExecutor(remasteredEnvironment(selfExecutable), complete.manifest.contract);
  assert(readyPrerequisites.contractValid);
  assert(readyPrerequisites.processIdentified);
  assert(readyPrerequisites.memoryAccessible);
  assert(readyPrerequisites.targetLocated);
  assert(!readyPrerequisites.executorAvailable);
  assert(readyPrerequisites.missingBehaviorProofs.size() == proofs.size());
  assert(!readyPrerequisites.warnings.empty());

  RuntimeExecutorPreflightResult missingTarget =
    preflightRuntimeExecutor(remasteredEnvironment(fixturePath("missing-starcraft")), complete.manifest.contract);
  assert(missingTarget.contractValid);
  assert(!missingTarget.processIdentified);
  assert(!missingTarget.memoryAccessible);
  assert(!missingTarget.targetLocated);
  assert(!missingTarget.executorAvailable);
  assert(missingTarget.missingBehaviorProofs.size() == proofs.size());
  assert(!missingTarget.errors.empty());

  RuntimeContract unresolved = makeRemasteredParityContract("test-build");
  RuntimeExecutorPreflightResult invalidContract =
    preflightRuntimeExecutor(remasteredEnvironment(selfExecutable), unresolved);
  assert(!invalidContract.contractValid);
  assert(invalidContract.processIdentified);
  assert(invalidContract.memoryAccessible);
  assert(invalidContract.targetLocated);
  assert(!invalidContract.executorAvailable);
  assert(invalidContract.missingBehaviorProofs.size() == proofs.size());
  assert(!invalidContract.errors.empty());

  std::filesystem::path bridgePath = makeBridgePath();
  RuntimeEnvironment bridgeEnvironment = remasteredEnvironment(selfExecutable);
  bridgeEnvironment.executorBridgePath = bridgePath.string();
  writeBootstrapReadyFile(bridgePath, bridgeEnvironment.processId, bridgeEnvironment.executablePath);
  RuntimeExecutorPreflightResult bootstrapPreflight =
    preflightRuntimeExecutor(bridgeEnvironment, complete.manifest.contract);
  assert(bootstrapPreflight.contractValid);
  assert(bootstrapPreflight.processIdentified);
  assert(bootstrapPreflight.memoryAccessible);
  assert(bootstrapPreflight.targetLocated);
  assert(!bootstrapPreflight.executorAvailable);
  assert(bootstrapPreflight.executorName == "filesystem-bridge-bootstrap");
  assert(bootstrapPreflight.executorBridgeMode == RuntimeExecutorBridgeBootstrapMode);
  assert(!bootstrapPreflight.missingBehaviorProofs.empty());
  assert(!bootstrapPreflight.errors.empty());

  {
    std::ofstream ready(bridgePath / RuntimeExecutorBridgeReadyFile);
    ready << "protocol=" << RuntimeExecutorBridgeProtocol << '\n';
    ready << "product=starcraft-remastered\n";
    ready << "version=test-build\n";
    ready << "mode=" << RuntimeExecutorBridgeValidatedAdapterMode << '\n';
    writeRuntimeIdentity(ready, bridgeEnvironment.processId, bridgeEnvironment.executablePath);
    ready << "proof.issue_commands=passed\n";
    ready << "contract.binding.BW::BWDATA::Game=data-address|proof.issue_commands=passed:wrong-surface\n";
    ready << "contract.structure.BW::CUnit=336|fixture:cunit-layout\n";
    ready << "contract.field.BW::CUnit.position=40|4|proof.issue_commands=passed:wrong-surface\n";
  }
  RuntimeContract rejectedSemanticProofs =
    applyRuntimeExecutorBridgeContractProofs(bridgeEnvironment, makeRemasteredParityContract("test-build"));
  const RuntimeBinding* rejectedGameBinding =
    findRuntimeBinding(rejectedSemanticProofs, "BW::BWDATA::Game", BindingKind::DataAddress);
  const StructureLayout* rejectedUnitLayout =
    findStructureLayout(rejectedSemanticProofs, "BW::CUnit");
  const StructureField* rejectedPositionField =
    findStructureField(rejectedSemanticProofs, "BW::CUnit", "position");
  assert(rejectedGameBinding != nullptr && !rejectedGameBinding->resolved);
  assert(rejectedUnitLayout != nullptr && rejectedUnitLayout->size == 0);
  assert(rejectedPositionField != nullptr && !rejectedPositionField->resolved);

  {
    std::ofstream ready(bridgePath / RuntimeExecutorBridgeReadyFile);
    ready << "protocol=" << RuntimeExecutorBridgeProtocol << '\n';
    ready << "product=starcraft-remastered\n";
    ready << "version=test-build\n";
    ready << "mode=" << RuntimeExecutorBridgeValidatedAdapterMode << '\n';
    writeRuntimeIdentity(ready, bridgeEnvironment.processId, bridgeEnvironment.executablePath);
    writeResidentStateProofs(
      ready,
      bridgeEnvironment.processId,
      bridgeEnvironment.executablePath,
      bridgePath);
    ready << "contract.structure.BW::CUnit=336|proof.read_units=passed:cunit-layout\n";
    ready << "contract.field.BW::CUnit.position=40|4|proof.read_units=passed:cunit-position\n";
  }
  RuntimeContract acceptedSemanticProofs =
    applyRuntimeExecutorBridgeContractProofs(bridgeEnvironment, makeRemasteredParityContract("test-build"));
  const StructureLayout* acceptedUnitLayout =
    findStructureLayout(acceptedSemanticProofs, "BW::CUnit");
  const StructureField* acceptedPositionField =
    findStructureField(acceptedSemanticProofs, "BW::CUnit", "position");
  assert(acceptedUnitLayout != nullptr && acceptedUnitLayout->size == 336);
  assert(acceptedUnitLayout->evidence == "proof.read_units=passed:cunit-layout");
  assert(acceptedPositionField != nullptr && acceptedPositionField->resolved);
  assert(acceptedPositionField->evidence == "proof.read_units=passed:cunit-position");

  {
    std::ofstream ready(bridgePath / RuntimeExecutorBridgeReadyFile);
    ready << "protocol=" << RuntimeExecutorBridgeProtocol << '\n';
    ready << "product=starcraft-remastered\n";
    ready << "version=test-build\n";
    ready << "mode=" << RuntimeExecutorBridgeValidatedAdapterMode << '\n';
    writeRuntimeIdentity(ready, bridgeEnvironment.processId, bridgeEnvironment.executablePath);
    const std::uint64_t heartbeat =
      writeResidentStateProofs(ready, bridgeEnvironment.processId, bridgeEnvironment.executablePath);
    ready << "proof.read_player_data=passed\n";
    ready << "proof.read_player_data.player_count=2\n";
    ready << "proof.read_player_data.observed_units=4\n";
    ready << "proof.read_player_data.player_info_projection=true\n";
    ready << "proof.read_player_data.player_info_record_size=128\n";
    ready << "proof.read_player_data.alliance_projection=true\n";
    ready << "proof.read_player_data.projection_source=compat-player-projection-v1:unit-snapshot-derived\n";
    ready << "proof.read_player_data.snapshot=players.snapshot.tsv\n";
    ready << "contract.binding.BW::BWDATA::Players=data-address|proof.read_player_data=passed:compat-player-projection-v1:unit-snapshot-derived\n";
    ready << "contract.field.BW::BWGame.players=0|4|proof.read_player_data=passed\n";
    ready << "contract.field.BW::BWGame.alliance=4|4|proof.read_player_data=passed:compat-alliance-mask\n";
    ready << "contract.structure.BW::PlayerInfo=128|proof.read_player_data=passed:compat-player-projection-v1:unit-snapshot-derived\n";
    ready << "contract.field.BW::PlayerInfo.stormId=0|4|proof.read_player_data=passed\n";
    ready << "contract.field.BW::PlayerInfo.race=4|4|proof.read_player_data=passed\n";
    ready << "contract.field.BW::PlayerInfo.resources=8|8|proof.read_player_data=passed:projection-unresolved-values\n";
    ready << "contract.field.BW::PlayerInfo.supply=16|8|proof.read_player_data=passed:projection-unresolved-values\n";
    writeResidentProofSnapshot(
      bridgePath / "players.snapshot.tsv",
      "read_player_data",
      bridgeEnvironment.processId,
      heartbeat,
      102,
      true);
  }
  RuntimeContract acceptedPlayerProjectionProof =
    applyRuntimeExecutorBridgeContractProofs(bridgeEnvironment, makeRemasteredParityContract("test-build"));
  const RuntimeBinding* acceptedPlayersBinding =
    findRuntimeBinding(acceptedPlayerProjectionProof, "BW::BWDATA::Players", BindingKind::DataAddress);
  const StructureField* acceptedPlayersField =
    findStructureField(acceptedPlayerProjectionProof, "BW::BWGame", "players");
  const StructureField* acceptedAllianceField =
    findStructureField(acceptedPlayerProjectionProof, "BW::BWGame", "alliance");
  const StructureLayout* acceptedPlayerInfoLayout =
    findStructureLayout(acceptedPlayerProjectionProof, "BW::PlayerInfo");
  const StructureField* acceptedSupplyField =
    findStructureField(acceptedPlayerProjectionProof, "BW::PlayerInfo", "supply");
  assert(acceptedPlayersBinding != nullptr && acceptedPlayersBinding->resolved);
  assert(acceptedPlayersField != nullptr && acceptedPlayersField->resolved);
  assert(acceptedAllianceField != nullptr && acceptedAllianceField->resolved);
  assert(acceptedPlayerInfoLayout != nullptr && acceptedPlayerInfoLayout->size == 128);
  assert(acceptedSupplyField != nullptr && acceptedSupplyField->resolved);

  auto writePlayerProjectionReady = [&](const std::string& snapshotPath) -> std::uint64_t
  {
    std::ofstream ready(bridgePath / RuntimeExecutorBridgeReadyFile);
    ready << "protocol=" << RuntimeExecutorBridgeProtocol << '\n';
    ready << "product=starcraft-remastered\n";
    ready << "version=test-build\n";
    ready << "mode=" << RuntimeExecutorBridgeValidatedAdapterMode << '\n';
    writeRuntimeIdentity(ready, bridgeEnvironment.processId, bridgeEnvironment.executablePath);
    const std::uint64_t heartbeat =
      writeResidentStateProofs(ready, bridgeEnvironment.processId, bridgeEnvironment.executablePath);
    ready << "proof.read_player_data=passed\n";
    ready << "proof.read_player_data.player_count=2\n";
    ready << "proof.read_player_data.observed_units=4\n";
    ready << "proof.read_player_data.player_info_projection=true\n";
    ready << "proof.read_player_data.player_info_record_size=128\n";
    ready << "proof.read_player_data.alliance_projection=true\n";
    ready << "proof.read_player_data.projection_source=compat-player-projection-v1:unit-snapshot-derived\n";
    ready << "proof.read_player_data.snapshot=" << snapshotPath << '\n';
    ready << "contract.binding.BW::BWDATA::Players=data-address|proof.read_player_data=passed:compat-player-projection-v1:unit-snapshot-derived\n";
    return heartbeat;
  };

  {
    const std::uint64_t heartbeat = writePlayerProjectionReady("players.snapshot.tsv");
    (void)heartbeat;
    std::ofstream playersSnapshot(bridgePath / "players.snapshot.tsv");
    playersSnapshot << "player\tunit_count\n0\t2\n1\t2\n";
    RuntimeContract rejectedSchemaLessPlayerSnapshot =
      applyRuntimeExecutorBridgeContractProofs(bridgeEnvironment, makeRemasteredParityContract("test-build"));
    const RuntimeBinding* rejectedPlayersBinding =
      findRuntimeBinding(rejectedSchemaLessPlayerSnapshot, "BW::BWDATA::Players", BindingKind::DataAddress);
    assert(rejectedPlayersBinding != nullptr && !rejectedPlayersBinding->resolved);
  }

  {
    const std::uint64_t heartbeat = writePlayerProjectionReady("players.snapshot.tsv");
    writeResidentProofSnapshot(
      bridgePath / "players.snapshot.tsv",
      "read_player_data",
      bridgeEnvironment.processId,
      heartbeat,
      99,
      true);
    RuntimeContract rejectedStaleFramePlayerSnapshot =
      applyRuntimeExecutorBridgeContractProofs(bridgeEnvironment, makeRemasteredParityContract("test-build"));
    const RuntimeBinding* rejectedPlayersBinding =
      findRuntimeBinding(rejectedStaleFramePlayerSnapshot, "BW::BWDATA::Players", BindingKind::DataAddress);
    assert(rejectedPlayersBinding != nullptr && !rejectedPlayersBinding->resolved);
  }

  {
    const std::uint64_t heartbeat = writePlayerProjectionReady("players.snapshot.tsv");
    writeResidentProofSnapshot(
      bridgePath / "players.snapshot.tsv",
      "read_player_data",
      bridgeEnvironment.processId,
      heartbeat,
      102,
      false);
    RuntimeContract rejectedUncorrelatedPlayerSnapshot =
      applyRuntimeExecutorBridgeContractProofs(bridgeEnvironment, makeRemasteredParityContract("test-build"));
    const RuntimeBinding* rejectedPlayersBinding =
      findRuntimeBinding(rejectedUncorrelatedPlayerSnapshot, "BW::BWDATA::Players", BindingKind::DataAddress);
    assert(rejectedPlayersBinding != nullptr && !rejectedPlayersBinding->resolved);
  }

  {
    const std::uint64_t heartbeat = writePlayerProjectionReady("players.snapshot.tsv");
    writeResidentProofSnapshot(
      bridgePath / "players.snapshot.tsv",
      "read_player_data",
      bridgeEnvironment.processId + 1,
      heartbeat,
      102,
      true);
    RuntimeContract rejectedWrongProcessPlayerSnapshot =
      applyRuntimeExecutorBridgeContractProofs(bridgeEnvironment, makeRemasteredParityContract("test-build"));
    const RuntimeBinding* rejectedPlayersBinding =
      findRuntimeBinding(rejectedWrongProcessPlayerSnapshot, "BW::BWDATA::Players", BindingKind::DataAddress);
    assert(rejectedPlayersBinding != nullptr && !rejectedPlayersBinding->resolved);
  }

  {
    const std::uint64_t heartbeat = writePlayerProjectionReady("players.snapshot.tsv");
    std::ofstream playersSnapshot(bridgePath / "players.snapshot.tsv");
    playersSnapshot << "# schema=starcraft-api.resident-snapshot.v1\n"
                    << "# proof=read_player_data\n"
                    << "# source_identity=resident-adapter\n"
                    << "# process_id=" << bridgeEnvironment.processId << '\n'
                    << "# heartbeat=" << heartbeat << '\n'
                    << "# frame_id=102\n"
                    << "# active_match_correlated=true\n"
                    << "field\tvalue\n"
                    << "passed\ttrue\n";
    RuntimeContract rejectedGenericPayloadPlayerSnapshot =
      applyRuntimeExecutorBridgeContractProofs(bridgeEnvironment, makeRemasteredParityContract("test-build"));
    const RuntimeBinding* rejectedPlayersBinding =
      findRuntimeBinding(rejectedGenericPayloadPlayerSnapshot, "BW::BWDATA::Players", BindingKind::DataAddress);
    assert(rejectedPlayersBinding != nullptr && !rejectedPlayersBinding->resolved);
  }

  {
    const std::uint64_t heartbeat = writePlayerProjectionReady("players.snapshot.tsv");
    writeResidentProofSnapshot(
      bridgePath / "players.snapshot.tsv",
      "read_map_data",
      bridgeEnvironment.processId,
      heartbeat,
      102,
      true);
    RuntimeContract rejectedWrongProofPlayerSnapshot =
      applyRuntimeExecutorBridgeContractProofs(bridgeEnvironment, makeRemasteredParityContract("test-build"));
    const RuntimeBinding* rejectedPlayersBinding =
      findRuntimeBinding(rejectedWrongProofPlayerSnapshot, "BW::BWDATA::Players", BindingKind::DataAddress);
    assert(rejectedPlayersBinding != nullptr && !rejectedPlayersBinding->resolved);
  }

  {
    const std::uint64_t heartbeat = writePlayerProjectionReady("players.snapshot.tsv");
    std::ofstream playersSnapshot(bridgePath / "players.snapshot.tsv");
    playersSnapshot << "# schema=starcraft-api.resident-snapshot.v1\n"
                    << "# proof=read_player_data\n"
                    << "# source_identity=diagnostic-probe\n"
                    << "# process_id=" << bridgeEnvironment.processId << '\n'
                    << "# heartbeat=" << heartbeat << '\n'
                    << "# frame_id=102\n"
                    << "# active_match_correlated=true\n";
    writeResidentSnapshotPayload(playersSnapshot, "read_player_data");
    RuntimeContract rejectedWrongSourceIdentityPlayerSnapshot =
      applyRuntimeExecutorBridgeContractProofs(bridgeEnvironment, makeRemasteredParityContract("test-build"));
    const RuntimeBinding* rejectedPlayersBinding =
      findRuntimeBinding(rejectedWrongSourceIdentityPlayerSnapshot, "BW::BWDATA::Players", BindingKind::DataAddress);
    assert(rejectedPlayersBinding != nullptr && !rejectedPlayersBinding->resolved);
  }

  {
    const std::uint64_t heartbeat = writePlayerProjectionReady("players.snapshot.tsv");
    writeResidentProofSnapshot(
      bridgePath / "players.snapshot.tsv",
      "read_player_data",
      bridgeEnvironment.processId,
      heartbeat - 1,
      102,
      true);
    RuntimeContract rejectedOldHeartbeatPlayerSnapshot =
      applyRuntimeExecutorBridgeContractProofs(bridgeEnvironment, makeRemasteredParityContract("test-build"));
    const RuntimeBinding* rejectedPlayersBinding =
      findRuntimeBinding(rejectedOldHeartbeatPlayerSnapshot, "BW::BWDATA::Players", BindingKind::DataAddress);
    assert(rejectedPlayersBinding != nullptr && !rejectedPlayersBinding->resolved);
  }

  {
    const std::uint64_t heartbeat = writePlayerProjectionReady("players.snapshot.tsv");
    writeResidentProofSnapshot(
      bridgePath / "players.snapshot.tsv",
      "read_player_data",
      bridgeEnvironment.processId,
      heartbeat + 1,
      102,
      true);
    RuntimeContract rejectedFutureHeartbeatPlayerSnapshot =
      applyRuntimeExecutorBridgeContractProofs(bridgeEnvironment, makeRemasteredParityContract("test-build"));
    const RuntimeBinding* rejectedPlayersBinding =
      findRuntimeBinding(rejectedFutureHeartbeatPlayerSnapshot, "BW::BWDATA::Players", BindingKind::DataAddress);
    assert(rejectedPlayersBinding != nullptr && !rejectedPlayersBinding->resolved);
  }

  {
    const std::uint64_t heartbeat = writePlayerProjectionReady("players.snapshot.tsv");
    writeResidentProofSnapshot(
      bridgePath / "players.snapshot.tsv",
      "read_player_data",
      bridgeEnvironment.processId,
      heartbeat,
      103,
      true);
    RuntimeContract rejectedFutureFramePlayerSnapshot =
      applyRuntimeExecutorBridgeContractProofs(bridgeEnvironment, makeRemasteredParityContract("test-build"));
    const RuntimeBinding* rejectedPlayersBinding =
      findRuntimeBinding(rejectedFutureFramePlayerSnapshot, "BW::BWDATA::Players", BindingKind::DataAddress);
    assert(rejectedPlayersBinding != nullptr && !rejectedPlayersBinding->resolved);
  }

  {
    const std::uint64_t heartbeat = writePlayerProjectionReady("players.snapshot.tsv");
    std::ofstream playersSnapshot(bridgePath / "players.snapshot.tsv");
    playersSnapshot << "# schema=starcraft-api.resident-snapshot.v1\n"
                    << "# proof=read_map_data\n"
                    << "# proof=read_player_data\n"
                    << "# source_identity=resident-adapter\n"
                    << "# process_id=" << bridgeEnvironment.processId << '\n'
                    << "# heartbeat=" << heartbeat << '\n'
                    << "# frame_id=102\n"
                    << "# active_match_correlated=true\n";
    writeResidentSnapshotPayload(playersSnapshot, "read_player_data");
    RuntimeContract rejectedDuplicateMetadataPlayerSnapshot =
      applyRuntimeExecutorBridgeContractProofs(bridgeEnvironment, makeRemasteredParityContract("test-build"));
    const RuntimeBinding* rejectedPlayersBinding =
      findRuntimeBinding(rejectedDuplicateMetadataPlayerSnapshot, "BW::BWDATA::Players", BindingKind::DataAddress);
    assert(rejectedPlayersBinding != nullptr && !rejectedPlayersBinding->resolved);
  }

  {
    const std::filesystem::path outsideSnapshot =
      std::filesystem::temp_directory_path() / "starcraft-api-outside-player.snapshot.tsv";
    const std::uint64_t heartbeat = writePlayerProjectionReady(outsideSnapshot.string());
    writeResidentProofSnapshot(
      outsideSnapshot,
      "read_player_data",
      bridgeEnvironment.processId,
      heartbeat,
      102,
      true);
    RuntimeContract rejectedOutsidePathPlayerSnapshot =
      applyRuntimeExecutorBridgeContractProofs(bridgeEnvironment, makeRemasteredParityContract("test-build"));
    const RuntimeBinding* rejectedPlayersBinding =
      findRuntimeBinding(rejectedOutsidePathPlayerSnapshot, "BW::BWDATA::Players", BindingKind::DataAddress);
    assert(rejectedPlayersBinding != nullptr && !rejectedPlayersBinding->resolved);
  }

  {
    std::ofstream ready(bridgePath / RuntimeExecutorBridgeReadyFile);
    ready << "protocol=" << RuntimeExecutorBridgeProtocol << '\n';
    ready << "product=starcraft-remastered\n";
    ready << "version=test-build\n";
    ready << "mode=" << RuntimeExecutorBridgeValidatedAdapterMode << '\n';
    writeRuntimeIdentity(ready, bridgeEnvironment.processId, bridgeEnvironment.executablePath);
    const std::uint64_t heartbeat =
      writeResidentStateProofs(ready, bridgeEnvironment.processId, bridgeEnvironment.executablePath);
    ready << "proof.read_map_data=passed\n";
    ready << "proof.read_map_data.source=latest-replay-artifact\n";
    ready << "proof.read_map_data.map_name_address=0x1600\n";
    ready << "proof.read_map_data.map_tile_array_address=0x1700\n";
    ready << "proof.read_map_data.tile_count=256\n";
    ready << "proof.read_map_data.snapshot=map.snapshot.tsv\n";
    ready << "contract.binding.BW::BWDATA::MapTileArray=data-address|proof.read_map_data=passed:latest-replay-artifact\n";
    writeResidentProofSnapshot(
      bridgePath / "map.snapshot.tsv",
      "read_map_data",
      bridgeEnvironment.processId,
      heartbeat,
      102,
      false);
    RuntimeContract rejectedReplayOnlyMapSnapshot =
      applyRuntimeExecutorBridgeContractProofs(bridgeEnvironment, makeRemasteredParityContract("test-build"));
    const RuntimeBinding* rejectedMapBinding =
      findRuntimeBinding(rejectedReplayOnlyMapSnapshot, "BW::BWDATA::MapTileArray", BindingKind::DataAddress);
    assert(rejectedMapBinding != nullptr && !rejectedMapBinding->resolved);
  }

  {
    std::ofstream ready(bridgePath / RuntimeExecutorBridgeReadyFile);
    ready << "protocol=" << RuntimeExecutorBridgeProtocol << '\n';
    ready << "product=starcraft-remastered\n";
    ready << "version=test-build\n";
    ready << "mode=" << RuntimeExecutorBridgeValidatedAdapterMode << '\n';
    writeRuntimeIdentity(ready, bridgeEnvironment.processId, bridgeEnvironment.executablePath);
    writeResidentStateProofs(ready, bridgeEnvironment.processId, bridgeEnvironment.executablePath);
    ready << "proof.read_bullet_data=passed\n";
    ready << "proof.read_bullet_data.source=live-sc-r-bullet-table\n";
    ready << "proof.read_bullet_data.address=0x1800\n";
    ready << "proof.read_bullet_data.record_size=128\n";
    ready << "proof.read_bullet_data.active_records=1\n";
    ready << "proof.read_bullet_data.unit_correlated_records=1\n";
    ready << "proof.read_bullet_data.snapshot=bullets.snapshot.tsv\n";
    ready << "contract.binding.BW::BWDATA::BulletNodeTable=data-address|proof.read_bullet_data=passed:bullet-node-table\n";
    std::ofstream bulletSnapshot(bridgePath / "bullets.snapshot.tsv");
    bulletSnapshot << "field\tvalue\n"
                   << "passed\ttrue\n";
    RuntimeContract rejectedSchemalessBulletSnapshot =
      applyRuntimeExecutorBridgeContractProofs(bridgeEnvironment, makeRemasteredParityContract("test-build"));
    const RuntimeBinding* rejectedBulletBinding =
      findRuntimeBinding(
        rejectedSchemalessBulletSnapshot,
        "BW::BWDATA::BulletNodeTable",
        BindingKind::DataAddress);
    assert(rejectedBulletBinding != nullptr && !rejectedBulletBinding->resolved);
  }

  {
    std::ofstream ready(bridgePath / RuntimeExecutorBridgeReadyFile);
    ready << "protocol=" << RuntimeExecutorBridgeProtocol << '\n';
    ready << "product=starcraft-remastered\n";
    ready << "version=test-build\n";
    ready << "mode=" << RuntimeExecutorBridgeValidatedAdapterMode << '\n';
    writeRuntimeIdentity(ready, bridgeEnvironment.processId, bridgeEnvironment.executablePath);
    const std::uint64_t heartbeat =
      writeResidentStateProofs(ready, bridgeEnvironment.processId, bridgeEnvironment.executablePath);
    ready << "proof.read_bullet_data=passed\n";
    ready << "proof.read_bullet_data.source=live-sc-r-bullet-table\n";
    ready << "proof.read_bullet_data.address=0x1800\n";
    ready << "proof.read_bullet_data.record_size=128\n";
    ready << "proof.read_bullet_data.active_records=1\n";
    ready << "proof.read_bullet_data.unit_correlated_records=1\n";
    ready << "proof.read_bullet_data.snapshot=bullets.snapshot.tsv\n";
    ready << "contract.binding.BW::BWDATA::BulletNodeTable=data-address|proof.read_bullet_data=passed:bullet-node-table\n";
    std::ofstream bulletSnapshot(bridgePath / "bullets.snapshot.tsv");
    bulletSnapshot << "# schema=starcraft-api.resident-snapshot.v1\n"
                   << "# proof=read_bullet_data\n"
                   << "# source_identity=resident-adapter\n"
                   << "# process_id=" << bridgeEnvironment.processId << '\n'
                   << "# heartbeat=" << heartbeat << '\n'
                   << "# frame_id=102\n"
                   << "# active_match_correlated=true\n"
                   << "index\taddress\tsprite\tsource_unit\ttarget_unit\ttype\tx\ty\tvelocity_x\tvelocity_y\tplayer\tremove_timer\n"
                   << "0\t\t0x3000\t0x1000\t0x2000\t1\t64\t80\t2\t0\t0\t12\n";
    RuntimeContract rejectedBlankBulletAddress =
      applyRuntimeExecutorBridgeContractProofs(bridgeEnvironment, makeRemasteredParityContract("test-build"));
    const RuntimeBinding* rejectedBulletBinding =
      findRuntimeBinding(
        rejectedBlankBulletAddress,
        "BW::BWDATA::BulletNodeTable",
        BindingKind::DataAddress);
    assert(rejectedBulletBinding != nullptr && !rejectedBulletBinding->resolved);
  }

  {
    std::ofstream ready(bridgePath / RuntimeExecutorBridgeReadyFile);
    ready << "protocol=" << RuntimeExecutorBridgeProtocol << '\n';
    ready << "product=starcraft-remastered\n";
    ready << "version=test-build\n";
    ready << "mode=" << RuntimeExecutorBridgeValidatedAdapterMode << '\n';
    writeRuntimeIdentity(ready, bridgeEnvironment.processId, bridgeEnvironment.executablePath);
    const std::uint64_t heartbeat =
      writeResidentStateProofs(ready, bridgeEnvironment.processId, bridgeEnvironment.executablePath);
    ready << "proof.read_bullet_data=passed\n";
    ready << "proof.read_bullet_data.source=live-sc-r-bullet-table\n";
    ready << "proof.read_bullet_data.address=0x1800\n";
    ready << "proof.read_bullet_data.record_size=128\n";
    ready << "proof.read_bullet_data.active_records=1\n";
    ready << "proof.read_bullet_data.unit_correlated_records=1\n";
    ready << "proof.read_bullet_data.snapshot=bullets.snapshot.tsv\n";
    ready << "contract.binding.BW::BWDATA::BulletNodeTable=data-address|proof.read_bullet_data=passed:bullet-node-table\n";
    std::ofstream bulletSnapshot(bridgePath / "bullets.snapshot.tsv");
    bulletSnapshot << "# schema=starcraft-api.resident-snapshot.v1\n"
                   << "# proof=read_bullet_data\n"
                   << "# source_identity=resident-adapter\n"
                   << "# process_id=" << bridgeEnvironment.processId << '\n'
                   << "# heartbeat=" << heartbeat << '\n'
                   << "# frame_id=102\n"
                   << "# active_match_correlated=true\n";
    writeResidentSnapshotPayload(bulletSnapshot, "read_bullet_data");
    RuntimeContract rejectedExtraBulletRows =
      applyRuntimeExecutorBridgeContractProofs(bridgeEnvironment, makeRemasteredParityContract("test-build"));
    const RuntimeBinding* rejectedBulletBinding =
      findRuntimeBinding(
        rejectedExtraBulletRows,
        "BW::BWDATA::BulletNodeTable",
        BindingKind::DataAddress);
    assert(rejectedBulletBinding != nullptr && !rejectedBulletBinding->resolved);
  }

  {
    std::ofstream ready(bridgePath / RuntimeExecutorBridgeReadyFile);
    ready << "protocol=" << RuntimeExecutorBridgeProtocol << '\n';
    ready << "product=starcraft-remastered\n";
    ready << "version=test-build\n";
    ready << "mode=" << RuntimeExecutorBridgeValidatedAdapterMode << '\n';
    writeRuntimeIdentity(ready, bridgeEnvironment.processId, bridgeEnvironment.executablePath);
    const std::uint64_t heartbeat =
      writeResidentStateProofs(ready, bridgeEnvironment.processId, bridgeEnvironment.executablePath);
    ready << "proof.read_bullet_data=passed\n";
    ready << "proof.read_bullet_data.source=live-sc-r-bullet-table\n";
    ready << "proof.read_bullet_data.address=0x1800\n";
    ready << "proof.read_bullet_data.record_size=128\n";
    ready << "proof.read_bullet_data.active_records=1\n";
    ready << "proof.read_bullet_data.unit_correlated_records=1\n";
    ready << "proof.read_bullet_data.snapshot=bullets.snapshot.tsv\n";
    ready << "contract.binding.BW::BWDATA::BulletNodeTable=data-address|proof.read_bullet_data=passed:bullet-node-table\n";
    ready << "contract.structure.BW::CBullet=128|proof.read_bullet_data=passed:cbullet-layout\n";
    ready << "contract.field.BW::CBullet.position=0|4|proof.read_bullet_data=passed:cbullet-position\n";
    std::ofstream bulletSnapshot(bridgePath / "bullets.snapshot.tsv");
    bulletSnapshot << "# schema=starcraft-api.resident-snapshot.v1\n"
                   << "# proof=read_bullet_data\n"
                   << "# source_identity=resident-adapter\n"
                   << "# process_id=" << bridgeEnvironment.processId << '\n'
                   << "# heartbeat=" << heartbeat << '\n'
                   << "# frame_id=102\n"
                   << "# active_match_correlated=true\n"
                   << "field\tvalue\n"
                   << "passed\ttrue\n";
    RuntimeContract rejectedGenericPayloadBulletSnapshot =
      applyRuntimeExecutorBridgeContractProofs(bridgeEnvironment, makeRemasteredParityContract("test-build"));
    const RuntimeBinding* rejectedBulletBinding =
      findRuntimeBinding(
        rejectedGenericPayloadBulletSnapshot,
        "BW::BWDATA::BulletNodeTable",
        BindingKind::DataAddress);
    const StructureLayout* rejectedBulletLayout =
      findStructureLayout(rejectedGenericPayloadBulletSnapshot, "BW::CBullet");
    const StructureField* rejectedBulletPosition =
      findStructureField(rejectedGenericPayloadBulletSnapshot, "BW::CBullet", "position");
    assert(rejectedBulletBinding != nullptr && !rejectedBulletBinding->resolved);
    assert(rejectedBulletLayout != nullptr && rejectedBulletLayout->size == 0);
    assert(rejectedBulletPosition != nullptr && !rejectedBulletPosition->resolved);
  }

  {
    std::ofstream ready(bridgePath / RuntimeExecutorBridgeReadyFile);
    ready << "protocol=" << RuntimeExecutorBridgeProtocol << '\n';
    ready << "product=starcraft-remastered\n";
    ready << "version=test-build\n";
    ready << "mode=" << RuntimeExecutorBridgeValidatedAdapterMode << '\n';
    writeRuntimeIdentity(ready, bridgeEnvironment.processId, bridgeEnvironment.executablePath);
    ready << "proof.read_game_state=passed\n";
    ready << "contract.binding.BW::BWDATA::Game=data-address|proof.read_game_state=passed:game\n";
    ready << "contract.structure.BW::BWGame=256|proof.read_game_state=passed:bwgame-layout\n";
    ready << "contract.field.BW::BWGame.elapsedFrames=8|4|proof.read_game_state=passed\n";
  }
  RuntimeContract rejectedFakeGameStateProof =
    applyRuntimeExecutorBridgeContractProofs(bridgeEnvironment, makeRemasteredParityContract("test-build"));
  const RuntimeBinding* rejectedGameStateBinding =
    findRuntimeBinding(rejectedFakeGameStateProof, "BW::BWDATA::Game", BindingKind::DataAddress);
  const StructureLayout* rejectedGameStateLayout =
    findStructureLayout(rejectedFakeGameStateProof, "BW::BWGame");
  const StructureField* rejectedElapsedFrames =
    findStructureField(rejectedFakeGameStateProof, "BW::BWGame", "elapsedFrames");
  assert(rejectedGameStateBinding != nullptr && !rejectedGameStateBinding->resolved);
  assert(rejectedGameStateLayout != nullptr && rejectedGameStateLayout->size == 0);
  assert(rejectedElapsedFrames != nullptr && !rejectedElapsedFrames->resolved);

  {
    std::ofstream ready(bridgePath / RuntimeExecutorBridgeReadyFile);
    ready << "protocol=" << RuntimeExecutorBridgeProtocol << '\n';
    ready << "product=starcraft-remastered\n";
    ready << "version=test-build\n";
    ready << "mode=" << RuntimeExecutorBridgeValidatedAdapterMode << '\n';
    writeRuntimeIdentity(ready, bridgeEnvironment.processId, bridgeEnvironment.executablePath);
    writeResidentStateProofs(ready, bridgeEnvironment.processId, bridgeEnvironment.executablePath, bridgePath);
    ready << "contract.binding.BW::BWDATA::Game=data-address|proof.read_game_state=passed:game\n";
    ready << "contract.structure.BW::BWGame=256|proof.read_game_state=passed:bwgame-layout\n";
    ready << "contract.field.BW::BWGame.elapsedFrames=8|4|proof.read_game_state=passed\n";
  }
  RuntimeContract acceptedResidentGameStateProof =
    applyRuntimeExecutorBridgeContractProofs(bridgeEnvironment, makeRemasteredParityContract("test-build"));
  const RuntimeBinding* acceptedGameStateBinding =
    findRuntimeBinding(acceptedResidentGameStateProof, "BW::BWDATA::Game", BindingKind::DataAddress);
  const StructureLayout* acceptedGameStateLayout =
    findStructureLayout(acceptedResidentGameStateProof, "BW::BWGame");
  const StructureField* acceptedElapsedFrames =
    findStructureField(acceptedResidentGameStateProof, "BW::BWGame", "elapsedFrames");
  assert(acceptedGameStateBinding != nullptr && acceptedGameStateBinding->resolved);
  assert(acceptedGameStateLayout != nullptr && acceptedGameStateLayout->size == 256);
  assert(acceptedElapsedFrames != nullptr && acceptedElapsedFrames->resolved);

  {
    std::ofstream ready(bridgePath / RuntimeExecutorBridgeReadyFile);
    ready << "protocol=" << RuntimeExecutorBridgeProtocol << '\n';
    ready << "product=starcraft-remastered\n";
    ready << "version=test-build\n";
    ready << "mode=" << RuntimeExecutorBridgeValidatedAdapterMode << '\n';
    writeRuntimeIdentity(ready, bridgeEnvironment.processId, bridgeEnvironment.executablePath);
    writeResidentStateProofs(ready, bridgeEnvironment.processId, bridgeEnvironment.executablePath, bridgePath);
    ready << "proof.read_game_state=failed\n";
    ready << "contract.binding.BW::BWDATA::Game=data-address|proof.read_game_state=passed:game\n";
    ready << "contract.structure.BW::BWGame=256|proof.read_game_state=passed:bwgame-layout\n";
    ready << "contract.field.BW::BWGame.elapsedFrames=8|4|proof.read_game_state=passed\n";
  }
  RuntimeContract rejectedDuplicateExactGameStateProof =
    applyRuntimeExecutorBridgeContractProofs(bridgeEnvironment, makeRemasteredParityContract("test-build"));
  const RuntimeBinding* rejectedDuplicateExactGameStateBinding =
    findRuntimeBinding(
      rejectedDuplicateExactGameStateProof,
      "BW::BWDATA::Game",
      BindingKind::DataAddress);
  const StructureLayout* rejectedDuplicateExactGameStateLayout =
    findStructureLayout(rejectedDuplicateExactGameStateProof, "BW::BWGame");
  const StructureField* rejectedDuplicateExactElapsedFrames =
    findStructureField(
      rejectedDuplicateExactGameStateProof,
      "BW::BWGame",
      "elapsedFrames");
  assert(rejectedDuplicateExactGameStateBinding != nullptr
    && !rejectedDuplicateExactGameStateBinding->resolved);
  assert(rejectedDuplicateExactGameStateLayout != nullptr
    && rejectedDuplicateExactGameStateLayout->size == 0);
  assert(rejectedDuplicateExactElapsedFrames != nullptr
    && !rejectedDuplicateExactElapsedFrames->resolved);

  const std::string fixtureExecutable = fixturePath("remastered-complete.manifest");
  {
    std::ofstream ready(bridgePath / RuntimeExecutorBridgeReadyFile);
    ready << "protocol=" << RuntimeExecutorBridgeProtocol << '\n';
    ready << "product=starcraft-remastered\n";
    ready << "version=test-build\n";
    ready << "mode=" << RuntimeExecutorBridgeValidatedAdapterMode << '\n';
    writeRuntimeIdentity(ready, bridgeEnvironment.processId, fixtureExecutable);
    writeResidentStateProofs(ready, bridgeEnvironment.processId, fixtureExecutable, bridgePath);
    ready << "contract.binding.BW::BWDATA::Game=data-address|proof.read_game_state=passed:game\n";
    ready << "contract.structure.BW::BWGame=256|proof.read_game_state=passed:bwgame-layout\n";
    ready << "contract.field.BW::BWGame.elapsedFrames=8|4|proof.read_game_state=passed\n";
  }
  RuntimeEnvironment mismatchedActualExecutableEnvironment = bridgeEnvironment;
  mismatchedActualExecutableEnvironment.executablePath = fixtureExecutable;
  RuntimeContract rejectedActualExecutableMismatchProof =
    applyRuntimeExecutorBridgeContractProofs(
      mismatchedActualExecutableEnvironment,
      makeRemasteredParityContract("test-build"));
  const RuntimeBinding* rejectedActualExecutableMismatchBinding =
    findRuntimeBinding(
      rejectedActualExecutableMismatchProof,
      "BW::BWDATA::Game",
      BindingKind::DataAddress);
  const StructureLayout* rejectedActualExecutableMismatchLayout =
    findStructureLayout(rejectedActualExecutableMismatchProof, "BW::BWGame");
  const StructureField* rejectedActualExecutableMismatchField =
    findStructureField(rejectedActualExecutableMismatchProof, "BW::BWGame", "elapsedFrames");
  assert(rejectedActualExecutableMismatchBinding != nullptr
    && !rejectedActualExecutableMismatchBinding->resolved);
  assert(rejectedActualExecutableMismatchLayout != nullptr
    && rejectedActualExecutableMismatchLayout->size == 0);
  assert(rejectedActualExecutableMismatchField != nullptr
    && !rejectedActualExecutableMismatchField->resolved);

  RuntimeEnvironment unselectedProcessBridgeEnvironment = bridgeEnvironment;
  unselectedProcessBridgeEnvironment.processId = 0;
  RuntimeContract rejectedUnselectedProcessProof =
    applyRuntimeExecutorBridgeContractProofs(
      unselectedProcessBridgeEnvironment,
      makeRemasteredParityContract("test-build"));
  const RuntimeBinding* rejectedUnselectedGameStateBinding =
    findRuntimeBinding(rejectedUnselectedProcessProof, "BW::BWDATA::Game", BindingKind::DataAddress);
  const StructureLayout* rejectedUnselectedGameStateLayout =
    findStructureLayout(rejectedUnselectedProcessProof, "BW::BWGame");
  const StructureField* rejectedUnselectedElapsedFrames =
    findStructureField(rejectedUnselectedProcessProof, "BW::BWGame", "elapsedFrames");
  assert(rejectedUnselectedGameStateBinding != nullptr && !rejectedUnselectedGameStateBinding->resolved);
  assert(rejectedUnselectedGameStateLayout != nullptr && rejectedUnselectedGameStateLayout->size == 0);
  assert(rejectedUnselectedElapsedFrames != nullptr && !rejectedUnselectedElapsedFrames->resolved);

  writeValidatedAdapterReadyFile(bridgePath, bridgeEnvironment.processId, bridgeEnvironment.executablePath);
  RuntimeExecutorPreflightResult freshResidentPreflight =
    preflightRuntimeExecutor(bridgeEnvironment, complete.manifest.contract);
  assert(freshResidentPreflight.executorAvailable);
  assert(hasMissingProof(freshResidentPreflight, "proof.issue_commands=passed"));
  assert(!freshResidentPreflight.errors.empty());

  const std::filesystem::path readyPath = bridgePath / RuntimeExecutorBridgeReadyFile;
  std::error_code timestampError;
  const std::filesystem::file_time_type rewrittenTime =
    std::filesystem::last_write_time(readyPath, timestampError);
  assert(!timestampError);
  std::filesystem::last_write_time(readyPath, rewrittenTime + std::chrono::seconds(2), timestampError);
  assert(!timestampError);
  RuntimeExecutorPreflightResult replayedResidentPreflight =
    preflightRuntimeExecutor(bridgeEnvironment, complete.manifest.contract);
  assert(!replayedResidentPreflight.executorAvailable);
  assert(!replayedResidentPreflight.errors.empty());

  writeValidatedAdapterReadyFile(bridgePath, bridgeEnvironment.processId, bridgeEnvironment.executablePath);
  {
    std::ofstream ready(bridgePath / RuntimeExecutorBridgeReadyFile, std::ios::app);
    ready << "proof.issue_commands.source=mock-command-path\n";
  }
  RuntimeExecutorPreflightResult duplicateEvidencePreflight =
    preflightRuntimeExecutor(bridgeEnvironment, complete.manifest.contract);
  assert(!duplicateEvidencePreflight.executorAvailable);
  assert(!duplicateEvidencePreflight.errors.empty());
  assert(hasErrorContaining(
    duplicateEvidencePreflight.errors,
    "duplicate evidence key: proof.issue_commands.source"));

  writeValidatedAdapterReadyFile(bridgePath, bridgeEnvironment.processId, bridgeEnvironment.executablePath);
  {
    std::ofstream ready(bridgePath / RuntimeExecutorBridgeReadyFile, std::ios::app);
    ready << "proof.issue_commands=failed\n";
  }
  RuntimeExecutorPreflightResult duplicateExactStatusPreflight =
    preflightRuntimeExecutor(bridgeEnvironment, complete.manifest.contract);
  assert(!duplicateExactStatusPreflight.executorAvailable);
  assert(!duplicateExactStatusPreflight.errors.empty());
  assert(hasErrorContaining(
    duplicateExactStatusPreflight.errors,
    "duplicate evidence key: proof.issue_commands"));

  writeValidatedAdapterReadyFile(bridgePath, bridgeEnvironment.processId, bridgeEnvironment.executablePath);
  {
    std::ofstream ready(bridgePath / RuntimeExecutorBridgeReadyFile, std::ios::app);
    ready << "proof.attach.source=mock-adapter\n";
  }
  RuntimeExecutorPreflightResult duplicateAttachMetadataPreflight =
    preflightRuntimeExecutor(bridgeEnvironment, complete.manifest.contract);
  assert(!duplicateAttachMetadataPreflight.executorAvailable);
  assert(!duplicateAttachMetadataPreflight.errors.empty());
  assert(hasErrorContaining(
    duplicateAttachMetadataPreflight.errors,
    "duplicate evidence key: proof.attach.source"));

  writeValidatedAdapterReadyFile(bridgePath, bridgeEnvironment.processId, bridgeEnvironment.executablePath);
  {
    const std::filesystem::path readyPath = bridgePath / RuntimeExecutorBridgeReadyFile;
    std::ifstream readyInput(readyPath);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(readyInput, line))
    {
      if (line.rfind("resident.queue.proof.path=", 0) != 0)
        lines.push_back(line);
    }
    readyInput.close();

    std::ofstream readyOutput(readyPath);
    for (const std::string& preservedLine : lines)
      readyOutput << preservedLine << '\n';
  }
  RuntimeExecutorPreflightResult missingProofQueueAttachPreflight =
    preflightRuntimeExecutor(bridgeEnvironment, complete.manifest.contract);
  assert(!missingProofQueueAttachPreflight.errors.empty());
  RuntimeCommandRequest missingProofQueueAttachCommand;
  missingProofQueueAttachCommand.kind = RuntimeCommandKind::GameAction;
  missingProofQueueAttachCommand.name = "pauseGame";
  RuntimeExecutorSubmitResult rejectedMissingProofQueueAttachSubmit =
    submitRuntimeCommands(bridgeEnvironment, { missingProofQueueAttachCommand });
  assert(!rejectedMissingProofQueueAttachSubmit.submitted);
  assert(hasErrorContaining(
    rejectedMissingProofQueueAttachSubmit.errors,
    "runtime executor bridge is missing command submission proof: proof.attach=passed"));

  writeValidatedAdapterReadyFile(bridgePath, bridgeEnvironment.processId, bridgeEnvironment.executablePath);
  {
    std::ofstream thinSnapshot(bridgePath / "issue_commands.snapshot.tsv");
    thinSnapshot << "field\tvalue\n"
                 << "passed\ttrue\n"
                 << "source\tlive-sc-r-command-path\n"
                 << "behavior_checked\ttrue\n";
  }
  RuntimeExecutorPreflightResult rejectedThinIssueSnapshotPreflight =
    preflightRuntimeExecutor(bridgeEnvironment, complete.manifest.contract);
  assert(hasMissingProof(rejectedThinIssueSnapshotPreflight, "proof.issue_commands=passed"));
  assert(!rejectedThinIssueSnapshotPreflight.errors.empty());
  RuntimeCommandRequest thinSnapshotCommand;
  thinSnapshotCommand.kind = RuntimeCommandKind::GameAction;
  thinSnapshotCommand.name = "pauseGame";
  RuntimeExecutorSubmitResult rejectedThinIssueSnapshotSubmit =
    submitRuntimeCommands(bridgeEnvironment, { thinSnapshotCommand });
  assert(!rejectedThinIssueSnapshotSubmit.submitted);
  assert(rejectedThinIssueSnapshotSubmit.reason.find(
    "issue-commands proof is missing validated production metadata") != std::string::npos);

  writeValidatedAdapterReadyFile(bridgePath, bridgeEnvironment.processId, bridgeEnvironment.executablePath);
  {
    std::ofstream staleSnapshot(bridgePath / "issue_commands.snapshot.tsv");
    staleSnapshot << "# schema=starcraft-api.resident-snapshot.v1\n"
                  << "# proof=issue_commands\n"
                  << "# source_identity=resident-adapter\n"
                  << "# process_id=" << (bridgeEnvironment.processId + 100000) << '\n'
                  << "# heartbeat=20\n"
                  << "# frame_id=102\n"
                  << "# active_match_correlated=true\n"
                  << "field\tvalue\n"
                  << "passed\ttrue\n"
                  << "delivery_checked\ttrue\n"
                  << "behavior_checked\ttrue\n"
                  << "self_fixture\tfalse\n"
                  << "receiver_active\ttrue\n"
                  << "stale_proof_bytes_cleared\ttrue\n"
                  << "pause_frame_counter_sampled\ttrue\n"
                  << "pause_frame_counter_matched\ttrue\n"
                  << "frame_counter_candidate_count\t1\n"
                  << "issue_commands_required_adapter_abi\tstarcraft-api-resident-adapter-v1\n"
                  << "issue_commands_required_adapter_location\tin-process-target-runtime\n"
                  << "issue_commands_required_adapter_thread_policy\texecute-on-target-runtime-thread\n"
                  << "issue_commands_required_adapter_behavior\tencoded-bwapi-command-reaches-live-scr-command-path-and-changes-frame-behavior\n"
                  << "issue_commands_required_adapter_promotion_rule\tdo-not-emit-production-proof-until-live-behavior-is-observed\n"
                  << "command\tpauseGame/resumeGame\n"
                  << "encoded_bytes\t10 / 11\n"
                  << "attempt_count\t1\n"
                  << "storage_kind\tlive-sc-r-command-queue-v1\n"
                  << "vector_address\t0x1000\n"
                  << "bytes_in_queue_address\t0x1100\n"
                  << "buffer_begin\t0x1000\n"
                  << "frame_counter_address\t0x1200\n"
                  << "original_used_bytes\t0\n"
                  << "appended_bytes\t1\n"
                  << "baseline_delta\t12\n"
                  << "paused_delta\t0\n"
                  << "resumed_delta\t12\n";
  }
  RuntimeExecutorPreflightResult rejectedStaleIssueSnapshotPreflight =
    preflightRuntimeExecutor(bridgeEnvironment, complete.manifest.contract);
  assert(hasMissingProof(rejectedStaleIssueSnapshotPreflight, "proof.issue_commands=passed"));
  assert(!rejectedStaleIssueSnapshotPreflight.errors.empty());

  writePartialValidatedAdapterReadyFile(bridgePath, bridgeEnvironment.processId, bridgeEnvironment.executablePath);
  RuntimeExecutorPreflightResult partialProofPreflight =
    preflightRuntimeExecutor(bridgeEnvironment, complete.manifest.contract);
  assert(partialProofPreflight.contractValid);
  assert(partialProofPreflight.processIdentified);
  assert(partialProofPreflight.memoryAccessible);
  assert(partialProofPreflight.targetLocated);
  assert(partialProofPreflight.executorAvailable);
  assert(partialProofPreflight.executorName == "filesystem-bridge-validated-runtime-adapter");
  assert(partialProofPreflight.executorBridgeMode == RuntimeExecutorBridgeValidatedAdapterMode);
  assert(partialProofPreflight.missingBehaviorProofs.size() == 2);
  assert(hasMissingProof(partialProofPreflight, "proof.issue_commands=passed"));
  assert(hasMissingProof(partialProofPreflight, "proof.multiplayer_sync=passed"));
  assert(!partialProofPreflight.errors.empty());

  writeMismatchedRuntimeIdentityReadyFile(bridgePath, bridgeEnvironment.processId, bridgeEnvironment.executablePath);
  RuntimeExecutorPreflightResult mismatchedIdentityPreflight =
    preflightRuntimeExecutor(bridgeEnvironment, complete.manifest.contract);
  assert(mismatchedIdentityPreflight.contractValid);
  assert(mismatchedIdentityPreflight.processIdentified);
  assert(mismatchedIdentityPreflight.memoryAccessible);
  assert(mismatchedIdentityPreflight.targetLocated);
  assert(!mismatchedIdentityPreflight.executorAvailable);
  assert(mismatchedIdentityPreflight.missingBehaviorProofs.empty());
  assert(!mismatchedIdentityPreflight.errors.empty());

  RuntimeEnvironment staleProcessEnvironment = bridgeEnvironment;
  staleProcessEnvironment.processId = currentProcessId() + 100000;
  writeValidatedAdapterReadyFile(
    bridgePath,
    staleProcessEnvironment.processId,
    staleProcessEnvironment.executablePath);
  RuntimeExecutorPreflightResult staleProcessPreflight =
    preflightRuntimeExecutor(staleProcessEnvironment, complete.manifest.contract);
  assert(staleProcessPreflight.contractValid);
  assert(!staleProcessPreflight.processIdentified);
  assert(staleProcessPreflight.targetLocated);
  assert(!staleProcessPreflight.executorAvailable);
  assert(staleProcessPreflight.executorBridgeMode.empty());
  assert(staleProcessPreflight.missingBehaviorProofs.empty());
  assert(!staleProcessPreflight.errors.empty());

  writeValidatedAdapterReadyFile(bridgePath, bridgeEnvironment.processId, bridgeEnvironment.executablePath);
  {
    const std::filesystem::path readyPath = bridgePath / RuntimeExecutorBridgeReadyFile;
    std::ifstream readyInput(readyPath);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(readyInput, line))
    {
      if (line != "proof.attach=passed")
        lines.push_back(line);
    }
    readyInput.close();
    std::ofstream readyOutput(readyPath);
    for (const std::string& preservedLine : lines)
      readyOutput << preservedLine << '\n';
  }
  RuntimeCommandRequest missingAttachStatusCommand;
  missingAttachStatusCommand.kind = RuntimeCommandKind::GameAction;
  missingAttachStatusCommand.name = "pauseGame";
  RuntimeExecutorSubmitResult rejectedMissingAttachStatusSubmit =
    submitRuntimeCommands(bridgeEnvironment, { missingAttachStatusCommand });
  assert(!rejectedMissingAttachStatusSubmit.submitted);
  assert(hasErrorContaining(
    rejectedMissingAttachStatusSubmit.errors,
    "runtime executor bridge is missing command submission proof: proof.attach=passed"));

  writeValidatedAdapterReadyFile(bridgePath, bridgeEnvironment.processId, bridgeEnvironment.executablePath);
  {
    const std::filesystem::path queuePath = bridgePath / "resident-command.queue";
    const RuntimeResidentQueueHeader desiredQueue =
      makeRuntimeResidentQueueHeader(RuntimeResidentQueueKind::Command, 128, 4, 20);
    RuntimeResidentQueueHeader actualQueue;
    RuntimeResidentQueueValidationResult ensuredQueue =
      ensureRuntimeResidentQueueFile(queuePath, desiredQueue, actualQueue);
    if (!ensuredQueue.valid)
    {
      for (const std::string& error : ensuredQueue.errors)
        std::cerr << "resident command queue fixture error: " << error << '\n';
    }
    assert(ensuredQueue.valid);

    const std::filesystem::path readyPath = bridgePath / RuntimeExecutorBridgeReadyFile;
    std::ifstream readyInput(readyPath);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(readyInput, line))
    {
      if (line != "proof.issue_commands=passed")
        lines.push_back(line);
    }
    readyInput.close();
    lines.push_back("resident.queue.command.path=resident-command.queue");

    std::ofstream readyOutput(readyPath);
    for (const std::string& preservedLine : lines)
      readyOutput << preservedLine << '\n';
  }
  RuntimeCommandRequest ingressWithoutIssueStatusCommand;
  ingressWithoutIssueStatusCommand.kind = RuntimeCommandKind::GameAction;
  ingressWithoutIssueStatusCommand.name = "pauseGame";
  RuntimeExecutorSubmitResult ingressWithoutIssueStatusSubmit =
    submitRuntimeCommands(bridgeEnvironment, { ingressWithoutIssueStatusCommand });
  assert(ingressWithoutIssueStatusSubmit.submitted);
  assert(ingressWithoutIssueStatusSubmit.submittedCommands == 1);
  assert(ingressWithoutIssueStatusSubmit.errors.empty());
  assert(hasWarningContaining(
    ingressWithoutIssueStatusSubmit.warnings,
    "proof.issue_commands=passed is still missing"));

  const std::filesystem::path outsideCommandQueuePath =
    bridgePath.parent_path() / "starcraft-api-outside-command.queue";
  {
    const RuntimeResidentQueueHeader desiredQueue =
      makeRuntimeResidentQueueHeader(RuntimeResidentQueueKind::Command, 128, 4, 20);
    RuntimeResidentQueueHeader actualQueue;
    RuntimeResidentQueueValidationResult ensuredQueue =
      ensureRuntimeResidentQueueFile(outsideCommandQueuePath, desiredQueue, actualQueue);
    assert(ensuredQueue.valid);
  }

  for (const std::string& rejectedQueuePath : {
         outsideCommandQueuePath.string(),
         (std::filesystem::path("..") / outsideCommandQueuePath.filename()).string() })
  {
    writeValidatedAdapterReadyFile(bridgePath, bridgeEnvironment.processId, bridgeEnvironment.executablePath);
    const std::filesystem::path readyPath = bridgePath / RuntimeExecutorBridgeReadyFile;
    std::ifstream readyInput(readyPath);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(readyInput, line))
    {
      if (line != "proof.issue_commands=passed")
        lines.push_back(line);
    }
    readyInput.close();
    lines.push_back("resident.queue.command.path=" + rejectedQueuePath);

    std::ofstream readyOutput(readyPath);
    for (const std::string& preservedLine : lines)
      readyOutput << preservedLine << '\n';

    RuntimeExecutorSubmitResult rejectedOutsideQueueSubmit =
      submitRuntimeCommands(bridgeEnvironment, { ingressWithoutIssueStatusCommand });
    assert(!rejectedOutsideQueueSubmit.submitted);
    RuntimeResidentQueueReadResult outsideQueueRead =
      readRuntimeResidentQueueRecords(
        outsideCommandQueuePath,
        RuntimeResidentQueueKind::Command,
        4);
    assert(outsideQueueRead.read);
    assert(outsideQueueRead.header.writeSequence == 0);
    assert(outsideQueueRead.records.empty());
  }

  writeValidatedAdapterReadyFile(bridgePath, bridgeEnvironment.processId, bridgeEnvironment.executablePath);
  RuntimeExecutorPreflightResult bridgePreflight =
    preflightRuntimeExecutor(bridgeEnvironment, complete.manifest.contract);
  assert(bridgePreflight.contractValid);
  assert(bridgePreflight.processIdentified);
  assert(bridgePreflight.memoryAccessible);
  assert(bridgePreflight.targetLocated);
  assert(bridgePreflight.executorAvailable);
  assert(bridgePreflight.executorName == "filesystem-bridge-validated-runtime-adapter");
  assert(bridgePreflight.executorBridgeMode == RuntimeExecutorBridgeValidatedAdapterMode);
  assert(hasMissingProof(bridgePreflight, "proof.issue_commands=passed"));
  assert(!bridgePreflight.errors.empty());

  RuntimeCommandRequest unitCommand;
  unitCommand.kind = RuntimeCommandKind::UnitCommand;
  unitCommand.name = "Move";
  unitCommand.targetUnitId = 5;
  unitCommand.arguments = { 10, 20 };

  RuntimeCommandRequest gameAction;
  gameAction.kind = RuntimeCommandKind::GameAction;
  gameAction.name = "pauseGame";

  RuntimeExecutorSubmitResult submitted =
    submitRuntimeCommands(bridgeEnvironment, { unitCommand, gameAction });
  assert(!submitted.submitted);
  assert(!submitted.errors.empty());
  assert(submitted.reason.find(
    "issue-commands proof is missing validated production metadata") != std::string::npos);
  assert(!std::filesystem::exists(bridgePath / RuntimeExecutorBridgeCommandFile));

  std::filesystem::path directBridgePath =
    makeBridgePath("starcraft-api-runtime-executor-direct-test");
  RuntimeEnvironment directBridgeEnvironment = bridgeEnvironment;
  directBridgeEnvironment.executorBridgePath = directBridgePath.string();
  std::uint32_t directBytesInQueue = 0;
  std::array<unsigned char, 512> directTurnBuffer = {};
  writeDirectValidatedAdapterReadyFile(
    directBridgePath,
    directBridgeEnvironment.processId,
    directBridgeEnvironment.executablePath,
    reinterpret_cast<std::uintptr_t>(&directBytesInQueue),
    reinterpret_cast<std::uintptr_t>(directTurnBuffer.data()));
  RuntimeExecutorPreflightResult directBridgePreflight =
    preflightRuntimeExecutor(directBridgeEnvironment, complete.manifest.contract);
  assert(directBridgePreflight.contractValid);
  assert(directBridgePreflight.processIdentified);
  assert(directBridgePreflight.memoryAccessible);
  assert(directBridgePreflight.executorAvailable);
  assert(hasMissingProof(directBridgePreflight, "proof.issue_commands=passed"));
  assert(!directBridgePreflight.errors.empty());
  RuntimeExecutorSubmitResult directSubmitted =
    submitRuntimeCommands(directBridgeEnvironment, { gameAction });
  assert(!directSubmitted.submitted);
  assert(!directSubmitted.errors.empty());
  assert(directSubmitted.reason.find("issue-commands proof is missing validated production metadata") != std::string::npos);
  assert(directBytesInQueue == 0);
  assert(directTurnBuffer[0] == 0);
  assert(!std::filesystem::exists(directBridgePath / "commands.applied.tsv"));
  assert(!std::filesystem::exists(directBridgePath / RuntimeExecutorBridgeCommandFile));

  writeDirectValidatedAdapterReadyFile(
    directBridgePath,
    directBridgeEnvironment.processId,
    directBridgeEnvironment.executablePath,
    reinterpret_cast<std::uintptr_t>(&directBytesInQueue),
    reinterpret_cast<std::uintptr_t>(directTurnBuffer.data()));
  {
    std::ofstream ready(directBridgePath / RuntimeExecutorBridgeReadyFile, std::ios::app);
    ready << "proof.issue_commands=failed\n";
  }
  RuntimeExecutorSubmitResult rejectedDuplicateExactStatusSubmit =
    submitRuntimeCommands(directBridgeEnvironment, { gameAction });
  assert(!rejectedDuplicateExactStatusSubmit.submitted);
  assert(hasErrorContaining(
    rejectedDuplicateExactStatusSubmit.errors,
    "duplicate evidence key: proof.issue_commands"));

  writeDirectValidatedAdapterReadyFile(
    directBridgePath,
    directBridgeEnvironment.processId,
    directBridgeEnvironment.executablePath,
    reinterpret_cast<std::uintptr_t>(&directBytesInQueue),
    reinterpret_cast<std::uintptr_t>(directTurnBuffer.data()));
  {
    std::ofstream ready(directBridgePath / RuntimeExecutorBridgeReadyFile, std::ios::app);
    ready << "proof.attach.source=mock-adapter\n";
  }
  RuntimeExecutorSubmitResult rejectedDuplicateAttachMetadataSubmit =
    submitRuntimeCommands(directBridgeEnvironment, { gameAction });
  assert(!rejectedDuplicateAttachMetadataSubmit.submitted);
  assert(hasErrorContaining(
    rejectedDuplicateAttachMetadataSubmit.errors,
    "duplicate evidence key: proof.attach.source"));

  std::filesystem::path rejectedDirectBridgePath =
    makeBridgePath("starcraft-api-runtime-executor-rejected-direct-test");
  RuntimeEnvironment rejectedDirectBridgeEnvironment = bridgeEnvironment;
  rejectedDirectBridgeEnvironment.executorBridgePath = rejectedDirectBridgePath.string();
  writeDirectValidatedAdapterReadyFile(
    rejectedDirectBridgePath,
    rejectedDirectBridgeEnvironment.processId,
    rejectedDirectBridgeEnvironment.executablePath,
    reinterpret_cast<std::uintptr_t>(&directBytesInQueue),
    reinterpret_cast<std::uintptr_t>(directTurnBuffer.data()),
    "proof.attach=passed");
  RuntimeExecutorPreflightResult rejectedDirectBridgePreflight =
    preflightRuntimeExecutor(rejectedDirectBridgeEnvironment, complete.manifest.contract);
  assert(!rejectedDirectBridgePreflight.missingBehaviorProofs.empty());
  assert(rejectedDirectBridgePreflight.missingBehaviorProofs.front() == "proof.issue_commands=passed");
  assert(!rejectedDirectBridgePreflight.errors.empty());
  RuntimeExecutorSubmitResult rejectedDirectSubmitted =
    submitRuntimeCommands(rejectedDirectBridgeEnvironment, { gameAction });
  assert(!rejectedDirectSubmitted.submitted);
  assert(!rejectedDirectSubmitted.errors.empty());

  writeMismatchedRuntimeIdentityReadyFile(bridgePath, bridgeEnvironment.processId, bridgeEnvironment.executablePath);
  RuntimeExecutorSubmitResult rejectedMismatchedIdentity =
    submitRuntimeCommands(bridgeEnvironment, { gameAction });
  assert(!rejectedMismatchedIdentity.submitted);
  assert(rejectedMismatchedIdentity.reason.find("process_id") != std::string::npos);

  writeValidatedAdapterReadyFile(bridgePath, bridgeEnvironment.processId, bridgeEnvironment.executablePath);

  RuntimeEnvironment noManifestEnvironment = bridgeEnvironment;
  noManifestEnvironment.manifestPath.clear();
  RuntimeExecutorSubmitResult rejectedWithoutManifest =
    submitRuntimeCommands(noManifestEnvironment, { gameAction });
  assert(!rejectedWithoutManifest.submitted);
  assert(rejectedWithoutManifest.reason.find("runtime manifest or bridge-proven command surface is required") != std::string::npos);

  const std::filesystem::path bootstrapManifest = bridgePath / "bootstrap.manifest";
  {
    std::ofstream bootstrap(bootstrapManifest);
    bootstrap << "product starcraft-remastered\n";
    bootstrap << "version test-build\n";
    bootstrap << "api-surface-methods 0\n";
    bootstrap << "command-surface-entries 0\n";
  }
  RuntimeEnvironment bootstrapManifestEnvironment = bridgeEnvironment;
  bootstrapManifestEnvironment.manifestPath = bootstrapManifest.string();
  RuntimeExecutorSubmitResult rejectedWithBootstrapManifest =
    submitRuntimeCommands(bootstrapManifestEnvironment, { gameAction });
  assert(!rejectedWithBootstrapManifest.submitted);
  assert(rejectedWithBootstrapManifest.reason.find("runtime manifest contract is invalid") != std::string::npos);

  RuntimeCommandRequest invalidCommand;
  invalidCommand.kind = RuntimeCommandKind::GameAction;
  invalidCommand.name = "notARealAction";
  RuntimeExecutorSubmitResult rejected =
    submitRuntimeCommands(bridgeEnvironment, { invalidCommand });
  assert(!rejected.submitted);
  assert(!rejected.errors.empty());

  RuntimeEnvironment mismatchEnvironment = bridgeEnvironment;
  mismatchEnvironment.product = Product::StarCraftBroodWar1161;
  RuntimeExecutorPreflightResult mismatchPreflight =
    preflightRuntimeExecutor(mismatchEnvironment, complete.manifest.contract);
  assert(!mismatchPreflight.executorAvailable);
  assert(!mismatchPreflight.errors.empty());

  std::filesystem::remove_all(bridgePath);
  std::filesystem::remove_all(directBridgePath);
  std::filesystem::remove(outsideCommandQueuePath);

  return 0;
}
