#include <BWAPI/Runtime/RuntimeExecutor.h>
#include <BWAPI/Runtime/RuntimeCommandEncoder.h>
#include <BWAPI/Runtime/RuntimeManifest.h>
#include <BWAPI/Runtime/RuntimeProcess.h>
#include <BWAPI/Runtime/RuntimeProcessMemory.h>
#include <BWAPI/Runtime/RuntimeResidentBridge.h>

#include <algorithm>
#include <chrono>
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

    std::size_t readyKeyCount(const std::filesystem::path& path, const std::string& key)
    {
      std::ifstream input(path);
      const std::string prefix = key + '=';
      std::size_t count = 0;
      std::string line;
      while (std::getline(input, line))
      {
        if (line.rfind(prefix, 0) == 0)
          ++count;
      }
      return count;
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

    bool startsWith(const std::string& value, const char* prefix)
    {
      return value.rfind(prefix, 0) == 0;
    }

    bool productionEvidenceAllowed(const std::string& evidence)
    {
      if (evidence.empty())
        return false;
      for (const char* prefix : {
             "fixture:",
             "unit-test:",
             "mock:",
             "self-fixture:",
             "diagnostic.",
             "static-anchor:",
             "scr-platform-anchor:",
             "static-layout:" })
      {
        if (startsWith(evidence, prefix))
          return false;
      }
      return startsWith(evidence, "proof.");
    }

    std::string evidenceProofLine(const std::string& evidence)
    {
      if (!startsWith(evidence, "proof."))
        return {};

      const std::size_t separator = evidence.find(':');
      if (separator == std::string::npos)
        return evidence;
      return evidence.substr(0, separator);
    }

    bool readyFileHasValidatedProductionProof(
      const std::filesystem::path& readyPath,
      const std::string& proofLine,
      const RuntimeResidentStateProofValidationResult* residentStateProof);

    bool readyFileContainsEvidenceProof(
      const std::filesystem::path& readyPath,
      const std::string& evidence,
      const RuntimeResidentStateProofValidationResult* residentStateProof = nullptr)
    {
      const std::string proofLine = evidenceProofLine(evidence);
      if (proofLine.empty() || !fileContainsLine(readyPath, proofLine))
        return false;
      return readyFileHasValidatedProductionProof(
        readyPath,
        proofLine,
        residentStateProof);
    }

    bool proofLineMatches(const std::string& evidence, const char* expectedProofLine)
    {
      return evidenceProofLine(evidence) == expectedProofLine;
    }

    bool parseUnsignedReadyValue(
      const std::filesystem::path& readyPath,
      const std::string& key,
      std::uint64_t& value)
    {
      const std::string text = readReadyValue(readyPath, key);
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

    bool readyValueIsNonZeroUnsigned(
      const std::filesystem::path& readyPath,
      const std::string& key)
    {
      std::uint64_t value = 0;
      return parseUnsignedReadyValue(readyPath, key, value) && value != 0;
    }

    bool readyValueIsTrue(
      const std::filesystem::path& readyPath,
      const std::string& key)
    {
      return readReadyValue(readyPath, key) == "true";
    }

    bool readySnapshotExists(
      const std::filesystem::path& readyPath,
      const std::string& key)
    {
      const std::string value = readReadyValue(readyPath, key);
      if (value.empty())
        return false;
      std::filesystem::path snapshot(value);
      if (!snapshot.is_absolute())
        snapshot = readyPath.parent_path() / snapshot;
      std::error_code error;
      if (!std::filesystem::is_regular_file(snapshot, error) || error)
        return false;
      const std::uintmax_t size = std::filesystem::file_size(snapshot, error);
      return !error && size > 0;
    }

    bool validatedReadUnitsProof(
      const std::filesystem::path& readyPath,
      const RuntimeResidentStateProofValidationResult* residentStateProof)
    {
      if (residentStateProof == nullptr || !residentStateProof->activeMatchValid)
        return false;
      return readyValueIsNonZeroUnsigned(readyPath, "proof.read_units.address")
        && readyValueIsNonZeroUnsigned(readyPath, "proof.read_units.record_size")
        && readyValueIsNonZeroUnsigned(readyPath, "proof.read_units.active_records");
    }

    bool validatedReadPlayerDataProof(const std::filesystem::path& readyPath)
    {
      const bool nativePlayerTable =
        readReadyValue(readyPath, "proof.read_player_data.source") == "live-sc-r-player-table"
        && readyValueIsNonZeroUnsigned(readyPath, "proof.read_player_data.players_address")
        && readyValueIsTrue(readyPath, "proof.read_player_data.resources_resolved")
        && readyValueIsTrue(readyPath, "proof.read_player_data.supply_resolved");

      const bool residentProjection =
        readReadyValue(readyPath, "proof.read_player_data.projection_source")
          == "compat-player-projection-v1:unit-snapshot-derived"
        && readyValueIsNonZeroUnsigned(readyPath, "proof.read_player_data.player_count")
        && readyValueIsNonZeroUnsigned(readyPath, "proof.read_player_data.observed_units")
        && readyValueIsTrue(readyPath, "proof.read_player_data.player_info_projection")
        && readyValueIsTrue(readyPath, "proof.read_player_data.alliance_projection");

      return (nativePlayerTable || residentProjection)
        && readyValueIsNonZeroUnsigned(readyPath, "proof.read_player_data.player_info_record_size")
        && readySnapshotExists(readyPath, "proof.read_player_data.snapshot");
    }

    bool validatedReadMapDataProof(const std::filesystem::path& readyPath)
    {
      const std::string source = readReadyValue(readyPath, "proof.read_map_data.source");
      return (source == "live-sc-r-map-tile-array"
          || source == "live-sc-r-map-open-file+resident-tile-projection-v1")
        && readyValueIsNonZeroUnsigned(readyPath, "proof.read_map_data.map_name_address")
        && readyValueIsNonZeroUnsigned(readyPath, "proof.read_map_data.map_tile_array_address")
        && readyValueIsNonZeroUnsigned(readyPath, "proof.read_map_data.tile_count")
        && readySnapshotExists(readyPath, "proof.read_map_data.snapshot");
    }

    bool validatedReadBulletDataProof(const std::filesystem::path& readyPath)
    {
      return readReadyValue(readyPath, "proof.read_bullet_data.source") == "live-sc-r-bullet-table"
        && readyValueIsNonZeroUnsigned(readyPath, "proof.read_bullet_data.address")
        && readyValueIsNonZeroUnsigned(readyPath, "proof.read_bullet_data.record_size")
        && readySnapshotExists(readyPath, "proof.read_bullet_data.snapshot");
    }

    bool validatedReadRegionDataProof(const std::filesystem::path& readyPath)
    {
      return readReadyValue(readyPath, "proof.read_region_data.source") == "live-bwapi-region-graph"
        && readyValueIsNonZeroUnsigned(readyPath, "proof.read_region_data.region_count")
        && readySnapshotExists(readyPath, "proof.read_region_data.snapshot");
    }

    bool validatedDispatchEventsProof(
      const std::filesystem::path& readyPath,
      const RuntimeResidentStateProofValidationResult* residentStateProof)
    {
      if (residentStateProof == nullptr || !residentStateProof->activeMatchValid)
        return false;
      return readyValueIsNonZeroUnsigned(readyPath, "proof.dispatch_events.frame_events")
        && readyValueIsNonZeroUnsigned(readyPath, "proof.dispatch_events.unit_discover_events")
        && readyValueIsNonZeroUnsigned(readyPath, "proof.dispatch_events.unit_update_events")
        && readySnapshotExists(readyPath, "proof.dispatch_events.snapshot");
    }

    bool validatedReplayAnalysisProof(const std::filesystem::path& readyPath)
    {
      const std::string source = readReadyValue(readyPath, "proof.replay_analysis.source");
      const bool parsedReplay =
        source == "parsed-replay-header"
        && readyValueIsTrue(readyPath, "proof.replay_analysis.current_process_replay");
      const bool activeMatchMetadata =
        source == "active-match-live-metadata"
        && readyValueIsTrue(readyPath, "proof.replay_analysis.active_match_metadata");
      return (parsedReplay || activeMatchMetadata)
        && readyValueIsNonZeroUnsigned(readyPath, "proof.replay_analysis.last_frame")
        && readyValueIsNonZeroUnsigned(readyPath, "proof.replay_analysis.player_count")
        && readySnapshotExists(readyPath, "proof.replay_analysis.snapshot");
    }

    bool validatedIssueCommandsProof(const std::filesystem::path& readyPath)
    {
      const std::string storageKind = readReadyValue(readyPath, "proof.issue_commands.storage_kind");
      return readReadyValue(readyPath, "proof.issue_commands.source") == "live-sc-r-command-path"
        && readyValueIsTrue(readyPath, "proof.issue_commands.delivery_checked")
        && readyValueIsTrue(readyPath, "proof.issue_commands.behavior_checked")
        && readReadyValue(readyPath, "proof.issue_commands.self_fixture") != "true"
        && readyValueIsTrue(readyPath, "proof.issue_commands.pause_frame_counter_matched")
        && !readReadyValue(readyPath, "proof.issue_commands.command").empty()
        && readyValueIsNonZeroUnsigned(readyPath, "proof.issue_commands.vector_address")
        && !storageKind.empty()
        && !startsWith(storageKind, "unit-test")
        && !startsWith(storageKind, "mock")
        && !startsWith(storageKind, "self-fixture")
        && readyValueIsNonZeroUnsigned(readyPath, "proof.issue_commands.bytes_in_queue_address")
        && readyValueIsNonZeroUnsigned(readyPath, "proof.issue_commands.frame_counter_address")
        && !readReadyValue(readyPath, "proof.issue_commands.encoded_bytes").empty()
        && readyValueIsTrue(readyPath, "proof.issue_commands.stale_proof_bytes_cleared")
        && readySnapshotExists(readyPath, "proof.issue_commands.snapshot");
    }

    bool validatedDrawOverlaysProof(const std::filesystem::path& readyPath)
    {
      return readReadyValue(readyPath, "proof.draw_overlays.source") == "live-render-hook"
        && readyValueIsNonZeroUnsigned(readyPath, "proof.draw_overlays.hook_address")
        && readySnapshotExists(readyPath, "proof.draw_overlays.snapshot");
    }

    bool validatedMultiplayerSyncProof(const std::filesystem::path& readyPath)
    {
      return readReadyValue(readyPath, "proof.multiplayer_sync.source") == "live-snet-turn-hooks"
        && readyValueIsNonZeroUnsigned(readyPath, "proof.multiplayer_sync.send_turn_address")
        && readyValueIsNonZeroUnsigned(readyPath, "proof.multiplayer_sync.receive_message_address")
        && readySnapshotExists(readyPath, "proof.multiplayer_sync.snapshot");
    }

    bool validatedBattleNetPolicyProof(const std::filesystem::path& readyPath)
    {
      std::uint64_t gameProcessCount = 0;
      std::uint64_t blockerCount = 1;
      return readReadyValue(readyPath, "proof.battle_net_policy.status") == "runtime-process-visible"
        && parseUnsignedReadyValue(
          readyPath,
          "proof.battle_net_policy.game_process_count",
          gameProcessCount)
        && gameProcessCount == 1
        && parseUnsignedReadyValue(
          readyPath,
          "proof.battle_net_policy.blocker_count",
          blockerCount)
        && blockerCount == 0;
    }

    bool validatedLoadAIModulesProof(const std::filesystem::path& readyPath)
    {
      return readReadyValue(readyPath, "proof.load_ai_modules.loader") == "dlopen"
        && readReadyValue(readyPath, "proof.load_ai_modules.self_process_smoke") == "false"
        && !readReadyValue(readyPath, "proof.load_ai_modules.module_path").empty()
        && readySnapshotExists(readyPath, "proof.load_ai_modules.snapshot");
    }

    bool readyFileHasValidatedProductionProof(
      const std::filesystem::path& readyPath,
      const std::string& proofLine,
      const RuntimeResidentStateProofValidationResult* residentStateProof)
    {
      if (proofLine == "proof.attach=passed")
        return readReadyValue(readyPath, "proof.attach.source") == "resident-adapter";
      if (proofLine == "proof.read_game_state=passed")
        return residentStateProof == nullptr || residentStateProof->readGameStateValid;
      if (proofLine == "proof.active_match_state=passed")
        return residentStateProof == nullptr || residentStateProof->activeMatchValid;
      if (proofLine == "proof.read_units=passed")
        return validatedReadUnitsProof(readyPath, residentStateProof);
      if (proofLine == "proof.read_player_data=passed")
        return validatedReadPlayerDataProof(readyPath);
      if (proofLine == "proof.read_map_data=passed")
        return validatedReadMapDataProof(readyPath);
      if (proofLine == "proof.read_bullet_data=passed")
        return validatedReadBulletDataProof(readyPath);
      if (proofLine == "proof.read_region_data=passed")
        return validatedReadRegionDataProof(readyPath);
      if (proofLine == "proof.issue_commands=passed")
        return validatedIssueCommandsProof(readyPath);
      if (proofLine == "proof.draw_overlays=passed")
        return validatedDrawOverlaysProof(readyPath);
      if (proofLine == "proof.dispatch_events=passed")
        return validatedDispatchEventsProof(readyPath, residentStateProof);
      if (proofLine == "proof.replay_analysis=passed")
        return validatedReplayAnalysisProof(readyPath);
      if (proofLine == "proof.multiplayer_sync=passed")
        return validatedMultiplayerSyncProof(readyPath);
      if (proofLine == "proof.battle_net_policy=passed")
        return validatedBattleNetPolicyProof(readyPath);
      if (proofLine == "proof.load_ai_modules=passed")
        return validatedLoadAIModulesProof(readyPath);
      return false;
    }

    bool productionEvidenceAllowedForBinding(
      const std::string& name,
      BindingKind kind,
      const std::string& evidence)
    {
      if (!productionEvidenceAllowed(evidence))
        return false;

      switch (kind)
      {
      case BindingKind::DataAddress:
        if (name == "BW::BWDATA::Game")
          return proofLineMatches(evidence, "proof.read_game_state=passed");
        if (name == "BW::BWDATA::Players")
          return proofLineMatches(evidence, "proof.read_player_data=passed");
        if (name == "BW::BWDATA::UnitNodeTable")
          return proofLineMatches(evidence, "proof.read_units=passed");
        if (name == "BW::BWDATA::BulletNodeTable")
          return proofLineMatches(evidence, "proof.read_bullet_data=passed");
        if (name == "BW::BWDATA::MapTileArray")
          return proofLineMatches(evidence, "proof.read_map_data=passed");
        return false;
      case BindingKind::FunctionAddress:
        return name == "BW::BWFXN_ExecuteGameTriggers"
          && proofLineMatches(evidence, "proof.dispatch_events=passed");
      case BindingKind::ImportedFunction:
        return (name == "Storm::SNetReceiveMessage" || name == "Storm::SNetSendTurn")
          && proofLineMatches(evidence, "proof.multiplayer_sync=passed");
      case BindingKind::HookPoint:
        return name == "draw-game-layer-hook"
          && proofLineMatches(evidence, "proof.draw_overlays=passed");
      case BindingKind::CommandQueue:
        return (name == "BW::BWDATA::sgdwBytesInCmdQueue" || name == "BW::BWDATA::TurnBuffer")
          && proofLineMatches(evidence, "proof.issue_commands=passed");
      case BindingKind::Transport:
        if (name == "ai-module-loader")
          return proofLineMatches(evidence, "proof.load_ai_modules=passed");
        if (name == "shared-memory-client-transport")
          return proofLineMatches(evidence, "proof.attach=passed");
        return false;
      case BindingKind::StructureLayout:
        return false;
      }
      return false;
    }

    bool productionEvidenceAllowedForStructure(
      const std::string& name,
      const std::string& evidence)
    {
      if (!productionEvidenceAllowed(evidence))
        return false;
      if (name == "BW::BWGame")
        return proofLineMatches(evidence, "proof.read_game_state=passed");
      if (name == "BW::CUnit")
        return proofLineMatches(evidence, "proof.read_units=passed");
      if (name == "BW::CBullet")
        return proofLineMatches(evidence, "proof.read_bullet_data=passed");
      if (name == "BW::PlayerInfo")
        return proofLineMatches(evidence, "proof.read_player_data=passed");
      if (name == "BW::ReplayHeader")
        return proofLineMatches(evidence, "proof.replay_analysis=passed");
      return false;
    }

    bool productionEvidenceAllowedForField(
      const std::string& structureName,
      const std::string& fieldName,
      const std::string& evidence)
    {
      if (!productionEvidenceAllowed(evidence))
        return false;
      if (structureName == "BW::BWGame")
      {
        if (fieldName == "players" || fieldName == "alliance")
          return proofLineMatches(evidence, "proof.read_player_data=passed");
        if (fieldName == "elapsedFrames")
          return proofLineMatches(evidence, "proof.read_game_state=passed");
        return false;
      }
      if (structureName == "BW::CUnit")
        return proofLineMatches(evidence, "proof.read_units=passed");
      if (structureName == "BW::CBullet")
        return proofLineMatches(evidence, "proof.read_bullet_data=passed");
      if (structureName == "BW::PlayerInfo")
        return proofLineMatches(evidence, "proof.read_player_data=passed");
      if (structureName == "BW::ReplayHeader")
        return proofLineMatches(evidence, "proof.replay_analysis=passed");
      return false;
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
      return productionEvidenceAllowedForBinding(name, kind, evidence)
        && readyFileContainsEvidenceProof(readyPath, evidence);
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
      if (!productionEvidenceAllowedForBinding(name, BindingKind::CommandQueue, evidence)
          || !readyFileContainsEvidenceProof(readyPath, evidence))
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

    std::filesystem::path resolveBridgeRelativePath(
      const std::filesystem::path& readyPath,
      const std::string& value)
    {
      std::filesystem::path path(value);
      if (path.is_absolute())
        return path;
      return readyPath.parent_path() / path;
    }

    bool readResidentCommandQueueSink(
      const std::filesystem::path& readyPath,
      std::filesystem::path& queuePath,
      std::vector<std::string>* errors = nullptr)
    {
      if (!fileContainsLine(readyPath, "proof.attach=passed"))
        return false;

      const std::string queuePathValue =
        readReadyValue(readyPath, "resident.queue.command.path");
      if (queuePathValue.empty())
        return false;

      queuePath = resolveBridgeRelativePath(readyPath, queuePathValue);
      RuntimeResidentQueueValidationResult queue =
        validateRuntimeResidentQueueFile(queuePath, RuntimeResidentQueueKind::Command);
      if (!queue.valid)
      {
        if (errors != nullptr)
          errors->insert(errors->end(), queue.errors.begin(), queue.errors.end());
        return false;
      }
      return true;
    }

    bool readyFileHasResidentCommandQueueSink(const std::filesystem::path& readyPath)
    {
      std::filesystem::path queuePath;
      return readResidentCommandQueueSink(readyPath, queuePath);
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

    bool bridgeReadyFileHasDuplicateIdentityKeys(
      const std::filesystem::path& readyPath,
      std::string& duplicateKey)
    {
      for (const char* key : { "protocol", "product", "version", "mode", "process_id", "executable" })
      {
        if (readyKeyCount(readyPath, key) > 1)
        {
          duplicateKey = key;
          return true;
        }
      }
      return false;
    }

    std::filesystem::path residentHeartbeatStatePath(const std::filesystem::path& readyPath)
    {
      return readyPath.parent_path() / ".resident-heartbeat-state";
    }

    bool readyFileModifiedMs(const std::filesystem::path& readyPath, long long& modifiedMs)
    {
      std::error_code error;
      const std::filesystem::file_time_type modified =
        std::filesystem::last_write_time(readyPath, error);
      if (error)
        return false;
      modifiedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        modified.time_since_epoch()).count();
      return true;
    }

    bool readResidentHeartbeatState(
      const std::filesystem::path& path,
      int& processId,
      std::uint64_t& heartbeat,
      long long& readyModifiedMs)
    {
      std::ifstream input(path);
      if (!input)
        return false;

      std::string line;
      while (std::getline(input, line))
      {
        const std::size_t separator = line.find('=');
        if (separator == std::string::npos)
          continue;
        const std::string key = line.substr(0, separator);
        const std::string value = line.substr(separator + 1);
        try
        {
          if (key == "process_id")
            processId = std::stoi(value);
          else if (key == "heartbeat")
            heartbeat = static_cast<std::uint64_t>(std::stoull(value));
          else if (key == "ready_mtime_ms")
            readyModifiedMs = std::stoll(value);
        }
        catch (...)
        {
          return false;
        }
      }
      return processId > 0 && heartbeat > 0;
    }

    void writeResidentHeartbeatState(
      const std::filesystem::path& path,
      const RuntimeResidentBridgeValidationResult& resident,
      long long readyModifiedMs)
    {
      std::ofstream output(path);
      if (!output)
        return;
      output << "process_id=" << resident.processId << '\n';
      output << "heartbeat=" << resident.heartbeat << '\n';
      output << "ready_mtime_ms=" << readyModifiedMs << '\n';
    }

    bool validateResidentHeartbeatProgression(
      const std::filesystem::path& readyPath,
      const RuntimeResidentBridgeValidationResult& resident,
      std::vector<std::string>& errors)
    {
      if (!resident.present || !resident.valid)
        return true;

      long long readyModifiedMs = 0;
      if (!readyFileModifiedMs(readyPath, readyModifiedMs))
        return true;

      int previousProcessId = 0;
      std::uint64_t previousHeartbeat = 0;
      long long previousReadyModifiedMs = 0;
      const std::filesystem::path statePath = residentHeartbeatStatePath(readyPath);
      if (readResidentHeartbeatState(
            statePath,
            previousProcessId,
            previousHeartbeat,
            previousReadyModifiedMs)
          && previousProcessId == resident.processId
          && readyModifiedMs > previousReadyModifiedMs
          && resident.heartbeat <= previousHeartbeat)
      {
        errors.push_back("resident adapter heartbeat did not advance after ready file update");
        return false;
      }

      if (resident.heartbeat > previousHeartbeat || readyModifiedMs > previousReadyModifiedMs)
        writeResidentHeartbeatState(statePath, resident, readyModifiedMs);
      return true;
    }

    void applyBindingProof(
      RuntimeContract& contract,
      const std::filesystem::path& readyPath,
      const std::string& name,
      const std::string& value,
      const RuntimeResidentStateProofValidationResult* residentStateProof)
    {
      const std::vector<std::string> parts = splitPipe(value);
      if (parts.size() != 2 || parts[1].empty())
        return;

      BindingKind kind = BindingKind::DataAddress;
      if (!parseBindingKind(parts[0], kind))
        return;
      if (!productionEvidenceAllowedForBinding(name, kind, parts[1])
          || !readyFileContainsEvidenceProof(readyPath, parts[1], residentStateProof))
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

    void applyStructureProof(
      RuntimeContract& contract,
      const std::filesystem::path& readyPath,
      const std::string& name,
      const std::string& value,
      const RuntimeResidentStateProofValidationResult* residentStateProof)
    {
      const std::vector<std::string> parts = splitPipe(value);
      if (parts.size() != 2 || parts[1].empty())
        return;
      if (!productionEvidenceAllowedForStructure(name, parts[1])
          || !readyFileContainsEvidenceProof(readyPath, parts[1], residentStateProof))
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
      it->evidence = parts[1];
    }

    void applyFieldProof(
      RuntimeContract& contract,
      const std::filesystem::path& readyPath,
      const std::string& name,
      const std::string& value,
      const RuntimeResidentStateProofValidationResult* residentStateProof)
    {
      const std::size_t separator = name.rfind('.');
      if (separator == std::string::npos || separator == 0 || separator + 1 >= name.size())
        return;

      const std::string structureName = name.substr(0, separator);
      const std::string fieldName = name.substr(separator + 1);
      const std::vector<std::string> parts = splitPipe(value);
      if (parts.size() != 3 || parts[2].empty())
        return;
      if (!productionEvidenceAllowedForField(structureName, fieldName, parts[2])
          || !readyFileContainsEvidenceProof(readyPath, parts[2], residentStateProof))
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
      fieldIt->evidence = parts[2];
    }

    bool validateProductionBridgeProof(
      const std::filesystem::path& readyPath,
      RuntimeExecutorPreflightResult& result,
      const RuntimeResidentStateProofValidationResult& residentStateProof,
      bool requireBehaviorProofs = true)
    {
      result.executorBridgeMode = readReadyValue(readyPath, "mode");
      result.missingBehaviorProofs.clear();
      result.provenCapabilities.clear();
      bool residentStateProofErrorsReported = false;
      for (const RuntimeExecutorBehaviorProof& proof : requiredRuntimeExecutorBehaviorProofs())
      {
        if (!fileContainsLine(readyPath, proof.readyFileLine))
        {
          result.missingBehaviorProofs.push_back(proof.readyFileLine);
          continue;
        }

        if (std::string(proof.id) == "read-game-state"
            && !residentStateProof.readGameStateValid)
        {
          result.missingBehaviorProofs.push_back(proof.readyFileLine);
          result.errors.push_back(
            "runtime executor bridge read-game-state proof is missing valid resident frame/tick samples");
          if (!residentStateProofErrorsReported)
          {
            result.errors.insert(
              result.errors.end(),
              residentStateProof.errors.begin(),
              residentStateProof.errors.end());
            residentStateProofErrorsReported = true;
          }
          continue;
        }

        if (std::string(proof.id) == "active-match-state"
            && !residentStateProof.activeMatchValid)
        {
          result.missingBehaviorProofs.push_back(proof.readyFileLine);
          result.errors.push_back(
            "runtime executor bridge active-match-state proof is missing valid resident match activity");
          if (!residentStateProofErrorsReported)
          {
            result.errors.insert(
              result.errors.end(),
              residentStateProof.errors.begin(),
              residentStateProof.errors.end());
            residentStateProofErrorsReported = true;
          }
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

        if (!readyFileHasValidatedProductionProof(
              readyPath,
              proof.readyFileLine,
              &residentStateProof))
        {
          result.missingBehaviorProofs.push_back(proof.readyFileLine);
          result.errors.push_back(
            std::string("runtime executor bridge proof is missing validated production metadata: ")
            + proof.readyFileLine);
          continue;
        }

        addCapabilityIfMissing(result.provenCapabilities, proof.capability);
      }
      if (readyFileHasValidatedProductionProof(
            readyPath,
            "proof.read_map_data=passed",
            &residentStateProof))
        addCapabilityIfMissing(result.provenCapabilities, Capability::ReadMapData);
      if (readyFileHasValidatedProductionProof(
            readyPath,
            "proof.read_player_data=passed",
            &residentStateProof))
        addCapabilityIfMissing(result.provenCapabilities, Capability::ReadPlayerData);
      if (readyFileHasValidatedProductionProof(
            readyPath,
            "proof.read_bullet_data=passed",
            &residentStateProof))
        addCapabilityIfMissing(result.provenCapabilities, Capability::ReadBulletData);
      if (readyFileHasValidatedProductionProof(
            readyPath,
            "proof.read_region_data=passed",
            &residentStateProof))
        addCapabilityIfMissing(result.provenCapabilities, Capability::ReadRegionData);
      if (readyFileHasValidatedProductionProof(
            readyPath,
            "proof.load_ai_modules=passed",
            &residentStateProof))
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
      std::string duplicateKey;
      if (bridgeReadyFileHasDuplicateIdentityKeys(readyPath, duplicateKey))
      {
        result.errors.push_back(
          "runtime executor bridge ready file has duplicate identity key: " + duplicateKey);
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

      RuntimeResidentBridgeValidationResult resident =
        validateRuntimeResidentBridgeReadyFile(environment, readyPath);
      if (resident.present && !resident.valid)
      {
        for (const std::string& residentError : resident.errors)
          result.errors.push_back(residentError);
        return true;
      }
      if (!validateResidentHeartbeatProgression(readyPath, resident, result.errors))
        return true;
      RuntimeResidentStateProofValidationResult residentStateProof =
        validateRuntimeResidentStateProofs(environment, readyPath, resident);

      if (!validateProductionBridgeProof(readyPath, result, residentStateProof, false))
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
      if (!readyFileHasRuntimeCommandQueueSink(readyPath)
          && !readyFileHasResidentCommandQueueSink(readyPath))
        missing.push_back("active or direct runtime command queue sink");

      if (missing.empty()
          || (missing.size() == 1
              && missing.front() == "proof.issue_commands=passed"
              && readyFileHasResidentCommandQueueSink(readyPath)))
      {
        if (!missing.empty())
        {
          result.warnings.push_back(
            "runtime commands can be enqueued to the resident command ingress, "
            "but live SC:R command execution proof.issue_commands=passed is still missing");
        }
        return true;
      }

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

    bool submitResidentRuntimeCommands(
      const RuntimeEnvironment& environment,
      const std::filesystem::path& queuePath,
      const std::vector<RuntimeCommandRequest>& commands,
      RuntimeExecutorSubmitResult& result)
    {
      std::size_t submitted = 0;
      for (const RuntimeCommandRequest& command : commands)
      {
        const std::string serialized = serializeCommand(command);
        const std::vector<unsigned char> payload(serialized.begin(), serialized.end());
        RuntimeResidentQueueAppendResult appended =
          appendRuntimeResidentQueueRecord(
            queuePath,
            RuntimeResidentQueueKind::Command,
            payload);
        if (!appended.appended)
        {
          const std::string reason = appended.reason.empty()
            ? "unable to append command to resident command queue"
            : appended.reason;
          return rejectSubmit(result, reason);
        }
        appendDirectCommandAudit(
          environment,
          appended.sequence,
          "resident-enqueued",
          command,
          "",
          "queued-to-resident-command-ingress:" + queuePath.string());
        ++submitted;
      }

      result.submitted = true;
      result.submittedCommands = submitted;
      result.warnings.push_back(
        "runtime commands were enqueued to the resident command ingress; "
        "production issue-command proof still requires observed SC:R behavior");
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
    std::string duplicateKey;
    if (bridgeReadyFileHasDuplicateIdentityKeys(readyPath, duplicateKey))
      return contract;
    if (!bridgeReadyFileMatchesRuntime(environment, readyPath))
      return contract;
    if (!validateBridgeRuntimeIdentity(environment, readyPath).empty())
      return contract;

    RuntimeResidentBridgeValidationResult resident =
      validateRuntimeResidentBridgeReadyFile(environment, readyPath);
    RuntimeResidentStateProofValidationResult residentStateProof =
      validateRuntimeResidentStateProofs(environment, readyPath, resident);

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
        applyBindingProof(
          contract,
          readyPath,
          key.substr(std::char_traits<char>::length(bindingPrefix)),
          value,
          &residentStateProof);
      else if (key.rfind(structurePrefix, 0) == 0)
        applyStructureProof(
          contract,
          readyPath,
          key.substr(std::char_traits<char>::length(structurePrefix)),
          value,
          &residentStateProof);
      else if (key.rfind(fieldPrefix, 0) == 0)
        applyFieldProof(
          contract,
          readyPath,
          key.substr(std::char_traits<char>::length(fieldPrefix)),
          value,
          &residentStateProof);
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
        "read-region-data",
        Capability::ReadRegionData,
        "proof.read_region_data=passed",
        "adapter read BWAPI-compatible region data from live map/unit metadata"
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
    std::string duplicateKey;
    if (bridgeReadyFileHasDuplicateIdentityKeys(readyPath, duplicateKey))
    {
      result.reason = "runtime executor bridge ready file has duplicate identity key: " + duplicateKey;
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

    const RuntimeProcessOpenResult runtimeProcess = openRuntimeProcess(environment);
    if (!runtimeProcess.opened)
    {
      rejectSubmit(
        result,
        runtimeProcess.reason.empty()
          ? "runtime process identity could not be verified before command submission"
          : runtimeProcess.reason);
      return result;
    }

    RuntimeResidentBridgeValidationResult resident =
      validateRuntimeResidentBridgeReadyFile(environment, readyPath);
    if (resident.present && !resident.valid)
    {
      result.reason = resident.errors.empty()
        ? "runtime resident adapter metadata is invalid"
        : resident.errors.front();
      result.errors.insert(result.errors.end(), resident.errors.begin(), resident.errors.end());
      return result;
    }
    if (!validateResidentHeartbeatProgression(readyPath, resident, result.errors))
    {
      result.reason = result.errors.empty()
        ? "runtime resident adapter heartbeat is stale"
        : result.errors.front();
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

    std::filesystem::path residentCommandQueuePath;
    if (readResidentCommandQueueSink(readyPath, residentCommandQueuePath, &result.errors))
    {
      submitResidentRuntimeCommands(environment, residentCommandQueuePath, drained, result);
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
