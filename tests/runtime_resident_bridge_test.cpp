#include <BWAPI/Runtime/RuntimeExecutor.h>
#include <BWAPI/Runtime/RuntimeManifest.h>
#include <BWAPI/Runtime/RuntimeProcessMemory.h>
#include <BWAPI/Runtime/RuntimeResidentBridge.h>
#include <BWAPI/Runtime/RuntimeResidentLoader.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace BWAPI::Runtime;

namespace
{
  std::atomic<std::uint32_t> residentFrameCounter{ 402 };
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

  std::filesystem::path makeBridgePath(const std::string& name)
  {
    std::filesystem::path path = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
  }

  RuntimeEnvironment makeEnvironment(const std::string& executable, const std::filesystem::path& bridgePath)
  {
    RuntimeEnvironment environment = RuntimeEnvironment::detectHost();
    environment.product = Product::StarCraftRemastered;
    environment.version = "test-build";
    environment.processId = currentProcessId();
    environment.executablePath = executable;
    environment.manifestPath = fixturePath("remastered-complete.manifest");
    environment.executorBridgePath = bridgePath.string();
    return environment;
  }

  void writeReadyFileWithResidentLines(
    const RuntimeEnvironment& environment,
    const std::vector<std::string>& residentLines,
    const std::vector<std::string>& extraLines = {})
  {
    std::ofstream ready(std::filesystem::path(environment.executorBridgePath) / RuntimeExecutorBridgeReadyFile);
    ready << "protocol=" << RuntimeExecutorBridgeProtocol << '\n';
    ready << "product=" << toString(environment.product) << '\n';
    ready << "version=" << environment.version << '\n';
    ready << "mode=" << RuntimeExecutorBridgeValidatedAdapterMode << '\n';
    ready << "process_id=" << environment.processId << '\n';
    ready << "executable=" << environment.executablePath << '\n';
    for (const std::string& line : residentLines)
      ready << line << '\n';
    for (const std::string& line : extraLines)
      ready << line << '\n';
  }

  void writeReadyFile(
    const RuntimeEnvironment& environment,
    std::uint64_t heartbeat,
    const std::vector<std::string>& extraLines = {})
  {
    writeReadyFileWithResidentLines(
      environment,
      makeRuntimeResidentAdapterReadyLines(environment, heartbeat),
      extraLines);
  }

  void writeQueueFile(
    const std::filesystem::path& path,
    const RuntimeResidentQueueHeader& header)
  {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(&header), sizeof(header));
  }

  std::vector<std::string> residentStateProofLines(
    const RuntimeEnvironment& environment,
    std::uint64_t heartbeat,
    std::uint64_t activeUnitCount = 4,
    const std::string& activeMode = "match",
    bool includeLiveReadAddress = true,
    bool includeActiveUnitEvidence = true,
    std::uintptr_t activeUnitAddress = 0)
  {
    const std::vector<RuntimeResidentGameStateSample> samples = {
      { 400, 10000 },
      { 401, 10016 },
      { 402, 10032 }
    };
    std::vector<std::string> lines =
      makeRuntimeResidentReadGameStateProofReadyLines(environment, heartbeat, samples);
    if (includeLiveReadAddress)
    {
      lines.push_back(
        "proof.read_game_state.address="
        + std::to_string(reinterpret_cast<std::uintptr_t>(&residentFrameCounter)));
    }
    const std::vector<std::string> activeLines =
      makeRuntimeResidentActiveMatchProofReadyLines(
        environment,
        heartbeat,
        activeUnitCount,
        activeMode);
    lines.insert(lines.end(), activeLines.begin(), activeLines.end());
    if (includeActiveUnitEvidence)
    {
      const std::uintptr_t unitAddress =
        activeUnitAddress == 0
          ? reinterpret_cast<std::uintptr_t>(activeUnitEvidence.data())
          : activeUnitAddress;
      lines.push_back("proof.read_units=passed");
      lines.push_back("proof.read_units.address=" + std::to_string(unitAddress));
      lines.push_back("proof.read_units.record_size=64");
      lines.push_back("proof.read_units.active_records=" + std::to_string(activeUnitCount));
      lines.push_back("proof.active_match_state.evidence=active-unit-node-snapshot");
      lines.push_back("proof.active_match_state.active_records=" + std::to_string(activeUnitCount));
      lines.push_back(
        "proof.active_match_state.unit_node_address="
        + std::to_string(unitAddress));
      lines.push_back("proof.active_match_state.unit_node_record_size=64");
    }
    return lines;
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
  const std::filesystem::path bridgePath =
    makeBridgePath("starcraft-api-runtime-resident-bridge-test");
  RuntimeEnvironment environment = makeEnvironment(selfExecutable, bridgePath);

  writeReadyFile(environment, 7);
  RuntimeResidentBridgeValidationResult resident =
    validateRuntimeResidentBridgeReadyFile(
      environment,
      bridgePath / RuntimeExecutorBridgeReadyFile);
  assert(resident.valid);
  assert(resident.present);
  assert(resident.active);
  assert(resident.abi == RuntimeResidentAdapterAbi);
  assert(resident.heartbeat == 7);
  assert(resident.processId == environment.processId);

  RuntimeManifestLoadResult manifest = loadRuntimeManifestFile(fixturePath("remastered-complete.manifest"));
  assert(manifest.loaded);
  RuntimeExecutorPreflightResult preflight =
    preflightRuntimeExecutor(environment, manifest.manifest.contract);
  assert(preflight.contractValid);
  assert(preflight.executorAvailable);
  assert(!preflight.errors.empty());
  assert(preflight.missingBehaviorProofs.size() == requiredRuntimeExecutorBehaviorProofs().size());

  writeReadyFile(environment, 9, residentStateProofLines(environment, 9));
  resident = validateRuntimeResidentBridgeReadyFile(
    environment,
    bridgePath / RuntimeExecutorBridgeReadyFile);
  RuntimeResidentStateProofValidationResult stateProof =
    validateRuntimeResidentStateProofs(
      environment,
      bridgePath / RuntimeExecutorBridgeReadyFile,
      resident);
  assert(stateProof.readGameStateValid);
  assert(stateProof.activeMatchValid);
  assert(stateProof.samples.size() == 3);
  assert(stateProof.activeUnitCount == 4);
  preflight = preflightRuntimeExecutor(environment, manifest.manifest.contract);
  assert(preflight.executorAvailable);
  assert(!hasMissingProof(preflight, "proof.read_game_state=passed"));
  assert(!hasMissingProof(preflight, "proof.active_match_state=passed"));

  std::vector<std::string> duplicateResidentProofLines =
    residentStateProofLines(environment, 9);
  duplicateResidentProofLines.push_back("resident.proof.active_match.source=mock");
  writeReadyFile(environment, 9, duplicateResidentProofLines);
  resident = validateRuntimeResidentBridgeReadyFile(
    environment,
    bridgePath / RuntimeExecutorBridgeReadyFile);
  stateProof = validateRuntimeResidentStateProofs(
    environment,
    bridgePath / RuntimeExecutorBridgeReadyFile,
    resident);
  assert(!stateProof.readGameStateValid);
  assert(!stateProof.activeMatchValid);

  std::vector<std::string> duplicateExactStateProofLines =
    residentStateProofLines(environment, 9);
  duplicateExactStateProofLines.push_back("proof.active_match_state=failed");
  writeReadyFile(environment, 9, duplicateExactStateProofLines);
  resident = validateRuntimeResidentBridgeReadyFile(
    environment,
    bridgePath / RuntimeExecutorBridgeReadyFile);
  stateProof = validateRuntimeResidentStateProofs(
    environment,
    bridgePath / RuntimeExecutorBridgeReadyFile,
    resident);
  assert(!stateProof.readGameStateValid);
  assert(!stateProof.activeMatchValid);

  std::vector<std::string> replayLines =
    residentStateProofLines(environment, 10, 4, "replay");
  writeReadyFile(environment, 10, replayLines);
  resident = validateRuntimeResidentBridgeReadyFile(
    environment,
    bridgePath / RuntimeExecutorBridgeReadyFile);
  stateProof = validateRuntimeResidentStateProofs(
    environment,
    bridgePath / RuntimeExecutorBridgeReadyFile,
    resident);
  assert(stateProof.readGameStateValid);
  assert(!stateProof.activeMatchValid);
  preflight = preflightRuntimeExecutor(environment, manifest.manifest.contract);
  assert(preflight.executorAvailable);
  assert(hasMissingProof(preflight, "proof.active_match_state=passed"));

  std::vector<std::string> adapterProofLines =
    residentStateProofLines(environment, 10, 4, "match");
  for (std::string& line : adapterProofLines)
  {
    if (line == "resident.proof.active_match.source=resident")
      line = "resident.proof.active_match.source=adapter-proof";
    else if (line == "resident.proof.active_match.evidence=resident-frame-unit-activity")
      line = "resident.proof.active_match.evidence=adapter-live-unit-activity";
  }
  writeReadyFile(environment, 10, adapterProofLines);
  resident = validateRuntimeResidentBridgeReadyFile(
    environment,
    bridgePath / RuntimeExecutorBridgeReadyFile);
  stateProof = validateRuntimeResidentStateProofs(
    environment,
    bridgePath / RuntimeExecutorBridgeReadyFile,
    resident);
  assert(stateProof.readGameStateValid);
  assert(!stateProof.activeMatchValid);
  preflight = preflightRuntimeExecutor(environment, manifest.manifest.contract);
  assert(preflight.executorAvailable);
  assert(hasMissingProof(preflight, "proof.active_match_state=passed"));

  std::vector<std::string> adapterProofRaceLines =
    residentStateProofLines(environment, 10, 4, "match");
  for (std::string& line : adapterProofRaceLines)
  {
    if (line == "resident.proof.active_match.heartbeat=10")
      line = "resident.proof.active_match.heartbeat=11";
  }
  writeReadyFile(environment, 10, adapterProofRaceLines);
  resident = validateRuntimeResidentBridgeReadyFile(
    environment,
    bridgePath / RuntimeExecutorBridgeReadyFile);
  stateProof = validateRuntimeResidentStateProofs(
    environment,
    bridgePath / RuntimeExecutorBridgeReadyFile,
    resident);
  assert(stateProof.readGameStateValid);
  assert(stateProof.activeMatchValid);

  writeReadyFile(environment, 10, { "proof.active_match_state=passed" });
  resident = validateRuntimeResidentBridgeReadyFile(
    environment,
    bridgePath / RuntimeExecutorBridgeReadyFile);
  stateProof = validateRuntimeResidentStateProofs(
    environment,
    bridgePath / RuntimeExecutorBridgeReadyFile,
    resident);
  assert(!stateProof.activeMatchValid);
  preflight = preflightRuntimeExecutor(environment, manifest.manifest.contract);
  assert(hasMissingProof(preflight, "proof.active_match_state=passed"));
  assert(!preflight.errors.empty());

  writeReadyFile(environment, 11, residentStateProofLines(environment, 10));
  resident = validateRuntimeResidentBridgeReadyFile(
    environment,
    bridgePath / RuntimeExecutorBridgeReadyFile);
  stateProof = validateRuntimeResidentStateProofs(
    environment,
    bridgePath / RuntimeExecutorBridgeReadyFile,
    resident);
  assert(!stateProof.readGameStateValid);
  assert(!stateProof.activeMatchValid);

  writeReadyFile(environment, 12, residentStateProofLines(environment, 12, 4, "menu"));
  resident = validateRuntimeResidentBridgeReadyFile(
    environment,
    bridgePath / RuntimeExecutorBridgeReadyFile);
  stateProof = validateRuntimeResidentStateProofs(
    environment,
    bridgePath / RuntimeExecutorBridgeReadyFile,
    resident);
  assert(stateProof.readGameStateValid);
  assert(!stateProof.activeMatchValid);

  writeReadyFile(
    environment,
    13,
    residentStateProofLines(environment, 13, 4, "match", false, true));
  resident = validateRuntimeResidentBridgeReadyFile(
    environment,
    bridgePath / RuntimeExecutorBridgeReadyFile);
  stateProof = validateRuntimeResidentStateProofs(
    environment,
    bridgePath / RuntimeExecutorBridgeReadyFile,
    resident);
  assert(!stateProof.readGameStateValid);
  assert(!stateProof.activeMatchValid);

  writeReadyFile(
    environment,
    14,
    residentStateProofLines(environment, 14, 4, "match", true, false));
  resident = validateRuntimeResidentBridgeReadyFile(
    environment,
    bridgePath / RuntimeExecutorBridgeReadyFile);
  stateProof = validateRuntimeResidentStateProofs(
    environment,
    bridgePath / RuntimeExecutorBridgeReadyFile,
    resident);
  assert(stateProof.readGameStateValid);
  assert(!stateProof.activeMatchValid);

  writeReadyFile(
    environment,
    15,
    residentStateProofLines(environment, 15, 4, "match", true, true, 1));
  resident = validateRuntimeResidentBridgeReadyFile(
    environment,
    bridgePath / RuntimeExecutorBridgeReadyFile);
  stateProof = validateRuntimeResidentStateProofs(
    environment,
    bridgePath / RuntimeExecutorBridgeReadyFile,
    resident);
  assert(stateProof.readGameStateValid);
  assert(!stateProof.activeMatchValid);

  writeReadyFile(
    environment,
    16,
    residentStateProofLines(
      environment,
      16,
      4,
      "match",
      true,
      true,
      reinterpret_cast<std::uintptr_t>(&residentFrameCounter)));
  resident = validateRuntimeResidentBridgeReadyFile(
    environment,
    bridgePath / RuntimeExecutorBridgeReadyFile);
  stateProof = validateRuntimeResidentStateProofs(
    environment,
    bridgePath / RuntimeExecutorBridgeReadyFile,
    resident);
  assert(stateProof.readGameStateValid);
  assert(!stateProof.activeMatchValid);

  std::vector<std::string> residentPreservedActiveLines =
    residentStateProofLines(
      environment,
      17,
      4,
      "match",
      true,
      true,
      1);
  residentPreservedActiveLines.push_back(
    "resident.proof.active_match.validation=resident-preserved-active-unit-memory-v1");
  residentPreservedActiveLines.push_back(
    "resident.proof.active_match.address_read=resident-self");
  writeReadyFile(environment, 17, residentPreservedActiveLines);
  resident = validateRuntimeResidentBridgeReadyFile(
    environment,
    bridgePath / RuntimeExecutorBridgeReadyFile);
  stateProof = validateRuntimeResidentStateProofs(
    environment,
    bridgePath / RuntimeExecutorBridgeReadyFile,
    resident);
  assert(stateProof.readGameStateValid);
  assert(stateProof.activeMatchValid);

  writeReadyFile(environment, 7, { "proof.issue_commands=passed" });
  preflight = preflightRuntimeExecutor(environment, manifest.manifest.contract);
  assert(preflight.executorAvailable);
  assert(!preflight.errors.empty());
  assert(!preflight.missingBehaviorProofs.empty());

  writeReadyFile(
    environment,
    7,
    {
      RuntimeExecutorBridgeActiveCommandReceiverLine,
      RuntimeExecutorBridgeRuntimeCommandQueueSinkLine,
      "proof.issue_commands=passed",
      "contract.binding.BW::BWDATA::sgdwBytesInCmdQueue=command-queue|unit-test:bytes-in-command-queue",
      "contract.binding.BW::BWDATA::TurnBuffer=command-queue|unit-test:turn-buffer"
    });
  preflight = preflightRuntimeExecutor(environment, manifest.manifest.contract);
  assert(preflight.executorAvailable);
  assert(!preflight.errors.empty());
  assert(!preflight.missingBehaviorProofs.empty());

  writeReadyFile(
    environment,
    7,
    {
      RuntimeExecutorBridgeActiveCommandReceiverLine,
      RuntimeExecutorBridgeRuntimeCommandQueueSinkLine,
      "contract.binding.BW::BWDATA::sgdwBytesInCmdQueue=command-queue|proof.issue_commands=passed:bytes-in-command-queue",
      "contract.binding.BW::BWDATA::TurnBuffer=command-queue|proof.issue_commands=passed:turn-buffer",
      "resident.proof.issue_commands_ingress=passed",
      "resident.proof.issue_commands_ingress.consumed_records=2",
      "resident.proof.issue_commands_ingress.parsed_commands=2",
      "resident.proof.draw_overlays_ingress=passed",
      "resident.proof.draw_overlays_ingress.accepted_primitives=1",
      "resident.proof.draw_overlays_ingress.renderer_bound=false",
      "proof.issue_commands=passed",
      "proof.draw_overlays=passed"
    });
  preflight = preflightRuntimeExecutor(environment, manifest.manifest.contract);
  assert(preflight.executorAvailable);
  assert(hasMissingProof(preflight, "proof.issue_commands=passed"));
  assert(hasMissingProof(preflight, "proof.draw_overlays=passed"));

  writeReadyFile(environment, 1);
  resident = validateRuntimeResidentBridgeReadyFile(
    environment,
    bridgePath / RuntimeExecutorBridgeReadyFile,
    RuntimeResidentBridgeValidationOptions{ 2, 30000 });
  assert(!resident.valid);
  assert(!resident.errors.empty());

  writeReadyFileWithResidentLines(
    environment,
    {
      "resident.adapter=active",
      "resident.adapter.abi=starcraft-api-resident-adapter-v99",
      "resident.adapter.process_id=" + std::to_string(environment.processId),
      "resident.adapter.heartbeat=5"
    });
  resident = validateRuntimeResidentBridgeReadyFile(
    environment,
    bridgePath / RuntimeExecutorBridgeReadyFile);
  assert(!resident.valid);
  preflight = preflightRuntimeExecutor(environment, manifest.manifest.contract);
  assert(!preflight.executorAvailable);
  assert(!preflight.errors.empty());
  RuntimeCommandRequest gameAction;
  gameAction.kind = RuntimeCommandKind::GameAction;
  gameAction.name = "pauseGame";
  RuntimeExecutorSubmitResult submitWithInvalidResident =
    submitRuntimeCommands(environment, { gameAction });
  assert(!submitWithInvalidResident.submitted);
  assert(!submitWithInvalidResident.errors.empty());

  writeReadyFileWithResidentLines(
    environment,
    {
      "resident.adapter=inactive",
      std::string("resident.adapter.abi=") + RuntimeResidentAdapterAbi,
      "resident.adapter.process_id=" + std::to_string(environment.processId),
      "resident.adapter.heartbeat=5"
    });
  resident = validateRuntimeResidentBridgeReadyFile(
    environment,
    bridgePath / RuntimeExecutorBridgeReadyFile);
  assert(resident.present);
  assert(!resident.valid);
  assert(!resident.active);
  preflight = preflightRuntimeExecutor(environment, manifest.manifest.contract);
  assert(!preflight.executorAvailable);
  assert(!preflight.errors.empty());

  writeReadyFileWithResidentLines(
    environment,
    {
      "resident.adapter=active",
      std::string("resident.adapter.abi=") + RuntimeResidentAdapterAbi,
      "resident.adapter.process_id=123456789",
      "resident.adapter.heartbeat=5"
    });
  resident = validateRuntimeResidentBridgeReadyFile(
    environment,
    bridgePath / RuntimeExecutorBridgeReadyFile);
  assert(!resident.valid);

  RuntimeEnvironment discoverOnlyEnvironment = environment;
  discoverOnlyEnvironment.processId = 0;
  writeReadyFileWithResidentLines(
    discoverOnlyEnvironment,
    {
      "resident.adapter=active",
      std::string("resident.adapter.abi=") + RuntimeResidentAdapterAbi,
      "resident.adapter.process_id=123456789",
      "resident.adapter.heartbeat=5"
    });
  resident = validateRuntimeResidentBridgeReadyFile(
    discoverOnlyEnvironment,
    bridgePath / RuntimeExecutorBridgeReadyFile);
  assert(!resident.valid);

  writeReadyFile(environment, 5, { "resident.adapter.heartbeat=6" });
  resident = validateRuntimeResidentBridgeReadyFile(
    environment,
    bridgePath / RuntimeExecutorBridgeReadyFile);
  assert(!resident.valid);

  writeReadyFile(environment, 5, { "product=starcraft-remastered" });
  preflight = preflightRuntimeExecutor(environment, manifest.manifest.contract);
  assert(!preflight.executorAvailable);
  assert(!preflight.errors.empty());

  writeReadyFile(environment, 5);
  std::filesystem::last_write_time(
    bridgePath / RuntimeExecutorBridgeReadyFile,
    std::filesystem::file_time_type::clock::now() - std::chrono::seconds(60));
  resident = validateRuntimeResidentBridgeReadyFile(
    environment,
    bridgePath / RuntimeExecutorBridgeReadyFile);
  assert(!resident.valid);

  RuntimeEnvironment wrongExecutable = environment;
  wrongExecutable.executablePath = fixturePath("missing-starcraft");
  resident = validateRuntimeResidentBridgeReadyFile(
    wrongExecutable,
    bridgePath / RuntimeExecutorBridgeReadyFile);
  assert(!resident.valid);

  RuntimeResidentQueueHeader commandQueue =
    makeRuntimeResidentQueueHeader(RuntimeResidentQueueKind::Command, 32, 8, 3);
  RuntimeResidentQueueValidationResult queue =
    validateRuntimeResidentQueueHeader(commandQueue, RuntimeResidentQueueKind::Command);
  assert(queue.valid);

  commandQueue.magic = 0;
  assert(!validateRuntimeResidentQueueHeader(
    commandQueue,
    RuntimeResidentQueueKind::Command).valid);
  commandQueue.magic = RuntimeResidentQueueMagic;
  commandQueue.kind = static_cast<std::uint16_t>(RuntimeResidentQueueKind::Overlay);
  assert(!validateRuntimeResidentQueueHeader(
    commandQueue,
    RuntimeResidentQueueKind::Command).valid);
  commandQueue.kind = static_cast<std::uint16_t>(RuntimeResidentQueueKind::Command);
  commandQueue.recordBytes = 0;
  assert(!validateRuntimeResidentQueueHeader(
    commandQueue,
    RuntimeResidentQueueKind::Command).valid);
  commandQueue.recordBytes = 32;
  commandQueue.heartbeat = 0;
  assert(!validateRuntimeResidentQueueHeader(
    commandQueue,
    RuntimeResidentQueueKind::Command).valid);

  commandQueue = makeRuntimeResidentQueueHeader(RuntimeResidentQueueKind::Command, 32, 8, 3);
  commandQueue.writeSequence = 2;
  RuntimeResidentRecordHeader record;
  record.kind = static_cast<std::uint16_t>(RuntimeResidentQueueKind::Command);
  record.payloadBytes = 8;
  record.sequence = 1;
  assert(validateRuntimeResidentRecordHeader(
    commandQueue,
    record,
    RuntimeResidentQueueKind::Command).valid);
  record.kind = static_cast<std::uint16_t>(RuntimeResidentQueueKind::Overlay);
  assert(!validateRuntimeResidentRecordHeader(
    commandQueue,
    record,
    RuntimeResidentQueueKind::Command).valid);
  record.kind = static_cast<std::uint16_t>(RuntimeResidentQueueKind::Command);
  record.payloadBytes = 64;
  assert(!validateRuntimeResidentRecordHeader(
    commandQueue,
    record,
    RuntimeResidentQueueKind::Command).valid);
  record.payloadBytes = 8;
  record.headerBytes = 40;
  assert(!validateRuntimeResidentRecordHeader(
    commandQueue,
    record,
    RuntimeResidentQueueKind::Command).valid);
  record.headerBytes = sizeof(RuntimeResidentRecordHeader);
  record.payloadBytes = 24;
  assert(!validateRuntimeResidentRecordHeader(
    commandQueue,
    record,
    RuntimeResidentQueueKind::Command).valid);
  record.payloadBytes = 8;
  record.sequence = 3;
  assert(!validateRuntimeResidentRecordHeader(
    commandQueue,
    record,
    RuntimeResidentQueueKind::Command).valid);

  const std::filesystem::path proofQueuePath = bridgePath / RuntimeResidentProofQueueFile;
  RuntimeResidentQueueHeader proofQueue =
    makeRuntimeResidentQueueHeader(
      RuntimeResidentQueueKind::Proof,
      sizeof(RuntimeResidentRecordHeader),
      4,
      21);
  proofQueue.writeSequence = 21;
  proofQueue.readSequence = 17;
  writeQueueFile(proofQueuePath, proofQueue);
  assert(validateRuntimeResidentQueueFile(
    proofQueuePath,
    RuntimeResidentQueueKind::Proof).valid);

  const std::filesystem::path commandQueuePath = bridgePath / RuntimeResidentCommandQueueFile;
  RuntimeResidentQueueHeader desiredCommandQueue =
    makeRuntimeResidentQueueHeader(
      RuntimeResidentQueueKind::Command,
      sizeof(RuntimeResidentRecordHeader) + 64,
      2,
      30);
  RuntimeResidentQueueHeader actualCommandQueue;
  RuntimeResidentQueueValidationResult ensuredCommandQueue =
    ensureRuntimeResidentQueueFile(
      commandQueuePath,
      desiredCommandQueue,
      actualCommandQueue);
  assert(ensuredCommandQueue.valid);
  assert(actualCommandQueue.writeSequence == 0);
  assert(actualCommandQueue.readSequence == 0);
  const std::vector<unsigned char> firstPayload = { 'f', 'i', 'r', 's', 't' };
  RuntimeResidentQueueAppendResult firstAppend =
    appendRuntimeResidentQueueRecord(
      commandQueuePath,
      RuntimeResidentQueueKind::Command,
      firstPayload);
  assert(firstAppend.appended);
  assert(firstAppend.sequence == 0);
  RuntimeResidentQueueAppendResult secondAppend =
    appendRuntimeResidentQueueRecord(
      commandQueuePath,
      RuntimeResidentQueueKind::Command,
      { 's', 'e', 'c', 'o', 'n', 'd' });
  assert(secondAppend.appended);
  assert(secondAppend.sequence == 1);
  RuntimeResidentQueueAppendResult fullAppend =
    appendRuntimeResidentQueueRecord(
      commandQueuePath,
      RuntimeResidentQueueKind::Command,
      { 'f', 'u', 'l', 'l' });
  assert(!fullAppend.appended);
  RuntimeResidentQueueReadResult commandRecords =
    readRuntimeResidentQueueRecords(
      commandQueuePath,
      RuntimeResidentQueueKind::Command,
      8);
  assert(commandRecords.read);
  assert(commandRecords.records.size() == 2);
  assert(commandRecords.records.front().header.sequence == 0);
  assert(commandRecords.records.front().payload == firstPayload);
  RuntimeResidentQueueAcknowledgeResult acknowledgedCommandQueue =
    acknowledgeRuntimeResidentQueueRecords(
      commandQueuePath,
      RuntimeResidentQueueKind::Command,
      2);
  assert(acknowledgedCommandQueue.acknowledged);
  assert(acknowledgedCommandQueue.header.readSequence == 2);
  RuntimeResidentQueueHeader preservedCommandQueue;
  desiredCommandQueue.heartbeat = 31;
  ensuredCommandQueue =
    ensureRuntimeResidentQueueFile(
      commandQueuePath,
      desiredCommandQueue,
      preservedCommandQueue);
  assert(ensuredCommandQueue.valid);
  assert(preservedCommandQueue.readSequence == 2);
  assert(preservedCommandQueue.writeSequence == 2);

  proofQueue.readSequence = 16;
  assert(!validateRuntimeResidentQueueHeader(
    proofQueue,
    RuntimeResidentQueueKind::Proof).valid);
  proofQueue.readSequence = 17;
  writeReadyFile(
    environment,
    21,
    makeRuntimeResidentQueueReadyLines(
      RuntimeResidentQueueKind::Proof,
      proofQueuePath,
      proofQueue));
  resident = validateRuntimeResidentBridgeReadyFile(
    environment,
    bridgePath / RuntimeExecutorBridgeReadyFile);
  assert(resident.valid);

  proofQueue.heartbeat = 25;
  writeQueueFile(proofQueuePath, proofQueue);
  resident = validateRuntimeResidentBridgeReadyFile(
    environment,
    bridgePath / RuntimeExecutorBridgeReadyFile);
  assert(resident.valid);

  proofQueue.heartbeat = 20;
  writeQueueFile(proofQueuePath, proofQueue);
  resident = validateRuntimeResidentBridgeReadyFile(
    environment,
    bridgePath / RuntimeExecutorBridgeReadyFile);
  assert(!resident.valid);

  proofQueue.heartbeat = 22;
  proofQueue.magic = 0;
  writeQueueFile(proofQueuePath, proofQueue);
  writeReadyFile(
    environment,
    22,
    makeRuntimeResidentQueueReadyLines(
      RuntimeResidentQueueKind::Proof,
      proofQueuePath,
      proofQueue));
  resident = validateRuntimeResidentBridgeReadyFile(
    environment,
    bridgePath / RuntimeExecutorBridgeReadyFile);
  assert(!resident.valid);

  RuntimeResidentLoadRequest loadRequest;
  loadRequest.environment = environment;
  loadRequest.adapterPath = bridgePath.string();
  RuntimeResidentLoadPlan loadPlan = planRuntimeResidentAdapterLoad(loadRequest);
  assert(loadPlan.valid == (environment.platform == Platform::MacOS));
  assert(loadPlan.loaderMode == "macos-resident-dylib");
  loadRequest.environment.processId = 0;
  assert(!planRuntimeResidentAdapterLoad(loadRequest).valid);

  std::filesystem::remove_all(bridgePath);
  return 0;
}
