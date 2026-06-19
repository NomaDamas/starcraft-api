#include <BWAPI/Runtime/RuntimeExecutor.h>

#include <filesystem>

namespace BWAPI::Runtime
{
  namespace
  {
    bool supportedExecutorPlatform(Platform platform)
    {
      return platform == Platform::MacOS || platform == Platform::Linux || platform == Platform::Windows;
    }

    bool supportedExecutorProduct(Product product)
    {
      return product == Product::StarCraftRemastered || product == Product::StarCraftBroodWar1161;
    }
  }

  RuntimeExecutorPreflightResult preflightRuntimeExecutor(
    const RuntimeEnvironment& environment,
    const RuntimeContract& contract)
  {
    RuntimeExecutorPreflightResult result;

    const ContractValidationResult validation = validateRuntimeContract(contract);
    result.contractValid = validation.valid;
    result.errors.insert(result.errors.end(), validation.errors.begin(), validation.errors.end());
    result.warnings.insert(result.warnings.end(), validation.warnings.begin(), validation.warnings.end());

    if (!supportedExecutorPlatform(environment.platform))
      result.errors.push_back("runtime platform is unsupported for process execution");
    if (!supportedExecutorProduct(environment.product))
      result.errors.push_back("runtime product is unsupported for process execution");

    if (environment.executablePath.empty())
    {
      result.errors.push_back("runtime executable path is empty");
    }
    else
    {
      std::error_code error;
      result.targetLocated = std::filesystem::exists(environment.executablePath, error);
      if (error)
        result.errors.push_back("unable to inspect runtime executable path: " + error.message());
      else if (!result.targetLocated)
        result.errors.push_back("runtime executable path does not exist: " + environment.executablePath);
    }

    result.warnings.push_back("authorized runtime executor is not implemented for this product/platform");
    result.executorAvailable = false;
    return result;
  }
}
