#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace BWAPI::Runtime
{
  struct RuntimeMemoryReadResult
  {
    bool success = false;
    std::size_t bytesRead = 0;
    std::vector<unsigned char> bytes;
    std::string reason;
  };

  int currentProcessId();
  RuntimeMemoryReadResult readProcessMemory(
    int processId,
    std::uintptr_t address,
    std::size_t size);
}
