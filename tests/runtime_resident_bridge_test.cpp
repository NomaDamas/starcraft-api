#include <BWAPI/Runtime/RuntimeExecutor.h>
#include <BWAPI/Runtime/RuntimeManifest.h>
#include <BWAPI/Runtime/RuntimeProcessMemory.h>
#include <BWAPI/Runtime/RuntimeResidentBridge.h>
#include <BWAPI/Runtime/RuntimeResidentLoader.h>

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace BWAPI::Runtime;

namespace
{
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
}

int main(int argc, char** argv)
{
  assert(argc > 0);
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
  record.sequence = 3;
  assert(!validateRuntimeResidentRecordHeader(
    commandQueue,
    record,
    RuntimeResidentQueueKind::Command).valid);

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
