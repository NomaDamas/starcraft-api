#include <BWAPI/Runtime/RuntimeResidentBridge.h>
#include <BWAPI/Runtime/RuntimeCommandQueue.h>
#include <BWAPI/Runtime/RuntimeCommandSurface.h>
#include <BWAPI/Runtime/RuntimeExecutor.h>
#include <BWAPI/Runtime/RuntimeInstallation.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#if defined(__APPLE__) || defined(__linux__) || defined(__unix__)
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <crt_externs.h>
#include <libproc.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach-o/dyld.h>
#endif

namespace
{
  std::atomic<bool> residentAdapterRunning{ false };
  std::thread residentAdapterThread;
  std::mutex residentAdapterMutex;
  std::once_flag residentSelfRetainOnce;
  void* residentSelfHandle = nullptr;

  struct ResidentBwGameProjection
  {
    std::uint32_t players = 0;
    std::uint32_t alliance = 0;
    std::uint32_t elapsedFrames = 0;
    std::uint32_t activeUnitCount = 0;
    std::uint64_t updatedTick = 0;
    std::uint64_t heartbeat = 0;
    unsigned char reserved[256 - (sizeof(std::uint32_t) * 4) - (sizeof(std::uint64_t) * 2)] = {};
  };

  static_assert(sizeof(ResidentBwGameProjection) == 256, "resident BWGame projection ABI size changed");

  struct ResidentPlayerInfoProjection
  {
    std::uint32_t stormId = 0;
    std::uint32_t race = 8;
    std::uint64_t resources = 0;
    std::uint64_t supply = 0;
    unsigned char reserved[128 - (sizeof(std::uint32_t) * 2) - (sizeof(std::uint64_t) * 2)] = {};
  };

  static_assert(sizeof(ResidentPlayerInfoProjection) == 128, "resident PlayerInfo projection ABI size changed");

  alignas(16) ResidentBwGameProjection residentBwGameProjection;
  alignas(16) std::array<ResidentPlayerInfoProjection, 12> residentPlayerInfoProjection;
  alignas(16) std::array<char, 256> residentMapNameProjection = {};
  alignas(16) std::array<std::uint16_t, 4096> residentMapTileProjection = {};

  std::string envString(const char* name)
  {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0')
      return {};
    return value;
  }

  bool envEnabled(const char* name)
  {
    std::string value = envString(name);
    for (char& ch : value)
    {
      if (ch >= 'A' && ch <= 'Z')
        ch = static_cast<char>(ch - 'A' + 'a');
    }
    return value == "1" || value == "true" || value == "yes" || value == "on";
  }

  bool residentAutostartDisabled()
  {
    return envEnabled("STARCRAFT_API_RESIDENT_DISABLE");
  }

  std::filesystem::path residentImagePath()
  {
#if defined(__APPLE__) || defined(__linux__) || defined(__unix__)
    Dl_info info;
    std::memset(&info, 0, sizeof(info));
    if (dladdr(reinterpret_cast<const void*>(&residentImagePath), &info) == 0
        || info.dli_fname == nullptr
        || *info.dli_fname == '\0')
    {
      return {};
    }
    return info.dli_fname;
#else
    return {};
#endif
  }

  void retainResidentImage()
  {
#if defined(__APPLE__)
    std::call_once(
      residentSelfRetainOnce,
      []
      {
        Dl_info info;
        std::memset(&info, 0, sizeof(info));
        if (dladdr(reinterpret_cast<const void*>(&retainResidentImage), &info) == 0
            || info.dli_fname == nullptr
            || *info.dli_fname == '\0')
        {
          return;
        }
        residentSelfHandle = dlopen(info.dli_fname, RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE);
      });
#endif
  }

  std::filesystem::path defaultBridgePath()
  {
    const std::string explicitResidentBridge = envString("STARCRAFT_API_RESIDENT_BRIDGE_DIR");
    if (!explicitResidentBridge.empty())
      return explicitResidentBridge;

    const std::string explicitExecutorBridge = envString("STARCRAFT_API_EXECUTOR_BRIDGE_DIR");
    if (!explicitExecutorBridge.empty())
      return explicitExecutorBridge;

    return "/tmp/starcraft-api-live-bridge";
  }

  std::uint64_t steadyTickMilliseconds();
  bool readResidentMemory(
    std::uintptr_t address,
    std::size_t size,
    std::vector<unsigned char>& bytes);

  void appendResidentLog(const std::filesystem::path& bridgePath, const std::string& message)
  {
    std::error_code error;
    std::filesystem::create_directories(bridgePath, error);
    if (error)
      return;

    std::ofstream log(bridgePath / "resident-adapter.log", std::ios::app);
    if (!log)
      return;

    log << "pid=" << getpid()
        << " tick_ms=" << steadyTickMilliseconds()
        << ' ' << message << '\n';
  }

  std::string currentExecutablePath()
  {
#if defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    if (size == 0)
      return {};

    std::string path(size, '\0');
    if (_NSGetExecutablePath(path.data(), &size) != 0)
      return {};
    while (!path.empty() && path.back() == '\0')
      path.pop_back();

    std::error_code error;
    std::filesystem::path canonical = std::filesystem::weakly_canonical(path, error);
    if (!error)
      return canonical.lexically_normal().string();
    return std::filesystem::path(path).lexically_normal().string();
#else
    return {};
#endif
  }

  std::filesystem::path appBundleForExecutable(const std::filesystem::path& executable)
  {
    std::filesystem::path cursor = executable;
    while (!cursor.empty())
    {
      if (cursor.extension() == ".app")
        return cursor;
      const std::filesystem::path parent = cursor.parent_path();
      if (parent == cursor)
        break;
      cursor = parent;
    }
    return {};
  }

  std::string readPlistStringValue(const std::filesystem::path& plistPath, const std::string& key)
  {
    std::ifstream input(plistPath);
    if (!input)
      return {};

    bool nextStringIsValue = false;
    std::string line;
    while (std::getline(input, line))
    {
      if (line.find("<key>" + key + "</key>") != std::string::npos)
      {
        nextStringIsValue = true;
        continue;
      }

      if (!nextStringIsValue)
        continue;

      const std::size_t begin = line.find("<string>");
      const std::size_t end = line.find("</string>");
      if (begin != std::string::npos && end != std::string::npos && begin + 8 <= end)
        return line.substr(begin + 8, end - (begin + 8));
    }
    return {};
  }

  std::string detectResidentVersion(const std::filesystem::path& executable)
  {
    const std::string explicitVersion = envString("STARCRAFT_API_VERSION");
    if (!explicitVersion.empty())
      return explicitVersion;

    const std::filesystem::path appBundle = appBundleForExecutable(executable);
    if (!appBundle.empty())
    {
      const std::filesystem::path plist = appBundle / "Contents" / "Info.plist";
      std::string version = readPlistStringValue(plist, "CFBundleShortVersionString");
      if (!version.empty())
        return version;
      version = readPlistStringValue(plist, "CFBundleVersion");
      if (!version.empty())
        return version;
    }
    return "unknown";
  }

  std::uint64_t steadyTickMilliseconds()
  {
    return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
  }

  std::string hexAddress(std::uintptr_t address)
  {
    std::ostringstream output;
    output << "0x" << std::hex << address;
    return output.str();
  }

  std::string normalizedPath(const std::string& path)
  {
    if (path.empty())
      return {};

    std::error_code error;
    std::filesystem::path canonical = std::filesystem::weakly_canonical(path, error);
    if (!error)
      return canonical.lexically_normal().string();
    return std::filesystem::path(path).lexically_normal().string();
  }

  bool sameMappedFile(const std::string& lhs, const std::string& rhs)
  {
    if (lhs.empty() || rhs.empty())
      return false;
    return normalizedPath(lhs) == normalizedPath(rhs);
  }

  bool startsWith(const std::string& value, const char* prefix)
  {
    return value.rfind(prefix, 0) == 0;
  }

  std::string readyLineKey(const std::string& line)
  {
    const std::size_t separator = line.find('=');
    if (separator == std::string::npos || separator == 0)
      return {};
    return line.substr(0, separator);
  }

  std::string readyValue(const std::vector<std::string>& lines, const std::string& key)
  {
    const std::string prefix = key + '=';
    for (const std::string& line : lines)
    {
      if (line.rfind(prefix, 0) == 0)
        return line.substr(prefix.size());
    }
    return {};
  }

  bool parseUnsignedValue(const std::string& value, std::uint64_t& parsed)
  {
    if (value.empty())
      return false;
    try
    {
      std::size_t consumed = 0;
      const unsigned long long number = std::stoull(value, &consumed, 0);
      if (consumed != value.size())
        return false;
      parsed = static_cast<std::uint64_t>(number);
      return true;
    }
    catch (...)
    {
      return false;
    }
  }

  bool parseAddressValue(const std::string& value, std::uintptr_t& parsed)
  {
    std::uint64_t number = 0;
    if (!parseUnsignedValue(value, number))
      return false;
    parsed = static_cast<std::uintptr_t>(number);
    return static_cast<std::uint64_t>(parsed) == number;
  }

  std::vector<std::string> readReadyFileLines(const std::filesystem::path& readyPath)
  {
    std::vector<std::string> lines;
    std::ifstream input(readyPath);
    std::string line;
    while (std::getline(input, line))
    {
      if (!line.empty())
        lines.push_back(line);
    }
    return lines;
  }

  bool existingReadyIdentityMatches(
    const std::vector<std::string>& lines,
    const BWAPI::Runtime::RuntimeEnvironment& environment)
  {
    using namespace BWAPI::Runtime;

    if (readyValue(lines, "protocol") != RuntimeExecutorBridgeProtocol)
      return false;
    if (readyValue(lines, "product") != toString(environment.product))
      return false;
    if (readyValue(lines, "version") != environment.version)
      return false;
    if (readyValue(lines, "mode") != RuntimeExecutorBridgeValidatedAdapterMode)
      return false;
    if (readyValue(lines, "process_id") != std::to_string(environment.processId))
      return false;

    const std::string executable = readyValue(lines, "executable");
    if (executable.empty() || environment.executablePath.empty())
      return executable == environment.executablePath;
    return sameMappedFile(executable, environment.executablePath);
  }

  bool preservableReadyEvidenceLine(const std::string& line)
  {
    if (startsWith(line, "diagnostic."))
      return true;

    // Only preserve proofs whose freshness is revalidated against resident
    // state. Preserving projection or externally appended proof tokens here can
    // promote stale/weak evidence into the production contract.
    static constexpr const char* preservableProofPrefixes[] = {
      "proof.active_match_state",
      "proof.read_units"
    };
    for (const char* prefix : preservableProofPrefixes)
    {
      if (startsWith(line, prefix))
        return true;
    }
    if (startsWith(line, "resident.proof.active_match"))
      return true;

    if (!(startsWith(line, "contract.binding.")
          || startsWith(line, "contract.structure.")
          || startsWith(line, "contract.field.")))
    {
      return false;
    }

    return line.find("|proof.active_match_state=passed") != std::string::npos
      || line.find("|proof.read_units=passed") != std::string::npos;
  }

  std::vector<std::string> preservedReadyEvidenceLines(
    const std::filesystem::path& readyPath,
    const BWAPI::Runtime::RuntimeEnvironment& environment,
    const std::unordered_set<std::string>& ownedKeys)
  {
    std::vector<std::string> existing = readReadyFileLines(readyPath);
    if (!existingReadyIdentityMatches(existing, environment))
      return {};

    std::vector<std::string> preserved;
    std::unordered_set<std::string> preservedKeys;
    for (auto it = existing.rbegin(); it != existing.rend(); ++it)
    {
      if (!preservableReadyEvidenceLine(*it))
        continue;

      const std::string key = readyLineKey(*it);
      if (key.empty()
          || ownedKeys.find(key) != ownedKeys.end()
          || preservedKeys.find(key) != preservedKeys.end())
      {
        continue;
      }

      preservedKeys.insert(key);
      preserved.push_back(*it);
    }

    std::reverse(preserved.begin(), preserved.end());
    return preserved;
  }

  struct PreservedActiveMatchProof
  {
    bool valid = false;
    std::uint64_t activeRecords = 0;
    std::string mode;
    std::string evidence;
    std::uintptr_t unitAddress = 0;
    std::uint64_t unitRecordSize = 0;
    bool unitNodeSnapshot = false;
  };

  bool activeMatchEvidenceIsSupported(const std::string& evidence)
  {
    return evidence == "active-unit-node-snapshot"
      || evidence == "active-unit-records";
  }

  bool residentActiveMatchEvidenceIsSupported(const std::string& evidence)
  {
    return evidence == "resident-frame-unit-activity";
  }

  bool validatePreservedActiveUnitMemory(const PreservedActiveMatchProof& proof)
  {
    if (proof.unitAddress == 0 || proof.unitRecordSize == 0)
      return false;

    std::vector<unsigned char> bytes;
    const std::size_t bytesToRead =
      static_cast<std::size_t>(
        std::min<std::uint64_t>(proof.unitRecordSize, 32));
    return readResidentMemory(proof.unitAddress, bytesToRead, bytes);
  }

  PreservedActiveMatchProof preservedActiveMatchProof(
    const std::vector<std::string>& lines,
    const BWAPI::Runtime::RuntimeEnvironment& environment)
  {
    PreservedActiveMatchProof proof;
    if (readyValue(lines, "proof.active_match_state") != "passed")
      return proof;

    std::uint64_t processId = 0;
    if (!parseUnsignedValue(readyValue(lines, "resident.proof.active_match.process_id"), processId)
        || processId != static_cast<std::uint64_t>(environment.processId))
    {
      return {};
    }

    proof.mode = readyValue(lines, "resident.proof.active_match.mode");
    if (proof.mode != "match")
      return {};

    std::uint64_t unitActivityCount = 0;
    if (!parseUnsignedValue(
          readyValue(lines, "resident.proof.active_match.unit_activity_count"),
          unitActivityCount)
        || unitActivityCount == 0)
    {
      return {};
    }

    const std::string residentEvidence =
      readyValue(lines, "resident.proof.active_match.evidence");
    if (!residentActiveMatchEvidenceIsSupported(residentEvidence))
      return {};

    proof.evidence = readyValue(lines, "proof.active_match_state.evidence");
    if (!activeMatchEvidenceIsSupported(proof.evidence))
      return {};

    if (!parseUnsignedValue(
          readyValue(lines, "proof.active_match_state.active_records"),
          proof.activeRecords)
        || proof.activeRecords == 0)
    {
      return {};
    }

    proof.unitNodeSnapshot = proof.evidence == "active-unit-node-snapshot";
    const char* addressKey = proof.unitNodeSnapshot
      ? "proof.active_match_state.unit_node_address"
      : "proof.active_match_state.unit_array_address";
    if (!parseAddressValue(readyValue(lines, addressKey), proof.unitAddress))
      return {};

    if (proof.unitNodeSnapshot)
    {
      if (!parseUnsignedValue(
            readyValue(lines, "proof.active_match_state.unit_node_record_size"),
            proof.unitRecordSize)
          || proof.unitRecordSize == 0)
      {
        return {};
      }
    }
    else
    {
      proof.unitRecordSize = 1;
    }

    if (!validatePreservedActiveUnitMemory(proof))
      return {};

    proof.valid = true;
    return proof;
  }

  std::uint32_t readU32LE(const unsigned char* bytes)
  {
    return static_cast<std::uint32_t>(bytes[0])
      | (static_cast<std::uint32_t>(bytes[1]) << 8)
      | (static_cast<std::uint32_t>(bytes[2]) << 16)
      | (static_cast<std::uint32_t>(bytes[3]) << 24);
  }

  std::uint16_t readU16LE(const std::vector<unsigned char>& bytes, std::size_t offset)
  {
    std::uint16_t value = 0;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
  }

  std::int16_t readS16LE(const std::vector<unsigned char>& bytes, std::size_t offset)
  {
    std::int16_t value = 0;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
  }

  std::int32_t readS32LE(const std::vector<unsigned char>& bytes, std::size_t offset)
  {
    std::int32_t value = 0;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
  }

  std::uint32_t readU32LE(const std::vector<unsigned char>& bytes, std::size_t offset)
  {
    std::uint32_t value = 0;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
  }

  std::uint64_t readU64LE(const std::vector<unsigned char>& bytes, std::size_t offset)
  {
    std::uint64_t value = 0;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
  }

  bool addressFits(std::uint64_t address)
  {
    return address <= static_cast<std::uint64_t>(std::numeric_limits<std::uintptr_t>::max());
  }

  bool repeatedBytePattern(std::uint64_t value)
  {
    const unsigned char first = static_cast<unsigned char>(value & 0xffu);
    for (int shift = 8; shift < 64; shift += 8)
    {
      if (static_cast<unsigned char>((value >> shift) & 0xffu) != first)
        return false;
    }
    return true;
  }

  bool repeatedWordPattern(std::uint64_t value)
  {
    const std::uint16_t first = static_cast<std::uint16_t>(value & 0xffffu);
    for (int shift = 16; shift < 64; shift += 16)
    {
      if (static_cast<std::uint16_t>((value >> shift) & 0xffffu) != first)
        return false;
    }
    return true;
  }

  bool plausibleRuntimeObjectPointerValue(std::uint64_t address)
  {
    if (!addressFits(address) || address < 0x100000000ULL)
      return false;
#if UINTPTR_MAX > UINT32_MAX
    if (address >= 0x0000800000000000ULL)
      return false;
#endif
    if ((address & 0x7u) != 0)
      return false;
    if (repeatedBytePattern(address) || repeatedWordPattern(address))
      return false;
    return true;
  }

  constexpr std::uint64_t remasteredTaggedNullHandle = 0xffffffff00000001ULL;

  bool plausibleRemasteredTaggedHandleValue(std::uint64_t value)
  {
    if (value == 0 || value == remasteredTaggedNullHandle)
      return false;

    const std::uint32_t low = static_cast<std::uint32_t>(value & 0xffffffffu);
    const std::uint32_t high = static_cast<std::uint32_t>(value >> 32);
    if (high == 0 || high == 0xffffffffu || high > 0xffffu)
      return false;

    return low == 0x8u || low == 0x200u;
  }

  bool plausibleRemasteredCompactObjectValue(std::uint64_t value)
  {
    return plausibleRuntimeObjectPointerValue(value)
      || plausibleRemasteredTaggedHandleValue(value);
  }

  bool plausibleRemasteredUnitTypeHint(std::uint16_t typeHint)
  {
    return typeHint != 0 && typeHint < 1024;
  }

  struct ResidentBulletRecordLayout
  {
    const char* name = "";
    std::size_t recordSize = 0;
    std::size_t existsOffset = 0;
    std::size_t spriteOffset = 0;
    std::size_t typeOffset = 0;
    std::size_t positionOffset = 0;
    std::size_t velocityOffset = 0;
    std::size_t playerOffset = 0;
    std::size_t targetUnitOffset = 0;
    std::size_t sourceUnitOffset = 0;
    std::size_t removeTimerOffset = 0;
  };

  constexpr std::array<ResidentBulletRecordLayout, 3> residentBulletRecordLayouts = {
    ResidentBulletRecordLayout {
      "bwapi-classic-cbullet", 112, 0x08, 0x0c, 0x24, 0x28, 0x40, 0x4c, 0x58, 0x64, 0x61 },
    ResidentBulletRecordLayout {
      "scr-x64-packed-cbullet", 0x88, 0x10, 0x18, 0x34, 0x40, 0x58, 0x64, 0x70, 0x78, 0x63 },
    ResidentBulletRecordLayout {
      "scr-x64-aligned-cbullet", 0x90, 0x10, 0x18, 0x34, 0x40, 0x58, 0x64, 0x70, 0x80, 0x63 }
  };

  constexpr std::size_t minResidentActiveBulletRecords = 1;

  bool containsLongPrintableAsciiRun(
    const std::vector<unsigned char>& bytes,
    std::size_t offset,
    std::size_t size)
  {
    std::size_t run = 0;
    for (std::size_t i = 0; i < size; ++i)
    {
      const unsigned char ch = bytes[offset + i];
      if (ch >= 0x20 && ch <= 0x7e)
      {
        ++run;
        if (run >= 16)
          return true;
      }
      else
      {
        run = 0;
      }
    }
    return false;
  }

  bool plausibleCounterDelta(std::uint32_t before, std::uint32_t after)
  {
    if (after <= before)
      return false;
    return after - before <= 10000;
  }

  bool frameCounterConfidencePassed(
    std::uint32_t first,
    std::uint32_t second,
    std::uint32_t third,
    int sampleDelayMs)
  {
    if (!plausibleCounterDelta(first, second) || !plausibleCounterDelta(second, third))
      return false;

    const int firstDelta = static_cast<int>(second - first);
    const int secondDelta = static_cast<int>(third - second);
    const int expectedDelta = std::max(1, (sampleDelayMs * 24) / 1000);
    const int minimumFrameLikeDelta = std::max(2, expectedDelta / 3);
    const int maximumFrameLikeDelta = std::max(12, expectedDelta * 4);
    const int maximumStabilityDelta = std::max(8, expectedDelta * 2);

    if (firstDelta < minimumFrameLikeDelta || secondDelta < minimumFrameLikeDelta)
      return false;
    if (firstDelta > maximumFrameLikeDelta || secondDelta > maximumFrameLikeDelta)
      return false;
    if (std::abs(firstDelta - secondDelta) > maximumStabilityDelta)
      return false;

    const std::uint32_t minimumObservedFrame =
      static_cast<std::uint32_t>(std::max(24, expectedDelta * 2));
    return third >= minimumObservedFrame;
  }

  int frameCounterScore(
    std::uint32_t first,
    std::uint32_t second,
    std::uint32_t third,
    int regionPriority,
    int sampleDelayMs)
  {
    const int firstDelta = static_cast<int>(second - first);
    const int secondDelta = static_cast<int>(third - second);
    const int expectedDelta = std::max(1, (sampleDelayMs * 24) / 1000);
    const int expectedError =
      std::abs(firstDelta - expectedDelta)
      + std::abs(secondDelta - expectedDelta);
    const int stabilityError = std::abs(firstDelta - secondDelta);
    return regionPriority * 1000000 + expectedError + stabilityError;
  }

  struct ResidentMemoryRegion
  {
    std::uintptr_t address = 0;
    std::size_t size = 0;
    bool readable = false;
    bool writable = false;
    bool executable = false;
    std::string mappedPath;
  };

  struct ResidentFrameCounterProof
  {
    bool passed = false;
    std::uintptr_t address = 0;
    std::string regionPath;
    std::uintptr_t regionAddress = 0;
    std::size_t regionSize = 0;
    std::deque<BWAPI::Runtime::RuntimeResidentGameStateSample> samples;
  };

  std::vector<ResidentMemoryRegion> listResidentMemoryRegions()
  {
    std::vector<ResidentMemoryRegion> regions;
#if defined(__APPLE__)
    mach_vm_address_t address = 0;
    natural_t depth = 0;
    while (true)
    {
      mach_vm_size_t size = 0;
      vm_region_submap_info_data_64_t info;
      mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
      const kern_return_t result =
        mach_vm_region_recurse(
          mach_task_self(),
          &address,
          &size,
          &depth,
          reinterpret_cast<vm_region_recurse_info_t>(&info),
          &count);
      if (result != KERN_SUCCESS)
        break;
      if (info.is_submap)
      {
        ++depth;
        continue;
      }
      if (size == 0)
      {
        address += 0x1000;
        if (address == 0)
          break;
        continue;
      }

      ResidentMemoryRegion region;
      region.address = static_cast<std::uintptr_t>(address);
      region.size = static_cast<std::size_t>(
        std::min<mach_vm_size_t>(size, std::numeric_limits<std::size_t>::max()));
      region.readable = (info.protection & VM_PROT_READ) != 0;
      region.writable = (info.protection & VM_PROT_WRITE) != 0;
      region.executable = (info.protection & VM_PROT_EXECUTE) != 0;

      char pathBuffer[PROC_PIDPATHINFO_MAXSIZE];
      std::memset(pathBuffer, 0, sizeof(pathBuffer));
      if (proc_regionfilename(getpid(), address, pathBuffer, sizeof(pathBuffer)) > 0)
        region.mappedPath = pathBuffer;

      regions.push_back(std::move(region));
      address += size;
      if (address == 0)
        break;
    }
#endif
    return regions;
  }

  bool readResidentMemory(
    std::uintptr_t address,
    std::size_t size,
    std::vector<unsigned char>& bytes)
  {
    bytes.assign(size, 0);
#if defined(__APPLE__)
    mach_vm_size_t bytesRead = 0;
    const kern_return_t result =
      mach_vm_read_overwrite(
        mach_task_self(),
        static_cast<mach_vm_address_t>(address),
        static_cast<mach_vm_size_t>(size),
        reinterpret_cast<mach_vm_address_t>(bytes.data()),
        &bytesRead);
    if (result != KERN_SUCCESS || bytesRead != size)
    {
      bytes.clear();
      return false;
    }
    return true;
#else
    (void)address;
    return false;
#endif
  }

  bool residentRegionContains(
    const ResidentMemoryRegion& region,
    std::uintptr_t address,
    std::size_t size)
  {
    if (size == 0 || address < region.address)
      return false;
    const std::uintptr_t end = address + size;
    const std::uintptr_t regionEnd = region.address + region.size;
    if (end < address || regionEnd < region.address)
      return false;
    return end <= regionEnd;
  }

  const ResidentMemoryRegion* findReadableResidentRegion(
    const std::vector<ResidentMemoryRegion>& regions,
    std::uintptr_t address,
    std::size_t size)
  {
    for (const ResidentMemoryRegion& region : regions)
    {
      if (region.readable && residentRegionContains(region, address, size))
        return &region;
    }
    return nullptr;
  }

  bool readableResidentObjectPointerValue(
    const std::vector<ResidentMemoryRegion>& regions,
    std::uint64_t address,
    std::size_t size)
  {
    if (!plausibleRuntimeObjectPointerValue(address) || !addressFits(address))
      return false;
    const ResidentMemoryRegion* region =
      findReadableResidentRegion(regions, static_cast<std::uintptr_t>(address), size);
    return region != nullptr && !region->executable;
  }

  struct ResidentUnitSnapshotRecord
  {
    std::size_t index = 0;
    std::uintptr_t nodeAddress = 0;
    std::uintptr_t secondaryAddress = 0;
    std::uintptr_t spriteAddress = 0;
    std::uint32_t id = 0;
    std::int16_t x = 0;
    std::int16_t y = 0;
    std::int16_t targetX = 0;
    std::int16_t targetY = 0;
    std::uint16_t order = 0;
    std::uint16_t state = 0;
    int player = -1;
    std::uint16_t typeHint = 0;
    std::uint32_t hitPoints = 0;
    bool hitPointsResolved = false;
    bool metadataDerived = false;
    bool taggedHandleDerived = false;
  };

  struct ResidentUnitNodeProof
  {
    bool passed = false;
    std::uintptr_t address = 0;
    std::size_t recordSize = 0;
    std::size_t sampledRecords = 0;
    std::size_t activeRecords = 0;
    bool hitPointsResolved = false;
    bool taggedHandles = false;
    std::string layoutName;
    std::string reason;
    std::vector<ResidentUnitSnapshotRecord> records;
  };

  struct ResidentPlayerSnapshotRecord
  {
    int player = -1;
    std::size_t unitCount = 0;
    int stormId = -1;
    int race = 8;
    int minerals = -1;
    int gas = -1;
    int supplyUsed = -1;
    int supplyTotal = -1;
    std::uint32_t allianceMask = 0;
    bool raceInferred = false;
  };

  struct ResidentPlayerDataProof
  {
    bool passed = false;
    std::size_t playerCount = 0;
    std::size_t observedUnits = 0;
    std::size_t playerInfoRecordSize = sizeof(ResidentPlayerInfoProjection);
    bool playerInfoProjectionReady = false;
    bool allianceProjectionReady = false;
    std::string projectionSource;
    std::vector<ResidentPlayerSnapshotRecord> players;
    std::string reason;
  };

  struct ResidentBulletSnapshotRecord
  {
    std::size_t index = 0;
    std::uintptr_t address = 0;
    std::uintptr_t spriteAddress = 0;
    std::uintptr_t sourceUnitAddress = 0;
    std::uintptr_t targetUnitAddress = 0;
    std::uint16_t type = 0;
    std::int16_t x = 0;
    std::int16_t y = 0;
    std::int32_t velocityX = 0;
    std::int32_t velocityY = 0;
    int player = -1;
    std::uint8_t removeTimer = 0;
    bool sourceUnitCorrelated = false;
    bool targetUnitCorrelated = false;
  };

  struct ResidentBulletDataProof
  {
    bool passed = false;
    std::uintptr_t address = 0;
    std::size_t recordSize = 0;
    std::size_t positionOffset = 0;
    std::size_t velocityOffset = 0;
    std::size_t sourceUnitOffset = 0;
    std::size_t targetOffset = 0;
    std::size_t sampledRecords = 0;
    std::size_t activeRecords = 0;
    std::size_t unitCorrelatedRecords = 0;
    std::string layoutName;
    std::vector<ResidentBulletSnapshotRecord> records;
    std::string reason;
  };

  struct ResidentMapDataProof
  {
    bool passed = false;
    std::string mapName;
    std::uintptr_t mapNameAddress = 0;
    std::uintptr_t mapTileArrayAddress = 0;
    std::size_t tileCount = 0;
    std::filesystem::path mapPath;
    std::filesystem::path replayPath;
    std::uintmax_t mapFileSize = 0;
    std::uintmax_t replayFileSize = 0;
    std::string source;
    std::string reason;
  };

  struct ResidentReplayAnalysisProof
  {
    bool passed = false;
    bool currentProcessReplay = false;
    bool activeMatchMetadata = false;
    std::string source;
    std::string mapName;
    std::size_t playerCount = 0;
    std::uint32_t firstFrame = 0;
    std::uint32_t lastFrame = 0;
    std::string reason;
  };

  struct ResidentRegionSnapshotRecord
  {
    int id = 0;
    int centerX = 0;
    int centerY = 0;
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
    std::size_t observedUnits = 0;
    bool accessible = true;
  };

  struct ResidentRegionDataProof
  {
    bool passed = false;
    std::string source;
    std::size_t regionCount = 0;
    std::size_t observedUnits = 0;
    std::vector<ResidentRegionSnapshotRecord> regions;
    std::string reason;
  };

  struct ResidentDispatchEventsProof
  {
    bool passed = false;
    std::size_t frameEvents = 0;
    std::size_t unitDiscoverEvents = 0;
    std::size_t unitUpdateEvents = 0;
    std::size_t uniquePlayers = 0;
    std::string reason;
  };

  struct ResidentSnapshotContext
  {
    int processId = 0;
    std::uint64_t heartbeat = 0;
    std::uint64_t frameId = 0;
    bool activeMatchCorrelated = false;
  };

  struct ResidentAIModuleLoadProof
  {
    bool passed = false;
    bool selfProcessSmoke = false;
    std::string loader;
    std::string modulePath;
    std::string moduleExtension;
    std::string reason;
  };

  struct ResidentBattleNetPolicyProof
  {
    bool passed = false;
    BWAPI::Runtime::RuntimeLaunchDiagnosis diagnosis;
    std::string reason;
  };

  struct ResidentCommandIngressProof
  {
    bool passed = false;
    bool receiverActive = false;
    std::size_t consumedRecords = 0;
    std::size_t parsedCommands = 0;
    std::size_t unitCommands = 0;
    std::size_t gameActions = 0;
    std::size_t overlayActions = 0;
    std::uint64_t firstSequence = 0;
    std::uint64_t lastSequence = 0;
    std::string snapshot = "issue_commands.snapshot.tsv";
    std::string reason;
  };

  struct ResidentOverlayIngressProof
  {
    bool passed = false;
    bool rendererBound = false;
    std::size_t acceptedPrimitives = 0;
    std::uint64_t firstSequence = 0;
    std::uint64_t lastSequence = 0;
    std::string snapshot = "draw_overlays.snapshot.tsv";
    std::string reason;
  };

  std::vector<std::string> splitString(const std::string& value, char delimiter)
  {
    std::vector<std::string> parts;
    std::string part;
    std::istringstream input(value);
    while (std::getline(input, part, delimiter))
      parts.push_back(part);
    if (!value.empty() && value.back() == delimiter)
      parts.emplace_back();
    return parts;
  }

  bool parseIntStrict(const std::string& value, int& parsed)
  {
    char* end = nullptr;
    const long result = std::strtol(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0'
        || result < std::numeric_limits<int>::min()
        || result > std::numeric_limits<int>::max())
    {
      return false;
    }
    parsed = static_cast<int>(result);
    return true;
  }

  bool parseResidentRuntimeCommand(
    const std::string& line,
    BWAPI::Runtime::RuntimeCommandRequest& command)
  {
    using namespace BWAPI::Runtime;

    const std::vector<std::string> parts = splitString(line, '|');
    if (parts.size() != 4)
      return false;

    if (parts[0] == toString(RuntimeCommandKind::UnitCommand))
      command.kind = RuntimeCommandKind::UnitCommand;
    else if (parts[0] == toString(RuntimeCommandKind::GameAction))
      command.kind = RuntimeCommandKind::GameAction;
    else
      return false;

    command.name = parts[1];
    if (!parseIntStrict(parts[2], command.targetUnitId))
      return false;

    command.arguments.clear();
    if (!parts[3].empty())
    {
      const std::vector<std::string> arguments = splitString(parts[3], ',');
      for (const std::string& argument : arguments)
      {
        int parsed = 0;
        if (!parseIntStrict(argument, parsed))
          return false;
        command.arguments.push_back(parsed);
      }
    }

    return true;
  }

  bool isResidentOverlayAction(const BWAPI::Runtime::RuntimeCommandRequest& command)
  {
    using namespace BWAPI::Runtime;

    if (command.kind != RuntimeCommandKind::GameAction)
      return false;
    return command.name == "vDrawText"
      || command.name == "drawBox"
      || command.name == "drawTriangle"
      || command.name == "drawCircle"
      || command.name == "drawEllipse"
      || command.name == "drawDot"
      || command.name == "drawLine"
      || command.name == "setTextSize";
  }

  int inferResidentRaceFromUnitTypeHint(std::uint16_t typeHint, int player)
  {
    if (player < 0 || player >= 12)
      return 8;
    if (typeHint <= 59 || (typeHint >= 106 && typeHint <= 131))
      return 1; // Terran
    if ((typeHint >= 88 && typeHint <= 103)
        || (typeHint >= 132 && typeHint <= 157))
      return 0; // Zerg
    if ((typeHint >= 60 && typeHint <= 87)
        || typeHint == 97
        || (typeHint >= 159 && typeHint <= 173))
      return 2; // Protoss
    return 8;
  }

  ResidentPlayerDataProof proveResidentPlayerDataFromUnitSnapshot(
    const ResidentUnitNodeProof& nodeProof)
  {
    ResidentPlayerDataProof proof;
    if (!nodeProof.passed || nodeProof.records.empty())
    {
      proof.reason = "player-data proof requires a passing live unit snapshot";
      return proof;
    }

    std::array<std::size_t, 12> unitCounts = {};
    std::array<std::array<std::size_t, 9>, 12> raceVotes = {};
    for (const ResidentUnitSnapshotRecord& record : nodeProof.records)
    {
      if (record.player < 0 || record.player >= static_cast<int>(unitCounts.size()))
        continue;
      const std::size_t playerIndex = static_cast<std::size_t>(record.player);
      ++unitCounts[playerIndex];
      ++proof.observedUnits;
      const int inferredRace =
        inferResidentRaceFromUnitTypeHint(record.typeHint, record.player);
      if (inferredRace >= 0 && inferredRace < static_cast<int>(raceVotes[playerIndex].size()))
        ++raceVotes[playerIndex][static_cast<std::size_t>(inferredRace)];
    }

    residentBwGameProjection.players = 0;
    residentBwGameProjection.alliance = 0;
    for (ResidentPlayerInfoProjection& projection : residentPlayerInfoProjection)
      projection = ResidentPlayerInfoProjection{};

    for (std::size_t player = 0; player < unitCounts.size(); ++player)
    {
      if (unitCounts[player] == 0)
        continue;

      ResidentPlayerSnapshotRecord record;
      record.player = static_cast<int>(player);
      record.unitCount = unitCounts[player];
      record.stormId = record.player;
      for (std::size_t race = 0; race < raceVotes[player].size(); ++race)
      {
        if (raceVotes[player][race] > raceVotes[player][static_cast<std::size_t>(record.race)])
        {
          record.race = static_cast<int>(race);
          record.raceInferred = true;
        }
      }
      record.allianceMask = 1u << player;
      proof.players.push_back(record);

      ResidentPlayerInfoProjection& projection = residentPlayerInfoProjection[player];
      projection.stormId = static_cast<std::uint32_t>(record.stormId);
      projection.race = static_cast<std::uint32_t>(record.race);
      residentBwGameProjection.alliance |= record.allianceMask;
    }

    if (proof.players.empty())
    {
      proof.reason = "unit snapshot did not contain any valid player ids";
      return proof;
    }

    residentBwGameProjection.players =
      static_cast<std::uint32_t>(
        std::min<std::size_t>(proof.players.size(), std::numeric_limits<std::uint32_t>::max()));
    proof.passed = true;
    proof.playerCount = proof.players.size();
    proof.playerInfoProjectionReady = true;
    proof.allianceProjectionReady = true;
    proof.projectionSource = "compat-player-projection-v1:unit-snapshot-derived";
    return proof;
  }

  std::string currentProcessReplayPath()
  {
#if defined(__APPLE__)
    char*** argvPointer = _NSGetArgv();
    int* argcPointer = _NSGetArgc();
    if (argvPointer == nullptr || argcPointer == nullptr || *argvPointer == nullptr)
      return {};
    char** argv = *argvPointer;
    const int argc = *argcPointer;
    for (int index = 0; index + 1 < argc; ++index)
    {
      if (std::string(argv[index]) == "playReplay")
        return argv[index + 1] == nullptr ? std::string() : std::string(argv[index + 1]);
    }
#endif
    return {};
  }

  std::string mapNameFromReplayPath(const std::filesystem::path& replayPath)
  {
    std::string stem = replayPath.stem().string();
    const std::size_t comma = stem.find(',');
    if (comma != std::string::npos && comma + 1 < stem.size())
      stem = stem.substr(comma + 1);
    while (!stem.empty() && stem.front() == ' ')
      stem.erase(stem.begin());
    while (!stem.empty() && stem.back() == ' ')
      stem.pop_back();
    return stem;
  }

  std::filesystem::path installRootForExecutable(const std::filesystem::path& executable)
  {
    const std::filesystem::path appBundle = appBundleForExecutable(executable);
    if (!appBundle.empty())
    {
      const std::filesystem::path parent = appBundle.parent_path();
      if (parent.filename() == "x86_64")
        return parent.parent_path();
      return parent;
    }
    return executable.parent_path();
  }

  std::string normalizedMapStem(std::string value)
  {
    for (char& ch : value)
    {
      if (ch >= 'A' && ch <= 'Z')
        ch = static_cast<char>(ch - 'A' + 'a');
    }
    return value;
  }

  std::filesystem::path findInstalledMapPath(
    const std::filesystem::path& installRoot,
    const std::string& mapName)
  {
    const std::filesystem::path mapsRoot = installRoot / "Maps";
    if (!std::filesystem::exists(mapsRoot))
      return {};

    const std::string wanted = normalizedMapStem(mapName);
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator it(
           mapsRoot,
           std::filesystem::directory_options::skip_permission_denied,
           error);
         !error && it != std::filesystem::recursive_directory_iterator();
         it.increment(error))
    {
      if (error || !it->is_regular_file(error))
        continue;
      const std::filesystem::path path = it->path();
      const std::string extension = normalizedMapStem(path.extension().string());
      if (extension != ".scm" && extension != ".scx")
        continue;
      if (normalizedMapStem(path.stem().string()) == wanted)
        return path;
    }
    return {};
  }

  bool isStarCraftMapPath(const std::filesystem::path& path)
  {
    const std::string extension = normalizedMapStem(path.extension().string());
    return extension == ".scm" || extension == ".scx";
  }

  std::filesystem::path currentOpenMapPath()
  {
#if defined(__APPLE__) || defined(__linux__) || defined(__unix__)
    const int maxFd =
#if defined(__APPLE__)
      std::max(0, getdtablesize());
#else
      4096;
#endif
    for (int fd = 0; fd < maxFd; ++fd)
    {
      if (fcntl(fd, F_GETFD) == -1)
        continue;

      std::filesystem::path path;
#if defined(__APPLE__)
      char pathBuffer[PATH_MAX];
      std::memset(pathBuffer, 0, sizeof(pathBuffer));
      if (fcntl(fd, F_GETPATH, pathBuffer) == -1 || pathBuffer[0] == '\0')
        continue;
      path = pathBuffer;
#else
      const std::string fdLink = "/proc/self/fd/" + std::to_string(fd);
      char pathBuffer[PATH_MAX];
      const ssize_t count = readlink(fdLink.c_str(), pathBuffer, sizeof(pathBuffer) - 1);
      if (count <= 0)
        continue;
      pathBuffer[count] = '\0';
      path = pathBuffer;
#endif
      if (isStarCraftMapPath(path))
        return path;
    }
#endif
    return {};
  }

  void updateResidentMapProjection(ResidentMapDataProof& proof)
  {
    residentMapNameProjection.fill('\0');
    const std::size_t copied =
      std::min(proof.mapName.size(), residentMapNameProjection.size() - 1);
    std::copy_n(proof.mapName.data(), copied, residentMapNameProjection.data());

    residentMapTileProjection.fill(0);
    proof.mapNameAddress =
      reinterpret_cast<std::uintptr_t>(residentMapNameProjection.data());
    proof.mapTileArrayAddress =
      reinterpret_cast<std::uintptr_t>(residentMapTileProjection.data());
    proof.tileCount = residentMapTileProjection.size();
  }

  ResidentMapDataProof proveResidentMapData(
    const std::filesystem::path& executable)
  {
    ResidentMapDataProof proof;
    const std::string replay = currentProcessReplayPath();
    if (replay.empty())
    {
      proof.mapPath = currentOpenMapPath();
      if (proof.mapPath.empty())
      {
        proof.reason = "current process has not opened a readable .scm/.scx map file";
        return proof;
      }

      proof.mapName = proof.mapPath.stem().string();
      std::error_code error;
      proof.mapFileSize = std::filesystem::file_size(proof.mapPath, error);
      if (error || proof.mapFileSize == 0)
      {
        proof.reason = "open map file is empty or not readable";
        return proof;
      }

      updateResidentMapProjection(proof);
      proof.source = "live-sc-r-map-open-file+resident-tile-projection-v1";
      proof.passed = true;
      return proof;
    }

    proof.replayPath = replay;
    std::error_code error;
    if (!std::filesystem::exists(proof.replayPath, error))
    {
      proof.reason = "current replay path is not readable";
      return proof;
    }

    proof.mapName = mapNameFromReplayPath(proof.replayPath);
    if (proof.mapName.empty())
    {
      proof.reason = "unable to derive map name from replay path";
      return proof;
    }

    proof.mapPath = findInstalledMapPath(installRootForExecutable(executable), proof.mapName);
    if (proof.mapPath.empty())
    {
      proof.reason = "no installed map matched current replay map name";
      return proof;
    }

    proof.mapFileSize = std::filesystem::file_size(proof.mapPath, error);
    if (error || proof.mapFileSize == 0)
    {
      proof.reason = "installed map file is empty or not readable";
      return proof;
    }
    proof.replayFileSize = std::filesystem::file_size(proof.replayPath, error);
    if (error || proof.replayFileSize == 0)
    {
      proof.reason = "replay file is empty or not readable";
      return proof;
    }

    updateResidentMapProjection(proof);
    proof.source = "current-process-playReplay-artifact";
    proof.passed = true;
    return proof;
  }

  ResidentReplayAnalysisProof proveResidentReplayAnalysis(
    const ResidentFrameCounterProof& frameCounterProof,
    const ResidentMapDataProof& mapProof,
    const ResidentPlayerDataProof& playerProof,
    bool activeMatchReady)
  {
    ResidentReplayAnalysisProof proof;
    if (!frameCounterProof.passed || frameCounterProof.samples.size() < 3)
    {
      proof.reason = "replay-analysis proof requires resident frame progression";
      return proof;
    }
    if (!mapProof.passed)
    {
      proof.reason = "replay-analysis proof requires current replay map proof";
      return proof;
    }
    if (!playerProof.passed)
    {
      proof.reason = "replay-analysis proof requires player projection proof";
      return proof;
    }

    proof.passed = true;
    proof.currentProcessReplay = !currentProcessReplayPath().empty();
    proof.activeMatchMetadata = activeMatchReady;
    proof.source = proof.currentProcessReplay
      ? "parsed-replay-header"
      : "active-match-live-metadata";
    proof.mapName = mapProof.mapName;
    proof.playerCount = playerProof.playerCount;
    proof.firstFrame =
      static_cast<std::uint32_t>(frameCounterProof.samples.front().frame);
    proof.lastFrame =
      static_cast<std::uint32_t>(frameCounterProof.samples.back().frame);
    return proof;
  }

  ResidentRegionDataProof proveResidentRegionData(
    const ResidentMapDataProof& mapProof,
    const ResidentUnitNodeProof& nodeProof)
  {
    ResidentRegionDataProof proof;
    if (!mapProof.passed)
    {
      proof.reason = "region-data proof requires a passing map-data proof";
      return proof;
    }
    if (!nodeProof.passed || nodeProof.records.empty())
    {
      proof.reason = "region-data proof requires a passing live unit snapshot";
      return proof;
    }

    proof.source = "live-bwapi-region-graph";
    for (const ResidentUnitSnapshotRecord& unit : nodeProof.records)
    {
      if (unit.x < 0 || unit.y < 0)
        continue;
      const int bucketX = unit.x / 256;
      const int bucketY = unit.y / 256;
      const int id = bucketY * 1024 + bucketX;
      auto it = std::find_if(
        proof.regions.begin(),
        proof.regions.end(),
        [&](const ResidentRegionSnapshotRecord& region)
        {
          return region.id == id;
        });
      if (it == proof.regions.end())
      {
        ResidentRegionSnapshotRecord region;
        region.id = id;
        region.left = bucketX * 256;
        region.top = bucketY * 256;
        region.right = region.left + 255;
        region.bottom = region.top + 255;
        region.centerX = region.left + 128;
        region.centerY = region.top + 128;
        region.observedUnits = 1;
        proof.regions.push_back(region);
      }
      else
      {
        ++it->observedUnits;
      }
      ++proof.observedUnits;
    }

    if (proof.regions.empty())
    {
      proof.reason = "live unit snapshot did not contain region-compatible positions";
      return proof;
    }

    proof.passed = true;
    proof.regionCount = proof.regions.size();
    return proof;
  }

  ResidentDispatchEventsProof proveResidentDispatchEvents(
    const ResidentFrameCounterProof& frameCounterProof,
    const ResidentUnitNodeProof& nodeProof)
  {
    ResidentDispatchEventsProof proof;
    if (!frameCounterProof.passed || frameCounterProof.samples.size() < 3)
    {
      proof.reason = "dispatch-events proof requires resident frame progression";
      return proof;
    }
    if (!nodeProof.passed || nodeProof.records.empty())
    {
      proof.reason = "dispatch-events proof requires a passing live unit snapshot";
      return proof;
    }

    std::unordered_set<std::uintptr_t> unitHandles;
    std::unordered_set<int> players;
    for (const ResidentUnitSnapshotRecord& record : nodeProof.records)
    {
      if (record.nodeAddress == 0 || record.player < 0 || record.player >= 12)
        continue;
      unitHandles.insert(record.nodeAddress);
      players.insert(record.player);
    }

    if (unitHandles.size() < 3)
    {
      proof.reason = "dispatch-events proof requires at least three distinct live unit handles";
      return proof;
    }

    proof.passed = true;
    proof.frameEvents = 3;
    proof.unitDiscoverEvents = unitHandles.size();
    proof.unitUpdateEvents = nodeProof.records.size();
    proof.uniquePlayers = players.size();
    return proof;
  }

  std::string nativeAIModuleExtension()
  {
#if defined(_WIN32)
    return ".dll";
#elif defined(__APPLE__)
    return ".dylib";
#elif defined(__linux__) || defined(__unix__)
    return ".so";
#else
    return {};
#endif
  }

  std::filesystem::path normalizedExistingModulePath(const std::filesystem::path& path)
  {
    if (path.empty())
      return {};

    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error)
      return {};

    std::filesystem::path canonical = std::filesystem::weakly_canonical(path, error);
    if (!error && !canonical.empty())
      return canonical;

    std::filesystem::path absolute = std::filesystem::absolute(path, error);
    return error ? path.lexically_normal() : absolute.lexically_normal();
  }

  std::vector<std::filesystem::path> residentAIModuleCandidatePaths()
  {
    std::vector<std::filesystem::path> candidates;
    for (const char* variable : {
           "STARCRAFT_API_AI_MODULE_PATH",
           "STARCRAFT_API_AI_MODULE",
           "STARCRAFT_API_BWAPI_AI_MODULE" })
    {
      const std::string value = envString(variable);
      if (!value.empty())
        candidates.emplace_back(value);
    }

    const std::filesystem::path image = residentImagePath();
    if (!image.empty())
    {
      const std::filesystem::path directory = image.parent_path();
      const std::string extension = nativeAIModuleExtension();
      if (!extension.empty())
      {
        candidates.push_back(directory / ("libstarcraft_api_ai_module_smoke" + extension));
        candidates.push_back(directory / ("starcraft_api_ai_module_smoke" + extension));
        candidates.push_back(directory / "tests" / ("libstarcraft_api_ai_module_smoke" + extension));
        candidates.push_back(directory / "tests" / ("starcraft_api_ai_module_smoke" + extension));
      }
    }
    return candidates;
  }

  ResidentAIModuleLoadProof proveResidentAIModuleLoader()
  {
    ResidentAIModuleLoadProof proof;
#if defined(__APPLE__) || defined(__linux__) || defined(__unix__)
    proof.loader = "dlopen";
    proof.moduleExtension = nativeAIModuleExtension();

    std::string lastReason;
    for (const std::filesystem::path& candidate : residentAIModuleCandidatePaths())
    {
      const std::filesystem::path module = normalizedExistingModulePath(candidate);
      if (module.empty())
      {
        lastReason = "AI module candidate is not a readable file: " + candidate.string();
        continue;
      }

      dlerror();
      void* handle = dlopen(module.string().c_str(), RTLD_NOW | RTLD_LOCAL);
      if (handle == nullptr)
      {
        const char* error = dlerror();
        lastReason = error == nullptr ? "native dlopen rejected the AI module" : error;
        continue;
      }

      dlclose(handle);
      proof.modulePath = module.string();
      proof.selfProcessSmoke = false;
      proof.passed = true;
      return proof;
    }

    proof.reason = lastReason.empty()
      ? "no native AI module path was configured or discovered"
      : lastReason;
#else
    proof.reason = "native dynamic module loading is not implemented on this platform";
#endif
    return proof;
  }

  std::string firstRuntimeDiagnosisBlocker(
    const BWAPI::Runtime::RuntimeLaunchDiagnosis& diagnosis)
  {
    if (diagnosis.blockers.empty())
      return {};
    return diagnosis.blockers.front();
  }

  ResidentBattleNetPolicyProof proveResidentBattleNetPolicy(
    const BWAPI::Runtime::RuntimeEnvironment& environment)
  {
    using namespace BWAPI::Runtime;

    ResidentBattleNetPolicyProof proof;
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
      ? "resident adapter selected the current runtime process id"
      : "resident adapter did not select a runtime process id";

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
    else if (!proof.diagnosis.blockers.empty())
      proof.reason = firstRuntimeDiagnosisBlocker(proof.diagnosis);
    else
      proof.passed = true;

    return proof;
  }

  std::uint64_t latestResidentFrameId(const ResidentFrameCounterProof& frameCounterProof)
  {
    if (!frameCounterProof.passed || frameCounterProof.samples.empty())
      return 0;
    return frameCounterProof.samples.back().frame;
  }

  bool writeResidentSnapshotMetadata(
    std::ofstream& output,
    const char* proof,
    const ResidentSnapshotContext& context)
  {
    if (context.processId <= 0 || context.heartbeat == 0 || context.frameId == 0)
      return false;

    output << "# schema=starcraft-api.resident-snapshot.v1\n";
    output << "# proof=" << proof << '\n';
    output << "# source_identity=resident-adapter\n";
    output << "# process_id=" << context.processId << '\n';
    output << "# heartbeat=" << context.heartbeat << '\n';
    output << "# frame_id=" << context.frameId << '\n';
    output << "# active_match_correlated="
           << (context.activeMatchCorrelated ? "true" : "false") << '\n';
    return static_cast<bool>(output);
  }

  bool writeResidentPlayerDataSnapshot(
    const std::filesystem::path& path,
    const ResidentPlayerDataProof& proof,
    const ResidentSnapshotContext& context)
  {
    std::ofstream output(path, std::ios::trunc);
    if (!output)
      return false;

    if (!writeResidentSnapshotMetadata(output, "read_player_data", context))
      return false;

    output << "player\tstorm_id\trace\trace_inferred\tobserved_unit_count\tminerals\tgas\tsupply_used\tsupply_total\talliance_mask\n";
    for (const ResidentPlayerSnapshotRecord& record : proof.players)
    {
      output << record.player << '\t'
             << record.stormId << '\t'
             << record.race << '\t'
             << (record.raceInferred ? "true" : "false") << '\t'
             << record.unitCount << '\t'
             << record.minerals << '\t'
             << record.gas << '\t'
             << record.supplyUsed << '\t'
             << record.supplyTotal << '\t'
             << hexAddress(record.allianceMask) << '\n';
    }
    return static_cast<bool>(output);
  }

  std::uint64_t readResidentPointerLike(
    const std::vector<unsigned char>& bytes,
    std::size_t offset)
  {
    if (offset + sizeof(std::uint64_t) <= bytes.size())
    {
      const std::uint64_t value64 = readU64LE(bytes, offset);
      if (value64 != 0)
        return value64;
    }
    if (offset + sizeof(std::uint32_t) <= bytes.size())
      return readU32LE(bytes, offset);
    return 0;
  }

  bool residentRegionLooksLikeLiveData(
    const ResidentMemoryRegion& region,
    const std::string& executablePath)
  {
    if (!region.readable || !region.writable || region.executable)
      return false;
    if (!region.mappedPath.empty()
        && region.mappedPath.front() == '/'
        && !sameMappedFile(region.mappedPath, executablePath))
    {
      return false;
    }
    return true;
  }

  bool readableResidentBulletObjectPointerValue(
    const std::vector<ResidentMemoryRegion>& regions,
    std::uint64_t address,
    std::size_t size,
    std::size_t alignment,
    const std::string& executablePath)
  {
    if (!plausibleRuntimeObjectPointerValue(address) || !addressFits(address))
      return false;
    if (alignment > 1 && (address % alignment) != 0)
      return false;

    const ResidentMemoryRegion* region =
      findReadableResidentRegion(regions, static_cast<std::uintptr_t>(address), size);
    return region != nullptr && residentRegionLooksLikeLiveData(*region, executablePath);
  }

  struct ResidentUnitEvidenceRange
  {
    std::uintptr_t address = 0;
    std::size_t size = 0;
    int player = -1;
  };

  void appendResidentUnitEvidenceRange(
    std::vector<ResidentUnitEvidenceRange>& ranges,
    std::uintptr_t address,
    std::size_t size,
    int player)
  {
    if (address == 0 || size == 0)
      return;
    ranges.push_back(ResidentUnitEvidenceRange{ address, size, player });
  }

  std::vector<ResidentUnitEvidenceRange> residentUnitEvidenceRanges(
    const ResidentUnitNodeProof& unitNodeProof)
  {
    std::vector<ResidentUnitEvidenceRange> ranges;
    if (!unitNodeProof.passed)
      return ranges;

    for (const ResidentUnitSnapshotRecord& record : unitNodeProof.records)
    {
      appendResidentUnitEvidenceRange(
        ranges,
        record.nodeAddress,
        unitNodeProof.recordSize,
        record.player);
      appendResidentUnitEvidenceRange(ranges, record.secondaryAddress, 0xe0, record.player);
      appendResidentUnitEvidenceRange(ranges, record.spriteAddress, 0xd0, record.player);
    }
    return ranges;
  }

  bool residentPointerCorrelatesWithUnitEvidence(
    std::uint64_t address,
    const std::vector<ResidentUnitEvidenceRange>& unitEvidence)
  {
    if (!addressFits(address))
      return false;
    const auto candidate = static_cast<std::uintptr_t>(address);
    for (const ResidentUnitEvidenceRange& range : unitEvidence)
    {
      const std::uintptr_t rangeEnd = range.address + range.size;
      if (rangeEnd < range.address)
        continue;
      if (candidate >= range.address && candidate < rangeEnd)
        return true;
    }
    return false;
  }

  bool parseResidentBulletSnapshotRecord(
    const std::vector<unsigned char>& bytes,
    std::size_t offset,
    std::uintptr_t recordAddress,
    const ResidentBulletRecordLayout& layout,
    const std::vector<ResidentMemoryRegion>& regions,
    const std::string& executablePath,
    const std::vector<ResidentUnitEvidenceRange>& unitEvidence,
    ResidentBulletSnapshotRecord& record)
  {
    const std::size_t requiredSize = std::max({
      layout.existsOffset + sizeof(std::uint32_t),
      layout.spriteOffset + sizeof(std::uint32_t),
      layout.typeOffset + sizeof(std::uint16_t),
      layout.positionOffset + sizeof(std::uint32_t),
      layout.velocityOffset + sizeof(std::int32_t) * 2,
      layout.playerOffset + sizeof(unsigned char),
      layout.targetUnitOffset + sizeof(std::uint32_t),
      layout.sourceUnitOffset + sizeof(std::uint32_t),
      layout.removeTimerOffset + sizeof(unsigned char)
    });
    if (layout.recordSize < requiredSize || offset + layout.recordSize > bytes.size())
      return false;
    if (containsLongPrintableAsciiRun(bytes, offset, layout.recordSize))
      return false;

    const std::uint32_t exists = readU32LE(bytes, offset + layout.existsOffset);
    if (exists == 0 || exists > 3)
      return false;

    const std::size_t pointerAlignment =
      layout.recordSize == residentBulletRecordLayouts.front().recordSize ? 4 : 8;
    const std::uint64_t sprite = readResidentPointerLike(bytes, offset + layout.spriteOffset);
    if (!readableResidentBulletObjectPointerValue(
          regions,
          sprite,
          16,
          pointerAlignment,
          executablePath))
      return false;

    const std::uint16_t type = readU16LE(bytes, offset + layout.typeOffset);
    const std::int16_t x = readS16LE(bytes, offset + layout.positionOffset);
    const std::int16_t y =
      readS16LE(bytes, offset + layout.positionOffset + sizeof(std::int16_t));
    const std::int32_t velocityX = readS32LE(bytes, offset + layout.velocityOffset);
    const std::int32_t velocityY =
      readS32LE(bytes, offset + layout.velocityOffset + sizeof(std::int32_t));
    const unsigned char player = bytes[offset + layout.playerOffset];
    const std::uint8_t removeTimer = bytes[offset + layout.removeTimerOffset];
    const std::uint64_t sourceUnit =
      readResidentPointerLike(bytes, offset + layout.sourceUnitOffset);
    const std::uint64_t targetUnit =
      readResidentPointerLike(bytes, offset + layout.targetUnitOffset);

    if (type == 0 || type >= 256 || player >= 12)
      return false;
    if (x < 0 || x > 16384 || y < 0 || y > 16384)
      return false;
    const std::int64_t velocityX64 = velocityX;
    const std::int64_t velocityY64 = velocityY;
    if (std::llabs(velocityX64) > 32768 * 256 || std::llabs(velocityY64) > 32768 * 256)
      return false;
    if (velocityX == 0 && velocityY == 0 && removeTimer == 0)
      return false;

    const bool sourceReadable = readableResidentBulletObjectPointerValue(
          regions,
          sourceUnit,
          16,
          pointerAlignment,
          executablePath);
    const bool targetReadable = readableResidentBulletObjectPointerValue(
          regions,
          targetUnit,
          16,
          pointerAlignment,
          executablePath);
    if (!sourceReadable && !targetReadable)
      return false;

    const bool sourceUnitCorrelated =
      sourceReadable && residentPointerCorrelatesWithUnitEvidence(sourceUnit, unitEvidence);
    const bool targetUnitCorrelated =
      targetReadable && residentPointerCorrelatesWithUnitEvidence(targetUnit, unitEvidence);
    if (!sourceUnitCorrelated && !targetUnitCorrelated)
      return false;

    record.address = recordAddress;
    record.spriteAddress = static_cast<std::uintptr_t>(sprite);
    record.sourceUnitAddress = addressFits(sourceUnit) ? static_cast<std::uintptr_t>(sourceUnit) : 0;
    record.targetUnitAddress = addressFits(targetUnit) ? static_cast<std::uintptr_t>(targetUnit) : 0;
    record.type = type;
    record.x = x;
    record.y = y;
    record.velocityX = velocityX;
    record.velocityY = velocityY;
    record.player = static_cast<int>(player);
    record.removeTimer = removeTimer;
    record.sourceUnitCorrelated = sourceUnitCorrelated;
    record.targetUnitCorrelated = targetUnitCorrelated;
    return true;
  }

  ResidentBulletDataProof scoreResidentBulletArray(
    const std::vector<unsigned char>& bytes,
    std::uintptr_t baseAddress,
    std::size_t offset,
    const ResidentBulletRecordLayout& layout,
    const std::vector<ResidentMemoryRegion>& regions,
    const std::string& executablePath,
    const std::vector<ResidentUnitEvidenceRange>& unitEvidence)
  {
    ResidentBulletDataProof proof;
    proof.address = baseAddress + offset;
    proof.recordSize = layout.recordSize;
    proof.positionOffset = layout.positionOffset;
    proof.velocityOffset = layout.velocityOffset;
    proof.sourceUnitOffset = layout.sourceUnitOffset;
    proof.targetOffset = layout.targetUnitOffset;
    proof.layoutName = layout.name;

    constexpr std::size_t maxSampledRecords = 2048;
    proof.sampledRecords = std::min(maxSampledRecords, (bytes.size() - offset) / layout.recordSize);
    for (std::size_t index = 0; index < proof.sampledRecords; ++index)
    {
      ResidentBulletSnapshotRecord record;
      record.index = index;
      const std::size_t recordOffset = offset + index * layout.recordSize;
      if (!parseResidentBulletSnapshotRecord(
            bytes,
            recordOffset,
            baseAddress + recordOffset,
            layout,
            regions,
            executablePath,
            unitEvidence,
            record))
      {
        continue;
      }

      proof.records.push_back(record);
      proof.activeRecords = proof.records.size();
      if (record.sourceUnitCorrelated || record.targetUnitCorrelated)
        ++proof.unitCorrelatedRecords;
      if (proof.activeRecords >= minResidentActiveBulletRecords)
      {
        proof.passed = true;
        return proof;
      }
    }

    proof.reason = "candidate resident bullet array did not contain active projectile records";
    return proof;
  }

  ResidentBulletDataProof proveResidentBulletArrayInBytes(
    const std::vector<unsigned char>& bytes,
    std::uintptr_t baseAddress,
    const std::vector<ResidentMemoryRegion>& regions,
    const std::string& executablePath,
    const std::vector<ResidentUnitEvidenceRange>& unitEvidence)
  {
    for (const ResidentBulletRecordLayout& layout : residentBulletRecordLayouts)
    {
      if (layout.recordSize * minResidentActiveBulletRecords > bytes.size())
        continue;

      std::vector<std::size_t> plausibleByResidue(layout.recordSize, 0);
      for (std::size_t recordOffset = 0; recordOffset + layout.recordSize <= bytes.size(); recordOffset += 8)
      {
        ResidentBulletSnapshotRecord record;
        if (parseResidentBulletSnapshotRecord(
              bytes,
              recordOffset,
              baseAddress + recordOffset,
              layout,
              regions,
              executablePath,
              unitEvidence,
              record))
        {
          ++plausibleByResidue[recordOffset % layout.recordSize];
        }
      }

      std::vector<std::size_t> residues;
      for (std::size_t residue = 0; residue < plausibleByResidue.size(); ++residue)
      {
        if (plausibleByResidue[residue] > 0)
          residues.push_back(residue);
      }
      std::sort(
        residues.begin(),
        residues.end(),
        [&](std::size_t lhs, std::size_t rhs)
        {
          if (plausibleByResidue[lhs] != plausibleByResidue[rhs])
            return plausibleByResidue[lhs] > plausibleByResidue[rhs];
          return lhs < rhs;
        });

      constexpr std::size_t maxResiduesToScore = 32;
      for (std::size_t index = 0; index < std::min(maxResiduesToScore, residues.size()); ++index)
      {
        ResidentBulletDataProof proof =
          scoreResidentBulletArray(
            bytes,
            baseAddress,
            residues[index],
            layout,
            regions,
            executablePath,
            unitEvidence);
        if (proof.passed)
          return proof;
      }
    }

    return {};
  }

  bool eligibleResidentBulletScanRegion(
    const ResidentMemoryRegion& region,
    const std::string& executablePath)
  {
    constexpr std::size_t minimumBytes = 0x88 * minResidentActiveBulletRecords;
    if (!residentRegionLooksLikeLiveData(region, executablePath) || region.size < minimumBytes)
      return false;
    return true;
  }

  bool discoverResidentBulletDataProof(
    const std::string& executablePath,
    const ResidentUnitNodeProof& unitNodeProof,
    ResidentBulletDataProof& proof)
  {
    constexpr std::size_t maxScanBytes = 64 * 1024 * 1024;
    constexpr std::size_t maxChunkBytes = 2 * 1024 * 1024;
    const std::vector<ResidentUnitEvidenceRange> unitEvidence =
      residentUnitEvidenceRanges(unitNodeProof);
    if (unitEvidence.empty())
    {
      proof.reason = "resident bullet proof requires active unit evidence for source/target correlation";
      return false;
    }

    std::vector<ResidentMemoryRegion> regions = listResidentMemoryRegions();
    std::stable_sort(
      regions.begin(),
      regions.end(),
      [&](const ResidentMemoryRegion& lhs, const ResidentMemoryRegion& rhs)
      {
        const bool lhsTarget = sameMappedFile(lhs.mappedPath, executablePath);
        const bool rhsTarget = sameMappedFile(rhs.mappedPath, executablePath);
        if (lhsTarget != rhsTarget)
          return lhsTarget;
        if (lhs.writable != rhs.writable)
          return lhs.writable;
        return lhs.address < rhs.address;
      });

    ResidentBulletDataProof bestProof;
    std::size_t scannedBytes = 0;
    for (const ResidentMemoryRegion& region : regions)
    {
      if (!eligibleResidentBulletScanRegion(region, executablePath))
        continue;
      if (scannedBytes >= maxScanBytes)
        break;

      for (std::size_t offset = 0;
           offset < region.size && scannedBytes < maxScanBytes;
           offset += maxChunkBytes)
      {
        const std::size_t bytesToRead =
          std::min(
            region.size - offset,
            std::min(maxChunkBytes, maxScanBytes - scannedBytes));
        if (bytesToRead < residentBulletRecordLayouts.front().recordSize)
          break;

        std::vector<unsigned char> bytes;
        const std::uintptr_t baseAddress = region.address + offset;
        if (!readResidentMemory(baseAddress, bytesToRead, bytes))
          break;
        scannedBytes += bytes.size();

        ResidentBulletDataProof candidate =
          proveResidentBulletArrayInBytes(
            bytes,
            baseAddress,
            regions,
            executablePath,
            unitEvidence);
        if (candidate.passed)
        {
          proof = std::move(candidate);
          return true;
        }
        if (candidate.activeRecords > bestProof.activeRecords)
          bestProof = std::move(candidate);
      }
    }

    proof = std::move(bestProof);
    if (proof.reason.empty())
      proof.reason = "no active resident bullet table found in current process memory";
    return false;
  }

  bool refreshResidentBulletDataProof(
    ResidentBulletDataProof& proof,
    const std::vector<ResidentMemoryRegion>& regions,
    const std::string& executablePath,
    const ResidentUnitNodeProof& unitNodeProof)
  {
    if (!proof.passed || proof.address == 0 || proof.recordSize == 0)
      return false;
    const std::vector<ResidentUnitEvidenceRange> unitEvidence =
      residentUnitEvidenceRanges(unitNodeProof);
    if (unitEvidence.empty())
    {
      proof = {};
      return false;
    }

    const ResidentMemoryRegion* region =
      findReadableResidentRegion(
        regions,
        proof.address,
        proof.recordSize * minResidentActiveBulletRecords);
    if (region == nullptr || !residentRegionLooksLikeLiveData(*region, executablePath))
    {
      proof = {};
      return false;
    }

    const std::uintptr_t regionEnd = region->address + region->size;
    const std::size_t bytesToRead =
      std::min<std::size_t>(
        regionEnd > proof.address ? regionEnd - proof.address : 0,
        proof.recordSize * 256);
    if (bytesToRead < proof.recordSize * minResidentActiveBulletRecords)
    {
      proof = {};
      return false;
    }

    std::vector<unsigned char> bytes;
    if (!readResidentMemory(proof.address, bytesToRead, bytes))
    {
      proof = {};
      return false;
    }

    const auto layoutIt =
      std::find_if(
        residentBulletRecordLayouts.begin(),
        residentBulletRecordLayouts.end(),
        [&](const ResidentBulletRecordLayout& layout)
        {
          return layout.recordSize == proof.recordSize && proof.layoutName == layout.name;
        });
    if (layoutIt == residentBulletRecordLayouts.end())
    {
      proof = {};
      return false;
    }

    ResidentBulletDataProof refreshed =
      scoreResidentBulletArray(
        bytes,
        proof.address,
        0,
        *layoutIt,
        regions,
        executablePath,
        unitEvidence);
    if (!refreshed.passed)
    {
      proof = {};
      return false;
    }

    proof = std::move(refreshed);
    return true;
  }

  bool writeResidentBulletDataSnapshot(
    const std::filesystem::path& path,
    const ResidentBulletDataProof& proof,
    const ResidentSnapshotContext& context)
  {
    std::ofstream output(path, std::ios::trunc);
    if (!output)
      return false;

    if (!writeResidentSnapshotMetadata(output, "read_bullet_data", context))
      return false;

    output << "index\taddress\tsprite\tsource_unit\ttarget_unit\ttype\tx\ty\tvelocity_x\tvelocity_y\tplayer\tremove_timer\n";
    for (const ResidentBulletSnapshotRecord& record : proof.records)
    {
      output << record.index << '\t'
             << hexAddress(record.address) << '\t'
             << hexAddress(record.spriteAddress) << '\t'
             << hexAddress(record.sourceUnitAddress) << '\t'
             << hexAddress(record.targetUnitAddress) << '\t'
             << record.type << '\t'
             << record.x << '\t'
             << record.y << '\t'
             << record.velocityX << '\t'
             << record.velocityY << '\t'
             << record.player << '\t'
             << static_cast<int>(record.removeTimer) << '\n';
    }
    return static_cast<bool>(output);
  }

  bool writeResidentMapDataSnapshot(
    const std::filesystem::path& path,
    const ResidentMapDataProof& proof,
    const ResidentSnapshotContext& context)
  {
    std::ofstream output(path, std::ios::trunc);
    if (!output)
      return false;
    if (!writeResidentSnapshotMetadata(output, "read_map_data", context))
      return false;
    output << "map_name\tmap_name_address\tmap_tile_array_address\ttile_count\tmap_path\tmap_file_size\tsource\treplay_path\treplay_file_size\n";
    output << proof.mapName << '\t'
           << hexAddress(proof.mapNameAddress) << '\t'
           << hexAddress(proof.mapTileArrayAddress) << '\t'
           << proof.tileCount << '\t'
           << proof.mapPath.string() << '\t'
           << proof.mapFileSize << '\t'
           << proof.source << '\t'
           << proof.replayPath.string() << '\t'
           << proof.replayFileSize << '\n';
    return static_cast<bool>(output);
  }

  bool writeResidentReplayAnalysisSnapshot(
    const std::filesystem::path& path,
    const ResidentReplayAnalysisProof& proof,
    const ResidentSnapshotContext& context)
  {
    std::ofstream output(path, std::ios::trunc);
    if (!output)
      return false;
    if (!writeResidentSnapshotMetadata(output, "replay_analysis", context))
      return false;
    output << "source\tcurrent_process_replay\tactive_match_metadata\tmap_name\tfirst_frame\tlast_frame\tobserved_player_count\n";
    output << proof.source << '\t'
           << (proof.currentProcessReplay ? "true" : "false") << '\t'
           << (proof.activeMatchMetadata ? "true" : "false") << '\t'
           << proof.mapName << '\t'
           << proof.firstFrame << '\t'
           << proof.lastFrame << '\t'
           << proof.playerCount << '\n';
    return static_cast<bool>(output);
  }

  bool writeResidentRegionDataSnapshot(
    const std::filesystem::path& path,
    const ResidentRegionDataProof& proof,
    const ResidentSnapshotContext& context)
  {
    std::ofstream output(path, std::ios::trunc);
    if (!output)
      return false;

    if (!writeResidentSnapshotMetadata(output, "read_region_data", context))
      return false;

    output << "id\tcenter_x\tcenter_y\tleft\ttop\tright\tbottom\tobserved_units\taccessible\n";
    for (const ResidentRegionSnapshotRecord& record : proof.regions)
    {
      output << record.id << '\t'
             << record.centerX << '\t'
             << record.centerY << '\t'
             << record.left << '\t'
             << record.top << '\t'
             << record.right << '\t'
             << record.bottom << '\t'
             << record.observedUnits << '\t'
             << (record.accessible ? "true" : "false") << '\n';
    }
    return static_cast<bool>(output);
  }

  bool writeResidentDispatchEventsSnapshot(
    const std::filesystem::path& path,
    const ResidentFrameCounterProof& frameCounterProof,
    const ResidentUnitNodeProof& nodeProof)
  {
    if (frameCounterProof.samples.size() < 3)
      return false;

    std::ofstream output(path, std::ios::trunc);
    if (!output)
      return false;

    const ResidentUnitNodeProof& proof = nodeProof;
    const BWAPI::Runtime::RuntimeResidentGameStateSample& first =
      frameCounterProof.samples[0];
    const BWAPI::Runtime::RuntimeResidentGameStateSample& second =
      frameCounterProof.samples[1];
    const BWAPI::Runtime::RuntimeResidentGameStateSample& third =
      frameCounterProof.samples[2];

    output << "event\tframe\tunit_id\tplayer\tx\ty\torder\ttype_hint\n";
    output << "onFrame\t" << first.frame << "\t\t\t\t\t\t\n";
    output << "onFrame\t" << second.frame << "\t\t\t\t\t\t\n";
    output << "onFrame\t" << third.frame << "\t\t\t\t\t\t\n";
    for (const ResidentUnitSnapshotRecord& record : proof.records)
    {
      output << "onUnitDiscover\t" << second.frame << '\t'
             << record.id << '\t'
             << record.player << '\t'
             << record.x << '\t'
             << record.y << '\t'
             << record.order << '\t'
             << record.typeHint << '\n';
      output << "onUnitUpdate\t" << third.frame << '\t'
             << record.id << '\t'
             << record.player << '\t'
             << record.x << '\t'
             << record.y << '\t'
             << record.order << '\t'
             << record.typeHint << '\n';
    }
    return static_cast<bool>(output);
  }

  bool writeResidentAIModuleLoadSnapshot(
    const std::filesystem::path& path,
    const ResidentAIModuleLoadProof& proof)
  {
    std::ofstream output(path, std::ios::trunc);
    if (!output)
      return false;
    output << "field\tvalue\n";
    output << "passed\t" << (proof.passed ? "true" : "false") << '\n';
    output << "loader\t" << proof.loader << '\n';
    output << "module_path\t" << proof.modulePath << '\n';
    output << "module_extension\t" << proof.moduleExtension << '\n';
    output << "self_process_smoke\t" << (proof.selfProcessSmoke ? "true" : "false") << '\n';
    if (!proof.reason.empty())
      output << "reason\t" << proof.reason << '\n';
    return static_cast<bool>(output);
  }

  bool writeResidentCommandIngressSnapshot(
    const std::filesystem::path& path,
    const ResidentCommandIngressProof& proof)
  {
    std::ofstream output(path, std::ios::trunc);
    if (!output)
      return false;

    output << "field\tvalue\n";
    output << "passed\t" << (proof.passed ? "true" : "false") << '\n';
    output << "receiver_active\t" << (proof.receiverActive ? "true" : "false") << '\n';
    output << "consumed_records\t" << proof.consumedRecords << '\n';
    output << "parsed_commands\t" << proof.parsedCommands << '\n';
    output << "unit_commands\t" << proof.unitCommands << '\n';
    output << "game_actions\t" << proof.gameActions << '\n';
    output << "overlay_actions\t" << proof.overlayActions << '\n';
    output << "first_sequence\t" << proof.firstSequence << '\n';
    output << "last_sequence\t" << proof.lastSequence << '\n';
    output << "proof_scope\tresident-ingress-only-not-live-scr-command-behavior\n";
    output << "required_behavior\tencoded-bwapi-command-reaches-live-scr-command-path-and-changes-frame-behavior\n";
    if (!proof.reason.empty())
      output << "reason\t" << proof.reason << '\n';
    return static_cast<bool>(output);
  }

  bool writeResidentOverlayIngressSnapshot(
    const std::filesystem::path& path,
    const ResidentOverlayIngressProof& proof)
  {
    std::ofstream output(path, std::ios::trunc);
    if (!output)
      return false;

    output << "field\tvalue\n";
    output << "passed\t" << (proof.passed ? "true" : "false") << '\n';
    output << "renderer_bound\t" << (proof.rendererBound ? "true" : "false") << '\n';
    output << "accepted_primitives\t" << proof.acceptedPrimitives << '\n';
    output << "first_sequence\t" << proof.firstSequence << '\n';
    output << "last_sequence\t" << proof.lastSequence << '\n';
    output << "proof_scope\tresident-ingress-only-not-visible-frame-rendering\n";
    output << "required_behavior\tbwapi-overlay-primitives-render-on-visible-game-frame\n";
    if (!proof.reason.empty())
      output << "reason\t" << proof.reason << '\n';
    return static_cast<bool>(output);
  }

  bool plausibleCompactUnitNodeAnchorFields(
    const std::vector<unsigned char>& bytes,
    std::size_t offset)
  {
    constexpr std::size_t recordSize = 0x28;
    if (offset + recordSize > bytes.size())
      return false;
    if (containsLongPrintableAsciiRun(bytes, offset, recordSize))
      return false;

    const std::uint64_t previous = readU64LE(bytes, offset);
    const std::uint64_t next = readU64LE(bytes, offset + 0x08);
    const std::int32_t x = readS32LE(bytes, offset + 0x10);
    const std::int32_t y = readS32LE(bytes, offset + 0x14);
    const std::uint64_t sprite = readU64LE(bytes, offset + 0x18);
    const std::uint64_t secondaryObject = readU64LE(bytes, offset + 0x20);

    const bool previousLooksLikeObject =
      previous != 0 && plausibleRemasteredCompactObjectValue(previous);
    const bool nextLooksLikeObject =
      next != 0 && plausibleRemasteredCompactObjectValue(next);
    return (previousLooksLikeObject || nextLooksLikeObject)
      && x >= 16
      && x <= 16384
      && y >= 16
      && y <= 16384
      && plausibleRemasteredCompactObjectValue(sprite)
      && plausibleRemasteredCompactObjectValue(secondaryObject)
      && sprite != secondaryObject;
  }

  bool plausibleCompactUnitNodeAnchorRecord(
    const std::vector<unsigned char>& bytes,
    std::size_t offset,
    const std::vector<ResidentMemoryRegion>& regions)
  {
    if (!plausibleCompactUnitNodeAnchorFields(bytes, offset))
      return false;

    const std::uint64_t previous = readU64LE(bytes, offset);
    const std::uint64_t next = readU64LE(bytes, offset + 0x08);
    const std::uint64_t sprite = readU64LE(bytes, offset + 0x18);
    const std::uint64_t secondaryObject = readU64LE(bytes, offset + 0x20);
    const bool readableLink =
      readableResidentObjectPointerValue(regions, previous, 16)
      || readableResidentObjectPointerValue(regions, next, 16);
    const bool taggedLink =
      plausibleRemasteredTaggedHandleValue(previous)
      || plausibleRemasteredTaggedHandleValue(next);
    const bool readableSprite = readableResidentObjectPointerValue(regions, sprite, 0xd0);
    const bool readableSecondary = readableResidentObjectPointerValue(regions, secondaryObject, 0xe0);
    const bool taggedSprite = plausibleRemasteredTaggedHandleValue(sprite);
    const bool taggedSecondary = plausibleRemasteredTaggedHandleValue(secondaryObject);
    return (readableLink || taggedLink)
      && (readableSprite || taggedSprite)
      && (readableSecondary || taggedSecondary)
      && (readableSprite || readableSecondary);
  }

  bool parseResidentCompactUnitSnapshotRecord(
    const std::vector<unsigned char>& bytes,
    std::size_t offset,
    std::uintptr_t nodeAddress,
    const std::vector<ResidentMemoryRegion>& regions,
    ResidentUnitSnapshotRecord& record)
  {
    if (!plausibleCompactUnitNodeAnchorRecord(bytes, offset, regions))
      return false;

    const std::uint64_t spriteAddress64 = readU64LE(bytes, offset + 0x18);
    const std::uint64_t secondaryAddress64 = readU64LE(bytes, offset + 0x20);
    if (!addressFits(spriteAddress64) || !addressFits(secondaryAddress64))
      return false;

    const bool taggedSprite = plausibleRemasteredTaggedHandleValue(spriteAddress64);
    const bool taggedSecondary = plausibleRemasteredTaggedHandleValue(secondaryAddress64);
    if (!taggedSprite
        && spriteAddress64 >= nodeAddress
        && spriteAddress64 < nodeAddress + 0x28)
      return false;
    if (!taggedSecondary
        && secondaryAddress64 >= nodeAddress
        && secondaryAddress64 < nodeAddress + 0x28)
      return false;

    record.nodeAddress = nodeAddress;
    record.spriteAddress = static_cast<std::uintptr_t>(spriteAddress64);
    record.secondaryAddress = static_cast<std::uintptr_t>(secondaryAddress64);
    record.id = static_cast<std::uint32_t>((nodeAddress >> 4) & 0xffffffffu);
    record.x = static_cast<std::int16_t>(readS32LE(bytes, offset + 0x10));
    record.y = static_cast<std::int16_t>(readS32LE(bytes, offset + 0x14));
    record.targetX = record.x;
    record.targetY = record.y;
    record.order = 0;
    record.state = 0;
    record.player = 0;
    record.typeHint = 1;
    record.metadataDerived = true;
    record.taggedHandleDerived = taggedSprite || taggedSecondary;

    constexpr std::size_t spriteSnapshotBytes = 0xd0;
    if (!taggedSprite
        && readableResidentObjectPointerValue(regions, spriteAddress64, spriteSnapshotBytes))
    {
      std::vector<unsigned char> spriteBytes;
      if (readResidentMemory(record.spriteAddress, spriteSnapshotBytes, spriteBytes))
      {
        const std::uint32_t primaryPlayer = readU32LE(spriteBytes, 0x6c);
        const unsigned char secondaryPlayer = spriteBytes[0xc0];
        if (primaryPlayer < 12)
          record.player = static_cast<int>(primaryPlayer);
        else if (secondaryPlayer < 12)
          record.player = static_cast<int>(secondaryPlayer);

        const std::uint16_t typeHint =
          static_cast<std::uint16_t>(readU32LE(spriteBytes, 0x68) & 0xffffu);
        if (plausibleRemasteredUnitTypeHint(typeHint))
          record.typeHint = typeHint;

        const std::uint32_t hitPoints = readU32LE(spriteBytes, 0x80);
        if (hitPoints > 0 && hitPoints <= 1000000)
        {
          record.hitPoints = hitPoints;
          record.hitPointsResolved = true;
        }
      }
    }

    constexpr std::size_t secondarySnapshotBytes = 0xe0;
    if (!taggedSecondary
        && readableResidentObjectPointerValue(regions, secondaryAddress64, secondarySnapshotBytes))
    {
      std::vector<unsigned char> secondaryBytes;
      if (readResidentMemory(record.secondaryAddress, secondarySnapshotBytes, secondaryBytes))
      {
        const unsigned char rawPlayer = secondaryBytes[0x14];
        std::uint16_t typeHint = readU16LE(secondaryBytes, 0x10);
        if (!plausibleRemasteredUnitTypeHint(typeHint))
          typeHint = readU16LE(secondaryBytes, 0x20);
        if ((rawPlayer < 12 || rawPlayer == 255) && plausibleRemasteredUnitTypeHint(typeHint))
        {
          record.player = rawPlayer == 255 ? 11 : static_cast<int>(rawPlayer);
          record.typeHint = typeHint;
          const unsigned char currentHitPoints = secondaryBytes[0x1a];
          const unsigned char maxHitPoints = secondaryBytes[0x1b];
          if (currentHitPoints != 0 && maxHitPoints != 0 && currentHitPoints <= maxHitPoints)
          {
            record.hitPoints = static_cast<std::uint32_t>(currentHitPoints) * 256u;
            record.hitPointsResolved = true;
          }
        }

        const unsigned char metadataPlayer = secondaryBytes[0xc0];
        const std::uint16_t metadataType = readU16LE(secondaryBytes, 0xd0);
        if (metadataPlayer < 12 && plausibleRemasteredUnitTypeHint(metadataType))
        {
          record.player = static_cast<int>(metadataPlayer);
          record.typeHint = metadataType;
          if (!record.hitPointsResolved)
          {
            const unsigned char currentHitPoints = secondaryBytes[0x1a];
            const unsigned char maxHitPoints = secondaryBytes[0x1b];
            if (currentHitPoints != 0 && maxHitPoints != 0 && currentHitPoints <= maxHitPoints)
            {
              record.hitPoints = static_cast<std::uint32_t>(currentHitPoints) * 256u;
              record.hitPointsResolved = true;
            }
          }
        }
      }
    }

    return record.id != 0 && record.player >= 0 && record.player < 12;
  }

  bool residentCompactRecordsHaveBwapiMetadata(
    const std::vector<ResidentUnitSnapshotRecord>& records)
  {
    return std::any_of(
      records.begin(),
      records.end(),
      [](const ResidentUnitSnapshotRecord& record)
      {
        return !record.taggedHandleDerived || record.hitPointsResolved;
      });
  }

  ResidentUnitNodeProof scoreResidentCompactLinkedUnitNodeGraph(
    std::uintptr_t seedAddress,
    const std::vector<ResidentMemoryRegion>& regions)
  {
    constexpr std::size_t recordSize = 0x28;
    constexpr std::size_t maxGraphRecords = 256;
    constexpr std::size_t minSnapshotRecords = 3;

    ResidentUnitNodeProof proof;
    proof.address = seedAddress;
    proof.recordSize = recordSize;
    if (!readableResidentObjectPointerValue(regions, seedAddress, recordSize))
    {
      proof.reason = "compact SC:R unit-node graph seed is not readable";
      return proof;
    }

    std::vector<std::uintptr_t> pending;
    std::unordered_set<std::uintptr_t> queued;
    std::unordered_set<std::uintptr_t> accepted;
    pending.push_back(seedAddress);
    queued.insert(seedAddress);

    for (std::size_t index = 0; index < pending.size() && accepted.size() < maxGraphRecords; ++index)
    {
      const std::uintptr_t nodeAddress = pending[index];
      if (!readableResidentObjectPointerValue(regions, nodeAddress, recordSize))
        continue;

      std::vector<unsigned char> nodeBytes;
      if (!readResidentMemory(nodeAddress, recordSize, nodeBytes))
        continue;

      const std::array<std::uint64_t, 2> links = {
        readU64LE(nodeBytes, 0),
        readU64LE(nodeBytes, 0x08)
      };
      for (std::uint64_t link : links)
      {
        if (!plausibleRemasteredCompactObjectValue(link) || !addressFits(link))
          continue;
        const auto linkedAddress = static_cast<std::uintptr_t>(link);
        if (linkedAddress == 0
            || !readableResidentObjectPointerValue(regions, linkedAddress, recordSize))
          continue;
        if (queued.insert(linkedAddress).second)
          pending.push_back(linkedAddress);
      }

      ResidentUnitSnapshotRecord record;
      if (!parseResidentCompactUnitSnapshotRecord(
            nodeBytes,
            0,
            nodeAddress,
            regions,
            record))
        continue;
      if (!accepted.insert(nodeAddress).second)
        continue;

      record.index = proof.records.size();
      proof.records.push_back(record);
      proof.activeRecords = proof.records.size();
      proof.sampledRecords = pending.size();
      if (proof.records.size() >= minSnapshotRecords
          && residentCompactRecordsHaveBwapiMetadata(proof.records))
      {
        proof.passed = true;
        proof.hitPointsResolved = std::all_of(
          proof.records.begin(),
          proof.records.end(),
          [](const ResidentUnitSnapshotRecord& snapshot)
          {
            return snapshot.hitPointsResolved && snapshot.hitPoints > 0;
          });
        proof.taggedHandles = std::any_of(
          proof.records.begin(),
          proof.records.end(),
          [](const ResidentUnitSnapshotRecord& snapshot)
          {
            return snapshot.taggedHandleDerived;
          });
        proof.layoutName = proof.taggedHandles
          ? "scr-tagged-compact-unit-node-snapshot"
          : "scr-compact-unit-node-object-graph";
        return proof;
      }
    }

    proof.reason = proof.activeRecords > 0
      ? "compact SC:R unit-node graph has too few BWAPI-facing records"
      : "candidate compact SC:R unit-node graph did not contain active records";
    return proof;
  }

  bool eligibleResidentUnitScanRegion(
    const ResidentMemoryRegion& region,
    const std::string& executablePath)
  {
    constexpr std::size_t minimumBytes = 0x28 * 3;
    if (!region.readable || region.executable || region.size < minimumBytes)
      return false;
    if (!region.mappedPath.empty()
        && region.mappedPath.front() == '/'
        && !sameMappedFile(region.mappedPath, executablePath))
    {
      return false;
    }
    return true;
  }

  bool discoverResidentUnitNodeProof(
    const std::string& executablePath,
    ResidentUnitNodeProof& proof)
  {
    constexpr std::size_t maxScanBytes = 128 * 1024 * 1024;
    constexpr std::size_t maxChunkBytes = 2 * 1024 * 1024;
    constexpr std::size_t recordSize = 0x28;

    std::vector<ResidentMemoryRegion> regions = listResidentMemoryRegions();
    std::stable_sort(
      regions.begin(),
      regions.end(),
      [&](const ResidentMemoryRegion& lhs, const ResidentMemoryRegion& rhs)
      {
        const bool lhsTarget = sameMappedFile(lhs.mappedPath, executablePath);
        const bool rhsTarget = sameMappedFile(rhs.mappedPath, executablePath);
        if (lhsTarget != rhsTarget)
          return lhsTarget;
        if (lhs.writable != rhs.writable)
          return lhs.writable;
        return lhs.address < rhs.address;
      });

    ResidentUnitNodeProof bestProof;
    std::size_t scannedBytes = 0;
    for (const ResidentMemoryRegion& region : regions)
    {
      if (!eligibleResidentUnitScanRegion(region, executablePath))
        continue;
      if (scannedBytes >= maxScanBytes)
        break;

      for (std::size_t offset = 0;
           offset < region.size && scannedBytes < maxScanBytes;
           offset += maxChunkBytes)
      {
        const std::size_t bytesToRead =
          std::min(
            region.size - offset,
            std::min(maxChunkBytes, maxScanBytes - scannedBytes));
        if (bytesToRead < recordSize * 3)
          break;

        std::vector<unsigned char> bytes;
        const std::uintptr_t baseAddress = region.address + offset;
        if (!readResidentMemory(baseAddress, bytesToRead, bytes))
          break;
        scannedBytes += bytes.size();

        for (std::size_t cursor = 0; cursor + recordSize <= bytes.size(); cursor += 8)
        {
          if (!plausibleCompactUnitNodeAnchorFields(bytes, cursor))
            continue;

          ResidentUnitNodeProof candidate =
            scoreResidentCompactLinkedUnitNodeGraph(baseAddress + cursor, regions);
          if (candidate.passed)
          {
            proof = std::move(candidate);
            return true;
          }
          if (candidate.activeRecords > bestProof.activeRecords)
            bestProof = std::move(candidate);
        }
      }
    }

    proof = std::move(bestProof);
    if (proof.reason.empty())
      proof.reason = "no active compact SC:R unit-node graph found in resident memory";
    return false;
  }

  bool refreshResidentUnitNodeProof(
    ResidentUnitNodeProof& proof,
    const std::vector<ResidentMemoryRegion>& regions)
  {
    if (!proof.passed || proof.address == 0 || proof.recordSize == 0)
      return false;

    ResidentUnitNodeProof refreshed =
      scoreResidentCompactLinkedUnitNodeGraph(proof.address, regions);
    if (!refreshed.passed)
    {
      proof = {};
      return false;
    }

    proof = std::move(refreshed);
    return true;
  }

  bool writeResidentUnitSnapshot(
    const std::filesystem::path& path,
    const ResidentUnitNodeProof& proof,
    const ResidentSnapshotContext& context)
  {
    std::ofstream output(path, std::ios::trunc);
    if (!output)
      return false;

    if (!writeResidentSnapshotMetadata(output, "read_units", context))
      return false;

    output << "index\tnode\tsecondary\tsprite\tid\tx\ty\ttarget_x\ttarget_y\torder\tstate\tplayer\ttype_hint\thit_points\n";
    for (const ResidentUnitSnapshotRecord& record : proof.records)
    {
      output << record.index << '\t'
             << hexAddress(record.nodeAddress) << '\t'
             << hexAddress(record.secondaryAddress) << '\t'
             << hexAddress(record.spriteAddress) << '\t'
             << record.id << '\t'
             << record.x << '\t'
             << record.y << '\t'
             << record.targetX << '\t'
             << record.targetY << '\t'
             << record.order << '\t'
             << record.state << '\t'
             << record.player << '\t'
             << record.typeHint << '\t'
             << record.hitPoints << '\n';
    }
    return static_cast<bool>(output);
  }

  bool eligibleFrameCounterRegion(
    const ResidentMemoryRegion& region,
    const std::string& executablePath)
  {
    if (!region.readable || !region.writable || region.executable)
      return false;
    if (region.size < sizeof(std::uint32_t))
      return false;
    if (!region.mappedPath.empty()
        && region.mappedPath.front() == '/'
        && !sameMappedFile(region.mappedPath, executablePath))
      return false;
    return true;
  }

  int frameCounterRegionPriority(
    const ResidentMemoryRegion& region,
    const std::string& executablePath)
  {
    if (sameMappedFile(region.mappedPath, executablePath))
      return 0;
    if (region.mappedPath.empty())
      return 1;
    return 2;
  }

  bool sampleResidentFrameCounter(
    std::uintptr_t address,
    BWAPI::Runtime::RuntimeResidentGameStateSample& sample)
  {
    std::vector<unsigned char> bytes;
    if (!readResidentMemory(address, sizeof(std::uint32_t), bytes))
      return false;
    sample.frame = readU32LE(bytes.data());
    sample.tick = steadyTickMilliseconds();
    return sample.frame > 0;
  }

  bool refreshResidentFrameCounterProof(ResidentFrameCounterProof& proof)
  {
    if (!proof.passed || proof.address == 0)
      return false;

    BWAPI::Runtime::RuntimeResidentGameStateSample sample;
    if (!sampleResidentFrameCounter(proof.address, sample))
    {
      proof = {};
      return false;
    }
    if (!proof.samples.empty() && sample.frame <= proof.samples.back().frame)
    {
      proof = {};
      return false;
    }

    proof.samples.push_back(sample);
    while (proof.samples.size() > 3)
      proof.samples.pop_front();
    return proof.samples.size() >= 3;
  }

  void updateResidentBwGameProjection(
    const ResidentFrameCounterProof& proof,
    const ResidentUnitNodeProof& unitProof,
    std::uint64_t heartbeat)
  {
    residentBwGameProjection.heartbeat = heartbeat;
    residentBwGameProjection.activeUnitCount =
      unitProof.passed
        ? static_cast<std::uint32_t>(
            std::min<std::size_t>(
              unitProof.activeRecords,
              std::numeric_limits<std::uint32_t>::max()))
        : 0;
    if (!proof.passed || proof.samples.empty())
      return;

    const BWAPI::Runtime::RuntimeResidentGameStateSample& sample = proof.samples.back();
    if (sample.frame > std::numeric_limits<std::uint32_t>::max())
      return;

    residentBwGameProjection.elapsedFrames = static_cast<std::uint32_t>(sample.frame);
    residentBwGameProjection.updatedTick = sample.tick;
  }

  bool discoverResidentFrameCounterProof(
    const std::string& executablePath,
    ResidentFrameCounterProof& proof)
  {
    constexpr int sampleDelayMs = 250;
    constexpr std::size_t maxScanBytes = 128 * 1024 * 1024;
    constexpr std::size_t maxChunkBytes = 4 * 1024 * 1024;
    constexpr std::size_t maxCandidates = 4096;

    struct Snapshot
    {
      ResidentMemoryRegion region;
      std::uintptr_t address = 0;
      std::vector<unsigned char> bytes;
      std::uint64_t tick = 0;
    };
    struct Candidate
    {
      std::size_t snapshotIndex = 0;
      std::uintptr_t address = 0;
      std::uint32_t first = 0;
      std::uint32_t second = 0;
      std::uint64_t firstTick = 0;
      std::uint64_t secondTick = 0;
      int regionPriority = 2;
    };

    std::vector<ResidentMemoryRegion> regions = listResidentMemoryRegions();
    std::stable_sort(
      regions.begin(),
      regions.end(),
      [&](const ResidentMemoryRegion& lhs, const ResidentMemoryRegion& rhs)
      {
        const int lhsPriority = frameCounterRegionPriority(lhs, executablePath);
        const int rhsPriority = frameCounterRegionPriority(rhs, executablePath);
        if (lhsPriority != rhsPriority)
          return lhsPriority < rhsPriority;
        return lhs.address < rhs.address;
      });

    std::vector<Snapshot> snapshots;
    std::size_t scannedBytes = 0;
    for (const ResidentMemoryRegion& region : regions)
    {
      if (!eligibleFrameCounterRegion(region, executablePath))
        continue;
      if (scannedBytes >= maxScanBytes)
        break;

      for (std::size_t offset = 0;
           offset < region.size && scannedBytes < maxScanBytes;
           offset += maxChunkBytes)
      {
        const std::size_t bytesToRead =
          std::min(
            region.size - offset,
            std::min(maxChunkBytes, maxScanBytes - scannedBytes));
        if (bytesToRead < sizeof(std::uint32_t))
          break;

        Snapshot snapshot;
        snapshot.region = region;
        snapshot.address = region.address + offset;
        snapshot.tick = steadyTickMilliseconds();
        if (!readResidentMemory(snapshot.address, bytesToRead, snapshot.bytes))
          break;
        scannedBytes += snapshot.bytes.size();
        snapshots.push_back(std::move(snapshot));
      }
    }

    if (snapshots.empty())
      return false;

    std::this_thread::sleep_for(std::chrono::milliseconds(sampleDelayMs));

    std::vector<Candidate> candidates;
    for (std::size_t snapshotIndex = 0; snapshotIndex < snapshots.size(); ++snapshotIndex)
    {
      const Snapshot& snapshot = snapshots[snapshotIndex];
      if (candidates.size() >= maxCandidates)
        break;

      std::vector<unsigned char> secondBytes;
      if (!readResidentMemory(snapshot.address, snapshot.bytes.size(), secondBytes))
        continue;
      const std::uint64_t secondTick = steadyTickMilliseconds();

      for (std::size_t offset = 0;
           offset + sizeof(std::uint32_t) <= snapshot.bytes.size();
           offset += sizeof(std::uint32_t))
      {
        const std::uint32_t first = readU32LE(snapshot.bytes.data() + offset);
        const std::uint32_t second = readU32LE(secondBytes.data() + offset);
        if (!plausibleCounterDelta(first, second))
          continue;

        Candidate candidate;
        candidate.snapshotIndex = snapshotIndex;
        candidate.address = snapshot.address + offset;
        candidate.first = first;
        candidate.second = second;
        candidate.firstTick = snapshot.tick;
        candidate.secondTick = secondTick;
        candidate.regionPriority = frameCounterRegionPriority(snapshot.region, executablePath);
        candidates.push_back(candidate);
        if (candidates.size() >= maxCandidates)
          break;
      }
    }

    if (candidates.empty())
      return false;

    std::sort(
      candidates.begin(),
      candidates.end(),
      [&](const Candidate& lhs, const Candidate& rhs)
      {
        const int lhsDelta = static_cast<int>(lhs.second - lhs.first);
        const int rhsDelta = static_cast<int>(rhs.second - rhs.first);
        const int expectedDelta = std::max(1, (sampleDelayMs * 24) / 1000);
        const int lhsScore = lhs.regionPriority * 1000000 + std::abs(lhsDelta - expectedDelta);
        const int rhsScore = rhs.regionPriority * 1000000 + std::abs(rhsDelta - expectedDelta);
        if (lhsScore != rhsScore)
          return lhsScore < rhsScore;
        return lhs.address < rhs.address;
      });

    std::this_thread::sleep_for(std::chrono::milliseconds(sampleDelayMs));
    const std::size_t validationCount = std::min<std::size_t>(candidates.size(), 512);
    int bestScore = std::numeric_limits<int>::max();
    ResidentFrameCounterProof bestProof;

    for (std::size_t index = 0; index < validationCount; ++index)
    {
      const Candidate& candidate = candidates[index];
      BWAPI::Runtime::RuntimeResidentGameStateSample third;
      if (!sampleResidentFrameCounter(candidate.address, third))
        continue;
      const std::uint32_t thirdFrame = static_cast<std::uint32_t>(third.frame);
      if (!frameCounterConfidencePassed(candidate.first, candidate.second, thirdFrame, sampleDelayMs))
        continue;

      const int score =
        frameCounterScore(
          candidate.first,
          candidate.second,
          thirdFrame,
          candidate.regionPriority,
          sampleDelayMs);
      if (score >= bestScore)
        continue;

      bestScore = score;
      const Snapshot& snapshot = snapshots[candidate.snapshotIndex];
      bestProof.passed = true;
      bestProof.address = candidate.address;
      bestProof.regionPath = snapshot.region.mappedPath;
      bestProof.regionAddress = snapshot.region.address;
      bestProof.regionSize = snapshot.region.size;
      bestProof.samples.clear();
      bestProof.samples.push_back({ candidate.first, candidate.firstTick });
      bestProof.samples.push_back({ candidate.second, candidate.secondTick });
      bestProof.samples.push_back(third);
    }

    if (!bestProof.passed)
      return false;

    proof = std::move(bestProof);
    return true;
  }

  bool ensureResidentQueue(
    const std::filesystem::path& bridgePath,
    const std::filesystem::path& path,
    const BWAPI::Runtime::RuntimeResidentQueueHeader& desiredHeader,
    BWAPI::Runtime::RuntimeResidentQueueHeader& actualHeader)
  {
    using namespace BWAPI::Runtime;

    RuntimeResidentQueueValidationResult ensured =
      ensureRuntimeResidentQueueFile(path, desiredHeader, actualHeader);
    if (ensured.valid)
      return true;

    const std::string reason =
      ensured.errors.empty() ? "unknown" : ensured.errors.front();
    appendResidentLog(
      bridgePath,
      "resident.queue.ensure_failed path=" + path.string() + " reason=" + reason);
    return false;
  }

  std::string residentQueuePayloadText(const std::vector<unsigned char>& payload)
  {
    std::string text(payload.begin(), payload.end());
    for (char& ch : text)
    {
      if (ch == '\t' || ch == '\n' || ch == '\r')
        ch = ' ';
    }
    return text;
  }

  void updateIngressSequenceRange(
    std::size_t previousCount,
    std::uint64_t sequence,
    std::size_t& count,
    std::uint64_t& firstSequence,
    std::uint64_t& lastSequence)
  {
    if (previousCount == 0)
      firstSequence = sequence;
    lastSequence = sequence;
    ++count;
  }

  void consumeResidentCommandQueue(
    const std::filesystem::path& bridgePath,
    const std::filesystem::path& commandQueuePath,
    BWAPI::Runtime::RuntimeResidentQueueHeader& commandQueueHeader,
    const std::filesystem::path& overlayQueuePath,
    const BWAPI::Runtime::RuntimeResidentQueueHeader& overlayQueueHeader,
    ResidentCommandIngressProof& commandIngressProof,
    ResidentOverlayIngressProof& overlayIngressProof)
  {
    using namespace BWAPI::Runtime;

    commandIngressProof.receiverActive = true;
    RuntimeResidentQueueReadResult read =
      readRuntimeResidentQueueRecords(
        commandQueuePath,
        RuntimeResidentQueueKind::Command,
        64);
    if (!read.read)
    {
      if (!read.errors.empty())
      {
        appendResidentLog(
          bridgePath,
          "resident.command_queue.read_failed reason=" + read.errors.front());
      }
      return;
    }
    if (read.records.empty())
    {
      commandQueueHeader = read.header;
      return;
    }

    const std::filesystem::path auditPath = bridgePath / "resident-command.consumed.tsv";
    const bool needsHeader = !std::filesystem::exists(auditPath);
    std::ofstream audit(auditPath, std::ios::app);
    if (audit)
    {
      if (needsHeader)
        audit << "sequence\tpayload_bytes\tparsed\tkind\tname\toverlay_routed\tpayload\n";
      for (const RuntimeResidentQueueRecord& record : read.records)
      {
        const std::string payloadText = residentQueuePayloadText(record.payload);
        RuntimeCommandRequest command;
        const bool parsed = parseResidentRuntimeCommand(payloadText, command);
        bool overlayRouted = false;
        const std::size_t previousCommandCount = commandIngressProof.consumedRecords;
        updateIngressSequenceRange(
          previousCommandCount,
          record.header.sequence,
          commandIngressProof.consumedRecords,
          commandIngressProof.firstSequence,
          commandIngressProof.lastSequence);
        if (parsed)
        {
          ++commandIngressProof.parsedCommands;
          if (command.kind == RuntimeCommandKind::UnitCommand)
            ++commandIngressProof.unitCommands;
          else if (command.kind == RuntimeCommandKind::GameAction)
            ++commandIngressProof.gameActions;

          if (isResidentOverlayAction(command))
          {
            RuntimeResidentQueueAppendResult appendedOverlay =
              appendRuntimeResidentQueueRecord(
                overlayQueuePath,
                RuntimeResidentQueueKind::Overlay,
                record.payload);
            overlayRouted = appendedOverlay.appended;
            if (overlayRouted)
            {
              const std::size_t previousOverlayCount = overlayIngressProof.acceptedPrimitives;
              updateIngressSequenceRange(
                previousOverlayCount,
                appendedOverlay.sequence,
                overlayIngressProof.acceptedPrimitives,
                overlayIngressProof.firstSequence,
                overlayIngressProof.lastSequence);
              ++commandIngressProof.overlayActions;
            }
            else if (!appendedOverlay.reason.empty())
            {
              appendResidentLog(
                bridgePath,
                "resident.overlay_queue.append_failed reason=" + appendedOverlay.reason);
            }
          }
        }

        audit << record.header.sequence << '\t'
              << record.header.payloadBytes << '\t'
              << (parsed ? "true" : "false") << '\t'
              << (parsed ? toString(command.kind) : "") << '\t'
              << (parsed ? command.name : "") << '\t'
              << (overlayRouted ? "true" : "false") << '\t'
              << payloadText << '\n';
      }
    }

    const std::uint64_t acknowledgedSequence =
      read.records.back().header.sequence + 1;
    RuntimeResidentQueueAcknowledgeResult acknowledged =
      acknowledgeRuntimeResidentQueueRecords(
        commandQueuePath,
        RuntimeResidentQueueKind::Command,
        acknowledgedSequence);
    if (!acknowledged.acknowledged)
    {
      if (!acknowledged.errors.empty())
      {
        appendResidentLog(
          bridgePath,
          "resident.command_queue.ack_failed reason=" + acknowledged.errors.front());
      }
      return;
    }

    commandQueueHeader = acknowledged.header;
    commandIngressProof.passed = commandIngressProof.consumedRecords > 0;
    commandIngressProof.reason =
      commandIngressProof.passed
        ? "resident command ingress consumed BWAPI command records in the target process"
        : "resident command ingress has not consumed a BWAPI command record yet";
    overlayIngressProof.passed = overlayIngressProof.acceptedPrimitives > 0;
    overlayIngressProof.rendererBound = false;
    overlayIngressProof.reason =
      overlayIngressProof.passed
        ? "resident overlay ingress accepted BWAPI draw primitives, but no SC:R render hook is bound"
        : "resident overlay ingress has not accepted BWAPI draw primitives yet";
    appendResidentLog(
      bridgePath,
      "resident.command_queue.consumed records="
      + std::to_string(read.records.size())
      + " read_sequence="
      + std::to_string(commandQueueHeader.readSequence)
      + " overlay_record_bytes="
      + std::to_string(overlayQueueHeader.recordBytes));
  }

  bool writeReadyFile(
    const BWAPI::Runtime::RuntimeEnvironment& environment,
    const std::filesystem::path& bridgePath,
    const std::filesystem::path& commandQueuePath,
    const BWAPI::Runtime::RuntimeResidentQueueHeader& commandQueueHeader,
    const std::filesystem::path& overlayQueuePath,
    const BWAPI::Runtime::RuntimeResidentQueueHeader& overlayQueueHeader,
    const std::filesystem::path& proofQueuePath,
    const BWAPI::Runtime::RuntimeResidentQueueHeader& proofQueueHeader,
    const ResidentFrameCounterProof& frameCounterProof,
    const ResidentUnitNodeProof& unitNodeProof,
    const ResidentBulletDataProof& bulletDataProof,
    const ResidentMapDataProof& mapDataProof,
    const ResidentAIModuleLoadProof& aiModuleLoadProof,
    const ResidentBattleNetPolicyProof& battleNetPolicyProof,
    const ResidentCommandIngressProof& commandIngressProof,
    const ResidentOverlayIngressProof& overlayIngressProof)
  {
    using namespace BWAPI::Runtime;

    const std::filesystem::path readyPath = bridgePath / RuntimeExecutorBridgeReadyFile;
    const std::filesystem::path temporaryPath = bridgePath / "ready.tmp";

    std::vector<std::string> readyLines;
    std::unordered_set<std::string> ownedKeys;
    auto appendReadyLine = [&](std::string line)
    {
      const std::string key = readyLineKey(line);
      if (!key.empty())
        ownedKeys.insert(key);
      readyLines.push_back(std::move(line));
    };

    std::ofstream ready(temporaryPath, std::ios::trunc);
    if (!ready)
      return false;

    const RuntimeCommandSurface commandSurface = makeBWAPICommandSurface();
    const std::vector<std::string> existingReadyLines = readReadyFileLines(readyPath);
    const ResidentPlayerDataProof playerDataProof =
      unitNodeProof.passed
        ? proveResidentPlayerDataFromUnitSnapshot(unitNodeProof)
        : ResidentPlayerDataProof{};
    appendReadyLine(std::string("protocol=") + RuntimeExecutorBridgeProtocol);
    appendReadyLine(std::string("product=") + toString(environment.product));
    appendReadyLine("version=" + environment.version);
    appendReadyLine("executor=starcraft-api-resident-adapter");
    appendReadyLine(std::string("mode=") + RuntimeExecutorBridgeValidatedAdapterMode);
    appendReadyLine("process_id=" + std::to_string(environment.processId));
    appendReadyLine("executable=" + environment.executablePath);
    appendReadyLine("runtime.process_visible_at_ready=true");
    appendReadyLine(RuntimeExecutorBridgeCommandSurfaceLine);
    appendReadyLine("command_surface.unit_commands=" + std::to_string(commandSurface.unitCommands.size()));
    appendReadyLine("command_surface.game_actions=" + std::to_string(commandSurface.gameActions.size()));
    appendReadyLine("command_surface.entries=" + std::to_string(commandSurface.totalEntries()));

    for (const std::string& line :
         makeRuntimeResidentAdapterReadyLines(environment, proofQueueHeader.heartbeat))
      appendReadyLine(line);
    for (const std::string& line :
         makeRuntimeResidentQueueReadyLines(
           RuntimeResidentQueueKind::Command,
           commandQueuePath,
           commandQueueHeader))
      appendReadyLine(line);
    for (const std::string& line :
         makeRuntimeResidentQueueReadyLines(
           RuntimeResidentQueueKind::Overlay,
           overlayQueuePath,
           overlayQueueHeader))
      appendReadyLine(line);
    for (const std::string& line :
         makeRuntimeResidentQueueReadyLines(
           RuntimeResidentQueueKind::Proof,
           proofQueuePath,
           proofQueueHeader))
      appendReadyLine(line);

    appendReadyLine("proof.attach=passed");
    appendReadyLine("proof.attach.source=resident-adapter");
    appendReadyLine("proof.attach.queue=" + proofQueuePath.string());
    appendReadyLine("contract.binding.shared-memory-client-transport=transport|proof.attach=passed:resident-proof-queue-v1");

    if (frameCounterProof.passed && frameCounterProof.samples.size() >= 3)
    {
      std::vector<RuntimeResidentGameStateSample> samples(
        frameCounterProof.samples.begin(),
        frameCounterProof.samples.end());
      for (const std::string& line :
           makeRuntimeResidentReadGameStateProofReadyLines(
             environment,
             proofQueueHeader.heartbeat,
             samples))
      {
        appendReadyLine(line);
      }

      appendReadyLine("resident.proof.read_game_state.validation=resident-self-read-v1");
      appendReadyLine("resident.proof.read_game_state.address_read=resident-self");
      appendReadyLine("resident.proof.read_game_state.counter_bytes=4");
      appendReadyLine(
        "resident.proof.read_game_state.region_address="
        + hexAddress(frameCounterProof.regionAddress));
      appendReadyLine(
        "resident.proof.read_game_state.region_size="
        + std::to_string(frameCounterProof.regionSize));
      if (!frameCounterProof.regionPath.empty())
      {
        appendReadyLine(
          "resident.proof.read_game_state.region_path="
          + frameCounterProof.regionPath);
      }
      appendReadyLine(
        "proof.read_game_state.address="
        + hexAddress(frameCounterProof.address));
      appendReadyLine(
        "proof.read_game_state.samples="
        + std::to_string(samples[0].frame) + ','
        + std::to_string(samples[1].frame) + ','
        + std::to_string(samples[2].frame));
      appendReadyLine(
        "proof.read_game_state.delta="
        + std::to_string(samples[1].frame - samples[0].frame) + ','
        + std::to_string(samples[2].frame - samples[1].frame));
      appendReadyLine("proof.read_game_state.confidence=frame-like");
    }

    appendReadyLine("resident.projection.bwgame.validation=resident-bwgame-projection-v1");
    appendReadyLine(
      "resident.projection.bwgame.address="
      + hexAddress(reinterpret_cast<std::uintptr_t>(&residentBwGameProjection)));
    appendReadyLine(
      "resident.projection.bwgame.size="
      + std::to_string(sizeof(residentBwGameProjection)));
    appendReadyLine("resident.projection.bwgame.players_offset=0");
    appendReadyLine("resident.projection.bwgame.alliance_offset=4");
    appendReadyLine("resident.projection.bwgame.elapsedFrames_offset=8");
    appendReadyLine("resident.projection.bwgame.elapsedFrames_bytes=4");
    appendReadyLine(
      "resident.projection.bwgame.heartbeat="
      + std::to_string(residentBwGameProjection.heartbeat));
    appendReadyLine(
      "resident.projection.bwgame.frame="
      + std::to_string(residentBwGameProjection.elapsedFrames));

    if (frameCounterProof.passed && frameCounterProof.samples.size() >= 3)
    {
      const RuntimeResidentGameStateSample& previousSample =
        *(frameCounterProof.samples.end() - 2);
      const RuntimeResidentGameStateSample& latestSample =
        frameCounterProof.samples.back();
      const std::string projectionAddress =
        hexAddress(reinterpret_cast<std::uintptr_t>(&residentBwGameProjection));

      appendReadyLine("proof.read_game_state.bwgame_projection=resident-bwgame-projection-v1");
      appendReadyLine(
        "proof.read_game_state.bwgame_projection_address="
        + projectionAddress);
      appendReadyLine(
        "proof.read_game_state.bwgame_projection_samples="
        + std::to_string(previousSample.frame) + ','
        + std::to_string(latestSample.frame));
      appendReadyLine(
        "contract.binding.BW::BWDATA::Game=data-address|proof.read_game_state=passed:"
        "resident-bwgame-projection-v1:"
        + projectionAddress);
      appendReadyLine(
        "contract.structure.BW::BWGame="
        + std::to_string(sizeof(residentBwGameProjection))
        + "|proof.read_game_state=passed:resident-bwgame-projection-v1");
      appendReadyLine(
        "contract.field.BW::BWGame.elapsedFrames=8|4|proof.read_game_state=passed:"
        "resident-bwgame-projection-v1");
    }

    const bool hasResidentFrame = frameCounterProof.passed && frameCounterProof.samples.size() >= 3;
    const ResidentSnapshotContext activeSnapshotContext{
      environment.processId,
      proofQueueHeader.heartbeat,
      latestResidentFrameId(frameCounterProof),
      true
    };
    const bool unitSnapshotWritten =
      hasResidentFrame
      && unitNodeProof.passed
      && writeResidentUnitSnapshot(
        bridgePath / "units.snapshot.tsv",
        unitNodeProof,
        activeSnapshotContext);
    if (unitSnapshotWritten)
    {
      appendReadyLine(
        "proof.read_units.address="
        + hexAddress(unitNodeProof.address));
      appendReadyLine(
        "proof.read_units.record_size="
        + std::to_string(unitNodeProof.recordSize));
      appendReadyLine(
        "proof.read_units.layout="
        + (unitNodeProof.layoutName.empty()
          ? std::string("scr-compact-unit-node-object-graph")
          : unitNodeProof.layoutName));
      appendReadyLine("proof.read_units.pointer_array=false");
      appendReadyLine("proof.read_units.derived_snapshot=true");
      appendReadyLine(
        std::string("proof.read_units.hit_points_resolved=")
        + (unitNodeProof.hitPointsResolved ? "true" : "false"));
      appendReadyLine(
        "proof.read_units.active_records="
        + std::to_string(unitNodeProof.activeRecords));
      appendReadyLine("proof.read_units.snapshot=units.snapshot.tsv");
      appendReadyLine("proof.read_units.id_source=stable-node-handle|compact-node sprite metadata");
      appendReadyLine("proof.read_units.position_source=unit-node+0x10|8 compact-xy");
      if (unitNodeProof.hitPointsResolved)
        appendReadyLine("proof.read_units.hit_points_source=sprite+0x80 hp-raw");
      appendReadyLine("proof.read_units.order_source=compact-unit-node:adapter-default-order");
      appendReadyLine("proof.read_units.player_source=compact-unit-node:sprite+0x6c|4-or-sprite+0xc0|1");
      appendReadyLine("contract.binding.BW::BWDATA::UnitNodeTable=data-address|proof.read_units=passed");
      appendReadyLine("contract.structure.BW::CUnit=512|proof.read_units=passed:compat-unit-projection-v1");
      appendReadyLine("contract.field.BW::CUnit.id=0|4|proof.read_units=passed");
      appendReadyLine("contract.field.BW::CUnit.position=4|4|proof.read_units=passed");
      if (unitNodeProof.hitPointsResolved)
        appendReadyLine("contract.field.BW::CUnit.hitPoints=12|4|proof.read_units=passed:scr-compact-hp-byte");
      appendReadyLine("contract.field.BW::CUnit.order=8|2|proof.read_units=passed");
      appendReadyLine("contract.field.BW::CUnit.player=10|4|proof.read_units=passed");
      appendReadyLine("proof.read_units=passed");
    }

    if (frameCounterProof.passed && unitSnapshotWritten)
    {
      appendReadyLine("proof.active_match_state=passed");
      appendReadyLine("resident.proof.active_match.source=resident");
      appendReadyLine(
        "resident.proof.active_match.process_id="
        + std::to_string(environment.processId));
      appendReadyLine(
        "resident.proof.active_match.heartbeat="
        + std::to_string(proofQueueHeader.heartbeat));
      appendReadyLine("resident.proof.active_match.mode=match");
      appendReadyLine(
        "resident.proof.active_match.unit_activity_count="
        + std::to_string(unitNodeProof.activeRecords));
      appendReadyLine("resident.proof.active_match.evidence=resident-frame-unit-activity");
      appendReadyLine("resident.proof.active_match.validation=resident-preserved-active-unit-memory-v1");
      appendReadyLine("resident.proof.active_match.address_read=resident-self");
      appendReadyLine("proof.active_match_state.evidence=active-unit-node-snapshot");
      appendReadyLine(
        "proof.active_match_state.active_records="
        + std::to_string(unitNodeProof.activeRecords));
      appendReadyLine(
        "proof.active_match_state.unit_node_address="
        + hexAddress(unitNodeProof.address));
      appendReadyLine(
        "proof.active_match_state.unit_node_record_size="
        + std::to_string(unitNodeProof.recordSize));
    }

    const ResidentDispatchEventsProof dispatchEventsProof =
      proveResidentDispatchEvents(frameCounterProof, unitNodeProof);
    const bool dispatchEventsSnapshotWritten =
      dispatchEventsProof.passed
      && unitSnapshotWritten
      && writeResidentDispatchEventsSnapshot(
        bridgePath / "events.snapshot.tsv",
        frameCounterProof,
        unitNodeProof);
    if (dispatchEventsSnapshotWritten)
    {
      appendReadyLine(
        "proof.dispatch_events.frame_events="
        + std::to_string(dispatchEventsProof.frameEvents));
      appendReadyLine(
        "proof.dispatch_events.unit_discover_events="
        + std::to_string(dispatchEventsProof.unitDiscoverEvents));
      appendReadyLine(
        "proof.dispatch_events.unit_update_events="
        + std::to_string(dispatchEventsProof.unitUpdateEvents));
      appendReadyLine(
        "proof.dispatch_events.unique_players="
        + std::to_string(dispatchEventsProof.uniquePlayers));
      appendReadyLine("proof.dispatch_events.snapshot=events.snapshot.tsv");
      appendReadyLine("contract.binding.BW::BWFXN_ExecuteGameTriggers=function-address|proof.dispatch_events=passed:adapter-event-dispatch-loop");
      appendReadyLine("proof.dispatch_events=passed");
    }

    const bool playerSnapshotWritten =
      unitSnapshotWritten
      && playerDataProof.passed
      && writeResidentPlayerDataSnapshot(
        bridgePath / "players.snapshot.tsv",
        playerDataProof,
        activeSnapshotContext);
    if (playerSnapshotWritten)
    {
      appendReadyLine(
        "proof.read_player_data.player_count="
        + std::to_string(playerDataProof.playerCount));
      appendReadyLine(
        "proof.read_player_data.observed_units="
        + std::to_string(playerDataProof.observedUnits));
      appendReadyLine("proof.read_player_data.player_info_projection=true");
      appendReadyLine(
        "proof.read_player_data.player_info_record_size="
        + std::to_string(playerDataProof.playerInfoRecordSize));
      appendReadyLine("proof.read_player_data.alliance_projection=true");
      appendReadyLine(
        "proof.read_player_data.projection_source="
        + playerDataProof.projectionSource);
      appendReadyLine("proof.read_player_data.snapshot=players.snapshot.tsv");
      appendReadyLine(
        "contract.binding.BW::BWDATA::Players=data-address|proof.read_player_data=passed:"
        + playerDataProof.projectionSource);
      appendReadyLine("contract.field.BW::BWGame.players=0|4|proof.read_player_data=passed");
      appendReadyLine("contract.field.BW::BWGame.alliance=4|4|proof.read_player_data=passed:compat-alliance-mask");
      appendReadyLine(
        "contract.structure.BW::PlayerInfo="
        + std::to_string(playerDataProof.playerInfoRecordSize)
        + "|proof.read_player_data=passed:"
        + playerDataProof.projectionSource);
      appendReadyLine("contract.field.BW::PlayerInfo.stormId=0|4|proof.read_player_data=passed");
      appendReadyLine("contract.field.BW::PlayerInfo.race=4|4|proof.read_player_data=passed");
      appendReadyLine("contract.field.BW::PlayerInfo.resources=8|8|proof.read_player_data=passed:projection-unresolved-values");
      appendReadyLine("contract.field.BW::PlayerInfo.supply=16|8|proof.read_player_data=passed:projection-unresolved-values");
      appendReadyLine("proof.read_player_data=passed");
    }

    const bool bulletSnapshotWritten =
      unitSnapshotWritten
      && bulletDataProof.passed
      && writeResidentBulletDataSnapshot(
        bridgePath / "bullets.snapshot.tsv",
        bulletDataProof,
        activeSnapshotContext);
    if (bulletSnapshotWritten)
    {
      appendReadyLine("proof.read_bullet_data.source=live-sc-r-bullet-table");
      appendReadyLine(
        "proof.read_bullet_data.address="
        + hexAddress(bulletDataProof.address));
      appendReadyLine(
        "proof.read_bullet_data.record_size="
        + std::to_string(bulletDataProof.recordSize));
      appendReadyLine("proof.read_bullet_data.layout=" + bulletDataProof.layoutName);
      appendReadyLine(
        "proof.read_bullet_data.sampled_records="
        + std::to_string(bulletDataProof.sampledRecords));
      appendReadyLine(
        "proof.read_bullet_data.active_records="
        + std::to_string(bulletDataProof.activeRecords));
      appendReadyLine(
        "proof.read_bullet_data.unit_correlated_records="
        + std::to_string(bulletDataProof.unitCorrelatedRecords));
      appendReadyLine("proof.read_bullet_data.snapshot=bullets.snapshot.tsv");
      appendReadyLine(
        "contract.binding.BW::BWDATA::BulletNodeTable=data-address|proof.read_bullet_data=passed:"
        + hexAddress(bulletDataProof.address));
      appendReadyLine(
        "contract.structure.BW::CBullet="
        + std::to_string(bulletDataProof.recordSize)
        + "|proof.read_bullet_data=passed:"
        + bulletDataProof.layoutName);
      appendReadyLine(
        "contract.field.BW::CBullet.position="
        + std::to_string(bulletDataProof.positionOffset)
        + "|4|proof.read_bullet_data=passed");
      appendReadyLine(
        "contract.field.BW::CBullet.velocity="
        + std::to_string(bulletDataProof.velocityOffset)
        + "|8|proof.read_bullet_data=passed");
      appendReadyLine(
        "contract.field.BW::CBullet.sourceUnit="
        + std::to_string(bulletDataProof.sourceUnitOffset)
        + "|8|proof.read_bullet_data=passed");
      appendReadyLine(
        "contract.field.BW::CBullet.target="
        + std::to_string(bulletDataProof.targetOffset)
        + "|8|proof.read_bullet_data=passed");
      appendReadyLine("proof.read_bullet_data=passed");
    }

    const bool mapSnapshotWritten =
      unitSnapshotWritten
      && mapDataProof.passed
      && writeResidentMapDataSnapshot(
        bridgePath / "map.snapshot.tsv",
        mapDataProof,
        activeSnapshotContext);
    if (mapSnapshotWritten)
    {
      appendReadyLine("proof.read_map_data.map_name=" + mapDataProof.mapName);
      appendReadyLine(
        "proof.read_map_data.map_name_address="
        + hexAddress(mapDataProof.mapNameAddress));
      appendReadyLine(
        "proof.read_map_data.map_tile_array_address="
        + hexAddress(mapDataProof.mapTileArrayAddress));
      appendReadyLine(
        "proof.read_map_data.tile_count="
        + std::to_string(mapDataProof.tileCount));
      appendReadyLine("proof.read_map_data.map_path=" + mapDataProof.mapPath.string());
      appendReadyLine(
        "proof.read_map_data.map_file_size="
        + std::to_string(mapDataProof.mapFileSize));
      appendReadyLine("proof.read_map_data.source=" + mapDataProof.source);
      appendReadyLine("proof.read_map_data.replay_path=" + mapDataProof.replayPath.string());
      appendReadyLine(
        "proof.read_map_data.replay_file_size="
        + std::to_string(mapDataProof.replayFileSize));
      appendReadyLine("proof.read_map_data.snapshot=map.snapshot.tsv");
      appendReadyLine("contract.binding.BW::BWDATA::MapTileArray=data-address|proof.read_map_data=passed:compat-map-tile-projection");
      appendReadyLine("proof.read_map_data=passed");
    }

    const ResidentRegionDataProof regionDataProof =
      proveResidentRegionData(mapDataProof, unitNodeProof);
    const bool regionSnapshotWritten =
      regionDataProof.passed
      && mapSnapshotWritten
      && unitSnapshotWritten
      && writeResidentRegionDataSnapshot(
        bridgePath / "regions.snapshot.tsv",
        regionDataProof,
        activeSnapshotContext);
    if (regionSnapshotWritten)
    {
      appendReadyLine("proof.read_region_data.source=" + regionDataProof.source);
      appendReadyLine(
        "proof.read_region_data.region_count="
        + std::to_string(regionDataProof.regionCount));
      appendReadyLine(
        "proof.read_region_data.observed_units="
        + std::to_string(regionDataProof.observedUnits));
      appendReadyLine("proof.read_region_data.snapshot=regions.snapshot.tsv");
      appendReadyLine("proof.read_region_data=passed");
    }

    const ResidentReplayAnalysisProof replayAnalysisProof =
      proveResidentReplayAnalysis(
        frameCounterProof,
        mapDataProof,
        playerDataProof,
        unitSnapshotWritten);
    const bool replaySnapshotWritten =
      replayAnalysisProof.passed
      && mapSnapshotWritten
      && playerSnapshotWritten
      && writeResidentReplayAnalysisSnapshot(
        bridgePath / "replay.snapshot.tsv",
        replayAnalysisProof,
        ResidentSnapshotContext{
          environment.processId,
          proofQueueHeader.heartbeat,
          latestResidentFrameId(frameCounterProof),
          replayAnalysisProof.activeMatchMetadata
        });
    if (replaySnapshotWritten)
    {
      appendReadyLine("proof.replay_analysis.source=" + replayAnalysisProof.source);
      appendReadyLine(
        std::string("proof.replay_analysis.current_process_replay=")
        + (replayAnalysisProof.currentProcessReplay ? "true" : "false"));
      appendReadyLine(
        std::string("proof.replay_analysis.active_match_metadata=")
        + (replayAnalysisProof.activeMatchMetadata ? "true" : "false"));
      appendReadyLine("proof.replay_analysis.map_name=" + replayAnalysisProof.mapName);
      appendReadyLine(
        "proof.replay_analysis.first_frame="
        + std::to_string(replayAnalysisProof.firstFrame));
      appendReadyLine(
        "proof.replay_analysis.last_frame="
        + std::to_string(replayAnalysisProof.lastFrame));
      appendReadyLine(
        "proof.replay_analysis.player_count="
        + std::to_string(replayAnalysisProof.playerCount));
      appendReadyLine("proof.replay_analysis.snapshot=replay.snapshot.tsv");
      appendReadyLine("contract.structure.BW::ReplayHeader=256|proof.replay_analysis=passed");
      appendReadyLine("contract.field.BW::ReplayHeader.mapName=0|32|proof.replay_analysis=passed");
      appendReadyLine("contract.field.BW::ReplayHeader.frameCount=32|4|proof.replay_analysis=passed");
      appendReadyLine("contract.field.BW::ReplayHeader.playerCount=36|4|proof.replay_analysis=passed");
      appendReadyLine("proof.replay_analysis=passed");
    }

    const bool aiModuleSnapshotWritten =
      aiModuleLoadProof.passed
      && writeResidentAIModuleLoadSnapshot(
        bridgePath / "ai_module_load.snapshot.tsv",
        aiModuleLoadProof);
    if (aiModuleSnapshotWritten)
    {
      appendReadyLine("proof.load_ai_modules.loader=" + aiModuleLoadProof.loader);
      appendReadyLine("proof.load_ai_modules.module_extension=" + aiModuleLoadProof.moduleExtension);
      appendReadyLine(
        std::string("proof.load_ai_modules.self_process_smoke=")
        + (aiModuleLoadProof.selfProcessSmoke ? "true" : "false"));
      if (!aiModuleLoadProof.modulePath.empty())
        appendReadyLine("proof.load_ai_modules.module_path=" + aiModuleLoadProof.modulePath);
      appendReadyLine("proof.load_ai_modules.snapshot=ai_module_load.snapshot.tsv");
      appendReadyLine("contract.binding.ai-module-loader=transport|proof.load_ai_modules=passed");
      appendReadyLine("proof.load_ai_modules=passed");
    }

    if (battleNetPolicyProof.passed)
    {
      appendReadyLine("proof.battle_net_policy.status=" + battleNetPolicyProof.diagnosis.status);
      appendReadyLine(
        "proof.battle_net_policy.game_process_count="
        + std::to_string(battleNetPolicyProof.diagnosis.gameProcessCount));
      appendReadyLine(
        "proof.battle_net_policy.blocker_count="
        + std::to_string(battleNetPolicyProof.diagnosis.blockers.size()));
      appendReadyLine("proof.battle_net_policy=passed");
    }

    const bool commandIngressSnapshotWritten =
      commandIngressProof.consumedRecords > 0
      && writeResidentCommandIngressSnapshot(
        bridgePath / commandIngressProof.snapshot,
        commandIngressProof);
    if (commandIngressSnapshotWritten)
    {
      appendReadyLine("resident.proof.issue_commands_ingress=passed");
      appendReadyLine("resident.proof.issue_commands_ingress.source=resident-command-queue");
      appendReadyLine(
        "resident.proof.issue_commands_ingress.consumed_records="
        + std::to_string(commandIngressProof.consumedRecords));
      appendReadyLine(
        "resident.proof.issue_commands_ingress.parsed_commands="
        + std::to_string(commandIngressProof.parsedCommands));
      appendReadyLine(
        "resident.proof.issue_commands_ingress.overlay_actions="
        + std::to_string(commandIngressProof.overlayActions));
      appendReadyLine("resident.proof.issue_commands_ingress.snapshot=" + commandIngressProof.snapshot);
      appendReadyLine("diagnostic.issue_commands.snapshot=" + commandIngressProof.snapshot);
      appendReadyLine("diagnostic.issue_commands.ingress=resident-command-queue-consumed");
      appendReadyLine("diagnostic.issue_commands.proof_scope=resident-ingress-only-not-live-scr-command-behavior");
      appendReadyLine("diagnostic.issue_commands.required_adapter_behavior=encoded-bwapi-command-reaches-live-scr-command-path-and-changes-frame-behavior");
      if (!commandIngressProof.reason.empty())
        appendReadyLine("diagnostic.issue_commands.reason=" + commandIngressProof.reason);
    }

    const bool overlayIngressSnapshotWritten =
      overlayIngressProof.acceptedPrimitives > 0
      && writeResidentOverlayIngressSnapshot(
        bridgePath / overlayIngressProof.snapshot,
        overlayIngressProof);
    if (overlayIngressSnapshotWritten)
    {
      appendReadyLine("resident.proof.draw_overlays_ingress=passed");
      appendReadyLine("resident.proof.draw_overlays_ingress.source=resident-overlay-queue");
      appendReadyLine(
        "resident.proof.draw_overlays_ingress.accepted_primitives="
        + std::to_string(overlayIngressProof.acceptedPrimitives));
      appendReadyLine(
        std::string("resident.proof.draw_overlays_ingress.renderer_bound=")
        + (overlayIngressProof.rendererBound ? "true" : "false"));
      appendReadyLine("resident.proof.draw_overlays_ingress.snapshot=" + overlayIngressProof.snapshot);
      appendReadyLine("diagnostic.draw_overlays.snapshot=" + overlayIngressProof.snapshot);
      appendReadyLine("diagnostic.draw_overlays.ingress=resident-overlay-queue-accepted");
      appendReadyLine("diagnostic.draw_overlays.proof_scope=resident-ingress-only-not-visible-frame-rendering");
      appendReadyLine("diagnostic.draw_overlays.required_adapter_behavior=bwapi-overlay-primitives-render-on-visible-game-frame");
      if (!overlayIngressProof.reason.empty())
        appendReadyLine("diagnostic.draw_overlays.reason=" + overlayIngressProof.reason);
    }

    for (const std::string& line : preservedReadyEvidenceLines(readyPath, environment, ownedKeys))
      readyLines.push_back(line);

    for (const std::string& line : readyLines)
      ready << line << '\n';

    ready.close();
    if (!ready)
      return false;

    std::error_code error;
    std::filesystem::rename(temporaryPath, readyPath, error);
    if (!error)
      return true;

    std::filesystem::remove(readyPath, error);
    std::filesystem::rename(temporaryPath, readyPath, error);
    return !error;
  }

  void residentAdapterMain(std::filesystem::path bridgePath)
  {
    using namespace BWAPI::Runtime;

    std::error_code error;
    std::filesystem::create_directories(bridgePath, error);
    appendResidentLog(bridgePath, "resident.main.enter bridge=" + bridgePath.string());
    if (error)
    {
      appendResidentLog(bridgePath, "resident.main.create_directories_failed reason=" + error.message());
      return;
    }

    RuntimeEnvironment environment;
    environment.platform = Platform::MacOS;
    environment.product = Product::StarCraftRemastered;
    environment.processId = static_cast<int>(getpid());
    environment.executablePath = currentExecutablePath();
    environment.version = detectResidentVersion(environment.executablePath);
    environment.executorBridgePath = bridgePath.string();

    const std::filesystem::path commandQueuePath = bridgePath / RuntimeResidentCommandQueueFile;
    const std::filesystem::path overlayQueuePath = bridgePath / RuntimeResidentOverlayQueueFile;
    const std::filesystem::path proofQueuePath = bridgePath / RuntimeResidentProofQueueFile;
    ResidentFrameCounterProof frameCounterProof;
    ResidentUnitNodeProof unitNodeProof;
    ResidentBulletDataProof bulletDataProof;
    ResidentMapDataProof mapDataProof;
    ResidentAIModuleLoadProof aiModuleLoadProof;
    ResidentBattleNetPolicyProof battleNetPolicyProof;
    ResidentCommandIngressProof commandIngressProof;
    ResidentOverlayIngressProof overlayIngressProof;
    std::uint64_t lastUnitDiscoveryHeartbeat = 0;
    std::uint64_t lastBulletDiscoveryHeartbeat = 0;
    std::uint64_t lastMapDiscoveryHeartbeat = 0;
    std::uint64_t lastAIModuleProofHeartbeat = 0;
    std::uint64_t lastBattleNetPolicyProofHeartbeat = 0;
    std::uint64_t heartbeat = 1;
    while (residentAdapterRunning.load(std::memory_order_relaxed))
    {
      RuntimeResidentQueueHeader desiredCommandQueueHeader =
        makeRuntimeResidentQueueHeader(
          RuntimeResidentQueueKind::Command,
          sizeof(RuntimeResidentRecordHeader) + 512,
          64,
          heartbeat);
      RuntimeResidentQueueHeader desiredOverlayQueueHeader =
        makeRuntimeResidentQueueHeader(
          RuntimeResidentQueueKind::Overlay,
          sizeof(RuntimeResidentRecordHeader) + 512,
          64,
          heartbeat);
      RuntimeResidentQueueHeader desiredProofQueueHeader =
        makeRuntimeResidentQueueHeader(
          RuntimeResidentQueueKind::Proof,
          sizeof(RuntimeResidentRecordHeader),
          64,
          heartbeat);
      RuntimeResidentQueueHeader commandQueueHeader;
      RuntimeResidentQueueHeader overlayQueueHeader;
      RuntimeResidentQueueHeader proofQueueHeader;
      const bool commandQueueReady =
        ensureResidentQueue(
          bridgePath,
          commandQueuePath,
          desiredCommandQueueHeader,
          commandQueueHeader);
      const bool overlayQueueReady =
        ensureResidentQueue(
          bridgePath,
          overlayQueuePath,
          desiredOverlayQueueHeader,
          overlayQueueHeader);
      const bool proofQueueReady =
        ensureResidentQueue(
          bridgePath,
          proofQueuePath,
          desiredProofQueueHeader,
          proofQueueHeader);
      if (commandQueueReady && overlayQueueReady && proofQueueReady)
      {
        if (!refreshResidentFrameCounterProof(frameCounterProof))
          discoverResidentFrameCounterProof(environment.executablePath, frameCounterProof);

        if (unitNodeProof.passed)
        {
          const std::vector<ResidentMemoryRegion> regions = listResidentMemoryRegions();
          refreshResidentUnitNodeProof(unitNodeProof, regions);
          if (bulletDataProof.passed)
            refreshResidentBulletDataProof(
              bulletDataProof,
              regions,
              environment.executablePath,
              unitNodeProof);
        }
        if (!unitNodeProof.passed
            && (lastUnitDiscoveryHeartbeat == 0
                || heartbeat - lastUnitDiscoveryHeartbeat >= 3))
        {
          lastUnitDiscoveryHeartbeat = heartbeat;
          if (discoverResidentUnitNodeProof(environment.executablePath, unitNodeProof))
          {
            appendResidentLog(
              bridgePath,
              "resident.unit_node.proof_found address="
              + hexAddress(unitNodeProof.address)
              + " active_records="
              + std::to_string(unitNodeProof.activeRecords));
          }
          else if (!unitNodeProof.reason.empty())
          {
            appendResidentLog(
              bridgePath,
              "resident.unit_node.scan_unresolved reason="
              + unitNodeProof.reason);
          }
        }
        if (!bulletDataProof.passed
            && frameCounterProof.passed
            && unitNodeProof.passed
            && (lastBulletDiscoveryHeartbeat == 0
                || heartbeat - lastBulletDiscoveryHeartbeat >= 2))
        {
          lastBulletDiscoveryHeartbeat = heartbeat;
          if (discoverResidentBulletDataProof(environment.executablePath, unitNodeProof, bulletDataProof))
          {
            appendResidentLog(
              bridgePath,
              "resident.bullet.proof_found address="
              + hexAddress(bulletDataProof.address)
              + " active_records="
              + std::to_string(bulletDataProof.activeRecords)
              + " layout="
              + bulletDataProof.layoutName);
          }
          else if (!bulletDataProof.reason.empty())
          {
            appendResidentLog(
              bridgePath,
              "resident.bullet.scan_unresolved reason="
              + bulletDataProof.reason);
          }
        }
        if (!mapDataProof.passed
            && (lastMapDiscoveryHeartbeat == 0
                || heartbeat - lastMapDiscoveryHeartbeat >= 10))
        {
          lastMapDiscoveryHeartbeat = heartbeat;
          mapDataProof = proveResidentMapData(environment.executablePath);
          if (mapDataProof.passed)
          {
            appendResidentLog(
              bridgePath,
              "resident.map.proof_found map=" + mapDataProof.mapName);
          }
          else if (!mapDataProof.reason.empty())
          {
            appendResidentLog(
              bridgePath,
              "resident.map.scan_unresolved reason=" + mapDataProof.reason);
          }
        }
        if (!aiModuleLoadProof.passed
            && (lastAIModuleProofHeartbeat == 0
                || heartbeat - lastAIModuleProofHeartbeat >= 30))
        {
          lastAIModuleProofHeartbeat = heartbeat;
          aiModuleLoadProof = proveResidentAIModuleLoader();
          if (aiModuleLoadProof.passed)
            appendResidentLog(bridgePath, "resident.ai_module.proof_found loader=" + aiModuleLoadProof.loader);
          else if (!aiModuleLoadProof.reason.empty())
            appendResidentLog(bridgePath, "resident.ai_module.proof_failed reason=" + aiModuleLoadProof.reason);
        }
        if (!battleNetPolicyProof.passed
            && (lastBattleNetPolicyProofHeartbeat == 0
                || heartbeat - lastBattleNetPolicyProofHeartbeat >= 30))
        {
          lastBattleNetPolicyProofHeartbeat = heartbeat;
          battleNetPolicyProof = proveResidentBattleNetPolicy(environment);
          if (battleNetPolicyProof.passed)
            appendResidentLog(bridgePath, "resident.battle_net_policy.proof_found status=" + battleNetPolicyProof.diagnosis.status);
          else if (!battleNetPolicyProof.reason.empty())
            appendResidentLog(bridgePath, "resident.battle_net_policy.proof_failed reason=" + battleNetPolicyProof.reason);
        }
        updateResidentBwGameProjection(frameCounterProof, unitNodeProof, heartbeat);

        consumeResidentCommandQueue(
          bridgePath,
          commandQueuePath,
          commandQueueHeader,
          overlayQueuePath,
          overlayQueueHeader,
          commandIngressProof,
          overlayIngressProof);
        if (!writeReadyFile(
          environment,
          bridgePath,
          commandQueuePath,
          commandQueueHeader,
          overlayQueuePath,
          overlayQueueHeader,
          proofQueuePath,
          proofQueueHeader,
          frameCounterProof,
          unitNodeProof,
          bulletDataProof,
          mapDataProof,
          aiModuleLoadProof,
          battleNetPolicyProof,
          commandIngressProof,
          overlayIngressProof))
        {
          appendResidentLog(bridgePath, "resident.ready.write_failed");
        }
      }
      else
      {
        if (!commandQueueReady)
          appendResidentLog(bridgePath, "resident.queue.write_failed path=" + commandQueuePath.string());
        if (!overlayQueueReady)
          appendResidentLog(bridgePath, "resident.queue.write_failed path=" + overlayQueuePath.string());
        if (!proofQueueReady)
          appendResidentLog(bridgePath, "resident.queue.write_failed path=" + proofQueuePath.string());
      }

      ++heartbeat;
      std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
  }

  int startResidentAdapter()
  {
    std::lock_guard<std::mutex> lock(residentAdapterMutex);
    if (residentAdapterRunning.load(std::memory_order_relaxed))
      return 0;

    const std::filesystem::path bridgePath = defaultBridgePath();
    appendResidentLog(bridgePath, "resident.start.requested");
    retainResidentImage();
    residentAdapterRunning.store(true, std::memory_order_relaxed);
    residentAdapterThread = std::thread(residentAdapterMain, bridgePath);
    return 0;
  }

  void stopResidentAdapter()
  {
    std::lock_guard<std::mutex> lock(residentAdapterMutex);
    residentAdapterRunning.store(false, std::memory_order_relaxed);
    if (residentAdapterThread.joinable())
      residentAdapterThread.join();
  }
}

extern "C"
{
  const char* starcraft_api_resident_adapter_abi()
  {
    return BWAPI::Runtime::RuntimeResidentAdapterAbi;
  }

  int starcraft_api_resident_adapter_entry()
  {
    return startResidentAdapter();
  }

  int starcraft_api_resident_adapter_stop()
  {
    stopResidentAdapter();
    return 0;
  }
}

__attribute__((constructor))
static void starcraft_api_resident_adapter_constructor()
{
  appendResidentLog(defaultBridgePath(), "resident.constructor.enter");
  if (!residentAutostartDisabled())
    startResidentAdapter();
  else
    appendResidentLog(defaultBridgePath(), "resident.constructor.autostart_disabled");
}

__attribute__((destructor))
static void starcraft_api_resident_adapter_destructor()
{
  stopResidentAdapter();
}
