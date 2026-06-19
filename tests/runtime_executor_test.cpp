#include <BWAPI/Runtime/RuntimeExecutor.h>
#include <BWAPI/Runtime/RuntimeManifest.h>
#include <BWAPI/Runtime/RuntimeProcessMemory.h>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>

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

  std::filesystem::path makeBridgePath()
  {
    std::filesystem::path path = std::filesystem::temp_directory_path() / "starcraft-api-runtime-executor-test";
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
  }

  void writeReadyFile(const std::filesystem::path& bridgePath)
  {
    std::ofstream ready(bridgePath / RuntimeExecutorBridgeReadyFile);
    ready << "protocol=" << RuntimeExecutorBridgeProtocol << '\n';
    ready << "product=starcraft-remastered\n";
    ready << "version=test-build\n";
  }
}

int main()
{
  RuntimeManifestLoadResult complete = loadRuntimeManifestFile(fixturePath("remastered-complete.manifest"));
  assert(complete.loaded);

  RuntimeExecutorPreflightResult readyPrerequisites =
    preflightRuntimeExecutor(remasteredEnvironment(fixturePath("remastered-complete.manifest")), complete.manifest.contract);
  assert(readyPrerequisites.contractValid);
  assert(readyPrerequisites.processIdentified);
  assert(readyPrerequisites.targetLocated);
  assert(!readyPrerequisites.executorAvailable);
  assert(!readyPrerequisites.warnings.empty());

  RuntimeExecutorPreflightResult missingTarget =
    preflightRuntimeExecutor(remasteredEnvironment(fixturePath("missing-starcraft")), complete.manifest.contract);
  assert(missingTarget.contractValid);
  assert(missingTarget.processIdentified);
  assert(!missingTarget.targetLocated);
  assert(!missingTarget.executorAvailable);
  assert(!missingTarget.errors.empty());

  RuntimeContract unresolved = makeRemasteredParityContract("test-build");
  RuntimeExecutorPreflightResult invalidContract =
    preflightRuntimeExecutor(remasteredEnvironment(fixturePath("remastered-complete.manifest")), unresolved);
  assert(!invalidContract.contractValid);
  assert(invalidContract.processIdentified);
  assert(invalidContract.targetLocated);
  assert(!invalidContract.executorAvailable);
  assert(!invalidContract.errors.empty());

  std::filesystem::path bridgePath = makeBridgePath();
  writeReadyFile(bridgePath);
  RuntimeEnvironment bridgeEnvironment = remasteredEnvironment(fixturePath("remastered-complete.manifest"));
  bridgeEnvironment.executorBridgePath = bridgePath.string();
  RuntimeExecutorPreflightResult bridgePreflight =
    preflightRuntimeExecutor(bridgeEnvironment, complete.manifest.contract);
  assert(bridgePreflight.contractValid);
  assert(bridgePreflight.processIdentified);
  assert(bridgePreflight.targetLocated);
  assert(bridgePreflight.executorAvailable);
  assert(bridgePreflight.executorName == "filesystem-bridge");
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

  RuntimeEnvironment noManifestEnvironment = bridgeEnvironment;
  noManifestEnvironment.manifestPath.clear();
  RuntimeExecutorSubmitResult rejectedWithoutManifest =
    submitRuntimeCommands(noManifestEnvironment, { gameAction });
  assert(!rejectedWithoutManifest.submitted);
  assert(rejectedWithoutManifest.reason.find("runtime manifest is required") != std::string::npos);

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
  assert(rejectedWithBootstrapManifest.reason.find("runtime manifest failed to load") != std::string::npos);

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

  return 0;
}
