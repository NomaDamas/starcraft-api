#pragma once

#include <BWAPI/Runtime/RuntimeBackend.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace BWAPI::Runtime
{
  inline constexpr const char* RuntimeResidentAdapterAbi = "starcraft-api-resident-adapter-v1";
  inline constexpr std::uint16_t RuntimeResidentAdapterAbiMajor = 1;
  inline constexpr std::uint32_t RuntimeResidentQueueMagic = 0x53434151; // SCAQ
  inline constexpr const char* RuntimeResidentCommandQueueFile = "resident-command.queue";
  inline constexpr const char* RuntimeResidentOverlayQueueFile = "resident-overlay.queue";
  inline constexpr const char* RuntimeResidentProofQueueFile = "resident-proof.queue";

  enum class RuntimeResidentQueueKind : std::uint16_t
  {
    Command = 1,
    StateSnapshot = 2,
    Event = 3,
    Overlay = 4,
    Proof = 5
  };

  struct RuntimeResidentQueueHeader
  {
    std::uint32_t magic = RuntimeResidentQueueMagic;
    std::uint16_t abiMajor = RuntimeResidentAdapterAbiMajor;
    std::uint16_t headerBytes = sizeof(RuntimeResidentQueueHeader);
    std::uint16_t recordBytes = 0;
    std::uint16_t kind = static_cast<std::uint16_t>(RuntimeResidentQueueKind::Command);
    std::uint32_t capacityRecords = 0;
    std::uint64_t writeSequence = 0;
    std::uint64_t readSequence = 0;
    std::uint64_t heartbeat = 0;
  };

  struct RuntimeResidentRecordHeader
  {
    std::uint16_t headerBytes = sizeof(RuntimeResidentRecordHeader);
    std::uint16_t kind = static_cast<std::uint16_t>(RuntimeResidentQueueKind::Command);
    std::uint32_t payloadBytes = 0;
    std::uint64_t sequence = 0;
  };

  struct RuntimeResidentBridgeValidationOptions
  {
    std::uint64_t minimumHeartbeat = 2;
    std::uint64_t maximumReadyFileAgeMs = 30000;
  };

  struct RuntimeResidentBridgeValidationResult
  {
    bool valid = false;
    bool present = false;
    bool active = false;
    std::string abi;
    std::uint64_t heartbeat = 0;
    int processId = 0;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
  };

  struct RuntimeResidentQueueValidationResult
  {
    bool valid = false;
    std::vector<std::string> errors;
  };

  struct RuntimeResidentQueueAppendResult
  {
    bool appended = false;
    std::uint64_t sequence = 0;
    std::string reason;
    std::vector<std::string> errors;
  };

  struct RuntimeResidentQueueRecord
  {
    RuntimeResidentRecordHeader header;
    std::vector<unsigned char> payload;
  };

  struct RuntimeResidentQueueReadResult
  {
    bool read = false;
    RuntimeResidentQueueHeader header;
    std::vector<RuntimeResidentQueueRecord> records;
    std::string reason;
    std::vector<std::string> errors;
  };

  struct RuntimeResidentQueueAcknowledgeResult
  {
    bool acknowledged = false;
    RuntimeResidentQueueHeader header;
    std::string reason;
    std::vector<std::string> errors;
  };

  struct RuntimeResidentGameStateSample
  {
    std::uint64_t frame = 0;
    std::uint64_t tick = 0;
  };

  struct RuntimeResidentStateProofValidationResult
  {
    bool readGameStateProofPresent = false;
    bool activeMatchProofPresent = false;
    bool readGameStateValid = false;
    bool activeMatchValid = false;
    std::vector<RuntimeResidentGameStateSample> samples;
    std::uint64_t activeUnitCount = 0;
    std::string activeMatchMode;
    std::vector<std::string> errors;
  };

  const char* toString(RuntimeResidentQueueKind kind);

  std::vector<std::string> makeRuntimeResidentAdapterReadyLines(
    const RuntimeEnvironment& environment,
    std::uint64_t heartbeat);

  std::vector<std::string> makeRuntimeResidentReadGameStateProofReadyLines(
    const RuntimeEnvironment& environment,
    std::uint64_t heartbeat,
    const std::vector<RuntimeResidentGameStateSample>& samples);

  std::vector<std::string> makeRuntimeResidentActiveMatchProofReadyLines(
    const RuntimeEnvironment& environment,
    std::uint64_t heartbeat,
    std::uint64_t activeUnitCount,
    const std::string& mode);

  RuntimeResidentBridgeValidationResult validateRuntimeResidentBridgeReadyFile(
    const RuntimeEnvironment& environment,
    const std::filesystem::path& readyPath,
    RuntimeResidentBridgeValidationOptions options = {});

  RuntimeResidentStateProofValidationResult validateRuntimeResidentStateProofs(
    const RuntimeEnvironment& environment,
    const std::filesystem::path& readyPath,
    const RuntimeResidentBridgeValidationResult& resident);

  RuntimeResidentQueueHeader makeRuntimeResidentQueueHeader(
    RuntimeResidentQueueKind kind,
    std::uint16_t recordBytes,
    std::uint32_t capacityRecords,
    std::uint64_t heartbeat);

  RuntimeResidentQueueValidationResult validateRuntimeResidentQueueHeader(
    const RuntimeResidentQueueHeader& header,
    RuntimeResidentQueueKind expectedKind,
    RuntimeResidentBridgeValidationOptions options = {});

  std::vector<std::string> makeRuntimeResidentQueueReadyLines(
    RuntimeResidentQueueKind kind,
    const std::filesystem::path& queuePath,
    const RuntimeResidentQueueHeader& header);

  RuntimeResidentQueueValidationResult validateRuntimeResidentQueueFile(
    const std::filesystem::path& queuePath,
    RuntimeResidentQueueKind expectedKind,
    RuntimeResidentBridgeValidationOptions options = {});

  RuntimeResidentQueueValidationResult ensureRuntimeResidentQueueFile(
    const std::filesystem::path& queuePath,
    const RuntimeResidentQueueHeader& desiredHeader,
    RuntimeResidentQueueHeader& actualHeader,
    RuntimeResidentBridgeValidationOptions options = {});

  RuntimeResidentQueueAppendResult appendRuntimeResidentQueueRecord(
    const std::filesystem::path& queuePath,
    RuntimeResidentQueueKind expectedKind,
    const std::vector<unsigned char>& payload,
    RuntimeResidentBridgeValidationOptions options = {});

  RuntimeResidentQueueReadResult readRuntimeResidentQueueRecords(
    const std::filesystem::path& queuePath,
    RuntimeResidentQueueKind expectedKind,
    std::size_t maxRecords,
    RuntimeResidentBridgeValidationOptions options = {});

  RuntimeResidentQueueAcknowledgeResult acknowledgeRuntimeResidentQueueRecords(
    const std::filesystem::path& queuePath,
    RuntimeResidentQueueKind expectedKind,
    std::uint64_t readSequence,
    RuntimeResidentBridgeValidationOptions options = {});

  RuntimeResidentQueueValidationResult validateRuntimeResidentRecordHeader(
    const RuntimeResidentQueueHeader& queue,
    const RuntimeResidentRecordHeader& record,
    RuntimeResidentQueueKind expectedKind);
}
