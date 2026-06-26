#include "RemasteredRuntimeBackend.h"

#include <BWAPI/Runtime/RuntimeCommandSurface.h>
#include <BWAPI/Runtime/RuntimeContract.h>
#include <BWAPI/Runtime/RuntimeExecutor.h>
#include <BWAPI/Runtime/RuntimeManifest.h>
#include <BWAPI/Runtime/RuntimeProcessMemory.h>
#include <BWAPI/Runtime/RuntimeResidentBridge.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace BWAPI::Runtime
{
  namespace
  {
    std::string unsupportedReason(const RuntimeEnvironment& environment)
    {
      RuntimeContract contract = makeRemasteredParityContract(environment.version.empty() ? "unknown" : environment.version);
      if (!environment.manifestPath.empty())
      {
        RuntimeManifestLoadResult manifest = loadRuntimeManifestFile(environment.manifestPath);
        if (manifest.loaded)
          contract = manifest.manifest.contract;
      }

      contract = applyRuntimeExecutorBridgeContractProofs(environment, contract);
      ContractValidationResult validation = validateRuntimeContract(contract);
      RuntimeExecutorPreflightResult preflight = preflightRuntimeExecutor(environment, contract);

      std::ostringstream message;
      message << "StarCraft Remastered runtime backend is not production ready. "
              << "The parity contract currently has " << validation.errors.size()
              << " unresolved production gate error(s). "
              << "Executor preflight has " << preflight.errors.size()
              << " error(s) and " << preflight.warnings.size() << " warning(s).";
      if (contractContainsFixtureEvidence(contract))
        message << " Runtime contract uses fixture validation evidence and cannot claim production support.";
      if (!environment.manifestPath.empty())
        message << " Runtime manifest: " << environment.manifestPath << '.';
      return message.str();
    }

    void addCapabilityIfMissing(RuntimeProbeResult& result, Capability capability)
    {
      if (std::find(result.capabilities.begin(), result.capabilities.end(), capability) == result.capabilities.end())
        result.capabilities.push_back(capability);
    }

    bool contractRequiresCapability(const RuntimeContract& contract, Capability capability)
    {
      return std::find(
        contract.requiredCapabilities.begin(),
        contract.requiredCapabilities.end(),
        capability) != contract.requiredCapabilities.end();
    }

    void addValidatedBridgeCapabilities(
      RuntimeProbeResult& result,
      const RuntimeExecutorPreflightResult& preflight)
    {
      if (preflight.executorBridgeMode != RuntimeExecutorBridgeValidatedAdapterMode)
        return;

      for (Capability capability : preflight.provenCapabilities)
        addCapabilityIfMissing(result, capability);
    }

    void addBuiltInApiSurface(RuntimeProbeResult& result, const RuntimeContract& contract)
    {
      result.implementedApiSurfaceMethods = contract.requiredApiSurfaceMethods;
    }

    void addBuiltInCommandSurface(RuntimeProbeResult& result)
    {
      const RuntimeCommandSurface surface = makeBWAPICommandSurface();
      result.implementedUnitCommands = surface.unitCommands;
      result.implementedGameActions = surface.gameActions;
      result.implementedUnitCommandEvidence = surface.unitCommandEvidence;
      result.implementedGameActionEvidence = surface.gameActionEvidence;
      result.implementedCommandSurfaceEntries = surface.totalEntries();
    }

    std::vector<RuntimeCommandEvidence> staticManifestCommandEvidence(
      std::vector<RuntimeCommandEvidence> evidence)
    {
      for (RuntimeCommandEvidence& entry : evidence)
      {
        if (entry.status != RuntimeCommandEvidenceStatus::LiveProven)
          continue;

        entry.status = RuntimeCommandEvidenceStatus::DocumentedScenario;
        entry.detail = entry.detail.empty()
          ? "static-manifest-live-proof-requires-validated-resident-proof"
          : entry.detail + ":static-manifest-live-proof-requires-validated-resident-proof";
      }
      return evidence;
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

    std::string readReadyValue(
      const std::filesystem::path& readyPath,
      const std::string& key)
    {
      std::ifstream input(readyPath);
      const std::string prefix = key + '=';
      std::string line;
      while (std::getline(input, line))
      {
        if (line.rfind(prefix, 0) == 0)
          return line.substr(prefix.size());
      }
      return {};
    }

    bool parseUnsignedString(const std::string& text, std::uint64_t& value)
    {
      if (text.empty())
        return false;
      try
      {
        std::size_t consumed = 0;
        const unsigned long long parsed = std::stoull(text, &consumed, 0);
        if (consumed != text.size())
          return false;
        value = static_cast<std::uint64_t>(parsed);
        return static_cast<unsigned long long>(value) == parsed;
      }
      catch (...)
      {
        return false;
      }
    }

    bool parseUnsignedReadyValue(
      const std::filesystem::path& readyPath,
      const std::string& key,
      std::uint64_t& value)
    {
      return parseUnsignedString(readReadyValue(readyPath, key), value);
    }

    bool pathIsWithinDirectory(
      const std::filesystem::path& directory,
      const std::filesystem::path& path)
    {
      auto directoryIt = directory.begin();
      auto pathIt = path.begin();
      for (; directoryIt != directory.end(); ++directoryIt, ++pathIt)
      {
        if (pathIt == path.end() || *directoryIt != *pathIt)
          return false;
      }
      return true;
    }

    bool readySnapshotPath(
      const std::filesystem::path& readyPath,
      const std::string& key,
      std::filesystem::path& snapshot)
    {
      const std::string value = readReadyValue(readyPath, key);
      if (value.empty())
        return false;

      const std::filesystem::path requested(value);
      if (requested.empty() || requested.is_absolute())
        return false;

      snapshot = (readyPath.parent_path() / requested).lexically_normal();
      std::error_code error;
      if (!std::filesystem::is_regular_file(snapshot, error) || error)
        return false;
      const std::uintmax_t size = std::filesystem::file_size(snapshot, error);
      if (error || size == 0)
        return false;

      const std::filesystem::path bridgeDirectory =
        std::filesystem::canonical(readyPath.parent_path(), error);
      if (error)
        return false;
      const std::filesystem::path canonicalSnapshot =
        std::filesystem::canonical(snapshot, error);
      if (error || !pathIsWithinDirectory(bridgeDirectory, canonicalSnapshot))
        return false;

      snapshot = canonicalSnapshot;
      return true;
    }

    std::vector<std::string> splitTabs(const std::string& line)
    {
      std::vector<std::string> fields;
      std::size_t start = 0;
      while (start <= line.size())
      {
        const std::size_t tab = line.find('\t', start);
        if (tab == std::string::npos)
        {
          fields.push_back(line.substr(start));
          break;
        }
        fields.push_back(line.substr(start, tab - start));
        start = tab + 1;
      }
      return fields;
    }

    struct SnapshotMetadata
    {
      std::unordered_map<std::string, std::string> values;
      bool hasDuplicateKey = false;
    };

    bool parseSnapshotMetadataLine(
      const std::string& line,
      SnapshotMetadata& metadata)
    {
      if (line.empty() || line[0] != '#')
        return false;

      std::string body = line.substr(1);
      while (!body.empty() && (body.front() == ' ' || body.front() == '\t'))
        body.erase(body.begin());
      if (body.empty())
        return true;

      std::size_t separator = body.find('=');
      if (separator == std::string::npos)
        separator = body.find('\t');
      if (separator == std::string::npos || separator == 0)
        return true;

      std::string key = body.substr(0, separator);
      std::string value = body.substr(separator + 1);
      while (!key.empty() && (key.back() == ' ' || key.back() == '\t'))
        key.pop_back();
      while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
        value.erase(value.begin());

      if (!key.empty())
      {
        if (metadata.values.find(key) != metadata.values.end())
          metadata.hasDuplicateKey = true;
        metadata.values[key] = value;
      }
      return true;
    }

    SnapshotMetadata readSnapshotMetadata(const std::filesystem::path& snapshot)
    {
      SnapshotMetadata metadata;
      std::ifstream input(snapshot);
      std::string line;
      while (std::getline(input, line))
      {
        if (!parseSnapshotMetadataLine(line, metadata))
          break;
      }
      return metadata;
    }

    std::string snapshotMetadataValue(
      const SnapshotMetadata& metadata,
      const std::string& key)
    {
      const auto it = metadata.values.find(key);
      return it == metadata.values.end() ? std::string() : it->second;
    }

    bool parseUnsignedMetadataValue(
      const SnapshotMetadata& metadata,
      const std::string& key,
      std::uint64_t& value)
    {
      return parseUnsignedString(snapshotMetadataValue(metadata, key), value);
    }

    struct SnapshotFieldValueTable
    {
      std::unordered_map<std::string, std::string> values;
      bool headerValid = false;
      bool hasDuplicateKey = false;
      bool hasMalformedRow = false;
      std::size_t rowCount = 0;
    };

    SnapshotFieldValueTable readSnapshotFieldValueTable(const std::filesystem::path& snapshot)
    {
      SnapshotFieldValueTable table;
      std::ifstream input(snapshot);
      std::string line;
      bool headerSeen = false;
      while (std::getline(input, line))
      {
        if (!headerSeen && (line.empty() || line[0] == '#'))
          continue;
        if (headerSeen && line.empty())
          break;
        if (headerSeen && line[0] == '#')
          continue;

        const std::vector<std::string> fields = splitTabs(line);
        if (!headerSeen)
        {
          headerSeen = true;
          table.headerValid = fields.size() == 2
            && fields[0] == "field"
            && fields[1] == "value";
          if (!table.headerValid)
            return table;
          continue;
        }

        if (fields.size() != 2 || fields[0].empty())
        {
          table.hasMalformedRow = true;
          continue;
        }
        if (table.values.find(fields[0]) != table.values.end())
          table.hasDuplicateKey = true;
        table.values[fields[0]] = fields[1];
        ++table.rowCount;
      }
      return table;
    }

    std::string snapshotFieldValue(
      const SnapshotFieldValueTable& table,
      const std::string& key)
    {
      const auto it = table.values.find(key);
      return it == table.values.end() ? std::string() : it->second;
    }

    bool snapshotFieldValueIsTrue(
      const SnapshotFieldValueTable& table,
      const std::string& key)
    {
      return snapshotFieldValue(table, key) == "true";
    }

    bool snapshotFieldValueIsFalse(
      const SnapshotFieldValueTable& table,
      const std::string& key)
    {
      return snapshotFieldValue(table, key) == "false";
    }

    bool snapshotFieldUnsignedValue(
      const SnapshotFieldValueTable& table,
      const std::string& key,
      std::uint64_t& value)
    {
      return parseUnsignedString(snapshotFieldValue(table, key), value);
    }

    bool preflightHasValidatedProof(
      const RuntimeExecutorPreflightResult& preflight,
      const std::string& proofLine)
    {
      return std::find(
        preflight.missingBehaviorProofs.begin(),
        preflight.missingBehaviorProofs.end(),
        proofLine) == preflight.missingBehaviorProofs.end();
    }

    bool preflightSupportsCommandSpecificEvidence(
      const RuntimeExecutorPreflightResult& preflight,
      const std::string& expectedKind)
    {
      if (preflight.executorBridgeMode != RuntimeExecutorBridgeValidatedAdapterMode
          || !preflightHasValidatedProof(preflight, "proof.read_game_state=passed")
          || !preflightHasValidatedProof(preflight, "proof.active_match_state=passed"))
      {
        return false;
      }

      return expectedKind != "unit-command"
        || preflightHasValidatedProof(preflight, "proof.read_units=passed");
    }

    bool readyFileMatchesEnvironmentProcess(
      const std::filesystem::path& readyPath,
      const RuntimeEnvironment& environment)
    {
      std::uint64_t readyProcessId = 0;
      if (!parseUnsignedReadyValue(readyPath, "process_id", readyProcessId)
          || readyProcessId == 0)
      {
        return false;
      }
      if (environment.processId > 0
          && readyProcessId != static_cast<std::uint64_t>(environment.processId))
      {
        return false;
      }

      const std::string readyExecutable = readReadyValue(readyPath, "executable");
      return environment.executablePath.empty()
        || readyExecutable.empty()
        || readyExecutable == environment.executablePath;
    }

    bool readyFileHasValidatedActiveMatchContext(const std::filesystem::path& readyPath)
    {
      std::uint64_t readyProcessId = 0;
      std::uint64_t activeMatchProcessId = 0;
      std::uint64_t residentHeartbeat = 0;
      std::uint64_t activeMatchHeartbeat = 0;
      std::uint64_t activeUnitCount = 0;
      return fileContainsLine(readyPath, "proof.active_match_state=passed")
        && readReadyValue(readyPath, "resident.proof.active_match.source") == "resident"
        && !readReadyValue(readyPath, "resident.proof.active_match.mode").empty()
        && parseUnsignedReadyValue(readyPath, "process_id", readyProcessId)
        && readyProcessId > 0
        && parseUnsignedReadyValue(
          readyPath,
          "resident.proof.active_match.process_id",
          activeMatchProcessId)
        && activeMatchProcessId == readyProcessId
        && parseUnsignedReadyValue(
          readyPath,
          "resident.adapter.heartbeat",
          residentHeartbeat)
        && residentHeartbeat > 0
        && parseUnsignedReadyValue(
          readyPath,
          "resident.proof.active_match.heartbeat",
          activeMatchHeartbeat)
        && activeMatchHeartbeat == residentHeartbeat
        && parseUnsignedReadyValue(
          readyPath,
          "resident.proof.active_match.unit_activity_count",
          activeUnitCount)
        && activeUnitCount > 0;
    }

    bool readyFileHasCommandSpecificEvidenceContext(
      const std::filesystem::path& readyPath,
      const std::string& expectedKind,
      const RuntimeResidentStateProofValidationResult& residentStateProof)
    {
      if (readReadyValue(readyPath, "executor") != "starcraft-api-resident-adapter"
          || readReadyValue(readyPath, "resident.adapter") != "active"
          || readReadyValue(readyPath, "resident.adapter.abi") != RuntimeResidentAdapterAbi
          || !fileContainsLine(readyPath, RuntimeExecutorBridgeActiveCommandReceiverLine)
          || !fileContainsLine(readyPath, RuntimeExecutorBridgeRuntimeCommandQueueSinkLine)
          || !fileContainsLine(readyPath, "proof.attach=passed")
          || !fileContainsLine(readyPath, "proof.read_game_state=passed")
          || !readyFileHasValidatedActiveMatchContext(readyPath))
      {
        return false;
      }
      if (!residentStateProof.readGameStateValid
          || !residentStateProof.activeMatchValid
          || residentStateProof.samples.empty())
      {
        return false;
      }

      return expectedKind != "unit-command"
        || fileContainsLine(readyPath, "proof.read_units=passed");
    }

    std::string commandSpecificProofLine(const std::string& commandName)
    {
      return "proof.issue_commands.command." + commandName + "=passed";
    }

    std::string commandSpecificSnapshotKey(const std::string& commandName)
    {
      return "proof.issue_commands.command." + commandName + ".snapshot";
    }

    bool validatedCommandSpecificSnapshotPayload(
      const std::filesystem::path& snapshot,
      const std::string& commandName,
      const std::string& expectedKind)
    {
      const SnapshotFieldValueTable fields = readSnapshotFieldValueTable(snapshot);
      if (!fields.headerValid
          || fields.hasDuplicateKey
          || fields.hasMalformedRow
          || fields.rowCount == 0)
      {
        return false;
      }

      std::uint64_t encodedBytes = 0;
      std::uint64_t attemptCount = 0;
      std::uint64_t appendedBytes = 0;
      std::uint64_t behaviorSampleCount = 0;
      std::uint64_t queueAddress = 0;
      std::uint64_t bytesInQueueAddress = 0;
      std::uint64_t frameCounterAddress = 0;
      std::uint64_t unitId = 0;
      return snapshotFieldValueIsTrue(fields, "passed")
        && snapshotFieldValueIsTrue(fields, "delivery_checked")
        && snapshotFieldValueIsTrue(fields, "behavior_checked")
        && snapshotFieldValueIsTrue(fields, "receiver_active")
        && snapshotFieldValueIsTrue(fields, "behavior_observed")
        && snapshotFieldValueIsFalse(fields, "self_fixture")
        && snapshotFieldValue(fields, "command") == commandName
        && snapshotFieldValue(fields, "command_kind") == expectedKind
        && snapshotFieldValue(fields, "live_behavior_witness")
          == "starcraft-runtime-adapter-proof-command-behavior-v1"
        && snapshotFieldValue(fields, "storage_kind") == "live-sc-r-command-queue-v1"
        && snapshotFieldValue(fields, "issue_commands_required_adapter_abi")
          == "starcraft-api-resident-adapter-v1"
        && snapshotFieldValue(fields, "issue_commands_required_adapter_location")
          == "in-process-target-runtime"
        && snapshotFieldValue(fields, "issue_commands_required_adapter_thread_policy")
          == "execute-on-target-runtime-thread"
        && snapshotFieldValue(fields, "issue_commands_required_adapter_behavior")
          == "encoded-bwapi-command-reaches-live-scr-command-path-and-changes-frame-behavior"
        && snapshotFieldValue(fields, "issue_commands_required_adapter_promotion_rule")
          == "promote-only-this-command-name-from-this-snapshot"
        && snapshotFieldUnsignedValue(fields, "encoded_bytes", encodedBytes)
        && encodedBytes > 0
        && snapshotFieldUnsignedValue(fields, "attempt_count", attemptCount)
        && attemptCount > 0
        && snapshotFieldUnsignedValue(fields, "appended_bytes", appendedBytes)
        && appendedBytes > 0
        && snapshotFieldUnsignedValue(fields, "behavior_sample_count", behaviorSampleCount)
        && behaviorSampleCount > 0
        && snapshotFieldUnsignedValue(fields, "vector_address", queueAddress)
        && queueAddress > 0
        && snapshotFieldUnsignedValue(fields, "bytes_in_queue_address", bytesInQueueAddress)
        && bytesInQueueAddress > 0
        && snapshotFieldUnsignedValue(fields, "frame_counter_address", frameCounterAddress)
        && frameCounterAddress > 0
        && (expectedKind != "unit-command"
            || (snapshotFieldUnsignedValue(fields, "unit_id", unitId) && unitId > 0));
    }

    bool validatedCommandSpecificSnapshotMetadata(
      const std::filesystem::path& snapshot,
      const std::filesystem::path& readyPath,
      const std::string& commandName,
      const std::string& expectedKind,
      const RuntimeResidentStateProofValidationResult& residentStateProof)
    {
      const SnapshotMetadata metadata = readSnapshotMetadata(snapshot);
      if (metadata.hasDuplicateKey)
        return false;
      if (snapshotMetadataValue(metadata, "schema") != "starcraft-api.resident-snapshot.v1")
        return false;
      if (snapshotMetadataValue(metadata, "proof") != "issue_commands.command")
        return false;
      if (snapshotMetadataValue(metadata, "source_identity") != "resident-adapter")
        return false;
      if (snapshotMetadataValue(metadata, "command") != commandName)
        return false;
      if (snapshotMetadataValue(metadata, "command_kind") != expectedKind)
        return false;
      if (snapshotMetadataValue(metadata, "active_match_correlated") != "true")
        return false;

      std::uint64_t snapshotProcessId = 0;
      std::uint64_t residentProcessId = 0;
      if (!parseUnsignedMetadataValue(metadata, "process_id", snapshotProcessId)
          || !parseUnsignedReadyValue(readyPath, "resident.adapter.process_id", residentProcessId)
          || snapshotProcessId == 0
          || snapshotProcessId != residentProcessId)
      {
        return false;
      }

      std::uint64_t snapshotHeartbeat = 0;
      std::uint64_t residentHeartbeat = 0;
      if (!parseUnsignedMetadataValue(metadata, "heartbeat", snapshotHeartbeat)
          || !parseUnsignedReadyValue(readyPath, "resident.adapter.heartbeat", residentHeartbeat)
          || snapshotHeartbeat == 0
          || snapshotHeartbeat != residentHeartbeat)
      {
        return false;
      }

      std::uint64_t frameId = 0;
      if (!parseUnsignedMetadataValue(metadata, "frame_id", frameId) || frameId == 0)
        return false;
      if (residentStateProof.samples.empty())
        return false;
      const std::uint64_t firstFrame = residentStateProof.samples.front().frame;
      const std::uint64_t lastFrame = residentStateProof.samples.back().frame;
      return frameId >= firstFrame && frameId <= lastFrame;
    }

    bool commandEvidenceProofIsProven(
      const std::filesystem::path& readyPath,
      const RuntimeExecutorPreflightResult& preflight,
      const RuntimeEnvironment& environment,
      const RuntimeResidentStateProofValidationResult& residentStateProof,
      const std::string& detail,
      const std::string& commandName,
      const std::string& expectedKind)
    {
      // Aggregate proof lines such as proof.issue_commands=passed are not
      // command-specific evidence. A live row may promote only the exact command
      // whose command-specific behavior snapshot has been validated.
      if (!preflightSupportsCommandSpecificEvidence(preflight, expectedKind))
        return false;
      if (!readyFileMatchesEnvironmentProcess(readyPath, environment))
        return false;
      if (!readyFileHasCommandSpecificEvidenceContext(readyPath, expectedKind, residentStateProof))
        return false;

      const std::string proofLine = commandSpecificProofLine(commandName);
      if (detail != proofLine || !fileContainsLine(readyPath, proofLine))
        return false;

      std::filesystem::path snapshot;
      if (!readySnapshotPath(readyPath, commandSpecificSnapshotKey(commandName), snapshot))
        return false;

      if (!validatedCommandSpecificSnapshotMetadata(
            snapshot,
            readyPath,
            commandName,
            expectedKind,
            residentStateProof))
        return false;
      if (!validatedCommandSpecificSnapshotPayload(snapshot, commandName, expectedKind))
        return false;
      return true;
    }

    bool allAsciiDigits(const std::string& value)
    {
      return !value.empty()
        && std::all_of(
          value.begin(),
          value.end(),
          [](unsigned char c) { return std::isdigit(c) != 0; });
    }

    bool parseBridgeLiveCommandEvidenceValue(
      const std::string& value,
      RuntimeCommandEvidence& evidence)
    {
      const std::size_t first = value.find('|');
      const std::size_t second = first == std::string::npos
        ? std::string::npos
        : value.find('|', first + 1);
      if (first == std::string::npos
          || second == std::string::npos
          || value.find('|', second + 1) != std::string::npos)
      {
        return false;
      }

      evidence.name = value.substr(0, first);
      evidence.detail = value.substr(second + 1);
      RuntimeCommandEvidenceStatus status = RuntimeCommandEvidenceStatus::Unknown;
      if (evidence.name.empty()
          || evidence.detail.empty()
          || !parseRuntimeCommandEvidenceStatus(value.substr(first + 1, second - first - 1), status))
      {
        return false;
      }
      evidence.status = status;
      return true;
    }

    bool expectedCommandEvidenceName(
      const std::vector<std::string>& expectedNames,
      const std::string& name)
    {
      return std::find(expectedNames.begin(), expectedNames.end(), name) != expectedNames.end();
    }

    std::vector<RuntimeCommandEvidence> readProofBackedLiveCommandEvidence(
      const std::filesystem::path& readyPath,
      const std::string& keyPrefix,
      const std::vector<std::string>& expectedNames,
      const std::string& expectedKind,
      const RuntimeEnvironment& environment,
      const RuntimeExecutorPreflightResult& preflight,
      const RuntimeResidentStateProofValidationResult& residentStateProof)
    {
      std::vector<RuntimeCommandEvidence> result;
      std::unordered_set<std::string> seenNames;
      std::ifstream input(readyPath);
      std::string line;
      while (std::getline(input, line))
      {
        const std::size_t separator = line.find('=');
        if (separator == std::string::npos)
          continue;

        const std::string key = line.substr(0, separator);
        if (key.rfind(keyPrefix, 0) != 0)
          continue;
        if (!allAsciiDigits(key.substr(keyPrefix.size())))
          return {};

        RuntimeCommandEvidence evidence;
        if (!parseBridgeLiveCommandEvidenceValue(line.substr(separator + 1), evidence)
            || evidence.status != RuntimeCommandEvidenceStatus::LiveProven
            || !expectedCommandEvidenceName(expectedNames, evidence.name)
            || !commandEvidenceProofIsProven(
              readyPath,
              preflight,
              environment,
              residentStateProof,
              evidence.detail,
              evidence.name,
              expectedKind)
            || !seenNames.insert(evidence.name).second)
        {
          return {};
        }
        result.push_back(std::move(evidence));
      }
      return result;
    }

    void mergeLiveCommandEvidence(
      std::vector<RuntimeCommandEvidence>& target,
      const std::vector<RuntimeCommandEvidence>& liveEvidence)
    {
      for (const RuntimeCommandEvidence& live : liveEvidence)
      {
        auto it = std::find_if(
          target.begin(),
          target.end(),
          [&](const RuntimeCommandEvidence& existing)
          {
            return existing.name == live.name;
          });
        if (it != target.end())
          *it = live;
      }
    }

    void applyBridgeLiveCommandEvidence(
      RuntimeProbeResult& result,
      const RuntimeEnvironment& environment,
      const RuntimeExecutorPreflightResult& preflight)
    {
      if (environment.executorBridgePath.empty()
          || preflight.executorBridgeMode != RuntimeExecutorBridgeValidatedAdapterMode)
      {
        return;
      }

      std::error_code error;
      const std::filesystem::path readyPath =
        std::filesystem::path(environment.executorBridgePath) / RuntimeExecutorBridgeReadyFile;
      if (!std::filesystem::is_regular_file(readyPath, error) || error)
        return;
      const RuntimeResidentBridgeValidationResult resident =
        validateRuntimeResidentBridgeReadyFile(environment, readyPath);
      const RuntimeResidentStateProofValidationResult residentStateProof =
        validateRuntimeResidentStateProofs(environment, readyPath, resident);

      mergeLiveCommandEvidence(
        result.implementedUnitCommandEvidence,
        readProofBackedLiveCommandEvidence(
          readyPath,
          "command_surface.live_unit_command.",
          result.implementedUnitCommands,
          "unit-command",
          environment,
          preflight,
          residentStateProof));
      mergeLiveCommandEvidence(
        result.implementedGameActionEvidence,
        readProofBackedLiveCommandEvidence(
          readyPath,
          "command_surface.live_game_action.",
          result.implementedGameActions,
          "game-action",
          environment,
          preflight,
          residentStateProof));
    }
  }

  RemasteredRuntimeBackend::RemasteredRuntimeBackend(RuntimeEnvironment environment)
    : environment_(std::move(environment))
  {
  }

  const char* RemasteredRuntimeBackend::name() const
  {
    return "starcraft-remastered-runtime";
  }

  RuntimeEnvironment RemasteredRuntimeBackend::environment() const
  {
    return environment_;
  }

  RuntimeProbeResult RemasteredRuntimeBackend::probe() const
  {
    RuntimeProbeResult result;
    RuntimeContract contract = makeRemasteredParityContract(environment_.version.empty() ? "unknown" : environment_.version);
    bool manifestLoaded = false;
    if (!environment_.manifestPath.empty())
    {
      RuntimeManifestLoadResult manifest = loadRuntimeManifestFile(environment_.manifestPath);
      if (manifest.loaded)
      {
        manifestLoaded = true;
        contract = manifest.manifest.contract;
        result.capabilities = manifest.manifest.capabilities;
        result.implementedUnitCommands = manifest.manifest.unitCommands;
        result.implementedGameActions = manifest.manifest.gameActions;
        result.implementedUnitCommandEvidence =
          staticManifestCommandEvidence(manifest.manifest.unitCommandEvidence);
        result.implementedGameActionEvidence =
          staticManifestCommandEvidence(manifest.manifest.gameActionEvidence);
        result.implementedApiSurfaceMethods = manifest.manifest.implementedApiSurfaceMethods;
        result.implementedCommandSurfaceEntries = manifest.manifest.implementedCommandSurfaceEntries;
      }
    }
    if (!manifestLoaded)
      addBuiltInApiSurface(result, contract);
    if (!manifestLoaded)
      addBuiltInCommandSurface(result);
    contract = applyRuntimeExecutorBridgeContractProofs(environment_, contract);

    const RuntimeMemoryAccessResult memoryAccess = openProcessMemoryAccess(environment_.processId);
    if (memoryAccess.accessible)
      addCapabilityIfMissing(result, Capability::SharedMemoryClient);

    RuntimeExecutorPreflightResult preflight = preflightRuntimeExecutor(environment_, contract);
    addValidatedBridgeCapabilities(result, preflight);
    applyBridgeLiveCommandEvidence(result, environment_, preflight);

    result.supported = true;
    result.supported = canClaimProductionSupport(result, contract)
      && preflight.executorAvailable
      && (!contractRequiresCapability(contract, Capability::SharedMemoryClient) || preflight.memoryAccessible)
      && preflight.errors.empty();
    if (!result.supported)
      result.reason = unsupportedReason(environment_);
    return result;
  }

  RuntimeOpenResult RemasteredRuntimeBackend::open()
  {
    RuntimeOpenResult result;
    RuntimeProbeResult probeResult = probe();
    if (!probeResult.supported)
    {
      state_ = RuntimeSessionState::Failed;
      result.opened = false;
      result.state = state_;
      result.reason = probeResult.reason.empty() ? unsupportedReason(environment_) : probeResult.reason;
      return result;
    }

    state_ = RuntimeSessionState::Open;
    result.opened = true;
    result.state = state_;
    result.reason = "StarCraft Remastered runtime backend opened";
    return result;
  }

  void RemasteredRuntimeBackend::close()
  {
    state_ = RuntimeSessionState::Closed;
  }

  RuntimeSessionState RemasteredRuntimeBackend::state() const
  {
    return state_;
  }
}
