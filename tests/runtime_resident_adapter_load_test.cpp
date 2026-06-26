#include <BWAPI/Runtime/RuntimeBackend.h>
#include <BWAPI/Runtime/RuntimeExecutor.h>
#include <BWAPI/Runtime/RuntimeProcess.h>
#include <BWAPI/Runtime/RuntimeResidentBridge.h>

#include <cassert>
#include <chrono>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>

using namespace BWAPI::Runtime;

namespace
{
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
}

int main(int argc, char** argv)
{
  assert(argc > 0);

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
