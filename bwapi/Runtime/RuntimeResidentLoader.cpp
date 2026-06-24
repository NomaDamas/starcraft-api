#include <BWAPI/Runtime/RuntimeResidentLoader.h>

#include <filesystem>

namespace BWAPI::Runtime
{
  RuntimeResidentLoadPlan planRuntimeResidentAdapterLoad(
    const RuntimeResidentLoadRequest& request)
  {
    RuntimeResidentLoadPlan plan;
    plan.loaderMode = "macos-resident-dylib";

    const RuntimeEnvironment& environment = request.environment;
    if (environment.platform != Platform::MacOS)
      plan.errors.push_back("resident adapter loader is currently scoped to macOS");
    if (environment.product != Product::StarCraftRemastered)
      plan.errors.push_back("resident adapter loader requires StarCraft Remastered");
    if (environment.processId <= 0)
      plan.errors.push_back("resident adapter loader requires a selected runtime process id");
    if (environment.executablePath.empty())
      plan.errors.push_back("resident adapter loader requires a selected runtime executable");
    if (environment.executorBridgePath.empty())
      plan.errors.push_back("resident adapter loader requires an executor bridge path");
    if (request.adapterPath.empty())
    {
      plan.errors.push_back("resident adapter loader requires an adapter dylib path");
    }
    else
    {
      std::error_code error;
      if (!std::filesystem::exists(request.adapterPath, error) || error)
        plan.errors.push_back("resident adapter dylib path does not exist");
    }

    plan.valid = plan.errors.empty();
    return plan;
  }
}
