#include <BWAPI/Runtime/RuntimeResidentBridge.h>

#include <chrono>
#include <climits>
#include <fstream>
#include <sstream>
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
