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
#include <utility>
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

  void writeProofSnapshots(
    const std::filesystem::path& bridgeDir,
    int processId,
    std::uint64_t heartbeat,
    std::uint64_t frameId,
    bool activeMatchCorrelated);

  void writeStaticLiveCommandEvidenceManifest(const std::filesystem::path& path)
  {
    const RuntimeCommandSurface surface = makeBWAPICommandSurface();
    std::ofstream manifest(path);
    manifest << "product starcraft-remastered\n";
    manifest << "version unknown\n";
    manifest << "api-surface-methods 0\n";
    manifest << "command-surface-entries " << surface.totalEntries() << '\n';
    for (const std::string& command : surface.unitCommands)
      manifest << "unit-command " << command << " live-proven static-manifest-test\n";
    for (const std::string& action : surface.gameActions)
      manifest << "game-action " << action << " live-proven static-manifest-test\n";
  }

  std::uint64_t nextResidentProofHeartbeat()
  {
    static std::uint64_t heartbeat = 20;
    return heartbeat++;
  }

  std::uint64_t writeResidentStateProofs(std::ofstream& ready, RuntimeEnvironment environment)
  {
    const std::uint64_t heartbeat = nextResidentProofHeartbeat();
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
    ready << "proof.read_units.snapshot=units.snapshot.tsv\n";
    ready << "proof.active_match_state.evidence=active-unit-node-snapshot\n";
    ready << "proof.active_match_state.active_records=4\n";
    ready << "proof.active_match_state.unit_node_address="
          << reinterpret_cast<std::uintptr_t>(activeUnitEvidence.data()) << '\n';
    ready << "proof.active_match_state.unit_node_record_size=64\n";
    if (!environment.executorBridgePath.empty())
    {
      writeProofSnapshots(
        environment.executorBridgePath,
        environment.processId,
        heartbeat,
        102,
        true);
    }
    return heartbeat;
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
    assert(ensuredQueue.valid);
    for (const std::string& line : makeRuntimeResidentQueueReadyLines(
           RuntimeResidentQueueKind::Proof,
           RuntimeResidentProofQueueFile,
           actualQueue))
    {
      ready << line << '\n';
    }
  }

  void writeProofSnapshotPayload(
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
               << "vector_address\t0x1000\n"
               << "bytes_in_queue_address\t0x1100\n"
               << "buffer_begin\t0x1000\n"
               << "frame_counter_address\t0x1200\n"
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

  void writeProofSnapshot(
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
    writeProofSnapshotPayload(snapshot, proof);
  }

  void writeProofSnapshots(
    const std::filesystem::path& bridgeDir,
    int processId,
    std::uint64_t heartbeat,
    std::uint64_t frameId,
    bool activeMatchCorrelated)
  {
    const std::vector<std::pair<std::string, std::string>> snapshots = {
      { "draw_overlays.snapshot.tsv", "draw_overlays" },
      { "units.snapshot.tsv", "read_units" },
      { "issue_commands.snapshot.tsv", "issue_commands" },
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
      writeProofSnapshot(
        bridgeDir / snapshotSpec.first,
        snapshotSpec.second,
        processId,
        heartbeat,
        frameId,
        activeMatchCorrelated);
    }
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
  assert(remasteredProbe.implementedUnitCommandEvidence.size() == remasteredSurface.unitCommands.size());
  assert(remasteredProbe.implementedGameActionEvidence.size() == remasteredSurface.gameActions.size());
  assert(commandEvidenceStatusFor(
    remasteredProbe.implementedUnitCommandEvidence,
    "Attack_Move") == RuntimeCommandEvidenceStatus::MockTested);
  assert(commandEvidenceStatusFor(
    remasteredProbe.implementedGameActionEvidence,
    "drawBox") == RuntimeCommandEvidenceStatus::AdapterLocal);

  const std::filesystem::path staticLiveManifestPath =
    std::filesystem::temp_directory_path() / "starcraft-api-static-live-command-evidence.manifest";
  writeStaticLiveCommandEvidenceManifest(staticLiveManifestPath);
  RuntimeEnvironment staticLiveManifestEnvironment = remastered;
  staticLiveManifestEnvironment.manifestPath = staticLiveManifestPath.string();
  std::unique_ptr<RuntimeBackend> staticLiveManifestBackend =
    createRuntimeBackend(staticLiveManifestEnvironment);
  RuntimeProbeResult staticLiveManifestProbe = staticLiveManifestBackend->probe();
  assert(!staticLiveManifestProbe.supported);
  assert(commandEvidenceStatusFor(
    staticLiveManifestProbe.implementedUnitCommandEvidence,
    "Attack_Move") == RuntimeCommandEvidenceStatus::DocumentedScenario);
  assert(commandEvidenceStatusFor(
    staticLiveManifestProbe.implementedGameActionEvidence,
    "pauseGame") == RuntimeCommandEvidenceStatus::DocumentedScenario);
  std::filesystem::remove(staticLiveManifestPath);

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
  writeProofSnapshots(bridgeDir, currentProcessId(), 20, 102, true);
  {
    std::ofstream ready(bridgeDir / RuntimeExecutorBridgeReadyFile);
    ready << "protocol=" << RuntimeExecutorBridgeProtocol << '\n';
    ready << "product=starcraft-remastered\n";
    ready << "version=unknown\n";
    ready << "process_id=" << currentProcessId() << '\n';
    ready << "executor=starcraft-api-resident-adapter\n";
    ready << "mode=" << RuntimeExecutorBridgeValidatedAdapterMode << '\n';
    ready << "proof.attach=passed\n";
    ready << "proof.attach.source=resident-adapter\n";
    ready << "proof.attach.queue=resident-proof.queue\n";
    RuntimeEnvironment proofEnvironment = attachableRemastered;
    proofEnvironment.executorBridgePath = bridgeDir.string();
    const std::uint64_t heartbeat = writeResidentStateProofs(ready, proofEnvironment);
    writeResidentProofQueueReadyLines(ready, bridgeDir, heartbeat);
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
  assert(commandEvidenceStatusFor(
    proofBackedRemasteredProbe.implementedUnitCommandEvidence,
    "Attack_Move") == RuntimeCommandEvidenceStatus::MockTested);

  {
    std::ofstream ready(bridgeDir / RuntimeExecutorBridgeReadyFile);
    ready << "protocol=" << RuntimeExecutorBridgeProtocol << '\n';
    ready << "product=starcraft-remastered\n";
    ready << "version=unknown\n";
    ready << "process_id=" << currentProcessId() << '\n';
    ready << "executor=starcraft-api-resident-adapter\n";
    ready << "mode=" << RuntimeExecutorBridgeValidatedAdapterMode << '\n';
    ready << "proof.attach=passed\n";
    ready << RuntimeExecutorBridgeCommandSurfaceLine << '\n';
  }

  RuntimeProbeResult commandSurfaceOnlyProbe = proofBackedRemasteredBackend->probe();
  assert(!hasCapability(commandSurfaceOnlyProbe, Capability::IssueCommands));
  assert(commandSurfaceOnlyProbe.implementedCommandSurfaceEntries == remasteredSurface.totalEntries());
  assert(commandSurfaceOnlyProbe.implementedUnitCommands == remasteredSurface.unitCommands);
  assert(commandSurfaceOnlyProbe.implementedGameActions == remasteredSurface.gameActions);
  assert(commandEvidenceStatusFor(
    commandSurfaceOnlyProbe.implementedGameActionEvidence,
    "drawBox") == RuntimeCommandEvidenceStatus::AdapterLocal);

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
    ready << "proof.issue_commands.command=pauseGame/resumeGame\n";
    ready << "proof.issue_commands.source=live-sc-r-command-path\n";
    ready << "proof.issue_commands.delivery_checked=true\n";
    ready << "proof.issue_commands.behavior_checked=true\n";
    ready << "proof.issue_commands.live_behavior_witness=starcraft-runtime-adapter-proof-live-write-v1\n";
    ready << "proof.issue_commands.self_fixture=false\n";
    ready << "proof.issue_commands.pause_frame_counter_matched=true\n";
    ready << "proof.issue_commands.vector_address=0x1000\n";
    ready << "proof.issue_commands.storage_kind=live-sc-r-command-queue-v1\n";
    ready << "proof.issue_commands.bytes_in_queue_address=0x1100\n";
    ready << "proof.issue_commands.frame_counter_address=0x1200\n";
    ready << "proof.issue_commands.encoded_bytes=10 / 11\n";
    ready << "proof.issue_commands.stale_proof_bytes_cleared=true\n";
    ready << "proof.issue_commands.snapshot=issue_commands.snapshot.tsv\n";
    ready << "command_surface.live_unit_command.0=Attack_Move|live-proven|proof.issue_commands=passed:Attack_Move\n";
    ready << "command_surface.live_game_action.0=pauseGame|live-proven|proof.issue_commands=passed:pauseGame\n";
    RuntimeEnvironment fakeLiveEvidenceEnvironment = attachableRemastered;
    fakeLiveEvidenceEnvironment.executorBridgePath = bridgeDir.string();
    writeResidentStateProofs(ready, fakeLiveEvidenceEnvironment);
    ready << "proof.issue_commands=passed\n";
  }

  RuntimeProbeResult rejectedFakeLiveEvidenceProbe = proofBackedRemasteredBackend->probe();
  assert(!hasCapability(rejectedFakeLiveEvidenceProbe, Capability::IssueCommands));
  assert(commandEvidenceStatusFor(
    rejectedFakeLiveEvidenceProbe.implementedUnitCommandEvidence,
    "Attack_Move") == RuntimeCommandEvidenceStatus::MockTested);
  assert(commandEvidenceStatusFor(
    rejectedFakeLiveEvidenceProbe.implementedGameActionEvidence,
    "pauseGame") == RuntimeCommandEvidenceStatus::MockTested);

  {
    std::ofstream ready(bridgeDir / RuntimeExecutorBridgeReadyFile);
    ready << "protocol=" << RuntimeExecutorBridgeProtocol << '\n';
    ready << "product=starcraft-remastered\n";
    ready << "version=unknown\n";
    ready << "process_id=" << currentProcessId() << '\n';
    ready << "executor=starcraft-api-resident-adapter\n";
    ready << "mode=" << RuntimeExecutorBridgeValidatedAdapterMode << '\n';
    ready << RuntimeExecutorBridgeActiveCommandReceiverLine << '\n';
    ready << RuntimeExecutorBridgeRuntimeCommandQueueSinkLine << '\n';
    ready << "proof.attach.source=resident-adapter\n";
    RuntimeEnvironment issueProofEnvironment = attachableRemastered;
    issueProofEnvironment.executorBridgePath = bridgeDir.string();
    writeResidentStateProofs(ready, issueProofEnvironment);
    ready << "contract.binding.BW::BWDATA::sgdwBytesInCmdQueue=command-queue|proof.issue_commands=passed:bytes-in-command-queue\n";
    ready << "contract.binding.BW::BWDATA::TurnBuffer=command-queue|proof.issue_commands=passed:turn-buffer\n";
    ready << "proof.issue_commands.command=pauseGame/resumeGame\n";
    ready << "proof.issue_commands.source=live-sc-r-command-path\n";
    ready << "proof.issue_commands.delivery_checked=true\n";
    ready << "proof.issue_commands.behavior_checked=true\n";
    ready << "proof.issue_commands.live_behavior_witness=starcraft-runtime-adapter-proof-live-write-v1\n";
    ready << "proof.issue_commands.self_fixture=false\n";
    ready << "proof.issue_commands.pause_frame_counter_matched=true\n";
    ready << "proof.issue_commands.vector_address=0x1000\n";
    ready << "proof.issue_commands.storage_kind=live-sc-r-command-queue-v1\n";
    ready << "proof.issue_commands.bytes_in_queue_address=0x1100\n";
    ready << "proof.issue_commands.frame_counter_address=0x1200\n";
    ready << "proof.issue_commands.encoded_bytes=10 / 11\n";
    ready << "proof.issue_commands.stale_proof_bytes_cleared=true\n";
    ready << "proof.issue_commands.snapshot=issue_commands.snapshot.tsv\n";
    ready << "proof.attach=passed\n";
    ready << "proof.issue_commands=passed\n";
  }

  RuntimeProbeResult issueCommandOnlyProbe = proofBackedRemasteredBackend->probe();
  assert(!hasCapability(issueCommandOnlyProbe, Capability::IssueCommands));
  assert(!hasCapability(issueCommandOnlyProbe, Capability::DrawOverlays));
  assert(issueCommandOnlyProbe.implementedCommandSurfaceEntries == remasteredSurface.totalEntries());
  assert(issueCommandOnlyProbe.implementedUnitCommands == remasteredSurface.unitCommands);
  assert(issueCommandOnlyProbe.implementedGameActions == remasteredSurface.gameActions);
  assert(commandEvidenceStatusFor(
    issueCommandOnlyProbe.implementedUnitCommandEvidence,
    "Attack_Move") == RuntimeCommandEvidenceStatus::MockTested);

  {
    std::ofstream ready(bridgeDir / RuntimeExecutorBridgeReadyFile);
    ready << "protocol=" << RuntimeExecutorBridgeProtocol << '\n';
    ready << "product=starcraft-remastered\n";
    ready << "version=unknown\n";
    ready << "process_id=" << currentProcessId() << '\n';
    ready << "executor=starcraft-api-resident-adapter\n";
    ready << "mode=" << RuntimeExecutorBridgeValidatedAdapterMode << '\n';
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
    ready << "proof.issue_commands.vector_address=0x1000\n";
    ready << "proof.issue_commands.storage_kind=live-sc-r-command-queue-v1\n";
    ready << "proof.issue_commands.bytes_in_queue_address=0x1100\n";
    ready << "proof.issue_commands.frame_counter_address=0x1200\n";
    ready << "proof.issue_commands.encoded_bytes=10 / 11\n";
    ready << "proof.issue_commands.stale_proof_bytes_cleared=true\n";
    ready << "proof.issue_commands.snapshot=issue_commands.snapshot.tsv\n";
    ready << "command_surface.live_unit_command.0=Attack_Move|live-proven|proof.issue_commands=passed:Attack_Move\n";
    ready << "command_surface.live_game_action.0=pauseGame|live-proven|proof.issue_commands=passed:pauseGame\n";
    ready << "proof.attach.source=resident-adapter\n";
    RuntimeEnvironment fullProofEnvironment = attachableRemastered;
    fullProofEnvironment.executorBridgePath = bridgeDir.string();
    writeResidentStateProofs(ready, fullProofEnvironment);
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
    writeBehaviorProofLines(ready);
    ready << "proof.read_map_data=passed\n";
    ready << "proof.read_player_data=passed\n";
    ready << "proof.read_bullet_data=passed\n";
  }

  RuntimeProbeResult commandProofRemasteredProbe = proofBackedRemasteredBackend->probe();
  assert(!hasCapability(commandProofRemasteredProbe, Capability::IssueCommands));
  assert(hasCapability(commandProofRemasteredProbe, Capability::DrawOverlays));
  assert(hasCapability(commandProofRemasteredProbe, Capability::ReadMapData));
  assert(hasCapability(commandProofRemasteredProbe, Capability::ReadPlayerData));
  assert(hasCapability(commandProofRemasteredProbe, Capability::ReadBulletData));
  assert(hasCapability(commandProofRemasteredProbe, Capability::ReadRegionData));
  assert(hasCapability(commandProofRemasteredProbe, Capability::LoadAIModules));
  assert(commandProofRemasteredProbe.implementedCommandSurfaceEntries == remasteredSurface.totalEntries());
  assert(commandProofRemasteredProbe.implementedUnitCommands == remasteredSurface.unitCommands);
  assert(commandProofRemasteredProbe.implementedGameActions == remasteredSurface.gameActions);
  assert(commandEvidenceStatusFor(
    commandProofRemasteredProbe.implementedUnitCommandEvidence,
    "Attack_Move") == RuntimeCommandEvidenceStatus::MockTested);
  assert(commandEvidenceStatusFor(
    commandProofRemasteredProbe.implementedUnitCommandEvidence,
    "Attack_Unit") == RuntimeCommandEvidenceStatus::MockTested);
  assert(commandEvidenceStatusFor(
    commandProofRemasteredProbe.implementedGameActionEvidence,
    "pauseGame") == RuntimeCommandEvidenceStatus::MockTested);
  assert(commandEvidenceStatusFor(
    commandProofRemasteredProbe.implementedGameActionEvidence,
    "drawBox") == RuntimeCommandEvidenceStatus::AdapterLocal);
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
