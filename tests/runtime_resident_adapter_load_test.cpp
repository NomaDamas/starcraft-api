#include <BWAPI/Runtime/RuntimeBackend.h>
#include <BWAPI/Runtime/RuntimeExecutor.h>
#include <BWAPI/Runtime/RuntimeProcess.h>
#include <BWAPI/Runtime/RuntimeResidentBridge.h>

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>

using namespace BWAPI::Runtime;

namespace
{
  struct CompactUnitNodeFixture
  {
    std::uint64_t previous = 0;
    std::uint64_t next = 0;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::uint64_t sprite = 0;
    std::uint64_t secondary = 0;
  };

  static_assert(sizeof(CompactUnitNodeFixture) == 0x28);

  alignas(16) std::array<CompactUnitNodeFixture, 3> residentUnitNodeFixture = {};
  alignas(16) std::array<std::array<unsigned char, 0xd0>, 3> residentSpriteFixture = {};
  alignas(16) std::array<std::array<unsigned char, 0xe0>, 3> residentSecondaryFixture = {};
  alignas(4) std::atomic<std::uint32_t> residentFrameCounterFixture{ 128 };

  struct ResidentFrameCounterFixtureTicker
  {
    std::atomic<bool> running{ true };
    std::thread thread;

    ResidentFrameCounterFixtureTicker()
      : thread([this]()
        {
          while (running.load(std::memory_order_relaxed))
          {
            residentFrameCounterFixture.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::milliseconds(42));
          }
        })
    {
    }

    ~ResidentFrameCounterFixtureTicker()
    {
      running.store(false, std::memory_order_relaxed);
      if (thread.joinable())
        thread.join();
    }
  };

  void require(bool condition, const std::string& message)
  {
    if (condition)
      return;
    std::cerr << message << '\n';
    std::exit(1);
  }

  template <std::size_t Size>
  void writeU16LE(std::array<unsigned char, Size>& bytes, std::size_t offset, std::uint16_t value)
  {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
  }

  template <std::size_t Size>
  void writeU32LE(std::array<unsigned char, Size>& bytes, std::size_t offset, std::uint32_t value)
  {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
  }

  void populateResidentUnitFixture()
  {
    for (std::size_t index = 0; index < residentUnitNodeFixture.size(); ++index)
    {
      residentSpriteFixture[index].fill(0);
      residentSecondaryFixture[index].fill(0);

      writeU32LE(residentSpriteFixture[index], 0x68, static_cast<std::uint32_t>(1 + index));
      writeU32LE(residentSpriteFixture[index], 0x6c, static_cast<std::uint32_t>(index % 2));
      writeU32LE(residentSpriteFixture[index], 0x80, static_cast<std::uint32_t>(4096 + index));

      residentSecondaryFixture[index][0x14] = static_cast<unsigned char>(index % 2);
      residentSecondaryFixture[index][0x1a] = 20;
      residentSecondaryFixture[index][0x1b] = 40;
      residentSecondaryFixture[index][0xc0] = static_cast<unsigned char>(index % 2);
      writeU16LE(residentSecondaryFixture[index], 0x10, static_cast<std::uint16_t>(1 + index));
      writeU16LE(residentSecondaryFixture[index], 0xd0, static_cast<std::uint16_t>(1 + index));
    }

    for (std::size_t index = 0; index < residentUnitNodeFixture.size(); ++index)
    {
      CompactUnitNodeFixture& node = residentUnitNodeFixture[index];
      node.previous = reinterpret_cast<std::uint64_t>(
        &residentUnitNodeFixture[(index + residentUnitNodeFixture.size() - 1) % residentUnitNodeFixture.size()]);
      node.next = reinterpret_cast<std::uint64_t>(
        &residentUnitNodeFixture[(index + 1) % residentUnitNodeFixture.size()]);
      node.x = static_cast<std::int32_t>(64 + (index * 32));
      node.y = static_cast<std::int32_t>(80 + (index * 32));
      node.sprite = reinterpret_cast<std::uint64_t>(residentSpriteFixture[index].data());
      node.secondary = reinterpret_cast<std::uint64_t>(residentSecondaryFixture[index].data());
    }
  }

  bool fileContainsLine(const std::filesystem::path& path, const std::string& expected)
  {
    std::ifstream input(path);
    std::string line;
    while (std::getline(input, line))
    {
      if (line == expected)
        return true;
    }
    return false;
  }

  bool fileContainsPrefix(const std::filesystem::path& path, const std::string& expectedPrefix)
  {
    std::ifstream input(path);
    std::string line;
    while (std::getline(input, line))
    {
      if (line.rfind(expectedPrefix, 0) == 0)
        return true;
    }
    return false;
  }

  bool fileContainsSubstring(const std::filesystem::path& path, const std::string& expected)
  {
    std::ifstream input(path);
    std::string line;
    while (std::getline(input, line))
    {
      if (line.find(expected) != std::string::npos)
        return true;
    }
    return false;
  }

  std::string fileValue(const std::filesystem::path& path, const std::string& key)
  {
    const std::string prefix = key + '=';
    std::ifstream input(path);
    std::string line;
    while (std::getline(input, line))
    {
      if (line.rfind(prefix, 0) == 0)
        return line.substr(prefix.size());
    }
    return {};
  }

  std::string snapshotMetadataValue(const std::filesystem::path& path, const std::string& key)
  {
    const std::string prefix = "# " + key + '=';
    std::ifstream input(path);
    std::string line;
    while (std::getline(input, line))
    {
      if (line.rfind(prefix, 0) == 0)
        return line.substr(prefix.size());
    }
    return {};
  }

  bool parseUnsignedStrict(const std::string& value, std::uint64_t& parsed)
  {
    if (value.empty())
      return false;
    char* end = nullptr;
    parsed = std::strtoull(value.c_str(), &end, 10);
    return end != nullptr && *end == '\0';
  }

  std::uint64_t requireUnsignedValue(const std::string& value, const std::string& context)
  {
    std::uint64_t parsed = 0;
    require(parseUnsignedStrict(value, parsed), "missing or invalid unsigned value for " + context + ": " + value);
    return parsed;
  }

  bool waitForLine(
    const std::filesystem::path& path,
    const std::string& expected,
    int attempts = 60)
  {
    for (int attempt = 0; attempt < attempts; ++attempt)
    {
      if (fileContainsLine(path, expected))
        return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return fileContainsLine(path, expected);
  }

  bool waitForReadyUnsignedGreater(
    const std::filesystem::path& path,
    const std::string& key,
    std::uint64_t previous,
    int attempts = 60)
  {
    for (int attempt = 0; attempt < attempts; ++attempt)
    {
      const std::string value = fileValue(path, key);
      std::uint64_t parsed = 0;
      if (parseUnsignedStrict(value, parsed) && parsed > previous)
        return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    const std::string value = fileValue(path, key);
    std::uint64_t parsed = 0;
    return parseUnsignedStrict(value, parsed) && parsed > previous;
  }

  void writeCommandSpecificIssueSnapshot(
    const std::filesystem::path& path,
    const std::string& command,
    std::uint64_t processId,
    std::uint64_t heartbeat,
    std::uint64_t frameId)
  {
    std::ofstream snapshot(path, std::ios::trunc);
    snapshot << "# schema=starcraft-api.resident-snapshot.v1\n"
             << "# proof=issue_commands.command\n"
             << "# source_identity=resident-adapter\n"
             << "# process_id=" << processId << '\n'
             << "# heartbeat=" << heartbeat << '\n'
             << "# frame_id=" << frameId << '\n'
             << "# active_match_correlated=true\n"
             << "# command=" << command << '\n'
             << "# command_kind=game-action\n"
             << "field\tvalue\n"
             << "passed\ttrue\n"
             << "delivery_checked\ttrue\n"
             << "behavior_checked\ttrue\n"
             << "receiver_active\ttrue\n"
             << "behavior_observed\ttrue\n"
             << "self_fixture\tfalse\n"
             << "live_behavior_witness\tstarcraft-runtime-adapter-proof-command-behavior-v1\n"
             << "command\t" << command << '\n'
             << "command_kind\tgame-action\n"
             << "encoded_bytes\t10\n"
             << "attempt_count\t1\n"
             << "storage_kind\tlive-sc-r-command-queue-v1\n"
             << "vector_address\t0x1000\n"
             << "bytes_in_queue_address\t0x1100\n"
             << "frame_counter_address\t0x1200\n"
             << "appended_bytes\t10\n"
             << "behavior_sample_count\t2\n"
             << "behavior_observation\tcommand-specific-live-scr-behavior\n"
             << "baseline_delta\t12\n"
             << "paused_delta\t0\n"
             << "resumed_delta\t12\n"
             << "issue_commands_required_adapter_abi\tstarcraft-api-resident-adapter-v1\n"
             << "issue_commands_required_adapter_location\tin-process-target-runtime\n"
             << "issue_commands_required_adapter_thread_policy\texecute-on-target-runtime-thread\n"
             << "issue_commands_required_adapter_behavior\tencoded-bwapi-command-reaches-live-scr-command-path-and-changes-frame-behavior\n"
             << "issue_commands_required_adapter_promotion_rule\tpromote-only-this-command-name-from-this-snapshot\n";
  }

  void writeCommandSpecificPendingFile(const std::filesystem::path& bridgePath)
  {
    std::ofstream pending(
      bridgePath / RuntimeExecutorBridgeIssueCommandProofPendingFile,
      std::ios::trunc);
    pending << "command\tcommand_kind\tsnapshot\n"
            << "pauseGame\tgame-action\tissue_commands.pauseGame.snapshot.tsv\n"
            << "resumeGame\tgame-action\tissue_commands.resumeGame.snapshot.tsv\n";
  }
}

int main(int argc, char** argv)
{
  assert(argc > 0);
  populateResidentUnitFixture();
  ResidentFrameCounterFixtureTicker frameTicker;

  const std::filesystem::path bridgePath =
    std::filesystem::temp_directory_path() / "starcraft-api-resident-adapter-load-test";
  std::filesystem::remove_all(bridgePath);
  std::filesystem::create_directories(bridgePath);

  setenv("STARCRAFT_API_RESIDENT_ENABLE", "1", 1);
  setenv("STARCRAFT_API_EXECUTOR_BRIDGE_DIR", bridgePath.string().c_str(), 1);
  setenv("STARCRAFT_API_VERSION", "test-build", 1);

  void* handle = dlopen(STARCRAFT_API_TEST_RESIDENT_DYLIB, RTLD_NOW);
  assert(handle != nullptr);

  using AbiFunction = const char* (*)();
  using EntryFunction = int (*)();
  using StopFunction = int (*)();
  auto abi = reinterpret_cast<AbiFunction>(dlsym(handle, "starcraft_api_resident_adapter_abi"));
  auto entry = reinterpret_cast<EntryFunction>(dlsym(handle, "starcraft_api_resident_adapter_entry"));
  auto stop = reinterpret_cast<StopFunction>(dlsym(handle, "starcraft_api_resident_adapter_stop"));
  assert(abi != nullptr);
  assert(entry != nullptr);
  assert(stop != nullptr);
  assert(std::strcmp(abi(), RuntimeResidentAdapterAbi) == 0);
  assert(entry() == 0);

  const std::filesystem::path readyPath = bridgePath / RuntimeExecutorBridgeReadyFile;
  const std::filesystem::path commandQueuePath = bridgePath / RuntimeResidentCommandQueueFile;
  const std::filesystem::path overlayQueuePath = bridgePath / RuntimeResidentOverlayQueueFile;
  const std::filesystem::path proofQueuePath = bridgePath / RuntimeResidentProofQueueFile;
  for (int attempt = 0; attempt < 30; ++attempt)
  {
    if (std::filesystem::exists(readyPath)
        && std::filesystem::exists(commandQueuePath)
        && std::filesystem::exists(overlayQueuePath)
        && std::filesystem::exists(proofQueuePath))
    {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  assert(std::filesystem::exists(readyPath));
  assert(std::filesystem::exists(commandQueuePath));
  assert(std::filesystem::exists(overlayQueuePath));
  assert(std::filesystem::exists(proofQueuePath));
  assert(fileContainsLine(readyPath, "proof.attach=passed"));
  assert(fileContainsPrefix(readyPath, "resident.queue.command.path="));
  assert(fileContainsPrefix(readyPath, "resident.queue.overlay.path="));
  assert(fileContainsPrefix(readyPath, "resident.queue.proof.path="));
  assert(fileContainsLine(
    readyPath,
    "contract.binding.shared-memory-client-transport=transport|proof.attach=passed:resident-proof-queue-v1"));
  assert(fileContainsLine(readyPath, "resident.projection.bwgame.validation=resident-bwgame-projection-v1"));
  assert(fileContainsPrefix(readyPath, "resident.projection.bwgame.address=0x"));
  assert(fileContainsLine(readyPath, "resident.projection.bwgame.size=256"));
  assert(fileContainsLine(readyPath, "resident.projection.bwgame.elapsedFrames_offset=8"));
  assert(fileContainsLine(readyPath, "resident.projection.bwgame.elapsedFrames_bytes=4"));
  require(
    waitForLine(readyPath, "proof.read_units.snapshot=units.snapshot.tsv"),
    "resident adapter did not write live read-units snapshot proof");
  require(
    std::filesystem::exists(bridgePath / "units.snapshot.tsv"),
    "resident adapter ready file claimed units snapshot but the snapshot file is missing");

  {
    std::ofstream ready(readyPath, std::ios::app);
    ready << "proof.active_match_state=passed\n";
    ready << "proof.active_match_state.evidence=active-unit-node-snapshot\n";
    ready << "proof.active_match_state.active_records=3\n";
    ready << "proof.active_match_state.unit_node_address=0x1234000\n";
    ready << "proof.active_match_state.unit_node_record_size=40\n";
    ready << "proof.read_units=passed\n";
    ready << "proof.read_units.address=0x1234000\n";
    ready << "proof.read_units.record_size=40\n";
    ready << "proof.read_units.active_records=3\n";
    ready << "contract.binding.BW::BWDATA::UnitNodeTable=data-address|proof.read_units=passed\n";
    ready << "proof.read_map_data=passed\n";
    ready << "contract.binding.BW::BWDATA::MapTileArray=data-address|proof.read_map_data=passed:compat-map-tile-projection\n";
    ready << "diagnostic.read_map_data.snapshot=map.snapshot.tsv\n";
    ready << "proof.read_bullet_data=passed\n";
    ready << "proof.read_bullet_data.source=live-sc-r-bullet-table\n";
    ready << "proof.read_bullet_data.address=0x77770000\n";
    ready << "proof.read_bullet_data.record_size=136\n";
    ready << "proof.read_bullet_data.active_records=1\n";
    ready << "proof.read_bullet_data.unit_correlated_records=1\n";
    ready << "proof.read_bullet_data.snapshot=bullets.snapshot.tsv\n";
    ready << "contract.binding.BW::BWDATA::BulletNodeTable=data-address|proof.read_bullet_data=passed:stale\n";
    ready << "contract.structure.BW::CBullet=136|proof.read_bullet_data=passed:stale\n";
    ready << "contract.field.BW::CBullet.position=64|4|proof.read_bullet_data=passed\n";
    ready << "diagnostic.read_bullet_data.snapshot=bullets.snapshot.tsv\n";
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  assert(fileContainsLine(readyPath, "proof.active_match_state=passed"));
  assert(fileContainsLine(readyPath, "proof.read_units=passed"));
  assert(fileContainsLine(
    readyPath,
    "contract.binding.BW::BWDATA::UnitNodeTable=data-address|proof.read_units=passed"));
  assert(!fileContainsLine(readyPath, "proof.read_map_data=passed"));
  assert(!fileContainsLine(
    readyPath,
    "contract.binding.BW::BWDATA::MapTileArray=data-address|proof.read_map_data=passed:compat-map-tile-projection"));
  assert(fileContainsLine(readyPath, "diagnostic.read_map_data.snapshot=map.snapshot.tsv"));
  assert(!fileContainsLine(readyPath, "proof.read_bullet_data=passed"));
  assert(!fileContainsPrefix(readyPath, "proof.read_bullet_data.source="));
  assert(!fileContainsPrefix(readyPath, "proof.read_bullet_data.address="));
  assert(!fileContainsPrefix(readyPath, "proof.read_bullet_data.record_size="));
  assert(!fileContainsPrefix(readyPath, "proof.read_bullet_data.active_records="));
  assert(!fileContainsPrefix(readyPath, "proof.read_bullet_data.unit_correlated_records="));
  assert(!fileContainsPrefix(readyPath, "proof.read_bullet_data.snapshot="));
  assert(!fileContainsLine(
    readyPath,
    "contract.binding.BW::BWDATA::BulletNodeTable=data-address|proof.read_bullet_data=passed:stale"));
  assert(!fileContainsLine(
    readyPath,
    "contract.structure.BW::CBullet=136|proof.read_bullet_data=passed:stale"));
  assert(!fileContainsLine(
    readyPath,
    "contract.field.BW::CBullet.position=64|4|proof.read_bullet_data=passed"));
  assert(fileContainsLine(readyPath, "diagnostic.read_bullet_data.snapshot=bullets.snapshot.tsv"));

  RuntimeEnvironment environment = RuntimeEnvironment::detectHost();
  environment.product = Product::StarCraftRemastered;
  environment.version = "test-build";
  environment.processId = static_cast<int>(getpid());
  environment.executablePath = std::filesystem::absolute(argv[0]).lexically_normal().string();
  environment.executorBridgePath = bridgePath.string();

  RuntimeResidentBridgeValidationResult resident =
    validateRuntimeResidentBridgeReadyFile(environment, readyPath);
  assert(resident.valid);
  assert(resident.active);
  assert(resident.processId == environment.processId);

  RuntimeResidentQueueValidationResult proofQueue =
    validateRuntimeResidentQueueFile(proofQueuePath, RuntimeResidentQueueKind::Proof);
  RuntimeResidentQueueValidationResult commandQueue =
    validateRuntimeResidentQueueFile(commandQueuePath, RuntimeResidentQueueKind::Command);
  RuntimeResidentQueueValidationResult overlayQueue =
    validateRuntimeResidentQueueFile(overlayQueuePath, RuntimeResidentQueueKind::Overlay);
  assert(commandQueue.valid);
  assert(overlayQueue.valid);
  assert(proofQueue.valid);

  const std::uint64_t commandProofHeartbeat =
    requireUnsignedValue(fileValue(readyPath, "resident.adapter.heartbeat"), "resident.adapter.heartbeat");
  const std::uint64_t commandProofFrame =
    requireUnsignedValue(
      snapshotMetadataValue(bridgePath / "units.snapshot.tsv", "frame_id"),
      "units.snapshot.tsv#frame_id");
  writeCommandSpecificIssueSnapshot(
    bridgePath / "issue_commands.pauseGame.snapshot.tsv",
    "pauseGame",
    static_cast<std::uint64_t>(environment.processId),
    commandProofHeartbeat,
    commandProofFrame);
  writeCommandSpecificIssueSnapshot(
    bridgePath / "issue_commands.resumeGame.snapshot.tsv",
    "resumeGame",
    static_cast<std::uint64_t>(environment.processId),
    commandProofHeartbeat,
    commandProofFrame);
  writeCommandSpecificPendingFile(bridgePath);

  require(
    waitForLine(readyPath, "proof.issue_commands.command.pauseGame=passed"),
    "resident adapter did not promote pending pauseGame command-specific proof");
  require(
    waitForLine(readyPath, "proof.issue_commands.command.resumeGame=passed"),
    "resident adapter did not promote pending resumeGame command-specific proof");
  require(
    fileContainsLine(
      readyPath,
      "command_surface.live_game_action.0=pauseGame|live-proven|proof.issue_commands.command.pauseGame=passed"),
    "pauseGame live-proven command surface row is missing");
  require(
    fileContainsLine(
      readyPath,
      "command_surface.live_game_action.1=resumeGame|live-proven|proof.issue_commands.command.resumeGame=passed"),
    "resumeGame live-proven command surface row is missing");
  require(
    !fileContainsLine(readyPath, "proof.issue_commands=passed"),
    "command-specific proof must not promote aggregate proof.issue_commands=passed");
  require(
    !std::filesystem::exists(bridgePath / RuntimeExecutorBridgeIssueCommandProofPendingFile),
    "resident adapter did not remove consumed command-specific pending proof handoff");
  const std::uint64_t firstRefreshedCommandProofHeartbeat =
    requireUnsignedValue(
      snapshotMetadataValue(bridgePath / "issue_commands.pauseGame.snapshot.tsv", "heartbeat"),
      "issue_commands.pauseGame.snapshot.tsv#heartbeat");
  require(
    firstRefreshedCommandProofHeartbeat >= commandProofHeartbeat,
    "resident adapter did not rewrite command-specific snapshot with a current heartbeat");

  const std::uint64_t firstReadyHeartbeat =
    requireUnsignedValue(fileValue(readyPath, "resident.adapter.heartbeat"), "resident.adapter.heartbeat");
  require(
    waitForReadyUnsignedGreater(readyPath, "resident.adapter.heartbeat", firstReadyHeartbeat),
    "resident adapter heartbeat did not advance after command-specific proof promotion");
  require(
    fileContainsLine(readyPath, "proof.issue_commands.command.pauseGame=passed"),
    "pauseGame command-specific proof was dropped on resident heartbeat refresh");
  require(
    fileContainsLine(readyPath, "proof.issue_commands.command.resumeGame=passed"),
    "resumeGame command-specific proof was dropped on resident heartbeat refresh");
  require(
    fileContainsLine(
      readyPath,
      "command_surface.live_game_action.0=pauseGame|live-proven|proof.issue_commands.command.pauseGame=passed"),
    "pauseGame live-proven command surface row was dropped on resident heartbeat refresh");
  require(
    fileContainsLine(
      readyPath,
      "command_surface.live_game_action.1=resumeGame|live-proven|proof.issue_commands.command.resumeGame=passed"),
    "resumeGame live-proven command surface row was dropped on resident heartbeat refresh");
  require(
    !fileContainsLine(readyPath, "proof.issue_commands=passed"),
    "resident heartbeat refresh promoted forbidden aggregate proof.issue_commands=passed");
  require(
    requireUnsignedValue(
      snapshotMetadataValue(bridgePath / "issue_commands.pauseGame.snapshot.tsv", "heartbeat"),
      "issue_commands.pauseGame.snapshot.tsv#heartbeat")
      > firstRefreshedCommandProofHeartbeat,
    "resident adapter did not refresh command-specific snapshot heartbeat on subsequent ready rewrite");

  RuntimeCommandRequest pauseCommand;
  pauseCommand.kind = RuntimeCommandKind::GameAction;
  pauseCommand.name = "pauseGame";
  RuntimeExecutorSubmitResult submitted =
    submitRuntimeCommands(environment, { pauseCommand });
  assert(submitted.submitted);
  assert(submitted.submittedCommands == 1);
  assert(!submitted.warnings.empty());

  const std::filesystem::path consumedPath = bridgePath / "resident-command.consumed.tsv";
  for (int attempt = 0; attempt < 30; ++attempt)
  {
    if (fileContainsSubstring(consumedPath, "game-action|pauseGame|0|"))
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  assert(fileContainsSubstring(consumedPath, "game-action|pauseGame|0|"));

  assert(stop() == 0);
  dlclose(handle);
  std::filesystem::remove_all(bridgePath);
  return 0;
}
