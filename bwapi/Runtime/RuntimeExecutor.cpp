#include <BWAPI/Runtime/RuntimeExecutor.h>
#include <BWAPI/Runtime/RuntimeManifest.h>
#include <BWAPI/Runtime/RuntimeProcess.h>
#include <BWAPI/Runtime/RuntimeProcessMemory.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

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

    bool contractRequiresCapability(const RuntimeContract& contract, Capability capability)
    {
      for (Capability required : contract.requiredCapabilities)
      {
        if (required == capability)
          return true;
      }
      return false;
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

    std::vector<std::string> splitPipe(const std::string& value)
    {
      std::vector<std::string> parts;
      std::string part;
      std::istringstream input(value);
      while (std::getline(input, part, '|'))
        parts.push_back(part);
      return parts;
    }

    bool parseSizeValue(const std::string& value, std::size_t& parsed)
    {
      try
      {
        std::size_t consumed = 0;
        const unsigned long long number = std::stoull(value, &consumed, 0);
        if (consumed != value.size())
          return false;
        parsed = static_cast<std::size_t>(number);
        return static_cast<unsigned long long>(parsed) == number;
      }
      catch (...)
      {
        return false;
      }
    }

    std::string normalizePath(const std::string& path)
    {
      if (path.empty())
        return {};

      std::error_code error;
      std::filesystem::path normalized = std::filesystem::weakly_canonical(path, error);
      if (error)
        normalized = std::filesystem::absolute(path, error);
      if (error)
        normalized = path;
      return normalized.lexically_normal().string();
    }

    bool bridgeReadyFileMatchesRuntime(
      const RuntimeEnvironment& environment,
      const std::filesystem::path& readyPath)
    {
      if (!fileContainsLine(readyPath, std::string("protocol=") + RuntimeExecutorBridgeProtocol))
        return false;
      if (!fileContainsLine(readyPath, std::string("product=") + toString(environment.product)))
        return false;
      if (!environment.version.empty()
          && !fileContainsLine(readyPath, std::string("version=") + environment.version))
        return false;
      return readReadyValue(readyPath, "mode") == RuntimeExecutorBridgeValidatedAdapterMode;
    }

    std::string validateBridgeRuntimeIdentity(
      const RuntimeEnvironment& environment,
      const std::filesystem::path& readyPath)
    {
      if (environment.processId > 0)
      {
        const std::string readyProcessId = readReadyValue(readyPath, "process_id");
        if (readyProcessId != std::to_string(environment.processId))
          return "runtime executor bridge ready file process_id does not match the selected runtime";
      }
      if (!environment.executablePath.empty())
      {
        const std::string readyExecutable = readReadyValue(readyPath, "executable");
        if (readyExecutable.empty()
            || normalizePath(readyExecutable) != normalizePath(environment.executablePath))
          return "runtime executor bridge ready file executable does not match the selected runtime";
      }
      return {};
    }

    void applyBindingProof(RuntimeContract& contract, const std::string& name, const std::string& value)
    {
      const std::vector<std::string> parts = splitPipe(value);
      if (parts.size() != 2 || parts[1].empty())
        return;

      BindingKind kind = BindingKind::DataAddress;
      if (!parseBindingKind(parts[0], kind))
        return;

      auto it = std::find_if(
        contract.bindings.begin(),
        contract.bindings.end(),
        [&](const RuntimeBinding& binding)
        {
          return binding.name == name && binding.kind == kind;
        });
      if (it == contract.bindings.end())
        return;

      it->resolved = true;
      it->evidence = parts[1];
    }

    void applyStructureProof(RuntimeContract& contract, const std::string& name, const std::string& value)
    {
      const std::vector<std::string> parts = splitPipe(value);
      if (parts.size() != 2 || parts[1].empty())
        return;

      std::size_t size = 0;
      if (!parseSizeValue(parts[0], size) || size == 0)
        return;

      auto it = std::find_if(
        contract.structures.begin(),
        contract.structures.end(),
        [&](const StructureLayout& structure)
        {
          return structure.name == name;
        });
      if (it == contract.structures.end())
        return;

      it->size = size;
    }

    void applyFieldProof(RuntimeContract& contract, const std::string& name, const std::string& value)
    {
      const std::size_t separator = name.rfind('.');
      if (separator == std::string::npos || separator == 0 || separator + 1 >= name.size())
        return;

      const std::string structureName = name.substr(0, separator);
      const std::string fieldName = name.substr(separator + 1);
      const std::vector<std::string> parts = splitPipe(value);
      if (parts.size() != 3 || parts[2].empty())
        return;

      std::size_t offset = 0;
      std::size_t size = 0;
      if (!parseSizeValue(parts[0], offset) || !parseSizeValue(parts[1], size) || size == 0)
        return;

      auto structureIt = std::find_if(
        contract.structures.begin(),
        contract.structures.end(),
        [&](const StructureLayout& structure)
        {
          return structure.name == structureName;
        });
      if (structureIt == contract.structures.end())
        return;

      auto fieldIt = std::find_if(
        structureIt->fields.begin(),
        structureIt->fields.end(),
        [&](const StructureField& field)
        {
          return field.name == fieldName;
        });
      if (fieldIt == structureIt->fields.end())
        return;

      fieldIt->resolved = true;
      fieldIt->offset = offset;
      fieldIt->size = size;
    }

    bool validateProductionBridgeProof(
      const std::filesystem::path& readyPath,
      RuntimeExecutorPreflightResult& result,
      bool requireBehaviorProofs = true)
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
      return requireBehaviorProofs ? valid : true;
    }

    void addMissingBehaviorProofsIfEmpty(RuntimeExecutorPreflightResult& result)
    {
      if (!result.missingBehaviorProofs.empty())
        return;

      for (const RuntimeExecutorBehaviorProof& proof : requiredRuntimeExecutorBehaviorProofs())
        result.missingBehaviorProofs.push_back(proof.readyFileLine);
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

      const std::string identityError = validateBridgeRuntimeIdentity(environment, readyPath);
      if (!identityError.empty())
      {
        result.errors.push_back(identityError);
        return true;
      }

      if (!validateProductionBridgeProof(readyPath, result, false))
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

  RuntimeContract applyRuntimeExecutorBridgeContractProofs(
    const RuntimeEnvironment& environment,
    RuntimeContract contract)
  {
    if (environment.executorBridgePath.empty())
      return contract;

    std::error_code error;
    const std::filesystem::path readyPath = readyFilePath(environment);
    if (!std::filesystem::exists(readyPath, error) || error)
      return contract;
    if (!bridgeReadyFileMatchesRuntime(environment, readyPath))
      return contract;
    if (!validateBridgeRuntimeIdentity(environment, readyPath).empty())
      return contract;

    std::ifstream input(readyPath);
    std::string line;
    while (std::getline(input, line))
    {
      const std::size_t separator = line.find('=');
      if (separator == std::string::npos)
        continue;

      const std::string key = line.substr(0, separator);
      const std::string value = line.substr(separator + 1);
      constexpr const char* bindingPrefix = "contract.binding.";
      constexpr const char* structurePrefix = "contract.structure.";
      constexpr const char* fieldPrefix = "contract.field.";

      if (key.rfind(bindingPrefix, 0) == 0)
        applyBindingProof(contract, key.substr(std::char_traits<char>::length(bindingPrefix)), value);
      else if (key.rfind(structurePrefix, 0) == 0)
        applyStructureProof(contract, key.substr(std::char_traits<char>::length(structurePrefix)), value);
      else if (key.rfind(fieldPrefix, 0) == 0)
        applyFieldProof(contract, key.substr(std::char_traits<char>::length(fieldPrefix)), value);
    }

    return contract;
  }

  RuntimeExecutorPreflightResult preflightRuntimeExecutor(
    const RuntimeEnvironment& environment,
    const RuntimeContract& contract)
  {
    RuntimeExecutorPreflightResult result;

    const RuntimeContract proofBackedContract =
      applyRuntimeExecutorBridgeContractProofs(environment, contract);
    const ContractValidationResult validation = validateRuntimeContract(proofBackedContract);
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
    {
      result.errors.push_back(process.reason);
    }
    else if (contractRequiresCapability(proofBackedContract, Capability::SharedMemoryClient))
    {
      const RuntimeMemoryAccessResult memoryAccess = openProcessMemoryAccess(environment.processId);
      result.memoryAccessible = memoryAccess.accessible;
      result.memoryAccessReason = memoryAccess.reason;
    }

    if (!preflightExecutorBridge(environment, result))
      result.warnings.push_back("authorized runtime executor bridge is not configured");
    if (!result.executorAvailable)
      addMissingBehaviorProofsIfEmpty(result);

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
        "active-match-state",
        Capability::ReadGameState,
        "proof.active_match_state=passed",
        "adapter proved the selected runtime is inside an active match/replay, not only menu/login state"
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

    const std::string identityError = validateBridgeRuntimeIdentity(environment, readyPath);
    if (!identityError.empty())
    {
      rejectSubmit(result, identityError);
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
