#pragma once

#include <BWAPI/Runtime/RuntimeCommandQueue.h>

#include <cstdint>
#include <string>
#include <vector>

namespace BWAPI::Runtime
{
  struct RuntimeEncodedCommand
  {
    bool encoded = false;
    std::vector<std::uint8_t> bytes;
    std::string reason;
    std::vector<std::string> warnings;
  };

  RuntimeEncodedCommand encodeRuntimeCommandRequest(const RuntimeCommandRequest& request);
  RuntimeEncodedCommand encodeRuntimeSelectCommand(
    const std::vector<int>& unitTargetIds,
    bool addToSelection = false);
  std::string formatCommandBytesHex(const std::vector<std::uint8_t>& bytes);
}
