#include <BWAPI/Runtime/RuntimeExecutor.h>
#include <BWAPI/Runtime/RuntimeCommandEncoder.h>
#include <BWAPI/Runtime/RuntimeManifest.h>
#include <BWAPI/Runtime/RuntimeProcess.h>
#include <BWAPI/Runtime/RuntimeProcessMemory.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>
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

    void addCapabilityIfMissing(std::vector<Capability>& capabilities, Capability capability)
    {
      if (std::find(capabilities.begin(), capabilities.end(), capability) == capabilities.end())
        capabilities.push_back(capability);
    }

    std::filesystem::path readyFilePath(const RuntimeEnvironment& environment)
    {
      return std::filesystem::path(environment.executorBridgePath) / RuntimeExecutorBridgeReadyFile;
    }

    std::filesystem::path commandFilePath(const RuntimeEnvironment& environment)
    {
      return std::filesystem::path(environment.executorBridgePath) / RuntimeExecutorBridgeCommandFile;
    }

    std::filesystem::path directCommandAuditPath(const RuntimeEnvironment& environment)
    {
      return std::filesystem::path(environment.executorBridgePath) / "commands.applied.tsv";
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

    std::string contractBindingReadyKey(const std::string& name)
    {
      return "contract.binding." + name;
    }

    bool readyFileHasProofBackedBinding(
      const std::filesystem::path& readyPath,
      const std::string& name,
      BindingKind kind)
    {
      const std::string value = readReadyValue(readyPath, contractBindingReadyKey(name));
      const std::string prefix = std::string(toString(kind)) + '|';
      if (value.rfind(prefix, 0) != 0)
        return false;

      const std::string evidence = value.substr(prefix.size());
      return !evidence.empty() && evidence.rfind("fixture:", 0) != 0;
    }

    bool parseAddressValue(const std::string& value, std::uintptr_t& parsed)
    {
      try
      {
        std::size_t consumed = 0;
        const unsigned long long number = std::stoull(value, &consumed, 0);
        if (consumed != value.size() || number == 0)
          return false;
        parsed = static_cast<std::uintptr_t>(number);
        return static_cast<unsigned long long>(parsed) == number;
      }
      catch (...)
      {
        return false;
      }
    }

    bool readCommandQueueBindingAddress(
      const std::filesystem::path& readyPath,
      const std::string& name,
      std::uintptr_t& address)
    {
      const std::string value = readReadyValue(readyPath, contractBindingReadyKey(name));
      const std::string prefix = std::string(toString(BindingKind::CommandQueue)) + '|';
      if (value.rfind(prefix, 0) != 0)
        return false;

      const std::string evidence = value.substr(prefix.size());
      if (evidence.empty() || evidence.rfind("fixture:", 0) == 0)
        return false;

      const std::size_t separator = evidence.rfind(':');
      if (separator == std::string::npos || separator + 1 >= evidence.size())
        return false;

      return parseAddressValue(evidence.substr(separator + 1), address);
    }

    struct DirectRuntimeCommandQueueSink
    {
      std::uintptr_t bytesInQueueAddress = 0;
      std::uintptr_t turnBufferAddress = 0;
      std::size_t capacityBytes = 512;
    };

    bool readDirectRuntimeCommandQueueSink(
      const std::filesystem::path& readyPath,
      DirectRuntimeCommandQueueSink& sink)
    {
      return readCommandQueueBindingAddress(
          readyPath,
          "BW::BWDATA::sgdwBytesInCmdQueue",
          sink.bytesInQueueAddress)
        && readCommandQueueBindingAddress(
          readyPath,
          "BW::BWDATA::TurnBuffer",
          sink.turnBufferAddress);
    }

    bool readyFileHasDirectRuntimeCommandQueueSink(const std::filesystem::path& readyPath)
    {
      DirectRuntimeCommandQueueSink sink;
      return readDirectRuntimeCommandQueueSink(readyPath, sink);
    }

    bool readyFileHasActiveRuntimeCommandQueueSink(const std::filesystem::path& readyPath)
    {
      return fileContainsLine(readyPath, RuntimeExecutorBridgeActiveCommandReceiverLine)
        && fileContainsLine(readyPath, RuntimeExecutorBridgeRuntimeCommandQueueSinkLine)
        && readyFileHasProofBackedBinding(
          readyPath,
          "BW::BWDATA::sgdwBytesInCmdQueue",
          BindingKind::CommandQueue)
        && readyFileHasProofBackedBinding(
          readyPath,
          "BW::BWDATA::TurnBuffer",
          BindingKind::CommandQueue);
    }

    bool readyFileHasRuntimeCommandQueueSink(const std::filesystem::path& readyPath)
    {
      return readyFileHasActiveRuntimeCommandQueueSink(readyPath)
        || readyFileHasDirectRuntimeCommandQueueSink(readyPath);
    }

    bool readyFileHasSharedMemoryClientTransportProof(const std::filesystem::path& readyPath)
    {
      return fileContainsLine(readyPath, "proof.attach=passed")
        && readyFileHasProofBackedBinding(
          readyPath,
          "shared-memory-client-transport",
          BindingKind::Transport);
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
      result.provenCapabilities.clear();
      for (const RuntimeExecutorBehaviorProof& proof : requiredRuntimeExecutorBehaviorProofs())
      {
        if (!fileContainsLine(readyPath, proof.readyFileLine))
        {
          result.missingBehaviorProofs.push_back(proof.readyFileLine);
          continue;
        }

        if (std::string(proof.id) == "issue-commands"
            && !readyFileHasRuntimeCommandQueueSink(readyPath))
        {
          result.missingBehaviorProofs.push_back(proof.readyFileLine);
          result.errors.push_back(
            "runtime executor bridge issue-commands proof is missing an active or direct runtime command queue sink");
          continue;
        }

        addCapabilityIfMissing(result.provenCapabilities, proof.capability);
      }
      if (fileContainsLine(readyPath, "proof.read_map_data=passed"))
        addCapabilityIfMissing(result.provenCapabilities, Capability::ReadMapData);
      if (fileContainsLine(readyPath, "proof.read_player_data=passed"))
        addCapabilityIfMissing(result.provenCapabilities, Capability::ReadPlayerData);
      if (fileContainsLine(readyPath, "proof.read_bullet_data=passed"))
        addCapabilityIfMissing(result.provenCapabilities, Capability::ReadBulletData);
      if (fileContainsLine(readyPath, "proof.read_region_data=passed"))
        addCapabilityIfMissing(result.provenCapabilities, Capability::ReadRegionData);
      if (fileContainsLine(readyPath, "proof.load_ai_modules=passed"))
        addCapabilityIfMissing(result.provenCapabilities, Capability::LoadAIModules);

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

    bool preflightExecutorBridge(
      const RuntimeEnvironment& environment,
      RuntimeExecutorPreflightResult& result,
      bool memoryRequired)
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

      if (!result.processIdentified)
      {
        result.errors.push_back(
          "runtime executor bridge is stale because the selected runtime process is not visible");
        return true;
      }
      if (memoryRequired
          && !result.memoryAccessible
          && readyFileHasSharedMemoryClientTransportProof(readyPath))
      {
        result.memoryAccessible = true;
        result.memoryAccessReason =
          "validated runtime adapter bridge reported proof.attach=passed";
      }
      if (memoryRequired && !result.memoryAccessible)
      {
        result.errors.push_back(
          "runtime executor bridge is unavailable because selected runtime memory access is denied");
        return true;
      }

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

    bool validateCommandsAgainstBridgeSurface(
      const RuntimeEnvironment& environment,
      const std::vector<RuntimeCommandRequest>& commands,
      RuntimeExecutorSubmitResult& result)
    {
      if (environment.executorBridgePath.empty())
        return false;

      std::error_code error;
      const std::filesystem::path readyPath = readyFilePath(environment);
      if (!std::filesystem::exists(readyPath, error) || error)
        return false;
      if (!bridgeReadyFileMatchesRuntime(environment, readyPath))
        return false;
      if (!validateBridgeRuntimeIdentity(environment, readyPath).empty())
        return false;
      if (!fileContainsLine(readyPath, RuntimeExecutorBridgeCommandSurfaceLine))
        return false;

      const RuntimeCommandSurface surface = makeBWAPICommandSurface();
      for (const RuntimeCommandRequest& command : commands)
      {
        if (command.kind == RuntimeCommandKind::UnitCommand)
        {
          if (!containsCommandSurfaceEntry(surface.unitCommands, command.name))
            return rejectSubmit(result, "runtime bridge command surface does not declare BWAPI unit command: " + command.name);
        }
        else if (command.kind == RuntimeCommandKind::GameAction)
        {
          if (!containsCommandSurfaceEntry(surface.gameActions, command.name))
            return rejectSubmit(result, "runtime bridge command surface does not declare BWAPI game action: " + command.name);
        }
      }

      result.warnings.push_back(
        "runtime manifest was not provided; command was validated against the bridge-proven BWAPI command surface");
      return true;
    }

    bool validateCommandsAgainstManifest(
      const RuntimeEnvironment& environment,
      const std::vector<RuntimeCommandRequest>& commands,
      RuntimeExecutorSubmitResult& result)
    {
      if (environment.product != Product::StarCraftRemastered && environment.manifestPath.empty())
        return true;

      if (environment.manifestPath.empty())
      {
        if (validateCommandsAgainstBridgeSurface(environment, commands, result))
          return true;
        return rejectSubmit(
          result,
          "runtime manifest or bridge-proven command surface is required for StarCraft Remastered command submission");
      }

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
      if (readReadyValue(readyPath, "mode") != RuntimeExecutorBridgeValidatedAdapterMode)
        return rejectSubmit(result, "runtime executor bridge is not a validated runtime adapter");

      std::vector<std::string> missing;
      if (!fileContainsLine(readyPath, "proof.attach=passed"))
        missing.push_back("proof.attach=passed");
      if (!fileContainsLine(readyPath, "proof.issue_commands=passed"))
        missing.push_back("proof.issue_commands=passed");
      if (!readyFileHasRuntimeCommandQueueSink(readyPath))
        missing.push_back("active or direct runtime command queue sink");

      if (missing.empty())
        return true;

      result.reason = "runtime executor bridge is missing command submission proof: " + missing.front();
      for (const std::string& proof : missing)
        result.errors.push_back("runtime executor bridge is missing command submission proof: " + proof);
      return false;
    }

    std::uint32_t readU32LE(const std::vector<unsigned char>& bytes)
    {
      return static_cast<std::uint32_t>(bytes[0])
        | (static_cast<std::uint32_t>(bytes[1]) << 8)
        | (static_cast<std::uint32_t>(bytes[2]) << 16)
        | (static_cast<std::uint32_t>(bytes[3]) << 24);
    }

    void appendDirectCommandAudit(
      const RuntimeEnvironment& environment,
      std::size_t sequence,
      const std::string& status,
      const RuntimeCommandRequest& command,
      const std::string& encodedBytes,
      const std::string& detail)
    {
      const std::filesystem::path auditPath = directCommandAuditPath(environment);
      const bool needsHeader = !std::filesystem::exists(auditPath);
      std::ofstream output(auditPath, std::ios::app);
      if (!output)
        return;
      if (needsHeader)
        output << "sequence\tstatus\tcommand\tencoded_bytes\tdetail\n";
      output << sequence << '\t'
             << status << '\t'
             << serializeCommand(command) << '\t'
             << encodedBytes << '\t'
             << detail << '\n';
    }

    bool submitDirectRuntimeCommands(
      const RuntimeEnvironment& environment,
      const DirectRuntimeCommandQueueSink& sink,
      const std::vector<RuntimeCommandRequest>& commands,
      RuntimeExecutorSubmitResult& result)
    {
      if (commands.empty())
      {
        result.submitted = true;
        return true;
      }

      std::vector<std::uint8_t> encodedBytes;
      std::vector<RuntimeEncodedCommand> encodedCommands;
      encodedCommands.reserve(commands.size());
      for (const RuntimeCommandRequest& command : commands)
      {
        RuntimeEncodedCommand encoded = encodeRuntimeCommandRequest(command);
        if (!encoded.encoded)
          return rejectSubmit(result, "runtime command is not directly encodable for the BW turn-buffer: " + encoded.reason);
        encodedBytes.insert(encodedBytes.end(), encoded.bytes.begin(), encoded.bytes.end());
        encodedCommands.push_back(std::move(encoded));
      }

      RuntimeMemoryReadResult usedBytesRead =
        readProcessMemory(environment.processId, sink.bytesInQueueAddress, sizeof(std::uint32_t));
      if (!usedBytesRead.success || usedBytesRead.bytesRead != sizeof(std::uint32_t))
      {
        const std::string reason = usedBytesRead.reason.empty()
          ? "unable to read runtime command queue byte count"
          : usedBytesRead.reason;
        return rejectSubmit(result, reason);
      }

      const std::size_t usedBytes = readU32LE(usedBytesRead.bytes);
      if (usedBytes > sink.capacityBytes)
        return rejectSubmit(result, "runtime command queue byte count exceeds known turn-buffer capacity");
      if (encodedBytes.size() > sink.capacityBytes - usedBytes)
        return rejectSubmit(result, "runtime command queue does not have enough remaining capacity");

      const std::uintptr_t writeAddress = sink.turnBufferAddress + usedBytes;
      RuntimeMemoryWriteResult writeBytes =
        writeProcessMemory(environment.processId, writeAddress, encodedBytes.data(), encodedBytes.size());
      if (!writeBytes.success || writeBytes.bytesWritten != encodedBytes.size())
      {
        const std::string reason = writeBytes.reason.empty()
          ? "unable to write encoded command bytes to runtime command queue"
          : writeBytes.reason;
        return rejectSubmit(result, reason);
      }

      RuntimeMemoryReadResult readback =
        readProcessMemory(environment.processId, writeAddress, encodedBytes.size());
      if (!readback.success || readback.bytesRead != encodedBytes.size())
      {
        const std::string reason = readback.reason.empty()
          ? "unable to read back encoded command bytes from runtime command queue"
          : readback.reason;
        return rejectSubmit(result, reason);
      }
      if (!std::equal(encodedBytes.begin(), encodedBytes.end(), readback.bytes.begin()))
        return rejectSubmit(result, "encoded command bytes readback did not match runtime command queue write");

      const std::uint32_t newUsedBytes =
        static_cast<std::uint32_t>(usedBytes + encodedBytes.size());
      RuntimeMemoryWriteResult writeCount =
        writeProcessMemory(environment.processId, sink.bytesInQueueAddress, &newUsedBytes, sizeof(newUsedBytes));
      if (!writeCount.success || writeCount.bytesWritten != sizeof(newUsedBytes))
      {
        const std::string reason = writeCount.reason.empty()
          ? "unable to advance runtime command queue byte count"
          : writeCount.reason;
        return rejectSubmit(result, reason);
      }

      std::size_t byteOffset = 0;
      for (std::size_t i = 0; i < commands.size(); ++i)
      {
        appendDirectCommandAudit(
          environment,
          i + 1,
          "applied",
          commands[i],
          formatCommandBytesHex(encodedCommands[i].bytes),
          "written-to-runtime-command-queue:" + std::to_string(writeAddress + byteOffset));
        byteOffset += encodedCommands[i].bytes.size();
      }

      result.submitted = true;
      result.submittedCommands = commands.size();
      result.warnings.push_back("runtime commands were submitted directly to the proven runtime command queue");
      return true;
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

    const bool bridgePreflightHandled = preflightExecutorBridge(
      environment,
      result,
      contractRequiresCapability(proofBackedContract, Capability::SharedMemoryClient));
    if (!bridgePreflightHandled)
      result.warnings.push_back("authorized runtime executor bridge is not configured");
    if (!result.executorAvailable && !bridgePreflightHandled)
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
      },
      {
        "load-ai-modules",
        Capability::LoadAIModules,
        "proof.load_ai_modules=passed",
        "adapter loaded or validated BWAPI-compatible AI module delivery for the target runtime"
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

    std::vector<RuntimeCommandRequest> drained = queue.drain();
    DirectRuntimeCommandQueueSink directSink;
    if (!readyFileHasActiveRuntimeCommandQueueSink(readyPath)
        && readDirectRuntimeCommandQueueSink(readyPath, directSink))
    {
      submitDirectRuntimeCommands(environment, directSink, drained, result);
      return result;
    }

    std::ofstream output(commandFilePath(environment), std::ios::app);
    if (!output)
    {
      result.reason = "unable to open runtime executor command file";
      result.errors.push_back(result.reason);
      return result;
    }

    for (const RuntimeCommandRequest& command : drained)
      output << serializeCommand(command) << '\n';

    result.submitted = true;
    result.submittedCommands = drained.size();
    return result;
  }
}
