#include <BWAPI/Runtime/RuntimeExecutor.h>
#include <BWAPI/Runtime/RuntimeManifest.h>
#include <BWAPI/Runtime/RuntimeProcessMemory.h>
#include <BWAPI/Runtime/RuntimeResidentBridge.h>

#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace BWAPI::Runtime;

namespace
{
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

  std::filesystem::path makeBridgePath(
    const std::string& name = "starcraft-api-runtime-executor-test")
  {
    std::filesystem::path path = std::filesystem::temp_directory_path() / name;
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
    ready << "mode=" << RuntimeExecutorBridgeValidatedAdapterMode << '\n';
    writeRuntimeIdentity(ready, processId, executable);
    ready << "contract.binding.BW::BWDATA::sgdwBytesInCmdQueue=command-queue|" << evidenceProof << ':'
          << bytesInQueueAddress << '\n';
    ready << "contract.binding.BW::BWDATA::TurnBuffer=command-queue|" << evidenceProof << ':'
          << turnBufferAddress << '\n';
    for (const RuntimeExecutorBehaviorProof& proof : requiredRuntimeExecutorBehaviorProofs())
      ready << proof.readyFileLine << '\n';
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
    ready << "mode=" << RuntimeExecutorBridgeValidatedAdapterMode << '\n';
    writeRuntimeIdentity(ready, processId + 100000, executable);
    writeRuntimeCommandQueueSink(ready);
    for (const RuntimeExecutorBehaviorProof& proof : requiredRuntimeExecutorBehaviorProofs())
      ready << proof.readyFileLine << '\n';
  }
}

int main(int argc, char** argv)
{
  assert(argc > 0);
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
    ready << "proof.read_units=passed\n";
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

  writeValidatedAdapterReadyFile(bridgePath, bridgeEnvironment.processId, bridgeEnvironment.executablePath);
  {
    std::ofstream ready(bridgePath / RuntimeExecutorBridgeReadyFile, std::ios::app);
    for (const std::string& line : makeRuntimeResidentAdapterReadyLines(bridgeEnvironment, 20))
      ready << line << '\n';
  }
  RuntimeExecutorPreflightResult freshResidentPreflight =
    preflightRuntimeExecutor(bridgeEnvironment, complete.manifest.contract);
  assert(freshResidentPreflight.executorAvailable);
  assert(freshResidentPreflight.errors.empty());

  writeValidatedAdapterReadyFile(bridgePath, bridgeEnvironment.processId, bridgeEnvironment.executablePath);
  {
    std::ofstream ready(bridgePath / RuntimeExecutorBridgeReadyFile, std::ios::app);
    for (const std::string& line : makeRuntimeResidentAdapterReadyLines(bridgeEnvironment, 20))
      ready << line << '\n';
  }
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
  assert(partialProofPreflight.missingBehaviorProofs.size() == 1);
  assert(partialProofPreflight.missingBehaviorProofs.front() == "proof.multiplayer_sync=passed");
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
  assert(staleProcessPreflight.executorBridgeMode == RuntimeExecutorBridgeValidatedAdapterMode);
  assert(staleProcessPreflight.missingBehaviorProofs.empty());
  assert(!staleProcessPreflight.errors.empty());

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
  assert(bridgePreflight.missingBehaviorProofs.empty());
  assert(bridgePreflight.errors.empty());

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
  assert(submitted.submitted);
  assert(submitted.submittedCommands == 2);
  assert(submitted.errors.empty());
  assert(std::filesystem::exists(bridgePath / RuntimeExecutorBridgeCommandFile));

  std::ifstream commandLog(bridgePath / RuntimeExecutorBridgeCommandFile);
  std::stringstream commandLogContent;
  commandLogContent << commandLog.rdbuf();
  commandLog.close();
  assert(commandLogContent.str().find("unit-command|Move|5|10,20") != std::string::npos);
  assert(commandLogContent.str().find("game-action|pauseGame|0|") != std::string::npos);

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
  assert(directBridgePreflight.missingBehaviorProofs.empty());
  RuntimeExecutorSubmitResult directSubmitted =
    submitRuntimeCommands(directBridgeEnvironment, { gameAction });
  assert(directSubmitted.submitted);
  assert(directSubmitted.submittedCommands == 1);
  assert(directSubmitted.errors.empty());
  assert(directBytesInQueue == 1);
  assert(directTurnBuffer[0] == 0x10);
  assert(std::filesystem::exists(directBridgePath / "commands.applied.tsv"));
  assert(!std::filesystem::exists(directBridgePath / RuntimeExecutorBridgeCommandFile));

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

  return 0;
}
