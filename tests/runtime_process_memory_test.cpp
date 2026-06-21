#include <BWAPI/Runtime/RuntimeProcessMemory.h>

#include <cassert>
#include <cstdint>
#include <cstring>

using namespace BWAPI::Runtime;

int main()
{
  std::uint64_t marker = 0x1122334455667788ULL;

  RuntimeMemoryReadResult read = readProcessMemory(
    currentProcessId(),
    reinterpret_cast<std::uintptr_t>(&marker),
    sizeof(marker));

  assert(read.success);
  assert(read.bytesRead == sizeof(marker));
  assert(read.bytes.size() == sizeof(marker));

  std::uint64_t copied = 0;
  std::memcpy(&copied, read.bytes.data(), sizeof(copied));
  assert(copied == marker);

  const std::uint64_t replacement = 0x8877665544332211ULL;
  RuntimeMemoryWriteResult write = writeProcessMemory(
    currentProcessId(),
    reinterpret_cast<std::uintptr_t>(&marker),
    &replacement,
    sizeof(replacement));
  assert(write.success);
  assert(write.bytesWritten == sizeof(replacement));
  assert(marker == replacement);

  RuntimeMemoryReadResult invalidPid = readProcessMemory(0, reinterpret_cast<std::uintptr_t>(&marker), sizeof(marker));
  assert(!invalidPid.success);
  assert(!invalidPid.reason.empty());

  RuntimeMemoryReadResult invalidAddress = readProcessMemory(currentProcessId(), 0, sizeof(marker));
  assert(!invalidAddress.success);
  assert(!invalidAddress.reason.empty());

  RuntimeMemoryWriteResult invalidWrite = writeProcessMemory(currentProcessId(), reinterpret_cast<std::uintptr_t>(&marker), nullptr, sizeof(marker));
  assert(!invalidWrite.success);
  assert(!invalidWrite.reason.empty());

  RuntimeMemoryRegionResult region = findFirstReadableProcessMemoryRegion(currentProcessId());
  assert(region.found);
  assert(region.address != 0);
  assert(region.size > 0);

  RuntimeMemoryReadResult regionRead = readProcessMemory(currentProcessId(), region.address, 1);
  assert(regionRead.success);
  assert(regionRead.bytesRead == 1);

  return 0;
}
