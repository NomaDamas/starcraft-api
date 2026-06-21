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

  struct RuntimeMemoryWriteResult
  {
    bool success = false;
    std::size_t bytesWritten = 0;
    std::string reason;
  };

  struct RuntimeMemoryAccessResult
  {
    bool accessible = false;
    std::string reason;
  };

  struct RuntimeMemoryRegionResult
  {
    bool found = false;
    std::uintptr_t address = 0;
    std::size_t size = 0;
    std::string reason;
  };

  int currentProcessId();
  RuntimeMemoryAccessResult openProcessMemoryAccess(int processId);
  RuntimeMemoryRegionResult findFirstReadableProcessMemoryRegion(int processId);
  RuntimeMemoryReadResult readProcessMemory(
    int processId,
    std::uintptr_t address,
    std::size_t size);
  RuntimeMemoryWriteResult writeProcessMemory(
    int processId,
    std::uintptr_t address,
    const void* bytes,
    std::size_t size);
}
