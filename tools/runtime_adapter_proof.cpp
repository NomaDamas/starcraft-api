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
#include <utility>
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
      << "  --prove-active-match-state\n"
      << "                           prove the target is in an active match/replay, not menu/login\n"
      << "  --prove-read-units       prove live unit reads by finding a BWAPI-compatible CUnit array\n"
      << "  --self-unit-fixture      allocate a self-test CUnit array before --prove-read-units\n"
      << "  --prove-battle-net-policy\n"
      << "                           prove Battle.net launch/attach policy preflight has no blockers\n"
      << "  --state-sample-delay-ms <ms>\n"
      << "                           delay between live state samples (default: 250)\n"
      << "  --state-scan-timeout-ms <ms>\n"
      << "                           maximum time for --prove-read-game-state scan (default: 30000)\n"
      << "  --state-max-scan-mb <mb> maximum readable writable memory to sample (default: 128)\n"
      << "  --unit-max-scan-mb <mb>  maximum readable writable memory to scan for units\n"
      << "                           (default: --state-max-scan-mb)\n"
      << "  --unit-scan-timeout-ms <ms>\n"
      << "                           maximum time for --prove-read-units scan (default: 15000)\n"
      << "  --unit-scan-diagnostics  print direct memory unit-scan counters on success/failure\n"
      << "  --unit-scan-readable-only\n"
      << "                           include readable non-writable non-executable regions in unit scans\n"
      << "  --unit-scan-vectors      also scan std::vector-like begin/end/capacity triples\n"
      << "                           after strided CUnit arrays\n"
      << "  --unit-best-dump-out <path>\n"
      << "                           dump bytes from the best CUnit candidate for offline analysis\n"
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
    std::size_t idOffset = 0;
    std::size_t positionOffset = 0;
    std::size_t hitPointsOffset = 0;
    std::size_t orderOffset = 0;
    std::size_t playerOffset = 0;
    std::size_t sampledRecords = 0;
    std::size_t activeRecords = 0;
    std::string layoutName;
    std::string reason;
  };

  struct UnitScanDiagnostics
  {
    std::size_t readableWritableRegions = 0;
    std::size_t readableOnlyRegions = 0;
    std::size_t scannedReadableOnlyRegions = 0;
    std::size_t executableReadableRegions = 0;
    std::size_t scannedRegions = 0;
    std::size_t scannedBytes = 0;
    std::size_t vectorCandidates = 0;
    std::size_t stridedCandidates = 0;
    std::size_t candidateArraysScored = 0;
    std::size_t windowCandidateArraysScored = 0;
    std::size_t fieldPlausibleRecords = 0;
    std::size_t spriteRejectedRecords = 0;
    std::size_t plausibleRecords = 0;
    std::size_t bestActiveRecords = 0;
    std::uintptr_t bestAddress = 0;
    std::size_t bestRecordSize = 0;
    std::string bestLayoutName;
    std::vector<unsigned char> bestBytes;
    bool timedOut = false;
    bool byteLimitReached = false;
  };

  struct UnitRecordLayout
  {
    const char* name = "";
    std::size_t hitPointsOffset = 0;
    std::size_t spriteOffset = 0;
    std::size_t positionOffset = 0;
    std::size_t playerOffset = 0;
    std::size_t orderOffset = 0;
    std::size_t unitTypeOffset = 0;
    std::size_t idOffset = 0;
  };

  constexpr std::array<UnitRecordLayout, 3> unitRecordLayouts = {
    UnitRecordLayout { "bwapi-classic-cunit", 0x08, 0x0c, 0x28, 0x4c, 0x4d, 0x64, 0x64 },
    UnitRecordLayout { "scr-x64-packed-cunit", 0x10, 0x14, 0x38, 0x5c, 0x5d, 0x78, 0x78 },
    UnitRecordLayout { "scr-x64-aligned-cunit", 0x10, 0x18, 0x40, 0x64, 0x65, 0x80, 0x80 }
  };

  constexpr std::array<std::size_t, 8> candidateUnitRecordSizes = {
    336, 384, 416, 432, 448, 512, 672, 768
  };

  constexpr std::size_t minActiveUnitRecords = 4;

  struct BattleNetPolicyProof
  {
    bool passed = false;
    RuntimeLaunchDiagnosis diagnosis;
    std::string reason;
  };

  LiveUnitsProof failedUnitsProof(std::string reason)
  {
    LiveUnitsProof proof;
    proof.reason = std::move(reason);
    return proof;
  }

  std::string unitScanTimeoutReason(const UnitScanDiagnostics* diagnostics)
  {
    std::string reason = "unit array scan timed out before proof";
    if (diagnostics != nullptr && diagnostics->bestActiveRecords == 0 && diagnostics->plausibleRecords == 0)
    {
      reason += "; no active in-game unit records were observed, so the attached process may be at menu/login instead of an active match";
    }
    else if (diagnostics != nullptr && diagnostics->bestActiveRecords > 0)
    {
      reason += "; best candidate active records="
        + std::to_string(diagnostics->bestActiveRecords)
        + " below required="
        + std::to_string(minActiveUnitRecords);
    }
    return reason;
  }

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

  bool writeBinaryFile(
    const std::filesystem::path& path,
    const std::vector<unsigned char>& bytes,
    std::string& reason)
  {
    std::error_code error;
    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty())
    {
      std::filesystem::create_directories(parent, error);
      if (error)
      {
        reason = "unable to create dump parent directory: " + error.message();
        return false;
      }
    }

    std::ofstream output(path, std::ios::binary);
    if (!output)
    {
      reason = "unable to open dump output";
      return false;
    }

    output.write(
      reinterpret_cast<const char*>(bytes.data()),
      static_cast<std::streamsize>(bytes.size()));
    if (!output)
    {
      reason = "unable to write dump output";
      return false;
    }

    return true;
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
    std::size_t spriteOffset,
    const std::vector<RuntimeMemoryRegion>& regions,
    bool requireReadableSprite)
  {
    const std::uint32_t sprite32 = readU32(bytes, offset + spriteOffset);
    if (!requireReadableSprite)
      return sprite32 != 0;

    if (readablePointerValue(regions, sprite32, 16))
      return true;
    if (offset + spriteOffset + sizeof(std::uint64_t) <= bytes.size()
        && readablePointerValue(regions, readU64(bytes, offset + spriteOffset), 16))
      return true;

    return false;
  }

  bool plausibleUnitRecordFields(
    const std::vector<unsigned char>& bytes,
    std::size_t offset,
    std::size_t recordSize,
    const UnitRecordLayout& layout)
  {
    const std::size_t requiredSize = std::max({
      layout.hitPointsOffset + sizeof(std::uint32_t),
      layout.spriteOffset + sizeof(std::uint64_t),
      layout.positionOffset + sizeof(std::uint32_t),
      layout.playerOffset + sizeof(unsigned char),
      layout.orderOffset + sizeof(unsigned char),
      layout.unitTypeOffset + sizeof(std::uint16_t)
    });
    if (recordSize < requiredSize || offset + recordSize > bytes.size())
      return false;

    const std::uint32_t hitPoints = readU32(bytes, offset + layout.hitPointsOffset);
    const std::int16_t x = readS16(bytes, offset + layout.positionOffset);
    const std::int16_t y = readS16(bytes, offset + layout.positionOffset + sizeof(std::int16_t));
    const unsigned char player = bytes[offset + layout.playerOffset];
    const unsigned char order = bytes[offset + layout.orderOffset];
    const std::uint16_t unitType = readU16(bytes, offset + layout.unitTypeOffset);

    return hitPoints >= 256
      && hitPoints <= 1000000
      && (hitPoints % 64) == 0
      && x >= 16
      && x <= 16384
      && y >= 16
      && y <= 16384
      && player < 12
      && order < 190
      && unitType < 256;
  }

  bool plausibleUnitRecord(
    const std::vector<unsigned char>& bytes,
    std::size_t offset,
    std::size_t recordSize,
    const UnitRecordLayout& layout,
    const std::vector<RuntimeMemoryRegion>& regions,
    bool requireReadableSprite)
  {
    return plausibleUnitRecordFields(bytes, offset, recordSize, layout)
      && plausibleSpritePointer(bytes, offset, layout.spriteOffset, regions, requireReadableSprite);
  }

  bool plausibleUnitRecordWithDiagnostics(
    const std::vector<unsigned char>& bytes,
    std::size_t offset,
    std::size_t recordSize,
    const UnitRecordLayout& layout,
    const std::vector<RuntimeMemoryRegion>& regions,
    bool requireReadableSprite,
    UnitScanDiagnostics* diagnostics)
  {
    if (!plausibleUnitRecordFields(bytes, offset, recordSize, layout))
      return false;

    if (diagnostics != nullptr)
      ++diagnostics->fieldPlausibleRecords;

    if (!plausibleSpritePointer(bytes, offset, layout.spriteOffset, regions, requireReadableSprite))
    {
      if (diagnostics != nullptr)
        ++diagnostics->spriteRejectedRecords;
      return false;
    }

    return true;
  }

  LiveUnitsProof scoreClassicCUnitArray(
    const std::vector<unsigned char>& bytes,
    std::uintptr_t baseAddress,
    std::size_t offset,
    std::size_t recordSize,
    const UnitRecordLayout& layout,
    const std::vector<RuntimeMemoryRegion>& regions,
    const std::chrono::steady_clock::time_point& deadline,
    bool& scanTimedOut,
    bool requireReadableSprite)
  {
    constexpr std::size_t maxSampledRecords = 1700;

    LiveUnitsProof proof;
    proof.address = baseAddress + offset;
    proof.recordSize = recordSize;
    proof.idOffset = layout.idOffset;
    proof.positionOffset = layout.positionOffset;
    proof.hitPointsOffset = layout.hitPointsOffset;
    proof.orderOffset = layout.orderOffset;
    proof.playerOffset = layout.playerOffset;
    proof.layoutName = layout.name;

    const std::size_t availableRecords = (bytes.size() - offset) / recordSize;
    proof.sampledRecords = std::min(maxSampledRecords, availableRecords);
    for (std::size_t i = 0; i < proof.sampledRecords; ++i)
    {
      if ((i % 16) == 0 && timedOut(deadline))
      {
        scanTimedOut = true;
        return {};
      }
      if (plausibleUnitRecord(
            bytes,
            offset + i * recordSize,
            recordSize,
            layout,
            regions,
            requireReadableSprite))
        ++proof.activeRecords;

      if (proof.activeRecords >= minActiveUnitRecords)
      {
        proof.passed = true;
        return proof;
      }
    }

    proof.reason = "candidate CUnit array did not contain enough active BWAPI-compatible records";
    return proof;
  }

  void rememberBestCandidate(
    UnitScanDiagnostics* diagnostics,
    const LiveUnitsProof& proof,
    const std::vector<unsigned char>* bytes = nullptr,
    std::size_t offset = 0,
    std::size_t recordSize = 0)
  {
    if (diagnostics == nullptr || proof.activeRecords <= diagnostics->bestActiveRecords)
      return;

    diagnostics->bestActiveRecords = proof.activeRecords;
    diagnostics->bestAddress = proof.address;
    diagnostics->bestRecordSize = proof.recordSize;
    diagnostics->bestLayoutName = proof.layoutName;
    diagnostics->bestBytes.clear();
    if (bytes != nullptr && offset < bytes->size() && recordSize > 0)
    {
      const std::size_t bytesToCopy =
        std::min(recordSize * 8, bytes->size() - offset);
      diagnostics->bestBytes.assign(
        bytes->begin() + static_cast<std::vector<unsigned char>::difference_type>(offset),
        bytes->begin() + static_cast<std::vector<unsigned char>::difference_type>(offset + bytesToCopy));
    }
  }

  LiveUnitsProof proveClassicUnitArrayInBytes(
    const std::vector<unsigned char>& bytes,
    std::uintptr_t baseAddress,
    const std::vector<RuntimeMemoryRegion>& regions,
    const std::chrono::steady_clock::time_point& deadline,
    bool& scanTimedOut,
    bool requireReadableSprite,
    UnitScanDiagnostics* diagnostics)
  {
    for (const UnitRecordLayout& layout : unitRecordLayouts)
    {
      for (std::size_t recordSize : candidateUnitRecordSizes)
      {
        if (recordSize * 4 > bytes.size())
          continue;

        constexpr std::size_t maxWindowScoresPerRecordSize = 512;
        std::size_t windowScoresForRecordSize = 0;
        std::vector<std::size_t> plausibleByResidue(recordSize, 0);
        std::vector<bool> scoredResidue(recordSize, false);
        LiveUnitsProof bestForRecordSize;
        for (std::size_t recordOffset = 0; recordOffset + recordSize <= bytes.size(); recordOffset += 8)
        {
          if ((recordOffset % (4 * 1024)) == 0 && timedOut(deadline))
          {
            scanTimedOut = true;
            return {};
          }
          if (!plausibleUnitRecordWithDiagnostics(
                bytes,
                recordOffset,
                recordSize,
                layout,
                regions,
                requireReadableSprite,
                diagnostics))
            continue;

          if (windowScoresForRecordSize < maxWindowScoresPerRecordSize)
          {
            LiveUnitsProof windowProof = scoreClassicCUnitArray(
              bytes,
              baseAddress,
              recordOffset,
              recordSize,
              layout,
              regions,
              deadline,
              scanTimedOut,
              requireReadableSprite);
            if (scanTimedOut)
              return {};
            if (diagnostics != nullptr)
            {
              ++diagnostics->candidateArraysScored;
              ++diagnostics->windowCandidateArraysScored;
              diagnostics->plausibleRecords += windowProof.activeRecords;
              if (windowProof.activeRecords > 0)
                ++diagnostics->stridedCandidates;
            }
            if (windowProof.passed)
              return windowProof;
            rememberBestCandidate(diagnostics, windowProof, &bytes, recordOffset, recordSize);
            if (windowProof.activeRecords > bestForRecordSize.activeRecords)
              bestForRecordSize = windowProof;
            ++windowScoresForRecordSize;
          }

          const std::size_t baseOffset = recordOffset % recordSize;
          ++plausibleByResidue[baseOffset];
          if (plausibleByResidue[baseOffset] < minActiveUnitRecords || scoredResidue[baseOffset])
            continue;

          LiveUnitsProof proof = scoreClassicCUnitArray(
            bytes,
            baseAddress,
            baseOffset,
            recordSize,
            layout,
            regions,
            deadline,
            scanTimedOut,
            requireReadableSprite);
          if (scanTimedOut)
            return {};
          if (diagnostics != nullptr)
          {
            ++diagnostics->candidateArraysScored;
            diagnostics->plausibleRecords += proof.activeRecords;
            if (proof.activeRecords > 0)
              ++diagnostics->stridedCandidates;
          }
          if (proof.passed)
            return proof;
          rememberBestCandidate(diagnostics, proof, &bytes, baseOffset, recordSize);
          if (proof.activeRecords > bestForRecordSize.activeRecords)
            bestForRecordSize = proof;
          scoredResidue[baseOffset] = true;
        }
        rememberBestCandidate(diagnostics, bestForRecordSize);
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
    bool& scanTimedOut,
    UnitScanDiagnostics* diagnostics)
  {
    constexpr std::size_t maxVectorBytes = 32 * 1024 * 1024;

    for (std::size_t offset = 0; offset + sizeof(std::uint64_t) * 3 <= bytes.size(); offset += 8)
    {
      if ((offset % (4 * 1024)) == 0 && timedOut(deadline))
      {
        scanTimedOut = true;
        return {};
      }

      const std::uintptr_t begin = static_cast<std::uintptr_t>(readU64(bytes, offset));
      const std::uintptr_t end = static_cast<std::uintptr_t>(readU64(bytes, offset + 8));
      const std::uintptr_t capacity = static_cast<std::uintptr_t>(readU64(bytes, offset + 16));
      if (begin == 0 || end <= begin || capacity < end)
        continue;
      if (diagnostics != nullptr)
        ++diagnostics->vectorCandidates;
      const std::size_t usedBytes = static_cast<std::size_t>(end - begin);
      if (usedBytes == 0 || usedBytes > maxVectorBytes)
        continue;
      if (!readableAddress(regions, begin, std::min<std::size_t>(usedBytes, 4096)))
        continue;

      for (const UnitRecordLayout& layout : unitRecordLayouts)
      {
        for (std::size_t recordSize : candidateUnitRecordSizes)
        {
          if (timedOut(deadline))
          {
            scanTimedOut = true;
            return {};
          }
          if (usedBytes < recordSize * 4 || usedBytes % recordSize != 0)
            continue;

          RuntimeMemoryReadResult read = readProcessMemory(processId, begin, std::min<std::size_t>(usedBytes, recordSize * 64));
          if (!read.success || read.bytesRead < recordSize * 4)
            continue;

          LiveUnitsProof proof = scoreClassicCUnitArray(
            read.bytes,
            begin,
            0,
            recordSize,
            layout,
            regions,
            deadline,
            scanTimedOut,
            true);
          if (scanTimedOut)
            return {};
          if (diagnostics != nullptr)
            ++diagnostics->candidateArraysScored;
          if (proof.passed)
            return proof;
          rememberBestCandidate(diagnostics, proof, &read.bytes, 0, recordSize);
        }
      }
    }

    (void)baseAddress;
    return {};
  }

  LiveUnitsProof proveLiveUnitsRead(
    int processId,
    std::size_t maxScanBytes,
    int scanTimeoutMs,
    bool includeReadableOnlyRegions,
    bool scanVectors,
    UnitScanDiagnostics* diagnostics)
  {
    RuntimeMemoryRegionListResult regions = listProcessMemoryRegions(processId);
    if (!regions.success)
      return failedUnitsProof(regions.reason);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(scanTimeoutMs);
    const std::size_t maxRegionBytes = 2 * 1024 * 1024;
    std::size_t scanned = 0;
    for (const RuntimeMemoryRegion& region : regions.regions)
    {
      if (timedOut(deadline))
      {
        if (diagnostics != nullptr)
          diagnostics->timedOut = true;
        return failedUnitsProof(unitScanTimeoutReason(diagnostics));
      }

      if (!region.readable || region.size < 336 * 4)
        continue;
      if (region.executable)
      {
        if (diagnostics != nullptr)
          ++diagnostics->executableReadableRegions;
        continue;
      }
      if (!region.writable)
      {
        if (diagnostics != nullptr)
          ++diagnostics->readableOnlyRegions;
        if (!includeReadableOnlyRegions)
          continue;
      }
      if (diagnostics != nullptr)
      {
        if (region.writable)
          ++diagnostics->readableWritableRegions;
        else
          ++diagnostics->scannedReadableOnlyRegions;
      }
      if (scanned >= maxScanBytes)
      {
        if (diagnostics != nullptr)
          diagnostics->byteLimitReached = true;
        break;
      }

      const std::size_t bytesToRead = std::min(region.size, std::min(maxRegionBytes, maxScanBytes - scanned));
      RuntimeMemoryReadResult read = readProcessMemory(processId, region.address, bytesToRead);
      if (!read.success || read.bytesRead < 336 * 4)
        continue;
      if (diagnostics != nullptr)
      {
        ++diagnostics->scannedRegions;
        diagnostics->scannedBytes += read.bytesRead;
      }

      bool scanTimedOut = false;
      LiveUnitsProof arrayProof = proveClassicUnitArrayInBytes(
        read.bytes,
        region.address,
        regions.regions,
        deadline,
        scanTimedOut,
        true,
        diagnostics);
      if (arrayProof.passed)
        return arrayProof;
      if (scanTimedOut)
      {
        if (diagnostics != nullptr)
          diagnostics->timedOut = true;
        return failedUnitsProof(unitScanTimeoutReason(diagnostics));
      }

      if (scanVectors)
      {
        LiveUnitsProof vectorProof = proveClassicUnitVectorInBytes(
          processId,
          regions.regions,
          read.bytes,
          region.address,
          deadline,
          scanTimedOut,
          diagnostics);
        if (vectorProof.passed)
          return vectorProof;
        if (scanTimedOut)
        {
          if (diagnostics != nullptr)
            diagnostics->timedOut = true;
          return failedUnitsProof(unitScanTimeoutReason(diagnostics));
        }
      }

      scanned += read.bytesRead;
    }

    if (diagnostics != nullptr && diagnostics->byteLimitReached)
      return failedUnitsProof("no active in-game BWAPI-compatible CUnit array candidate found before scan byte limit");
    return failedUnitsProof("no active in-game BWAPI-compatible CUnit array candidate found");
  }

  LiveCounterProof proveLiveCounterRead(
    int processId,
    int sampleDelayMs,
    std::size_t maxScanBytes,
    int scanTimeoutMs)
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

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(scanTimeoutMs);
    std::vector<Snapshot> snapshots;
    const std::size_t maxRegionBytes = 4 * 1024 * 1024;
    std::size_t scanned = 0;

    for (const RuntimeMemoryRegion& region : regions.regions)
    {
      if (timedOut(deadline))
        return { false, 0, 0, 0, 0, "state counter scan timed out before proof" };
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
      if (timedOut(deadline))
        return { false, 0, 0, 0, 0, "state counter scan timed out before proof" };
      RuntimeMemoryReadResult second = readProcessMemory(processId, snapshot.address, snapshot.bytes.size());
      if (!second.success || second.bytesRead != snapshot.bytes.size())
        continue;

      for (std::size_t offset = 0; offset + sizeof(std::uint32_t) <= snapshot.bytes.size(); offset += sizeof(std::uint32_t))
      {
        if ((offset % (4 * 1024)) == 0 && timedOut(deadline))
          return { false, 0, 0, 0, 0, "state counter scan timed out before proof" };
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
      if (timedOut(deadline))
        return { false, 0, 0, 0, 0, "state counter scan timed out before proof" };
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
  bool proveActiveMatchState = false;
  bool proveReadUnits = false;
  bool selfUnitFixture = false;
  bool proveBattleNetPolicyFlag = false;
  bool unitScanDiagnosticsFlag = false;
  bool unitScanReadableOnlyFlag = false;
  bool unitScanVectorsFlag = false;
  std::string unitBestDumpOut;
  int stateSampleDelayMs = 250;
  std::size_t stateMaxScanBytes = 128 * 1024 * 1024;
  int stateScanTimeoutMs = 30000;
  std::size_t unitMaxScanBytes = 0;
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
    if (arg == "--prove-active-match-state")
    {
      proveActiveMatchState = true;
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
    if (arg == "--unit-scan-diagnostics")
    {
      unitScanDiagnosticsFlag = true;
      continue;
    }
    if (arg == "--unit-scan-readable-only")
    {
      unitScanReadableOnlyFlag = true;
      continue;
    }
    if (arg == "--unit-scan-vectors")
    {
      unitScanVectorsFlag = true;
      continue;
    }
    if (arg == "--unit-best-dump-out")
    {
      if (i + 1 >= argc)
      {
        std::cerr << "--unit-best-dump-out requires a path\n";
        return 64;
      }
      unitBestDumpOut = argv[++i];
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
    if (arg == "--state-scan-timeout-ms")
    {
      if (i + 1 >= argc || !parsePositiveInt(argv[++i], stateScanTimeoutMs))
      {
        std::cerr << "--state-scan-timeout-ms requires a positive integer\n";
        return 64;
      }
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
    if (arg == "--unit-max-scan-mb")
    {
      int megabytes = 0;
      if (i + 1 >= argc || !parsePositiveInt(argv[++i], megabytes))
      {
        std::cerr << "--unit-max-scan-mb requires a positive integer\n";
        return 64;
      }
      unitMaxScanBytes = static_cast<std::size_t>(megabytes) * 1024 * 1024;
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
  if (unitMaxScanBytes == 0)
    unitMaxScanBytes = stateMaxScanBytes;

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
  UnitScanDiagnostics unitScanDiagnostics;
  BattleNetPolicyProof battleNetPolicyProof;
  SelfUnitFixture unitFixture;
  int proofFailureCode = 0;
  if (self && selfUnitFixture)
    unitFixture = makeSelfUnitFixture();

  if (proveReadGameState)
  {
    readGameStateProof = proveLiveCounterRead(
      environment.processId,
      stateSampleDelayMs,
      stateMaxScanBytes,
      stateScanTimeoutMs);
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
      proofFailureCode = proofFailureCode == 0 ? 4 : proofFailureCode;
  }

  if (proveActiveMatchState && self)
  {
    std::cout << "active_match_state.in_game=false\n";
    std::cout << "active_match_state.reason=self process and self fixtures cannot prove StarCraft active match state\n";
    proofFailureCode = proofFailureCode == 0 ? 7 : proofFailureCode;
  }

  if (proveReadUnits || (proveActiveMatchState && !self))
  {
    readUnitsProof = proveLiveUnitsRead(
      environment.processId,
      unitMaxScanBytes,
      unitScanTimeoutMs,
      unitScanReadableOnlyFlag,
      unitScanVectorsFlag,
      unitScanDiagnosticsFlag ? &unitScanDiagnostics : nullptr);
    if (proveReadUnits)
    {
      std::cout << "read_units.unit_array=" << (readUnitsProof.passed ? "true" : "false") << '\n';
      if (readUnitsProof.passed)
      {
        std::cout << "read_units.address=0x" << std::hex << readUnitsProof.address << std::dec << '\n';
        std::cout << "read_units.record_size=" << readUnitsProof.recordSize << '\n';
        std::cout << "read_units.layout=" << readUnitsProof.layoutName << '\n';
        std::cout << "read_units.sampled_records=" << readUnitsProof.sampledRecords << '\n';
        std::cout << "read_units.active_records=" << readUnitsProof.activeRecords << '\n';
      }
      if (!readUnitsProof.reason.empty())
        std::cout << "read_units.reason=" << readUnitsProof.reason << '\n';
    }

    if (proveActiveMatchState)
    {
      std::cout << "active_match_state.in_game=" << (readUnitsProof.passed ? "true" : "false") << '\n';
      std::cout << "active_match_state.evidence=active-unit-records\n";
      if (readUnitsProof.passed)
      {
        std::cout << "active_match_state.active_records=" << readUnitsProof.activeRecords << '\n';
        std::cout << "active_match_state.unit_array_address=0x"
                  << std::hex << readUnitsProof.address << std::dec << '\n';
      }
      if (!readUnitsProof.reason.empty())
        std::cout << "active_match_state.reason=" << readUnitsProof.reason << '\n';
    }

    if (unitScanDiagnosticsFlag)
    {
      std::cout << "read_units.scan.readable_writable_regions="
                << unitScanDiagnostics.readableWritableRegions << '\n';
      std::cout << "read_units.scan.readable_only_regions="
                << unitScanDiagnostics.readableOnlyRegions << '\n';
      std::cout << "read_units.scan.scanned_readable_only_regions="
                << unitScanDiagnostics.scannedReadableOnlyRegions << '\n';
      std::cout << "read_units.scan.executable_readable_regions="
                << unitScanDiagnostics.executableReadableRegions << '\n';
      std::cout << "read_units.scan.scanned_regions=" << unitScanDiagnostics.scannedRegions << '\n';
      std::cout << "read_units.scan.scanned_bytes=" << unitScanDiagnostics.scannedBytes << '\n';
      std::cout << "read_units.scan.vector_candidates=" << unitScanDiagnostics.vectorCandidates << '\n';
      std::cout << "read_units.scan.strided_candidates=" << unitScanDiagnostics.stridedCandidates << '\n';
      std::cout << "read_units.scan.candidate_arrays_scored=" << unitScanDiagnostics.candidateArraysScored << '\n';
      std::cout << "read_units.scan.window_candidate_arrays_scored="
                << unitScanDiagnostics.windowCandidateArraysScored << '\n';
      std::cout << "read_units.scan.field_plausible_records="
                << unitScanDiagnostics.fieldPlausibleRecords << '\n';
      std::cout << "read_units.scan.sprite_rejected_records="
                << unitScanDiagnostics.spriteRejectedRecords << '\n';
      std::cout << "read_units.scan.plausible_records=" << unitScanDiagnostics.plausibleRecords << '\n';
      std::cout << "read_units.scan.timed_out="
                << (unitScanDiagnostics.timedOut ? "true" : "false") << '\n';
      std::cout << "read_units.scan.byte_limit_reached="
                << (unitScanDiagnostics.byteLimitReached ? "true" : "false") << '\n';
      std::cout << "read_units.scan.best_active_records=" << unitScanDiagnostics.bestActiveRecords << '\n';
      if (unitScanDiagnostics.bestAddress != 0)
      {
        std::cout << "read_units.scan.best_address=0x"
                  << std::hex << unitScanDiagnostics.bestAddress << std::dec << '\n';
        std::cout << "read_units.scan.best_record_size=" << unitScanDiagnostics.bestRecordSize << '\n';
        std::cout << "read_units.scan.best_layout=" << unitScanDiagnostics.bestLayoutName << '\n';
        std::cout << "read_units.scan.best_snapshot_bytes="
                  << unitScanDiagnostics.bestBytes.size() << '\n';
      }
    }
    if (!unitBestDumpOut.empty())
    {
      const std::uintptr_t dumpAddress =
        readUnitsProof.passed ? readUnitsProof.address : unitScanDiagnostics.bestAddress;
      const std::size_t dumpRecordSize =
        readUnitsProof.passed ? readUnitsProof.recordSize : unitScanDiagnostics.bestRecordSize;
      const std::size_t dumpSize = dumpRecordSize == 0 ? 0 : dumpRecordSize * 8;
      if (dumpAddress == 0 || dumpSize == 0)
      {
        std::cout << "read_units.scan.best_dump.success=false\n";
        std::cout << "read_units.scan.best_dump.reason=no unit candidate address is available\n";
      }
      else
      {
        RuntimeMemoryReadResult dumpRead =
          readProcessMemory(environment.processId, dumpAddress, dumpSize);
        std::cout << "read_units.scan.best_dump.address=0x"
                  << std::hex << dumpAddress << std::dec << '\n';
        std::cout << "read_units.scan.best_dump.requested_bytes=" << dumpSize << '\n';
        std::cout << "read_units.scan.best_dump.read_success="
                  << (dumpRead.success ? "true" : "false") << '\n';
        std::cout << "read_units.scan.best_dump.bytes=" << dumpRead.bytesRead << '\n';
        if (!dumpRead.reason.empty())
          std::cout << "read_units.scan.best_dump.read_reason=" << dumpRead.reason << '\n';
        if (dumpRead.success)
        {
          std::string dumpReason;
          const bool dumpWritten = writeBinaryFile(unitBestDumpOut, dumpRead.bytes, dumpReason);
          std::cout << "read_units.scan.best_dump.success="
                    << (dumpWritten ? "true" : "false") << '\n';
          if (dumpWritten)
            std::cout << "read_units.scan.best_dump.path=" << unitBestDumpOut << '\n';
          if (!dumpReason.empty())
            std::cout << "read_units.scan.best_dump.reason=" << dumpReason << '\n';
        }
        else if (!unitScanDiagnostics.bestBytes.empty())
        {
          std::string dumpReason;
          const bool dumpWritten =
            writeBinaryFile(unitBestDumpOut, unitScanDiagnostics.bestBytes, dumpReason);
          std::cout << "read_units.scan.best_dump.success="
                    << (dumpWritten ? "true" : "false") << '\n';
          std::cout << "read_units.scan.best_dump.source=snapshot\n";
          if (dumpWritten)
          {
            std::cout << "read_units.scan.best_dump.path=" << unitBestDumpOut << '\n';
            std::cout << "read_units.scan.best_dump.bytes="
                      << unitScanDiagnostics.bestBytes.size() << '\n';
          }
          if (!dumpReason.empty())
            std::cout << "read_units.scan.best_dump.reason=" << dumpReason << '\n';
        }
      }
    }
    if (!readUnitsProof.passed)
      proofFailureCode = proofFailureCode == 0 ? (proveReadUnits ? 6 : 7) : proofFailureCode;
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
      proofFailureCode = proofFailureCode == 0 ? 5 : proofFailureCode;
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

  const RuntimeExecutorBehaviorProof* activeMatchStateBehaviorProof = findProof("active-match-state");
  if (proveActiveMatchState && activeMatchStateBehaviorProof == nullptr)
  {
    std::cerr << "active-match-state proof definition is missing\n";
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
  if (proveReadGameState && readGameStateProof.passed)
  {
    ready << "proof.read_game_state.address=0x" << std::hex << readGameStateProof.address << std::dec << '\n';
    ready << "proof.read_game_state.samples="
          << readGameStateProof.first << ','
          << readGameStateProof.second << ','
          << readGameStateProof.third << '\n';
    ready << readGameStateBehaviorProof->readyFileLine << '\n';
  }
  if (proveActiveMatchState && readUnitsProof.passed)
  {
    ready << "proof.active_match_state.evidence=active-unit-records\n";
    ready << "proof.active_match_state.active_records=" << readUnitsProof.activeRecords << '\n';
    ready << "proof.active_match_state.unit_array_address=0x"
          << std::hex << readUnitsProof.address << std::dec << '\n';
    ready << activeMatchStateBehaviorProof->readyFileLine << '\n';
  }
  if (proveReadUnits && readUnitsProof.passed)
  {
    ready << "proof.read_units.address=0x" << std::hex << readUnitsProof.address << std::dec << '\n';
    ready << "proof.read_units.record_size=" << readUnitsProof.recordSize << '\n';
    ready << "proof.read_units.layout=" << readUnitsProof.layoutName << '\n';
    ready << "proof.read_units.active_records=" << readUnitsProof.activeRecords << '\n';
    ready << "contract.binding.BW::BWDATA::UnitNodeTable=data-address|proof.read_units=passed\n";
    ready << "contract.structure.BW::CUnit=" << readUnitsProof.recordSize << "|proof.read_units=passed\n";
    ready << "contract.field.BW::CUnit.id=" << readUnitsProof.idOffset << "|2|proof.read_units=passed\n";
    ready << "contract.field.BW::CUnit.position=" << readUnitsProof.positionOffset << "|4|proof.read_units=passed\n";
    ready << "contract.field.BW::CUnit.hitPoints=" << readUnitsProof.hitPointsOffset << "|4|proof.read_units=passed\n";
    ready << "contract.field.BW::CUnit.order=" << readUnitsProof.orderOffset << "|1|proof.read_units=passed\n";
    ready << "contract.field.BW::CUnit.player=" << readUnitsProof.playerOffset << "|1|proof.read_units=passed\n";
    ready << readUnitsBehaviorProof->readyFileLine << '\n';
  }
  if (proveBattleNetPolicyFlag && battleNetPolicyProof.passed)
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
  if (proveReadGameState && readGameStateProof.passed)
    std::cout << readGameStateBehaviorProof->readyFileLine << '\n';
  if (proveActiveMatchState && readUnitsProof.passed)
    std::cout << activeMatchStateBehaviorProof->readyFileLine << '\n';
  if (proveReadUnits && readUnitsProof.passed)
    std::cout << readUnitsBehaviorProof->readyFileLine << '\n';
  if (proveBattleNetPolicyFlag && battleNetPolicyProof.passed)
    std::cout << battleNetPolicyBehaviorProof->readyFileLine << '\n';
  return proofFailureCode == 0 ? 0 : proofFailureCode;
}
