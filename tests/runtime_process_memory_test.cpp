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

  RuntimeMemoryReadResult invalidPid = readProcessMemory(0, reinterpret_cast<std::uintptr_t>(&marker), sizeof(marker));
  assert(!invalidPid.success);
  assert(!invalidPid.reason.empty());

  RuntimeMemoryReadResult invalidAddress = readProcessMemory(currentProcessId(), 0, sizeof(marker));
  assert(!invalidAddress.success);
  assert(!invalidAddress.reason.empty());

  return 0;
}
