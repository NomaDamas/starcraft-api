#include <BWAPI/Runtime/RuntimeBackend.h>

#include "Legacy1161RuntimeBackend.h"
#include "RemasteredRuntimeBackend.h"
#include "UnsupportedRuntimeBackend.h"

#include <memory>

namespace BWAPI::Runtime
{
  std::unique_ptr<RuntimeBackend> createRuntimeBackend(const RuntimeEnvironment& environment)
  {
    if (environment.product == Product::StarCraftBroodWar1161)
      return std::make_unique<Legacy1161RuntimeBackend>(environment);

    if (environment.product == Product::StarCraftRemastered)
      return std::make_unique<RemasteredRuntimeBackend>(environment);

    return std::make_unique<UnsupportedRuntimeBackend>(
      environment,
      "unknown-starcraft-runtime",
      "No StarCraft runtime product/version was selected.");
  }
}
