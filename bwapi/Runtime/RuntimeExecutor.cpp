#include <BWAPI/Runtime/RuntimeExecutor.h>
#include <BWAPI/Runtime/RuntimeManifest.h>
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

    std::string readReadyValue(const std::filesystem::path& path, const std::string& key)
    {
      std::ifstream input(path);
      const std::string prefix = key + '=';
      std::string line;
      while (std::getline(input, line))
      {
        if (line.rfind(prefix, 0) == 0)
          return line.substr(prefix.size());
      }
      return {};
    }

    bool validateProductionBridgeProof(
      const std::filesystem::path& readyPath,
      RuntimeExecutorPreflightResult& result)
    {
      result.executorBridgeMode = readReadyValue(readyPath, "mode");
      result.missingBehaviorProofs.clear();
      for (const RuntimeExecutorBehaviorProof& proof : requiredRuntimeExecutorBehaviorProofs())
      {
        if (!fileContainsLine(readyPath, proof.readyFileLine))
          result.missingBehaviorProofs.push_back(proof.readyFileLine);
      }

      if (result.executorBridgeMode == RuntimeExecutorBridgeBootstrapMode)
      {
        result.executorName = "filesystem-bridge-bootstrap";
        result.errors.push_back(
          "runtime executor bridge is launch/attach bootstrap only; validated runtime adapter proof is required");
        return false;
      }

      if (result.executorBridgeMode != RuntimeExecutorBridgeValidatedAdapterMode)
      {
        result.errors.push_back("runtime executor bridge ready file is missing validated runtime adapter mode");
        return false;
      }

      bool valid = true;
      for (const std::string& proof : result.missingBehaviorProofs)
      {
        result.errors.push_back("runtime executor bridge ready file is missing behavior proof: " + proof);
        valid = false;
      }
      return valid;
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

      if (!validateProductionBridgeProof(readyPath, result))
        return true;

      result.executorAvailable = true;
      result.executorName = "filesystem-bridge-validated-runtime-adapter";
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

    bool rejectSubmit(RuntimeExecutorSubmitResult& result, const std::string& reason)
    {
      result.reason = reason;
      result.errors.push_back(reason);
      return false;
    }

    bool validateCommandsAgainstManifest(
      const RuntimeEnvironment& environment,
      const std::vector<RuntimeCommandRequest>& commands,
      RuntimeExecutorSubmitResult& result)
    {
      if (environment.product != Product::StarCraftRemastered && environment.manifestPath.empty())
        return true;

      if (environment.manifestPath.empty())
        return rejectSubmit(result, "runtime manifest is required for StarCraft Remastered command submission");

      RuntimeManifestLoadResult manifest = loadRuntimeManifestFile(environment.manifestPath);
      if (!manifest.loaded)
      {
        const std::string reason = manifest.errors.empty()
          ? "runtime manifest failed to load"
          : "runtime manifest failed to load: " + manifest.errors.front();
        return rejectSubmit(result, reason);
      }

      if (manifest.manifest.contract.product != environment.product)
        return rejectSubmit(result, "runtime manifest product does not match the selected runtime");
      if (!environment.version.empty() && manifest.manifest.contract.version != environment.version)
        return rejectSubmit(result, "runtime manifest version does not match the selected runtime");

      ContractValidationResult validation = validateRuntimeContract(manifest.manifest.contract);
      if (!validation.valid)
      {
        const std::string reason = validation.errors.empty()
          ? "runtime manifest contract is invalid"
          : "runtime manifest contract is invalid: " + validation.errors.front();
        return rejectSubmit(result, reason);
      }

      for (const RuntimeCommandRequest& command : commands)
      {
        if (command.kind == RuntimeCommandKind::UnitCommand)
        {
          if (!containsCommandSurfaceEntry(manifest.manifest.unitCommands, command.name))
            return rejectSubmit(result, "runtime manifest does not declare BWAPI unit command: " + command.name);
        }
        else if (command.kind == RuntimeCommandKind::GameAction)
        {
          if (!containsCommandSurfaceEntry(manifest.manifest.gameActions, command.name))
            return rejectSubmit(result, "runtime manifest does not declare BWAPI game action: " + command.name);
        }
      }

      return true;
    }

    bool validateSubmissionBridgeProof(
      const std::filesystem::path& readyPath,
      RuntimeExecutorSubmitResult& result)
    {
      RuntimeExecutorPreflightResult bridgeProof;
      if (validateProductionBridgeProof(readyPath, bridgeProof))
        return true;

      const std::string reason = bridgeProof.errors.empty()
        ? "runtime executor bridge is not a validated runtime adapter"
        : bridgeProof.errors.front();
      result.reason = reason;
      result.errors.insert(result.errors.end(), bridgeProof.errors.begin(), bridgeProof.errors.end());
      return false;
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

  const std::vector<RuntimeExecutorBehaviorProof>& requiredRuntimeExecutorBehaviorProofs()
  {
    static const std::vector<RuntimeExecutorBehaviorProof> proofs = {
      {
        "attach",
        Capability::SharedMemoryClient,
        "proof.attach=passed",
        "authorized adapter attached to the selected StarCraft runtime process"
      },
      {
        "read-game-state",
        Capability::ReadGameState,
        "proof.read_game_state=passed",
        "adapter read stable frame/game-state data from the target runtime"
      },
      {
        "read-units",
        Capability::ReadUnitData,
        "proof.read_units=passed",
        "adapter read BWAPI-compatible unit data from the target runtime"
      },
      {
        "issue-commands",
        Capability::IssueCommands,
        "proof.issue_commands=passed",
        "adapter delivered a BWAPI command into the target runtime command path"
      },
      {
        "draw-overlays",
        Capability::DrawOverlays,
        "proof.draw_overlays=passed",
        "adapter rendered BWAPI overlay primitives in the target runtime"
      },
      {
        "dispatch-events",
        Capability::DispatchEvents,
        "proof.dispatch_events=passed",
        "adapter dispatched BWAPI lifecycle and unit events from observed runtime state"
      },
      {
        "replay-analysis",
        Capability::ReplayAnalysis,
        "proof.replay_analysis=passed",
        "adapter read replay metadata and frame progression consistently"
      },
      {
        "multiplayer-sync",
        Capability::MultiplayerSync,
        "proof.multiplayer_sync=passed",
        "adapter validated multiplayer command synchronization behavior"
      },
      {
        "battle-net-policy",
        Capability::BattleNet,
        "proof.battle_net_policy=passed",
        "adapter completed the authorized Battle.net policy validation path"
      }
    };
    return proofs;
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

    if (!validateCommandsAgainstManifest(environment, commands, result))
      return result;

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
    if (!validateSubmissionBridgeProof(readyPath, result))
      return result;

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
