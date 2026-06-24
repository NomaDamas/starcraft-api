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
    std::uint64_t minimumHeartbeat = 1;
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

  RuntimeResidentQueueValidationResult validateRuntimeResidentRecordHeader(
    const RuntimeResidentQueueHeader& queue,
    const RuntimeResidentRecordHeader& record,
    RuntimeResidentQueueKind expectedKind);
}
