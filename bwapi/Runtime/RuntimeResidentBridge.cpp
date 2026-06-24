#include <BWAPI/Runtime/RuntimeResidentBridge.h>
#include <BWAPI/Runtime/RuntimeProcessMemory.h>

#include <chrono>
#include <climits>
#include <limits>
#include <fstream>
#include <sstream>
#include <thread>
#include <utility>

namespace BWAPI::Runtime
{
  static_assert(CHAR_BIT == 8, "resident adapter ABI requires 8-bit bytes");
  static_assert(sizeof(RuntimeResidentQueueHeader) == 40, "resident queue header ABI size changed");
  static_assert(sizeof(RuntimeResidentRecordHeader) == 16, "resident record header ABI size changed");

  namespace
  {
    std::string readyValue(const std::filesystem::path& path, const std::string& key)
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

    bool parseUnsigned(const std::string& value, std::uint64_t& parsed)
    {
      try
      {
        std::size_t consumed = 0;
        const unsigned long long number = std::stoull(value, &consumed, 0);
        if (consumed != value.size())
          return false;
        parsed = static_cast<std::uint64_t>(number);
        return static_cast<unsigned long long>(parsed) == number;
      }
      catch (...)
      {
        return false;
      }
    }

    bool parseSignedInt(const std::string& value, int& parsed)
    {
      try
      {
        std::size_t consumed = 0;
        const long number = std::stol(value, &consumed, 0);
        if (consumed != value.size())
          return false;
        parsed = static_cast<int>(number);
        return static_cast<long>(parsed) == number;
      }
      catch (...)
      {
        return false;
      }
    }

    bool parseAddress(const std::string& value, std::uintptr_t& parsed)
    {
      std::uint64_t number = 0;
      if (!parseUnsigned(value, number) || number == 0)
        return false;
      parsed = static_cast<std::uintptr_t>(number);
      return static_cast<std::uint64_t>(parsed) == number;
    }

    std::uint32_t readLittleU32(const std::vector<unsigned char>& bytes)
    {
      if (bytes.size() < sizeof(std::uint32_t))
        return 0;
      return static_cast<std::uint32_t>(bytes[0])
        | (static_cast<std::uint32_t>(bytes[1]) << 8)
        | (static_cast<std::uint32_t>(bytes[2]) << 16)
        | (static_cast<std::uint32_t>(bytes[3]) << 24);
    }

    std::string joinUnsignedSamples(
      const std::vector<RuntimeResidentGameStateSample>& samples,
      bool ticks)
    {
      std::ostringstream output;
      for (std::size_t i = 0; i < samples.size(); ++i)
      {
        if (i > 0)
          output << ',';
        output << (ticks ? samples[i].tick : samples[i].frame);
      }
      return output.str();
    }

    bool parseUnsignedList(const std::string& value, std::vector<std::uint64_t>& parsed)
    {
      parsed.clear();
      if (value.empty())
        return false;

      std::istringstream input(value);
      std::string item;
      while (std::getline(input, item, ','))
      {
        std::uint64_t number = 0;
        if (!parseUnsigned(item, number))
          return false;
        parsed.push_back(number);
      }
      return !parsed.empty();
    }

    bool strictlyIncreasing(const std::vector<std::uint64_t>& values)
    {
      for (std::size_t i = 1; i < values.size(); ++i)
      {
        if (values[i] <= values[i - 1])
          return false;
      }
      return true;
    }

    bool parseProofProcessAndHeartbeat(
      const std::filesystem::path& readyPath,
      const char* prefix,
      const RuntimeResidentBridgeValidationResult& resident,
      std::vector<std::string>& errors)
    {
      int processId = 0;
      std::uint64_t heartbeat = 0;
      bool valid = true;

      if (!parseSignedInt(readyValue(readyPath, std::string(prefix) + ".process_id"), processId)
          || processId <= 0)
      {
        errors.push_back(std::string(prefix) + " process_id is missing or malformed");
        valid = false;
      }
      else if (processId != resident.processId)
      {
        errors.push_back(std::string(prefix) + " process_id does not match the resident adapter");
        valid = false;
      }

      if (!parseUnsigned(readyValue(readyPath, std::string(prefix) + ".heartbeat"), heartbeat))
      {
        errors.push_back(std::string(prefix) + " heartbeat is missing or malformed");
        valid = false;
      }
      else if (heartbeat != resident.heartbeat)
      {
        errors.push_back(std::string(prefix) + " heartbeat does not match the current resident adapter heartbeat");
        valid = false;
      }

      return valid;
    }

    bool validateReadGameStateLiveMemoryProof(
      const std::filesystem::path& readyPath,
      int processId,
      const std::vector<std::uint64_t>& frames,
      std::vector<std::string>& errors)
    {
      std::uintptr_t address = 0;
      if (!parseAddress(readyValue(readyPath, "proof.read_game_state.address"), address))
      {
        errors.push_back("resident read_game_state proof address is missing or malformed");
        return false;
      }

      if (frames.empty() || frames.back() > std::numeric_limits<std::uint32_t>::max())
      {
        errors.push_back("resident read_game_state frame sample is outside u32 counter range");
        return false;
      }

      RuntimeMemoryReadResult read =
        readProcessMemory(processId, address, sizeof(std::uint32_t));
      if (!read.success || read.bytesRead != sizeof(std::uint32_t))
      {
        errors.push_back(
          read.reason.empty()
            ? "resident read_game_state proof address could not be read from the selected process"
            : "resident read_game_state proof address could not be read: " + read.reason);
        return false;
      }

      const std::uint32_t currentFrame = readLittleU32(read.bytes);
      if (currentFrame < static_cast<std::uint32_t>(frames.back()))
      {
        errors.push_back("resident read_game_state proof address no longer contains the sampled frame counter");
        return false;
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(64));
      RuntimeMemoryReadResult nextRead =
        readProcessMemory(processId, address, sizeof(std::uint32_t));
      if (!nextRead.success || nextRead.bytesRead != sizeof(std::uint32_t))
      {
        errors.push_back(
          nextRead.reason.empty()
            ? "resident read_game_state proof address could not be re-sampled from the selected process"
            : "resident read_game_state proof address could not be re-sampled: " + nextRead.reason);
        return false;
      }
      const std::uint32_t nextFrame = readLittleU32(nextRead.bytes);
      if (nextFrame <= currentFrame)
      {
        errors.push_back("resident read_game_state proof address did not advance during live re-sampling");
        return false;
      }
      return true;
    }

    bool validateLiveReadableEvidenceAddress(
      int processId,
      std::uintptr_t address,
      std::size_t bytes,
      const char* description,
      bool requireNonZeroPayload,
      std::vector<std::string>& errors)
    {
      RuntimeMemoryReadResult read = readProcessMemory(processId, address, bytes);
      if (!read.success || read.bytesRead != bytes)
      {
        errors.push_back(
          read.reason.empty()
            ? std::string(description) + " could not be read from the selected process"
            : std::string(description) + " could not be read: " + read.reason);
        return false;
      }
      if (requireNonZeroPayload)
      {
        bool anyNonZero = false;
        for (unsigned char byte : read.bytes)
        {
          if (byte != 0)
          {
            anyNonZero = true;
            break;
          }
        }
        if (!anyNonZero)
        {
          errors.push_back(std::string(description) + " contains only zero bytes");
          return false;
        }
      }
      return true;
    }

    bool validateActiveMatchLiveUnitEvidence(
      const std::filesystem::path& readyPath,
      int processId,
      std::uint64_t activeUnitCount,
      std::vector<std::string>& errors)
    {
      std::uint64_t activeRecords = 0;
      if (!parseUnsigned(readyValue(readyPath, "proof.active_match_state.active_records"), activeRecords)
          || activeRecords < activeUnitCount)
      {
        errors.push_back("resident active_match proof is missing matching active unit record evidence");
        return false;
      }

      const std::string evidence = readyValue(readyPath, "proof.active_match_state.evidence");
      if (evidence != "active-unit-records"
          && evidence != "active-unit-node-snapshot")
      {
        errors.push_back("resident active_match proof is missing supported active unit evidence details");
        return false;
      }
      if (readyValue(readyPath, "proof.read_units") != "passed")
      {
        errors.push_back("resident active_match proof requires validated read_units proof");
        return false;
      }
      std::uint64_t readUnitsActiveRecords = 0;
      if (!parseUnsigned(readyValue(readyPath, "proof.read_units.active_records"), readUnitsActiveRecords)
          || readUnitsActiveRecords != activeRecords)
      {
        errors.push_back("resident active_match proof active record count does not match read_units proof");
        return false;
      }
      std::uintptr_t readUnitsAddress = 0;
      if (!parseAddress(readyValue(readyPath, "proof.read_units.address"), readUnitsAddress))
      {
        errors.push_back("resident active_match proof requires read_units address evidence");
        return false;
      }
      std::uint64_t readUnitsRecordSize = 0;
      if (!parseUnsigned(readyValue(readyPath, "proof.read_units.record_size"), readUnitsRecordSize)
          || readUnitsRecordSize < 16)
      {
        errors.push_back("resident active_match proof requires read_units record-size evidence");
        return false;
      }

      std::uintptr_t readStateAddress = 0;
      parseAddress(readyValue(readyPath, "proof.read_game_state.address"), readStateAddress);
      std::uintptr_t address = 0;
      if (evidence == "active-unit-records")
      {
        if (!parseAddress(readyValue(readyPath, "proof.active_match_state.unit_array_address"), address))
        {
          errors.push_back("resident active_match proof is missing unit array address evidence");
          return false;
        }
        if (address != readUnitsAddress)
        {
          errors.push_back("resident active_match unit array evidence does not match read_units address");
          return false;
        }
        if (address == readStateAddress)
        {
          errors.push_back("resident active_match unit array evidence reuses the read_game_state counter address");
          return false;
        }
        return validateLiveReadableEvidenceAddress(
          processId,
          address,
          64,
          "resident active_match unit array evidence address",
          true,
          errors);
      }

      if (!parseAddress(readyValue(readyPath, "proof.active_match_state.unit_node_address"), address))
      {
        errors.push_back("resident active_match proof is missing unit-node address evidence");
        return false;
      }
      if (address == readStateAddress)
      {
        errors.push_back("resident active_match unit-node evidence reuses the read_game_state counter address");
        return false;
      }
      std::uint64_t recordSize = 0;
      if (!parseUnsigned(readyValue(readyPath, "proof.active_match_state.unit_node_record_size"), recordSize)
          || recordSize < 16)
      {
        errors.push_back("resident active_match proof is missing unit-node record size evidence");
        return false;
      }
      if (address != readUnitsAddress || recordSize != readUnitsRecordSize)
      {
        errors.push_back("resident active_match unit-node evidence does not match read_units proof");
        return false;
      }
      const std::size_t proofReadBytes =
        recordSize < 64
          ? static_cast<std::size_t>(recordSize)
          : 64;
      return validateLiveReadableEvidenceAddress(
        processId,
        address,
        proofReadBytes,
        "resident active_match unit-node evidence address",
        true,
        errors);
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

    void addError(std::vector<std::string>& errors, std::string error)
    {
      errors.push_back(std::move(error));
    }

    void validateReadyFileFreshness(
      const std::filesystem::path& readyPath,
      RuntimeResidentBridgeValidationOptions options,
      std::vector<std::string>& errors)
    {
      if (options.maximumReadyFileAgeMs == 0)
        return;

      std::error_code error;
      const std::filesystem::file_time_type modified =
        std::filesystem::last_write_time(readyPath, error);
      if (error)
      {
        addError(errors, "resident adapter ready file timestamp is unavailable");
        return;
      }

      const auto now = std::filesystem::file_time_type::clock::now();
      const auto maximumAge =
        std::chrono::milliseconds(static_cast<long long>(options.maximumReadyFileAgeMs));
      if (modified + maximumAge < now)
        addError(errors, "resident adapter heartbeat file is stale");
    }
  }

  const char* toString(RuntimeResidentQueueKind kind)
  {
    switch (kind)
    {
    case RuntimeResidentQueueKind::Command: return "command";
    case RuntimeResidentQueueKind::StateSnapshot: return "state-snapshot";
    case RuntimeResidentQueueKind::Event: return "event";
    case RuntimeResidentQueueKind::Overlay: return "overlay";
    case RuntimeResidentQueueKind::Proof: return "proof";
    }
    return "unknown";
  }

  std::vector<std::string> makeRuntimeResidentAdapterReadyLines(
    const RuntimeEnvironment& environment,
    std::uint64_t heartbeat)
  {
    std::vector<std::string> lines;
    lines.push_back("resident.adapter=active");
    lines.push_back(std::string("resident.adapter.abi=") + RuntimeResidentAdapterAbi);
    lines.push_back("resident.adapter.process_id=" + std::to_string(environment.processId));
    lines.push_back("resident.adapter.heartbeat=" + std::to_string(heartbeat));
    return lines;
  }

  std::vector<std::string> makeRuntimeResidentReadGameStateProofReadyLines(
    const RuntimeEnvironment& environment,
    std::uint64_t heartbeat,
    const std::vector<RuntimeResidentGameStateSample>& samples)
  {
    std::vector<std::string> lines;
    lines.push_back("proof.read_game_state=passed");
    lines.push_back("resident.proof.read_game_state.source=resident");
    lines.push_back("resident.proof.read_game_state.process_id=" + std::to_string(environment.processId));
    lines.push_back("resident.proof.read_game_state.heartbeat=" + std::to_string(heartbeat));
    lines.push_back("resident.proof.read_game_state.sample_count=" + std::to_string(samples.size()));
    lines.push_back("resident.proof.read_game_state.frame_samples=" + joinUnsignedSamples(samples, false));
    lines.push_back("resident.proof.read_game_state.tick_samples=" + joinUnsignedSamples(samples, true));
    return lines;
  }

  std::vector<std::string> makeRuntimeResidentActiveMatchProofReadyLines(
    const RuntimeEnvironment& environment,
    std::uint64_t heartbeat,
    std::uint64_t activeUnitCount,
    const std::string& mode)
  {
    std::vector<std::string> lines;
    lines.push_back("proof.active_match_state=passed");
    lines.push_back("resident.proof.active_match.source=resident");
    lines.push_back("resident.proof.active_match.process_id=" + std::to_string(environment.processId));
    lines.push_back("resident.proof.active_match.heartbeat=" + std::to_string(heartbeat));
    lines.push_back("resident.proof.active_match.mode=" + mode);
    lines.push_back("resident.proof.active_match.unit_activity_count=" + std::to_string(activeUnitCount));
    lines.push_back("resident.proof.active_match.evidence=resident-frame-unit-activity");
    return lines;
  }

  RuntimeResidentBridgeValidationResult validateRuntimeResidentBridgeReadyFile(
    const RuntimeEnvironment& environment,
    const std::filesystem::path& readyPath,
    RuntimeResidentBridgeValidationOptions options)
  {
    RuntimeResidentBridgeValidationResult result;

    std::error_code error;
    if (!std::filesystem::exists(readyPath, error) || error)
    {
      addError(result.errors, "resident adapter ready file does not exist");
      return result;
    }

    const std::string state = readyValue(readyPath, "resident.adapter");
    if (state.empty())
    {
      result.valid = true;
      result.active = false;
      result.warnings.push_back("resident adapter lines are not present");
      return result;
    }
    result.present = true;
    for (const std::string& key : {
           "resident.adapter",
           "resident.adapter.abi",
           "resident.adapter.heartbeat",
           "resident.adapter.process_id",
           "product",
           "version",
           "process_id",
           "executable" })
    {
      if (readyKeyCount(readyPath, key) > 1)
        addError(result.errors, "resident adapter ready file has duplicate key: " + key);
    }
    validateReadyFileFreshness(readyPath, options, result.errors);
    result.active = state == "active";
    if (!result.active)
      addError(result.errors, "resident adapter state is not active");

    result.abi = readyValue(readyPath, "resident.adapter.abi");
    if (result.abi != RuntimeResidentAdapterAbi)
      addError(result.errors, "resident adapter ABI is unsupported");

    const std::string heartbeat = readyValue(readyPath, "resident.adapter.heartbeat");
    if (!parseUnsigned(heartbeat, result.heartbeat))
    {
      addError(result.errors, "resident adapter heartbeat is missing or malformed");
    }
    else if (result.heartbeat < options.minimumHeartbeat)
    {
      addError(result.errors, "resident adapter heartbeat is stale");
    }

    const std::string residentProcessId = readyValue(readyPath, "resident.adapter.process_id");
    if (!parseSignedInt(residentProcessId, result.processId) || result.processId <= 0)
    {
      addError(result.errors, "resident adapter process_id is missing or malformed");
    }
    else if (environment.processId > 0 && result.processId != environment.processId)
    {
      addError(result.errors, "resident adapter process_id does not match the selected runtime");
    }

    if (readyValue(readyPath, "product") != toString(environment.product))
      addError(result.errors, "resident adapter product does not match the selected runtime");
    if (!environment.version.empty() && readyValue(readyPath, "version") != environment.version)
      addError(result.errors, "resident adapter version does not match the selected runtime");

    if (!environment.executablePath.empty())
    {
      const std::string executable = readyValue(readyPath, "executable");
      if (executable.empty()
          || normalizePath(executable) != normalizePath(environment.executablePath))
      {
        addError(result.errors, "resident adapter executable does not match the selected runtime");
      }
    }

    result.valid = result.errors.empty();
    return result;
  }

  RuntimeResidentStateProofValidationResult validateRuntimeResidentStateProofs(
    const RuntimeEnvironment& environment,
    const std::filesystem::path& readyPath,
    const RuntimeResidentBridgeValidationResult& resident)
  {
    (void)environment;

    RuntimeResidentStateProofValidationResult result;
    result.readGameStateProofPresent = readyValue(readyPath, "proof.read_game_state") == "passed";
    result.activeMatchProofPresent = readyValue(readyPath, "proof.active_match_state") == "passed";
    if (!result.readGameStateProofPresent && !result.activeMatchProofPresent)
      return result;

    if (!resident.present || !resident.valid)
    {
      addError(result.errors, "resident state proof requires active resident adapter metadata");
      return result;
    }

    if (result.readGameStateProofPresent)
    {
      bool readGameStateValid = true;
      const char* prefix = "resident.proof.read_game_state";
      if (readyValue(readyPath, std::string(prefix) + ".source") != "resident")
      {
        addError(result.errors, "resident read_game_state proof source is not resident");
        readGameStateValid = false;
      }
      if (!parseProofProcessAndHeartbeat(readyPath, prefix, resident, result.errors))
        readGameStateValid = false;

      std::uint64_t sampleCount = 0;
      std::vector<std::uint64_t> frames;
      std::vector<std::uint64_t> ticks;
      if (!parseUnsigned(readyValue(readyPath, std::string(prefix) + ".sample_count"), sampleCount)
          || sampleCount < 3)
      {
        addError(result.errors, "resident read_game_state proof must contain at least three samples");
        readGameStateValid = false;
      }
      if (!parseUnsignedList(readyValue(readyPath, std::string(prefix) + ".frame_samples"), frames)
          || frames.size() < 3
          || !strictlyIncreasing(frames))
      {
        addError(result.errors, "resident read_game_state frame samples must be strictly increasing");
        readGameStateValid = false;
      }
      if (!parseUnsignedList(readyValue(readyPath, std::string(prefix) + ".tick_samples"), ticks)
          || ticks.size() < 3
          || !strictlyIncreasing(ticks))
      {
        addError(result.errors, "resident read_game_state tick samples must be strictly increasing");
        readGameStateValid = false;
      }
      if (!frames.empty() && !ticks.empty() && frames.size() != ticks.size())
      {
        addError(result.errors, "resident read_game_state frame/tick sample counts differ");
        readGameStateValid = false;
      }
      if (sampleCount > 0
          && (!frames.empty() && frames.size() != sampleCount
              || !ticks.empty() && ticks.size() != sampleCount))
      {
        addError(result.errors, "resident read_game_state sample_count does not match samples");
        readGameStateValid = false;
      }
      if (readGameStateValid
          && !validateReadGameStateLiveMemoryProof(readyPath, resident.processId, frames, result.errors))
      {
        readGameStateValid = false;
      }

      if (readGameStateValid)
      {
        for (std::size_t i = 0; i < frames.size(); ++i)
          result.samples.push_back(RuntimeResidentGameStateSample{ frames[i], ticks[i] });
        result.readGameStateValid = true;
      }
    }

    if (result.activeMatchProofPresent)
    {
      bool activeMatchValid = true;
      const char* prefix = "resident.proof.active_match";
      if (readyValue(readyPath, std::string(prefix) + ".source") != "resident")
      {
        addError(result.errors, "resident active_match proof source is not resident");
        activeMatchValid = false;
      }
      if (!parseProofProcessAndHeartbeat(readyPath, prefix, resident, result.errors))
        activeMatchValid = false;

      result.activeMatchMode = readyValue(readyPath, std::string(prefix) + ".mode");
      if (result.activeMatchMode != "match")
      {
        addError(result.errors, "resident active_match proof mode is not match");
        activeMatchValid = false;
      }

      if (!parseUnsigned(
            readyValue(readyPath, std::string(prefix) + ".unit_activity_count"),
            result.activeUnitCount)
          || result.activeUnitCount == 0)
      {
        addError(result.errors, "resident active_match proof requires live unit/activity evidence");
        activeMatchValid = false;
      }

      if (readyValue(readyPath, std::string(prefix) + ".evidence") != "resident-frame-unit-activity")
      {
        addError(result.errors, "resident active_match proof evidence is unsupported");
        activeMatchValid = false;
      }
      if (activeMatchValid
          && !validateActiveMatchLiveUnitEvidence(
            readyPath,
            resident.processId,
            result.activeUnitCount,
            result.errors))
      {
        activeMatchValid = false;
      }

      if (!result.readGameStateValid)
      {
        addError(result.errors, "resident active_match proof requires valid resident frame/tick progression");
        activeMatchValid = false;
      }

      result.activeMatchValid = activeMatchValid;
    }

    return result;
  }

  RuntimeResidentQueueHeader makeRuntimeResidentQueueHeader(
    RuntimeResidentQueueKind kind,
    std::uint16_t recordBytes,
    std::uint32_t capacityRecords,
    std::uint64_t heartbeat)
  {
    RuntimeResidentQueueHeader header;
    header.recordBytes = recordBytes;
    header.kind = static_cast<std::uint16_t>(kind);
    header.capacityRecords = capacityRecords;
    header.heartbeat = heartbeat;
    return header;
  }

  RuntimeResidentQueueValidationResult validateRuntimeResidentQueueHeader(
    const RuntimeResidentQueueHeader& header,
    RuntimeResidentQueueKind expectedKind,
    RuntimeResidentBridgeValidationOptions options)
  {
    RuntimeResidentQueueValidationResult result;

    if (header.magic != RuntimeResidentQueueMagic)
      addError(result.errors, "resident queue magic is invalid");
    if (header.abiMajor != RuntimeResidentAdapterAbiMajor)
      addError(result.errors, "resident queue ABI major version is unsupported");
    if (header.headerBytes < sizeof(RuntimeResidentQueueHeader))
      addError(result.errors, "resident queue header is truncated");
    if (header.kind != static_cast<std::uint16_t>(expectedKind))
      addError(result.errors, "resident queue kind does not match expected queue");
    if (header.recordBytes == 0)
      addError(result.errors, "resident queue record size is zero");
    if (header.capacityRecords == 0)
      addError(result.errors, "resident queue capacity is zero");
    if (header.readSequence > header.writeSequence)
    {
      addError(result.errors, "resident queue read sequence is ahead of write sequence");
    }
    else if (header.writeSequence - header.readSequence > header.capacityRecords)
    {
      addError(result.errors, "resident queue sequence distance exceeds capacity");
    }
    if (header.heartbeat < options.minimumHeartbeat)
      addError(result.errors, "resident queue heartbeat is stale");

    result.valid = result.errors.empty();
    return result;
  }

  RuntimeResidentQueueValidationResult validateRuntimeResidentRecordHeader(
    const RuntimeResidentQueueHeader& queue,
    const RuntimeResidentRecordHeader& record,
    RuntimeResidentQueueKind expectedKind)
  {
    RuntimeResidentQueueValidationResult result;

    if (record.headerBytes < sizeof(RuntimeResidentRecordHeader))
      addError(result.errors, "resident queue record header is truncated");
    if (record.kind != static_cast<std::uint16_t>(expectedKind))
      addError(result.errors, "resident queue record kind does not match expected queue");
    if (queue.recordBytes < sizeof(RuntimeResidentRecordHeader))
    {
      addError(result.errors, "resident queue record size cannot contain a record header");
    }
    else if (record.headerBytes > queue.recordBytes)
    {
      addError(result.errors, "resident queue record header exceeds record capacity");
    }
    else if (record.payloadBytes > queue.recordBytes - record.headerBytes)
    {
      addError(result.errors, "resident queue record payload exceeds record capacity");
    }
    if (record.sequence < queue.readSequence || record.sequence >= queue.writeSequence)
      addError(result.errors, "resident queue record sequence is outside the readable window");

    result.valid = result.errors.empty();
    return result;
  }
}
