#include <BWAPI/Runtime/RuntimeResidentBridge.h>
#include <BWAPI/Runtime/RuntimeProcess.h>
#include <BWAPI/Runtime/RuntimeProcessMemory.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstring>
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

    const char* residentQueueReadyName(RuntimeResidentQueueKind kind)
    {
      switch (kind)
      {
      case RuntimeResidentQueueKind::Command: return "command";
      case RuntimeResidentQueueKind::StateSnapshot: return "state_snapshot";
      case RuntimeResidentQueueKind::Event: return "event";
      case RuntimeResidentQueueKind::Overlay: return "overlay";
      case RuntimeResidentQueueKind::Proof: return "proof";
      }
      return "unknown";
    }

    std::string residentQueueReadyKey(RuntimeResidentQueueKind kind, const char* suffix)
    {
      return std::string("resident.queue.")
        + residentQueueReadyName(kind)
        + '.'
        + suffix;
    }

    std::filesystem::path resolveReadyRelativePath(
      const std::filesystem::path& readyPath,
      const std::string& value)
    {
      std::filesystem::path path(value);
      if (path.is_absolute())
        return path;
      return readyPath.parent_path() / path;
    }

    bool readResidentQueueHeaderFile(
      const std::filesystem::path& queuePath,
      RuntimeResidentQueueHeader& header,
      std::vector<std::string>& errors)
    {
      std::ifstream input(queuePath, std::ios::binary);
      if (!input)
      {
        errors.push_back("resident queue file does not exist or is not readable: " + queuePath.string());
        return false;
      }

      RuntimeResidentQueueHeader candidate;
      input.read(reinterpret_cast<char*>(&candidate), sizeof(candidate));
      if (input.gcount() != static_cast<std::streamsize>(sizeof(candidate)))
      {
        errors.push_back("resident queue file does not contain a complete queue header: " + queuePath.string());
        return false;
      }

      header = candidate;
      return true;
    }

    void validateOptionalResidentQueue(
      const std::filesystem::path& readyPath,
      RuntimeResidentQueueKind kind,
      const char* label,
      RuntimeResidentBridgeValidationOptions options,
      std::vector<std::string>& errors)
    {
      const std::string queuePathValue =
        readyValue(readyPath, residentQueueReadyKey(kind, "path"));
      if (queuePathValue.empty())
        return;

      for (const char* suffix : {
             "path",
             "record_bytes",
             "capacity_records",
             "write_sequence",
             "read_sequence",
             "heartbeat" })
      {
        const std::string key = residentQueueReadyKey(kind, suffix);
        if (readyKeyCount(readyPath, key) > 1)
          errors.push_back("resident adapter ready file has duplicate key: " + key);
      }

      const std::filesystem::path queuePath =
        resolveReadyRelativePath(readyPath, queuePathValue);
      RuntimeResidentQueueHeader header;
      if (!readResidentQueueHeaderFile(queuePath, header, errors))
        return;

      RuntimeResidentQueueValidationResult queue =
        validateRuntimeResidentQueueHeader(
          header,
          kind,
          options);
      for (const std::string& queueError : queue.errors)
        errors.push_back(std::string("resident ") + label + " queue invalid: " + queueError);

      const struct
      {
        const char* suffix;
        std::uint64_t value;
      } expected[] = {
        { "record_bytes", header.recordBytes },
        { "capacity_records", header.capacityRecords },
        { "write_sequence", header.writeSequence },
        { "read_sequence", header.readSequence }
      };

      for (const auto& field : expected)
      {
        const std::string key = residentQueueReadyKey(kind, field.suffix);
        const std::string raw = readyValue(readyPath, key);
        if (raw.empty())
          continue;

        std::uint64_t parsed = 0;
        if (!parseUnsigned(raw, parsed) || parsed != field.value)
          errors.push_back(
            std::string("resident ") + label
            + " queue ready metadata does not match queue header: " + key);
      }

      const std::string heartbeatKey = residentQueueReadyKey(kind, "heartbeat");
      const std::string rawHeartbeat = readyValue(readyPath, heartbeatKey);
      if (!rawHeartbeat.empty())
      {
        std::uint64_t readyHeartbeat = 0;
        if (!parseUnsigned(rawHeartbeat, readyHeartbeat))
        {
          errors.push_back(
            std::string("resident ") + label
            + " queue ready metadata does not match queue header: " + heartbeatKey);
        }
        else if (header.heartbeat < readyHeartbeat)
        {
          errors.push_back(
            std::string("resident ") + label
            + " queue header heartbeat is older than ready metadata: " + heartbeatKey);
        }
      }
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
      std::vector<std::string>& errors,
      std::uint64_t maximumHeartbeatLag = 0)
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

      constexpr std::uint64_t maximumHeartbeatLead = 2;
      if (!parseUnsigned(readyValue(readyPath, std::string(prefix) + ".heartbeat"), heartbeat))
      {
        errors.push_back(std::string(prefix) + " heartbeat is missing or malformed");
        valid = false;
      }
      else if (heartbeat > resident.heartbeat + maximumHeartbeatLead)
      {
        errors.push_back(std::string(prefix) + " heartbeat is newer than the current resident adapter heartbeat");
        valid = false;
      }
      else if (heartbeat <= resident.heartbeat
               && resident.heartbeat - heartbeat > maximumHeartbeatLag)
      {
        errors.push_back(std::string(prefix) + " heartbeat is too old for the current resident adapter heartbeat");
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

      RuntimeMemoryReadResult nextRead;
      std::uint32_t nextFrame = currentFrame;
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(750);
      while (std::chrono::steady_clock::now() < deadline)
      {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        nextRead = readProcessMemory(processId, address, sizeof(std::uint32_t));
        if (!nextRead.success || nextRead.bytesRead != sizeof(std::uint32_t))
          break;

        nextFrame = readLittleU32(nextRead.bytes);
        if (nextFrame > currentFrame)
          return true;
      }

      if (!nextRead.success || nextRead.bytesRead != sizeof(std::uint32_t))
      {
        errors.push_back(
          nextRead.reason.empty()
            ? "resident read_game_state proof address could not be re-sampled from the selected process"
            : "resident read_game_state proof address could not be re-sampled: " + nextRead.reason);
        return false;
      }

      errors.push_back("resident read_game_state proof address did not advance during live re-sampling");
      return false;
    }

    bool validateReadGameStateResidentSelfReadProof(
      const std::filesystem::path& readyPath,
      const std::vector<std::uint64_t>& frames,
      std::vector<std::string>& errors)
    {
      if (readyValue(readyPath, "resident.proof.read_game_state.validation")
          != "resident-self-read-v1")
        return false;

      std::uintptr_t address = 0;
      if (!parseAddress(readyValue(readyPath, "proof.read_game_state.address"), address))
      {
        errors.push_back("resident read_game_state self-read proof address is missing or malformed");
        return false;
      }

      if (frames.empty() || frames.back() > std::numeric_limits<std::uint32_t>::max())
      {
        errors.push_back("resident read_game_state self-read frame sample is outside u32 counter range");
        return false;
      }

      if (readyValue(readyPath, "resident.proof.read_game_state.address_read") != "resident-self")
      {
        errors.push_back("resident read_game_state self-read proof did not report resident-self address read");
        return false;
      }

      if (readyValue(readyPath, "resident.proof.read_game_state.counter_bytes") != "4")
      {
        errors.push_back("resident read_game_state self-read proof must report a 32-bit frame counter");
        return false;
      }

      if (readyValue(readyPath, "proof.read_game_state.confidence") != "frame-like")
      {
        errors.push_back("resident read_game_state self-read proof confidence is not frame-like");
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
      const bool residentPreservedActiveUnitMemory =
        readyValue(readyPath, "resident.proof.active_match.validation")
          == "resident-preserved-active-unit-memory-v1"
        && readyValue(readyPath, "resident.proof.active_match.address_read")
          == "resident-self";
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
        if (residentPreservedActiveUnitMemory)
          return true;
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
      if (residentPreservedActiveUnitMemory)
        return true;
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

    std::uintmax_t residentQueueFileSize(const RuntimeResidentQueueHeader& header)
    {
      return static_cast<std::uintmax_t>(header.headerBytes)
        + static_cast<std::uintmax_t>(header.recordBytes)
        * static_cast<std::uintmax_t>(header.capacityRecords);
    }

    std::uintmax_t residentQueueRecordOffset(
      const RuntimeResidentQueueHeader& header,
      std::uint64_t sequence)
    {
      const std::uint64_t slot =
        header.capacityRecords == 0 ? 0 : sequence % header.capacityRecords;
      return static_cast<std::uintmax_t>(header.headerBytes)
        + static_cast<std::uintmax_t>(slot)
        * static_cast<std::uintmax_t>(header.recordBytes);
    }

    bool writeResidentQueueHeaderFile(
      const std::filesystem::path& queuePath,
      const RuntimeResidentQueueHeader& header,
      std::vector<std::string>& errors)
    {
      std::fstream file(queuePath, std::ios::binary | std::ios::in | std::ios::out);
      if (!file)
      {
        std::ofstream create(queuePath, std::ios::binary | std::ios::trunc);
        if (!create)
        {
          errors.push_back("unable to create resident queue file: " + queuePath.string());
          return false;
        }
        create.close();
        file.open(queuePath, std::ios::binary | std::ios::in | std::ios::out);
      }
      if (!file)
      {
        errors.push_back("unable to open resident queue file for header update: " + queuePath.string());
        return false;
      }

      file.seekp(0);
      file.write(reinterpret_cast<const char*>(&header), sizeof(header));
      if (!file)
      {
        errors.push_back("unable to write resident queue header: " + queuePath.string());
        return false;
      }
      return true;
    }

    RuntimeResidentQueueAppendResult rejectAppend(std::string reason)
    {
      RuntimeResidentQueueAppendResult result;
      result.reason = std::move(reason);
      result.errors.push_back(result.reason);
      return result;
    }

    RuntimeResidentQueueReadResult rejectRead(std::string reason)
    {
      RuntimeResidentQueueReadResult result;
      result.reason = std::move(reason);
      result.errors.push_back(result.reason);
      return result;
    }

    RuntimeResidentQueueAcknowledgeResult rejectAcknowledge(std::string reason)
    {
      RuntimeResidentQueueAcknowledgeResult result;
      result.reason = std::move(reason);
      result.errors.push_back(result.reason);
      return result;
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
    else if (!runtimeProcessExists(result.processId))
    {
      addError(result.errors, "resident adapter process_id is not a live runtime process");
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

    validateOptionalResidentQueue(
      readyPath,
      RuntimeResidentQueueKind::Command,
      "command",
      options,
      result.errors);
    validateOptionalResidentQueue(
      readyPath,
      RuntimeResidentQueueKind::Overlay,
      "overlay",
      options,
      result.errors);
    validateOptionalResidentQueue(
      readyPath,
      RuntimeResidentQueueKind::Proof,
      "proof",
      options,
      result.errors);

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
      const bool residentSelfReadProof =
        readyValue(readyPath, "resident.proof.read_game_state.validation")
          == "resident-self-read-v1";
      if (readGameStateValid
          && residentSelfReadProof
          && !validateReadGameStateResidentSelfReadProof(readyPath, frames, result.errors))
      {
        readGameStateValid = false;
      }
      if (readGameStateValid
          && !residentSelfReadProof
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
      const std::string activeMatchSource =
        readyValue(readyPath, std::string(prefix) + ".source");
      if (activeMatchSource != "resident" && activeMatchSource != "adapter-proof")
      {
        addError(result.errors, "resident active_match proof source is unsupported");
        activeMatchValid = false;
      }
      constexpr std::uint64_t maxPreservedActiveMatchHeartbeatLag = 120;
      if (!parseProofProcessAndHeartbeat(
            readyPath,
            prefix,
            resident,
            result.errors,
            maxPreservedActiveMatchHeartbeatLag))
        activeMatchValid = false;

      result.activeMatchMode = readyValue(readyPath, std::string(prefix) + ".mode");
      if (result.activeMatchMode != "match" && result.activeMatchMode != "replay")
      {
        addError(result.errors, "resident active_match proof mode is neither match nor replay");
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

      const std::string activeMatchEvidence =
        readyValue(readyPath, std::string(prefix) + ".evidence");
      if (activeMatchEvidence != "resident-frame-unit-activity"
          && activeMatchEvidence != "adapter-live-unit-activity")
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

  std::vector<std::string> makeRuntimeResidentQueueReadyLines(
    RuntimeResidentQueueKind kind,
    const std::filesystem::path& queuePath,
    const RuntimeResidentQueueHeader& header)
  {
    const std::string prefix = std::string("resident.queue.") + residentQueueReadyName(kind);
    return {
      prefix + ".path=" + queuePath.string(),
      prefix + ".record_bytes=" + std::to_string(header.recordBytes),
      prefix + ".capacity_records=" + std::to_string(header.capacityRecords),
      prefix + ".write_sequence=" + std::to_string(header.writeSequence),
      prefix + ".read_sequence=" + std::to_string(header.readSequence),
      prefix + ".heartbeat=" + std::to_string(header.heartbeat)
    };
  }

  RuntimeResidentQueueValidationResult validateRuntimeResidentQueueFile(
    const std::filesystem::path& queuePath,
    RuntimeResidentQueueKind expectedKind,
    RuntimeResidentBridgeValidationOptions options)
  {
    RuntimeResidentQueueValidationResult result;
    RuntimeResidentQueueHeader header;
    if (!readResidentQueueHeaderFile(queuePath, header, result.errors))
    {
      result.valid = false;
      return result;
    }

    result = validateRuntimeResidentQueueHeader(header, expectedKind, options);
    return result;
  }

  RuntimeResidentQueueValidationResult ensureRuntimeResidentQueueFile(
    const std::filesystem::path& queuePath,
    const RuntimeResidentQueueHeader& desiredHeader,
    RuntimeResidentQueueHeader& actualHeader,
    RuntimeResidentBridgeValidationOptions options)
  {
    RuntimeResidentQueueValidationResult desired =
      validateRuntimeResidentQueueHeader(
        desiredHeader,
        static_cast<RuntimeResidentQueueKind>(desiredHeader.kind),
        options);
    if (!desired.valid)
    {
      actualHeader = desiredHeader;
      return desired;
    }

    actualHeader = desiredHeader;
    RuntimeResidentQueueHeader existing;
    std::vector<std::string> readErrors;
    if (readResidentQueueHeaderFile(queuePath, existing, readErrors))
    {
      RuntimeResidentQueueValidationResult existingValidation =
        validateRuntimeResidentQueueHeader(
          existing,
          static_cast<RuntimeResidentQueueKind>(desiredHeader.kind),
          RuntimeResidentBridgeValidationOptions{ 0, options.maximumReadyFileAgeMs });
      if (existingValidation.valid
          && existing.recordBytes == desiredHeader.recordBytes
          && existing.capacityRecords == desiredHeader.capacityRecords
          && existing.headerBytes == desiredHeader.headerBytes)
      {
        actualHeader.readSequence = existing.readSequence;
        actualHeader.writeSequence = existing.writeSequence;
      }
    }

    std::error_code resizeError;
    std::filesystem::resize_file(queuePath, residentQueueFileSize(actualHeader), resizeError);
    if (resizeError)
      addError(desired.errors, "unable to size resident queue file: " + resizeError.message());

    if (!writeResidentQueueHeaderFile(queuePath, actualHeader, desired.errors))
      desired.valid = false;

    desired.valid = desired.errors.empty();
    return desired;
  }

  RuntimeResidentQueueAppendResult appendRuntimeResidentQueueRecord(
    const std::filesystem::path& queuePath,
    RuntimeResidentQueueKind expectedKind,
    const std::vector<unsigned char>& payload,
    RuntimeResidentBridgeValidationOptions options)
  {
    RuntimeResidentQueueHeader header;
    std::vector<std::string> errors;
    if (!readResidentQueueHeaderFile(queuePath, header, errors))
    {
      RuntimeResidentQueueAppendResult result =
        rejectAppend(errors.empty() ? "unable to read resident queue header" : errors.front());
      result.errors = std::move(errors);
      if (result.errors.empty())
        result.errors.push_back(result.reason);
      return result;
    }

    RuntimeResidentQueueValidationResult validation =
      validateRuntimeResidentQueueHeader(header, expectedKind, options);
    if (!validation.valid)
    {
      RuntimeResidentQueueAppendResult result =
        rejectAppend(validation.errors.empty()
          ? "resident queue header is invalid"
          : validation.errors.front());
      result.errors = std::move(validation.errors);
      return result;
    }

    if (header.recordBytes < sizeof(RuntimeResidentRecordHeader))
      return rejectAppend("resident queue record size cannot contain a record header");
    if (payload.size() > header.recordBytes - sizeof(RuntimeResidentRecordHeader))
      return rejectAppend("resident queue payload exceeds record capacity");
    if (header.writeSequence - header.readSequence >= header.capacityRecords)
      return rejectAppend("resident queue is full");

    RuntimeResidentRecordHeader record;
    record.kind = static_cast<std::uint16_t>(expectedKind);
    record.payloadBytes = static_cast<std::uint32_t>(payload.size());
    record.sequence = header.writeSequence;

    RuntimeResidentQueueHeader validationHeader = header;
    ++validationHeader.writeSequence;
    RuntimeResidentQueueValidationResult recordValidation =
      validateRuntimeResidentRecordHeader(validationHeader, record, expectedKind);
    if (!recordValidation.valid)
    {
      RuntimeResidentQueueAppendResult result =
        rejectAppend(recordValidation.errors.empty()
          ? "resident queue record header is invalid"
          : recordValidation.errors.front());
      result.errors = std::move(recordValidation.errors);
      return result;
    }

    std::fstream file(queuePath, std::ios::binary | std::ios::in | std::ios::out);
    if (!file)
      return rejectAppend("unable to open resident queue file for append");

    std::vector<unsigned char> slot(header.recordBytes, 0);
    std::memcpy(slot.data(), &record, sizeof(record));
    if (!payload.empty())
      std::memcpy(slot.data() + sizeof(record), payload.data(), payload.size());

    file.seekp(static_cast<std::streamoff>(residentQueueRecordOffset(header, record.sequence)));
    file.write(reinterpret_cast<const char*>(slot.data()), static_cast<std::streamsize>(slot.size()));
    if (!file)
      return rejectAppend("unable to write resident queue record");

    ++header.writeSequence;
    std::vector<std::string> writeErrors;
    if (!writeResidentQueueHeaderFile(queuePath, header, writeErrors))
    {
      RuntimeResidentQueueAppendResult result =
        rejectAppend(writeErrors.empty()
          ? "unable to update resident queue write sequence"
          : writeErrors.front());
      result.errors = std::move(writeErrors);
      return result;
    }

    RuntimeResidentQueueAppendResult result;
    result.appended = true;
    result.sequence = record.sequence;
    return result;
  }

  RuntimeResidentQueueReadResult readRuntimeResidentQueueRecords(
    const std::filesystem::path& queuePath,
    RuntimeResidentQueueKind expectedKind,
    std::size_t maxRecords,
    RuntimeResidentBridgeValidationOptions options)
  {
    RuntimeResidentQueueHeader header;
    std::vector<std::string> errors;
    if (!readResidentQueueHeaderFile(queuePath, header, errors))
    {
      RuntimeResidentQueueReadResult result =
        rejectRead(errors.empty() ? "unable to read resident queue header" : errors.front());
      result.errors = std::move(errors);
      return result;
    }

    RuntimeResidentQueueValidationResult validation =
      validateRuntimeResidentQueueHeader(header, expectedKind, options);
    if (!validation.valid)
    {
      RuntimeResidentQueueReadResult result =
        rejectRead(validation.errors.empty()
          ? "resident queue header is invalid"
          : validation.errors.front());
      result.errors = std::move(validation.errors);
      return result;
    }

    RuntimeResidentQueueReadResult result;
    result.header = header;
    if (maxRecords == 0 || header.readSequence == header.writeSequence)
    {
      result.read = true;
      return result;
    }

    std::ifstream file(queuePath, std::ios::binary);
    if (!file)
      return rejectRead("unable to open resident queue file for read");

    const std::uint64_t available = header.writeSequence - header.readSequence;
    const std::uint64_t recordsToRead =
      std::min<std::uint64_t>(available, static_cast<std::uint64_t>(maxRecords));
    for (std::uint64_t index = 0; index < recordsToRead; ++index)
    {
      const std::uint64_t sequence = header.readSequence + index;
      std::vector<unsigned char> slot(header.recordBytes, 0);
      file.seekg(static_cast<std::streamoff>(residentQueueRecordOffset(header, sequence)));
      file.read(reinterpret_cast<char*>(slot.data()), static_cast<std::streamsize>(slot.size()));
      if (file.gcount() != static_cast<std::streamsize>(slot.size()))
        return rejectRead("resident queue record slot is truncated");

      RuntimeResidentRecordHeader record;
      std::memcpy(&record, slot.data(), sizeof(record));
      RuntimeResidentQueueValidationResult recordValidation =
        validateRuntimeResidentRecordHeader(header, record, expectedKind);
      if (!recordValidation.valid)
      {
        RuntimeResidentQueueReadResult rejected =
          rejectRead(recordValidation.errors.empty()
            ? "resident queue record header is invalid"
            : recordValidation.errors.front());
        rejected.errors = std::move(recordValidation.errors);
        return rejected;
      }

      RuntimeResidentQueueRecord queueRecord;
      queueRecord.header = record;
      queueRecord.payload.assign(
        slot.begin() + static_cast<std::vector<unsigned char>::difference_type>(record.headerBytes),
        slot.begin() + static_cast<std::vector<unsigned char>::difference_type>(record.headerBytes + record.payloadBytes));
      result.records.push_back(std::move(queueRecord));
    }

    result.read = true;
    return result;
  }

  RuntimeResidentQueueAcknowledgeResult acknowledgeRuntimeResidentQueueRecords(
    const std::filesystem::path& queuePath,
    RuntimeResidentQueueKind expectedKind,
    std::uint64_t readSequence,
    RuntimeResidentBridgeValidationOptions options)
  {
    RuntimeResidentQueueHeader header;
    std::vector<std::string> errors;
    if (!readResidentQueueHeaderFile(queuePath, header, errors))
    {
      RuntimeResidentQueueAcknowledgeResult result =
        rejectAcknowledge(errors.empty() ? "unable to read resident queue header" : errors.front());
      result.errors = std::move(errors);
      return result;
    }

    RuntimeResidentQueueValidationResult validation =
      validateRuntimeResidentQueueHeader(header, expectedKind, options);
    if (!validation.valid)
    {
      RuntimeResidentQueueAcknowledgeResult result =
        rejectAcknowledge(validation.errors.empty()
          ? "resident queue header is invalid"
          : validation.errors.front());
      result.errors = std::move(validation.errors);
      return result;
    }

    if (readSequence < header.readSequence || readSequence > header.writeSequence)
      return rejectAcknowledge("resident queue acknowledge sequence is outside the readable window");

    header.readSequence = readSequence;
    std::vector<std::string> writeErrors;
    if (!writeResidentQueueHeaderFile(queuePath, header, writeErrors))
    {
      RuntimeResidentQueueAcknowledgeResult result =
        rejectAcknowledge(writeErrors.empty()
          ? "unable to update resident queue read sequence"
          : writeErrors.front());
      result.errors = std::move(writeErrors);
      return result;
    }

    RuntimeResidentQueueAcknowledgeResult result;
    result.acknowledged = true;
    result.header = header;
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
