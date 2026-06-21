#include <BWAPI/Runtime/RuntimeBackend.h>
#include <BWAPI/Runtime/RuntimeExecutor.h>
#include <BWAPI/Runtime/RuntimeInstallation.h>
#include <BWAPI/Runtime/RuntimeProcess.h>
#include <BWAPI/Runtime/RuntimeProcessMemory.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

using namespace BWAPI::Runtime;

namespace
{
  void printUsage()
  {
    std::cout
      << "usage: starcraft-runtime-adapter-proof [options] --bridge <dir>\n"
      << "  --product <name>         override runtime product\n"
      << "  --version <version>      override runtime version\n"
      << "  --process-id <pid>       override runtime process id\n"
      << "  --executable <path>      override runtime executable path\n"
      << "  --self                   prove attach against this CLI process\n"
      << "  --prove-read-game-state  prove live state reads by finding a changing runtime counter\n"
      << "  --prove-read-units       prove live unit reads by finding a BWAPI-compatible CUnit array\n"
      << "  --self-unit-fixture      allocate a self-test CUnit array before --prove-read-units\n"
      << "  --prove-battle-net-policy\n"
      << "                           prove Battle.net launch/attach policy preflight has no blockers\n"
      << "  --state-sample-delay-ms <ms>\n"
      << "                           delay between live state samples (default: 250)\n"
      << "  --state-max-scan-mb <mb> maximum readable writable memory to sample (default: 128)\n"
      << "  --unit-scan-timeout-ms <ms>\n"
      << "                           maximum time for --prove-read-units scan (default: 15000)\n"
      << "  --bridge <dir>           write adapter proof ready file\n"
      << "  --help                   show this help\n";
  }

  bool parsePositiveInt(const std::string& value, int& output)
  {
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0' || parsed <= 0 || parsed > std::numeric_limits<int>::max())
      return false;
    output = static_cast<int>(parsed);
    return true;
  }

  const RuntimeExecutorBehaviorProof* findProof(const std::string& id)
  {
    for (const RuntimeExecutorBehaviorProof& proof : requiredRuntimeExecutorBehaviorProofs())
    {
      if (proof.id == id)
        return &proof;
    }
    return nullptr;
  }

  struct LiveCounterProof
  {
    bool passed = false;
    std::uintptr_t address = 0;
    std::uint32_t first = 0;
    std::uint32_t second = 0;
    std::uint32_t third = 0;
    std::string reason;
  };

  struct LiveUnitsProof
  {
    bool passed = false;
    std::uintptr_t address = 0;
    std::size_t recordSize = 0;
    std::size_t sampledRecords = 0;
    std::size_t activeRecords = 0;
    std::string reason;
  };

  struct BattleNetPolicyProof
  {
    bool passed = false;
    RuntimeLaunchDiagnosis diagnosis;
    std::string reason;
  };

  std::uint32_t readU32(const std::vector<unsigned char>& bytes, std::size_t offset)
  {
    std::uint32_t value = 0;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
  }

  std::uint16_t readU16(const std::vector<unsigned char>& bytes, std::size_t offset)
  {
    std::uint16_t value = 0;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
  }

  std::int16_t readS16(const std::vector<unsigned char>& bytes, std::size_t offset)
  {
    std::int16_t value = 0;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
  }

  std::uint64_t readU64(const std::vector<unsigned char>& bytes, std::size_t offset)
  {
    std::uint64_t value = 0;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
  }

  bool addressFits(std::uint64_t address)
  {
    return address <= static_cast<std::uint64_t>(std::numeric_limits<std::uintptr_t>::max());
  }

  bool plausibleCounterDelta(std::uint32_t before, std::uint32_t after)
  {
    if (after <= before)
      return false;
    const std::uint32_t delta = after - before;
    return delta <= 10000;
  }

  bool timedOut(const std::chrono::steady_clock::time_point& deadline)
  {
    return std::chrono::steady_clock::now() >= deadline;
  }

  bool regionContains(const RuntimeMemoryRegion& region, std::uintptr_t address, std::size_t size)
  {
    if (size == 0 || address < region.address)
      return false;
    const std::uintptr_t offset = address - region.address;
    return offset <= region.size && size <= region.size - offset;
  }

  bool readableAddress(
    const std::vector<RuntimeMemoryRegion>& regions,
    std::uintptr_t address,
    std::size_t size)
  {
    for (const RuntimeMemoryRegion& region : regions)
    {
      if (region.readable && regionContains(region, address, size))
        return true;
    }
    return false;
  }

  bool readablePointerValue(
    const std::vector<RuntimeMemoryRegion>& regions,
    std::uint64_t address,
    std::size_t size)
  {
    if (address == 0 || !addressFits(address))
      return false;
    return readableAddress(regions, static_cast<std::uintptr_t>(address), size);
  }

  bool plausibleSpritePointer(
    const std::vector<unsigned char>& bytes,
    std::size_t offset,
    const std::vector<RuntimeMemoryRegion>& regions,
    bool requireReadableSprite)
  {
    const std::uint32_t sprite32 = readU32(bytes, offset + 0x0c);
    if (!requireReadableSprite)
      return sprite32 != 0;

    if (readablePointerValue(regions, sprite32, 16))
      return true;
    if (offset + 0x14 <= bytes.size() && readablePointerValue(regions, readU64(bytes, offset + 0x0c), 16))
      return true;

    return false;
  }

  bool plausibleClassicCUnitRecord(
    const std::vector<unsigned char>& bytes,
    std::size_t offset,
    std::size_t recordSize,
    const std::vector<RuntimeMemoryRegion>& regions,
    bool requireReadableSprite)
  {
    if (recordSize < 0x66 || offset + recordSize > bytes.size())
      return false;

    const std::uint32_t hitPoints = readU32(bytes, offset + 0x08);
    const std::int16_t x = readS16(bytes, offset + 0x28);
    const std::int16_t y = readS16(bytes, offset + 0x2a);
    const unsigned char player = bytes[offset + 0x4c];
    const unsigned char order = bytes[offset + 0x4d];
    const std::uint16_t unitType = readU16(bytes, offset + 0x64);

    return hitPoints >= 256
      && hitPoints <= 1000000
      && x > 0
      && x <= 16384
      && y > 0
      && y <= 16384
      && player < 12
      && order < 190
      && unitType < 256
      && plausibleSpritePointer(bytes, offset, regions, requireReadableSprite);
  }

  LiveUnitsProof scoreClassicCUnitArray(
    const std::vector<unsigned char>& bytes,
    std::uintptr_t baseAddress,
    std::size_t offset,
    std::size_t recordSize,
    const std::vector<RuntimeMemoryRegion>& regions,
    bool requireReadableSprite)
  {
    constexpr std::size_t minActiveRecords = 8;
    constexpr std::size_t maxSampledRecords = 1700;

    LiveUnitsProof proof;
    proof.address = baseAddress + offset;
    proof.recordSize = recordSize;

    const std::size_t availableRecords = (bytes.size() - offset) / recordSize;
    proof.sampledRecords = std::min(maxSampledRecords, availableRecords);
    for (std::size_t i = 0; i < proof.sampledRecords; ++i)
    {
      if (plausibleClassicCUnitRecord(
            bytes,
            offset + i * recordSize,
            recordSize,
            regions,
            requireReadableSprite))
        ++proof.activeRecords;

      if (proof.activeRecords >= minActiveRecords)
      {
        proof.passed = true;
        return proof;
      }
    }

    proof.reason = "candidate CUnit array did not contain enough active BWAPI-compatible records";
    return proof;
  }

  LiveUnitsProof proveClassicUnitArrayInBytes(
    const std::vector<unsigned char>& bytes,
    std::uintptr_t baseAddress,
    const std::vector<RuntimeMemoryRegion>& regions,
    const std::chrono::steady_clock::time_point& deadline,
    bool& scanTimedOut,
    bool requireReadableSprite)
  {
    const std::array<std::size_t, 3> recordSizes = { 336, 512, 672 };
    for (std::size_t offset = 0; offset < bytes.size(); offset += 8)
    {
      if ((offset % (64 * 1024)) == 0 && timedOut(deadline))
      {
        scanTimedOut = true;
        return {};
      }

      for (std::size_t recordSize : recordSizes)
      {
        if (offset + recordSize * 4 > bytes.size())
          continue;
        if (!plausibleClassicCUnitRecord(bytes, offset, recordSize, regions, requireReadableSprite))
          continue;

        LiveUnitsProof proof = scoreClassicCUnitArray(
          bytes,
          baseAddress,
          offset,
          recordSize,
          regions,
          requireReadableSprite);
        if (proof.passed)
          return proof;
      }
    }

    return {};
  }

  LiveUnitsProof proveClassicUnitVectorInBytes(
    int processId,
    const std::vector<RuntimeMemoryRegion>& regions,
    const std::vector<unsigned char>& bytes,
    std::uintptr_t baseAddress,
    const std::chrono::steady_clock::time_point& deadline,
    bool& scanTimedOut)
  {
    const std::array<std::size_t, 3> recordSizes = { 336, 512, 672 };
    constexpr std::size_t maxVectorBytes = 32 * 1024 * 1024;

    for (std::size_t offset = 0; offset + sizeof(std::uint64_t) * 3 <= bytes.size(); offset += 8)
    {
      if ((offset % (64 * 1024)) == 0 && timedOut(deadline))
      {
        scanTimedOut = true;
        return {};
      }

      const std::uintptr_t begin = static_cast<std::uintptr_t>(readU64(bytes, offset));
      const std::uintptr_t end = static_cast<std::uintptr_t>(readU64(bytes, offset + 8));
      const std::uintptr_t capacity = static_cast<std::uintptr_t>(readU64(bytes, offset + 16));
      if (begin == 0 || end <= begin || capacity < end)
        continue;
      const std::size_t usedBytes = static_cast<std::size_t>(end - begin);
      if (usedBytes == 0 || usedBytes > maxVectorBytes)
        continue;
      if (!readableAddress(regions, begin, std::min<std::size_t>(usedBytes, 4096)))
        continue;

      for (std::size_t recordSize : recordSizes)
      {
        if (usedBytes < recordSize * 4 || usedBytes % recordSize != 0)
          continue;

        RuntimeMemoryReadResult read = readProcessMemory(processId, begin, std::min<std::size_t>(usedBytes, recordSize * 64));
        if (!read.success || read.bytesRead < recordSize * 4)
          continue;

        LiveUnitsProof proof = scoreClassicCUnitArray(read.bytes, begin, 0, recordSize, regions, true);
        if (proof.passed)
          return proof;
      }
    }

    (void)baseAddress;
    return {};
  }

  LiveUnitsProof proveLiveUnitsRead(
    int processId,
    std::size_t maxScanBytes,
    int scanTimeoutMs)
  {
    RuntimeMemoryRegionListResult regions = listProcessMemoryRegions(processId);
    if (!regions.success)
      return { false, 0, 0, 0, 0, regions.reason };

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(scanTimeoutMs);
    const std::size_t maxRegionBytes = 2 * 1024 * 1024;
    std::size_t scanned = 0;
    for (const RuntimeMemoryRegion& region : regions.regions)
    {
      if (timedOut(deadline))
        return { false, 0, 0, 0, 0, "unit array scan timed out before proof" };

      if (!region.readable || !region.writable || region.executable || region.size < 336 * 4)
        continue;
      if (scanned >= maxScanBytes)
        break;

      const std::size_t bytesToRead = std::min(region.size, std::min(maxRegionBytes, maxScanBytes - scanned));
      RuntimeMemoryReadResult read = readProcessMemory(processId, region.address, bytesToRead);
      if (!read.success || read.bytesRead < 336 * 4)
        continue;

      bool scanTimedOut = false;
      LiveUnitsProof vectorProof = proveClassicUnitVectorInBytes(
        processId,
        regions.regions,
        read.bytes,
        region.address,
        deadline,
        scanTimedOut);
      if (vectorProof.passed)
        return vectorProof;
      if (scanTimedOut)
        return { false, 0, 0, 0, 0, "unit array scan timed out before proof" };

      LiveUnitsProof arrayProof = proveClassicUnitArrayInBytes(
        read.bytes,
        region.address,
        regions.regions,
        deadline,
        scanTimedOut,
        true);
      if (arrayProof.passed)
        return arrayProof;
      if (scanTimedOut)
        return { false, 0, 0, 0, 0, "unit array scan timed out before proof" };

      scanned += read.bytesRead;
    }

    return { false, 0, 0, 0, 0, "no BWAPI-compatible live CUnit array candidate found" };
  }

  LiveCounterProof proveLiveCounterRead(
    int processId,
    int sampleDelayMs,
    std::size_t maxScanBytes)
  {
    struct Candidate
    {
      std::uintptr_t address = 0;
      std::uint32_t first = 0;
      std::uint32_t second = 0;
    };
    struct Snapshot
    {
      std::uintptr_t address = 0;
      std::vector<unsigned char> bytes;
    };

    RuntimeMemoryRegionListResult regions = listProcessMemoryRegions(processId);
    if (!regions.success)
      return { false, 0, 0, 0, 0, regions.reason };

    std::vector<Snapshot> snapshots;
    const std::size_t maxRegionBytes = 4 * 1024 * 1024;
    std::size_t scanned = 0;

    for (const RuntimeMemoryRegion& region : regions.regions)
    {
      if (!region.readable || !region.writable || region.size < sizeof(std::uint32_t))
        continue;
      if (scanned >= maxScanBytes)
        break;

      const std::size_t bytesToRead = std::min(region.size, std::min(maxRegionBytes, maxScanBytes - scanned));
      RuntimeMemoryReadResult first = readProcessMemory(processId, region.address, bytesToRead);
      if (!first.success || first.bytesRead < sizeof(std::uint32_t))
        continue;

      Snapshot snapshot;
      snapshot.address = region.address;
      snapshot.bytes = std::move(first.bytes);
      snapshots.push_back(std::move(snapshot));
      scanned += first.bytesRead;
    }

    if (snapshots.empty())
      return { false, 0, 0, 0, 0, "no readable writable runtime memory snapshots could be captured" };

    std::this_thread::sleep_for(std::chrono::milliseconds(sampleDelayMs));

    std::vector<Candidate> candidates;
    for (const Snapshot& snapshot : snapshots)
    {
      RuntimeMemoryReadResult second = readProcessMemory(processId, snapshot.address, snapshot.bytes.size());
      if (!second.success || second.bytesRead != snapshot.bytes.size())
        continue;

      for (std::size_t offset = 0; offset + sizeof(std::uint32_t) <= snapshot.bytes.size(); offset += sizeof(std::uint32_t))
      {
        const std::uint32_t firstValue = readU32(snapshot.bytes, offset);
        const std::uint32_t secondValue = readU32(second.bytes, offset);
        if (plausibleCounterDelta(firstValue, secondValue))
        {
          Candidate candidate;
          candidate.address = snapshot.address + offset;
          candidate.first = firstValue;
          candidate.second = secondValue;
          candidates.push_back(candidate);
          if (candidates.size() >= 4096)
            break;
        }
      }
      if (candidates.size() >= 4096)
        break;
    }

    if (candidates.empty())
      return { false, 0, 0, 0, 0, "no increasing 32-bit runtime state counter candidate found" };

    std::this_thread::sleep_for(std::chrono::milliseconds(sampleDelayMs));

    for (const Candidate& candidate : candidates)
    {
      RuntimeMemoryReadResult third = readProcessMemory(processId, candidate.address, sizeof(std::uint32_t));
      if (!third.success || third.bytesRead != sizeof(std::uint32_t))
        continue;

      const std::uint32_t thirdValue = readU32(third.bytes, 0);
      if (plausibleCounterDelta(candidate.second, thirdValue))
        return { true, candidate.address, candidate.first, candidate.second, thirdValue, {} };
    }

    return { false, 0, 0, 0, 0, "candidate counters did not keep increasing across the third sample" };
  }

  struct SelfUnitFixture
  {
    std::vector<std::array<unsigned char, 336>> records;
    std::vector<std::array<unsigned char, 64>> sprites;
  };

  void writeU32(std::array<unsigned char, 336>& bytes, std::size_t offset, std::uint32_t value)
  {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
  }

  void writeU64(std::array<unsigned char, 336>& bytes, std::size_t offset, std::uint64_t value)
  {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
  }

  void writeU16(std::array<unsigned char, 336>& bytes, std::size_t offset, std::uint16_t value)
  {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
  }

  void writeS16(std::array<unsigned char, 336>& bytes, std::size_t offset, std::int16_t value)
  {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
  }

  SelfUnitFixture makeSelfUnitFixture()
  {
    SelfUnitFixture fixture;
    fixture.records.resize(16);
    fixture.sprites.resize(fixture.records.size());
    for (std::size_t i = 0; i < fixture.records.size(); ++i)
    {
      auto& record = fixture.records[i];
      record.fill(0);
      fixture.sprites[i].fill(static_cast<unsigned char>(0xa0 + i));
      writeU32(record, 0x08, 256u * static_cast<std::uint32_t>(40 + i));
      writeU64(
        record,
        0x0c,
        static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(fixture.sprites[i].data())));
      writeS16(record, 0x28, static_cast<std::int16_t>(128 + i * 32));
      writeS16(record, 0x2a, static_cast<std::int16_t>(96 + i * 16));
      record[0x4c] = static_cast<unsigned char>(i % 8);
      record[0x4d] = static_cast<unsigned char>(3 + i);
      writeU16(record, 0x64, static_cast<std::uint16_t>(i % 228));
    }
    return fixture;
  }

  std::string firstBlocker(const RuntimeLaunchDiagnosis& diagnosis)
  {
    if (diagnosis.blockers.empty())
      return {};
    return diagnosis.blockers.front();
  }

  BattleNetPolicyProof proveBattleNetPolicy(const RuntimeEnvironment& environment)
  {
    BattleNetPolicyProof proof;
    if (environment.product != Product::StarCraftRemastered)
    {
      proof.reason = "Battle.net policy proof requires StarCraft Remastered";
      return proof;
    }

    const RuntimeInstallation installation = detectStarCraftInstallation(environment);
    RuntimeLaunchResult launchResult;
    launchResult.running = environment.processId > 0;
    launchResult.processId = environment.processId;
    launchResult.reason = launchResult.running
      ? "adapter proof selected an existing runtime process id"
      : "adapter proof did not select a runtime process id";

    const RuntimeEvidence evidence = collectRuntimeEvidence(installation, launchResult);
    proof.diagnosis = evidence.diagnosis;

    if (!installation.found)
      proof.reason = installation.reason.empty() ? "StarCraft Remastered installation is not configured" : installation.reason;
    else if (!evidence.executable.exists)
      proof.reason = evidence.executable.reason.empty() ? "StarCraft executable is missing" : evidence.executable.reason;
    else if (!proof.diagnosis.readyForAttach)
      proof.reason = proof.diagnosis.status.empty() ? "runtime is not ready for attach" : proof.diagnosis.status;
    else if (proof.diagnosis.gameProcessCount != 1)
      proof.reason = "expected exactly one StarCraft game process";
    else if (proof.diagnosis.multipleBattleNetMainVisible)
      proof.reason = "multiple Battle.net main processes are visible";
    else if (proof.diagnosis.multipleBattleNetHandoffsVisible)
      proof.reason = "multiple Battle.net StarCraft handoff processes are visible";
    else if (!proof.diagnosis.blockers.empty())
      proof.reason = firstBlocker(proof.diagnosis);
    else
      proof.passed = true;

    return proof;
  }
}

int main(int argc, char** argv)
{
  RuntimeEnvironment environment = RuntimeEnvironment::detectHost();
  bool self = false;
  bool proveReadGameState = false;
  bool proveReadUnits = false;
  bool selfUnitFixture = false;
  bool proveBattleNetPolicyFlag = false;
  int stateSampleDelayMs = 250;
  std::size_t stateMaxScanBytes = 128 * 1024 * 1024;
  int unitScanTimeoutMs = 15000;

  for (int i = 1; i < argc; ++i)
  {
    const std::string arg = argv[i];
    if (arg == "--help" || arg == "-h")
    {
      printUsage();
      return 0;
    }
    if (arg == "--self")
    {
      self = true;
      continue;
    }
    if (arg == "--prove-read-game-state")
    {
      proveReadGameState = true;
      continue;
    }
    if (arg == "--prove-read-units")
    {
      proveReadUnits = true;
      continue;
    }
    if (arg == "--self-unit-fixture")
    {
      selfUnitFixture = true;
      continue;
    }
    if (arg == "--prove-battle-net-policy")
    {
      proveBattleNetPolicyFlag = true;
      continue;
    }
    if (arg == "--state-sample-delay-ms")
    {
      if (i + 1 >= argc || !parsePositiveInt(argv[++i], stateSampleDelayMs))
      {
        std::cerr << "--state-sample-delay-ms requires a positive integer\n";
        return 64;
      }
      continue;
    }
    if (arg == "--state-max-scan-mb")
    {
      int megabytes = 0;
      if (i + 1 >= argc || !parsePositiveInt(argv[++i], megabytes))
      {
        std::cerr << "--state-max-scan-mb requires a positive integer\n";
        return 64;
      }
      stateMaxScanBytes = static_cast<std::size_t>(megabytes) * 1024 * 1024;
      continue;
    }
    if (arg == "--unit-scan-timeout-ms")
    {
      if (i + 1 >= argc || !parsePositiveInt(argv[++i], unitScanTimeoutMs))
      {
        std::cerr << "--unit-scan-timeout-ms requires a positive integer\n";
        return 64;
      }
      continue;
    }
    if (arg == "--product")
    {
      if (i + 1 >= argc)
      {
        std::cerr << "--product requires a value\n";
        return 64;
      }
      environment.product = parseProduct(argv[++i]);
      continue;
    }
    if (arg == "--version")
    {
      if (i + 1 >= argc)
      {
        std::cerr << "--version requires a value\n";
        return 64;
      }
      environment.version = argv[++i];
      continue;
    }
    if (arg == "--process-id")
    {
      if (i + 1 >= argc || !parsePositiveInt(argv[++i], environment.processId))
      {
        std::cerr << "--process-id requires a positive integer\n";
        return 64;
      }
      continue;
    }
    if (arg == "--executable")
    {
      if (i + 1 >= argc)
      {
        std::cerr << "--executable requires a path\n";
        return 64;
      }
      environment.executablePath = argv[++i];
      continue;
    }
    if (arg == "--bridge")
    {
      if (i + 1 >= argc)
      {
        std::cerr << "--bridge requires a directory\n";
        return 64;
      }
      environment.executorBridgePath = argv[++i];
      continue;
    }

    std::cerr << "unknown argument: " << arg << '\n';
    return 64;
  }

  if (self)
  {
    environment.processId = currentProcessId();
    if (environment.product == Product::Unknown)
      environment.product = Product::StarCraftRemastered;
    if (environment.version.empty())
      environment.version = "self-test";
  }
  else
  {
    environment = resolveRuntimeEnvironment(environment);
  }

  if (environment.product == Product::Unknown)
  {
    std::cerr << "runtime product is required\n";
    return 64;
  }
  if (environment.version.empty())
  {
    std::cerr << "runtime version is required\n";
    return 64;
  }
  if (environment.executorBridgePath.empty())
  {
    std::cerr << "runtime executor bridge directory is required\n";
    return 64;
  }

  const RuntimeProcessOpenResult attach = openRuntimeProcess(environment);
  std::cout << "attach.opened=" << (attach.opened ? "true" : "false") << '\n';
  if (!attach.reason.empty())
    std::cout << "attach.reason=" << attach.reason << '\n';
  if (!attach.opened)
    return 2;

  const RuntimeMemoryAccessResult memoryAccess = openProcessMemoryAccess(environment.processId);
  std::cout << "attach.memory_accessible=" << (memoryAccess.accessible ? "true" : "false") << '\n';
  if (!memoryAccess.reason.empty())
    std::cout << "attach.memory_access.reason=" << memoryAccess.reason << '\n';
  if (!memoryAccess.accessible)
    return 3;

  LiveCounterProof readGameStateProof;
  LiveUnitsProof readUnitsProof;
  BattleNetPolicyProof battleNetPolicyProof;
  SelfUnitFixture unitFixture;
  if (self && selfUnitFixture)
    unitFixture = makeSelfUnitFixture();

  if (proveReadGameState)
  {
    readGameStateProof = proveLiveCounterRead(
      environment.processId,
      stateSampleDelayMs,
      stateMaxScanBytes);
    std::cout << "read_game_state.live_counter=" << (readGameStateProof.passed ? "true" : "false") << '\n';
    if (readGameStateProof.passed)
    {
      std::cout << "read_game_state.address=0x" << std::hex << readGameStateProof.address << std::dec << '\n';
      std::cout << "read_game_state.sample.0=" << readGameStateProof.first << '\n';
      std::cout << "read_game_state.sample.1=" << readGameStateProof.second << '\n';
      std::cout << "read_game_state.sample.2=" << readGameStateProof.third << '\n';
    }
    if (!readGameStateProof.reason.empty())
      std::cout << "read_game_state.reason=" << readGameStateProof.reason << '\n';
    if (!readGameStateProof.passed)
      return 4;
  }

  if (proveReadUnits)
  {
    readUnitsProof = proveLiveUnitsRead(environment.processId, stateMaxScanBytes, unitScanTimeoutMs);
    std::cout << "read_units.unit_array=" << (readUnitsProof.passed ? "true" : "false") << '\n';
    if (readUnitsProof.passed)
    {
      std::cout << "read_units.address=0x" << std::hex << readUnitsProof.address << std::dec << '\n';
      std::cout << "read_units.record_size=" << readUnitsProof.recordSize << '\n';
      std::cout << "read_units.sampled_records=" << readUnitsProof.sampledRecords << '\n';
      std::cout << "read_units.active_records=" << readUnitsProof.activeRecords << '\n';
    }
    if (!readUnitsProof.reason.empty())
      std::cout << "read_units.reason=" << readUnitsProof.reason << '\n';
    if (!readUnitsProof.passed)
      return 6;
  }

  if (proveBattleNetPolicyFlag)
  {
    battleNetPolicyProof = proveBattleNetPolicy(environment);
    std::cout << "battle_net_policy.ready_for_attach="
              << (battleNetPolicyProof.diagnosis.readyForAttach ? "true" : "false") << '\n';
    std::cout << "battle_net_policy.status=" << battleNetPolicyProof.diagnosis.status << '\n';
    std::cout << "battle_net_policy.game_process_count="
              << battleNetPolicyProof.diagnosis.gameProcessCount << '\n';
    std::cout << "battle_net_policy.battle_net_main_count="
              << battleNetPolicyProof.diagnosis.battleNetMainCount << '\n';
    std::cout << "battle_net_policy.battle_net_handoff_count="
              << battleNetPolicyProof.diagnosis.battleNetHandoffCount << '\n';
    std::cout << "battle_net_policy.battle_net_support_count="
              << battleNetPolicyProof.diagnosis.battleNetSupportCount << '\n';
    std::cout << "battle_net_policy.blocker_count="
              << battleNetPolicyProof.diagnosis.blockers.size() << '\n';
    if (!battleNetPolicyProof.reason.empty())
      std::cout << "battle_net_policy.reason=" << battleNetPolicyProof.reason << '\n';
    if (!battleNetPolicyProof.passed)
      return 5;
  }

  std::error_code error;
  std::filesystem::create_directories(environment.executorBridgePath, error);
  if (error)
  {
    std::cerr << "unable to create bridge directory: " << error.message() << '\n';
    return 1;
  }

  const RuntimeExecutorBehaviorProof* attachProof = findProof("attach");
  if (attachProof == nullptr)
  {
    std::cerr << "attach proof definition is missing\n";
    return 1;
  }

  const RuntimeExecutorBehaviorProof* readGameStateBehaviorProof = findProof("read-game-state");
  if (proveReadGameState && readGameStateBehaviorProof == nullptr)
  {
    std::cerr << "read-game-state proof definition is missing\n";
    return 1;
  }

  const RuntimeExecutorBehaviorProof* readUnitsBehaviorProof = findProof("read-units");
  if (proveReadUnits && readUnitsBehaviorProof == nullptr)
  {
    std::cerr << "read-units proof definition is missing\n";
    return 1;
  }

  const RuntimeExecutorBehaviorProof* battleNetPolicyBehaviorProof = findProof("battle-net-policy");
  if (proveBattleNetPolicyFlag && battleNetPolicyBehaviorProof == nullptr)
  {
    std::cerr << "battle-net-policy proof definition is missing\n";
    return 1;
  }

  const std::filesystem::path readyPath =
    std::filesystem::path(environment.executorBridgePath) / RuntimeExecutorBridgeReadyFile;
  std::ofstream ready(readyPath);
  if (!ready)
  {
    std::cerr << "unable to write bridge ready file: " << readyPath.string() << '\n';
    return 1;
  }

  ready << "protocol=" << RuntimeExecutorBridgeProtocol << '\n';
  ready << "product=" << toString(environment.product) << '\n';
  ready << "version=" << environment.version << '\n';
  ready << "executor=starcraft-api-attach-proof\n";
  ready << "mode=" << RuntimeExecutorBridgeValidatedAdapterMode << '\n';
  ready << "process_id=" << environment.processId << '\n';
  ready << "executable=" << environment.executablePath << '\n';
  ready << "contract.binding.shared-memory-client-transport=transport|proof.attach=passed\n";
  ready << attachProof->readyFileLine << '\n';
  if (proveReadGameState)
  {
    ready << "proof.read_game_state.address=0x" << std::hex << readGameStateProof.address << std::dec << '\n';
    ready << "proof.read_game_state.samples="
          << readGameStateProof.first << ','
          << readGameStateProof.second << ','
          << readGameStateProof.third << '\n';
    ready << readGameStateBehaviorProof->readyFileLine << '\n';
  }
  if (proveReadUnits)
  {
    ready << "proof.read_units.address=0x" << std::hex << readUnitsProof.address << std::dec << '\n';
    ready << "proof.read_units.record_size=" << readUnitsProof.recordSize << '\n';
    ready << "proof.read_units.active_records=" << readUnitsProof.activeRecords << '\n';
    ready << "contract.binding.BW::BWDATA::UnitNodeTable=data-address|proof.read_units=passed\n";
    ready << "contract.structure.BW::CUnit=" << readUnitsProof.recordSize << "|proof.read_units=passed\n";
    ready << "contract.field.BW::CUnit.id=100|2|proof.read_units=passed\n";
    ready << "contract.field.BW::CUnit.position=40|4|proof.read_units=passed\n";
    ready << "contract.field.BW::CUnit.hitPoints=8|4|proof.read_units=passed\n";
    ready << "contract.field.BW::CUnit.order=77|1|proof.read_units=passed\n";
    ready << "contract.field.BW::CUnit.player=76|1|proof.read_units=passed\n";
    ready << readUnitsBehaviorProof->readyFileLine << '\n';
  }
  if (proveBattleNetPolicyFlag)
  {
    ready << "proof.battle_net_policy.status=" << battleNetPolicyProof.diagnosis.status << '\n';
    ready << "proof.battle_net_policy.game_process_count="
          << battleNetPolicyProof.diagnosis.gameProcessCount << '\n';
    ready << "proof.battle_net_policy.blocker_count="
          << battleNetPolicyProof.diagnosis.blockers.size() << '\n';
    ready << battleNetPolicyBehaviorProof->readyFileLine << '\n';
  }

  std::cout << "bridge.ready=" << readyPath.string() << '\n';
  std::cout << attachProof->readyFileLine << '\n';
  if (proveReadGameState)
    std::cout << readGameStateBehaviorProof->readyFileLine << '\n';
  if (proveReadUnits)
    std::cout << readUnitsBehaviorProof->readyFileLine << '\n';
  if (proveBattleNetPolicyFlag)
    std::cout << battleNetPolicyBehaviorProof->readyFileLine << '\n';
  return 0;
}
