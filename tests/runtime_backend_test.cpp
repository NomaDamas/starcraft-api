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
    for (const RuntimeExecutorBehaviorProof& proof : requiredRuntimeExecutorBehaviorProofs())
      ready << proof.readyFileLine << '\n';
    ready << "proof.read_map_data=passed\n";
    ready << "proof.read_player_data=passed\n";
    ready << "proof.read_bullet_data=passed\n";
    ready << "proof.read_region_data=passed\n";
    ready << "proof.load_ai_modules=passed\n";
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
