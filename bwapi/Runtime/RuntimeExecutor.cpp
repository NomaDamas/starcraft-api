#include <BWAPI/Runtime/RuntimeExecutor.h>
#include <BWAPI/Runtime/RuntimeProcess.h>

#include <filesystem>
#include <fstream>
#include <sstream>

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

    std::filesystem::path readyFilePath(const RuntimeEnvironment& environment)
    {
      return std::filesystem::path(environment.executorBridgePath) / RuntimeExecutorBridgeReadyFile;
    }

    std::filesystem::path commandFilePath(const RuntimeEnvironment& environment)
    {
      return std::filesystem::path(environment.executorBridgePath) / RuntimeExecutorBridgeCommandFile;
    }

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

    bool preflightExecutorBridge(const RuntimeEnvironment& environment, RuntimeExecutorPreflightResult& result)
    {
      if (environment.executorBridgePath.empty())
        return false;

      std::error_code error;
      const std::filesystem::path bridgePath(environment.executorBridgePath);
      if (!std::filesystem::exists(bridgePath, error))
      {
        result.errors.push_back("runtime executor bridge path does not exist: " + environment.executorBridgePath);
        return true;
      }
      if (error)
      {
        result.errors.push_back("unable to inspect runtime executor bridge path: " + error.message());
        return true;
      }
      if (!std::filesystem::is_directory(bridgePath, error))
      {
        result.errors.push_back("runtime executor bridge path is not a directory: " + environment.executorBridgePath);
        return true;
      }
      if (error)
      {
        result.errors.push_back("unable to inspect runtime executor bridge directory: " + error.message());
        return true;
      }

      const std::filesystem::path readyPath = readyFilePath(environment);
      if (!std::filesystem::exists(readyPath, error))
      {
        result.errors.push_back("runtime executor bridge ready file does not exist: " + readyPath.string());
        return true;
      }
      if (error)
      {
        result.errors.push_back("unable to inspect runtime executor bridge ready file: " + error.message());
        return true;
      }
      if (!fileContainsLine(readyPath, std::string("protocol=") + RuntimeExecutorBridgeProtocol))
      {
        result.errors.push_back("runtime executor bridge ready file has an unsupported protocol");
        return true;
      }
      if (!fileContainsLine(readyPath, std::string("product=") + toString(environment.product)))
      {
        result.errors.push_back("runtime executor bridge ready file product does not match the selected runtime");
        return true;
      }
      if (!environment.version.empty()
          && !fileContainsLine(readyPath, std::string("version=") + environment.version))
      {
        result.errors.push_back("runtime executor bridge ready file version does not match the selected runtime");
        return true;
      }

      result.executorAvailable = true;
      result.executorName = "filesystem-bridge";
      return true;
    }

    std::string serializeArguments(const std::vector<int>& arguments)
    {
      std::ostringstream output;
      for (std::size_t i = 0; i < arguments.size(); ++i)
      {
        if (i > 0)
          output << ',';
        output << arguments[i];
      }
      return output.str();
    }

    std::string serializeCommand(const RuntimeCommandRequest& command)
    {
      std::ostringstream output;
      output << toString(command.kind) << '|'
             << command.name << '|'
             << command.targetUnitId << '|'
             << serializeArguments(command.arguments);
      return output.str();
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

    RuntimeProcessOpenResult process = openRuntimeProcess(environment);
    result.processIdentified = process.opened;
    if (!process.opened)
      result.errors.push_back(process.reason);

    if (!preflightExecutorBridge(environment, result))
      result.warnings.push_back("authorized runtime executor bridge is not configured");

    return result;
  }

  RuntimeExecutorSubmitResult submitRuntimeCommands(
    const RuntimeEnvironment& environment,
    const std::vector<RuntimeCommandRequest>& commands)
  {
    RuntimeExecutorSubmitResult result;

    RuntimeCommandQueue queue;
    for (const RuntimeCommandRequest& command : commands)
    {
      RuntimeCommandQueueResult queued = queue.enqueue(command);
      if (!queued.accepted)
      {
        result.errors.push_back(queued.reason);
        result.reason = queued.reason;
        return result;
      }
    }

    if (environment.executorBridgePath.empty())
    {
      result.reason = "runtime executor bridge path is empty";
      result.errors.push_back(result.reason);
      return result;
    }

    std::error_code error;
    const std::filesystem::path bridgePath(environment.executorBridgePath);
    if (!std::filesystem::is_directory(bridgePath, error) || error)
    {
      result.reason = "runtime executor bridge path is not available: " + environment.executorBridgePath;
      result.errors.push_back(result.reason);
      return result;
    }

    const std::filesystem::path readyPath = readyFilePath(environment);
    if (!fileContainsLine(readyPath, std::string("protocol=") + RuntimeExecutorBridgeProtocol))
    {
      result.reason = "runtime executor bridge ready file has an unsupported protocol";
      result.errors.push_back(result.reason);
      return result;
    }
    if (!fileContainsLine(readyPath, std::string("product=") + toString(environment.product)))
    {
      result.reason = "runtime executor bridge ready file product does not match the selected runtime";
      result.errors.push_back(result.reason);
      return result;
    }
    if (!environment.version.empty()
        && !fileContainsLine(readyPath, std::string("version=") + environment.version))
    {
      result.reason = "runtime executor bridge ready file version does not match the selected runtime";
      result.errors.push_back(result.reason);
      return result;
    }

    std::ofstream output(commandFilePath(environment), std::ios::app);
    if (!output)
    {
      result.reason = "unable to open runtime executor command file";
      result.errors.push_back(result.reason);
      return result;
    }

    std::vector<RuntimeCommandRequest> drained = queue.drain();
    for (const RuntimeCommandRequest& command : drained)
      output << serializeCommand(command) << '\n';

    result.submitted = true;
    result.submittedCommands = drained.size();
    return result;
  }
}
