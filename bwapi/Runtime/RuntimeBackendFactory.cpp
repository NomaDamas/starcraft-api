#include <BWAPI/Runtime/RuntimeBackend.h>

#include "UnsupportedRuntimeBackend.h"

#include <memory>
#include <string>

namespace BWAPI::Runtime
{
  std::unique_ptr<RuntimeBackend> createRuntimeBackend(const RuntimeEnvironment& environment)
  {
    if (environment.product == Product::StarCraftBroodWar1161)
    {
      if (environment.platform == Platform::Windows)
      {
        return std::make_unique<UnsupportedRuntimeBackend>(
          environment,
          "legacy-bwapi-1.16.1-runtime",
          "The legacy BWAPI runtime is still implemented by the existing Windows DLL injection backend; "
          "it has not yet been moved behind the portable RuntimeBackend interface.");
      }

      return std::make_unique<UnsupportedRuntimeBackend>(
        environment,
        "legacy-bwapi-1.16.1-runtime",
        "StarCraft Brood War 1.16.1 support depends on Windows-only process injection, Storm.dll imports, "
        "and fixed 1.16.1 memory offsets.");
    }

    if (environment.product == Product::StarCraftRemastered)
    {
      return std::make_unique<UnsupportedRuntimeBackend>(
        environment,
        "starcraft-remastered-runtime",
        "StarCraft Remastered requires a versioned runtime adapter with an authorized symbol map, "
        "validated game-state structures, command queue binding, and multiplayer synchronization tests "
        "before BWAPI parity can be claimed.");
    }

    return std::make_unique<UnsupportedRuntimeBackend>(
      environment,
      "unknown-starcraft-runtime",
      "No StarCraft runtime product/version was selected.");
  }
}
