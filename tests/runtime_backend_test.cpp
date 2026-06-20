#include <BWAPI/Runtime/RuntimeBackend.h>
#include <BWAPI/Runtime/RuntimeContract.h>
#include <BWAPI/Runtime/RuntimeProcessMemory.h>

#include <cassert>
#include <memory>
#include <string>

using namespace BWAPI::Runtime;

int main()
{
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
  assert(remasteredProbe.implementedApiSurfaceMethods == 0);
  assert(remasteredProbe.implementedCommandSurfaceEntries == 0);
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
