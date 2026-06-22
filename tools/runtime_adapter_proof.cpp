#include <BWAPI/Runtime/RuntimeBackend.h>
#include <BWAPI/Runtime/RuntimeExecutor.h>
#include <BWAPI/Runtime/RuntimeInstallation.h>
#include <BWAPI/Runtime/RuntimeProcess.h>
#include <BWAPI/Runtime/RuntimeProcessMemory.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
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
      << "  --prove-dispatch-events  prove BWAPI event dispatch from live frame/unit snapshots\n"
      << "  --prove-read-map-data    prove live map metadata by matching the active map to an installed map file\n"
      << "  --prove-read-player-data prove live player ids from unit snapshots\n"
      << "  --prove-replay-analysis  prove replay-compatible map/frame metadata from live state\n"
      << "  --prove-battle-net-policy\n"
      << "                           prove Battle.net launch/attach policy preflight has no blockers\n"
      << "  --discover-command-queue\n"
      << "                           scan live memory for command-queue-like vector candidates without claiming command proof\n"
      << "  --self-command-queue-fixture\n"
      << "                           allocate a self-test command queue candidate before --discover-command-queue\n"
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
      << "  --unit-scan-include-image-regions\n"
      << "                           include regions mapped from the target executable in unit scans\n"
      << "  --unit-candidate-address <address>\n"
      << "                           validate an explicit CUnit array candidate before broad scans\n"
      << "  --unit-best-dump-out <path>\n"
      << "                           dump bytes from the best CUnit candidate for offline analysis\n"
      << "  --state-scan-diagnostics\n"
      << "                           print live state-counter scan counters on success/failure\n"
      << "  --active-match-wait-ms <ms>\n"
      << "                           poll live memory until an active match is proven or timeout expires\n"
      << "  --active-match-poll-ms <ms>\n"
      << "                           delay between active-match polling attempts (default: 1000)\n"
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

  bool parseAddress(const std::string& value, std::uintptr_t& output)
  {
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value.c_str(), &end, 0);
    if (end == value.c_str() || *end != '\0' || parsed == 0)
      return false;
    output = static_cast<std::uintptr_t>(parsed);
    return static_cast<unsigned long long>(output) == parsed;
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
    bool pointerArray = false;
    bool derivedSnapshot = false;
    bool hitPointsResolved = true;
    std::string layoutName;
    std::string reason;
  };

  struct RemasteredUnitSnapshotRecord
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
  };

  struct LiveUnitNodeProof
  {
    bool passed = false;
    std::uintptr_t address = 0;
    std::uintptr_t vectorAddress = 0;
    std::size_t recordSize = 0;
    std::size_t sampledRecords = 0;
    std::size_t activeRecords = 0;
    std::vector<RemasteredUnitSnapshotRecord> records;
    std::string reason;
  };

  struct DispatchEventsProof
  {
    bool passed = false;
    std::size_t frameEvents = 0;
    std::size_t unitDiscoverEvents = 0;
    std::size_t unitUpdateEvents = 0;
    std::size_t uniquePlayers = 0;
    std::string reason;
  };

  struct MapDataProof
  {
    bool passed = false;
    std::uintptr_t mapNameAddress = 0;
    std::string mapName;
    std::string mapPath;
    std::string source;
    std::string replayPath;
    std::uintmax_t mapFileSize = 0;
    std::uintmax_t replayFileSize = 0;
    std::string reason;
  };

  struct PlayerSnapshotRecord
  {
    int player = -1;
    std::size_t unitCount = 0;
  };

  struct PlayerDataProof
  {
    bool passed = false;
    std::size_t playerCount = 0;
    std::size_t observedUnits = 0;
    std::vector<PlayerSnapshotRecord> players;
    std::string reason;
  };

  struct ReplayAnalysisProof
  {
    bool passed = false;
    std::string mapName;
    std::size_t playerCount = 0;
    std::uint32_t firstFrame = 0;
    std::uint32_t lastFrame = 0;
    std::string reason;
  };

  struct CommandQueueCandidate
  {
    std::uintptr_t vectorAddress = 0;
    std::uintptr_t bufferBegin = 0;
    std::uintptr_t bufferEnd = 0;
    std::uintptr_t bufferCapacity = 0;
    std::size_t usedBytes = 0;
    std::size_t capacityBytes = 0;
    int score = 0;
    std::string regionPath;
  };

  struct CommandQueueDiscoveryProof
  {
    bool ready = false;
    std::size_t scannedRegions = 0;
    std::size_t scannedBytes = 0;
    std::vector<CommandQueueCandidate> candidates;
    std::string reason;
  };

  struct UnitScanDiagnostics
  {
    std::size_t readableWritableRegions = 0;
    std::size_t readableOnlyRegions = 0;
    std::size_t scannedReadableOnlyRegions = 0;
    std::size_t executableReadableRegions = 0;
    std::size_t imageMappedRegions = 0;
    std::size_t skippedImageMappedRegions = 0;
    std::size_t scannedRegions = 0;
    std::size_t scannedBytes = 0;
    std::size_t vectorCandidates = 0;
    std::size_t vectorDuplicateBegins = 0;
    std::size_t vectorRejectedTargetRegions = 0;
    std::size_t pointerArrayCandidates = 0;
    std::size_t pointerArraysScored = 0;
    std::size_t pointerArrayReadablePointerHits = 0;
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

  struct StateScanDiagnostics
  {
    std::size_t readableWritableRegions = 0;
    std::size_t skippedNonReadableRegions = 0;
    std::size_t skippedNonWritableRegions = 0;
    std::size_t scannedRegions = 0;
    std::size_t scannedBytes = 0;
    std::size_t candidateCounters = 0;
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

  struct SelfCommandQueueFixture
  {
    std::array<unsigned char, 4096> buffer;
    std::uintptr_t begin = 0;
    std::uintptr_t end = 0;
    std::uintptr_t capacity = 0;
  };

  LiveUnitsProof failedUnitsProof(std::string reason)
  {
    LiveUnitsProof proof;
    proof.reason = std::move(reason);
    return proof;
  }

  LiveUnitNodeProof failedUnitNodeProof(std::string reason)
  {
    LiveUnitNodeProof proof;
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

  std::string hexAddress(std::uintptr_t address)
  {
    std::ostringstream output;
    output << "0x" << std::hex << address;
    return output.str();
  }

  std::string lowerCase(std::string value)
  {
    std::transform(
      value.begin(),
      value.end(),
      value.begin(),
      [](unsigned char ch)
      {
        return static_cast<char>(std::tolower(ch));
      });
    return value;
  }

  bool asciiPrintable(unsigned char ch)
  {
    return ch >= 0x20 && ch <= 0x7e;
  }

  bool mapFilenameCandidate(const std::string& value)
  {
    if (value.size() < 5 || value.size() > 128)
      return false;
    const std::string lower = lowerCase(value);
    const bool hasMapExtension =
      (lower.size() >= 4 && lower.compare(lower.size() - 4, 4, ".scm") == 0)
      || (lower.size() >= 4 && lower.compare(lower.size() - 4, 4, ".scx") == 0);
    if (!hasMapExtension)
      return false;
    return value.find('/') == std::string::npos
      && value.find('\\') == std::string::npos;
  }

  std::string basenameFromMapPathCandidate(const std::string& value)
  {
    if (value.size() < 5 || value.size() > 512)
      return {};

    const std::string lower = lowerCase(value);
    std::size_t extension = lower.find(".scx");
    if (extension == std::string::npos)
      extension = lower.find(".scm");
    if (extension == std::string::npos)
      return {};

    const std::size_t end = extension + 4;
    std::size_t begin = value.find_last_of("/\\", extension);
    begin = begin == std::string::npos ? 0 : begin + 1;
    if (begin >= end)
      return {};

    const std::string basename = value.substr(begin, end - begin);
    return mapFilenameCandidate(basename) ? basename : std::string();
  }

  std::filesystem::path existingMapPathCandidate(const std::string& value)
  {
    const std::string lower = lowerCase(value);
    std::size_t extension = lower.find(".scx");
    if (extension == std::string::npos)
      extension = lower.find(".scm");
    if (extension == std::string::npos)
      return {};

    const std::string candidate = value.substr(0, extension + 4);
    if (candidate.find('/') == std::string::npos && candidate.find('\\') == std::string::npos)
      return {};

    std::error_code error;
    std::filesystem::path path(candidate);
    if (std::filesystem::is_regular_file(path, error) && !error)
      return path;
    return {};
  }

  std::string extractNullTerminatedAsciiString(
    const std::vector<unsigned char>& bytes,
    std::size_t offset,
    std::size_t maxLength)
  {
    std::string value;
    for (std::size_t i = offset; i < bytes.size() && value.size() < maxLength; ++i)
    {
      const unsigned char ch = bytes[i];
      if (ch == 0)
        break;
      if (!asciiPrintable(ch))
        return {};
      value.push_back(static_cast<char>(ch));
    }
    return value;
  }

  std::string extractNullTerminatedUtf16String(
    const std::vector<unsigned char>& bytes,
    std::size_t offset,
    std::size_t maxLength,
    bool bigEndian)
  {
    std::string value;
    for (std::size_t i = offset; i + 1 < bytes.size() && value.size() < maxLength; i += 2)
    {
      const unsigned char first = bytes[i];
      const unsigned char second = bytes[i + 1];
      const unsigned char high = bigEndian ? first : second;
      const unsigned char low = bigEndian ? second : first;
      if (high == 0 && low == 0)
        break;
      if (high != 0 || !asciiPrintable(low))
        return {};
      value.push_back(static_cast<char>(low));
    }
    return value;
  }

  bool looksLikeUtf16AsciiAt(
    const std::vector<unsigned char>& bytes,
    std::size_t offset,
    bool bigEndian)
  {
    if (offset + 3 >= bytes.size())
      return false;
    const unsigned char firstHigh = bigEndian ? bytes[offset] : bytes[offset + 1];
    const unsigned char firstLow = bigEndian ? bytes[offset + 1] : bytes[offset];
    const unsigned char secondHigh = bigEndian ? bytes[offset + 2] : bytes[offset + 3];
    const unsigned char secondLow = bigEndian ? bytes[offset + 3] : bytes[offset + 2];
    return firstHigh == 0
      && secondHigh == 0
      && asciiPrintable(firstLow)
      && asciiPrintable(secondLow);
  }

  std::filesystem::path findMapFileByName(
    const std::string& installRoot,
    const std::string& mapName)
  {
    if (installRoot.empty() || mapName.empty())
      return {};

    const std::string target = lowerCase(mapName);
    const std::filesystem::path mapsRoot = std::filesystem::path(installRoot) / "Maps";
    std::error_code error;
    if (!std::filesystem::is_directory(mapsRoot, error) || error)
      return {};

    std::filesystem::recursive_directory_iterator it(
      mapsRoot,
      std::filesystem::directory_options::skip_permission_denied,
      error);
    const std::filesystem::recursive_directory_iterator end;
    while (!error && it != end)
    {
      const std::filesystem::directory_entry& entry = *it;
      if (entry.is_regular_file(error)
          && lowerCase(entry.path().filename().string()) == target)
        return entry.path();
      it.increment(error);
    }
    return {};
  }

  std::string trimWhitespace(std::string value)
  {
    const std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
      return {};
    const std::size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
  }

  std::string replayAutosaveMapStem(const std::filesystem::path& path)
  {
    std::string stem = path.stem().string();
    const std::size_t comma = stem.find(',');
    if (comma != std::string::npos && comma + 1 < stem.size())
      stem = stem.substr(comma + 1);
    else if (lowerCase(path.filename().string()) == "lastreplay.rep")
      return {};
    return trimWhitespace(stem);
  }

  bool fileTimeClose(
    const std::filesystem::file_time_type& lhs,
    const std::filesystem::file_time_type& rhs,
    std::chrono::seconds tolerance)
  {
    const auto delta = lhs > rhs ? lhs - rhs : rhs - lhs;
    return delta <= tolerance;
  }

  std::vector<std::filesystem::path> replayRootCandidates(const std::string& installRoot)
  {
    std::vector<std::filesystem::path> roots;
    const auto addRoot =
      [&](const std::filesystem::path& root)
      {
        if (!root.empty())
          roots.push_back(root);
      };

    if (const char* explicitRoot = std::getenv("STARCRAFT_API_REPLAY_DIR"))
      addRoot(explicitRoot);

    if (!installRoot.empty())
      addRoot(std::filesystem::path(installRoot) / "Maps" / "Replays");

    if (const char* homeValue = std::getenv("HOME"))
    {
      const std::filesystem::path home(homeValue);
      addRoot(home / "Library" / "Application Support" / "Blizzard" / "StarCraft" / "Maps" / "Replays");
      addRoot(home / "Documents" / "StarCraft" / "Maps" / "Replays");
      addRoot(home / ".local" / "share" / "Blizzard" / "StarCraft" / "Maps" / "Replays");
    }

    if (const char* appData = std::getenv("APPDATA"))
      addRoot(std::filesystem::path(appData) / "Blizzard" / "StarCraft" / "Maps" / "Replays");
    if (const char* userProfile = std::getenv("USERPROFILE"))
      addRoot(std::filesystem::path(userProfile) / "Documents" / "StarCraft" / "Maps" / "Replays");
    if (const char* xdgData = std::getenv("XDG_DATA_HOME"))
      addRoot(std::filesystem::path(xdgData) / "Blizzard" / "StarCraft" / "Maps" / "Replays");

    std::vector<std::filesystem::path> uniqueRoots;
    for (const std::filesystem::path& root : roots)
    {
      std::error_code error;
      const std::filesystem::path normalized = std::filesystem::weakly_canonical(root, error);
      const std::filesystem::path comparable = error ? root.lexically_normal() : normalized.lexically_normal();
      bool duplicate = false;
      for (const std::filesystem::path& existing : uniqueRoots)
      {
        if (existing == comparable)
        {
          duplicate = true;
          break;
        }
      }
      if (!duplicate)
        uniqueRoots.push_back(comparable);
    }
    return uniqueRoots;
  }

  std::filesystem::path findInstalledMapForReplayStem(
    const std::string& installRoot,
    const std::string& mapStem)
  {
    if (installRoot.empty() || mapStem.empty())
      return {};

    for (const std::string& candidate : {
           mapStem,
           mapStem + ".scx",
           mapStem + ".scm",
           mapStem + ".SCX",
           mapStem + ".SCM" })
    {
      std::filesystem::path mapPath = findMapFileByName(installRoot, candidate);
      if (!mapPath.empty())
        return mapPath;
    }
    return {};
  }

  MapDataProof proveMapDataFromReplayArtifact(const std::string& installRoot)
  {
    MapDataProof proof;
    const std::vector<std::filesystem::path> roots = replayRootCandidates(installRoot);
    if (roots.empty())
    {
      proof.reason = "no StarCraft replay directories are configured";
      return proof;
    }

    bool sawReplayRoot = false;
    std::filesystem::path bestReplayPath;
    std::filesystem::path bestMapPath;
    std::string bestMapName;
    std::uintmax_t bestReplaySize = 0;
    std::uintmax_t bestMapSize = 0;
    std::filesystem::file_time_type bestTime = std::filesystem::file_time_type::min();

    for (const std::filesystem::path& root : roots)
    {
      std::error_code error;
      if (!std::filesystem::is_directory(root, error) || error)
        continue;
      sawReplayRoot = true;

      const std::filesystem::path lastReplay = root / "LastReplay.rep";
      const bool hasLastReplay = std::filesystem::is_regular_file(lastReplay, error) && !error;
      const std::uintmax_t lastReplaySize =
        hasLastReplay ? std::filesystem::file_size(lastReplay, error) : 0;
      if (error)
        continue;
      const std::filesystem::file_time_type lastReplayTime =
        hasLastReplay ? std::filesystem::last_write_time(lastReplay, error) : std::filesystem::file_time_type::min();
      if (error)
        continue;

      const std::filesystem::recursive_directory_iterator end;
      for (std::filesystem::recursive_directory_iterator it(
             root,
             std::filesystem::directory_options::skip_permission_denied,
             error);
           !error && it != end;
           it.increment(error))
      {
        if (error || !it->is_regular_file(error) || error)
          continue;
        const std::filesystem::path replayPath = it->path();
        if (lowerCase(replayPath.extension().string()) != ".rep")
          continue;
        if (lowerCase(replayPath.filename().string()) == "lastreplay.rep")
          continue;

        const std::string mapName = replayAutosaveMapStem(replayPath);
        if (mapName.empty())
          continue;

        const std::uintmax_t replaySize = std::filesystem::file_size(replayPath, error);
        if (error || replaySize == 0)
          continue;
        const std::filesystem::file_time_type replayTime =
          std::filesystem::last_write_time(replayPath, error);
        if (error)
          continue;

        if (hasLastReplay
            && (replaySize != lastReplaySize
                || !fileTimeClose(replayTime, lastReplayTime, std::chrono::seconds(3))))
          continue;

        std::filesystem::path mapPath = findInstalledMapForReplayStem(installRoot, mapName);
        if (mapPath.empty())
          continue;

        const std::uintmax_t mapSize = std::filesystem::file_size(mapPath, error);
        if (error || mapSize == 0)
          continue;

        if (bestReplayPath.empty() || replayTime > bestTime)
        {
          bestReplayPath = replayPath;
          bestMapPath = mapPath;
          bestMapName = mapName;
          bestReplaySize = replaySize;
          bestMapSize = mapSize;
          bestTime = replayTime;
        }
      }
    }

    if (bestReplayPath.empty())
    {
      proof.reason = sawReplayRoot
        ? "no fresh LastReplay-matched autosave replay mapped to an installed map"
        : "no StarCraft replay directory exists";
      return proof;
    }

    proof.passed = true;
    proof.mapName = bestMapName;
    proof.mapPath = bestMapPath.string();
    proof.source = "latest-replay-artifact";
    proof.replayPath = bestReplayPath.string();
    proof.mapFileSize = bestMapSize;
    proof.replayFileSize = bestReplaySize;
    return proof;
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

  int frameCounterScore(
    std::uint32_t first,
    std::uint32_t second,
    std::uint32_t third,
    int sampleDelayMs)
  {
    const int firstDelta = static_cast<int>(second - first);
    const int secondDelta = static_cast<int>(third - second);
    const int expectedDelta = std::max(1, (sampleDelayMs * 24) / 1000);
    const int minimumFrameLikeDelta = std::max(2, expectedDelta / 3);
    const bool frameLike =
      firstDelta >= minimumFrameLikeDelta
      && secondDelta >= minimumFrameLikeDelta;
    const int expectedError =
      std::abs(firstDelta - expectedDelta)
      + std::abs(secondDelta - expectedDelta);
    const int stabilityError = std::abs(firstDelta - secondDelta);
    return (frameLike ? 0 : 100000) + expectedError + stabilityError;
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

  bool regionsIntersect(
    std::uintptr_t lhsAddress,
    std::size_t lhsSize,
    std::uintptr_t rhsAddress,
    std::size_t rhsSize)
  {
    if (lhsSize == 0 || rhsSize == 0)
      return false;
    const std::uintptr_t lhsEnd = lhsAddress + lhsSize;
    const std::uintptr_t rhsEnd = rhsAddress + rhsSize;
    if (lhsEnd < lhsAddress || rhsEnd < rhsAddress)
      return false;
    return lhsAddress < rhsEnd && rhsAddress < lhsEnd;
  }

  const RuntimeMemoryRegion* findReadableRegion(
    const std::vector<RuntimeMemoryRegion>& regions,
    std::uintptr_t address,
    std::size_t size)
  {
    for (const RuntimeMemoryRegion& region : regions)
    {
      if (region.readable && regionContains(region, address, size))
        return &region;
    }
    return nullptr;
  }

  bool readableAddress(
    const std::vector<RuntimeMemoryRegion>& regions,
    std::uintptr_t address,
    std::size_t size)
  {
    return findReadableRegion(regions, address, size) != nullptr;
  }

  const RuntimeMemoryRegion* findWritableRegion(
    const std::vector<RuntimeMemoryRegion>& regions,
    std::uintptr_t address,
    std::size_t size)
  {
    for (const RuntimeMemoryRegion& region : regions)
    {
      if (region.readable && region.writable && regionContains(region, address, size))
        return &region;
    }
    return nullptr;
  }

  bool writableAddress(
    const std::vector<RuntimeMemoryRegion>& regions,
    std::uintptr_t address,
    std::size_t size)
  {
    return findWritableRegion(regions, address, size) != nullptr;
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

  std::string normalizedPathForCompare(const std::string& path)
  {
    if (path.empty())
      return {};

    std::error_code error;
    std::filesystem::path normalized = std::filesystem::weakly_canonical(path, error);
    if (error)
      normalized = std::filesystem::absolute(path, error);
    if (error)
      normalized = path;
    return normalized.lexically_normal().string();
  }

  bool sameMappedFile(const std::string& lhs, const std::string& rhs)
  {
    if (lhs.empty() || rhs.empty())
      return false;
    return normalizedPathForCompare(lhs) == normalizedPathForCompare(rhs);
  }

  bool shouldSkipImageMappedRegion(
    const RuntimeMemoryRegion& region,
    const std::string& executablePath,
    bool includeImageMappedRegions)
  {
    return !includeImageMappedRegions
      && !executablePath.empty()
      && sameMappedFile(region.mappedPath, executablePath)
      && !region.writable;
  }

  bool fileBackedNonTargetRegion(
    const RuntimeMemoryRegion& region,
    const std::string& executablePath)
  {
    return !region.mappedPath.empty()
      && region.mappedPath.front() == '/'
      && !sameMappedFile(region.mappedPath, executablePath);
  }

  struct StarCraftImageSectionHints
  {
    std::uintptr_t commonAddress = 0;
    std::size_t commonSize = 0;
    std::uintptr_t bssAddress = 0;
    std::size_t bssSize = 0;
  };

  StarCraftImageSectionHints starCraftImageSectionHints(std::uintptr_t targetImageBase)
  {
    StarCraftImageSectionHints hints;
    if (targetImageBase == 0 || targetImageBase < 0x100000000ULL)
      return hints;

    const std::uintptr_t slide = targetImageBase - 0x100000000ULL;
    hints.commonAddress = 0x100f79b20ULL + slide;
    hints.commonSize = 0x521c8;
    hints.bssAddress = 0x100fcbcf0ULL + slide;
    hints.bssSize = 0x4d3844;
    return hints;
  }

  bool regionIntersectsStarCraftRuntimeData(
    const RuntimeMemoryRegion& region,
    const StarCraftImageSectionHints& hints)
  {
    return regionsIntersect(region.address, region.size, hints.commonAddress, hints.commonSize)
      || regionsIntersect(region.address, region.size, hints.bssAddress, hints.bssSize);
  }

  bool usableUnitStorageRegion(
    const RuntimeMemoryRegion& region,
    const std::string& executablePath,
    std::uintptr_t targetImageBase,
    bool allowTargetTextMapping = false)
  {
    if (!region.readable || region.executable)
      return false;
    if (fileBackedNonTargetRegion(region, executablePath))
      return false;

    const bool targetImageRegion = sameMappedFile(region.mappedPath, executablePath);
    const bool likelyTargetTextMapping =
      targetImageRegion
      && targetImageBase != 0
      && region.address == targetImageBase
      && region.size >= 8 * 1024 * 1024;
    return allowTargetTextMapping || !likelyTargetTextMapping;
  }

  int unitScanRegionPriority(
    const RuntimeMemoryRegion& region,
    const std::string& executablePath,
    std::uintptr_t targetImageBase)
  {
    const StarCraftImageSectionHints hints = starCraftImageSectionHints(targetImageBase);
    if (regionIntersectsStarCraftRuntimeData(region, hints))
      return 0;

    const bool targetImageRegion = sameMappedFile(region.mappedPath, executablePath);
    const bool likelyTargetTextMapping =
      targetImageRegion
      && targetImageBase != 0
      && region.address == targetImageBase
      && region.size >= 8 * 1024 * 1024;
    if (targetImageRegion && !likelyTargetTextMapping)
      return 2;
    if (region.mappedPath.empty())
      return 1;
    if (!fileBackedNonTargetRegion(region, executablePath))
      return 2;
    return 3;
  }

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
    if (containsLongPrintableAsciiRun(bytes, offset, recordSize))
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

  bool plausibleUnitNodeAnchorFields(
    const std::vector<unsigned char>& bytes,
    std::size_t offset)
  {
    constexpr std::size_t recordSize = 0x58;
    if (offset + recordSize > bytes.size())
      return false;
    if (containsLongPrintableAsciiRun(bytes, offset, recordSize))
      return false;

    const std::uint64_t previous = readU64(bytes, offset);
    const std::uint64_t next = readU64(bytes, offset + 0x08);
    const std::int16_t x = readS16(bytes, offset + 0x24);
    const std::int16_t y = readS16(bytes, offset + 0x26);
    const std::int16_t targetX = readS16(bytes, offset + 0x28);
    const std::int16_t targetY = readS16(bytes, offset + 0x2a);
    const std::uint16_t stateA = readU16(bytes, offset + 0x30);
    const std::uint16_t stateB = readU16(bytes, offset + 0x32);
    const std::uint64_t sprite = readU64(bytes, offset + 0x38);
    const std::uint64_t secondaryObject = readU64(bytes, offset + 0x50);

    const bool linked = previous >= 0x100000000ULL || next >= 0x100000000ULL;
    return linked
      && x >= 16
      && x <= 16384
      && y >= 16
      && y <= 16384
      && targetX >= 16
      && targetX <= 16384
      && targetY >= 16
      && targetY <= 16384
      && stateA < 256
      && stateB < 256
      && sprite >= 0x100000000ULL
      && secondaryObject >= 0x100000000ULL;
  }

  bool plausibleUnitNodeAnchorRecord(
    const std::vector<unsigned char>& bytes,
    std::size_t offset,
    const std::vector<RuntimeMemoryRegion>& regions)
  {
    if (!plausibleUnitNodeAnchorFields(bytes, offset))
      return false;

    const std::uint64_t previous = readU64(bytes, offset);
    const std::uint64_t next = readU64(bytes, offset + 0x08);
    const std::uint64_t sprite = readU64(bytes, offset + 0x38);
    const std::uint64_t secondaryObject = readU64(bytes, offset + 0x50);
    return (readablePointerValue(regions, previous, 16) || readablePointerValue(regions, next, 16))
      && readablePointerValue(regions, sprite, 16)
      && readablePointerValue(regions, secondaryObject, 16);
  }

  LiveUnitNodeProof scoreUnitNodeAnchorArray(
    const std::vector<unsigned char>& bytes,
    std::uintptr_t baseAddress,
    std::size_t offset,
    const std::vector<RuntimeMemoryRegion>& regions,
    const std::chrono::steady_clock::time_point& deadline,
    bool& scanTimedOut)
  {
    constexpr std::size_t recordSize = 0x58;
    constexpr std::size_t maxSampledRecords = 2048;

    LiveUnitNodeProof proof;
    proof.recordSize = recordSize;
    proof.sampledRecords = std::min(maxSampledRecords, (bytes.size() - offset) / recordSize);
    std::size_t consecutiveActiveRecords = 0;
    std::uintptr_t firstConsecutiveAddress = 0;

    for (std::size_t i = 0; i < proof.sampledRecords; ++i)
    {
      if ((i % 16) == 0 && timedOut(deadline))
      {
        scanTimedOut = true;
        return {};
      }
      if (plausibleUnitNodeAnchorRecord(bytes, offset + i * recordSize, regions))
      {
        if (consecutiveActiveRecords == 0)
          firstConsecutiveAddress = baseAddress + offset + i * recordSize;
        ++consecutiveActiveRecords;
        proof.activeRecords = std::max(proof.activeRecords, consecutiveActiveRecords);
        if (consecutiveActiveRecords >= minActiveUnitRecords)
        {
          proof.address = firstConsecutiveAddress;
          proof.passed = true;
          return proof;
        }
      }
      else
      {
        consecutiveActiveRecords = 0;
        firstConsecutiveAddress = 0;
      }
    }

    proof.reason = "candidate SC:R unit-node anchor array did not contain enough active records";
    return proof;
  }

  LiveUnitNodeProof proveUnitNodeAnchorsInBytes(
    const std::vector<unsigned char>& bytes,
    std::uintptr_t baseAddress,
    const std::vector<RuntimeMemoryRegion>& regions,
    const std::chrono::steady_clock::time_point& deadline,
    bool& scanTimedOut)
  {
    constexpr std::size_t recordSize = 0x58;
    if (recordSize * minActiveUnitRecords > bytes.size())
      return {};

    std::vector<std::size_t> plausibleByResidue(recordSize, 0);
    for (std::size_t recordOffset = 0; recordOffset + recordSize <= bytes.size(); recordOffset += 8)
    {
      if ((recordOffset % (4 * 1024)) == 0 && timedOut(deadline))
      {
        scanTimedOut = true;
        return {};
      }
      if (plausibleUnitNodeAnchorFields(bytes, recordOffset))
        ++plausibleByResidue[recordOffset % recordSize];
    }

    std::vector<std::size_t> residues;
    residues.reserve(recordSize);
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

    constexpr std::size_t maxResiduesToScore = 16;
    const std::size_t residuesToScore = std::min(maxResiduesToScore, residues.size());
    for (std::size_t index = 0; index < residuesToScore; ++index)
    {
      LiveUnitNodeProof proof = scoreUnitNodeAnchorArray(
        bytes,
        baseAddress,
        residues[index],
        regions,
        deadline,
        scanTimedOut);
      if (scanTimedOut || proof.passed)
        return proof;
    }

    return {};
  }

  LiveUnitNodeProof proveUnitNodeVectorsInBytes(
    int processId,
    const std::vector<unsigned char>& bytes,
    std::uintptr_t baseAddress,
    const std::vector<RuntimeMemoryRegion>& regions,
    const std::chrono::steady_clock::time_point& deadline,
    bool& scanTimedOut)
  {
    constexpr std::size_t recordSize = 0x58;
    constexpr std::size_t maxUnitNodeRecords = 4096;
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

      const std::size_t usedBytes = static_cast<std::size_t>(end - begin);
      if (usedBytes < recordSize * minActiveUnitRecords || (usedBytes % recordSize) != 0)
        continue;
      const std::size_t recordCount = usedBytes / recordSize;
      if (recordCount > maxUnitNodeRecords)
        continue;
      if (!readableAddress(regions, begin, std::min<std::size_t>(usedBytes, recordSize * minActiveUnitRecords)))
        continue;

      RuntimeMemoryReadResult read = readProcessMemory(processId, begin, usedBytes);
      if (!read.success || read.bytesRead < recordSize * minActiveUnitRecords)
        continue;

      LiveUnitNodeProof proof = scoreUnitNodeAnchorArray(
        read.bytes,
        begin,
        0,
        regions,
        deadline,
        scanTimedOut);
      if (scanTimedOut)
        return {};
      if (proof.passed)
      {
        proof.vectorAddress = baseAddress + offset;
        proof.sampledRecords = recordCount;
        return proof;
      }
    }

    return {};
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

  LiveUnitsProof scoreCUnitPointerArray(
    const std::vector<std::vector<unsigned char>>& recordSnapshots,
    std::uintptr_t pointerArrayAddress,
    std::size_t recordSize,
    const UnitRecordLayout& layout,
    const std::vector<RuntimeMemoryRegion>& regions,
    const std::chrono::steady_clock::time_point& deadline,
    bool& scanTimedOut,
    bool requireReadableSprite,
    UnitScanDiagnostics* diagnostics)
  {
    constexpr std::size_t maxSampledPointers = 256;

    LiveUnitsProof proof;
    proof.address = pointerArrayAddress;
    proof.recordSize = recordSize;
    proof.idOffset = layout.idOffset;
    proof.positionOffset = layout.positionOffset;
    proof.hitPointsOffset = layout.hitPointsOffset;
    proof.orderOffset = layout.orderOffset;
    proof.playerOffset = layout.playerOffset;
    proof.pointerArray = true;
    proof.layoutName = std::string(layout.name) + "-pointer-array";

    proof.sampledRecords = std::min(maxSampledPointers, recordSnapshots.size());
    for (std::size_t i = 0; i < proof.sampledRecords; ++i)
    {
      if ((i % 16) == 0 && timedOut(deadline))
      {
        scanTimedOut = true;
        return {};
      }

      const std::vector<unsigned char>& recordBytes = recordSnapshots[i];
      if (recordBytes.size() < recordSize)
        continue;

      if (plausibleUnitRecordWithDiagnostics(
            recordBytes,
            0,
            recordSize,
            layout,
            regions,
            requireReadableSprite,
            diagnostics))
        ++proof.activeRecords;

      if (proof.activeRecords >= minActiveUnitRecords)
      {
        proof.passed = true;
        return proof;
      }
    }

    proof.reason = "candidate CUnit pointer array did not contain enough active BWAPI-compatible records";
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

        std::vector<std::size_t> plausibleByResidue(recordSize, 0);
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

          ++plausibleByResidue[recordOffset % recordSize];
        }

        std::vector<std::size_t> residues;
        residues.reserve(recordSize);
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

        constexpr std::size_t maxResiduesToScorePerRecordSize = 64;
        const std::size_t residuesToScore =
          std::min(maxResiduesToScorePerRecordSize, residues.size());
        for (std::size_t index = 0; index < residuesToScore; ++index)
        {
          const std::size_t baseOffset = residues[index];
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
        }
      }
    }

    return {};
  }

  LiveUnitsProof proveClassicUnitVectorInBytes(
    int processId,
    const std::vector<RuntimeMemoryRegion>& regions,
    const std::vector<unsigned char>& bytes,
    std::uintptr_t baseAddress,
    const std::string& executablePath,
    std::uintptr_t targetImageBase,
    const std::chrono::steady_clock::time_point& deadline,
    bool& scanTimedOut,
    UnitScanDiagnostics* diagnostics)
  {
    constexpr std::size_t maxVectorBytes = 32 * 1024 * 1024;
    constexpr std::size_t maxPointerArrayScores = 128;
    std::size_t pointerArrayScores = 0;
    std::unordered_set<std::uintptr_t> scoredBegins;

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
      const std::size_t usedBytes = static_cast<std::size_t>(end - begin);
      if (usedBytes == 0 || usedBytes > maxVectorBytes)
        continue;
      if (!scoredBegins.insert(begin).second)
      {
        if (diagnostics != nullptr)
          ++diagnostics->vectorDuplicateBegins;
        continue;
      }

      const RuntimeMemoryRegion* beginRegion =
        findReadableRegion(regions, begin, std::min<std::size_t>(usedBytes, 4096));
      if (beginRegion == nullptr)
        continue;
      if (!usableUnitStorageRegion(*beginRegion, executablePath, targetImageBase))
      {
        if (diagnostics != nullptr)
          ++diagnostics->vectorRejectedTargetRegions;
        continue;
      }

      if (diagnostics != nullptr)
        ++diagnostics->vectorCandidates;

      if (pointerArrayScores < maxPointerArrayScores && usedBytes % sizeof(std::uint64_t) == 0)
      {
        const std::size_t pointerCount = usedBytes / sizeof(std::uint64_t);
        if (pointerCount >= minActiveUnitRecords && pointerCount <= 1700)
        {
          if (diagnostics != nullptr)
            ++diagnostics->pointerArrayCandidates;
          RuntimeMemoryReadResult pointerRead = readProcessMemory(processId, begin, usedBytes);
          if (pointerRead.success && pointerRead.bytesRead == usedBytes)
          {
            std::size_t readablePointers = 0;
            const std::size_t pointersToPrecheck = std::min<std::size_t>(pointerCount, 128);
            for (std::size_t i = 0; i < pointersToPrecheck; ++i)
            {
              const std::uint64_t pointerValue = readU64(pointerRead.bytes, i * sizeof(std::uint64_t));
              if (readablePointerValue(regions, pointerValue, 336))
              {
                ++readablePointers;
                if (diagnostics != nullptr)
                  ++diagnostics->pointerArrayReadablePointerHits;
                if (readablePointers >= minActiveUnitRecords)
                  break;
              }
            }

            if (readablePointers >= minActiveUnitRecords)
            {
              std::vector<std::vector<unsigned char>> recordSnapshots;
              recordSnapshots.reserve(std::min<std::size_t>(pointerCount, 256));
              constexpr std::size_t maxRecordSnapshotBytes = 768;
              for (std::size_t i = 0;
                   i < pointerCount && recordSnapshots.size() < 256 && !timedOut(deadline);
                   ++i)
              {
                const std::uint64_t pointerValue = readU64(pointerRead.bytes, i * sizeof(std::uint64_t));
                if (!readablePointerValue(regions, pointerValue, 336))
                  continue;
                RuntimeMemoryReadResult recordRead = readProcessMemory(
                  processId,
                  static_cast<std::uintptr_t>(pointerValue),
                  maxRecordSnapshotBytes);
                if (recordRead.success && recordRead.bytesRead >= 336)
                  recordSnapshots.push_back(std::move(recordRead.bytes));
              }
              if (timedOut(deadline))
              {
                scanTimedOut = true;
                return {};
              }
              if (recordSnapshots.size() < minActiveUnitRecords)
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
                  ++pointerArrayScores;
                  if (diagnostics != nullptr)
                  {
                    ++diagnostics->candidateArraysScored;
                    ++diagnostics->pointerArraysScored;
                  }

                  LiveUnitsProof proof = scoreCUnitPointerArray(
                    recordSnapshots,
                    begin,
                    recordSize,
                    layout,
                    regions,
                    deadline,
                    scanTimedOut,
                    true,
                    diagnostics);
                  if (scanTimedOut)
                    return {};
                  if (diagnostics != nullptr)
                  {
                    diagnostics->plausibleRecords += proof.activeRecords;
                    if (proof.activeRecords > 0)
                      ++diagnostics->stridedCandidates;
                  }
                  if (proof.passed)
                    return proof;
                  rememberBestCandidate(diagnostics, proof);
                  if (pointerArrayScores >= maxPointerArrayScores)
                    break;
                }
                if (pointerArrayScores >= maxPointerArrayScores)
                  break;
              }
            }
          }
        }
      }

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
    const std::string& executablePath,
    std::size_t maxScanBytes,
    int scanTimeoutMs,
    bool includeReadableOnlyRegions,
    bool includeImageMappedRegions,
    bool scanVectors,
    UnitScanDiagnostics* diagnostics)
  {
    RuntimeMemoryRegionListResult regions = listProcessMemoryRegions(processId);
    if (!regions.success)
      return failedUnitsProof(regions.reason);

    std::uintptr_t targetImageBase = 0;
    for (const RuntimeMemoryRegion& region : regions.regions)
    {
      if (!sameMappedFile(region.mappedPath, executablePath))
        continue;
      if (targetImageBase == 0 || region.address < targetImageBase)
        targetImageBase = region.address;
    }

    std::vector<RuntimeMemoryRegion> scanRegions = regions.regions;
    std::stable_sort(
      scanRegions.begin(),
      scanRegions.end(),
      [&](const RuntimeMemoryRegion& lhs, const RuntimeMemoryRegion& rhs)
      {
        const int lhsPriority = unitScanRegionPriority(lhs, executablePath, targetImageBase);
        const int rhsPriority = unitScanRegionPriority(rhs, executablePath, targetImageBase);
        if (lhsPriority != rhsPriority)
          return lhsPriority < rhsPriority;
        return lhs.address < rhs.address;
      });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(scanTimeoutMs);
    const std::size_t defaultMaxRegionBytes = 2 * 1024 * 1024;
    const std::size_t targetImageDataMaxRegionBytes = 16 * 1024 * 1024;
    std::size_t scanned = 0;
    for (const RuntimeMemoryRegion& region : scanRegions)
    {
      if (timedOut(deadline))
      {
        if (diagnostics != nullptr)
          diagnostics->timedOut = true;
        return failedUnitsProof(unitScanTimeoutReason(diagnostics));
      }

      if (!region.readable || region.size < 336 * 4)
        continue;
      if (fileBackedNonTargetRegion(region, executablePath))
        continue;
      if (region.executable)
      {
        if (diagnostics != nullptr)
          ++diagnostics->executableReadableRegions;
        continue;
      }
      const bool imageMappedRegion = sameMappedFile(region.mappedPath, executablePath);
      if (imageMappedRegion && diagnostics != nullptr)
        ++diagnostics->imageMappedRegions;
      const bool likelyTargetTextMapping =
        imageMappedRegion
        && targetImageBase != 0
        && region.address == targetImageBase
        && region.size >= 8 * 1024 * 1024;
      if (likelyTargetTextMapping && !includeImageMappedRegions)
      {
        if (diagnostics != nullptr)
          ++diagnostics->skippedImageMappedRegions;
        continue;
      }
      if (shouldSkipImageMappedRegion(region, executablePath, includeImageMappedRegions))
      {
        if (diagnostics != nullptr)
          ++diagnostics->skippedImageMappedRegions;
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

      const std::size_t maxRegionBytes =
        imageMappedRegion && region.writable
          ? targetImageDataMaxRegionBytes
          : defaultMaxRegionBytes;
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
          executablePath,
          targetImageBase,
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

  LiveUnitNodeProof proveLiveUnitNodeAnchors(
    int processId,
    const std::string& executablePath,
    std::size_t maxScanBytes,
    int scanTimeoutMs)
  {
    RuntimeMemoryRegionListResult regions = listProcessMemoryRegions(processId);
    if (!regions.success)
      return failedUnitNodeProof(regions.reason);

    std::uintptr_t targetImageBase = 0;
    for (const RuntimeMemoryRegion& region : regions.regions)
    {
      if (!sameMappedFile(region.mappedPath, executablePath))
        continue;
      if (targetImageBase == 0 || region.address < targetImageBase)
        targetImageBase = region.address;
    }

    std::vector<RuntimeMemoryRegion> scanRegions = regions.regions;
    std::stable_sort(
      scanRegions.begin(),
      scanRegions.end(),
      [&](const RuntimeMemoryRegion& lhs, const RuntimeMemoryRegion& rhs)
      {
        const int lhsPriority = unitScanRegionPriority(lhs, executablePath, targetImageBase);
        const int rhsPriority = unitScanRegionPriority(rhs, executablePath, targetImageBase);
        if (lhsPriority != rhsPriority)
          return lhsPriority < rhsPriority;
        return lhs.address < rhs.address;
      });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(scanTimeoutMs);
    constexpr std::size_t maxRegionBytes = 4 * 1024 * 1024;
    std::size_t scanned = 0;

    const StarCraftImageSectionHints hints = starCraftImageSectionHints(targetImageBase);
    const std::array<std::pair<std::uintptr_t, std::size_t>, 2> prioritySections = {
      std::make_pair(hints.bssAddress, hints.bssSize),
      std::make_pair(hints.commonAddress, hints.commonSize)
    };
    for (const auto& section : prioritySections)
    {
      if (section.first == 0 || section.second < 0x58 * minActiveUnitRecords)
        continue;
      const std::size_t bytesToRead = std::min(section.second, maxRegionBytes);
      RuntimeMemoryReadResult read = readProcessMemory(processId, section.first, bytesToRead);
      if (!read.success || read.bytesRead < 0x58 * minActiveUnitRecords)
        continue;

      bool scanTimedOut = false;
      LiveUnitNodeProof vectorProof = proveUnitNodeVectorsInBytes(
        processId,
        read.bytes,
        section.first,
        regions.regions,
        deadline,
        scanTimedOut);
      if (scanTimedOut)
        return failedUnitNodeProof("SC:R unit-node vector section scan timed out before proof");
      if (vectorProof.passed)
        return vectorProof;

      LiveUnitNodeProof proof = proveUnitNodeAnchorsInBytes(
        read.bytes,
        section.first,
        regions.regions,
        deadline,
        scanTimedOut);
      if (scanTimedOut)
        return failedUnitNodeProof("SC:R unit-node anchor section scan timed out before proof");
      if (proof.passed)
        return proof;
    }

    for (const RuntimeMemoryRegion& region : scanRegions)
    {
      if (timedOut(deadline))
        return failedUnitNodeProof("SC:R unit-node anchor scan timed out before proof");
      if (!usableUnitStorageRegion(region, executablePath, targetImageBase))
        continue;
      if (region.size < 0x58 * minActiveUnitRecords)
        continue;
      if (scanned >= maxScanBytes)
        return failedUnitNodeProof("no active SC:R unit-node anchor found before scan byte limit");

      const std::size_t bytesToRead =
        std::min(region.size, std::min(maxRegionBytes, maxScanBytes - scanned));
      RuntimeMemoryReadResult read = readProcessMemory(processId, region.address, bytesToRead);
      if (!read.success || read.bytesRead < 0x58 * minActiveUnitRecords)
        continue;

      bool scanTimedOut = false;
      LiveUnitNodeProof vectorProof = proveUnitNodeVectorsInBytes(
        processId,
        read.bytes,
        region.address,
        regions.regions,
        deadline,
        scanTimedOut);
      if (scanTimedOut)
        return failedUnitNodeProof("SC:R unit-node vector scan timed out before proof");
      if (vectorProof.passed)
        return vectorProof;

      LiveUnitNodeProof proof = proveUnitNodeAnchorsInBytes(
        read.bytes,
        region.address,
        regions.regions,
        deadline,
        scanTimedOut);
      if (scanTimedOut)
        return failedUnitNodeProof("SC:R unit-node anchor scan timed out before proof");
      if (proof.passed)
        return proof;

      scanned += read.bytesRead;
    }

    return failedUnitNodeProof("no active SC:R unit-node anchor found");
  }

  bool parseRemasteredUnitSnapshotRecord(
    int processId,
    const std::vector<unsigned char>& bytes,
    std::size_t offset,
    std::uintptr_t nodeAddress,
    const std::vector<RuntimeMemoryRegion>& regions,
    RemasteredUnitSnapshotRecord& record)
  {
    if (!plausibleUnitNodeAnchorRecord(bytes, offset, regions))
      return false;

    constexpr std::size_t secondaryRecordSize = 0x48;
    const std::uint64_t secondaryAddress64 = readU64(bytes, offset + 0x50);
    if (!addressFits(secondaryAddress64))
      return false;
    const auto secondaryAddress = static_cast<std::uintptr_t>(secondaryAddress64);
    if (!readableAddress(regions, secondaryAddress, secondaryRecordSize))
      return false;

    RuntimeMemoryReadResult secondaryRead =
      readProcessMemory(processId, secondaryAddress, secondaryRecordSize);
    if (!secondaryRead.success || secondaryRead.bytesRead < secondaryRecordSize)
      return false;

    const unsigned char rawPlayer = secondaryRead.bytes[0x14];
    if (!(rawPlayer < 12 || rawPlayer == 255))
      return false;

    record.nodeAddress = nodeAddress;
    record.secondaryAddress = secondaryAddress;
    record.spriteAddress = static_cast<std::uintptr_t>(readU64(bytes, offset + 0x38));
    record.id = readU16(secondaryRead.bytes, 0x18);
    record.x = readS16(bytes, offset + 0x24);
    record.y = readS16(bytes, offset + 0x26);
    record.targetX = readS16(bytes, offset + 0x28);
    record.targetY = readS16(bytes, offset + 0x2a);
    record.order = readU16(bytes, offset + 0x30);
    record.state = readU16(bytes, offset + 0x32);
    record.player = rawPlayer == 255 ? 11 : static_cast<int>(rawPlayer);
    record.typeHint = readU16(secondaryRead.bytes, 0x20);
    return record.id != 0;
  }

  LiveUnitsProof proveRemasteredUnitNodeSnapshot(
    int processId,
    const std::string& executablePath,
    std::size_t maxScanBytes,
    int scanTimeoutMs,
    LiveUnitNodeProof* activeUnitNodeProof)
  {
    LiveUnitNodeProof nodeProof =
      activeUnitNodeProof != nullptr && activeUnitNodeProof->passed
        ? *activeUnitNodeProof
        : proveLiveUnitNodeAnchors(processId, executablePath, maxScanBytes, scanTimeoutMs);
    if (!nodeProof.passed)
      return failedUnitsProof(nodeProof.reason.empty()
        ? "no active SC:R unit-node graph found"
        : nodeProof.reason);

    RuntimeMemoryRegionListResult regions = listProcessMemoryRegions(processId);
    if (!regions.success)
      return failedUnitsProof(regions.reason);

    const RuntimeMemoryRegion* containingRegion =
      findReadableRegion(regions.regions, nodeProof.address, nodeProof.recordSize * minActiveUnitRecords);
    if (containingRegion == nullptr)
      return failedUnitsProof("active SC:R unit-node address is no longer readable");

    constexpr std::size_t maxSnapshotRecords = 256;
    const std::uintptr_t regionEnd = containingRegion->address + containingRegion->size;
    const std::size_t regionBytes =
      regionEnd > nodeProof.address
        ? static_cast<std::size_t>(regionEnd - nodeProof.address)
        : 0;
    const std::size_t bytesToRead = std::min(
      regionBytes,
      nodeProof.recordSize * maxSnapshotRecords);
    if (bytesToRead < nodeProof.recordSize * minActiveUnitRecords)
      return failedUnitsProof("active SC:R unit-node region is too small for a unit snapshot");

    RuntimeMemoryReadResult read =
      readProcessMemory(processId, nodeProof.address, bytesToRead);
    if (!read.success || read.bytesRead < nodeProof.recordSize * minActiveUnitRecords)
      return failedUnitsProof(read.reason.empty()
        ? "unable to read active SC:R unit-node snapshot"
        : read.reason);

    std::vector<RemasteredUnitSnapshotRecord> records;
    std::size_t invalidAfterFirstRecord = 0;
    const std::size_t availableRecords = read.bytesRead / nodeProof.recordSize;
    for (std::size_t i = 0; i < availableRecords; ++i)
    {
      RemasteredUnitSnapshotRecord record;
      record.index = i;
      const std::size_t offset = i * nodeProof.recordSize;
      if (parseRemasteredUnitSnapshotRecord(
            processId,
            read.bytes,
            offset,
            nodeProof.address + offset,
            regions.regions,
            record))
      {
        records.push_back(record);
        invalidAfterFirstRecord = 0;
        continue;
      }

      if (!records.empty() && ++invalidAfterFirstRecord >= 8)
        break;
    }

    if (records.size() < minActiveUnitRecords)
      return failedUnitsProof("SC:R unit-node graph did not produce enough BWAPI-facing unit snapshot records");

    nodeProof.records = records;
    nodeProof.activeRecords = records.size();
    if (activeUnitNodeProof != nullptr)
      *activeUnitNodeProof = nodeProof;

    LiveUnitsProof proof;
    proof.passed = true;
    proof.address = nodeProof.address;
    proof.recordSize = nodeProof.recordSize;
    proof.positionOffset = 0x24;
    proof.orderOffset = 0x30;
    proof.sampledRecords = availableRecords;
    proof.activeRecords = records.size();
    proof.derivedSnapshot = true;
    proof.hitPointsResolved = false;
    proof.layoutName = "scr-unit-node-object-graph";
    return proof;
  }

  MapDataProof proveMapData(
    int processId,
    const std::string& executablePath,
    const std::string& installRoot,
    std::size_t maxScanBytes,
    int scanTimeoutMs)
  {
    RuntimeMemoryRegionListResult regions = listProcessMemoryRegions(processId);
    if (!regions.success)
    {
      MapDataProof proof;
      proof.reason = regions.reason;
      return proof;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(scanTimeoutMs);
    constexpr std::size_t maxRegionBytes = 4 * 1024 * 1024;
    std::uintptr_t targetImageBase = 0;
    for (const RuntimeMemoryRegion& region : regions.regions)
    {
      if (!sameMappedFile(region.mappedPath, executablePath))
        continue;
      if (targetImageBase == 0 || region.address < targetImageBase)
        targetImageBase = region.address;
    }

    std::vector<RuntimeMemoryRegion> candidateRegions;
    for (const RuntimeMemoryRegion& region : regions.regions)
    {
      if (!region.readable || region.executable)
        continue;
      if (fileBackedNonTargetRegion(region, executablePath))
        continue;
      if (region.size < 8)
        continue;
      candidateRegions.push_back(region);
    }

    std::sort(
      candidateRegions.begin(),
      candidateRegions.end(),
      [&](const RuntimeMemoryRegion& lhs, const RuntimeMemoryRegion& rhs)
      {
        const int lhsPriority = unitScanRegionPriority(lhs, executablePath, targetImageBase);
        const int rhsPriority = unitScanRegionPriority(rhs, executablePath, targetImageBase);
        if (lhsPriority != rhsPriority)
          return lhsPriority < rhsPriority;
        return lhs.size < rhs.size;
      });

    std::size_t scanned = 0;
    for (const RuntimeMemoryRegion& region : candidateRegions)
    {
      if (timedOut(deadline))
      {
        MapDataProof proof = proveMapDataFromReplayArtifact(installRoot);
        if (proof.passed)
          return proof;
        proof.reason = "map-data scan timed out before proof; replay artifact fallback failed: " + proof.reason;
        return proof;
      }
      if (scanned >= maxScanBytes)
        break;

      const std::size_t bytesToRead =
        std::min(region.size, std::min(maxRegionBytes, maxScanBytes - scanned));
      RuntimeMemoryReadResult read = readProcessMemory(processId, region.address, bytesToRead);
      if (!read.success || read.bytesRead < 8)
        continue;
      scanned += read.bytesRead;

      for (std::size_t offset = 0; offset < read.bytes.size(); ++offset)
      {
        if ((offset % (4 * 1024)) == 0 && timedOut(deadline))
        {
          MapDataProof proof = proveMapDataFromReplayArtifact(installRoot);
          if (proof.passed)
            return proof;
          proof.reason = "map-data scan timed out before proof; replay artifact fallback failed: " + proof.reason;
          return proof;
        }
        std::vector<std::string> candidates;
        if (asciiPrintable(read.bytes[offset]))
          candidates.push_back(extractNullTerminatedAsciiString(read.bytes, offset, 128));
        if (looksLikeUtf16AsciiAt(read.bytes, offset, false))
          candidates.push_back(extractNullTerminatedUtf16String(read.bytes, offset, 128, false));
        if (looksLikeUtf16AsciiAt(read.bytes, offset, true))
          candidates.push_back(extractNullTerminatedUtf16String(read.bytes, offset, 128, true));

        for (const std::string& candidate : candidates)
        {
          std::string mapName = basenameFromMapPathCandidate(candidate);
          if (mapName.empty())
            continue;

          std::filesystem::path mapPath = existingMapPathCandidate(candidate);
          if (mapPath.empty())
            mapPath = findMapFileByName(installRoot, mapName);
          if (mapPath.empty())
            continue;

          std::error_code error;
          const std::uintmax_t fileSize = std::filesystem::file_size(mapPath, error);
          if (error || fileSize == 0)
            continue;

          MapDataProof proof;
          proof.passed = true;
          proof.mapNameAddress = region.address + offset;
          proof.mapName = mapName;
          proof.mapPath = mapPath.string();
          proof.mapFileSize = fileSize;
          return proof;
        }
      }
    }

    MapDataProof replayProof = proveMapDataFromReplayArtifact(installRoot);
    if (replayProof.passed)
      return replayProof;

    MapDataProof proof;
    proof.reason =
      "no live StarCraft map path or filename matched an installed map file; replay artifact fallback failed: "
      + replayProof.reason;
    return proof;
  }

  PlayerDataProof provePlayerDataFromUnitSnapshot(const LiveUnitNodeProof& nodeProof)
  {
    PlayerDataProof proof;
    if (!nodeProof.passed || nodeProof.records.empty())
    {
      proof.reason = "player-data proof requires a passing live unit snapshot";
      return proof;
    }

    std::array<std::size_t, 12> unitCounts = {};
    for (const RemasteredUnitSnapshotRecord& record : nodeProof.records)
    {
      if (record.player < 0 || record.player >= static_cast<int>(unitCounts.size()))
        continue;
      ++unitCounts[static_cast<std::size_t>(record.player)];
      ++proof.observedUnits;
    }

    for (std::size_t player = 0; player < unitCounts.size(); ++player)
    {
      if (unitCounts[player] == 0)
        continue;
      PlayerSnapshotRecord record;
      record.player = static_cast<int>(player);
      record.unitCount = unitCounts[player];
      proof.players.push_back(record);
    }

    if (proof.players.empty())
    {
      proof.reason = "unit snapshot did not contain any valid player ids";
      return proof;
    }

    proof.passed = true;
    proof.playerCount = proof.players.size();
    return proof;
  }

  ReplayAnalysisProof proveReplayAnalysisFromLiveMetadata(
    const LiveCounterProof& gameStateProof,
    const MapDataProof& mapProof,
    const PlayerDataProof& playerProof)
  {
    ReplayAnalysisProof proof;
    if (!gameStateProof.passed)
    {
      proof.reason = "replay-analysis proof requires a passing live game-state counter proof";
      return proof;
    }
    if (!mapProof.passed)
    {
      proof.reason = "replay-analysis proof requires a passing live map-data proof";
      return proof;
    }
    if (gameStateProof.first == gameStateProof.second && gameStateProof.second == gameStateProof.third)
    {
      proof.reason = "replay-analysis proof requires frame progression";
      return proof;
    }

    proof.passed = true;
    proof.mapName = mapProof.mapName;
    proof.playerCount = playerProof.passed ? playerProof.playerCount : 0;
    proof.firstFrame = gameStateProof.first;
    proof.lastFrame = gameStateProof.third;
    return proof;
  }

  bool plausibleCommandQueueVector(
    const std::vector<RuntimeMemoryRegion>& regions,
    std::uintptr_t begin,
    std::uintptr_t end,
    std::uintptr_t capacity,
    std::size_t& usedBytes,
    std::size_t& capacityBytes)
  {
    if (begin == 0 || capacity <= begin || end < begin || end > capacity)
      return false;

    usedBytes = static_cast<std::size_t>(end - begin);
    capacityBytes = static_cast<std::size_t>(capacity - begin);
    if (capacityBytes < 64 || capacityBytes > 64 * 1024)
      return false;
    if (usedBytes > capacityBytes)
      return false;

    const std::size_t bytesToCheck = std::max<std::size_t>(1, std::min<std::size_t>(capacityBytes, 64));
    return writableAddress(regions, begin, bytesToCheck);
  }

  int commandQueueCandidateScore(
    const RuntimeMemoryRegion& vectorRegion,
    const RuntimeMemoryRegion& bufferRegion,
    const std::string& executablePath,
    const StarCraftImageSectionHints& hints,
    std::size_t usedBytes,
    std::size_t capacityBytes)
  {
    int score = 0;
    if (regionIntersectsStarCraftRuntimeData(vectorRegion, hints))
      score += 100;
    if (sameMappedFile(vectorRegion.mappedPath, executablePath))
      score += 50;
    if (sameMappedFile(bufferRegion.mappedPath, executablePath))
      score += 25;
    if (usedBytes == 0)
      score += 8;
    else if (usedBytes <= 512)
      score += 12;
    if (capacityBytes <= 8192)
      score += 10;
    if (capacityBytes <= 4096)
      score += 5;
    return score;
  }

  CommandQueueDiscoveryProof discoverCommandQueueCandidates(
    int processId,
    const std::string& executablePath,
    std::size_t maxScanBytes,
    int scanTimeoutMs)
  {
    CommandQueueDiscoveryProof proof;
    RuntimeMemoryRegionListResult regions = listProcessMemoryRegions(processId);
    if (!regions.success)
    {
      proof.reason = regions.reason;
      return proof;
    }

    std::uintptr_t targetImageBase = 0;
    for (const RuntimeMemoryRegion& region : regions.regions)
    {
      if (!sameMappedFile(region.mappedPath, executablePath))
        continue;
      if (targetImageBase == 0 || region.address < targetImageBase)
        targetImageBase = region.address;
    }

    const StarCraftImageSectionHints hints = starCraftImageSectionHints(targetImageBase);
    std::vector<RuntimeMemoryRegion> scanRegions = regions.regions;
    std::stable_sort(
      scanRegions.begin(),
      scanRegions.end(),
      [&](const RuntimeMemoryRegion& lhs, const RuntimeMemoryRegion& rhs)
      {
        const int lhsPriority = unitScanRegionPriority(lhs, executablePath, targetImageBase);
        const int rhsPriority = unitScanRegionPriority(rhs, executablePath, targetImageBase);
        if (lhsPriority != rhsPriority)
          return lhsPriority < rhsPriority;
        return lhs.address < rhs.address;
      });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(scanTimeoutMs);
    constexpr std::size_t maxRegionBytes = 4 * 1024 * 1024;
    constexpr std::size_t maxCandidates = 128;
    std::unordered_set<std::uintptr_t> seenVectors;
    std::size_t scanned = 0;

    for (const RuntimeMemoryRegion& region : scanRegions)
    {
      if (timedOut(deadline))
      {
        proof.reason = "command queue discovery timed out before scan completed";
        break;
      }
      if (!region.readable || !region.writable || region.executable || region.size < sizeof(std::uint64_t) * 3)
        continue;
      if (fileBackedNonTargetRegion(region, executablePath))
        continue;
      if (scanned >= maxScanBytes)
      {
        proof.reason = "command queue discovery reached scan byte limit";
        break;
      }

      const std::size_t bytesToRead = std::min(region.size, std::min(maxRegionBytes, maxScanBytes - scanned));
      RuntimeMemoryReadResult read = readProcessMemory(processId, region.address, bytesToRead);
      if (!read.success || read.bytesRead < sizeof(std::uint64_t) * 3)
        continue;

      ++proof.scannedRegions;
      proof.scannedBytes += read.bytesRead;
      scanned += read.bytesRead;

      for (std::size_t offset = 0; offset + sizeof(std::uint64_t) * 3 <= read.bytes.size(); offset += 8)
      {
        if ((offset % (16 * 1024)) == 0 && timedOut(deadline))
        {
          proof.reason = "command queue discovery timed out while scoring vector candidates";
          break;
        }

        const std::uintptr_t vectorAddress = region.address + offset;
        if (!seenVectors.insert(vectorAddress).second)
          continue;

        const auto begin = static_cast<std::uintptr_t>(readU64(read.bytes, offset));
        const auto end = static_cast<std::uintptr_t>(readU64(read.bytes, offset + 8));
        const auto capacity = static_cast<std::uintptr_t>(readU64(read.bytes, offset + 16));
        std::size_t usedBytes = 0;
        std::size_t capacityBytes = 0;
        if (!plausibleCommandQueueVector(regions.regions, begin, end, capacity, usedBytes, capacityBytes))
          continue;

        const RuntimeMemoryRegion* bufferRegion =
          findWritableRegion(regions.regions, begin, std::max<std::size_t>(1, std::min<std::size_t>(capacityBytes, 64)));
        if (bufferRegion == nullptr)
          continue;

        CommandQueueCandidate candidate;
        candidate.vectorAddress = vectorAddress;
        candidate.bufferBegin = begin;
        candidate.bufferEnd = end;
        candidate.bufferCapacity = capacity;
        candidate.usedBytes = usedBytes;
        candidate.capacityBytes = capacityBytes;
        candidate.score = commandQueueCandidateScore(
          region,
          *bufferRegion,
          executablePath,
          hints,
          usedBytes,
          capacityBytes);
        candidate.regionPath = region.mappedPath;
        proof.candidates.push_back(std::move(candidate));
      }

      if (proof.candidates.size() >= maxCandidates || !proof.reason.empty())
        break;
    }

    std::sort(
      proof.candidates.begin(),
      proof.candidates.end(),
      [](const CommandQueueCandidate& lhs, const CommandQueueCandidate& rhs)
      {
        if (lhs.score != rhs.score)
          return lhs.score > rhs.score;
        if (lhs.capacityBytes != rhs.capacityBytes)
          return lhs.capacityBytes < rhs.capacityBytes;
        return lhs.vectorAddress < rhs.vectorAddress;
      });

    if (proof.candidates.size() > 32)
      proof.candidates.resize(32);

    proof.ready = !proof.candidates.empty();
    if (!proof.ready && proof.reason.empty())
      proof.reason = "no command-queue-like live vector candidates were found";
    return proof;
  }

  bool writeCommandQueueDiscoverySnapshot(
    const std::filesystem::path& path,
    const CommandQueueDiscoveryProof& proof,
    std::string& reason)
  {
    std::ofstream output(path);
    if (!output)
    {
      reason = "unable to open command queue discovery snapshot output";
      return false;
    }

    output << "rank\tscore\tvector_address\tbuffer_begin\tbuffer_end\tbuffer_capacity\tused_bytes\tcapacity_bytes\tregion_path\n";
    for (std::size_t i = 0; i < proof.candidates.size(); ++i)
    {
      const CommandQueueCandidate& candidate = proof.candidates[i];
      output << i << '\t'
             << candidate.score << '\t'
             << hexAddress(candidate.vectorAddress) << '\t'
             << hexAddress(candidate.bufferBegin) << '\t'
             << hexAddress(candidate.bufferEnd) << '\t'
             << hexAddress(candidate.bufferCapacity) << '\t'
             << candidate.usedBytes << '\t'
             << candidate.capacityBytes << '\t'
             << candidate.regionPath << '\n';
    }
    if (!output)
    {
      reason = "unable to write command queue discovery snapshot output";
      return false;
    }
    return true;
  }

  bool writeRemasteredUnitSnapshot(
    const std::filesystem::path& path,
    const std::vector<RemasteredUnitSnapshotRecord>& records,
    std::string& reason)
  {
    std::ofstream output(path);
    if (!output)
    {
      reason = "unable to open unit snapshot output";
      return false;
    }

    output << "index\tnode\tsecondary\tsprite\tid\tx\ty\ttarget_x\ttarget_y\torder\tstate\tplayer\ttype_hint\thit_points\n";
    for (const RemasteredUnitSnapshotRecord& record : records)
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
             << "unresolved\n";
    }

    if (!output)
    {
      reason = "unable to write unit snapshot output";
      return false;
    }
    return true;
  }

  DispatchEventsProof proveDispatchEventsFromLiveState(
    const LiveCounterProof& gameStateProof,
    const LiveUnitsProof& unitsProof,
    const LiveUnitNodeProof& nodeProof)
  {
    DispatchEventsProof proof;
    if (!gameStateProof.passed)
    {
      proof.reason = "dispatch-events proof requires a passing live game-state counter proof";
      return proof;
    }
    if (!unitsProof.passed)
    {
      proof.reason = "dispatch-events proof requires a passing live unit snapshot proof";
      return proof;
    }
    if (!unitsProof.derivedSnapshot || nodeProof.records.empty())
    {
      proof.reason = "dispatch-events proof requires a BWAPI-facing SC:R unit snapshot with stable unit handles";
      return proof;
    }
    if (gameStateProof.first == gameStateProof.second && gameStateProof.second == gameStateProof.third)
    {
      proof.reason = "dispatch-events proof requires frame progression";
      return proof;
    }

    std::unordered_set<std::uintptr_t> unitHandles;
    std::unordered_set<int> players;
    for (const RemasteredUnitSnapshotRecord& record : nodeProof.records)
    {
      if (record.nodeAddress == 0 || record.player < 0 || record.player > 11)
        continue;
      unitHandles.insert(record.nodeAddress);
      players.insert(record.player);
    }

    if (unitHandles.size() < minActiveUnitRecords)
    {
      proof.reason = "dispatch-events proof requires enough distinct live unit handles";
      return proof;
    }

    proof.passed = true;
    proof.frameEvents = 3;
    proof.unitDiscoverEvents = unitHandles.size();
    proof.unitUpdateEvents = nodeProof.records.size();
    proof.uniquePlayers = players.size();
    return proof;
  }

  bool writeDispatchEventsSnapshot(
    const std::filesystem::path& path,
    const LiveCounterProof& gameStateProof,
    const std::vector<RemasteredUnitSnapshotRecord>& records,
    std::string& reason)
  {
    std::ofstream output(path);
    if (!output)
    {
      reason = "unable to open event snapshot output";
      return false;
    }

    output << "event\tframe\tunit_id\tplayer\tx\ty\torder\ttype_hint\n";
    output << "onFrame\t" << gameStateProof.first << "\t\t\t\t\t\t\n";
    output << "onFrame\t" << gameStateProof.second << "\t\t\t\t\t\t\n";
    output << "onFrame\t" << gameStateProof.third << "\t\t\t\t\t\t\n";
    for (const RemasteredUnitSnapshotRecord& record : records)
    {
      output << "onUnitDiscover\t" << gameStateProof.second << '\t'
             << record.id << '\t'
             << record.player << '\t'
             << record.x << '\t'
             << record.y << '\t'
             << record.order << '\t'
             << record.typeHint << '\n';
      output << "onUnitUpdate\t" << gameStateProof.third << '\t'
             << record.id << '\t'
             << record.player << '\t'
             << record.x << '\t'
             << record.y << '\t'
             << record.order << '\t'
             << record.typeHint << '\n';
    }

    if (!output)
    {
      reason = "unable to write event snapshot output";
      return false;
    }
    return true;
  }

  bool writeMapDataSnapshot(
    const std::filesystem::path& path,
    const MapDataProof& proof,
    std::string& reason)
  {
    std::ofstream output(path);
    if (!output)
    {
      reason = "unable to open map snapshot output";
      return false;
    }

    output << "map_name\tmap_name_address\tmap_path\tmap_file_size\tsource\treplay_path\treplay_file_size\n";
    output << proof.mapName << '\t'
           << hexAddress(proof.mapNameAddress) << '\t'
           << proof.mapPath << '\t'
           << proof.mapFileSize << '\t'
           << proof.source << '\t'
           << proof.replayPath << '\t'
           << proof.replayFileSize << '\n';
    if (!output)
    {
      reason = "unable to write map snapshot output";
      return false;
    }
    return true;
  }

  bool writePlayerDataSnapshot(
    const std::filesystem::path& path,
    const PlayerDataProof& proof,
    std::string& reason)
  {
    std::ofstream output(path);
    if (!output)
    {
      reason = "unable to open player snapshot output";
      return false;
    }

    output << "player\tobserved_unit_count\n";
    for (const PlayerSnapshotRecord& record : proof.players)
      output << record.player << '\t' << record.unitCount << '\n';
    if (!output)
    {
      reason = "unable to write player snapshot output";
      return false;
    }
    return true;
  }

  bool writeReplayAnalysisSnapshot(
    const std::filesystem::path& path,
    const ReplayAnalysisProof& proof,
    std::string& reason)
  {
    std::ofstream output(path);
    if (!output)
    {
      reason = "unable to open replay analysis snapshot output";
      return false;
    }

    output << "map_name\tfirst_frame\tlast_frame\tobserved_player_count\n";
    output << proof.mapName << '\t'
           << proof.firstFrame << '\t'
           << proof.lastFrame << '\t'
           << proof.playerCount << '\n';
    if (!output)
    {
      reason = "unable to write replay analysis snapshot output";
      return false;
    }
    return true;
  }

  LiveUnitsProof proveExplicitUnitCandidateAddresses(
    int processId,
    const std::vector<std::uintptr_t>& candidateAddresses,
    std::size_t maxScanBytes,
    int scanTimeoutMs,
    UnitScanDiagnostics* diagnostics)
  {
    if (candidateAddresses.empty())
      return failedUnitsProof("no explicit CUnit candidate address was provided");

    RuntimeMemoryRegionListResult regions = listProcessMemoryRegions(processId);
    if (!regions.success)
      return failedUnitsProof(regions.reason);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(scanTimeoutMs);
    for (std::uintptr_t candidateAddress : candidateAddresses)
    {
      if (timedOut(deadline))
      {
        if (diagnostics != nullptr)
          diagnostics->timedOut = true;
        return failedUnitsProof(unitScanTimeoutReason(diagnostics));
      }

      const RuntimeMemoryRegion* containingRegion = nullptr;
      for (const RuntimeMemoryRegion& region : regions.regions)
      {
        if (region.readable && !region.executable && regionContains(region, candidateAddress, 336 * 4))
        {
          containingRegion = &region;
          break;
        }
      }
      if (containingRegion == nullptr)
        continue;

      if (diagnostics != nullptr)
      {
        if (containingRegion->writable)
          ++diagnostics->readableWritableRegions;
        else
          ++diagnostics->scannedReadableOnlyRegions;
      }

      for (const UnitRecordLayout& layout : unitRecordLayouts)
      {
        for (std::size_t recordSize : candidateUnitRecordSizes)
        {
          if (timedOut(deadline))
          {
            if (diagnostics != nullptr)
              diagnostics->timedOut = true;
            return failedUnitsProof(unitScanTimeoutReason(diagnostics));
          }

          constexpr std::size_t maxSampledRecords = 1700;
          const std::uintptr_t regionEnd = containingRegion->address + containingRegion->size;
          const std::size_t regionBytes =
            regionEnd > candidateAddress
              ? static_cast<std::size_t>(regionEnd - candidateAddress)
              : 0;
          const std::size_t bytesToRead = std::min({
            maxScanBytes,
            regionBytes,
            recordSize * maxSampledRecords
          });
          if (bytesToRead < recordSize * minActiveUnitRecords)
            continue;

          RuntimeMemoryReadResult read = readProcessMemory(processId, candidateAddress, bytesToRead);
          if (!read.success || read.bytesRead < recordSize * minActiveUnitRecords)
            continue;

          if (diagnostics != nullptr)
          {
            ++diagnostics->scannedRegions;
            diagnostics->scannedBytes += read.bytesRead;
          }

          bool scanTimedOut = false;
          LiveUnitsProof proof = scoreClassicCUnitArray(
            read.bytes,
            candidateAddress,
            0,
            recordSize,
            layout,
            regions.regions,
            deadline,
            scanTimedOut,
            true);
          if (scanTimedOut)
          {
            if (diagnostics != nullptr)
              diagnostics->timedOut = true;
            return failedUnitsProof(unitScanTimeoutReason(diagnostics));
          }
          if (diagnostics != nullptr)
          {
            ++diagnostics->candidateArraysScored;
            diagnostics->plausibleRecords += proof.activeRecords;
            if (proof.activeRecords > 0)
              ++diagnostics->stridedCandidates;
          }
          if (proof.passed)
            return proof;
          rememberBestCandidate(diagnostics, proof, &read.bytes, 0, recordSize);
        }
      }
    }

    return failedUnitsProof("no explicit CUnit candidate address contained enough active BWAPI-compatible records");
  }

  LiveCounterProof proveLiveCounterRead(
    int processId,
    const std::string& executablePath,
    int sampleDelayMs,
    std::size_t maxScanBytes,
    int scanTimeoutMs,
    StateScanDiagnostics* diagnostics)
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

    std::uintptr_t targetImageBase = 0;
    for (const RuntimeMemoryRegion& region : regions.regions)
    {
      if (!sameMappedFile(region.mappedPath, executablePath))
        continue;
      if (targetImageBase == 0 || region.address < targetImageBase)
        targetImageBase = region.address;
    }

    std::vector<RuntimeMemoryRegion> scanRegions = regions.regions;
    std::stable_sort(
      scanRegions.begin(),
      scanRegions.end(),
      [&](const RuntimeMemoryRegion& lhs, const RuntimeMemoryRegion& rhs)
      {
        const int lhsPriority = unitScanRegionPriority(lhs, executablePath, targetImageBase);
        const int rhsPriority = unitScanRegionPriority(rhs, executablePath, targetImageBase);
        if (lhsPriority != rhsPriority)
          return lhsPriority < rhsPriority;
        return lhs.address < rhs.address;
      });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(scanTimeoutMs);
    std::vector<Snapshot> snapshots;
    const std::size_t maxRegionBytes = 4 * 1024 * 1024;
    std::size_t scanned = 0;

    for (const RuntimeMemoryRegion& region : scanRegions)
    {
      if (timedOut(deadline))
      {
        if (diagnostics != nullptr)
          diagnostics->timedOut = true;
        return { false, 0, 0, 0, 0, "state counter scan timed out before proof" };
      }
      if (!region.readable)
      {
        if (diagnostics != nullptr)
          ++diagnostics->skippedNonReadableRegions;
        continue;
      }
      if (!region.writable)
      {
        if (diagnostics != nullptr)
          ++diagnostics->skippedNonWritableRegions;
        continue;
      }
      if (fileBackedNonTargetRegion(region, executablePath))
        continue;
      if (region.size < sizeof(std::uint32_t))
        continue;
      if (diagnostics != nullptr)
        ++diagnostics->readableWritableRegions;
      if (scanned >= maxScanBytes)
      {
        if (diagnostics != nullptr)
          diagnostics->byteLimitReached = true;
        break;
      }

      const std::size_t bytesToRead = std::min(region.size, std::min(maxRegionBytes, maxScanBytes - scanned));
      RuntimeMemoryReadResult first = readProcessMemory(processId, region.address, bytesToRead);
      if (!first.success || first.bytesRead < sizeof(std::uint32_t))
        continue;

      Snapshot snapshot;
      snapshot.address = region.address;
      snapshot.bytes = std::move(first.bytes);
      snapshots.push_back(std::move(snapshot));
      if (diagnostics != nullptr)
      {
        ++diagnostics->scannedRegions;
        diagnostics->scannedBytes += snapshots.back().bytes.size();
      }
      scanned += first.bytesRead;
    }

    if (snapshots.empty())
      return { false, 0, 0, 0, 0, "no readable writable runtime memory snapshots could be captured" };

    std::this_thread::sleep_for(std::chrono::milliseconds(sampleDelayMs));

    std::vector<Candidate> candidates;
    for (const Snapshot& snapshot : snapshots)
    {
      if (timedOut(deadline))
      {
        if (diagnostics != nullptr)
          diagnostics->timedOut = true;
        return { false, 0, 0, 0, 0, "state counter scan timed out before proof" };
      }
      RuntimeMemoryReadResult second = readProcessMemory(processId, snapshot.address, snapshot.bytes.size());
      if (!second.success || second.bytesRead != snapshot.bytes.size())
        continue;

      for (std::size_t offset = 0; offset + sizeof(std::uint32_t) <= snapshot.bytes.size(); offset += sizeof(std::uint32_t))
      {
        if ((offset % (4 * 1024)) == 0 && timedOut(deadline))
        {
          if (diagnostics != nullptr)
            diagnostics->timedOut = true;
          return { false, 0, 0, 0, 0, "state counter scan timed out before proof" };
        }
        const std::uint32_t firstValue = readU32(snapshot.bytes, offset);
        const std::uint32_t secondValue = readU32(second.bytes, offset);
        if (plausibleCounterDelta(firstValue, secondValue))
        {
          Candidate candidate;
          candidate.address = snapshot.address + offset;
          candidate.first = firstValue;
          candidate.second = secondValue;
          candidates.push_back(candidate);
          if (diagnostics != nullptr)
            ++diagnostics->candidateCounters;
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

    LiveCounterProof bestProof;
    int bestScore = std::numeric_limits<int>::max();
    for (const Candidate& candidate : candidates)
    {
      if (timedOut(deadline))
      {
        if (diagnostics != nullptr)
          diagnostics->timedOut = true;
        return { false, 0, 0, 0, 0, "state counter scan timed out before proof" };
      }
      RuntimeMemoryReadResult third = readProcessMemory(processId, candidate.address, sizeof(std::uint32_t));
      if (!third.success || third.bytesRead != sizeof(std::uint32_t))
        continue;

      const std::uint32_t thirdValue = readU32(third.bytes, 0);
      if (frameCounterConfidencePassed(candidate.first, candidate.second, thirdValue, sampleDelayMs))
      {
        const int score =
          frameCounterScore(candidate.first, candidate.second, thirdValue, sampleDelayMs);
        if (!bestProof.passed || score < bestScore)
        {
          bestProof = { true, candidate.address, candidate.first, candidate.second, thirdValue, {} };
          bestScore = score;
        }
      }
    }

    if (bestProof.passed)
      return bestProof;

    return { false, 0, 0, 0, 0, "candidate counters did not pass frame-counter confidence checks" };
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

  void initializeSelfCommandQueueFixture(SelfCommandQueueFixture& fixture)
  {
    fixture.buffer.fill(0);
    fixture.buffer[0] = 0x14;
    fixture.buffer[1] = 0x00;
    fixture.buffer[2] = 0x34;
    fixture.buffer[3] = 0x12;
    fixture.begin = reinterpret_cast<std::uintptr_t>(fixture.buffer.data());
    fixture.end = fixture.begin + 4;
    fixture.capacity = fixture.begin + fixture.buffer.size();
  }

  void keepSelfCommandQueueFixtureAlive(const SelfCommandQueueFixture& fixture)
  {
    if (fixture.begin == 0 || fixture.end < fixture.begin || fixture.capacity <= fixture.begin)
      std::cerr << "invalid self command queue fixture\n";
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
  bool proveDispatchEvents = false;
  bool proveReadMapData = false;
  bool proveReadPlayerData = false;
  bool proveReplayAnalysis = false;
  bool proveBattleNetPolicyFlag = false;
  bool discoverCommandQueue = false;
  bool selfCommandQueueFixture = false;
  bool unitScanDiagnosticsFlag = false;
  bool unitScanReadableOnlyFlag = false;
  bool unitScanVectorsFlag = false;
  bool unitScanIncludeImageRegionsFlag = false;
  bool stateScanDiagnosticsFlag = false;
  std::string unitBestDumpOut;
  std::vector<std::uintptr_t> unitCandidateAddresses;
  int stateSampleDelayMs = 250;
  std::size_t stateMaxScanBytes = 128 * 1024 * 1024;
  int stateScanTimeoutMs = 30000;
  std::size_t unitMaxScanBytes = 0;
  int unitScanTimeoutMs = 15000;
  int activeMatchWaitMs = 0;
  int activeMatchPollMs = 1000;

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
    if (arg == "--prove-dispatch-events")
    {
      proveDispatchEvents = true;
      continue;
    }
    if (arg == "--prove-read-map-data")
    {
      proveReadMapData = true;
      continue;
    }
    if (arg == "--prove-read-player-data")
    {
      proveReadPlayerData = true;
      continue;
    }
    if (arg == "--prove-replay-analysis")
    {
      proveReplayAnalysis = true;
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
    if (arg == "--unit-scan-include-image-regions")
    {
      unitScanIncludeImageRegionsFlag = true;
      continue;
    }
    if (arg == "--state-scan-diagnostics")
    {
      stateScanDiagnosticsFlag = true;
      continue;
    }
    if (arg == "--unit-candidate-address")
    {
      std::uintptr_t address = 0;
      if (i + 1 >= argc || !parseAddress(argv[++i], address))
      {
        std::cerr << "--unit-candidate-address requires a positive integer address\n";
        return 64;
      }
      unitCandidateAddresses.push_back(address);
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
    if (arg == "--discover-command-queue")
    {
      discoverCommandQueue = true;
      continue;
    }
    if (arg == "--self-command-queue-fixture")
    {
      selfCommandQueueFixture = true;
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
    if (arg == "--active-match-wait-ms")
    {
      if (i + 1 >= argc || !parsePositiveInt(argv[++i], activeMatchWaitMs))
      {
        std::cerr << "--active-match-wait-ms requires a positive integer\n";
        return 64;
      }
      continue;
    }
    if (arg == "--active-match-poll-ms")
    {
      if (i + 1 >= argc || !parsePositiveInt(argv[++i], activeMatchPollMs))
      {
        std::cerr << "--active-match-poll-ms requires a positive integer\n";
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
  if (unitMaxScanBytes == 0)
    unitMaxScanBytes = stateMaxScanBytes;
  if (proveDispatchEvents)
  {
    proveReadGameState = true;
    proveActiveMatchState = true;
    proveReadUnits = true;
  }
  if (proveReadPlayerData)
  {
    proveActiveMatchState = true;
    proveReadUnits = true;
  }
  if (proveActiveMatchState && !self)
    proveReadGameState = true;
  if (proveReplayAnalysis)
  {
    proveReadGameState = true;
    proveReadMapData = true;
    proveReadPlayerData = true;
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
  LiveUnitNodeProof activeUnitNodeProof;
  DispatchEventsProof dispatchEventsProof;
  MapDataProof mapDataProof;
  PlayerDataProof playerDataProof;
  ReplayAnalysisProof replayAnalysisProof;
  StateScanDiagnostics stateScanDiagnostics;
  UnitScanDiagnostics unitScanDiagnostics;
  BattleNetPolicyProof battleNetPolicyProof;
  CommandQueueDiscoveryProof commandQueueDiscoveryProof;
  bool unitSnapshotWritten = false;
  bool dispatchEventsSnapshotWritten = false;
  bool mapDataSnapshotWritten = false;
  bool playerDataSnapshotWritten = false;
  bool replayAnalysisSnapshotWritten = false;
  bool commandQueueDiscoverySnapshotWritten = false;
  SelfUnitFixture unitFixture;
  SelfCommandQueueFixture commandQueueFixture;
  int proofFailureCode = 0;
  if (self && selfUnitFixture)
    unitFixture = makeSelfUnitFixture();
  if (self && selfCommandQueueFixture)
  {
    initializeSelfCommandQueueFixture(commandQueueFixture);
    keepSelfCommandQueueFixtureAlive(commandQueueFixture);
  }

  if (proveReadGameState)
  {
    readGameStateProof = proveLiveCounterRead(
      environment.processId,
      environment.executablePath,
      stateSampleDelayMs,
      stateMaxScanBytes,
      stateScanTimeoutMs,
      stateScanDiagnosticsFlag ? &stateScanDiagnostics : nullptr);
    std::cout << "read_game_state.live_counter=" << (readGameStateProof.passed ? "true" : "false") << '\n';
    if (readGameStateProof.passed)
    {
      std::cout << "read_game_state.address=0x" << std::hex << readGameStateProof.address << std::dec << '\n';
      std::cout << "read_game_state.sample.0=" << readGameStateProof.first << '\n';
      std::cout << "read_game_state.sample.1=" << readGameStateProof.second << '\n';
      std::cout << "read_game_state.sample.2=" << readGameStateProof.third << '\n';
      std::cout << "read_game_state.delta.0="
                << (readGameStateProof.second - readGameStateProof.first) << '\n';
      std::cout << "read_game_state.delta.1="
                << (readGameStateProof.third - readGameStateProof.second) << '\n';
      std::cout << "read_game_state.confidence=frame-like\n";
    }
    if (!readGameStateProof.reason.empty())
      std::cout << "read_game_state.reason=" << readGameStateProof.reason << '\n';
    if (stateScanDiagnosticsFlag)
    {
      std::cout << "read_game_state.scan.readable_writable_regions="
                << stateScanDiagnostics.readableWritableRegions << '\n';
      std::cout << "read_game_state.scan.skipped_non_readable_regions="
                << stateScanDiagnostics.skippedNonReadableRegions << '\n';
      std::cout << "read_game_state.scan.skipped_non_writable_regions="
                << stateScanDiagnostics.skippedNonWritableRegions << '\n';
      std::cout << "read_game_state.scan.scanned_regions="
                << stateScanDiagnostics.scannedRegions << '\n';
      std::cout << "read_game_state.scan.scanned_bytes="
                << stateScanDiagnostics.scannedBytes << '\n';
      std::cout << "read_game_state.scan.candidate_counters="
                << stateScanDiagnostics.candidateCounters << '\n';
      std::cout << "read_game_state.scan.timed_out="
                << (stateScanDiagnostics.timedOut ? "true" : "false") << '\n';
      std::cout << "read_game_state.scan.byte_limit_reached="
                << (stateScanDiagnostics.byteLimitReached ? "true" : "false") << '\n';
    }
    if (!readGameStateProof.passed)
      proofFailureCode = proofFailureCode == 0 ? 4 : proofFailureCode;
  }

  if (proveActiveMatchState && self)
  {
    std::cout << "active_match_state.in_game=false\n";
    std::cout << "active_match_state.reason=self process and self fixtures cannot prove StarCraft active match state\n";
    proofFailureCode = proofFailureCode == 0 ? 7 : proofFailureCode;
  }

  if (proveReadMapData)
  {
    RuntimeInstallation installation = detectStarCraftInstallation(environment);
    mapDataProof = proveMapData(
      environment.processId,
      environment.executablePath,
      installation.installRoot,
      stateMaxScanBytes,
      stateScanTimeoutMs);
    std::cout << "read_map_data.ready=" << (mapDataProof.passed ? "true" : "false") << '\n';
    if (mapDataProof.passed)
    {
      std::cout << "read_map_data.map_name=" << mapDataProof.mapName << '\n';
      if (mapDataProof.mapNameAddress != 0)
        std::cout << "read_map_data.map_name_address=0x"
                  << std::hex << mapDataProof.mapNameAddress << std::dec << '\n';
      std::cout << "read_map_data.map_path=" << mapDataProof.mapPath << '\n';
      std::cout << "read_map_data.map_file_size=" << mapDataProof.mapFileSize << '\n';
      if (!mapDataProof.source.empty())
        std::cout << "read_map_data.source=" << mapDataProof.source << '\n';
      if (!mapDataProof.replayPath.empty())
      {
        std::cout << "read_map_data.replay_path=" << mapDataProof.replayPath << '\n';
        std::cout << "read_map_data.replay_file_size=" << mapDataProof.replayFileSize << '\n';
      }
    }
    if (!mapDataProof.reason.empty())
      std::cout << "read_map_data.reason=" << mapDataProof.reason << '\n';
    if (!mapDataProof.passed)
      proofFailureCode = proofFailureCode == 0 ? 9 : proofFailureCode;
  }

  if (proveReadUnits || (proveActiveMatchState && !self))
  {
    int unitScanAttempts = 0;
    const auto activeMatchDeadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(activeMatchWaitMs);
    while (true)
    {
      ++unitScanAttempts;
      unitScanDiagnostics = {};
      if (proveActiveMatchState && !self)
      {
        activeUnitNodeProof = proveLiveUnitNodeAnchors(
          environment.processId,
          environment.executablePath,
          unitMaxScanBytes,
          unitScanTimeoutMs);
      }
      if (proveReadUnits && !unitCandidateAddresses.empty())
      {
        readUnitsProof = proveExplicitUnitCandidateAddresses(
          environment.processId,
          unitCandidateAddresses,
          unitMaxScanBytes,
          unitScanTimeoutMs,
          unitScanDiagnosticsFlag ? &unitScanDiagnostics : nullptr);
      }
      if (proveReadUnits
          && !readUnitsProof.passed
          && !self
          && environment.product == Product::StarCraftRemastered)
      {
        readUnitsProof = proveRemasteredUnitNodeSnapshot(
          environment.processId,
          environment.executablePath,
          unitMaxScanBytes,
          unitScanTimeoutMs,
          &activeUnitNodeProof);
      }
      if (proveReadUnits && !readUnitsProof.passed)
      {
        readUnitsProof = proveLiveUnitsRead(
          environment.processId,
          environment.executablePath,
          unitMaxScanBytes,
          unitScanTimeoutMs,
          unitScanReadableOnlyFlag,
          unitScanIncludeImageRegionsFlag,
          unitScanVectorsFlag,
          unitScanDiagnosticsFlag ? &unitScanDiagnostics : nullptr);
      }
      if (readUnitsProof.passed
          || activeUnitNodeProof.passed
          || activeMatchWaitMs <= 0
          || std::chrono::steady_clock::now() >= activeMatchDeadline)
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(activeMatchPollMs));
    }
    if (activeMatchWaitMs > 0)
    {
      std::cout << "active_match_state.wait_ms=" << activeMatchWaitMs << '\n';
      std::cout << "active_match_state.poll_ms=" << activeMatchPollMs << '\n';
      std::cout << "active_match_state.scan_attempts=" << unitScanAttempts << '\n';
    }
    if (proveReadUnits)
    {
      if (!unitCandidateAddresses.empty())
        std::cout << "read_units.candidate_address.count=" << unitCandidateAddresses.size() << '\n';
      std::cout << "read_units.unit_array=" << (readUnitsProof.passed ? "true" : "false") << '\n';
      if (readUnitsProof.passed)
      {
        std::cout << "read_units.address=0x" << std::hex << readUnitsProof.address << std::dec << '\n';
        std::cout << "read_units.record_size=" << readUnitsProof.recordSize << '\n';
        std::cout << "read_units.layout=" << readUnitsProof.layoutName << '\n';
        std::cout << "read_units.pointer_array=" << (readUnitsProof.pointerArray ? "true" : "false") << '\n';
        std::cout << "read_units.derived_snapshot="
                  << (readUnitsProof.derivedSnapshot ? "true" : "false") << '\n';
        std::cout << "read_units.hit_points_resolved="
                  << (readUnitsProof.hitPointsResolved ? "true" : "false") << '\n';
        std::cout << "read_units.sampled_records=" << readUnitsProof.sampledRecords << '\n';
        std::cout << "read_units.active_records=" << readUnitsProof.activeRecords << '\n';
      }
      if (!readUnitsProof.reason.empty())
        std::cout << "read_units.reason=" << readUnitsProof.reason << '\n';
    }

    if (proveActiveMatchState)
    {
      const bool frameGatePassed = !proveReadGameState || readGameStateProof.passed;
      const bool activeMatchProven =
        !self && frameGatePassed && (readUnitsProof.passed || activeUnitNodeProof.passed);
      const char* activeMatchEvidence =
        readUnitsProof.passed
          ? (readUnitsProof.derivedSnapshot ? "active-unit-node-snapshot" : "active-unit-records")
          : "active-unit-node-anchor";
      std::cout << "active_match_state.in_game=" << (activeMatchProven ? "true" : "false") << '\n';
      std::cout << "active_match_state.evidence=" << activeMatchEvidence << '\n';
      if (!frameGatePassed)
        std::cout << "active_match_state.reason=active match proof requires live frame progression\n";
      if (readUnitsProof.passed)
      {
        std::cout << "active_match_state.active_records=" << readUnitsProof.activeRecords << '\n';
        std::cout << "active_match_state.unit_array_address=0x"
                  << std::hex << readUnitsProof.address << std::dec << '\n';
      }
      else if (activeUnitNodeProof.passed)
      {
        std::cout << "active_match_state.active_records=" << activeUnitNodeProof.activeRecords << '\n';
        std::cout << "active_match_state.unit_node_address=0x"
                  << std::hex << activeUnitNodeProof.address << std::dec << '\n';
        if (activeUnitNodeProof.vectorAddress != 0)
          std::cout << "active_match_state.unit_node_vector_address=0x"
                    << std::hex << activeUnitNodeProof.vectorAddress << std::dec << '\n';
        std::cout << "active_match_state.unit_node_record_size="
                  << activeUnitNodeProof.recordSize << '\n';
      }
      if (!readUnitsProof.reason.empty() && !activeUnitNodeProof.passed)
        std::cout << "active_match_state.reason=" << readUnitsProof.reason << '\n';
      if (!activeUnitNodeProof.reason.empty() && !activeUnitNodeProof.passed)
        std::cout << "active_match_state.unit_node_reason=" << activeUnitNodeProof.reason << '\n';
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
      std::cout << "read_units.scan.image_mapped_regions="
                << unitScanDiagnostics.imageMappedRegions << '\n';
      std::cout << "read_units.scan.skipped_image_mapped_regions="
                << unitScanDiagnostics.skippedImageMappedRegions << '\n';
      std::cout << "read_units.scan.scanned_regions=" << unitScanDiagnostics.scannedRegions << '\n';
      std::cout << "read_units.scan.scanned_bytes=" << unitScanDiagnostics.scannedBytes << '\n';
      std::cout << "read_units.scan.vector_candidates=" << unitScanDiagnostics.vectorCandidates << '\n';
      std::cout << "read_units.scan.vector_duplicate_begins="
                << unitScanDiagnostics.vectorDuplicateBegins << '\n';
      std::cout << "read_units.scan.vector_rejected_target_regions="
                << unitScanDiagnostics.vectorRejectedTargetRegions << '\n';
      std::cout << "read_units.scan.pointer_array_candidates="
                << unitScanDiagnostics.pointerArrayCandidates << '\n';
      std::cout << "read_units.scan.pointer_arrays_scored="
                << unitScanDiagnostics.pointerArraysScored << '\n';
      std::cout << "read_units.scan.pointer_array_readable_pointer_hits="
                << unitScanDiagnostics.pointerArrayReadablePointerHits << '\n';
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
    if (!readUnitsProof.passed && !(proveActiveMatchState && activeUnitNodeProof.passed && !proveReadUnits))
      proofFailureCode = proofFailureCode == 0 ? (proveReadUnits ? 6 : 7) : proofFailureCode;
  }

  if (proveReadPlayerData)
  {
    playerDataProof = provePlayerDataFromUnitSnapshot(activeUnitNodeProof);
    std::cout << "read_player_data.ready=" << (playerDataProof.passed ? "true" : "false") << '\n';
    if (playerDataProof.passed)
    {
      std::cout << "read_player_data.player_count=" << playerDataProof.playerCount << '\n';
      std::cout << "read_player_data.observed_units=" << playerDataProof.observedUnits << '\n';
    }
    if (!playerDataProof.reason.empty())
      std::cout << "read_player_data.reason=" << playerDataProof.reason << '\n';
    if (!playerDataProof.passed)
      proofFailureCode = proofFailureCode == 0 ? 10 : proofFailureCode;
  }

  if (proveReplayAnalysis)
  {
    replayAnalysisProof = proveReplayAnalysisFromLiveMetadata(
      readGameStateProof,
      mapDataProof,
      playerDataProof);
    std::cout << "replay_analysis.ready=" << (replayAnalysisProof.passed ? "true" : "false") << '\n';
    if (replayAnalysisProof.passed)
    {
      std::cout << "replay_analysis.map_name=" << replayAnalysisProof.mapName << '\n';
      std::cout << "replay_analysis.first_frame=" << replayAnalysisProof.firstFrame << '\n';
      std::cout << "replay_analysis.last_frame=" << replayAnalysisProof.lastFrame << '\n';
      std::cout << "replay_analysis.player_count=" << replayAnalysisProof.playerCount << '\n';
    }
    if (!replayAnalysisProof.reason.empty())
      std::cout << "replay_analysis.reason=" << replayAnalysisProof.reason << '\n';
    if (!replayAnalysisProof.passed)
      proofFailureCode = proofFailureCode == 0 ? 11 : proofFailureCode;
  }

  if (proveDispatchEvents)
  {
    dispatchEventsProof = proveDispatchEventsFromLiveState(
      readGameStateProof,
      readUnitsProof,
      activeUnitNodeProof);
    std::cout << "dispatch_events.ready=" << (dispatchEventsProof.passed ? "true" : "false") << '\n';
    if (dispatchEventsProof.passed)
    {
      std::cout << "dispatch_events.frame_events=" << dispatchEventsProof.frameEvents << '\n';
      std::cout << "dispatch_events.unit_discover_events="
                << dispatchEventsProof.unitDiscoverEvents << '\n';
      std::cout << "dispatch_events.unit_update_events="
                << dispatchEventsProof.unitUpdateEvents << '\n';
      std::cout << "dispatch_events.unique_players=" << dispatchEventsProof.uniquePlayers << '\n';
    }
    if (!dispatchEventsProof.reason.empty())
      std::cout << "dispatch_events.reason=" << dispatchEventsProof.reason << '\n';
    if (!dispatchEventsProof.passed)
      proofFailureCode = proofFailureCode == 0 ? 8 : proofFailureCode;
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

  if (discoverCommandQueue)
  {
    commandQueueDiscoveryProof = discoverCommandQueueCandidates(
      environment.processId,
      environment.executablePath,
      unitMaxScanBytes,
      unitScanTimeoutMs);
    std::cout << "command_queue_discovery.ready="
              << (commandQueueDiscoveryProof.ready ? "true" : "false") << '\n';
    std::cout << "command_queue_discovery.scanned_regions="
              << commandQueueDiscoveryProof.scannedRegions << '\n';
    std::cout << "command_queue_discovery.scanned_bytes="
              << commandQueueDiscoveryProof.scannedBytes << '\n';
    std::cout << "command_queue_discovery.candidate_count="
              << commandQueueDiscoveryProof.candidates.size() << '\n';
    if (!commandQueueDiscoveryProof.candidates.empty())
    {
      const CommandQueueCandidate& best = commandQueueDiscoveryProof.candidates.front();
      std::cout << "command_queue_discovery.best.vector_address="
                << hexAddress(best.vectorAddress) << '\n';
      std::cout << "command_queue_discovery.best.buffer_begin="
                << hexAddress(best.bufferBegin) << '\n';
      std::cout << "command_queue_discovery.best.used_bytes="
                << best.usedBytes << '\n';
      std::cout << "command_queue_discovery.best.capacity_bytes="
                << best.capacityBytes << '\n';
      std::cout << "command_queue_discovery.best.score="
                << best.score << '\n';
    }
    if (!commandQueueDiscoveryProof.reason.empty())
      std::cout << "command_queue_discovery.reason=" << commandQueueDiscoveryProof.reason << '\n';
    std::cout << "command_queue_discovery.proof_scope=discovery-only-not-command-behavior\n";
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

  const RuntimeExecutorBehaviorProof* dispatchEventsBehaviorProof = findProof("dispatch-events");
  if (proveDispatchEvents && dispatchEventsBehaviorProof == nullptr)
  {
    std::cerr << "dispatch-events proof definition is missing\n";
    return 1;
  }

  const RuntimeExecutorBehaviorProof* replayAnalysisBehaviorProof = findProof("replay-analysis");
  if (proveReplayAnalysis && replayAnalysisBehaviorProof == nullptr)
  {
    std::cerr << "replay-analysis proof definition is missing\n";
    return 1;
  }

  const RuntimeExecutorBehaviorProof* battleNetPolicyBehaviorProof = findProof("battle-net-policy");
  if (proveBattleNetPolicyFlag && battleNetPolicyBehaviorProof == nullptr)
  {
    std::cerr << "battle-net-policy proof definition is missing\n";
    return 1;
  }

  const std::filesystem::path unitSnapshotPath =
    std::filesystem::path(environment.executorBridgePath) / "units.snapshot.tsv";
  if (proveReadUnits && readUnitsProof.passed && readUnitsProof.derivedSnapshot)
  {
    std::string snapshotReason;
    const bool snapshotWritten = writeRemasteredUnitSnapshot(
      unitSnapshotPath,
      activeUnitNodeProof.records,
      snapshotReason);
    unitSnapshotWritten = snapshotWritten;
    std::cout << "read_units.snapshot.success=" << (snapshotWritten ? "true" : "false") << '\n';
    if (snapshotWritten)
      std::cout << "read_units.snapshot.path=" << unitSnapshotPath.string() << '\n';
    if (!snapshotReason.empty())
      std::cout << "read_units.snapshot.reason=" << snapshotReason << '\n';
    if (!snapshotWritten)
      proofFailureCode = proofFailureCode == 0 ? 6 : proofFailureCode;
  }

  const std::filesystem::path dispatchEventsPath =
    std::filesystem::path(environment.executorBridgePath) / "events.snapshot.tsv";
  if (proveDispatchEvents && dispatchEventsProof.passed)
  {
    std::string eventsReason;
    const bool eventsWritten = writeDispatchEventsSnapshot(
      dispatchEventsPath,
      readGameStateProof,
      activeUnitNodeProof.records,
      eventsReason);
    dispatchEventsSnapshotWritten = eventsWritten;
    std::cout << "dispatch_events.snapshot.success=" << (eventsWritten ? "true" : "false") << '\n';
    if (eventsWritten)
      std::cout << "dispatch_events.snapshot.path=" << dispatchEventsPath.string() << '\n';
    if (!eventsReason.empty())
      std::cout << "dispatch_events.snapshot.reason=" << eventsReason << '\n';
    if (!eventsWritten)
      proofFailureCode = proofFailureCode == 0 ? 8 : proofFailureCode;
  }

  const std::filesystem::path mapDataPath =
    std::filesystem::path(environment.executorBridgePath) / "map.snapshot.tsv";
  if (proveReadMapData && mapDataProof.passed)
  {
    std::string mapReason;
    const bool mapWritten = writeMapDataSnapshot(mapDataPath, mapDataProof, mapReason);
    mapDataSnapshotWritten = mapWritten;
    std::cout << "read_map_data.snapshot.success=" << (mapWritten ? "true" : "false") << '\n';
    if (mapWritten)
      std::cout << "read_map_data.snapshot.path=" << mapDataPath.string() << '\n';
    if (!mapReason.empty())
      std::cout << "read_map_data.snapshot.reason=" << mapReason << '\n';
    if (!mapWritten)
      proofFailureCode = proofFailureCode == 0 ? 9 : proofFailureCode;
  }

  const std::filesystem::path playerDataPath =
    std::filesystem::path(environment.executorBridgePath) / "players.snapshot.tsv";
  if (proveReadPlayerData && playerDataProof.passed)
  {
    std::string playerReason;
    const bool playerWritten = writePlayerDataSnapshot(playerDataPath, playerDataProof, playerReason);
    playerDataSnapshotWritten = playerWritten;
    std::cout << "read_player_data.snapshot.success=" << (playerWritten ? "true" : "false") << '\n';
    if (playerWritten)
      std::cout << "read_player_data.snapshot.path=" << playerDataPath.string() << '\n';
    if (!playerReason.empty())
      std::cout << "read_player_data.snapshot.reason=" << playerReason << '\n';
    if (!playerWritten)
      proofFailureCode = proofFailureCode == 0 ? 10 : proofFailureCode;
  }

  const std::filesystem::path replayAnalysisPath =
    std::filesystem::path(environment.executorBridgePath) / "replay.snapshot.tsv";
  if (proveReplayAnalysis && replayAnalysisProof.passed)
  {
    std::string replayReason;
    const bool replayWritten =
      writeReplayAnalysisSnapshot(replayAnalysisPath, replayAnalysisProof, replayReason);
    replayAnalysisSnapshotWritten = replayWritten;
    std::cout << "replay_analysis.snapshot.success=" << (replayWritten ? "true" : "false") << '\n';
    if (replayWritten)
      std::cout << "replay_analysis.snapshot.path=" << replayAnalysisPath.string() << '\n';
    if (!replayReason.empty())
      std::cout << "replay_analysis.snapshot.reason=" << replayReason << '\n';
    if (!replayWritten)
      proofFailureCode = proofFailureCode == 0 ? 11 : proofFailureCode;
  }

  const std::filesystem::path commandQueueDiscoveryPath =
    std::filesystem::path(environment.executorBridgePath) / "command_queue.candidates.tsv";
  if (discoverCommandQueue && commandQueueDiscoveryProof.ready)
  {
    std::string commandQueueReason;
    const bool commandQueueWritten = writeCommandQueueDiscoverySnapshot(
      commandQueueDiscoveryPath,
      commandQueueDiscoveryProof,
      commandQueueReason);
    commandQueueDiscoverySnapshotWritten = commandQueueWritten;
    std::cout << "command_queue_discovery.snapshot.success="
              << (commandQueueWritten ? "true" : "false") << '\n';
    if (commandQueueWritten)
      std::cout << "command_queue_discovery.snapshot.path="
                << commandQueueDiscoveryPath.string() << '\n';
    if (!commandQueueReason.empty())
      std::cout << "command_queue_discovery.snapshot.reason=" << commandQueueReason << '\n';
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
    ready << "proof.read_game_state.delta="
          << (readGameStateProof.second - readGameStateProof.first) << ','
          << (readGameStateProof.third - readGameStateProof.second) << '\n';
    ready << "proof.read_game_state.confidence=frame-like\n";
    ready << readGameStateBehaviorProof->readyFileLine << '\n';
  }
  const bool activeMatchReady =
    proveActiveMatchState
    && !self
    && (!proveReadGameState || readGameStateProof.passed)
    && (readUnitsProof.passed || activeUnitNodeProof.passed);
  if (activeMatchReady)
  {
    const char* activeMatchEvidence =
      readUnitsProof.passed
        ? (readUnitsProof.derivedSnapshot ? "active-unit-node-snapshot" : "active-unit-records")
        : "active-unit-node-anchor";
    ready << "proof.active_match_state.evidence="
          << activeMatchEvidence << '\n';
    ready << "proof.active_match_state.active_records="
          << (readUnitsProof.passed ? readUnitsProof.activeRecords : activeUnitNodeProof.activeRecords)
          << '\n';
    if (readUnitsProof.passed)
    {
      ready << "proof.active_match_state.unit_array_address=0x"
            << std::hex << readUnitsProof.address << std::dec << '\n';
    }
    else
    {
      ready << "proof.active_match_state.unit_node_address=0x"
            << std::hex << activeUnitNodeProof.address << std::dec << '\n';
      if (activeUnitNodeProof.vectorAddress != 0)
        ready << "proof.active_match_state.unit_node_vector_address=0x"
              << std::hex << activeUnitNodeProof.vectorAddress << std::dec << '\n';
      ready << "proof.active_match_state.unit_node_record_size="
            << activeUnitNodeProof.recordSize << '\n';
      ready << "contract.binding.BW::BWDATA::UnitNodeTable=data-address|proof.active_match_state=passed\n";
    }
    ready << activeMatchStateBehaviorProof->readyFileLine << '\n';
  }
  const bool readUnitsReady =
    proveReadUnits
    && readUnitsProof.passed
    && (!readUnitsProof.derivedSnapshot || unitSnapshotWritten)
    && (!proveActiveMatchState || activeMatchReady);
  if (readUnitsReady)
  {
    ready << "proof.read_units.address=0x" << std::hex << readUnitsProof.address << std::dec << '\n';
    ready << "proof.read_units.record_size=" << readUnitsProof.recordSize << '\n';
    ready << "proof.read_units.layout=" << readUnitsProof.layoutName << '\n';
    ready << "proof.read_units.pointer_array=" << (readUnitsProof.pointerArray ? "true" : "false") << '\n';
    ready << "proof.read_units.derived_snapshot=" << (readUnitsProof.derivedSnapshot ? "true" : "false") << '\n';
    ready << "proof.read_units.hit_points_resolved=" << (readUnitsProof.hitPointsResolved ? "true" : "false") << '\n';
    ready << "proof.read_units.active_records=" << readUnitsProof.activeRecords << '\n';
    ready << "contract.binding.BW::BWDATA::UnitNodeTable=data-address|proof.read_units=passed\n";
    if (readUnitsProof.derivedSnapshot)
    {
      ready << "proof.read_units.snapshot=units.snapshot.tsv\n";
      ready << "proof.read_units.id_source=secondary+24\n";
      ready << "proof.read_units.position_source=unit-node+36|4\n";
      ready << "proof.read_units.order_source=unit-node+48|2\n";
      ready << "proof.read_units.player_source=secondary+20|1\n";
    }
    else
    {
      ready << "contract.structure.BW::CUnit=" << readUnitsProof.recordSize << "|proof.read_units=passed\n";
      ready << "contract.field.BW::CUnit.id=" << readUnitsProof.idOffset << "|2|proof.read_units=passed\n";
      ready << "contract.field.BW::CUnit.position=" << readUnitsProof.positionOffset << "|4|proof.read_units=passed\n";
      ready << "contract.field.BW::CUnit.hitPoints=" << readUnitsProof.hitPointsOffset << "|4|proof.read_units=passed\n";
      ready << "contract.field.BW::CUnit.order=" << readUnitsProof.orderOffset << "|1|proof.read_units=passed\n";
      ready << "contract.field.BW::CUnit.player=" << readUnitsProof.playerOffset << "|1|proof.read_units=passed\n";
    }
    ready << readUnitsBehaviorProof->readyFileLine << '\n';
  }
  if (proveDispatchEvents && dispatchEventsProof.passed && dispatchEventsSnapshotWritten && activeMatchReady)
  {
    ready << "proof.dispatch_events.frame_events=" << dispatchEventsProof.frameEvents << '\n';
    ready << "proof.dispatch_events.unit_discover_events="
          << dispatchEventsProof.unitDiscoverEvents << '\n';
    ready << "proof.dispatch_events.unit_update_events="
          << dispatchEventsProof.unitUpdateEvents << '\n';
    ready << "proof.dispatch_events.unique_players=" << dispatchEventsProof.uniquePlayers << '\n';
    ready << "proof.dispatch_events.snapshot=events.snapshot.tsv\n";
    ready << dispatchEventsBehaviorProof->readyFileLine << '\n';
  }
  if (proveReadMapData && mapDataProof.passed && mapDataSnapshotWritten)
  {
    ready << "proof.read_map_data.map_name=" << mapDataProof.mapName << '\n';
    if (mapDataProof.mapNameAddress != 0)
      ready << "proof.read_map_data.map_name_address=0x"
            << std::hex << mapDataProof.mapNameAddress << std::dec << '\n';
    ready << "proof.read_map_data.map_path=" << mapDataProof.mapPath << '\n';
    ready << "proof.read_map_data.map_file_size=" << mapDataProof.mapFileSize << '\n';
    if (!mapDataProof.source.empty())
      ready << "proof.read_map_data.source=" << mapDataProof.source << '\n';
    if (!mapDataProof.replayPath.empty())
    {
      ready << "proof.read_map_data.replay_path=" << mapDataProof.replayPath << '\n';
      ready << "proof.read_map_data.replay_file_size=" << mapDataProof.replayFileSize << '\n';
    }
    ready << "proof.read_map_data.snapshot=map.snapshot.tsv\n";
    ready << "proof.read_map_data=passed\n";
  }
  if (proveReadPlayerData && playerDataProof.passed && playerDataSnapshotWritten && activeMatchReady)
  {
    ready << "proof.read_player_data.player_count=" << playerDataProof.playerCount << '\n';
    ready << "proof.read_player_data.observed_units=" << playerDataProof.observedUnits << '\n';
    ready << "proof.read_player_data.snapshot=players.snapshot.tsv\n";
    ready << "proof.read_player_data=passed\n";
  }
  if (proveReplayAnalysis && replayAnalysisProof.passed && replayAnalysisSnapshotWritten)
  {
    ready << "proof.replay_analysis.map_name=" << replayAnalysisProof.mapName << '\n';
    ready << "proof.replay_analysis.first_frame=" << replayAnalysisProof.firstFrame << '\n';
    ready << "proof.replay_analysis.last_frame=" << replayAnalysisProof.lastFrame << '\n';
    ready << "proof.replay_analysis.player_count=" << replayAnalysisProof.playerCount << '\n';
    ready << "proof.replay_analysis.snapshot=replay.snapshot.tsv\n";
    ready << replayAnalysisBehaviorProof->readyFileLine << '\n';
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
  if (discoverCommandQueue)
  {
    ready << "proof.command_queue_discovery="
          << (commandQueueDiscoveryProof.ready ? "candidate-found" : "not-found") << '\n';
    ready << "proof.command_queue_discovery.candidate_count="
          << commandQueueDiscoveryProof.candidates.size() << '\n';
    ready << "proof.command_queue_discovery.scanned_regions="
          << commandQueueDiscoveryProof.scannedRegions << '\n';
    ready << "proof.command_queue_discovery.scanned_bytes="
          << commandQueueDiscoveryProof.scannedBytes << '\n';
    ready << "proof.command_queue_discovery.proof_scope=discovery-only-not-command-behavior\n";
    if (commandQueueDiscoverySnapshotWritten)
      ready << "proof.command_queue_discovery.snapshot=command_queue.candidates.tsv\n";
    if (!commandQueueDiscoveryProof.candidates.empty())
    {
      const CommandQueueCandidate& best = commandQueueDiscoveryProof.candidates.front();
      ready << "proof.command_queue_discovery.best.vector_address="
            << hexAddress(best.vectorAddress) << '\n';
      ready << "proof.command_queue_discovery.best.buffer_begin="
            << hexAddress(best.bufferBegin) << '\n';
      ready << "proof.command_queue_discovery.best.capacity_bytes="
            << best.capacityBytes << '\n';
    }
    if (!commandQueueDiscoveryProof.reason.empty())
      ready << "proof.command_queue_discovery.reason=" << commandQueueDiscoveryProof.reason << '\n';
  }

  std::cout << "bridge.ready=" << readyPath.string() << '\n';
  std::cout << attachProof->readyFileLine << '\n';
  if (proveReadGameState && readGameStateProof.passed)
    std::cout << readGameStateBehaviorProof->readyFileLine << '\n';
  if (activeMatchReady)
    std::cout << activeMatchStateBehaviorProof->readyFileLine << '\n';
  if (readUnitsReady)
    std::cout << readUnitsBehaviorProof->readyFileLine << '\n';
  if (proveDispatchEvents && dispatchEventsProof.passed && dispatchEventsSnapshotWritten && activeMatchReady)
    std::cout << dispatchEventsBehaviorProof->readyFileLine << '\n';
  if (proveReadMapData && mapDataProof.passed && mapDataSnapshotWritten)
    std::cout << "proof.read_map_data=passed\n";
  if (proveReadPlayerData && playerDataProof.passed && playerDataSnapshotWritten && activeMatchReady)
    std::cout << "proof.read_player_data=passed\n";
  if (proveReplayAnalysis && replayAnalysisProof.passed && replayAnalysisSnapshotWritten)
    std::cout << replayAnalysisBehaviorProof->readyFileLine << '\n';
  if (proveBattleNetPolicyFlag && battleNetPolicyProof.passed)
    std::cout << battleNetPolicyBehaviorProof->readyFileLine << '\n';
  if (discoverCommandQueue)
    std::cout << "proof.command_queue_discovery="
              << (commandQueueDiscoveryProof.ready ? "candidate-found" : "not-found") << '\n';
  return proofFailureCode == 0 ? 0 : proofFailureCode;
}
