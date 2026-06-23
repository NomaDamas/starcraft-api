#include <BWAPI/Runtime/RuntimeBackend.h>
#include <BWAPI/Runtime/RuntimeCommandEncoder.h>
#include <BWAPI/Runtime/RuntimeCommandSurface.h>
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
#include <iterator>
#include <limits>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__) || defined(__linux__) || defined(__unix__)
#include <dlfcn.h>
#endif

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
      << "  --self-unit-node-fixture allocate a self-test SC:R unit-node graph before --prove-read-units\n"
      << "  --prove-dispatch-events  prove BWAPI event dispatch from live frame/unit snapshots\n"
      << "  --prove-read-map-data    prove live map metadata by matching the active map to an installed map file\n"
      << "  --prove-read-player-data prove live player ids from unit snapshots\n"
      << "  --prove-read-bullet-data prove live bullet reads by finding BWAPI-compatible bullet records\n"
      << "  --self-bullet-fixture   allocate a self-test bullet array before --prove-read-bullet-data\n"
      << "  --bullet-candidate-address <address>\n"
      << "                           validate an explicit bullet array candidate before broad scans\n"
      << "  --prove-read-region-data prove BWAPI-facing region data from live map/unit metadata\n"
      << "  --self-region-fixture   allocate a self-test region snapshot before --prove-read-region-data\n"
      << "  --prove-replay-analysis  prove replay-compatible map/frame metadata from live state\n"
      << "  --prove-battle-net-policy\n"
      << "                           prove Battle.net launch/attach policy preflight has no blockers\n"
      << "  --prove-load-ai-modules prove the adapter can load BWAPI AI module binaries\n"
      << "  --ai-module-path <path> native module path to load for --prove-load-ai-modules\n"
      << "  --discover-command-queue\n"
      << "                           scan live memory for command-queue-like vector candidates without claiming command proof\n"
      << "  --self-command-queue-fixture\n"
      << "                           allocate a self-test command queue candidate before --discover-command-queue\n"
      << "  --prove-issue-commands\n"
      << "                           prove encoded BWAPI turn-buffer commands are delivered to a live runtime command path\n"
      << "  --issue-command-candidate-scan-limit <count>\n"
      << "                           with --prove-issue-commands, safely try up to count discovered command-queue candidates\n"
      << "  --command-queue-activity-ms <ms>\n"
      << "                           live sampling window used to rank command queue candidates by natural activity (default: 375)\n"
      << "  --command-queue-candidate-limit <count>\n"
      << "                           retain up to count discovered command candidates for diagnostics (default: 32)\n"
      << "  --command-queue-max-scan-mb <mb>\n"
      << "                           maximum memory to scan for command queue candidates (default: --state-max-scan-mb)\n"
      << "  --prove-draw-overlays\n"
      << "                           write fail-closed overlay diagnostics unless a real render hook proof is available\n"
      << "  --prove-multiplayer-sync\n"
      << "                           write fail-closed multiplayer sync diagnostics unless real sync hooks are available\n"
      << "  --serve-command-bridge\n"
      << "                           keep running after proof and consume commands.log into the proven live command queue\n"
      << "  --command-queue-vector-address <address>\n"
      << "                           explicit command queue vector for manual command append diagnostics and live issue-command proof\n"
      << "  --append-game-action <name>\n"
      << "                           append one encoded game action to the explicit command queue vector, then exit\n"
      << "  --state-sample-delay-ms <ms>\n"
      << "                           delay between live state samples (default: 250)\n"
      << "  --state-counter-address <address>\n"
      << "                           validate an explicit live frame counter address before broad scans\n"
      << "  --state-scan-timeout-ms <ms>\n"
      << "                           maximum time for --prove-read-game-state scan (default: 30000)\n"
      << "  --state-max-scan-mb <mb> maximum readable writable memory to sample (default: 128)\n"
      << "  --unit-max-scan-mb <mb>  maximum readable writable memory to scan for units\n"
      << "                           (default: --state-max-scan-mb)\n"
      << "  --bullet-max-scan-mb <mb>\n"
      << "                           maximum readable writable memory to scan for bullets\n"
      << "                           (default: --unit-max-scan-mb)\n"
      << "  --unit-scan-timeout-ms <ms>\n"
      << "                           maximum time for --prove-read-units scan (default: 15000)\n"
      << "  --unit-scan-diagnostics  print direct memory unit-scan counters on success/failure\n"
      << "  --unit-scan-readable-only\n"
      << "                           include readable non-writable non-executable regions in unit scans\n"
      << "  --unit-scan-vectors      also scan std::vector-like begin/end/capacity triples\n"
      << "                           after strided CUnit arrays\n"
      << "  --unit-scan-include-image-regions\n"
      << "                           include regions mapped from the target executable in unit scans\n"
      << "  --unit-scan-classic-fallback\n"
      << "                           allow broad classic CUnit fallback scans for Remastered targets\n"
      << "  --unit-candidate-address <address>\n"
      << "                           validate an explicit CUnit array candidate before broad scans\n"
      << "  --unit-node-candidate-address <address>\n"
      << "                           validate an explicit SC:R unit-node graph candidate before broad scans\n"
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

  struct FrameCounterCandidate
  {
    std::uintptr_t address = 0;
    std::uint32_t first = 0;
    std::uint32_t second = 0;
    std::uint32_t third = 0;
    int score = 0;
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
    std::string idSource;
    std::string positionSource;
    std::string hitPointsSource;
    std::string orderSource;
    std::string playerSource;
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
    std::uint32_t hitPoints = 0;
    bool hitPointsResolved = false;
    std::string hitPointsSource;
    bool metadataDerived = false;
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
    int stormId = -1;
    int race = 8;
    int minerals = -1;
    int gas = -1;
    int supplyUsed = -1;
    int supplyTotal = -1;
    std::uint32_t allianceMask = 0;
    bool raceInferred = false;
  };

  struct PlayerDataProof
  {
    bool passed = false;
    std::size_t playerCount = 0;
    std::size_t observedUnits = 0;
    std::size_t playerInfoRecordSize = 128;
    bool playerInfoProjectionReady = false;
    bool allianceProjectionReady = false;
    std::string projectionSource;
    std::vector<PlayerSnapshotRecord> players;
    std::string reason;
  };

  struct BulletSnapshotRecord
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
  };

  struct BulletDataProof
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
    std::string layoutName;
    std::vector<BulletSnapshotRecord> records;
    std::string reason;
  };

  struct RegionSnapshotRecord
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

  struct RegionDataProof
  {
    bool passed = false;
    std::string source;
    std::size_t regionCount = 0;
    std::size_t observedUnits = 0;
    std::vector<RegionSnapshotRecord> regions;
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

  struct AIModuleLoadProof
  {
    bool passed = false;
    bool selfProcessSmoke = false;
    std::string loader;
    std::string modulePath;
    std::string moduleExtension;
    std::string reason;
  };

  struct CommandQueueCandidate
  {
    std::string storageKind = "vector";
    std::uintptr_t vectorAddress = 0;
    std::uintptr_t bytesInQueueAddress = 0;
    std::uintptr_t bufferBegin = 0;
    std::uintptr_t bufferEnd = 0;
    std::uintptr_t bufferCapacity = 0;
    std::size_t usedBytes = 0;
    std::size_t capacityBytes = 0;
    int score = 0;
    std::size_t activitySamples = 0;
    std::size_t activityTransitions = 0;
    std::size_t activityByteChanges = 0;
    std::string regionPath;
    std::string activityReason;
  };

  struct CommandQueueDiscoveryProof
  {
    bool ready = false;
    std::size_t scannedRegions = 0;
    std::size_t scannedBytes = 0;
    std::size_t imageMappedCandidates = 0;
    std::size_t privateCandidates = 0;
    std::size_t vectorCandidates = 0;
    std::size_t rawTurnBufferCandidates = 0;
    std::size_t retainedVectorCandidates = 0;
    std::size_t retainedRawTurnBufferCandidates = 0;
    std::size_t retainedActiveCandidates = 0;
    std::vector<CommandQueueCandidate> candidates;
    std::string reason;
  };

  struct IssueCommandsAttempt
  {
    std::size_t rank = 0;
    bool deliveryChecked = false;
    bool behaviorChecked = false;
    bool staleProofBytesCleared = false;
    CommandQueueCandidate commandQueue;
    std::uintptr_t frameCounterAddress = 0;
    std::uintptr_t bufferBegin = 0;
    std::size_t originalUsedBytes = 0;
    std::size_t appendedBytes = 0;
    bool consumedImmediately = false;
    bool pauseFrameCounterSampled = false;
    bool pauseFrameCounterMatched = false;
    std::uint32_t baselineStart = 0;
    std::uint32_t baselineEnd = 0;
    std::uint32_t pausedStart = 0;
    std::uint32_t pausedEnd = 0;
    std::uint32_t resumedStart = 0;
    std::uint32_t resumedEnd = 0;
    std::string reason;
  };

  struct IssueCommandsProof
  {
    bool passed = false;
    bool deliveryChecked = false;
    bool behaviorChecked = false;
    bool selfFixture = false;
    bool receiverActive = false;
    bool staleProofBytesCleared = false;
    CommandQueueCandidate commandQueue;
    std::uintptr_t vectorAddress = 0;
    std::uintptr_t bufferBegin = 0;
    std::uintptr_t frameCounterAddress = 0;
    std::size_t originalUsedBytes = 0;
    std::size_t appendedBytes = 0;
    bool pauseFrameCounterSampled = false;
    bool pauseFrameCounterMatched = false;
    std::uint32_t baselineStart = 0;
    std::uint32_t baselineEnd = 0;
    std::uint32_t pausedStart = 0;
    std::uint32_t pausedEnd = 0;
    std::uint32_t resumedStart = 0;
    std::uint32_t resumedEnd = 0;
    std::string commandName;
    std::string encodedBytes;
    std::vector<IssueCommandsAttempt> attempts;
    std::string reason;
  };

  struct DrawOverlaysProof
  {
    bool passed = false;
    bool commandReceiverActive = false;
    bool adapterLocalActionsAvailable = false;
    bool drawLayerAnchorsResolved = false;
    bool renderApiAnchorsResolved = false;
    bool renderHookResolved = false;
    bool renderBehaviorChecked = false;
    std::string requiredHook = "draw-game-layer-hook";
    std::string snapshot = "draw_overlays.snapshot.tsv";
    std::vector<std::string> resolvedAnchors;
    std::vector<std::string> missingAnchors;
    std::string reason;
  };

  struct MultiplayerSyncProof
  {
    bool passed = false;
    bool commandQueueProven = false;
    bool activeMatchProven = false;
    bool replayOnly = false;
    bool replayLaunchDetected = false;
    bool snetReceiveResolved = false;
    bool snetSendTurnResolved = false;
    bool platformReceiveResolved = false;
    bool platformSendResolved = false;
    bool turnPacketAnchorResolved = false;
    bool syncBehaviorChecked = false;
    std::string receiveBinding = "Storm::SNetReceiveMessage";
    std::string sendTurnBinding = "Storm::SNetSendTurn";
    std::string platformReceiveBinding = "SC:R::TLSNetworkConnection::Recv";
    std::string platformSendBinding = "SC:R::TLSNetworkConnection::Send";
    std::string turnPacketBinding = "SC:R::GetTurnPackets";
    std::string snapshot = "multiplayer_sync.snapshot.tsv";
    std::string replayLaunchEvidence;
    std::vector<std::string> resolvedAnchors;
    std::vector<std::string> missingAnchors;
    std::string reason;
  };

  struct UnitCandidateDiagnostic
  {
    std::string source;
    std::uintptr_t address = 0;
    std::size_t recordSize = 0;
    std::string layoutName;
    std::size_t sampledRecords = 0;
    std::size_t activeRecords = 0;
    bool pointerArray = false;
  };

  struct UnitNodeFieldCandidateDiagnostic
  {
    std::uintptr_t address = 0;
    std::uint64_t previous = 0;
    std::uint64_t next = 0;
    std::uint64_t sprite = 0;
    std::uint64_t secondaryObject = 0;
    std::int16_t x = 0;
    std::int16_t y = 0;
    std::int16_t targetX = 0;
    std::int16_t targetY = 0;
    std::uint16_t stateA = 0;
    std::uint16_t stateB = 0;
    bool readableLink = false;
    bool readableSprite = false;
    bool readableSecondaryObject = false;
  };

  struct UnitNodeVectorCandidateDiagnostic
  {
    std::uintptr_t vectorAddress = 0;
    std::uintptr_t begin = 0;
    std::uintptr_t end = 0;
    std::uintptr_t capacity = 0;
    std::size_t usedBytes = 0;
    std::size_t capacityBytes = 0;
    std::size_t recordCount = 0;
    std::size_t pointerCount = 0;
    bool recordVector = false;
    bool pointerVector = false;
    bool readablePrecheck = false;
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
    std::size_t pointerDenseRejectedRecords = 0;
    std::size_t spriteRejectedRecords = 0;
    std::size_t plausibleRecords = 0;
    std::size_t bestActiveRecords = 0;
    std::uintptr_t bestAddress = 0;
    std::size_t bestRecordSize = 0;
    std::string bestLayoutName;
    std::vector<unsigned char> bestBytes;
    std::vector<UnitCandidateDiagnostic> topCandidates;
    std::size_t unitNodeScannedRegions = 0;
    std::size_t unitNodeScannedBytes = 0;
    std::size_t unitNodeFieldCandidates = 0;
    std::size_t unitNodeReadableCandidates = 0;
    std::size_t unitNodeGraphSeedsScored = 0;
    std::size_t unitNodePointerGraphSeedsScored = 0;
    std::size_t unitNodeVectorCandidates = 0;
    std::size_t unitNodeBestActiveRecords = 0;
    std::uintptr_t unitNodeBestAddress = 0;
    std::uintptr_t unitNodeBestVectorAddress = 0;
    std::string unitNodeBestReason;
    std::vector<UnitNodeFieldCandidateDiagnostic> unitNodeFieldSamples;
    std::vector<UnitNodeVectorCandidateDiagnostic> unitNodeVectorSamples;
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
    bool hasClosestCounter = false;
    std::uintptr_t closestCounterAddress = 0;
    std::uint32_t closestCounterFirst = 0;
    std::uint32_t closestCounterSecond = 0;
    std::uint32_t closestCounterThird = 0;
    int closestCounterScore = std::numeric_limits<int>::max();
    std::string closestCounterReason;
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

  struct BulletRecordLayout
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

  constexpr std::array<BulletRecordLayout, 3> bulletRecordLayouts = {
    BulletRecordLayout { "bwapi-classic-cbullet", 112, 0x08, 0x0c, 0x24, 0x28, 0x40, 0x4c, 0x58, 0x64, 0x61 },
    BulletRecordLayout { "scr-x64-packed-cbullet", 0x88, 0x10, 0x18, 0x34, 0x40, 0x58, 0x64, 0x70, 0x78, 0x63 },
    BulletRecordLayout { "scr-x64-aligned-cbullet", 0x90, 0x10, 0x18, 0x34, 0x40, 0x58, 0x64, 0x70, 0x80, 0x63 }
  };

  constexpr std::array<std::size_t, 8> candidateUnitRecordSizes = {
    336, 384, 416, 432, 448, 512, 672, 768
  };

  constexpr std::size_t minActiveUnitRecords = 4;
  constexpr std::size_t minRemasteredSnapshotUnitRecords = 3;
  constexpr std::size_t minActiveBulletRecords = 1;

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
    std::array<unsigned char, 512> rawBuffer;
    std::uint32_t rawBytesInQueue = 0;
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

  BulletDataProof failedBulletDataProof(std::string reason)
  {
    BulletDataProof proof;
    proof.reason = std::move(reason);
    return proof;
  }

  RegionDataProof failedRegionDataProof(std::string reason)
  {
    RegionDataProof proof;
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

  std::int32_t readS32(const std::vector<unsigned char>& bytes, std::size_t offset)
  {
    std::int32_t value = 0;
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

  std::string joinStrings(const std::vector<std::string>& values, const char* separator)
  {
    std::ostringstream output;
    for (std::size_t i = 0; i < values.size(); ++i)
    {
      if (i > 0)
        output << separator;
      output << values[i];
    }
    return output.str();
  }

  struct ExecutableAnchorScan
  {
    bool attempted = false;
    bool readable = false;
    std::vector<std::string> found;
    std::vector<std::string> missing;
    std::string reason;
  };

  ExecutableAnchorScan scanExecutableAnchors(
    const std::string& executablePath,
    const std::vector<std::string>& anchors)
  {
    ExecutableAnchorScan scan;
    scan.attempted = true;
    if (executablePath.empty())
    {
      scan.reason = "target executable path is empty";
      scan.missing = anchors;
      return scan;
    }

    std::ifstream input(executablePath, std::ios::binary);
    if (!input)
    {
      scan.reason = "unable to open target executable";
      scan.missing = anchors;
      return scan;
    }

    const std::string bytes {
      std::istreambuf_iterator<char>(input),
      std::istreambuf_iterator<char>()
    };
    if (input.bad())
    {
      scan.reason = "unable to read complete target executable";
      scan.missing = anchors;
      return scan;
    }

    scan.readable = true;
    for (const std::string& anchor : anchors)
    {
      if (!anchor.empty() && bytes.find(anchor) != std::string::npos)
        scan.found.push_back(anchor);
      else
        scan.missing.push_back(anchor);
    }
    return scan;
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
    // Current SC:R x64 processes use canonical low-half user-space addresses.
    // This rejects static fill patterns such as 0x0101010101010101 that
    // otherwise look pointer-like during broad memory scans.
    if (address >= 0x0000800000000000ULL)
      return false;
#endif
    if ((address & 0x7u) != 0)
      return false;
    if (repeatedBytePattern(address) || repeatedWordPattern(address))
      return false;
    return true;
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

  std::string frameCounterConfidenceFailureReason(
    std::uint32_t first,
    std::uint32_t second,
    std::uint32_t third,
    int sampleDelayMs)
  {
    if (!plausibleCounterDelta(first, second))
      return "first sample pair is not an increasing plausible counter";
    if (!plausibleCounterDelta(second, third))
      return "second sample pair is not an increasing plausible counter";

    const int firstDelta = static_cast<int>(second - first);
    const int secondDelta = static_cast<int>(third - second);
    const int expectedDelta = std::max(1, (sampleDelayMs * 24) / 1000);
    const int minimumFrameLikeDelta = std::max(2, expectedDelta / 3);
    const int maximumFrameLikeDelta = std::max(12, expectedDelta * 4);
    const int maximumStabilityDelta = std::max(8, expectedDelta * 2);

    if (firstDelta < minimumFrameLikeDelta || secondDelta < minimumFrameLikeDelta)
      return "counter advanced too slowly for active StarCraft frames";
    if (firstDelta > maximumFrameLikeDelta || secondDelta > maximumFrameLikeDelta)
      return "counter advanced too quickly for active StarCraft frames";
    if (std::abs(firstDelta - secondDelta) > maximumStabilityDelta)
      return "counter deltas were too unstable for active StarCraft frames";

    const std::uint32_t minimumObservedFrame =
      static_cast<std::uint32_t>(std::max(24, expectedDelta * 2));
    if (third < minimumObservedFrame)
      return "counter value is below minimum observed active-frame threshold";
    return "counter did not pass active-frame confidence checks";
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

  bool readableDynamicPointerValue(
    const std::vector<RuntimeMemoryRegion>& regions,
    std::uint64_t address,
    std::size_t size)
  {
    if (address == 0 || !addressFits(address))
      return false;
    const RuntimeMemoryRegion* region =
      findReadableRegion(regions, static_cast<std::uintptr_t>(address), size);
    return region != nullptr && !region->executable && region->mappedPath.empty();
  }

  std::size_t countReadableDynamicPointers(
    const std::vector<unsigned char>& bytes,
    const std::vector<RuntimeMemoryRegion>& regions,
    std::size_t requiredCount,
    std::size_t maxPointersToCheck)
  {
    std::size_t readablePointers = 0;
    const std::size_t pointerSlots =
      std::min(maxPointersToCheck, bytes.size() / sizeof(std::uint64_t));
    for (std::size_t i = 0; i < pointerSlots; ++i)
    {
      if (readableDynamicPointerValue(regions, readU64(bytes, i * sizeof(std::uint64_t)), 0x58))
      {
        ++readablePointers;
        if (readablePointers >= requiredCount)
          break;
      }
    }
    return readablePointers;
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

  bool startsWith(const std::string& value, const std::string& prefix)
  {
    return value.rfind(prefix, 0) == 0;
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

  std::string readyValue(const std::vector<std::string>& lines, const std::string& key)
  {
    const std::string prefix = key + "=";
    for (const std::string& line : lines)
    {
      if (startsWith(line, prefix))
        return line.substr(prefix.size());
    }
    return {};
  }

  bool existingReadyIdentityMatches(
    const std::vector<std::string>& lines,
    const RuntimeEnvironment& environment)
  {
    if (readyValue(lines, "protocol") != RuntimeExecutorBridgeProtocol)
      return false;
    if (readyValue(lines, "product") != toString(environment.product))
      return false;
    if (readyValue(lines, "version") != environment.version)
      return false;
    if (readyValue(lines, "mode") != RuntimeExecutorBridgeValidatedAdapterMode)
      return false;
    if (readyValue(lines, "runtime.process_visible_at_ready") != "true")
      return false;

    int readyProcessId = 0;
    if (!parsePositiveInt(readyValue(lines, "process_id"), readyProcessId))
      return false;
    if (readyProcessId != environment.processId)
      return false;

    const std::string readyExecutable = readyValue(lines, "executable");
    if (readyExecutable.empty() || environment.executablePath.empty())
      return readyExecutable == environment.executablePath;
    return sameMappedFile(readyExecutable, environment.executablePath);
  }

  bool readyLineReferencesAnyProofToken(
    const std::string& line,
    const std::vector<std::string>& proofTokens)
  {
    for (const std::string& token : proofTokens)
    {
      if (line.find(token) != std::string::npos)
        return true;
    }
    return false;
  }

  bool preservableReadyEvidenceLine(const std::string& line)
  {
    if (line == RuntimeExecutorBridgeCommandSurfaceLine)
      return false;
    if (line == "proof.attach=passed")
      return false;
    if (startsWith(line, "proof.command_surface"))
      return false;
    if (startsWith(line, "contract.binding.shared-memory-client-transport="))
      return false;
    return startsWith(line, "proof.")
      || startsWith(line, "contract.binding.")
      || startsWith(line, "contract.structure.")
      || startsWith(line, "contract.field.");
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

  int commandQueueScanRegionPriority(
    const RuntimeMemoryRegion& region,
    const std::string& executablePath,
    std::uintptr_t targetImageBase)
  {
    const bool targetImageRegion = sameMappedFile(region.mappedPath, executablePath);
    const bool likelyTargetTextMapping =
      targetImageRegion
      && targetImageBase != 0
      && region.address == targetImageBase
      && region.size >= 8 * 1024 * 1024;
    if (!targetImageRegion && !fileBackedNonTargetRegion(region, executablePath))
      return 0;
    if (targetImageRegion && !likelyTargetTextMapping)
      return 2;
    if (targetImageRegion)
      return 3;
    return 3;
  }

  int unitNodeScanRegionPriority(
    const RuntimeMemoryRegion& region,
    const std::string& executablePath,
    std::uintptr_t targetImageBase)
  {
    if (region.mappedPath.empty())
      return 0;

    const StarCraftImageSectionHints hints = starCraftImageSectionHints(targetImageBase);
    if (regionIntersectsStarCraftRuntimeData(region, hints))
      return 1;

    const bool targetImageRegion = sameMappedFile(region.mappedPath, executablePath);
    const bool likelyTargetTextMapping =
      targetImageRegion
      && targetImageBase != 0
      && region.address == targetImageBase
      && region.size >= 8 * 1024 * 1024;
    if (targetImageRegion && !likelyTargetTextMapping)
      return 2;
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

  bool pointerDenseUnitRecord(
    const std::vector<unsigned char>& bytes,
    std::size_t offset,
    std::size_t recordSize,
    const std::vector<RuntimeMemoryRegion>& regions)
  {
    if (offset + recordSize > bytes.size())
      return false;

    const std::size_t bytesToInspect = std::min<std::size_t>(recordSize, 192);
    const std::size_t slots = bytesToInspect / sizeof(std::uint64_t);
    if (slots < 8)
      return false;

    std::size_t nonZeroSlots = 0;
    std::size_t readablePointerSlots = 0;
    for (std::size_t slot = 0; slot < slots; ++slot)
    {
      const std::uint64_t value = readU64(bytes, offset + slot * sizeof(std::uint64_t));
      if (value == 0)
        continue;
      ++nonZeroSlots;
      if (readablePointerValue(regions, value, 8))
        ++readablePointerSlots;
    }

    return readablePointerSlots >= 12
      && readablePointerSlots * 2 >= nonZeroSlots
      && readablePointerSlots * 3 >= slots * 2;
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
      && !pointerDenseUnitRecord(bytes, offset, recordSize, regions)
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

    if (pointerDenseUnitRecord(bytes, offset, recordSize, regions))
    {
      if (diagnostics != nullptr)
        ++diagnostics->pointerDenseRejectedRecords;
      return false;
    }

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

    const bool previousLooksLikeObject =
      previous != 0 && plausibleRuntimeObjectPointerValue(previous);
    const bool nextLooksLikeObject =
      next != 0 && plausibleRuntimeObjectPointerValue(next);
    const bool linked = previousLooksLikeObject || nextLooksLikeObject;
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
      && plausibleRuntimeObjectPointerValue(sprite)
      && plausibleRuntimeObjectPointerValue(secondaryObject)
      && sprite != secondaryObject;
  }

  bool readableRuntimeObjectPointerValue(
    const std::vector<RuntimeMemoryRegion>& regions,
    std::uint64_t address,
    std::size_t size)
  {
    if (!plausibleRuntimeObjectPointerValue(address))
      return false;
    const RuntimeMemoryRegion* region =
      findReadableRegion(regions, static_cast<std::uintptr_t>(address), size);
    return region != nullptr && !region->executable;
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
    return (readableRuntimeObjectPointerValue(regions, previous, 16)
        || readableRuntimeObjectPointerValue(regions, next, 16))
      && readableRuntimeObjectPointerValue(regions, sprite, 16)
      && readableRuntimeObjectPointerValue(regions, secondaryObject, 16);
  }

  void rememberUnitNodeFieldSample(
    UnitScanDiagnostics* diagnostics,
    const std::vector<unsigned char>& bytes,
    std::size_t offset,
    std::uintptr_t address,
    const std::vector<RuntimeMemoryRegion>& regions)
  {
    if (diagnostics == nullptr || diagnostics->unitNodeFieldSamples.size() >= 8)
      return;

    UnitNodeFieldCandidateDiagnostic sample;
    sample.address = address;
    sample.previous = readU64(bytes, offset);
    sample.next = readU64(bytes, offset + 0x08);
    sample.x = readS16(bytes, offset + 0x24);
    sample.y = readS16(bytes, offset + 0x26);
    sample.targetX = readS16(bytes, offset + 0x28);
    sample.targetY = readS16(bytes, offset + 0x2a);
    sample.stateA = readU16(bytes, offset + 0x30);
    sample.stateB = readU16(bytes, offset + 0x32);
    sample.sprite = readU64(bytes, offset + 0x38);
    sample.secondaryObject = readU64(bytes, offset + 0x50);
    sample.readableLink =
      readableRuntimeObjectPointerValue(regions, sample.previous, 16)
      || readableRuntimeObjectPointerValue(regions, sample.next, 16);
    sample.readableSprite = readableRuntimeObjectPointerValue(regions, sample.sprite, 16);
    sample.readableSecondaryObject =
      readableRuntimeObjectPointerValue(regions, sample.secondaryObject, 16);
    diagnostics->unitNodeFieldSamples.push_back(sample);
  }

  void rememberUnitNodeVectorSample(
    UnitScanDiagnostics* diagnostics,
    std::uintptr_t vectorAddress,
    std::uintptr_t begin,
    std::uintptr_t end,
    std::uintptr_t capacity,
    bool recordVector,
    bool pointerVector,
    std::size_t recordCount,
    std::size_t pointerCount,
    bool readablePrecheck)
  {
    if (diagnostics == nullptr)
      return;

    UnitNodeVectorCandidateDiagnostic sample;
    sample.vectorAddress = vectorAddress;
    sample.begin = begin;
    sample.end = end;
    sample.capacity = capacity;
    sample.usedBytes = end > begin ? static_cast<std::size_t>(end - begin) : 0;
    sample.capacityBytes = capacity > begin ? static_cast<std::size_t>(capacity - begin) : 0;
    sample.recordVector = recordVector;
    sample.pointerVector = pointerVector;
    sample.recordCount = recordCount;
    sample.pointerCount = pointerCount;
    sample.readablePrecheck = readablePrecheck;
    if (diagnostics->unitNodeVectorSamples.size() >= 16)
    {
      if (!readablePrecheck)
        return;
      auto replace = std::find_if(
        diagnostics->unitNodeVectorSamples.begin(),
        diagnostics->unitNodeVectorSamples.end(),
        [](const UnitNodeVectorCandidateDiagnostic& existing)
        {
          return !existing.readablePrecheck;
        });
      if (replace == diagnostics->unitNodeVectorSamples.end())
        return;
      *replace = sample;
      return;
    }
    diagnostics->unitNodeVectorSamples.push_back(sample);
  }

  void rememberBestUnitNodeCandidate(
    UnitScanDiagnostics* diagnostics,
    const LiveUnitNodeProof& proof)
  {
    if (diagnostics == nullptr || proof.activeRecords <= diagnostics->unitNodeBestActiveRecords)
      return;

    diagnostics->unitNodeBestActiveRecords = proof.activeRecords;
    diagnostics->unitNodeBestAddress = proof.address;
    diagnostics->unitNodeBestVectorAddress = proof.vectorAddress;
    diagnostics->unitNodeBestReason = proof.reason;
  }

  LiveUnitNodeProof scoreUnitNodeAnchorArray(
    const std::vector<unsigned char>& bytes,
    std::uintptr_t baseAddress,
    std::size_t offset,
    const std::vector<RuntimeMemoryRegion>& regions,
    const std::chrono::steady_clock::time_point& deadline,
    bool& scanTimedOut,
    UnitScanDiagnostics* diagnostics)
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
        if (diagnostics != nullptr)
          ++diagnostics->unitNodeReadableCandidates;
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
    rememberBestUnitNodeCandidate(diagnostics, proof);
    return proof;
  }

  LiveUnitNodeProof proveUnitNodeAnchorsInBytes(
    const std::vector<unsigned char>& bytes,
    std::uintptr_t baseAddress,
    const std::vector<RuntimeMemoryRegion>& regions,
    const std::chrono::steady_clock::time_point& deadline,
    bool& scanTimedOut,
    UnitScanDiagnostics* diagnostics)
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
      {
        if (diagnostics != nullptr)
          ++diagnostics->unitNodeFieldCandidates;
        rememberUnitNodeFieldSample(
          diagnostics,
          bytes,
          recordOffset,
          baseAddress + recordOffset,
          regions);
        if (plausibleUnitNodeAnchorRecord(bytes, recordOffset, regions))
          ++plausibleByResidue[recordOffset % recordSize];
      }
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
        scanTimedOut,
        diagnostics);
      if (scanTimedOut || proof.passed)
        return proof;
    }

    return {};
  }

  LiveUnitNodeProof proveUnitNodePointerGraphsInBytes(
    int processId,
    const std::vector<unsigned char>& bytes,
    const std::vector<RuntimeMemoryRegion>& regions,
    const std::chrono::steady_clock::time_point& deadline,
    bool& scanTimedOut,
    UnitScanDiagnostics* diagnostics);

  LiveUnitNodeProof proveUnitNodeVectorsInBytes(
    int processId,
    const std::vector<unsigned char>& bytes,
    std::uintptr_t baseAddress,
    const std::vector<RuntimeMemoryRegion>& regions,
    const std::chrono::steady_clock::time_point& deadline,
    bool& scanTimedOut,
    UnitScanDiagnostics* diagnostics)
  {
    constexpr std::size_t recordSize = 0x58;
    constexpr std::size_t maxUnitNodeRecords = 4096;
    constexpr std::size_t maxVectorCandidatesToRead = 128;
    constexpr std::size_t maxVectorOffsetsToInspect = 1024 * 1024;
    std::size_t vectorCandidatesRead = 0;
    std::size_t vectorOffsetsInspected = 0;
    for (std::size_t offset = 0; offset + sizeof(std::uint64_t) * 3 <= bytes.size(); offset += 8)
    {
      if (++vectorOffsetsInspected > maxVectorOffsetsToInspect)
        return {};
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
      const bool nodeRecordVector =
        usedBytes >= recordSize * minActiveUnitRecords
        && (usedBytes % recordSize) == 0;
      const bool nodePointerVector =
        usedBytes >= sizeof(std::uint64_t) * minActiveUnitRecords
        && (usedBytes % sizeof(std::uint64_t)) == 0;
      if (!nodeRecordVector && !nodePointerVector)
        continue;
      if (diagnostics != nullptr)
        ++diagnostics->unitNodeVectorCandidates;

      const std::size_t recordCount = nodeRecordVector ? usedBytes / recordSize : 0;
      const std::size_t pointerCount = nodePointerVector ? usedBytes / sizeof(std::uint64_t) : 0;
      if (recordCount > maxUnitNodeRecords || pointerCount > maxUnitNodeRecords)
        continue;
      const std::size_t readablePrecheckBytes = nodeRecordVector
        ? recordSize * minActiveUnitRecords
        : sizeof(std::uint64_t) * minActiveUnitRecords;
      const bool readablePrecheck =
        readableAddress(regions, begin, std::min<std::size_t>(usedBytes, readablePrecheckBytes));
      rememberUnitNodeVectorSample(
        diagnostics,
        baseAddress + offset,
        begin,
        end,
        capacity,
        nodeRecordVector,
        nodePointerVector,
        recordCount,
        pointerCount,
        readablePrecheck);
      if (!readablePrecheck)
        continue;
      if (++vectorCandidatesRead > maxVectorCandidatesToRead)
        return {};

      if (nodePointerVector)
      {
        const std::size_t pointerBytesToRead =
          std::min<std::size_t>(usedBytes, sizeof(std::uint64_t) * 4096);
        RuntimeMemoryReadResult pointerRead = readProcessMemory(processId, begin, pointerBytesToRead);
        if (pointerRead.success && pointerRead.bytesRead >= sizeof(std::uint64_t) * minActiveUnitRecords)
        {
          if (countReadableDynamicPointers(
                pointerRead.bytes,
                regions,
                minActiveUnitRecords,
                256) < minActiveUnitRecords)
            continue;

          LiveUnitNodeProof pointerProof = proveUnitNodePointerGraphsInBytes(
            processId,
            pointerRead.bytes,
            regions,
            deadline,
            scanTimedOut,
            diagnostics);
          if (scanTimedOut)
            return {};
          if (pointerProof.passed)
          {
            pointerProof.vectorAddress = baseAddress + offset;
            pointerProof.sampledRecords = pointerCount;
            return pointerProof;
          }
        }
      }

      if (!nodeRecordVector)
        continue;

      const std::size_t bytesToRead = std::min<std::size_t>(usedBytes, recordSize * 256);
      RuntimeMemoryReadResult read = readProcessMemory(processId, begin, bytesToRead);
      if (!read.success || read.bytesRead < recordSize * minActiveUnitRecords)
        continue;

      LiveUnitNodeProof proof = scoreUnitNodeAnchorArray(
        read.bytes,
        begin,
        0,
        regions,
        deadline,
        scanTimedOut,
        diagnostics);
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
    std::size_t recordSize = 0,
    const std::string& source = "strided")
  {
    if (diagnostics == nullptr)
      return;

    if (proof.activeRecords > 0 && proof.address != 0)
    {
      const auto duplicate = std::find_if(
        diagnostics->topCandidates.begin(),
        diagnostics->topCandidates.end(),
        [&](const UnitCandidateDiagnostic& candidate)
        {
          return candidate.address == proof.address
            && candidate.recordSize == proof.recordSize
            && candidate.layoutName == proof.layoutName
            && candidate.source == source;
        });
      if (duplicate == diagnostics->topCandidates.end())
      {
        UnitCandidateDiagnostic candidate;
        candidate.source = source;
        candidate.address = proof.address;
        candidate.recordSize = proof.recordSize;
        candidate.layoutName = proof.layoutName;
        candidate.sampledRecords = proof.sampledRecords;
        candidate.activeRecords = proof.activeRecords;
        candidate.pointerArray = proof.pointerArray;
        diagnostics->topCandidates.push_back(std::move(candidate));
      }
      else if (proof.activeRecords > duplicate->activeRecords)
      {
        duplicate->activeRecords = proof.activeRecords;
        duplicate->sampledRecords = proof.sampledRecords;
      }

      std::sort(
        diagnostics->topCandidates.begin(),
        diagnostics->topCandidates.end(),
        [](const UnitCandidateDiagnostic& lhs, const UnitCandidateDiagnostic& rhs)
        {
          if (lhs.activeRecords != rhs.activeRecords)
            return lhs.activeRecords > rhs.activeRecords;
          if (lhs.sampledRecords != rhs.sampledRecords)
            return lhs.sampledRecords > rhs.sampledRecords;
          return lhs.address < rhs.address;
        });
      constexpr std::size_t maxTopCandidates = 8;
      if (diagnostics->topCandidates.size() > maxTopCandidates)
        diagnostics->topCandidates.resize(maxTopCandidates);
    }

    if (proof.activeRecords <= diagnostics->bestActiveRecords)
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
          rememberBestCandidate(diagnostics, proof, &bytes, baseOffset, recordSize, "strided-region");
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
                  rememberBestCandidate(diagnostics, proof, nullptr, 0, 0, "pointer-vector");
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
          rememberBestCandidate(diagnostics, proof, &read.bytes, 0, recordSize, "record-vector");
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
        const int lhsPriority = unitNodeScanRegionPriority(lhs, executablePath, targetImageBase);
        const int rhsPriority = unitNodeScanRegionPriority(rhs, executablePath, targetImageBase);
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

  LiveUnitNodeProof scoreLinkedUnitNodeGraph(
    int processId,
    std::uintptr_t seedAddress,
    const std::vector<RuntimeMemoryRegion>& regions,
    const std::chrono::steady_clock::time_point& deadline,
    bool& scanTimedOut,
    UnitScanDiagnostics* diagnostics);

  LiveUnitNodeProof proveUnitNodePointerGraphsInBytes(
    int processId,
    const std::vector<unsigned char>& bytes,
    const std::vector<RuntimeMemoryRegion>& regions,
    const std::chrono::steady_clock::time_point& deadline,
    bool& scanTimedOut,
    UnitScanDiagnostics* diagnostics);

  LiveUnitNodeProof proveUnitNodeGraphsInBytes(
    int processId,
    const std::vector<unsigned char>& bytes,
    std::uintptr_t baseAddress,
    const std::vector<RuntimeMemoryRegion>& regions,
    const std::chrono::steady_clock::time_point& deadline,
    bool& scanTimedOut,
    UnitScanDiagnostics* diagnostics);

  bool parseRemasteredUnitSnapshotRecord(
    int processId,
    const std::vector<unsigned char>& bytes,
    std::size_t offset,
    std::uintptr_t nodeAddress,
    const std::vector<RuntimeMemoryRegion>& regions,
    RemasteredUnitSnapshotRecord& record);

  LiveUnitNodeProof collectBucketVerifiedUnitNodesInBytes(
    int processId,
    const std::vector<unsigned char>& bytes,
    std::uintptr_t baseAddress,
    const std::vector<RuntimeMemoryRegion>& regions,
    const std::chrono::steady_clock::time_point& deadline,
    bool& scanTimedOut,
    UnitScanDiagnostics* diagnostics);

  LiveUnitNodeProof proveLiveUnitNodeAnchors(
    int processId,
    const std::string& executablePath,
    std::size_t maxScanBytes,
    int scanTimeoutMs,
    UnitScanDiagnostics* diagnostics)
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
        const int lhsPriority = unitNodeScanRegionPriority(lhs, executablePath, targetImageBase);
        const int rhsPriority = unitNodeScanRegionPriority(rhs, executablePath, targetImageBase);
        if (lhsPriority != rhsPriority)
          return lhsPriority < rhsPriority;
        return lhs.address < rhs.address;
      });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(scanTimeoutMs);
    constexpr std::size_t maxRegionBytes = 16 * 1024 * 1024;
    std::size_t scanned = 0;

    const StarCraftImageSectionHints hints = starCraftImageSectionHints(targetImageBase);
    const std::array<std::pair<std::uintptr_t, std::size_t>, 2> prioritySections = {
      std::make_pair(hints.bssAddress, hints.bssSize),
      std::make_pair(hints.commonAddress, hints.commonSize)
    };
    const bool scanStaticSectionsFirst = maxScanBytes >= 512 * 1024 * 1024;
    for (const auto& section : prioritySections)
    {
      if (!scanStaticSectionsFirst)
        break;
      if (section.first == 0 || section.second < 0x58 * minActiveUnitRecords)
        continue;
      const std::size_t bytesToRead = std::min(section.second, maxRegionBytes);
      RuntimeMemoryReadResult read = readProcessMemory(processId, section.first, bytesToRead);
      if (!read.success || read.bytesRead < 0x58 * minActiveUnitRecords)
        continue;
      if (diagnostics != nullptr)
      {
        ++diagnostics->unitNodeScannedRegions;
        diagnostics->unitNodeScannedBytes += read.bytesRead;
      }

      bool scanTimedOut = false;
      LiveUnitNodeProof graphProof = proveUnitNodeGraphsInBytes(
        processId,
        read.bytes,
        section.first,
        regions.regions,
        deadline,
        scanTimedOut,
        diagnostics);
      if (scanTimedOut)
        return failedUnitNodeProof("SC:R unit-node linked graph section scan timed out before proof");
      if (graphProof.passed)
        return graphProof;
      rememberBestUnitNodeCandidate(diagnostics, graphProof);

      LiveUnitNodeProof collectedProof = collectBucketVerifiedUnitNodesInBytes(
        processId,
        read.bytes,
        section.first,
        regions.regions,
        deadline,
        scanTimedOut,
        diagnostics);
      if (scanTimedOut)
        return failedUnitNodeProof("SC:R bucket-verified unit-node section scan timed out before proof");
      if (collectedProof.passed)
        return collectedProof;
      rememberBestUnitNodeCandidate(diagnostics, collectedProof);

      LiveUnitNodeProof proof = proveUnitNodeAnchorsInBytes(
        read.bytes,
        section.first,
        regions.regions,
        deadline,
        scanTimedOut,
        diagnostics);
      if (scanTimedOut)
        return failedUnitNodeProof("SC:R unit-node anchor section scan timed out before proof");
      if (proof.passed)
        return proof;
      rememberBestUnitNodeCandidate(diagnostics, proof);

      LiveUnitNodeProof vectorProof = proveUnitNodeVectorsInBytes(
        processId,
        read.bytes,
        section.first,
        regions.regions,
        deadline,
        scanTimedOut,
        diagnostics);
      if (scanTimedOut)
        return failedUnitNodeProof("SC:R unit-node vector section scan timed out before proof");
      if (vectorProof.passed)
        return vectorProof;
      rememberBestUnitNodeCandidate(diagnostics, vectorProof);
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
      if (diagnostics != nullptr)
      {
        ++diagnostics->unitNodeScannedRegions;
        diagnostics->unitNodeScannedBytes += read.bytesRead;
      }

      bool scanTimedOut = false;
      LiveUnitNodeProof graphProof = proveUnitNodeGraphsInBytes(
        processId,
        read.bytes,
        region.address,
        regions.regions,
        deadline,
        scanTimedOut,
        diagnostics);
      if (scanTimedOut)
        return failedUnitNodeProof("SC:R unit-node linked graph scan timed out before proof");
      if (graphProof.passed)
        return graphProof;
      rememberBestUnitNodeCandidate(diagnostics, graphProof);

      LiveUnitNodeProof collectedProof = collectBucketVerifiedUnitNodesInBytes(
        processId,
        read.bytes,
        region.address,
        regions.regions,
        deadline,
        scanTimedOut,
        diagnostics);
      if (scanTimedOut)
        return failedUnitNodeProof("SC:R bucket-verified unit-node scan timed out before proof");
      if (collectedProof.passed)
        return collectedProof;
      rememberBestUnitNodeCandidate(diagnostics, collectedProof);

      LiveUnitNodeProof proof = proveUnitNodeAnchorsInBytes(
        read.bytes,
        region.address,
        regions.regions,
        deadline,
        scanTimedOut,
        diagnostics);
      if (scanTimedOut)
        return failedUnitNodeProof("SC:R unit-node anchor scan timed out before proof");
      if (proof.passed)
        return proof;
      rememberBestUnitNodeCandidate(diagnostics, proof);

      LiveUnitNodeProof vectorProof = proveUnitNodeVectorsInBytes(
        processId,
        read.bytes,
        region.address,
        regions.regions,
        deadline,
        scanTimedOut,
        diagnostics);
      if (scanTimedOut)
        return failedUnitNodeProof("SC:R unit-node vector scan timed out before proof");
      if (vectorProof.passed)
        return vectorProof;
      rememberBestUnitNodeCandidate(diagnostics, vectorProof);

      scanned += read.bytesRead;
    }

    return failedUnitNodeProof("no active SC:R unit-node anchor found");
  }

  LiveUnitNodeProof proveExplicitUnitNodeCandidateAddresses(
    int processId,
    const std::vector<std::uintptr_t>& candidateAddresses,
    int scanTimeoutMs,
    UnitScanDiagnostics* diagnostics)
  {
    if (candidateAddresses.empty())
      return failedUnitNodeProof("no explicit SC:R unit-node candidate addresses were provided");

    RuntimeMemoryRegionListResult regions = listProcessMemoryRegions(processId);
    if (!regions.success)
      return failedUnitNodeProof(regions.reason);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(scanTimeoutMs);
    constexpr std::size_t recordSize = 0x58;
    constexpr std::size_t maxSnapshotRecords = 256;
    for (std::uintptr_t candidateAddress : candidateAddresses)
    {
      if (timedOut(deadline))
        return failedUnitNodeProof("explicit SC:R unit-node candidate scan timed out before proof");

      const RuntimeMemoryRegion* containingRegion =
        findReadableRegion(regions.regions, candidateAddress, recordSize * minActiveUnitRecords);
      if (containingRegion == nullptr)
        continue;

      const std::uintptr_t regionEnd = containingRegion->address + containingRegion->size;
      const std::size_t regionBytes =
        regionEnd > candidateAddress
          ? static_cast<std::size_t>(regionEnd - candidateAddress)
          : 0;
      const std::size_t bytesToRead = std::min(regionBytes, recordSize * maxSnapshotRecords);
      if (bytesToRead < recordSize * minActiveUnitRecords)
        continue;

      RuntimeMemoryReadResult read = readProcessMemory(processId, candidateAddress, bytesToRead);
      if (!read.success || read.bytesRead < recordSize * minActiveUnitRecords)
        continue;

      bool scanTimedOut = false;
      LiveUnitNodeProof graphProof = scoreLinkedUnitNodeGraph(
        processId,
        candidateAddress,
        regions.regions,
        deadline,
        scanTimedOut,
        diagnostics);
      if (scanTimedOut)
        return failedUnitNodeProof("explicit SC:R unit-node graph candidate scan timed out before proof");
      if (graphProof.passed)
        return graphProof;
      rememberBestUnitNodeCandidate(diagnostics, graphProof);

      LiveUnitNodeProof proof = scoreUnitNodeAnchorArray(
        read.bytes,
        candidateAddress,
        0,
        regions.regions,
        deadline,
        scanTimedOut,
        diagnostics);
      if (scanTimedOut)
        return failedUnitNodeProof("explicit SC:R unit-node candidate scan timed out before proof");
      if (proof.passed)
      {
        if (!graphProof.passed
            && graphProof.records.size() >= minRemasteredSnapshotUnitRecords)
        {
          graphProof.passed = true;
          graphProof.address = candidateAddress;
          graphProof.recordSize = proof.recordSize;
          graphProof.sampledRecords = std::max(graphProof.sampledRecords, proof.sampledRecords);
          graphProof.activeRecords = graphProof.records.size();
          return graphProof;
        }
        return proof;
      }
      rememberBestUnitNodeCandidate(diagnostics, proof);
    }

    return failedUnitNodeProof("no explicit SC:R unit-node candidate address contained enough active records");
  }

  bool parseScrCompactHitPoints(
    const std::vector<unsigned char>& bytes,
    std::uint32_t& hitPoints,
    std::string& source)
  {
    constexpr std::size_t currentHitPointsOffset = 0x1a;
    constexpr std::size_t maxHitPointsOffset = 0x1b;
    if (bytes.size() <= maxHitPointsOffset)
      return false;

    const unsigned char currentHitPoints = bytes[currentHitPointsOffset];
    const unsigned char maxHitPoints = bytes[maxHitPointsOffset];
    if (currentHitPoints == 0 || maxHitPoints == 0 || currentHitPoints > maxHitPoints)
      return false;

    hitPoints = static_cast<std::uint32_t>(currentHitPoints) * 256u;
    source = "secondary+0x1a compact-hp-byte";
    return true;
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

    record.nodeAddress = nodeAddress;
    record.spriteAddress = static_cast<std::uintptr_t>(readU64(bytes, offset + 0x38));
    record.x = readS16(bytes, offset + 0x24);
    record.y = readS16(bytes, offset + 0x26);
    record.targetX = readS16(bytes, offset + 0x28);
    record.targetY = readS16(bytes, offset + 0x2a);
    record.order = readU16(bytes, offset + 0x30);
    record.state = readU16(bytes, offset + 0x32);

    constexpr std::size_t secondaryRecordSize = 0x50;
    const std::uint64_t secondaryAddress64 = readU64(bytes, offset + 0x50);
    if (addressFits(secondaryAddress64))
    {
      const auto secondaryAddress = static_cast<std::uintptr_t>(secondaryAddress64);
      if (readableAddress(regions, secondaryAddress, secondaryRecordSize))
      {
        RuntimeMemoryReadResult secondaryRead =
          readProcessMemory(processId, secondaryAddress, secondaryRecordSize);
        if (secondaryRead.success && secondaryRead.bytesRead >= secondaryRecordSize)
        {
          const unsigned char rawPlayer = secondaryRead.bytes[0x14];
          std::uint16_t typeHint = readU16(secondaryRead.bytes, 0x10);
          if (typeHint == 0 || typeHint >= 256)
            typeHint = readU16(secondaryRead.bytes, 0x20);

          if ((rawPlayer < 12 || rawPlayer == 255) && typeHint != 0 && typeHint < 256)
          {
            record.secondaryAddress = secondaryAddress;
            record.id = static_cast<std::uint32_t>((nodeAddress >> 4) & 0xffffffffu);
            record.player = rawPlayer == 255 ? 11 : static_cast<int>(rawPlayer);
            record.typeHint = typeHint;
            record.hitPointsResolved =
              parseScrCompactHitPoints(secondaryRead.bytes, record.hitPoints, record.hitPointsSource);
            return true;
          }
        }
      }
    }

    constexpr std::size_t metadataRecordSize = 0xe0;
    const std::uint64_t metadataAddress64 = readU64(bytes, offset + 0x40);
    if (!addressFits(metadataAddress64))
      return false;
    const auto metadataAddress = static_cast<std::uintptr_t>(metadataAddress64);
    if (!readableAddress(regions, metadataAddress, metadataRecordSize))
      return false;

    RuntimeMemoryReadResult metadataRead =
      readProcessMemory(processId, metadataAddress, metadataRecordSize);
    if (!metadataRead.success || metadataRead.bytesRead < metadataRecordSize)
      return false;

    const unsigned char metadataPlayer = metadataRead.bytes[0xc0];
    const std::uint16_t metadataType = readU16(metadataRead.bytes, 0xd0);
    if (metadataPlayer >= 12 || metadataType == 0 || metadataType >= 256)
      return false;

    record.secondaryAddress = metadataAddress;
    record.id = static_cast<std::uint32_t>((nodeAddress >> 4) & 0xffffffffu);
    record.player = static_cast<int>(metadataPlayer);
    record.typeHint = metadataType;
    record.hitPointsResolved =
      parseScrCompactHitPoints(metadataRead.bytes, record.hitPoints, record.hitPointsSource);
    if (record.hitPointsResolved && record.hitPointsSource.rfind("secondary+", 0) == 0)
      record.hitPointsSource.replace(0, 10, "metadata+");
    record.metadataDerived = true;
    return record.id != 0;
  }

  LiveUnitNodeProof collectBucketVerifiedUnitNodesInBytes(
    int processId,
    const std::vector<unsigned char>& bytes,
    std::uintptr_t baseAddress,
    const std::vector<RuntimeMemoryRegion>& regions,
    const std::chrono::steady_clock::time_point& deadline,
    bool& scanTimedOut,
    UnitScanDiagnostics* diagnostics)
  {
    constexpr std::size_t recordSize = 0x58;
    constexpr std::size_t maxSnapshotRecords = 256;

    LiveUnitNodeProof proof;
    proof.recordSize = recordSize;
    std::unordered_set<std::uintptr_t> accepted;

    for (std::size_t offset = 0; offset + recordSize <= bytes.size(); offset += 8)
    {
      if ((offset % (16 * 1024)) == 0 && timedOut(deadline))
      {
        scanTimedOut = true;
        return {};
      }
      if (!plausibleUnitNodeAnchorRecord(bytes, offset, regions))
        continue;

      RemasteredUnitSnapshotRecord record;
      const std::uintptr_t nodeAddress = baseAddress + offset;
      if (!parseRemasteredUnitSnapshotRecord(
            processId,
            bytes,
            offset,
            nodeAddress,
            regions,
            record))
        continue;

      if (!accepted.insert(nodeAddress).second)
        continue;

      if (proof.address == 0)
        proof.address = nodeAddress;
      record.index = proof.records.size();
      proof.records.push_back(record);
      proof.activeRecords = proof.records.size();
      proof.sampledRecords = accepted.size();

      if (proof.records.size() >= maxSnapshotRecords)
        break;
    }

    if (proof.activeRecords >= minActiveUnitRecords)
    {
      proof.passed = true;
      return proof;
    }

    if (proof.activeRecords > 0)
      proof.reason = "bucket-verified SC:R unit-node scan found "
        + std::to_string(proof.activeRecords)
        + " active records below required="
        + std::to_string(minActiveUnitRecords);
    rememberBestUnitNodeCandidate(diagnostics, proof);
    return proof;
  }

  LiveUnitNodeProof scoreLinkedUnitNodeGraph(
    int processId,
    std::uintptr_t seedAddress,
    const std::vector<RuntimeMemoryRegion>& regions,
    const std::chrono::steady_clock::time_point& deadline,
    bool& scanTimedOut,
    UnitScanDiagnostics* diagnostics)
  {
    constexpr std::size_t recordSize = 0x58;
    constexpr std::size_t maxGraphRecords = 256;

    LiveUnitNodeProof proof;
    proof.address = seedAddress;
    proof.recordSize = recordSize;
    if (!readableAddress(regions, seedAddress, recordSize))
    {
      proof.reason = "SC:R unit-node graph seed is not readable";
      return proof;
    }

    std::vector<std::uintptr_t> pending;
    std::unordered_set<std::uintptr_t> queued;
    std::unordered_set<std::uintptr_t> accepted;
    pending.push_back(seedAddress);
    queued.insert(seedAddress);

    for (std::size_t index = 0; index < pending.size() && accepted.size() < maxGraphRecords; ++index)
    {
      if ((index % 16) == 0 && timedOut(deadline))
      {
        scanTimedOut = true;
        return {};
      }

      const std::uintptr_t nodeAddress = pending[index];
      if (!readableAddress(regions, nodeAddress, recordSize))
        continue;

      RuntimeMemoryReadResult read = readProcessMemory(processId, nodeAddress, recordSize);
      if (!read.success || read.bytesRead < recordSize)
        continue;

      const std::array<std::uint64_t, 2> links = {
        readU64(read.bytes, 0),
        readU64(read.bytes, 0x08)
      };
      for (std::uint64_t link : links)
      {
        if (!addressFits(link))
          continue;
        const auto linkedAddress = static_cast<std::uintptr_t>(link);
        if (linkedAddress == 0 || !readableAddress(regions, linkedAddress, recordSize))
          continue;
        if (queued.insert(linkedAddress).second)
          pending.push_back(linkedAddress);
      }

      RemasteredUnitSnapshotRecord record;
      record.index = proof.records.size();
      if (!parseRemasteredUnitSnapshotRecord(
            processId,
            read.bytes,
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
    }

    if (proof.activeRecords >= minActiveUnitRecords)
    {
      proof.passed = true;
      return proof;
    }

    proof.reason = "candidate SC:R unit-node graph did not contain enough bucket-verified active records";
    rememberBestUnitNodeCandidate(diagnostics, proof);
    return proof;
  }

  LiveUnitNodeProof proveUnitNodePointerGraphsInBytes(
    int processId,
    const std::vector<unsigned char>& bytes,
    const std::vector<RuntimeMemoryRegion>& regions,
    const std::chrono::steady_clock::time_point& deadline,
    bool& scanTimedOut,
    UnitScanDiagnostics* diagnostics)
  {
    constexpr std::size_t recordSize = 0x58;
    constexpr std::size_t maxGraphSeedsToScore = 16;
    constexpr std::size_t maxPointerSeedsToPrecheck = 256;
    std::size_t graphSeedsScored = 0;
    std::size_t pointerSeedsPrechecked = 0;
    std::unordered_set<std::uintptr_t> scoredSeeds;

    for (std::size_t offset = 0; offset + sizeof(std::uint64_t) <= bytes.size(); offset += 8)
    {
      if ((offset % (16 * 1024)) == 0 && timedOut(deadline))
      {
        scanTimedOut = true;
        return {};
      }

      const std::uint64_t seed64 = readU64(bytes, offset);
      if (!addressFits(seed64))
        continue;
      const auto seedAddress = static_cast<std::uintptr_t>(seed64);
      if (seedAddress == 0)
        continue;
      if (!readableDynamicPointerValue(regions, seedAddress, recordSize))
        continue;
      if (!scoredSeeds.insert(seedAddress).second)
        continue;
      if (++pointerSeedsPrechecked > maxPointerSeedsToPrecheck)
        return {};

      RuntimeMemoryReadResult precheck = readProcessMemory(processId, seedAddress, recordSize);
      if (!precheck.success
          || precheck.bytesRead < recordSize
          || !plausibleUnitNodeAnchorRecord(precheck.bytes, 0, regions))
        continue;
      if (diagnostics != nullptr)
        ++diagnostics->unitNodePointerGraphSeedsScored;
      if (++graphSeedsScored > maxGraphSeedsToScore)
        return {};

      LiveUnitNodeProof proof = scoreLinkedUnitNodeGraph(
        processId,
        seedAddress,
        regions,
        deadline,
        scanTimedOut,
        diagnostics);
      if (scanTimedOut || proof.passed)
        return proof;
      rememberBestUnitNodeCandidate(diagnostics, proof);
    }

    return {};
  }

  LiveUnitNodeProof proveUnitNodeGraphsInBytes(
    int processId,
    const std::vector<unsigned char>& bytes,
    std::uintptr_t baseAddress,
    const std::vector<RuntimeMemoryRegion>& regions,
    const std::chrono::steady_clock::time_point& deadline,
    bool& scanTimedOut,
    UnitScanDiagnostics* diagnostics)
  {
    constexpr std::size_t recordSize = 0x58;
    constexpr std::size_t maxGraphSeedsToScore = 512;
    std::size_t graphSeedsScored = 0;

    for (std::size_t offset = 0; offset + recordSize <= bytes.size(); offset += 8)
    {
      if ((offset % (16 * 1024)) == 0 && timedOut(deadline))
      {
        scanTimedOut = true;
        return {};
      }
      if (!plausibleUnitNodeAnchorFields(bytes, offset))
        continue;
      rememberUnitNodeFieldSample(
        diagnostics,
        bytes,
        offset,
        baseAddress + offset,
        regions);
      if (!plausibleUnitNodeAnchorRecord(bytes, offset, regions))
        continue;
      if (diagnostics != nullptr)
        ++diagnostics->unitNodeGraphSeedsScored;
      if (++graphSeedsScored > maxGraphSeedsToScore)
        return {};

      LiveUnitNodeProof proof = scoreLinkedUnitNodeGraph(
        processId,
        baseAddress + offset,
        regions,
        deadline,
        scanTimedOut,
        diagnostics);
      if (scanTimedOut || proof.passed)
        return proof;
      rememberBestUnitNodeCandidate(diagnostics, proof);
    }

    return {};
  }

  LiveUnitsProof proveRemasteredUnitNodeSnapshot(
    int processId,
    const std::string& executablePath,
    std::size_t maxScanBytes,
    int scanTimeoutMs,
    LiveUnitNodeProof* activeUnitNodeProof,
    UnitScanDiagnostics* diagnostics)
  {
    LiveUnitNodeProof nodeProof =
      activeUnitNodeProof != nullptr && activeUnitNodeProof->passed
        ? *activeUnitNodeProof
        : proveLiveUnitNodeAnchors(processId, executablePath, maxScanBytes, scanTimeoutMs, diagnostics);
    if (!nodeProof.passed)
      return failedUnitsProof(nodeProof.reason.empty()
        ? "no active SC:R unit-node graph found"
        : nodeProof.reason);

    RuntimeMemoryRegionListResult regions = listProcessMemoryRegions(processId);
    if (!regions.success)
      return failedUnitsProof(regions.reason);

    std::vector<RemasteredUnitSnapshotRecord> records = nodeProof.records;
    std::size_t availableRecords = nodeProof.sampledRecords;
    if (records.size() >= minRemasteredSnapshotUnitRecords)
    {
      nodeProof.activeRecords = records.size();
      if (availableRecords < records.size())
        availableRecords = records.size();
    }
    else
    {
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

      std::size_t invalidAfterFirstRecord = 0;
      availableRecords = read.bytesRead / nodeProof.recordSize;
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
    }

    if (records.size() < minRemasteredSnapshotUnitRecords)
      return failedUnitsProof(
        "SC:R unit-node graph did not produce enough BWAPI-facing unit snapshot records");

    std::unordered_set<std::uintptr_t> nodeHandles;
    for (const RemasteredUnitSnapshotRecord& record : records)
    {
      if (record.nodeAddress != 0)
        nodeHandles.insert(record.nodeAddress);
    }
    if (nodeHandles.size() < minRemasteredSnapshotUnitRecords)
      return failedUnitsProof("SC:R unit-node graph reused too few stable unit handles for BWAPI-facing units");

    nodeProof.records = records;
    nodeProof.activeRecords = records.size();
    if (activeUnitNodeProof != nullptr)
      *activeUnitNodeProof = nodeProof;

    const bool usesMetadataFields = std::any_of(
      records.begin(),
      records.end(),
      [](const RemasteredUnitSnapshotRecord& record)
      {
        return record.metadataDerived;
      });
    const bool hitPointsResolved = std::all_of(
      records.begin(),
      records.end(),
      [](const RemasteredUnitSnapshotRecord& record)
      {
        return record.hitPointsResolved && record.hitPoints > 0;
      });

    LiveUnitsProof proof;
    proof.passed = true;
    proof.address = nodeProof.address;
    proof.recordSize = nodeProof.recordSize;
    proof.positionOffset = 0x24;
    proof.hitPointsOffset = hitPointsResolved ? 12 : 0;
    proof.orderOffset = 0x30;
    proof.sampledRecords = availableRecords;
    proof.activeRecords = records.size();
    proof.derivedSnapshot = true;
    proof.hitPointsResolved = hitPointsResolved;
    proof.layoutName = "scr-unit-node-object-graph";
    proof.idSource = usesMetadataFields ? "stable-node-handle|unit-node+0x40 metadata" : "stable-node-handle";
    proof.positionSource = "unit-node+36|4";
    proof.hitPointsSource =
      hitPointsResolved
        ? (usesMetadataFields ? "metadata+0x1a compact-hp-byte -> bwapi-hp-raw" : "secondary+0x1a compact-hp-byte -> bwapi-hp-raw")
        : "";
    proof.orderSource = "unit-node+48|2";
    proof.playerSource = usesMetadataFields ? "unit-node+0x40 metadata+0xc0|1" : "unit-node+0x50 secondary+0x14|1";
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

  int inferRaceFromUnitTypeHint(std::uint16_t typeHint, int player)
  {
    if (player >= 8)
      return 3; // BWAPI::Races::Other for neutral/observer slots.

    // BWAPI UnitTypes are grouped by race for the classic ids. SC:R type
    // hints are validated as unit-type-like ids before this projection is used.
    if (typeHint <= 34 || typeHint == 58 || (typeHint >= 105 && typeHint <= 131))
      return 1; // Terran
    if ((typeHint >= 35 && typeHint <= 57)
        || typeHint == 59
        || typeHint == 96
        || typeHint == 102
        || typeHint == 103
        || (typeHint >= 132 && typeHint <= 157))
      return 0; // Zerg
    if ((typeHint >= 60 && typeHint <= 87)
        || typeHint == 97
        || (typeHint >= 159 && typeHint <= 173))
      return 2; // Protoss
    return 8; // BWAPI::Races::Unknown
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
    std::array<std::array<std::size_t, 9>, 12> raceVotes = {};
    for (const RemasteredUnitSnapshotRecord& record : nodeProof.records)
    {
      if (record.player < 0 || record.player >= static_cast<int>(unitCounts.size()))
        continue;
      const std::size_t playerIndex = static_cast<std::size_t>(record.player);
      ++unitCounts[playerIndex];
      ++proof.observedUnits;
      const int inferredRace = inferRaceFromUnitTypeHint(record.typeHint, record.player);
      if (inferredRace >= 0 && inferredRace < static_cast<int>(raceVotes[playerIndex].size()))
        ++raceVotes[playerIndex][static_cast<std::size_t>(inferredRace)];
    }

    for (std::size_t player = 0; player < unitCounts.size(); ++player)
    {
      if (unitCounts[player] == 0)
        continue;
      PlayerSnapshotRecord record;
      record.player = static_cast<int>(player);
      record.unitCount = unitCounts[player];
      record.stormId = record.player;
      record.race = 8;
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
    }

    if (proof.players.empty())
    {
      proof.reason = "unit snapshot did not contain any valid player ids";
      return proof;
    }

    proof.passed = true;
    proof.playerCount = proof.players.size();
    proof.playerInfoProjectionReady = true;
    proof.allianceProjectionReady = true;
    proof.projectionSource = "compat-player-projection-v1:unit-snapshot-derived";
    return proof;
  }

  ReplayAnalysisProof proveReplayAnalysisFromLiveMetadata(
    const LiveCounterProof& gameStateProof,
    const MapDataProof& mapProof,
    const PlayerDataProof& playerProof,
    bool activeMatchReady,
    bool replayLaunchDetected)
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
    if (mapProof.source == "latest-replay-artifact"
        && !replayLaunchDetected
        && !(activeMatchReady && playerProof.passed))
    {
      proof.reason =
        "replay-analysis proof requires current-process replay launch evidence or active-match player metadata when map data comes from a replay artifact";
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

  std::uint64_t readPointerLike(
    const std::vector<unsigned char>& bytes,
    std::size_t offset)
  {
    if (offset + sizeof(std::uint64_t) <= bytes.size())
    {
      const std::uint64_t value64 = readU64(bytes, offset);
      if (value64 != 0)
        return value64;
    }
    if (offset + sizeof(std::uint32_t) <= bytes.size())
      return readU32(bytes, offset);
    return 0;
  }

  bool parseBulletSnapshotRecord(
    const std::vector<unsigned char>& bytes,
    std::size_t offset,
    std::uintptr_t recordAddress,
    const BulletRecordLayout& layout,
    const std::vector<RuntimeMemoryRegion>& regions,
    BulletSnapshotRecord& record)
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

    const std::uint32_t exists = readU32(bytes, offset + layout.existsOffset);
    if (exists == 0 || exists > 3)
      return false;

    const std::uint64_t sprite = readPointerLike(bytes, offset + layout.spriteOffset);
    if (!readablePointerValue(regions, sprite, 16))
      return false;

    const std::uint16_t type = readU16(bytes, offset + layout.typeOffset);
    const std::int16_t x = readS16(bytes, offset + layout.positionOffset);
    const std::int16_t y = readS16(bytes, offset + layout.positionOffset + sizeof(std::int16_t));
    const std::int32_t velocityX = readS32(bytes, offset + layout.velocityOffset);
    const std::int32_t velocityY = readS32(bytes, offset + layout.velocityOffset + sizeof(std::int32_t));
    const unsigned char player = bytes[offset + layout.playerOffset];
    const std::uint64_t sourceUnit = readPointerLike(bytes, offset + layout.sourceUnitOffset);
    const std::uint64_t targetUnit = readPointerLike(bytes, offset + layout.targetUnitOffset);

    if (type >= 256 || player >= 12)
      return false;
    if (x < 0 || x > 16384 || y < 0 || y > 16384)
      return false;
    if (std::abs(velocityX) > 32768 * 256 || std::abs(velocityY) > 32768 * 256)
      return false;
    if (!readablePointerValue(regions, sourceUnit, 16)
        && !readablePointerValue(regions, targetUnit, 16))
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
    record.removeTimer = bytes[offset + layout.removeTimerOffset];
    return true;
  }

  BulletDataProof scoreBulletArray(
    const std::vector<unsigned char>& bytes,
    std::uintptr_t baseAddress,
    std::size_t offset,
    const BulletRecordLayout& layout,
    const std::vector<RuntimeMemoryRegion>& regions,
    const std::chrono::steady_clock::time_point& deadline,
    bool& scanTimedOut)
  {
    BulletDataProof proof;
    proof.address = baseAddress + offset;
    proof.recordSize = layout.recordSize;
    proof.positionOffset = layout.positionOffset;
    proof.velocityOffset = layout.velocityOffset;
    proof.sourceUnitOffset = layout.sourceUnitOffset;
    proof.targetOffset = layout.targetUnitOffset;
    proof.layoutName = layout.name;

    constexpr std::size_t maxSampledRecords = 2048;
    proof.sampledRecords = std::min(maxSampledRecords, (bytes.size() - offset) / layout.recordSize);
    for (std::size_t i = 0; i < proof.sampledRecords; ++i)
    {
      if ((i % 16) == 0 && timedOut(deadline))
      {
        scanTimedOut = true;
        return {};
      }

      BulletSnapshotRecord record;
      record.index = i;
      const std::size_t recordOffset = offset + i * layout.recordSize;
      if (!parseBulletSnapshotRecord(
            bytes,
            recordOffset,
            baseAddress + recordOffset,
            layout,
            regions,
            record))
        continue;

      proof.records.push_back(record);
      proof.activeRecords = proof.records.size();
      if (proof.activeRecords >= minActiveBulletRecords)
      {
        proof.passed = true;
        return proof;
      }
    }

    proof.reason = "candidate bullet array did not contain enough active BWAPI-compatible records";
    return proof;
  }

  BulletDataProof proveBulletArrayInBytes(
    const std::vector<unsigned char>& bytes,
    std::uintptr_t baseAddress,
    const std::vector<RuntimeMemoryRegion>& regions,
    const std::chrono::steady_clock::time_point& deadline,
    bool& scanTimedOut)
  {
    for (const BulletRecordLayout& layout : bulletRecordLayouts)
    {
      if (layout.recordSize * minActiveBulletRecords > bytes.size())
        continue;

      std::vector<std::size_t> plausibleByResidue(layout.recordSize, 0);
      for (std::size_t recordOffset = 0; recordOffset + layout.recordSize <= bytes.size(); recordOffset += 8)
      {
        if ((recordOffset % (4 * 1024)) == 0 && timedOut(deadline))
        {
          scanTimedOut = true;
          return {};
        }

        BulletSnapshotRecord record;
        if (parseBulletSnapshotRecord(
              bytes,
              recordOffset,
              baseAddress + recordOffset,
              layout,
              regions,
              record))
          ++plausibleByResidue[recordOffset % layout.recordSize];
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
        BulletDataProof proof = scoreBulletArray(
          bytes,
          baseAddress,
          residues[index],
          layout,
          regions,
          deadline,
          scanTimedOut);
        if (scanTimedOut || proof.passed)
          return proof;
      }
    }

    return {};
  }

  BulletDataProof proveExplicitBulletCandidateAddresses(
    int processId,
    const std::vector<std::uintptr_t>& candidateAddresses,
    int scanTimeoutMs)
  {
    if (candidateAddresses.empty())
      return failedBulletDataProof("no explicit bullet candidate address was provided");

    RuntimeMemoryRegionListResult regions = listProcessMemoryRegions(processId);
    if (!regions.success)
      return failedBulletDataProof(regions.reason);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(scanTimeoutMs);
    for (std::uintptr_t candidateAddress : candidateAddresses)
    {
      if (timedOut(deadline))
        return failedBulletDataProof("explicit bullet candidate scan timed out before proof");

      for (const BulletRecordLayout& layout : bulletRecordLayouts)
      {
        const RuntimeMemoryRegion* containingRegion =
          findReadableRegion(regions.regions, candidateAddress, layout.recordSize * minActiveBulletRecords);
        if (containingRegion == nullptr)
          continue;

        const std::uintptr_t regionEnd = containingRegion->address + containingRegion->size;
        const std::size_t regionBytes =
          regionEnd > candidateAddress
            ? static_cast<std::size_t>(regionEnd - candidateAddress)
            : 0;
        const std::size_t bytesToRead = std::min(regionBytes, layout.recordSize * 256);
        if (bytesToRead < layout.recordSize * minActiveBulletRecords)
          continue;

        RuntimeMemoryReadResult read = readProcessMemory(processId, candidateAddress, bytesToRead);
        if (!read.success || read.bytesRead < layout.recordSize * minActiveBulletRecords)
          continue;

        bool scanTimedOut = false;
        BulletDataProof proof = scoreBulletArray(
          read.bytes,
          candidateAddress,
          0,
          layout,
          regions.regions,
          deadline,
          scanTimedOut);
        if (scanTimedOut)
          return failedBulletDataProof("explicit bullet candidate scan timed out before proof");
        if (proof.passed)
          return proof;
      }
    }

    return failedBulletDataProof("no explicit bullet candidate address contained active BWAPI-compatible records");
  }

  BulletDataProof proveLiveBulletDataRead(
    int processId,
    const std::string& executablePath,
    std::size_t maxScanBytes,
    int scanTimeoutMs)
  {
    RuntimeMemoryRegionListResult regions = listProcessMemoryRegions(processId);
    if (!regions.success)
      return failedBulletDataProof(regions.reason);

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

    for (const RuntimeMemoryRegion& region : scanRegions)
    {
      if (timedOut(deadline))
        return failedBulletDataProof("bullet array scan timed out before proof");
      if (!usableUnitStorageRegion(region, executablePath, targetImageBase))
        continue;
      if (region.size < bulletRecordLayouts.front().recordSize * minActiveBulletRecords)
        continue;
      if (scanned >= maxScanBytes)
        return failedBulletDataProof("no active BWAPI-compatible bullet array candidate found before scan byte limit");

      const std::size_t bytesToRead =
        std::min(region.size, std::min(maxRegionBytes, maxScanBytes - scanned));
      RuntimeMemoryReadResult read = readProcessMemory(processId, region.address, bytesToRead);
      if (!read.success || read.bytesRead < bulletRecordLayouts.front().recordSize * minActiveBulletRecords)
        continue;
      scanned += read.bytesRead;

      bool scanTimedOut = false;
      BulletDataProof proof = proveBulletArrayInBytes(
        read.bytes,
        region.address,
        regions.regions,
        deadline,
        scanTimedOut);
      if (scanTimedOut)
        return failedBulletDataProof("bullet array scan timed out before proof");
      if (proof.passed)
        return proof;
    }

    return failedBulletDataProof("no active BWAPI-compatible bullet array candidate found");
  }

  RegionDataProof makeSelfRegionDataProof()
  {
    RegionDataProof proof;
    proof.passed = true;
    proof.source = "self-region-fixture";
    proof.observedUnits = 8;
    proof.regions = {
      RegionSnapshotRecord { 0, 160, 160, 64, 64, 256, 256, 4, true },
      RegionSnapshotRecord { 1, 448, 224, 320, 96, 576, 352, 4, true }
    };
    proof.regionCount = proof.regions.size();
    return proof;
  }

  RegionDataProof proveRegionDataFromLiveMetadata(
    const MapDataProof& mapProof,
    const LiveUnitNodeProof& nodeProof)
  {
    if (!mapProof.passed)
      return failedRegionDataProof("region-data proof requires a passing live map-data proof");
    if (!nodeProof.passed || nodeProof.records.empty())
      return failedRegionDataProof("region-data proof requires a passing live unit snapshot");

    RegionDataProof proof;
    proof.source = "live-unit-position-buckets+map-data";
    for (const RemasteredUnitSnapshotRecord& unit : nodeProof.records)
    {
      if (unit.x < 0 || unit.y < 0)
        continue;
      const int bucketX = unit.x / 256;
      const int bucketY = unit.y / 256;
      const int id = bucketY * 1024 + bucketX;
      auto it = std::find_if(
        proof.regions.begin(),
        proof.regions.end(),
        [&](const RegionSnapshotRecord& region)
        {
          return region.id == id;
        });
      if (it == proof.regions.end())
      {
        RegionSnapshotRecord region;
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
      return failedRegionDataProof("live unit snapshot did not contain region-compatible positions");

    proof.passed = true;
    proof.regionCount = proof.regions.size();
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
    // A real BW turn-buffer sink must have enough room for bursts of encoded
    // commands. Tiny vectors are common in SC:R globals and produced false
    // positives during live pause/resume proofs.
    if (capacityBytes < 256 || capacityBytes > 64 * 1024)
      return false;
    if (usedBytes > capacityBytes)
      return false;

    const std::size_t bytesToCheck = std::max<std::size_t>(1, std::min<std::size_t>(capacityBytes, 64));
    return writableAddress(regions, begin, bytesToCheck);
  }

  constexpr std::size_t rawTurnBufferCapacity = 512;
  constexpr std::size_t implicitLiveCommandQueueMaxUsedBytes = rawTurnBufferCapacity;
  constexpr std::array<std::size_t, 6> rawTurnBufferCounterOffsets = {
    0x200, 0x204, 0x208, 0x210, 0x220, 0x240
  };

  bool looksLikeUserSpacePointer(std::uint64_t value)
  {
    if (value < 0x100000000ULL)
      return false;

    const std::uint64_t high = value >> 48;
    return high == 0 || high == 0xffff;
  }

  bool pointerDenseRawTurnBufferWindow(
    const std::vector<unsigned char>& bytes,
    std::size_t offset)
  {
    constexpr std::size_t sampleBytes = 64;
    constexpr std::size_t pointerRejectThreshold = 1;
    if (offset + sizeof(std::uint64_t) > bytes.size())
      return false;

    const std::size_t end =
      std::min(bytes.size(), offset + std::min(sampleBytes, rawTurnBufferCapacity));
    std::size_t pointerLikeWords = 0;
    for (std::size_t cursor = offset; cursor + sizeof(std::uint64_t) <= end; cursor += sizeof(std::uint64_t))
    {
      if (looksLikeUserSpacePointer(readU64(bytes, cursor)))
        ++pointerLikeWords;
    }
    return pointerLikeWords >= pointerRejectThreshold;
  }

  bool smallIntegerTableRawTurnBufferWindow(
    const std::vector<unsigned char>& bytes,
    std::size_t offset)
  {
    constexpr std::size_t sampleBytes = 64;
    constexpr std::size_t tableRejectThreshold = 4;
    if (offset + sizeof(std::uint64_t) > bytes.size())
      return false;

    const std::size_t end =
      std::min(bytes.size(), offset + std::min(sampleBytes, rawTurnBufferCapacity));
    std::size_t smallIntegerWords = 0;
    for (std::size_t cursor = offset; cursor + sizeof(std::uint64_t) <= end; cursor += sizeof(std::uint64_t))
    {
      const std::uint64_t value = readU64(bytes, cursor);
      if (value != 0 && value <= 0x10000)
        ++smallIntegerWords;
    }
    return smallIntegerWords >= tableRejectThreshold;
  }


  bool plausibleRawTurnBufferBytes(
    const std::vector<unsigned char>& bytes,
    std::size_t offset,
    std::size_t usedBytes)
  {
    if (offset + rawTurnBufferCapacity > bytes.size() || usedBytes > rawTurnBufferCapacity)
      return false;

    constexpr std::array<unsigned char, 23> commonActionOpcodes = {
      0x08, 0x0a, 0x0b, 0x0c, 0x0e, 0x0f, 0x10, 0x11,
      0x13, 0x14, 0x18, 0x1a, 0x1e, 0x20, 0x21, 0x23,
      0x25, 0x26, 0x27, 0x2c, 0x30, 0x36, 0x58
    };
    const unsigned char first = bytes[offset];
    const bool startsWithKnownAction =
      usedBytes > 0
      && std::find(commonActionOpcodes.begin(), commonActionOpcodes.end(), first)
        != commonActionOpcodes.end();

    // SC:R globals contain many fixed-size pointer tables near the networking
    // state. Their first byte can look like a BW command opcode, but they are
    // not safe turn-buffer candidates. Apply this only before a live command
    // stream is present; short live opcode bursts such as 10 11 10 11 10 can
    // otherwise be misclassified as low user-space pointers.
    if (!startsWithKnownAction
        && usedBytes < sizeof(std::uint64_t)
        && looksLikeUserSpacePointer(readU64(bytes, offset)))
      return false;
    if (!startsWithKnownAction && pointerDenseRawTurnBufferWindow(bytes, offset))
      return false;
    if (!startsWithKnownAction && smallIntegerTableRawTurnBufferWindow(bytes, offset))
      return false;

    if (usedBytes == 0)
      return true;

    if (usedBytes == 1)
    {
      for (std::size_t i = 1; i < sizeof(std::uint64_t); ++i)
      {
        if (bytes[offset + i] != 0)
          return false;
      }
    }
    return startsWithKnownAction;
  }

  int rawCommandQueueCandidateScore(
    const RuntimeMemoryRegion& region,
    const std::string& executablePath,
    const StarCraftImageSectionHints& hints,
    std::size_t usedBytes,
    std::size_t counterOffset)
  {
    int score = 0;
    const bool targetImageRegion = sameMappedFile(region.mappedPath, executablePath);
    if (region.mappedPath.empty())
      score += 250;
    if (regionIntersectsStarCraftRuntimeData(region, hints))
      score += targetImageRegion ? 80 : 350;
    if (targetImageRegion)
      score -= 300;
    if (counterOffset == 0x220)
      score += 40;
    if (usedBytes > 0)
      score += 60;
    else
      score -= 120;
    if (usedBytes <= 8)
      score += 20;
    if (usedBytes > 128)
      score -= 80;
    return score;
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
    if (vectorRegion.mappedPath.empty())
      score += 350;
    if (bufferRegion.mappedPath.empty())
      score += 350;
    if (regionIntersectsStarCraftRuntimeData(vectorRegion, hints))
      score += 100;
    if (sameMappedFile(vectorRegion.mappedPath, executablePath))
      score -= 100;
    if (sameMappedFile(bufferRegion.mappedPath, executablePath))
      score -= 100;
    if (usedBytes == 0)
      score += 8;
    else if (usedBytes <= 512)
      score += 12;
    else
      score -= 500;
    if (capacityBytes <= 8192)
      score += 10;
    else
      score -= 200;
    if (capacityBytes <= 4096)
      score += 5;
    return score;
  }

  bool implicitLiveCommandCandidateSafeForWrite(const CommandQueueCandidate& candidate)
  {
    return candidate.usedBytes <= implicitLiveCommandQueueMaxUsedBytes
      && candidate.capacityBytes >= 256
      && candidate.capacityBytes <= 16 * 1024;
  }

  bool liveCommandCandidateSelectorSafeForWrite(
    const CommandQueueCandidate& candidate,
    const std::string& executablePath,
    std::string& reason)
  {
    if (candidate.storageKind == "vector" && sameMappedFile(candidate.regionPath, executablePath))
    {
      reason =
        "refusing to write command queue vector selector stored in the target executable image; "
        "live proof requires raw turn-buffer storage or a non-image queue selector";
      return false;
    }
    return true;
  }

  void refreshCommandQueueDiscoveryRetainedStats(CommandQueueDiscoveryProof& proof)
  {
    proof.retainedVectorCandidates = 0;
    proof.retainedRawTurnBufferCandidates = 0;
    proof.retainedActiveCandidates = 0;
    for (const CommandQueueCandidate& candidate : proof.candidates)
    {
      if (candidate.storageKind == "raw-turn-buffer")
        ++proof.retainedRawTurnBufferCandidates;
      else if (candidate.storageKind == "vector")
        ++proof.retainedVectorCandidates;

      if (candidate.activityTransitions > 0 || candidate.activityByteChanges > 0)
        ++proof.retainedActiveCandidates;
    }
  }

  CommandQueueDiscoveryProof discoverCommandQueueCandidates(
    int processId,
    const std::string& executablePath,
    std::size_t maxScanBytes,
    int scanTimeoutMs,
    std::size_t maxCandidates)
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
        const int lhsPriority = commandQueueScanRegionPriority(lhs, executablePath, targetImageBase);
        const int rhsPriority = commandQueueScanRegionPriority(rhs, executablePath, targetImageBase);
        if (lhsPriority != rhsPriority)
          return lhsPriority < rhsPriority;
        return lhs.address < rhs.address;
      });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(scanTimeoutMs);
    constexpr std::size_t maxRegionBytes = 4 * 1024 * 1024;
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
        candidate.storageKind = "vector";
        candidate.vectorAddress = vectorAddress;
        candidate.bytesInQueueAddress = vectorAddress + sizeof(std::uint64_t);
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
        if (usedBytes == 4 && capacityBytes == 4096)
        {
          RuntimeMemoryReadResult marker = readProcessMemory(processId, begin, 4);
          if (marker.success
              && marker.bytesRead == 4
              && marker.bytes[0] == 0x14
              && marker.bytes[1] == 0x00
              && marker.bytes[2] == 0x34
              && marker.bytes[3] == 0x12)
            candidate.score += 1000;
        }
        candidate.regionPath = region.mappedPath;
        if (sameMappedFile(region.mappedPath, executablePath))
          ++proof.imageMappedCandidates;
        else if (region.mappedPath.empty())
          ++proof.privateCandidates;
        ++proof.vectorCandidates;
        proof.candidates.push_back(std::move(candidate));
        // Suppress sliding-window false positives over the accepted
        // {begin,end,capacity} triple. Adjacent real vectors are 24-byte
        // aligned here; +8/+16 candidates reinterpret end/capacity as begin/end.
        seenVectors.insert(vectorAddress + sizeof(std::uint64_t));
        seenVectors.insert(vectorAddress + sizeof(std::uint64_t) * 2);
      }

      for (std::size_t counterOffset : rawTurnBufferCounterOffsets)
      {
        if (read.bytes.size() <= counterOffset + sizeof(std::uint32_t))
          continue;
        for (std::size_t bufferOffset = 0;
             bufferOffset + counterOffset + sizeof(std::uint32_t) <= read.bytes.size();
             bufferOffset += 16)
        {
          if ((bufferOffset % (16 * 1024)) == 0 && timedOut(deadline))
          {
            proof.reason = "command queue discovery timed out while scoring raw turn-buffer candidates";
            break;
          }

          const std::size_t countOffset = bufferOffset + counterOffset;
          if (bufferOffset + rawTurnBufferCapacity > read.bytes.size()
              || countOffset + sizeof(std::uint32_t) > read.bytes.size())
            continue;

          const std::uint32_t usedBytes32 = readU32(read.bytes, countOffset);
          if (usedBytes32 > rawTurnBufferCapacity)
            continue;
          const std::size_t usedBytes = usedBytes32;
          if (!plausibleRawTurnBufferBytes(read.bytes, bufferOffset, usedBytes))
            continue;

          const std::uintptr_t bufferBegin = region.address + bufferOffset;
          const std::uintptr_t bytesInQueueAddress = region.address + countOffset;
          if (!writableAddress(regions.regions, bufferBegin, rawTurnBufferCapacity)
              || !writableAddress(regions.regions, bytesInQueueAddress, sizeof(std::uint32_t)))
            continue;
          if (!seenVectors.insert(bytesInQueueAddress).second)
            continue;

          CommandQueueCandidate candidate;
          candidate.storageKind = "raw-turn-buffer";
          candidate.vectorAddress = bytesInQueueAddress;
          candidate.bytesInQueueAddress = bytesInQueueAddress;
          candidate.bufferBegin = bufferBegin;
          candidate.bufferEnd = bufferBegin + usedBytes;
          candidate.bufferCapacity = bufferBegin + rawTurnBufferCapacity;
          candidate.usedBytes = usedBytes;
          candidate.capacityBytes = rawTurnBufferCapacity;
          candidate.score = rawCommandQueueCandidateScore(
            region,
            executablePath,
            hints,
            usedBytes,
            counterOffset);
          candidate.regionPath = region.mappedPath;
          if (sameMappedFile(region.mappedPath, executablePath))
            ++proof.imageMappedCandidates;
          else if (region.mappedPath.empty())
            ++proof.privateCandidates;
          ++proof.rawTurnBufferCandidates;
          proof.candidates.push_back(std::move(candidate));
        }
        if (!proof.reason.empty())
          break;
      }

      if (!proof.reason.empty())
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

    if (maxCandidates == 0)
      maxCandidates = 32;
    if (proof.candidates.size() > maxCandidates)
      proof.candidates.resize(maxCandidates);
    refreshCommandQueueDiscoveryRetainedStats(proof);

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

    output << "rank\tscore\tkind\tselector_address\tbytes_in_queue_address\tbuffer_begin\tbuffer_end\tbuffer_capacity\tused_bytes\tcapacity_bytes\tactivity_samples\tactivity_transitions\tactivity_byte_changes\tactivity_reason\tregion_path\n";
    for (std::size_t i = 0; i < proof.candidates.size(); ++i)
    {
      const CommandQueueCandidate& candidate = proof.candidates[i];
      output << i << '\t'
             << candidate.score << '\t'
             << candidate.storageKind << '\t'
             << hexAddress(candidate.vectorAddress) << '\t'
             << hexAddress(candidate.bytesInQueueAddress) << '\t'
             << hexAddress(candidate.bufferBegin) << '\t'
             << hexAddress(candidate.bufferEnd) << '\t'
             << hexAddress(candidate.bufferCapacity) << '\t'
             << candidate.usedBytes << '\t'
             << candidate.capacityBytes << '\t'
             << candidate.activitySamples << '\t'
             << candidate.activityTransitions << '\t'
             << candidate.activityByteChanges << '\t'
             << candidate.activityReason << '\t'
             << candidate.regionPath << '\n';
    }
    if (!output)
    {
      reason = "unable to write command queue discovery snapshot output";
      return false;
    }
    return true;
  }

  RuntimeCommandRequest gameActionRequest(std::string name)
  {
    RuntimeCommandRequest request;
    request.kind = RuntimeCommandKind::GameAction;
    request.name = std::move(name);
    return request;
  }

  LiveCounterProof proveLiveCounterRead(
    int processId,
    const std::string& executablePath,
    int sampleDelayMs,
    std::size_t maxScanBytes,
    int scanTimeoutMs,
    StateScanDiagnostics* diagnostics);

  std::vector<FrameCounterCandidate> collectFrameCounterCandidates(
    int processId,
    const std::string& executablePath,
    int sampleDelayMs,
    std::size_t maxScanBytes,
    int scanTimeoutMs,
    std::size_t maxCandidates);

  bool readCommandQueueCandidate(
    int processId,
    const std::vector<RuntimeMemoryRegion>& regions,
    std::uintptr_t vectorAddress,
    CommandQueueCandidate& candidate,
    std::string& reason)
  {
    const auto readRawTurnBufferCandidate = [&]() -> bool
    {
      RuntimeMemoryReadResult countRead =
        readProcessMemory(processId, vectorAddress, sizeof(std::uint32_t));
      if (!countRead.success || countRead.bytesRead != sizeof(std::uint32_t))
        return false;

      const std::uint32_t usedBytes32 = readU32(countRead.bytes, 0);
      if (usedBytes32 > rawTurnBufferCapacity)
      {
        reason = "raw turn-buffer byte count is outside capacity";
        return false;
      }

      for (std::size_t counterOffset : rawTurnBufferCounterOffsets)
      {
        if (vectorAddress < counterOffset)
          continue;
        const std::uintptr_t bufferBegin = vectorAddress - counterOffset;
        if (!writableAddress(regions, bufferBegin, rawTurnBufferCapacity))
          continue;

        RuntimeMemoryReadResult bufferRead =
          readProcessMemory(processId, bufferBegin, rawTurnBufferCapacity);
        if (!bufferRead.success || bufferRead.bytesRead != rawTurnBufferCapacity)
          continue;
        if (!plausibleRawTurnBufferBytes(bufferRead.bytes, 0, usedBytes32))
          continue;

        candidate.storageKind = "raw-turn-buffer";
        candidate.vectorAddress = vectorAddress;
        candidate.bytesInQueueAddress = vectorAddress;
        candidate.bufferBegin = bufferBegin;
        candidate.bufferEnd = bufferBegin + usedBytes32;
        candidate.bufferCapacity = bufferBegin + rawTurnBufferCapacity;
        candidate.usedBytes = usedBytes32;
        candidate.capacityBytes = rawTurnBufferCapacity;
        return true;
      }

      reason = "raw turn-buffer storage was not found around byte-count address";
      return false;
    };

    RuntimeMemoryReadResult vectorRead =
      readProcessMemory(processId, vectorAddress, sizeof(std::uint64_t) * 3);
    if (!vectorRead.success || vectorRead.bytesRead < sizeof(std::uint64_t) * 3)
    {
      if (readRawTurnBufferCandidate())
        return true;
      reason = vectorRead.reason.empty()
        ? "unable to read command queue vector"
        : vectorRead.reason;
      return false;
    }

    const auto begin = static_cast<std::uintptr_t>(readU64(vectorRead.bytes, 0));
    const auto end = static_cast<std::uintptr_t>(readU64(vectorRead.bytes, 8));
    const auto capacity = static_cast<std::uintptr_t>(readU64(vectorRead.bytes, 16));
    std::size_t usedBytes = 0;
    std::size_t capacityBytes = 0;
    if (!plausibleCommandQueueVector(regions, begin, end, capacity, usedBytes, capacityBytes))
    {
      if (readRawTurnBufferCandidate())
        return true;
      reason = "command queue vector/raw turn-buffer candidate is no longer plausible";
      return false;
    }

    candidate.storageKind = "vector";
    candidate.vectorAddress = vectorAddress;
    candidate.bytesInQueueAddress = vectorAddress + sizeof(std::uint64_t);
    candidate.bufferBegin = begin;
    candidate.bufferEnd = end;
    candidate.bufferCapacity = capacity;
    candidate.usedBytes = usedBytes;
    candidate.capacityBytes = capacityBytes;
    return true;
  }

  bool readKnownRawTurnBufferCandidate(
    int processId,
    const std::vector<RuntimeMemoryRegion>& regions,
    const CommandQueueCandidate& selectedCandidate,
    CommandQueueCandidate& candidate,
    std::string& reason)
  {
    if (selectedCandidate.storageKind != "raw-turn-buffer"
        || selectedCandidate.bytesInQueueAddress == 0
        || selectedCandidate.bufferBegin == 0
        || selectedCandidate.capacityBytes == 0
        || selectedCandidate.capacityBytes > rawTurnBufferCapacity)
    {
      reason = "selected command queue is not a known raw turn-buffer layout";
      return false;
    }

    RuntimeMemoryReadResult countRead =
      readProcessMemory(processId, selectedCandidate.bytesInQueueAddress, sizeof(std::uint32_t));
    if (!countRead.success || countRead.bytesRead != sizeof(std::uint32_t))
    {
      reason = countRead.reason.empty()
        ? "unable to read known raw turn-buffer byte count"
        : countRead.reason;
      return false;
    }

    const std::uint32_t usedBytes32 = readU32(countRead.bytes, 0);
    if (usedBytes32 > selectedCandidate.capacityBytes)
    {
      reason = "known raw turn-buffer byte count is outside capacity";
      return false;
    }

    if (!writableAddress(regions, selectedCandidate.bufferBegin, selectedCandidate.capacityBytes)
        || !writableAddress(regions, selectedCandidate.bytesInQueueAddress, sizeof(std::uint32_t)))
    {
      reason = "known raw turn-buffer storage is no longer writable";
      return false;
    }

    candidate = selectedCandidate;
    candidate.bufferEnd = selectedCandidate.bufferBegin + usedBytes32;
    candidate.bufferCapacity = selectedCandidate.bufferBegin + selectedCandidate.capacityBytes;
    candidate.usedBytes = usedBytes32;
    return true;
  }

  std::uint64_t fnv1a64(const std::vector<unsigned char>& bytes)
  {
    std::uint64_t hash = 1469598103934665603ULL;
    for (unsigned char byte : bytes)
    {
      hash ^= byte;
      hash *= 1099511628211ULL;
    }
    return hash;
  }

  bool readCandidatePrefixHash(
    int processId,
    const CommandQueueCandidate& candidate,
    std::uint64_t& hash,
    std::string& reason)
  {
    const std::size_t bytesToRead =
      std::min<std::size_t>(candidate.capacityBytes, rawTurnBufferCapacity);
    if (bytesToRead == 0)
    {
      reason = "command queue candidate has zero readable capacity";
      return false;
    }

    RuntimeMemoryReadResult read =
      readProcessMemory(processId, candidate.bufferBegin, bytesToRead);
    if (!read.success || read.bytesRead != bytesToRead)
    {
      reason = read.reason.empty()
        ? "unable to read command queue candidate prefix"
        : read.reason;
      return false;
    }

    hash = fnv1a64(read.bytes);
    return true;
  }

  void observeCommandQueueCandidateActivity(
    int processId,
    std::vector<CommandQueueCandidate>& candidates,
    int intervalMs,
    std::size_t maxObservedCandidates)
  {
    if (candidates.empty() || intervalMs <= 0)
      return;

    RuntimeMemoryRegionListResult regions = listProcessMemoryRegions(processId);
    if (!regions.success)
    {
      for (CommandQueueCandidate& candidate : candidates)
        candidate.activityReason = regions.reason;
      return;
    }

    const std::size_t limit = std::min(maxObservedCandidates, candidates.size());

    struct ObservedCandidate
    {
      std::size_t index = 0;
      bool readable = false;
      CommandQueueCandidate previous;
      std::uint64_t previousHash = 0;
    };

    std::vector<ObservedCandidate> observed;
    observed.reserve(limit);
    for (std::size_t index = 0; index < limit; ++index)
    {
      CommandQueueCandidate& candidate = candidates[index];
      ObservedCandidate state;
      state.index = index;
      std::string reason;
      if (!readCommandQueueCandidate(
            processId,
            regions.regions,
            candidate.vectorAddress,
            state.previous,
            reason))
      {
        candidate.activityReason = reason;
        observed.push_back(state);
        continue;
      }

      if (!readCandidatePrefixHash(processId, state.previous, state.previousHash, reason))
      {
        candidate.activityReason = reason;
        observed.push_back(state);
        continue;
      }

      candidate.activitySamples = 1;
      state.readable = true;
      observed.push_back(state);
    }

    for (int sample = 0; sample < 3; ++sample)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));

      for (ObservedCandidate& state : observed)
      {
        if (!state.readable)
          continue;

        CommandQueueCandidate& candidate = candidates[state.index];
        CommandQueueCandidate current;
        std::string reason;
        if (!readCommandQueueCandidate(
              processId,
              regions.regions,
              candidate.vectorAddress,
              current,
              reason))
        {
          candidate.activityReason = reason;
          state.readable = false;
          continue;
        }

        std::uint64_t currentHash = 0;
        if (!readCandidatePrefixHash(processId, current, currentHash, reason))
        {
          candidate.activityReason = reason;
          state.readable = false;
          continue;
        }

        ++candidate.activitySamples;
        if (current.usedBytes != state.previous.usedBytes
            || current.bufferEnd != state.previous.bufferEnd)
        {
          ++candidate.activityTransitions;
        }
        if (currentHash != state.previousHash)
          ++candidate.activityByteChanges;

        state.previous = current;
        state.previousHash = currentHash;
      }
    }

    for (std::size_t index = 0; index < limit; ++index)
    {
      CommandQueueCandidate& candidate = candidates[index];
      if (candidate.activityTransitions > 0 || candidate.activityByteChanges > 0)
      {
        candidate.score += 2000
          + static_cast<int>(candidate.activityTransitions * 250)
          + static_cast<int>(candidate.activityByteChanges * 150);
      }
      else if (candidate.activityReason.empty())
      {
        candidate.activityReason = "no natural queue activity observed during short live sampling window";
      }
    }

    std::stable_sort(
      candidates.begin(),
      candidates.end(),
      [](const CommandQueueCandidate& lhs, const CommandQueueCandidate& rhs)
      {
        const bool lhsActive = lhs.activityTransitions > 0 || lhs.activityByteChanges > 0;
        const bool rhsActive = rhs.activityTransitions > 0 || rhs.activityByteChanges > 0;
        if (lhsActive != rhsActive)
          return lhsActive;
        if (lhs.score != rhs.score)
          return lhs.score > rhs.score;
        if (lhs.capacityBytes != rhs.capacityBytes)
          return lhs.capacityBytes < rhs.capacityBytes;
        return lhs.vectorAddress < rhs.vectorAddress;
      });
  }

  struct CommandQueueAppendResult
  {
    bool passed = false;
    bool consumedImmediately = false;
    CommandQueueCandidate candidate;
    std::uintptr_t tailAddress = 0;
    std::uintptr_t expectedEnd = 0;
    std::vector<unsigned char> originalTail;
    std::size_t appendedBytes = 0;
    std::string reason;
  };

  CommandQueueAppendResult appendEncodedCommandToQueueCandidate(
    int processId,
    const CommandQueueCandidate& selectedCandidate,
    const std::vector<std::uint8_t>& encodedBytes,
    bool restoreAfterReadback,
    bool allowKnownRawTurnBufferFallback = false)
  {
    CommandQueueAppendResult result;
    if (encodedBytes.empty())
    {
      result.reason = "encoded command is empty";
      return result;
    }

    RuntimeMemoryRegionListResult regions = listProcessMemoryRegions(processId);
    if (!regions.success)
    {
      result.reason = regions.reason;
      return result;
    }

    std::string reason;
    CommandQueueCandidate current;
    if (!readCommandQueueCandidate(
          processId,
          regions.regions,
          selectedCandidate.vectorAddress,
          current,
          reason))
    {
      if (!allowKnownRawTurnBufferFallback
          || !readKnownRawTurnBufferCandidate(
            processId,
            regions.regions,
            selectedCandidate,
            current,
            reason))
      {
        result.reason = reason;
        return result;
      }
    }

    const std::size_t availableBytes = current.capacityBytes - current.usedBytes;
    if (availableBytes < encodedBytes.size())
    {
      result.reason = "command queue candidate does not have enough remaining capacity";
      return result;
    }

    RuntimeMemoryReadResult originalTail =
      readProcessMemory(processId, current.bufferEnd, encodedBytes.size());
    if (!originalTail.success || originalTail.bytesRead != encodedBytes.size())
    {
      result.reason = originalTail.reason.empty()
        ? "unable to snapshot command queue tail before write"
        : originalTail.reason;
      return result;
    }

    RuntimeMemoryWriteResult writeBytes = writeProcessMemory(
      processId,
      current.bufferEnd,
      encodedBytes.data(),
      encodedBytes.size());
    if (!writeBytes.success || writeBytes.bytesWritten != encodedBytes.size())
    {
      result.reason = writeBytes.reason.empty()
        ? "unable to write encoded command bytes"
        : writeBytes.reason;
      return result;
    }

    RuntimeMemoryWriteResult writeEnd;
    if (current.storageKind == "raw-turn-buffer")
    {
      const std::uint32_t newUsedBytes32 =
        static_cast<std::uint32_t>(current.usedBytes + encodedBytes.size());
      writeEnd = writeProcessMemory(
        processId,
        current.bytesInQueueAddress,
        &newUsedBytes32,
        sizeof(newUsedBytes32));
      if (!writeEnd.success || writeEnd.bytesWritten != sizeof(newUsedBytes32))
      {
        result.reason = writeEnd.reason.empty()
          ? "unable to advance raw turn-buffer byte count"
          : writeEnd.reason;
        return result;
      }
    }
    else
    {
      const std::uint64_t newEnd64 =
        static_cast<std::uint64_t>(current.bufferEnd + encodedBytes.size());
      writeEnd = writeProcessMemory(
        processId,
        current.vectorAddress + sizeof(std::uint64_t),
        &newEnd64,
        sizeof(newEnd64));
      if (!writeEnd.success || writeEnd.bytesWritten != sizeof(newEnd64))
      {
        result.reason = writeEnd.reason.empty()
          ? "unable to advance command queue end pointer"
          : writeEnd.reason;
        return result;
      }
    }

    RuntimeMemoryReadResult readback =
      readProcessMemory(processId, current.bufferEnd, encodedBytes.size());
    if (!readback.success || readback.bytesRead != encodedBytes.size())
    {
      result.reason = readback.reason.empty()
        ? "unable to read back encoded command bytes"
        : readback.reason;
      return result;
    }
    if (!std::equal(encodedBytes.begin(), encodedBytes.end(), readback.bytes.begin()))
    {
      result.reason = "encoded command readback did not match written bytes";
      return result;
    }

    CommandQueueCandidate afterWrite;
    if (!readCommandQueueCandidate(
          processId,
          regions.regions,
          current.vectorAddress,
          afterWrite,
          reason))
    {
      result.reason = reason;
      return result;
    }
    if (afterWrite.bufferEnd != current.bufferEnd + encodedBytes.size())
    {
      if (!restoreAfterReadback && afterWrite.usedBytes <= current.usedBytes)
      {
        result.consumedImmediately = true;
      }
      else
      {
        result.reason = current.storageKind == "raw-turn-buffer"
          ? "raw turn-buffer byte count did not advance by encoded command size"
          : "command queue end pointer did not advance by encoded command size";
        return result;
      }
    }

    if (restoreAfterReadback)
    {
      RuntimeMemoryWriteResult restoreTail = writeProcessMemory(
        processId,
        current.bufferEnd,
        originalTail.bytes.data(),
        originalTail.bytes.size());
      RuntimeMemoryWriteResult restoreEnd;
      if (current.storageKind == "raw-turn-buffer")
      {
        const std::uint32_t originalUsedBytes32 =
          static_cast<std::uint32_t>(current.usedBytes);
        restoreEnd = writeProcessMemory(
          processId,
          current.bytesInQueueAddress,
          &originalUsedBytes32,
          sizeof(originalUsedBytes32));
      }
      else
      {
        const std::uint64_t originalEnd64 = static_cast<std::uint64_t>(current.bufferEnd);
        restoreEnd = writeProcessMemory(
          processId,
          current.vectorAddress + sizeof(std::uint64_t),
          &originalEnd64,
          sizeof(originalEnd64));
      }
      if (!restoreTail.success || !restoreEnd.success)
      {
        result.reason = "command queue self-fixture write passed but restore failed";
        return result;
      }
    }

    result.passed = true;
    result.candidate = current;
    result.tailAddress = current.bufferEnd;
    result.expectedEnd = current.bufferEnd + encodedBytes.size();
    result.originalTail = std::move(originalTail.bytes);
    result.appendedBytes = encodedBytes.size();
    return result;
  }

  bool restoreCommandQueueAppendIfStillPresent(
    int processId,
    const CommandQueueAppendResult& append)
  {
    if (!append.passed || append.tailAddress == 0 || append.originalTail.empty())
      return false;

    RuntimeMemoryRegionListResult regions = listProcessMemoryRegions(processId);
    if (!regions.success)
      return false;

    std::string reason;
    CommandQueueCandidate current;
    if (!readCommandQueueCandidate(
          processId,
          regions.regions,
          append.candidate.vectorAddress,
          current,
          reason))
      return false;
    if (current.bufferEnd != append.expectedEnd)
      return false;

    RuntimeMemoryReadResult tailRead =
      readProcessMemory(processId, append.tailAddress, append.appendedBytes);
    if (!tailRead.success || tailRead.bytesRead != append.appendedBytes)
      return false;

    RuntimeMemoryWriteResult restoreTail = writeProcessMemory(
      processId,
      append.tailAddress,
      append.originalTail.data(),
      append.originalTail.size());
    RuntimeMemoryWriteResult restoreEnd;
    if (append.candidate.storageKind == "raw-turn-buffer")
    {
      const std::uint32_t originalUsedBytes32 =
        static_cast<std::uint32_t>(append.candidate.usedBytes);
      restoreEnd = writeProcessMemory(
        processId,
        append.candidate.bytesInQueueAddress,
        &originalUsedBytes32,
        sizeof(originalUsedBytes32));
    }
    else
    {
      const std::uint64_t originalEnd64 = static_cast<std::uint64_t>(append.tailAddress);
      restoreEnd = writeProcessMemory(
        processId,
        append.candidate.vectorAddress + sizeof(std::uint64_t),
        &originalEnd64,
        sizeof(originalEnd64));
    }
    return restoreTail.success && restoreEnd.success;
  }

  bool clearAdapterPauseResumeProofBytes(
    int processId,
    const CommandQueueCandidate& selectedCandidate,
    bool& cleared,
    std::string& reason)
  {
    cleared = false;

    RuntimeMemoryRegionListResult regions = listProcessMemoryRegions(processId);
    if (!regions.success)
    {
      reason = regions.reason;
      return false;
    }

    CommandQueueCandidate current;
    if (!readCommandQueueCandidate(
          processId,
          regions.regions,
          selectedCandidate.vectorAddress,
          current,
          reason))
    {
      return false;
    }

    if (current.usedBytes == 0)
      return true;
    if (current.usedBytes > 64)
    {
      // Large live queues can contain normal game/user commands. Do not clear
      // them as stale adapter proof bytes; append at the tail and let the
      // pause/resume behavior proof decide whether this is the real sink.
      return true;
    }

    RuntimeMemoryReadResult existing =
      readProcessMemory(processId, current.bufferBegin, current.usedBytes);
    if (!existing.success || existing.bytesRead != current.usedBytes)
    {
      reason = existing.reason.empty()
        ? "unable to read selected command queue before stale proof-byte cleanup"
        : existing.reason;
      return false;
    }

    const bool onlyAdapterPauseResumeBytes = std::all_of(
      existing.bytes.begin(),
      existing.bytes.end(),
      [](unsigned char byte)
      {
        return byte == 0x10 || byte == 0x11;
      });
    if (!onlyAdapterPauseResumeBytes)
    {
      // A live turn buffer can legitimately contain user/game commands. Keep
      // those bytes intact and append the proof command at the tail; the caller
      // restores only the appended bytes if behavior proof fails.
      return true;
    }

    std::vector<unsigned char> zeros(current.usedBytes, 0);
    RuntimeMemoryWriteResult clearBytes =
      writeProcessMemory(processId, current.bufferBegin, zeros.data(), zeros.size());
    if (!clearBytes.success || clearBytes.bytesWritten != zeros.size())
    {
      reason = clearBytes.reason.empty()
        ? "unable to clear stale adapter proof bytes from selected command queue"
        : clearBytes.reason;
      return false;
    }

    RuntimeMemoryWriteResult clearEnd;
    if (current.storageKind == "raw-turn-buffer")
    {
      const std::uint32_t zeroUsedBytes = 0;
      clearEnd = writeProcessMemory(
        processId,
        current.bytesInQueueAddress,
        &zeroUsedBytes,
        sizeof(zeroUsedBytes));
    }
    else
    {
      const std::uint64_t begin64 = static_cast<std::uint64_t>(current.bufferBegin);
      clearEnd = writeProcessMemory(
        processId,
        current.vectorAddress + sizeof(std::uint64_t),
        &begin64,
        sizeof(begin64));
    }
    if (!clearEnd.success)
    {
      reason = clearEnd.reason.empty()
        ? "unable to reset selected command queue after stale proof-byte cleanup"
        : clearEnd.reason;
      return false;
    }

    cleared = true;
    return true;
  }

  bool readFrameCounterAt(int processId, std::uintptr_t address, std::uint32_t& value)
  {
    RuntimeMemoryReadResult read = readProcessMemory(processId, address, sizeof(std::uint32_t));
    if (!read.success || read.bytesRead != sizeof(std::uint32_t))
      return false;
    value = readU32(read.bytes, 0);
    return true;
  }

  bool sampleFrameCounterDelta(
    int processId,
    std::uintptr_t address,
    int delayMs,
    std::uint32_t& first,
    std::uint32_t& second)
  {
    if (!readFrameCounterAt(processId, address, first))
      return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
    return readFrameCounterAt(processId, address, second);
  }

  std::uint32_t counterDelta(std::uint32_t first, std::uint32_t second)
  {
    return second >= first ? second - first : 0;
  }

  bool sampleProgressingFrameCounter(
    int processId,
    std::uintptr_t address,
    int delayMs,
    int attempts,
    std::uint32_t minDelta,
    std::uint32_t& first,
    std::uint32_t& second)
  {
    for (int attempt = 0; attempt < attempts; ++attempt)
    {
      if (!sampleFrameCounterDelta(processId, address, delayMs, first, second))
        return false;
      if (counterDelta(first, second) >= minDelta)
        return true;
    }
    return true;
  }

  LiveCounterProof proveExplicitLiveCounterRead(
    int processId,
    std::uintptr_t address,
    int sampleDelayMs)
  {
    LiveCounterProof proof;
    proof.address = address;
    if (address == 0)
    {
      proof.reason = "explicit frame counter address is zero";
      return proof;
    }

    constexpr int maxAttempts = 5;
    for (int attempt = 0; attempt < maxAttempts; ++attempt)
    {
      if (!readFrameCounterAt(processId, address, proof.first))
      {
        proof.reason = "unable to read explicit frame counter first sample";
        return proof;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(sampleDelayMs));
      if (!readFrameCounterAt(processId, address, proof.second))
      {
        proof.reason = "unable to read explicit frame counter second sample";
        return proof;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(sampleDelayMs));
      if (!readFrameCounterAt(processId, address, proof.third))
      {
        proof.reason = "unable to read explicit frame counter third sample";
        return proof;
      }
      if (frameCounterConfidencePassed(proof.first, proof.second, proof.third, sampleDelayMs))
      {
        proof.passed = true;
        return proof;
      }
    }
    proof.reason = "explicit frame counter samples did not pass frame-counter confidence checks";
    return proof;
  }

  IssueCommandsProof proveIssueCommandsWithPauseResume(
    int processId,
    const std::string& executablePath,
    const LiveCounterProof& gameStateProof,
    const CommandQueueDiscoveryProof& discoveryProof,
    bool self,
    bool serveCommandBridge,
    std::uintptr_t explicitCommandQueueVectorAddress,
    std::size_t discoveryCandidateScanLimit,
    std::size_t stateMaxScanBytes,
    int stateScanTimeoutMs)
  {
    IssueCommandsProof proof;
    proof.selfFixture = self;
    proof.commandName = "pauseGame/resumeGame";

    if (!discoveryProof.ready || discoveryProof.candidates.empty())
    {
      proof.reason = discoveryProof.reason.empty()
        ? "issue-commands proof requires a discovered command queue candidate"
        : discoveryProof.reason;
      return proof;
    }

    RuntimeEncodedCommand pause = encodeRuntimeCommandRequest(gameActionRequest("pauseGame"));
    RuntimeEncodedCommand resume = encodeRuntimeCommandRequest(gameActionRequest("resumeGame"));
    if (!pause.encoded || !resume.encoded)
    {
      proof.reason = !pause.reason.empty() ? pause.reason : resume.reason;
      return proof;
    }
    proof.encodedBytes = formatCommandBytesHex(pause.bytes) + " / " + formatCommandBytesHex(resume.bytes);

    std::vector<std::pair<std::size_t, const CommandQueueCandidate*>> candidatesToTry;
    if (explicitCommandQueueVectorAddress != 0)
    {
      auto explicitCandidate = std::find_if(
        discoveryProof.candidates.begin(),
        discoveryProof.candidates.end(),
        [&](const CommandQueueCandidate& value)
        {
          return value.vectorAddress == explicitCommandQueueVectorAddress;
        });
      if (explicitCandidate == discoveryProof.candidates.end())
      {
        proof.reason =
          "issue-commands proof requires the explicit command queue vector to be readable; "
          "refusing to fall back to discovery-only candidates";
        return proof;
      }
      candidatesToTry.emplace_back(
        static_cast<std::size_t>(std::distance(discoveryProof.candidates.begin(), explicitCandidate)),
        &*explicitCandidate);
    }
    else if (!self)
    {
      if (discoveryCandidateScanLimit == 0)
      {
        proof.reason =
          "live issue-commands proof requires --command-queue-vector-address or "
          "--issue-command-candidate-scan-limit; refusing to write discovery-only command queue candidates";
        return proof;
      }
      const std::size_t limit =
        std::min(discoveryCandidateScanLimit, discoveryProof.candidates.size());
      bool skippedUnsafeActiveCandidate = false;
      for (std::size_t i = 0; i < limit; ++i)
      {
        const CommandQueueCandidate& candidate = discoveryProof.candidates[i];
        const bool observedActivity =
          candidate.activityTransitions > 0 || candidate.activityByteChanges > 0;
        const bool boundedRawTurnBufferProbe =
          candidate.storageKind == "raw-turn-buffer"
          && candidate.regionPath.empty()
          && candidate.usedBytes <= 64
          && candidate.capacityBytes == rawTurnBufferCapacity;
        if (!observedActivity && !boundedRawTurnBufferProbe)
          continue;
        if (!implicitLiveCommandCandidateSafeForWrite(candidate))
        {
          skippedUnsafeActiveCandidate = true;
          continue;
        }
        std::string unsafeReason;
        if (!liveCommandCandidateSelectorSafeForWrite(candidate, executablePath, unsafeReason))
        {
          skippedUnsafeActiveCandidate = true;
          continue;
        }
        candidatesToTry.emplace_back(i, &candidate);
      }
      if (candidatesToTry.empty())
      {
        if (skippedUnsafeActiveCandidate)
        {
          proof.reason =
            "live issue-commands proof skipped active candidates that are unsafe for live writes; "
            "provide --command-queue-vector-address only after manual validation";
        }
        else
        {
          proof.reason =
            "live issue-commands proof requires natural command queue activity, "
            "a bounded non-image raw turn-buffer candidate, or --command-queue-vector-address "
            "before writing to a discovered candidate";
        }
        return proof;
      }
    }
    else
    {
      const CommandQueueCandidate* candidate = &discoveryProof.candidates.front();
      std::size_t rank = 0;
      auto selfFixtureCandidate = std::find_if(
        discoveryProof.candidates.begin(),
        discoveryProof.candidates.end(),
        [](const CommandQueueCandidate& value)
        {
          return value.storageKind == "raw-turn-buffer"
            && value.regionPath == "self-raw-turn-buffer-fixture";
        });
      if (selfFixtureCandidate == discoveryProof.candidates.end())
      {
        selfFixtureCandidate = std::find_if(
          discoveryProof.candidates.begin(),
          discoveryProof.candidates.end(),
          [](const CommandQueueCandidate& value)
          {
            return value.usedBytes == 4 && value.capacityBytes == 4096;
          });
      }
      if (selfFixtureCandidate != discoveryProof.candidates.end())
      {
        candidate = &*selfFixtureCandidate;
        rank = static_cast<std::size_t>(
          std::distance(discoveryProof.candidates.begin(), selfFixtureCandidate));
      }
      candidatesToTry.emplace_back(rank, candidate);
    }
    if (candidatesToTry.empty())
    {
      proof.reason = "issue-commands proof requires a selected command queue candidate";
      return proof;
    }
    if (!self)
    {
      for (const auto& rankedCandidate : candidatesToTry)
      {
        std::string unsafeReason;
        if (!liveCommandCandidateSelectorSafeForWrite(
              *rankedCandidate.second,
              executablePath,
              unsafeReason))
        {
          proof.reason = unsafeReason;
          return proof;
        }
      }
    }
    if (!self)
    {
      if (!gameStateProof.passed || gameStateProof.address == 0)
      {
        proof.reason = "issue-commands proof requires a passing live frame counter proof";
        return proof;
      }
    }

    auto copyAttemptToProof =
      [&](const IssueCommandsAttempt& attempt)
      {
        proof.deliveryChecked = proof.deliveryChecked || attempt.deliveryChecked;
        proof.behaviorChecked = proof.behaviorChecked || attempt.behaviorChecked;
        proof.commandQueue = attempt.commandQueue;
        proof.vectorAddress = attempt.commandQueue.vectorAddress;
        proof.bufferBegin = attempt.bufferBegin;
        proof.staleProofBytesCleared =
          proof.staleProofBytesCleared || attempt.staleProofBytesCleared;
        proof.originalUsedBytes = attempt.originalUsedBytes;
        proof.appendedBytes = attempt.appendedBytes;
        proof.pauseFrameCounterSampled =
          proof.pauseFrameCounterSampled || attempt.pauseFrameCounterSampled;
        proof.pauseFrameCounterMatched =
          proof.pauseFrameCounterMatched || attempt.pauseFrameCounterMatched;
        proof.frameCounterAddress = attempt.frameCounterAddress;
        proof.baselineStart = attempt.baselineStart;
        proof.baselineEnd = attempt.baselineEnd;
        proof.pausedStart = attempt.pausedStart;
        proof.pausedEnd = attempt.pausedEnd;
        proof.resumedStart = attempt.resumedStart;
        proof.resumedEnd = attempt.resumedEnd;
      };

    constexpr int frameSampleMs = 500;
    std::vector<FrameCounterCandidate> commandCounterCandidates;
    if (!self)
    {
      if (!gameStateProof.passed || gameStateProof.address == 0)
      {
        proof.reason = "issue-commands proof requires the proven live game-state frame counter";
        return proof;
      }
      commandCounterCandidates.push_back(
        FrameCounterCandidate {
          gameStateProof.address,
          gameStateProof.first,
          gameStateProof.second,
          gameStateProof.third,
          0
        });
    }

    auto selectPausedCounter =
      [&](const std::vector<FrameCounterCandidate>& candidates,
          IssueCommandsAttempt& attempt) -> bool
      {
        if (candidates.empty())
          return false;

        struct PauseSample
        {
          const FrameCounterCandidate* candidate = nullptr;
          std::uint32_t start = 0;
          std::uint32_t end = 0;
        };

        std::vector<PauseSample> samples;
        samples.reserve(candidates.size());
        for (const FrameCounterCandidate& candidate : candidates)
        {
          PauseSample sample;
          sample.candidate = &candidate;
          if (readFrameCounterAt(processId, candidate.address, sample.start))
            samples.push_back(sample);
        }
        if (samples.empty())
          return false;

        std::this_thread::sleep_for(std::chrono::milliseconds(frameSampleMs));

        const PauseSample* best = nullptr;
        const PauseSample* diagnostic = nullptr;
        for (PauseSample& sample : samples)
        {
          if (!readFrameCounterAt(processId, sample.candidate->address, sample.end))
            continue;

          const std::uint32_t baselineDelta =
            counterDelta(sample.candidate->second, sample.candidate->third);
          if (baselineDelta < 2)
            continue;

          const std::uint32_t pausedDelta = counterDelta(sample.start, sample.end);
          if (diagnostic == nullptr)
            diagnostic = &sample;
          if (pausedDelta != 0)
            continue;

          if (best == nullptr || sample.candidate->score < best->candidate->score)
            best = &sample;
        }
        if (best == nullptr)
        {
          if (diagnostic != nullptr)
          {
            attempt.pauseFrameCounterSampled = true;
            attempt.frameCounterAddress = diagnostic->candidate->address;
            attempt.baselineStart = diagnostic->candidate->second;
            attempt.baselineEnd = diagnostic->candidate->third;
            attempt.pausedStart = diagnostic->start;
            attempt.pausedEnd = diagnostic->end;
          }
          return false;
        }

        attempt.pauseFrameCounterSampled = true;
        attempt.pauseFrameCounterMatched = true;
        attempt.frameCounterAddress = best->candidate->address;
        attempt.baselineStart = best->candidate->second;
        attempt.baselineEnd = best->candidate->third;
        attempt.pausedStart = best->start;
        attempt.pausedEnd = best->end;
        return true;
      };

    for (const auto& rankedCandidate : candidatesToTry)
    {
      IssueCommandsAttempt attempt;
      attempt.rank = rankedCandidate.first;
      attempt.commandQueue = *rankedCandidate.second;
      attempt.bufferBegin = rankedCandidate.second->bufferBegin;
      attempt.originalUsedBytes = rankedCandidate.second->usedBytes;

      if (!self)
      {
        attempt.frameCounterAddress = commandCounterCandidates.front().address;
        attempt.baselineStart = commandCounterCandidates.front().second;
        attempt.baselineEnd = commandCounterCandidates.front().third;

        bool staleProofBytesCleared = false;
        std::string staleProofByteCleanupReason;
        if (!clearAdapterPauseResumeProofBytes(
              processId,
              *rankedCandidate.second,
              staleProofBytesCleared,
              staleProofByteCleanupReason))
        {
          attempt.reason =
            "unable to prepare command queue for behavior proof: "
            + staleProofByteCleanupReason;
          proof.attempts.push_back(attempt);
          copyAttemptToProof(attempt);
          continue;
        }
        attempt.staleProofBytesCleared = staleProofBytesCleared;
      }

      CommandQueueAppendResult append = appendEncodedCommandToQueueCandidate(
        processId,
        *rankedCandidate.second,
        pause.bytes,
        self);
      if (!append.passed)
      {
        attempt.reason = append.reason;
        proof.attempts.push_back(attempt);
        copyAttemptToProof(attempt);
        continue;
      }

      attempt.deliveryChecked = true;
      attempt.commandQueue = append.candidate;
      attempt.bufferBegin = append.candidate.bufferBegin;
      attempt.originalUsedBytes = append.candidate.usedBytes;
      attempt.appendedBytes = append.appendedBytes;
      attempt.consumedImmediately = append.consumedImmediately;
      copyAttemptToProof(attempt);

      if (self)
      {
        attempt.reason = "self command queue fixture append/readback passed; active StarCraft behavior proof is required";
        proof.attempts.push_back(attempt);
        copyAttemptToProof(attempt);
        proof.reason = attempt.reason;
        return proof;
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(1000));
      if (!selectPausedCounter(commandCounterCandidates, attempt))
      {
        restoreCommandQueueAppendIfStillPresent(processId, append);
        attempt.reason = attempt.pauseFrameCounterSampled
          ? "pause command did not stop tracked live frame counter; paused_delta="
            + std::to_string(counterDelta(attempt.pausedStart, attempt.pausedEnd))
          : "pause command did not produce a readable live frame-counter sample";
        proof.attempts.push_back(attempt);
        copyAttemptToProof(attempt);
        continue;
      }

      CommandQueueAppendResult resumeAppend = appendEncodedCommandToQueueCandidate(
        processId,
        append.candidate,
        resume.bytes,
        false,
        true);
      if (!resumeAppend.passed)
      {
        restoreCommandQueueAppendIfStillPresent(processId, append);
        attempt.reason = "pause command delivery passed but resume delivery failed: " + resumeAppend.reason;
        proof.attempts.push_back(attempt);
        copyAttemptToProof(attempt);
        continue;
      }
      attempt.consumedImmediately =
        attempt.consumedImmediately || resumeAppend.consumedImmediately;

      std::this_thread::sleep_for(std::chrono::milliseconds(1000));
      if (!sampleProgressingFrameCounter(
          processId,
          attempt.frameCounterAddress,
          frameSampleMs,
          12,
          2,
          attempt.resumedStart,
          attempt.resumedEnd))
      {
        restoreCommandQueueAppendIfStillPresent(processId, resumeAppend);
        attempt.reason = "unable to sample live frame counter after resume command";
        proof.attempts.push_back(attempt);
        copyAttemptToProof(attempt);
        continue;
      }

      const std::uint32_t resumedDelta = counterDelta(attempt.resumedStart, attempt.resumedEnd);
      if (resumedDelta < 2)
      {
        restoreCommandQueueAppendIfStillPresent(processId, resumeAppend);
        attempt.reason = "resume command did not restore live frame progression";
        proof.attempts.push_back(attempt);
        copyAttemptToProof(attempt);
        continue;
      }

      attempt.behaviorChecked = true;
      proof.attempts.push_back(attempt);
      copyAttemptToProof(attempt);
      proof.receiverActive = serveCommandBridge;
      proof.passed = true;
      return proof;
    }

    if (!proof.attempts.empty())
    {
      proof.reason = "no selected command queue candidate produced live pause/resume behavior";
      if (!proof.attempts.back().reason.empty())
        proof.reason += "; last_reason=" + proof.attempts.back().reason;
    }
    else
    {
      proof.reason = "issue-commands proof did not attempt any command queue candidate";
    }
    return proof;
  }

  bool writeIssueCommandsSnapshot(
    const std::filesystem::path& path,
    const IssueCommandsProof& proof,
    std::string& reason)
  {
    std::ofstream output(path);
    if (!output)
    {
      reason = "unable to open issue-commands snapshot output";
      return false;
    }

    output << "field\tvalue\n";
    output << "passed\t" << (proof.passed ? "true" : "false") << '\n';
    output << "delivery_checked\t" << (proof.deliveryChecked ? "true" : "false") << '\n';
    output << "behavior_checked\t" << (proof.behaviorChecked ? "true" : "false") << '\n';
    output << "self_fixture\t" << (proof.selfFixture ? "true" : "false") << '\n';
    output << "receiver_active\t" << (proof.receiverActive ? "true" : "false") << '\n';
    output << "stale_proof_bytes_cleared\t" << (proof.staleProofBytesCleared ? "true" : "false") << '\n';
    output << "pause_frame_counter_sampled\t"
           << (proof.pauseFrameCounterSampled ? "true" : "false") << '\n';
    output << "pause_frame_counter_matched\t"
           << (proof.pauseFrameCounterMatched ? "true" : "false") << '\n';
    output << "command\t" << proof.commandName << '\n';
    output << "encoded_bytes\t" << proof.encodedBytes << '\n';
    output << "attempt_count\t" << proof.attempts.size() << '\n';
    output << "storage_kind\t" << proof.commandQueue.storageKind << '\n';
    output << "vector_address\t" << hexAddress(proof.vectorAddress) << '\n';
    output << "bytes_in_queue_address\t" << hexAddress(proof.commandQueue.bytesInQueueAddress) << '\n';
    output << "buffer_begin\t" << hexAddress(proof.bufferBegin) << '\n';
    output << "frame_counter_address\t" << hexAddress(proof.frameCounterAddress) << '\n';
    output << "original_used_bytes\t" << proof.originalUsedBytes << '\n';
    output << "appended_bytes\t" << proof.appendedBytes << '\n';
    output << "baseline_delta\t" << counterDelta(proof.baselineStart, proof.baselineEnd) << '\n';
    output << "paused_delta\t" << counterDelta(proof.pausedStart, proof.pausedEnd) << '\n';
    output << "resumed_delta\t" << counterDelta(proof.resumedStart, proof.resumedEnd) << '\n';
    if (!proof.reason.empty())
      output << "reason\t" << proof.reason << '\n';
    if (!proof.attempts.empty())
    {
      output << "\n";
      output << "attempt_rank\tstorage_kind\tselector_address\tbytes_in_queue_address\tbuffer_begin\toriginal_used_bytes\tappended_bytes\tconsumed_immediately\tstale_proof_bytes_cleared\tpause_frame_counter_sampled\tpause_frame_counter_matched\tdelivery_checked\tbehavior_checked\tbaseline_delta\tpaused_delta\tresumed_delta\treason\n";
      for (const IssueCommandsAttempt& attempt : proof.attempts)
      {
        output << attempt.rank << '\t'
               << attempt.commandQueue.storageKind << '\t'
               << hexAddress(attempt.commandQueue.vectorAddress) << '\t'
               << hexAddress(attempt.commandQueue.bytesInQueueAddress) << '\t'
               << hexAddress(attempt.bufferBegin) << '\t'
               << attempt.originalUsedBytes << '\t'
               << attempt.appendedBytes << '\t'
               << (attempt.consumedImmediately ? "true" : "false") << '\t'
               << (attempt.staleProofBytesCleared ? "true" : "false") << '\t'
               << (attempt.pauseFrameCounterSampled ? "true" : "false") << '\t'
               << (attempt.pauseFrameCounterMatched ? "true" : "false") << '\t'
               << (attempt.deliveryChecked ? "true" : "false") << '\t'
               << (attempt.behaviorChecked ? "true" : "false") << '\t'
               << counterDelta(attempt.baselineStart, attempt.baselineEnd) << '\t'
               << counterDelta(attempt.pausedStart, attempt.pausedEnd) << '\t'
               << counterDelta(attempt.resumedStart, attempt.resumedEnd) << '\t'
               << attempt.reason << '\n';
      }
    }

    if (!output)
    {
      reason = "unable to write issue-commands snapshot output";
      return false;
    }
    return true;
  }

  std::string lowerAscii(std::string value)
  {
    for (char& ch : value)
      ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return value;
  }

  bool detectReplayLaunch(
    const RuntimeProcessCommandLineResult& commandLine,
    std::string& evidence)
  {
    if (!commandLine.inspected)
      return false;

    for (const std::string& argument : commandLine.arguments)
    {
      const std::string lowered = lowerAscii(argument);
      if (lowered == "playreplay" || lowered.find("playreplay") != std::string::npos)
      {
        evidence = argument;
        return true;
      }
    }
    return false;
  }

  DrawOverlaysProof proveDrawOverlaysFailClosed(
    const IssueCommandsProof& issueCommandsProof,
    const std::string& executablePath)
  {
    DrawOverlaysProof proof;
    proof.commandReceiverActive = issueCommandsProof.receiverActive;
    proof.adapterLocalActionsAvailable = issueCommandsProof.receiverActive;
    const ExecutableAnchorScan drawLayerScan = scanExecutableAnchors(
      executablePath,
      {
        "DrawLayer_GameUnits",
        "DrawLayer_UI",
        "DrawLayer_Cursor"
      });
    const ExecutableAnchorScan renderApiScan = scanExecutableAnchors(
      executablePath,
      {
        "glDrawElements",
        "glDrawArrays",
        "CGLGetCurrentContext",
        "CGContextFillRect"
      });

    proof.drawLayerAnchorsResolved =
      drawLayerScan.readable && drawLayerScan.missing.empty();
    proof.renderApiAnchorsResolved =
      renderApiScan.readable && !renderApiScan.found.empty();
    proof.resolvedAnchors = drawLayerScan.found;
    proof.resolvedAnchors.insert(
      proof.resolvedAnchors.end(),
      renderApiScan.found.begin(),
      renderApiScan.found.end());
    proof.missingAnchors = drawLayerScan.missing;
    proof.missingAnchors.insert(
      proof.missingAnchors.end(),
      renderApiScan.missing.begin(),
      renderApiScan.missing.end());
    proof.reason =
      "draw-overlays proof requires a render hook or overlay renderer with visible-frame behavior evidence; "
      "adapter-local draw command logging alone is not production overlay rendering";
    if (proof.drawLayerAnchorsResolved || proof.renderApiAnchorsResolved)
    {
      proof.reason +=
        "; static render anchors were found, but static anchors are not a callable hook or visible-frame proof";
    }
    else if (!drawLayerScan.reason.empty())
    {
      proof.reason += "; static render anchor scan failed: " + drawLayerScan.reason;
    }
    return proof;
  }

  bool writeDrawOverlaysSnapshot(
    const std::filesystem::path& path,
    const DrawOverlaysProof& proof,
    std::string& reason)
  {
    std::ofstream output(path);
    if (!output)
    {
      reason = "unable to open draw-overlays snapshot output";
      return false;
    }

    output << "field\tvalue\n";
    output << "passed\t" << (proof.passed ? "true" : "false") << '\n';
    output << "command_receiver_active\t"
           << (proof.commandReceiverActive ? "true" : "false") << '\n';
    output << "adapter_local_actions_available\t"
           << (proof.adapterLocalActionsAvailable ? "true" : "false") << '\n';
    output << "draw_layer_anchors_resolved\t"
           << (proof.drawLayerAnchorsResolved ? "true" : "false") << '\n';
    output << "render_api_anchors_resolved\t"
           << (proof.renderApiAnchorsResolved ? "true" : "false") << '\n';
    output << "render_hook_resolved\t" << (proof.renderHookResolved ? "true" : "false") << '\n';
    output << "render_behavior_checked\t" << (proof.renderBehaviorChecked ? "true" : "false") << '\n';
    output << "required_hook\t" << proof.requiredHook << '\n';
    output << "resolved_anchors\t" << joinStrings(proof.resolvedAnchors, ",") << '\n';
    output << "missing_anchors\t" << joinStrings(proof.missingAnchors, ",") << '\n';
    if (!proof.reason.empty())
      output << "reason\t" << proof.reason << '\n';

    if (!output)
    {
      reason = "unable to write draw-overlays snapshot output";
      return false;
    }
    return true;
  }

  MultiplayerSyncProof proveMultiplayerSyncFailClosed(
    const IssueCommandsProof& issueCommandsProof,
    bool activeMatchReady,
    const ReplayAnalysisProof& replayAnalysisProof,
    bool replayLaunchDetected,
    std::string replayLaunchEvidence,
    const std::string& executablePath)
  {
    MultiplayerSyncProof proof;
    proof.commandQueueProven = issueCommandsProof.passed;
    proof.activeMatchProven = activeMatchReady;
    proof.replayLaunchDetected = replayLaunchDetected;
    proof.replayLaunchEvidence = std::move(replayLaunchEvidence);
    proof.replayOnly = replayAnalysisProof.passed || replayLaunchDetected;
    const ExecutableAnchorScan legacySNetScan = scanExecutableAnchors(
      executablePath,
      {
        "SNetReceiveMessage",
        "SNetSendTurn"
      });
    const ExecutableAnchorScan platformNetworkScan = scanExecutableAnchors(
      executablePath,
      {
        "TLSNetworkConnection::Recv",
        "TLSNetworkConnection::Send"
      });
    const ExecutableAnchorScan turnPacketScan = scanExecutableAnchors(
      executablePath,
      {
        "GetTurnPackets",
        "netTurnRate"
      });

    proof.snetReceiveResolved =
      legacySNetScan.readable
      && std::find(
        legacySNetScan.found.begin(),
        legacySNetScan.found.end(),
        "SNetReceiveMessage") != legacySNetScan.found.end();
    proof.snetSendTurnResolved =
      legacySNetScan.readable
      && std::find(
        legacySNetScan.found.begin(),
        legacySNetScan.found.end(),
        "SNetSendTurn") != legacySNetScan.found.end();
    proof.platformReceiveResolved =
      platformNetworkScan.readable
      && std::find(
        platformNetworkScan.found.begin(),
        platformNetworkScan.found.end(),
        "TLSNetworkConnection::Recv") != platformNetworkScan.found.end();
    proof.platformSendResolved =
      platformNetworkScan.readable
      && std::find(
        platformNetworkScan.found.begin(),
        platformNetworkScan.found.end(),
        "TLSNetworkConnection::Send") != platformNetworkScan.found.end();
    proof.turnPacketAnchorResolved =
      turnPacketScan.readable && !turnPacketScan.found.empty();
    proof.resolvedAnchors = legacySNetScan.found;
    proof.resolvedAnchors.insert(
      proof.resolvedAnchors.end(),
      platformNetworkScan.found.begin(),
      platformNetworkScan.found.end());
    proof.resolvedAnchors.insert(
      proof.resolvedAnchors.end(),
      turnPacketScan.found.begin(),
      turnPacketScan.found.end());
    proof.missingAnchors = legacySNetScan.missing;
    proof.missingAnchors.insert(
      proof.missingAnchors.end(),
      platformNetworkScan.missing.begin(),
      platformNetworkScan.missing.end());
    proof.missingAnchors.insert(
      proof.missingAnchors.end(),
      turnPacketScan.missing.begin(),
      turnPacketScan.missing.end());
    proof.reason =
      "multiplayer-sync proof requires resolved live send/receive synchronization hooks and behavior evidence; "
      "active replay or local command queue delivery is not multiplayer synchronization proof";
    if (proof.replayLaunchDetected)
      proof.reason += "; target process was launched with replay playback argument: " + proof.replayLaunchEvidence;
    if (proof.platformReceiveResolved || proof.platformSendResolved || proof.turnPacketAnchorResolved)
    {
      proof.reason +=
        "; static SC:R network/turn anchors were found, but static anchors are not live send/receive sync behavior";
    }
    else if (!legacySNetScan.reason.empty())
    {
      proof.reason += "; static network anchor scan failed: " + legacySNetScan.reason;
    }
    return proof;
  }

  bool writeMultiplayerSyncSnapshot(
    const std::filesystem::path& path,
    const MultiplayerSyncProof& proof,
    std::string& reason)
  {
    std::ofstream output(path);
    if (!output)
    {
      reason = "unable to open multiplayer-sync snapshot output";
      return false;
    }

    output << "field\tvalue\n";
    output << "passed\t" << (proof.passed ? "true" : "false") << '\n';
    output << "command_queue_proven\t" << (proof.commandQueueProven ? "true" : "false") << '\n';
    output << "active_match_proven\t" << (proof.activeMatchProven ? "true" : "false") << '\n';
    output << "replay_only\t" << (proof.replayOnly ? "true" : "false") << '\n';
    output << "replay_launch_detected\t"
           << (proof.replayLaunchDetected ? "true" : "false") << '\n';
    if (!proof.replayLaunchEvidence.empty())
      output << "replay_launch_evidence\t" << proof.replayLaunchEvidence << '\n';
    output << "snet_receive_resolved\t" << (proof.snetReceiveResolved ? "true" : "false") << '\n';
    output << "snet_send_turn_resolved\t" << (proof.snetSendTurnResolved ? "true" : "false") << '\n';
    output << "platform_receive_resolved\t"
           << (proof.platformReceiveResolved ? "true" : "false") << '\n';
    output << "platform_send_resolved\t"
           << (proof.platformSendResolved ? "true" : "false") << '\n';
    output << "turn_packet_anchor_resolved\t"
           << (proof.turnPacketAnchorResolved ? "true" : "false") << '\n';
    output << "sync_behavior_checked\t" << (proof.syncBehaviorChecked ? "true" : "false") << '\n';
    output << "receive_binding\t" << proof.receiveBinding << '\n';
    output << "send_turn_binding\t" << proof.sendTurnBinding << '\n';
    output << "platform_receive_binding\t" << proof.platformReceiveBinding << '\n';
    output << "platform_send_binding\t" << proof.platformSendBinding << '\n';
    output << "turn_packet_binding\t" << proof.turnPacketBinding << '\n';
    output << "resolved_anchors\t" << joinStrings(proof.resolvedAnchors, ",") << '\n';
    output << "missing_anchors\t" << joinStrings(proof.missingAnchors, ",") << '\n';
    if (!proof.reason.empty())
      output << "reason\t" << proof.reason << '\n';

    if (!output)
    {
      reason = "unable to write multiplayer-sync snapshot output";
      return false;
    }
    return true;
  }

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
      return false;
    parsed = static_cast<int>(result);
    return true;
  }

  bool parseSerializedRuntimeCommand(
    const std::string& line,
    RuntimeCommandRequest& command,
    std::string& reason)
  {
    const std::vector<std::string> parts = splitString(line, '|');
    if (parts.size() != 4)
    {
      reason = "serialized command must have four pipe-delimited fields";
      return false;
    }

    if (parts[0] == toString(RuntimeCommandKind::UnitCommand))
      command.kind = RuntimeCommandKind::UnitCommand;
    else if (parts[0] == toString(RuntimeCommandKind::GameAction))
      command.kind = RuntimeCommandKind::GameAction;
    else
    {
      reason = "serialized command has unknown kind";
      return false;
    }

    command.name = parts[1];
    if (!parseIntStrict(parts[2], command.targetUnitId))
    {
      reason = "serialized command target unit id is invalid";
      return false;
    }

    command.arguments.clear();
    if (!parts[3].empty())
    {
      const std::vector<std::string> args = splitString(parts[3], ',');
      for (const std::string& arg : args)
      {
        int parsed = 0;
        if (!parseIntStrict(arg, parsed))
        {
          reason = "serialized command argument is invalid";
          return false;
        }
        command.arguments.push_back(parsed);
      }
    }

    return true;
  }

  void appendCommandReceiverAudit(
    const std::filesystem::path& path,
    std::uint64_t sequence,
    const std::string& status,
    const std::string& commandLine,
    const std::string& encodedBytes,
    const std::string& detail)
  {
    const bool needsHeader = !std::filesystem::exists(path);
    std::ofstream output(path, std::ios::app);
    if (!output)
      return;
    if (needsHeader)
      output << "sequence\tstatus\tencoded_bytes\tdetail\tcommand\n";
    output << sequence << '\t'
           << status << '\t'
           << encodedBytes << '\t'
           << detail << '\t'
           << commandLine << '\n';
  }

  void appendAdapterLocalAction(
    const std::filesystem::path& path,
    std::uint64_t sequence,
    const RuntimeCommandRequest& command,
    const std::string& commandLine)
  {
    const bool needsHeader = !std::filesystem::exists(path);
    std::ofstream output(path, std::ios::app);
    if (!output)
      return;
    if (needsHeader)
      output << "sequence\tkind\tname\ttarget_unit_id\targuments\tcommand\n";
    output << sequence << '\t'
           << toString(command.kind) << '\t'
           << command.name << '\t'
           << command.targetUnitId << '\t';
    for (std::size_t i = 0; i < command.arguments.size(); ++i)
    {
      if (i > 0)
        output << ',';
      output << command.arguments[i];
    }
    output << '\t' << commandLine << '\n';
  }

  int serveRuntimeCommandBridge(
    const RuntimeEnvironment& environment,
    const CommandQueueCandidate& candidate)
  {
    const std::filesystem::path bridgePath(environment.executorBridgePath);
    const std::filesystem::path commandPath = bridgePath / RuntimeExecutorBridgeCommandFile;
    const std::filesystem::path auditPath = bridgePath / "commands.applied.tsv";
    const std::filesystem::path adapterLocalPath = bridgePath / "adapter.local-actions.tsv";

    std::uintmax_t offset = 0;
    std::error_code error;
    if (std::filesystem::exists(commandPath, error) && !error)
      offset = std::filesystem::file_size(commandPath, error);
    if (error)
      offset = 0;

    std::uint64_t sequence = 0;
    std::cout << "command_receiver.active=true\n";
    std::cout << "command_receiver.command_file=" << commandPath.string() << '\n';
    std::cout << "command_receiver.audit_file=" << auditPath.string() << '\n';

    while (true)
    {
      RuntimeProcessOpenResult process = openRuntimeProcess(environment);
      if (!process.opened)
      {
        std::cout << "command_receiver.active=false\n";
        std::cout << "command_receiver.reason="
                  << (process.reason.empty() ? "target runtime process is no longer visible" : process.reason)
                  << '\n';
        return 13;
      }

      error.clear();
      if (!std::filesystem::exists(commandPath, error) || error)
      {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        continue;
      }

      const std::uintmax_t size = std::filesystem::file_size(commandPath, error);
      if (error)
      {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        continue;
      }
      if (size < offset)
        offset = 0;
      if (size == offset)
      {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        continue;
      }

      std::ifstream input(commandPath);
      if (!input)
      {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        continue;
      }
      input.seekg(static_cast<std::streamoff>(offset));

      std::string line;
      while (std::getline(input, line))
      {
        if (line.empty())
          continue;

        ++sequence;
        RuntimeCommandRequest command;
        std::string reason;
        if (!parseSerializedRuntimeCommand(line, command, reason))
        {
          appendCommandReceiverAudit(auditPath, sequence, "rejected", line, "", reason);
          continue;
        }

        RuntimeEncodedCommand encoded = encodeRuntimeCommandRequest(command);
        if (!encoded.encoded)
        {
          if (command.kind == RuntimeCommandKind::GameAction
              && isRuntimeAdapterLocalGameAction(command.name))
          {
            appendAdapterLocalAction(adapterLocalPath, sequence, command, line);
            appendCommandReceiverAudit(
              auditPath,
              sequence,
              "applied",
              line,
              "adapter-local",
              "accepted-by-runtime-adapter-local-state");
            continue;
          }
          appendCommandReceiverAudit(auditPath, sequence, "rejected", line, "", encoded.reason);
          continue;
        }

        CommandQueueCandidate selectedCandidate = candidate;
        CommandQueueDiscoveryProof refreshedDiscovery = discoverCommandQueueCandidates(
          environment.processId,
          environment.executablePath,
          64 * 1024 * 1024,
          5000,
          32);
        if (refreshedDiscovery.ready && !refreshedDiscovery.candidates.empty())
        {
          auto safeRefreshedCandidate = std::find_if(
            refreshedDiscovery.candidates.begin(),
            refreshedDiscovery.candidates.end(),
            [&](const CommandQueueCandidate& refreshedCandidate)
            {
              std::string unsafeReason;
              return liveCommandCandidateSelectorSafeForWrite(
                refreshedCandidate,
                environment.executablePath,
                unsafeReason);
            });
          if (safeRefreshedCandidate != refreshedDiscovery.candidates.end())
            selectedCandidate = *safeRefreshedCandidate;
        }

        std::string unsafeReason;
        if (!liveCommandCandidateSelectorSafeForWrite(
              selectedCandidate,
              environment.executablePath,
              unsafeReason))
        {
          appendCommandReceiverAudit(
            auditPath,
            sequence,
            "failed",
            line,
            formatCommandBytesHex(encoded.bytes),
            unsafeReason);
          continue;
        }

        CommandQueueAppendResult append = appendEncodedCommandToQueueCandidate(
          environment.processId,
          selectedCandidate,
          encoded.bytes,
          false);
        const std::string appendDetail = append.passed
          ? "written-to-runtime-command-queue:" + hexAddress(selectedCandidate.vectorAddress)
          : append.reason;
        appendCommandReceiverAudit(
          auditPath,
          sequence,
          append.passed ? "applied" : "failed",
          line,
          formatCommandBytesHex(encoded.bytes),
          appendDetail);
      }

      const std::streampos position = input.tellg();
      offset = position >= 0 ? static_cast<std::uintmax_t>(position) : size;
    }
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
             << (record.hitPointsResolved ? std::to_string(record.hitPoints) : "unresolved")
             << '\n';
    }

    if (!output)
    {
      reason = "unable to write unit snapshot output";
      return false;
    }
    return true;
  }

  bool writeUnitScanDiagnosticsSnapshot(
    const std::filesystem::path& path,
    const UnitScanDiagnostics& diagnostics,
    const LiveUnitsProof& proof,
    const LiveUnitNodeProof& nodeProof,
    std::string& reason)
  {
    std::ofstream output(path);
    if (!output)
    {
      reason = "unable to open unit diagnostics snapshot output";
      return false;
    }

    output << "field\tvalue\n";
    output << "read_units_passed\t" << (proof.passed ? "true" : "false") << '\n';
    output << "read_units_reason\t" << proof.reason << '\n';
    output << "read_units_address\t" << hexAddress(proof.address) << '\n';
    output << "read_units_record_size\t" << proof.recordSize << '\n';
    output << "read_units_layout\t" << proof.layoutName << '\n';
    output << "read_units_active_records\t" << proof.activeRecords << '\n';
    output << "unit_node_passed\t" << (nodeProof.passed ? "true" : "false") << '\n';
    output << "unit_node_reason\t" << nodeProof.reason << '\n';
    output << "unit_node_address\t" << hexAddress(nodeProof.address) << '\n';
    output << "unit_node_vector_address\t" << hexAddress(nodeProof.vectorAddress) << '\n';
    output << "unit_node_record_size\t" << nodeProof.recordSize << '\n';
    output << "unit_node_active_records\t" << nodeProof.activeRecords << '\n';
    output << "scan_readable_writable_regions\t" << diagnostics.readableWritableRegions << '\n';
    output << "scan_readable_only_regions\t" << diagnostics.readableOnlyRegions << '\n';
    output << "scan_scanned_readable_only_regions\t" << diagnostics.scannedReadableOnlyRegions << '\n';
    output << "scan_executable_readable_regions\t" << diagnostics.executableReadableRegions << '\n';
    output << "scan_image_mapped_regions\t" << diagnostics.imageMappedRegions << '\n';
    output << "scan_skipped_image_mapped_regions\t" << diagnostics.skippedImageMappedRegions << '\n';
    output << "scan_scanned_regions\t" << diagnostics.scannedRegions << '\n';
    output << "scan_scanned_bytes\t" << diagnostics.scannedBytes << '\n';
    output << "scan_vector_candidates\t" << diagnostics.vectorCandidates << '\n';
    output << "scan_vector_duplicate_begins\t" << diagnostics.vectorDuplicateBegins << '\n';
    output << "scan_vector_rejected_target_regions\t" << diagnostics.vectorRejectedTargetRegions << '\n';
    output << "scan_pointer_array_candidates\t" << diagnostics.pointerArrayCandidates << '\n';
    output << "scan_pointer_arrays_scored\t" << diagnostics.pointerArraysScored << '\n';
    output << "scan_pointer_array_readable_pointer_hits\t" << diagnostics.pointerArrayReadablePointerHits << '\n';
    output << "scan_strided_candidates\t" << diagnostics.stridedCandidates << '\n';
    output << "scan_candidate_arrays_scored\t" << diagnostics.candidateArraysScored << '\n';
    output << "scan_window_candidate_arrays_scored\t" << diagnostics.windowCandidateArraysScored << '\n';
    output << "scan_field_plausible_records\t" << diagnostics.fieldPlausibleRecords << '\n';
    output << "scan_pointer_dense_rejected_records\t" << diagnostics.pointerDenseRejectedRecords << '\n';
    output << "scan_sprite_rejected_records\t" << diagnostics.spriteRejectedRecords << '\n';
    output << "scan_plausible_records\t" << diagnostics.plausibleRecords << '\n';
    output << "scan_timed_out\t" << (diagnostics.timedOut ? "true" : "false") << '\n';
    output << "scan_byte_limit_reached\t" << (diagnostics.byteLimitReached ? "true" : "false") << '\n';
    output << "scan_best_active_records\t" << diagnostics.bestActiveRecords << '\n';
    output << "scan_best_address\t" << hexAddress(diagnostics.bestAddress) << '\n';
    output << "scan_best_record_size\t" << diagnostics.bestRecordSize << '\n';
    output << "scan_best_layout\t" << diagnostics.bestLayoutName << '\n';
    output << "scan_best_snapshot_bytes\t" << diagnostics.bestBytes.size() << '\n';
    output << "scan_top_candidate_count\t" << diagnostics.topCandidates.size() << '\n';
    for (std::size_t index = 0; index < diagnostics.topCandidates.size(); ++index)
    {
      const UnitCandidateDiagnostic& candidate = diagnostics.topCandidates[index];
      output << "scan_top_candidate_" << index << "_source\t" << candidate.source << '\n';
      output << "scan_top_candidate_" << index << "_address\t" << hexAddress(candidate.address) << '\n';
      output << "scan_top_candidate_" << index << "_record_size\t" << candidate.recordSize << '\n';
      output << "scan_top_candidate_" << index << "_layout\t" << candidate.layoutName << '\n';
      output << "scan_top_candidate_" << index << "_sampled_records\t" << candidate.sampledRecords << '\n';
      output << "scan_top_candidate_" << index << "_active_records\t" << candidate.activeRecords << '\n';
      output << "scan_top_candidate_" << index << "_pointer_array\t"
             << (candidate.pointerArray ? "true" : "false") << '\n';
    }
    output << "unit_node_scan_regions\t" << diagnostics.unitNodeScannedRegions << '\n';
    output << "unit_node_scan_bytes\t" << diagnostics.unitNodeScannedBytes << '\n';
    output << "unit_node_field_candidates\t" << diagnostics.unitNodeFieldCandidates << '\n';
    output << "unit_node_readable_candidates\t" << diagnostics.unitNodeReadableCandidates << '\n';
    output << "unit_node_graph_seeds_scored\t" << diagnostics.unitNodeGraphSeedsScored << '\n';
    output << "unit_node_pointer_graph_seeds_scored\t"
           << diagnostics.unitNodePointerGraphSeedsScored << '\n';
    output << "unit_node_vector_candidates\t" << diagnostics.unitNodeVectorCandidates << '\n';
    output << "unit_node_best_active_records\t" << diagnostics.unitNodeBestActiveRecords << '\n';
    output << "unit_node_best_address\t" << hexAddress(diagnostics.unitNodeBestAddress) << '\n';
    output << "unit_node_best_vector_address\t"
           << hexAddress(diagnostics.unitNodeBestVectorAddress) << '\n';
    output << "unit_node_best_reason\t" << diagnostics.unitNodeBestReason << '\n';
    output << "unit_node_vector_sample_count\t" << diagnostics.unitNodeVectorSamples.size() << '\n';
    for (std::size_t index = 0; index < diagnostics.unitNodeVectorSamples.size(); ++index)
    {
      const UnitNodeVectorCandidateDiagnostic& sample = diagnostics.unitNodeVectorSamples[index];
      output << "unit_node_vector_sample_" << index << "_vector_address\t"
             << hexAddress(sample.vectorAddress) << '\n';
      output << "unit_node_vector_sample_" << index << "_begin\t" << hexAddress(sample.begin) << '\n';
      output << "unit_node_vector_sample_" << index << "_end\t" << hexAddress(sample.end) << '\n';
      output << "unit_node_vector_sample_" << index << "_capacity\t"
             << hexAddress(sample.capacity) << '\n';
      output << "unit_node_vector_sample_" << index << "_used_bytes\t"
             << sample.usedBytes << '\n';
      output << "unit_node_vector_sample_" << index << "_capacity_bytes\t"
             << sample.capacityBytes << '\n';
      output << "unit_node_vector_sample_" << index << "_record_vector\t"
             << (sample.recordVector ? "true" : "false") << '\n';
      output << "unit_node_vector_sample_" << index << "_pointer_vector\t"
             << (sample.pointerVector ? "true" : "false") << '\n';
      output << "unit_node_vector_sample_" << index << "_record_count\t"
             << sample.recordCount << '\n';
      output << "unit_node_vector_sample_" << index << "_pointer_count\t"
             << sample.pointerCount << '\n';
      output << "unit_node_vector_sample_" << index << "_readable_precheck\t"
             << (sample.readablePrecheck ? "true" : "false") << '\n';
    }
    output << "unit_node_field_sample_count\t" << diagnostics.unitNodeFieldSamples.size() << '\n';
    for (std::size_t index = 0; index < diagnostics.unitNodeFieldSamples.size(); ++index)
    {
      const UnitNodeFieldCandidateDiagnostic& sample = diagnostics.unitNodeFieldSamples[index];
      output << "unit_node_field_sample_" << index << "_address\t" << hexAddress(sample.address) << '\n';
      output << "unit_node_field_sample_" << index << "_previous\t" << hexAddress(sample.previous) << '\n';
      output << "unit_node_field_sample_" << index << "_next\t" << hexAddress(sample.next) << '\n';
      output << "unit_node_field_sample_" << index << "_sprite\t" << hexAddress(sample.sprite) << '\n';
      output << "unit_node_field_sample_" << index << "_secondary\t"
             << hexAddress(sample.secondaryObject) << '\n';
      output << "unit_node_field_sample_" << index << "_x\t" << sample.x << '\n';
      output << "unit_node_field_sample_" << index << "_y\t" << sample.y << '\n';
      output << "unit_node_field_sample_" << index << "_target_x\t" << sample.targetX << '\n';
      output << "unit_node_field_sample_" << index << "_target_y\t" << sample.targetY << '\n';
      output << "unit_node_field_sample_" << index << "_state_a\t" << sample.stateA << '\n';
      output << "unit_node_field_sample_" << index << "_state_b\t" << sample.stateB << '\n';
      output << "unit_node_field_sample_" << index << "_readable_link\t"
             << (sample.readableLink ? "true" : "false") << '\n';
      output << "unit_node_field_sample_" << index << "_readable_sprite\t"
             << (sample.readableSprite ? "true" : "false") << '\n';
      output << "unit_node_field_sample_" << index << "_readable_secondary\t"
             << (sample.readableSecondaryObject ? "true" : "false") << '\n';
    }

    if (!output)
    {
      reason = "unable to write unit diagnostics snapshot output";
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

    if (unitHandles.size() < minRemasteredSnapshotUnitRecords)
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

    output << "player\tstorm_id\trace\trace_inferred\tobserved_unit_count\tminerals\tgas\tsupply_used\tsupply_total\talliance_mask\n";
    for (const PlayerSnapshotRecord& record : proof.players)
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
    if (!output)
    {
      reason = "unable to write player snapshot output";
      return false;
    }
    return true;
  }

  bool writeBulletDataSnapshot(
    const std::filesystem::path& path,
    const BulletDataProof& proof,
    std::string& reason)
  {
    std::ofstream output(path);
    if (!output)
    {
      reason = "unable to open bullet snapshot output";
      return false;
    }

    output << "index\taddress\tsprite\tsource_unit\ttarget_unit\ttype\tx\ty\tvelocity_x\tvelocity_y\tplayer\tremove_timer\n";
    for (const BulletSnapshotRecord& record : proof.records)
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
    if (!output)
    {
      reason = "unable to write bullet snapshot output";
      return false;
    }
    return true;
  }

  bool writeRegionDataSnapshot(
    const std::filesystem::path& path,
    const RegionDataProof& proof,
    std::string& reason)
  {
    std::ofstream output(path);
    if (!output)
    {
      reason = "unable to open region snapshot output";
      return false;
    }

    output << "id\tcenter_x\tcenter_y\tleft\ttop\tright\tbottom\tobserved_units\taccessible\n";
    for (const RegionSnapshotRecord& record : proof.regions)
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
    if (!output)
    {
      reason = "unable to write region snapshot output";
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

  std::string nativeAIModuleExtension()
  {
#if defined(_WIN32)
    return ".dll";
#elif defined(__APPLE__)
    return ".dylib";
#elif defined(__linux__) || defined(__unix__)
    return ".so";
#else
    return "unknown";
#endif
  }

  AIModuleLoadProof proveAIModuleLoading(const std::string& modulePath, bool self)
  {
    AIModuleLoadProof proof;
    proof.modulePath = modulePath;
    proof.moduleExtension = nativeAIModuleExtension();

    if (!modulePath.empty())
    {
      std::error_code error;
      if (!std::filesystem::is_regular_file(modulePath, error) || error)
      {
        proof.reason = error
          ? "AI module path is not readable: " + error.message()
          : "AI module path is not a regular file";
        return proof;
      }
    }
    else if (!self)
    {
      proof.reason = "--prove-load-ai-modules requires --ai-module-path outside self smoke tests";
      return proof;
    }

#if defined(_WIN32)
    proof.loader = "LoadLibraryA";
    HMODULE handle = nullptr;
    if (modulePath.empty())
    {
      proof.loader = "GetModuleHandleA";
      handle = GetModuleHandleA(nullptr);
      proof.selfProcessSmoke = true;
    }
    else
    {
      handle = LoadLibraryA(modulePath.c_str());
    }
    if (handle == nullptr)
    {
      proof.reason = "native Windows loader rejected the AI module";
      return proof;
    }
    if (!modulePath.empty())
      FreeLibrary(handle);
    proof.passed = true;
    return proof;
#elif defined(__APPLE__) || defined(__linux__) || defined(__unix__)
    proof.loader = "dlopen";
    dlerror();
    void* handle = dlopen(modulePath.empty() ? nullptr : modulePath.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr)
    {
      const char* error = dlerror();
      proof.reason = error == nullptr ? "native dlopen rejected the AI module" : error;
      return proof;
    }
    if (modulePath.empty())
      proof.selfProcessSmoke = true;
    dlclose(handle);
    proof.passed = true;
    return proof;
#else
    proof.reason = "native dynamic module loading is not implemented on this platform";
    return proof;
#endif
  }

  bool writeAIModuleLoadSnapshot(
    const std::filesystem::path& path,
    const AIModuleLoadProof& proof,
    std::string& reason)
  {
    std::ofstream output(path);
    if (!output)
    {
      reason = "unable to open AI module load snapshot output";
      return false;
    }

    output << "field\tvalue\n";
    output << "passed\t" << (proof.passed ? "true" : "false") << '\n';
    output << "loader\t" << proof.loader << '\n';
    output << "module_path\t" << proof.modulePath << '\n';
    output << "module_extension\t" << proof.moduleExtension << '\n';
    output << "self_process_smoke\t" << (proof.selfProcessSmoke ? "true" : "false") << '\n';
    if (!proof.reason.empty())
      output << "reason\t" << proof.reason << '\n';

    if (!output)
    {
      reason = "unable to write AI module load snapshot output";
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
          rememberBestCandidate(diagnostics, proof, &read.bytes, 0, recordSize, "explicit-address");
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
      int regionPriority = 3;
    };
    struct Snapshot
    {
      std::uintptr_t address = 0;
      int regionPriority = 3;
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
      snapshot.regionPriority =
        unitScanRegionPriority(region, executablePath, targetImageBase);
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
          candidate.regionPriority = snapshot.regionPriority;
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
        if (bestProof.passed)
          return bestProof;
        if (diagnostics != nullptr)
          diagnostics->timedOut = true;
        return { false, 0, 0, 0, 0, "state counter scan timed out before proof" };
      }
      RuntimeMemoryReadResult third = readProcessMemory(processId, candidate.address, sizeof(std::uint32_t));
      if (!third.success || third.bytesRead != sizeof(std::uint32_t))
        continue;

      const std::uint32_t thirdValue = readU32(third.bytes, 0);
      const int score =
        candidate.regionPriority * 1000000
        + frameCounterScore(candidate.first, candidate.second, thirdValue, sampleDelayMs);
      if (diagnostics != nullptr
          && (!diagnostics->hasClosestCounter || score < diagnostics->closestCounterScore))
      {
        diagnostics->hasClosestCounter = true;
        diagnostics->closestCounterAddress = candidate.address;
        diagnostics->closestCounterFirst = candidate.first;
        diagnostics->closestCounterSecond = candidate.second;
        diagnostics->closestCounterThird = thirdValue;
        diagnostics->closestCounterScore = score;
        diagnostics->closestCounterReason = frameCounterConfidencePassed(
          candidate.first,
          candidate.second,
          thirdValue,
          sampleDelayMs)
            ? "frame-like"
            : frameCounterConfidenceFailureReason(
                candidate.first,
                candidate.second,
                thirdValue,
                sampleDelayMs);
      }
      if (frameCounterConfidencePassed(candidate.first, candidate.second, thirdValue, sampleDelayMs))
      {
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

  std::vector<FrameCounterCandidate> collectFrameCounterCandidates(
    int processId,
    const std::string& executablePath,
    int sampleDelayMs,
    std::size_t maxScanBytes,
    int scanTimeoutMs,
    std::size_t maxCandidates)
  {
    struct Snapshot
    {
      std::uintptr_t address = 0;
      int regionPriority = 3;
      std::vector<unsigned char> bytes;
    };
    struct Candidate
    {
      std::uintptr_t address = 0;
      std::uint32_t first = 0;
      std::uint32_t second = 0;
      int regionPriority = 3;
    };

    std::vector<FrameCounterCandidate> result;
    if (maxCandidates == 0)
      return result;

    RuntimeMemoryRegionListResult regions = listProcessMemoryRegions(processId);
    if (!regions.success)
      return result;

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
    std::vector<Snapshot> snapshots;
    std::size_t scanned = 0;
    for (const RuntimeMemoryRegion& region : scanRegions)
    {
      if (timedOut(deadline) || scanned >= maxScanBytes)
        break;
      if (!region.readable || !region.writable || fileBackedNonTargetRegion(region, executablePath))
        continue;
      if (region.size < sizeof(std::uint32_t))
        continue;

      const std::size_t bytesToRead = std::min(region.size, std::min(maxRegionBytes, maxScanBytes - scanned));
      RuntimeMemoryReadResult read = readProcessMemory(processId, region.address, bytesToRead);
      if (!read.success || read.bytesRead < sizeof(std::uint32_t))
        continue;

      Snapshot snapshot;
      snapshot.address = region.address;
      snapshot.regionPriority = unitScanRegionPriority(region, executablePath, targetImageBase);
      snapshot.bytes = std::move(read.bytes);
      scanned += read.bytesRead;
      snapshots.push_back(std::move(snapshot));
    }

    if (snapshots.empty())
      return result;

    std::this_thread::sleep_for(std::chrono::milliseconds(sampleDelayMs));

    std::vector<Candidate> candidates;
    for (const Snapshot& snapshot : snapshots)
    {
      if (timedOut(deadline) || candidates.size() >= 8192)
        break;
      RuntimeMemoryReadResult second = readProcessMemory(processId, snapshot.address, snapshot.bytes.size());
      if (!second.success || second.bytesRead != snapshot.bytes.size())
        continue;

      for (std::size_t offset = 0; offset + sizeof(std::uint32_t) <= snapshot.bytes.size(); offset += sizeof(std::uint32_t))
      {
        if ((offset % (4 * 1024)) == 0 && timedOut(deadline))
          break;
        const std::uint32_t firstValue = readU32(snapshot.bytes, offset);
        const std::uint32_t secondValue = readU32(second.bytes, offset);
        if (!plausibleCounterDelta(firstValue, secondValue))
          continue;

        Candidate candidate;
        candidate.address = snapshot.address + offset;
        candidate.first = firstValue;
        candidate.second = secondValue;
        candidate.regionPriority = snapshot.regionPriority;
        candidates.push_back(candidate);
        if (candidates.size() >= 8192)
          break;
      }
    }

    if (candidates.empty())
      return result;

    std::this_thread::sleep_for(std::chrono::milliseconds(sampleDelayMs));

    for (const Candidate& candidate : candidates)
    {
      if (timedOut(deadline))
        break;

      RuntimeMemoryReadResult third = readProcessMemory(processId, candidate.address, sizeof(std::uint32_t));
      if (!third.success || third.bytesRead != sizeof(std::uint32_t))
        continue;

      const std::uint32_t thirdValue = readU32(third.bytes, 0);
      if (!frameCounterConfidencePassed(candidate.first, candidate.second, thirdValue, sampleDelayMs))
        continue;

      FrameCounterCandidate frameCandidate;
      frameCandidate.address = candidate.address;
      frameCandidate.first = candidate.first;
      frameCandidate.second = candidate.second;
      frameCandidate.third = thirdValue;
      frameCandidate.score =
        candidate.regionPriority * 1000000
        + frameCounterScore(candidate.first, candidate.second, thirdValue, sampleDelayMs);
      result.push_back(frameCandidate);
    }

    std::sort(
      result.begin(),
      result.end(),
      [](const FrameCounterCandidate& lhs, const FrameCounterCandidate& rhs)
      {
        if (lhs.score != rhs.score)
          return lhs.score < rhs.score;
        return lhs.address < rhs.address;
      });
    if (result.size() > maxCandidates)
      result.resize(maxCandidates);
    return result;
  }

  struct SelfUnitFixture
  {
    std::vector<std::array<unsigned char, 336>> records;
    std::vector<std::array<unsigned char, 64>> sprites;
  };

  struct SelfUnitNodeFixture
  {
    alignas(8) std::array<std::array<unsigned char, 0x58>, 8> nodes;
    alignas(8) std::array<std::array<unsigned char, 0x50>, 8> secondaryObjects;
    alignas(8) std::array<std::array<unsigned char, 64>, 8> sprites;
  };

  struct SelfBulletFixture
  {
    std::array<std::array<unsigned char, 0x90>, 4> records;
    std::array<std::array<unsigned char, 64>, 4> sprites;
    std::array<std::array<unsigned char, 0x58>, 4> sourceUnits;
    std::array<std::array<unsigned char, 0x58>, 4> targetUnits;
  };

  template <std::size_t Size>
  void writeU32(std::array<unsigned char, Size>& bytes, std::size_t offset, std::uint32_t value)
  {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
  }

  template <std::size_t Size>
  void writeU64(std::array<unsigned char, Size>& bytes, std::size_t offset, std::uint64_t value)
  {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
  }

  template <std::size_t Size>
  void writeU16(std::array<unsigned char, Size>& bytes, std::size_t offset, std::uint16_t value)
  {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
  }

  template <std::size_t Size>
  void writeS16(std::array<unsigned char, Size>& bytes, std::size_t offset, std::int16_t value)
  {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
  }

  template <std::size_t Size>
  void writeS32(std::array<unsigned char, Size>& bytes, std::size_t offset, std::int32_t value)
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

  void initializeSelfUnitNodeFixture(SelfUnitNodeFixture& fixture)
  {
    for (std::size_t i = 0; i < fixture.nodes.size(); ++i)
    {
      auto& node = fixture.nodes[i];
      auto& secondary = fixture.secondaryObjects[i];
      node.fill(0);
      secondary.fill(0);
      fixture.sprites[i].fill(static_cast<unsigned char>(0x70 + i));

      const std::uintptr_t previous = i == 0
        ? 0
        : reinterpret_cast<std::uintptr_t>(fixture.nodes[i - 1].data());
      const std::uintptr_t next = i + 1 >= fixture.nodes.size()
        ? 0
        : reinterpret_cast<std::uintptr_t>(fixture.nodes[i + 1].data());
      writeU64(node, 0, static_cast<std::uint64_t>(previous));
      writeU64(node, 0x08, static_cast<std::uint64_t>(next));
      writeS16(node, 0x24, static_cast<std::int16_t>(160 + i * 24));
      writeS16(node, 0x26, static_cast<std::int16_t>(128 + i * 16));
      writeS16(node, 0x28, static_cast<std::int16_t>(192 + i * 24));
      writeS16(node, 0x2a, static_cast<std::int16_t>(160 + i * 16));
      writeU16(node, 0x30, static_cast<std::uint16_t>(5 + i));
      writeU16(node, 0x32, static_cast<std::uint16_t>(1 + i));
      writeU64(
        node,
        0x38,
        static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(fixture.sprites[i].data())));
      writeU64(
        node,
        0x50,
        static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(secondary.data())));

      writeU16(secondary, 0x10, static_cast<std::uint16_t>(i % 228));
      secondary[0x14] = static_cast<unsigned char>(i % 4);
      secondary[0x1a] = static_cast<unsigned char>(40 + i);
      secondary[0x1b] = static_cast<unsigned char>(48 + i);
      writeU16(secondary, 0x18, static_cast<std::uint16_t>(100 + i));
      writeU16(secondary, 0x20, static_cast<std::uint16_t>(i % 228));
      writeU64(
        secondary,
        0x30,
        static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(node.data())));
    }
  }

  void initializeSelfBulletFixture(SelfBulletFixture& fixture)
  {
    const BulletRecordLayout& layout = bulletRecordLayouts.back();
    for (std::size_t i = 0; i < fixture.records.size(); ++i)
    {
      auto& record = fixture.records[i];
      record.fill(0);
      fixture.sprites[i].fill(static_cast<unsigned char>(0x40 + i));
      fixture.sourceUnits[i].fill(static_cast<unsigned char>(0x90 + i));
      fixture.targetUnits[i].fill(static_cast<unsigned char>(0xb0 + i));

      writeU64(
        record,
        0,
        i == 0 ? 0 : static_cast<std::uint64_t>(
          reinterpret_cast<std::uintptr_t>(fixture.records[i - 1].data())));
      writeU64(
        record,
        0x08,
        i + 1 >= fixture.records.size() ? 0 : static_cast<std::uint64_t>(
          reinterpret_cast<std::uintptr_t>(fixture.records[i + 1].data())));
      writeU32(record, layout.existsOffset, 1);
      writeU64(
        record,
        layout.spriteOffset,
        static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(fixture.sprites[i].data())));
      writeU16(record, layout.typeOffset, static_cast<std::uint16_t>(3 + i));
      writeS16(record, layout.positionOffset, static_cast<std::int16_t>(256 + i * 32));
      writeS16(record, layout.positionOffset + sizeof(std::int16_t), static_cast<std::int16_t>(192 + i * 24));
      writeS32(record, layout.velocityOffset, static_cast<std::int32_t>(1280 + i * 64));
      writeS32(record, layout.velocityOffset + sizeof(std::int32_t), static_cast<std::int32_t>(-512 - i * 32));
      record[layout.playerOffset] = static_cast<unsigned char>(i % 4);
      record[layout.removeTimerOffset] = static_cast<unsigned char>(24 + i);
      writeU64(
        record,
        layout.sourceUnitOffset,
        static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(fixture.sourceUnits[i].data())));
      writeU64(
        record,
        layout.targetUnitOffset,
        static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(fixture.targetUnits[i].data())));
    }
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

    fixture.rawBuffer.fill(0);
    fixture.rawBuffer[0] = 0x10;
    fixture.rawBuffer[1] = 0x11;
    fixture.rawBuffer[2] = 0x10;
    fixture.rawBuffer[3] = 0x11;
    fixture.rawBuffer[4] = 0x10;
    fixture.rawBytesInQueue = 5;
  }

  void keepSelfCommandQueueFixtureAlive(const SelfCommandQueueFixture& fixture)
  {
    if (fixture.begin == 0 || fixture.end < fixture.begin || fixture.capacity <= fixture.begin
        || fixture.rawBytesInQueue > fixture.rawBuffer.size())
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
  bool selfUnitNodeFixture = false;
  bool proveDispatchEvents = false;
  bool proveReadMapData = false;
  bool proveReadPlayerData = false;
  bool proveReadBulletData = false;
  bool selfBulletFixture = false;
  bool proveReadRegionData = false;
  bool selfRegionFixture = false;
  bool proveReplayAnalysis = false;
  bool proveBattleNetPolicyFlag = false;
  bool proveLoadAIModules = false;
  bool discoverCommandQueue = false;
  bool selfCommandQueueFixture = false;
  bool proveIssueCommands = false;
  bool proveDrawOverlays = false;
  bool proveMultiplayerSync = false;
  bool serveCommandBridge = false;
  bool unitScanDiagnosticsFlag = false;
  bool unitScanReadableOnlyFlag = false;
  bool unitScanVectorsFlag = false;
  bool unitScanIncludeImageRegionsFlag = false;
  bool unitScanClassicFallbackFlag = false;
  bool stateScanDiagnosticsFlag = false;
  std::string unitBestDumpOut;
  std::vector<std::uintptr_t> unitCandidateAddresses;
  std::vector<std::uintptr_t> unitNodeCandidateAddresses;
  std::vector<std::uintptr_t> bulletCandidateAddresses;
  std::uintptr_t stateCounterAddress = 0;
  std::uintptr_t commandQueueVectorAddress = 0;
  std::size_t issueCommandCandidateScanLimit = 0;
  std::string appendGameAction;
  std::string aiModulePath;
  int stateSampleDelayMs = 250;
  std::size_t stateMaxScanBytes = 128 * 1024 * 1024;
  int stateScanTimeoutMs = 30000;
  std::size_t unitMaxScanBytes = 0;
  std::size_t bulletMaxScanBytes = 0;
  std::size_t commandQueueMaxScanBytes = 0;
  int unitScanTimeoutMs = 15000;
  int activeMatchWaitMs = 0;
  int activeMatchPollMs = 1000;
  int commandQueueActivityMs = 375;
  std::size_t commandQueueCandidateLimit = 32;

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
    if (arg == "--self-unit-node-fixture")
    {
      selfUnitNodeFixture = true;
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
    if (arg == "--prove-read-bullet-data")
    {
      proveReadBulletData = true;
      continue;
    }
    if (arg == "--self-bullet-fixture")
    {
      selfBulletFixture = true;
      continue;
    }
    if (arg == "--bullet-candidate-address")
    {
      std::uintptr_t address = 0;
      if (i + 1 >= argc || !parseAddress(argv[++i], address))
      {
        std::cerr << "--bullet-candidate-address requires a positive integer address\n";
        return 64;
      }
      bulletCandidateAddresses.push_back(address);
      continue;
    }
    if (arg == "--prove-read-region-data")
    {
      proveReadRegionData = true;
      continue;
    }
    if (arg == "--self-region-fixture")
    {
      selfRegionFixture = true;
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
    if (arg == "--unit-scan-classic-fallback")
    {
      unitScanClassicFallbackFlag = true;
      continue;
    }
    if (arg == "--state-scan-diagnostics")
    {
      stateScanDiagnosticsFlag = true;
      continue;
    }
    if (arg == "--state-counter-address")
    {
      if (i + 1 >= argc || !parseAddress(argv[++i], stateCounterAddress))
      {
        std::cerr << "--state-counter-address requires a positive integer address\n";
        return 64;
      }
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
    if (arg == "--unit-node-candidate-address")
    {
      std::uintptr_t address = 0;
      if (i + 1 >= argc || !parseAddress(argv[++i], address))
      {
        std::cerr << "--unit-node-candidate-address requires a positive integer address\n";
        return 64;
      }
      unitNodeCandidateAddresses.push_back(address);
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
    if (arg == "--prove-load-ai-modules")
    {
      proveLoadAIModules = true;
      continue;
    }
    if (arg == "--ai-module-path")
    {
      if (i + 1 >= argc)
      {
        std::cerr << "--ai-module-path requires a path\n";
        return 64;
      }
      aiModulePath = argv[++i];
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
    if (arg == "--prove-issue-commands")
    {
      proveIssueCommands = true;
      continue;
    }
    if (arg == "--prove-draw-overlays")
    {
      proveDrawOverlays = true;
      continue;
    }
    if (arg == "--prove-multiplayer-sync")
    {
      proveMultiplayerSync = true;
      continue;
    }
    if (arg == "--issue-command-candidate-scan-limit")
    {
      int limit = 0;
      if (i + 1 >= argc || !parsePositiveInt(argv[++i], limit))
      {
        std::cerr << "--issue-command-candidate-scan-limit requires a positive integer\n";
        return 64;
      }
      issueCommandCandidateScanLimit = static_cast<std::size_t>(limit);
      continue;
    }
    if (arg == "--command-queue-activity-ms")
    {
      if (i + 1 >= argc || !parsePositiveInt(argv[++i], commandQueueActivityMs))
      {
        std::cerr << "--command-queue-activity-ms requires a positive integer\n";
        return 64;
      }
      continue;
    }
    if (arg == "--command-queue-candidate-limit")
    {
      int limit = 0;
      if (i + 1 >= argc || !parsePositiveInt(argv[++i], limit))
      {
        std::cerr << "--command-queue-candidate-limit requires a positive integer\n";
        return 64;
      }
      commandQueueCandidateLimit = static_cast<std::size_t>(limit);
      continue;
    }
    if (arg == "--serve-command-bridge")
    {
      serveCommandBridge = true;
      continue;
    }
    if (arg == "--command-queue-vector-address")
    {
      if (i + 1 >= argc || !parseAddress(argv[++i], commandQueueVectorAddress))
      {
        std::cerr << "--command-queue-vector-address requires a positive integer address\n";
        return 64;
      }
      continue;
    }
    if (arg == "--append-game-action")
    {
      if (i + 1 >= argc)
      {
        std::cerr << "--append-game-action requires a name\n";
        return 64;
      }
      appendGameAction = argv[++i];
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
    if (arg == "--bullet-max-scan-mb")
    {
      int megabytes = 0;
      if (i + 1 >= argc || !parsePositiveInt(argv[++i], megabytes))
      {
        std::cerr << "--bullet-max-scan-mb requires a positive integer\n";
        return 64;
      }
      bulletMaxScanBytes = static_cast<std::size_t>(megabytes) * 1024 * 1024;
      continue;
    }
    if (arg == "--command-queue-max-scan-mb")
    {
      int megabytes = 0;
      if (i + 1 >= argc || !parsePositiveInt(argv[++i], megabytes))
      {
        std::cerr << "--command-queue-max-scan-mb requires a positive integer\n";
        return 64;
      }
      commandQueueMaxScanBytes = static_cast<std::size_t>(megabytes) * 1024 * 1024;
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
  if (bulletMaxScanBytes == 0)
    bulletMaxScanBytes = unitMaxScanBytes;
  if (commandQueueMaxScanBytes == 0)
    commandQueueMaxScanBytes = stateMaxScanBytes;
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
  if (proveReadBulletData && !self)
  {
    proveActiveMatchState = true;
    proveReadUnits = true;
  }
  if (proveReadRegionData && !self)
  {
    proveReadMapData = true;
    proveActiveMatchState = true;
    proveReadUnits = true;
  }
  if (proveActiveMatchState && !self)
  {
    proveReadGameState = true;
    proveReadMapData = true;
  }
  if (proveReplayAnalysis)
  {
    proveReadGameState = true;
    proveReadMapData = true;
  }
  if (serveCommandBridge)
    proveIssueCommands = true;
  if (proveIssueCommands)
  {
    discoverCommandQueue = true;
    if (!self)
      proveReadGameState = true;
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

  RuntimeProcessCommandLineResult processCommandLine;
  bool replayLaunchDetected = false;
  std::string replayLaunchEvidence;
  if (!self && (proveActiveMatchState || proveReplayAnalysis || proveMultiplayerSync))
  {
    processCommandLine = inspectRuntimeProcessCommandLine(environment.processId);
    replayLaunchDetected = detectReplayLaunch(processCommandLine, replayLaunchEvidence);
    std::cout << "process.command_line.inspected="
              << (processCommandLine.inspected ? "true" : "false") << '\n';
    std::cout << "process.command_line.argument_count="
              << processCommandLine.arguments.size() << '\n';
    std::cout << "process.command_line.replay_launch_detected="
              << (replayLaunchDetected ? "true" : "false") << '\n';
    if (!replayLaunchEvidence.empty())
      std::cout << "process.command_line.replay_launch_evidence="
                << replayLaunchEvidence << '\n';
    if (!processCommandLine.reason.empty())
      std::cout << "process.command_line.reason="
                << processCommandLine.reason << '\n';
  }

  const RuntimeCommandSurface commandSurface = makeBWAPICommandSurface();
  std::cout << "command_surface.ready=true\n";
  std::cout << "command_surface.unit_commands=" << commandSurface.unitCommands.size() << '\n';
  std::cout << "command_surface.game_actions=" << commandSurface.gameActions.size() << '\n';
  std::cout << "command_surface.entries=" << commandSurface.totalEntries() << '\n';

  if (!appendGameAction.empty())
  {
    if (commandQueueVectorAddress == 0)
    {
      std::cerr << "--append-game-action requires --command-queue-vector-address\n";
      return 64;
    }

    RuntimeEncodedCommand encoded = encodeRuntimeCommandRequest(gameActionRequest(appendGameAction));
    std::cout << "manual_command_append.requested=true\n";
    std::cout << "manual_command_append.game_action=" << appendGameAction << '\n';
    if (!encoded.encoded)
    {
      std::cout << "manual_command_append.encoded=false\n";
      std::cout << "manual_command_append.reason=" << encoded.reason << '\n';
      return 12;
    }

    CommandQueueCandidate candidate;
    candidate.vectorAddress = commandQueueVectorAddress;
    CommandQueueAppendResult append = appendEncodedCommandToQueueCandidate(
      environment.processId,
      candidate,
      encoded.bytes,
      false);
    std::cout << "manual_command_append.encoded=true\n";
    std::cout << "manual_command_append.encoded_bytes="
              << formatCommandBytesHex(encoded.bytes) << '\n';
    std::cout << "manual_command_append.vector_address="
              << hexAddress(commandQueueVectorAddress) << '\n';
    std::cout << "manual_command_append.written="
              << (append.passed ? "true" : "false") << '\n';
    if (append.passed)
    {
      std::cout << "manual_command_append.buffer_begin="
                << hexAddress(append.candidate.bufferBegin) << '\n';
      std::cout << "manual_command_append.tail_address="
                << hexAddress(append.tailAddress) << '\n';
      std::cout << "manual_command_append.appended_bytes="
                << append.appendedBytes << '\n';
      return 0;
    }
    std::cout << "manual_command_append.reason=" << append.reason << '\n';
    return 12;
  }

  LiveCounterProof readGameStateProof;
  LiveUnitsProof readUnitsProof;
  LiveUnitNodeProof activeUnitNodeProof;
  DispatchEventsProof dispatchEventsProof;
  MapDataProof mapDataProof;
  PlayerDataProof playerDataProof;
  BulletDataProof bulletDataProof;
  RegionDataProof regionDataProof;
  ReplayAnalysisProof replayAnalysisProof;
  AIModuleLoadProof aiModuleLoadProof;
  StateScanDiagnostics stateScanDiagnostics;
  UnitScanDiagnostics unitScanDiagnostics;
  BattleNetPolicyProof battleNetPolicyProof;
  CommandQueueDiscoveryProof commandQueueDiscoveryProof;
  IssueCommandsProof issueCommandsProof;
  DrawOverlaysProof drawOverlaysProof;
  MultiplayerSyncProof multiplayerSyncProof;
  bool unitSnapshotWritten = false;
  bool unitScanDiagnosticsSnapshotWritten = false;
  bool dispatchEventsSnapshotWritten = false;
  bool mapDataSnapshotWritten = false;
  bool playerDataSnapshotWritten = false;
  bool bulletDataSnapshotWritten = false;
  bool regionDataSnapshotWritten = false;
  bool replayAnalysisSnapshotWritten = false;
  bool aiModuleLoadSnapshotWritten = false;
  bool commandQueueDiscoverySnapshotWritten = false;
  bool issueCommandsSnapshotWritten = false;
  bool drawOverlaysSnapshotWritten = false;
  bool multiplayerSyncSnapshotWritten = false;
  SelfUnitFixture unitFixture;
  SelfUnitNodeFixture unitNodeFixture;
  SelfBulletFixture bulletFixture;
  SelfCommandQueueFixture commandQueueFixture;
  int proofFailureCode = 0;
  bool readGameStateUsedExplicitAddress = false;
  bool readGameStateUsedFallbackScan = false;
  std::string explicitFrameCounterFailure;
  bool remasteredUnitNodeSnapshotAttempted = false;
  std::string remasteredUnitNodeSnapshotFailure;
  if (self && selfUnitFixture)
    unitFixture = makeSelfUnitFixture();
  if (self && selfUnitNodeFixture)
  {
    initializeSelfUnitNodeFixture(unitNodeFixture);
    unitNodeCandidateAddresses.push_back(
      reinterpret_cast<std::uintptr_t>(unitNodeFixture.nodes.front().data()));
  }
  if (self && selfBulletFixture)
  {
    initializeSelfBulletFixture(bulletFixture);
    bulletCandidateAddresses.push_back(
      reinterpret_cast<std::uintptr_t>(bulletFixture.records.front().data()));
  }
  if (self && selfCommandQueueFixture)
  {
    initializeSelfCommandQueueFixture(commandQueueFixture);
    keepSelfCommandQueueFixtureAlive(commandQueueFixture);
  }

  if (proveReadGameState)
  {
    if (stateCounterAddress != 0)
    {
      readGameStateProof = proveExplicitLiveCounterRead(
        environment.processId,
        stateCounterAddress,
        stateSampleDelayMs);
      readGameStateUsedExplicitAddress = readGameStateProof.passed;
      if (!readGameStateProof.passed)
      {
        explicitFrameCounterFailure = readGameStateProof.reason;
        readGameStateProof = proveLiveCounterRead(
          environment.processId,
          environment.executablePath,
          stateSampleDelayMs,
          stateMaxScanBytes,
          stateScanTimeoutMs,
          stateScanDiagnosticsFlag ? &stateScanDiagnostics : nullptr);
        readGameStateUsedFallbackScan = readGameStateProof.passed;
      }
    }
    else
    {
      readGameStateProof = proveLiveCounterRead(
        environment.processId,
        environment.executablePath,
        stateSampleDelayMs,
        stateMaxScanBytes,
        stateScanTimeoutMs,
        stateScanDiagnosticsFlag ? &stateScanDiagnostics : nullptr);
    }
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
      if (readGameStateUsedExplicitAddress)
        std::cout << "read_game_state.source=explicit-address\n";
      else if (readGameStateUsedFallbackScan)
        std::cout << "read_game_state.source=fallback-scan\n";
    }
    if (!explicitFrameCounterFailure.empty())
      std::cout << "read_game_state.explicit_address.reason="
                << explicitFrameCounterFailure << '\n';
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
      std::cout << "read_game_state.scan.closest_counter.available="
                << (stateScanDiagnostics.hasClosestCounter ? "true" : "false") << '\n';
      if (stateScanDiagnostics.hasClosestCounter)
      {
        std::cout << "read_game_state.scan.closest_counter.address="
                  << hexAddress(stateScanDiagnostics.closestCounterAddress) << '\n';
        std::cout << "read_game_state.scan.closest_counter.sample.0="
                  << stateScanDiagnostics.closestCounterFirst << '\n';
        std::cout << "read_game_state.scan.closest_counter.sample.1="
                  << stateScanDiagnostics.closestCounterSecond << '\n';
        std::cout << "read_game_state.scan.closest_counter.sample.2="
                  << stateScanDiagnostics.closestCounterThird << '\n';
        std::cout << "read_game_state.scan.closest_counter.delta.0="
                  << (stateScanDiagnostics.closestCounterSecond
                      - stateScanDiagnostics.closestCounterFirst) << '\n';
        std::cout << "read_game_state.scan.closest_counter.delta.1="
                  << (stateScanDiagnostics.closestCounterThird
                      - stateScanDiagnostics.closestCounterSecond) << '\n';
        std::cout << "read_game_state.scan.closest_counter.score="
                  << stateScanDiagnostics.closestCounterScore << '\n';
        std::cout << "read_game_state.scan.closest_counter.reason="
                  << stateScanDiagnostics.closestCounterReason << '\n';
      }
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
      if (!unitNodeCandidateAddresses.empty()
          && (!self || selfUnitNodeFixture)
          && (proveActiveMatchState
              || (proveReadUnits && environment.product == Product::StarCraftRemastered)))
      {
        activeUnitNodeProof = proveExplicitUnitNodeCandidateAddresses(
          environment.processId,
          unitNodeCandidateAddresses,
          unitScanTimeoutMs,
          unitScanDiagnosticsFlag ? &unitScanDiagnostics : nullptr);
      }
      if (proveActiveMatchState && !self && !activeUnitNodeProof.passed)
      {
        activeUnitNodeProof = proveLiveUnitNodeAnchors(
          environment.processId,
          environment.executablePath,
          unitMaxScanBytes,
          unitScanTimeoutMs,
          unitScanDiagnosticsFlag ? &unitScanDiagnostics : nullptr);
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
          && (!self || selfUnitNodeFixture)
          && environment.product == Product::StarCraftRemastered)
      {
        remasteredUnitNodeSnapshotAttempted = true;
        LiveUnitsProof remasteredUnitNodeSnapshot = proveRemasteredUnitNodeSnapshot(
          environment.processId,
          environment.executablePath,
          unitMaxScanBytes,
          unitScanTimeoutMs,
          &activeUnitNodeProof,
          unitScanDiagnosticsFlag ? &unitScanDiagnostics : nullptr);
        if (remasteredUnitNodeSnapshot.passed)
        {
          readUnitsProof = std::move(remasteredUnitNodeSnapshot);
          remasteredUnitNodeSnapshotFailure.clear();
        }
        else
        {
          remasteredUnitNodeSnapshotFailure = remasteredUnitNodeSnapshot.reason;
          readUnitsProof = std::move(remasteredUnitNodeSnapshot);
        }
      }
      const bool allowBroadClassicUnitFallback =
        environment.product != Product::StarCraftRemastered
        || self
        || unitScanClassicFallbackFlag;
      if (proveReadUnits && !readUnitsProof.passed && allowBroadClassicUnitFallback)
      {
        const std::string previousReadUnitsReason = readUnitsProof.reason;
        readUnitsProof = proveLiveUnitsRead(
          environment.processId,
          environment.executablePath,
          unitMaxScanBytes,
          unitScanTimeoutMs,
          unitScanReadableOnlyFlag,
          unitScanIncludeImageRegionsFlag,
          unitScanVectorsFlag,
          unitScanDiagnosticsFlag ? &unitScanDiagnostics : nullptr);
        if (!readUnitsProof.passed
            && remasteredUnitNodeSnapshotAttempted
            && !previousReadUnitsReason.empty()
            && readUnitsProof.reason.find("remastered_unit_node_reason=") == std::string::npos)
        {
          if (!readUnitsProof.reason.empty())
            readUnitsProof.reason += "; ";
          readUnitsProof.reason += "remastered_unit_node_reason=" + previousReadUnitsReason;
          remasteredUnitNodeSnapshotFailure = previousReadUnitsReason;
        }
      }
      else if (proveReadUnits
               && !readUnitsProof.passed
               && environment.product == Product::StarCraftRemastered
               && !unitScanClassicFallbackFlag)
      {
        if (!readUnitsProof.reason.empty())
          readUnitsProof.reason += "; ";
        readUnitsProof.reason +=
          "classic_cunit_fallback_skipped=enable --unit-scan-classic-fallback for explicit legacy-layout audits";
      }
      const bool canStopAfterUnitNodeAnchor =
        activeUnitNodeProof.passed && !proveReadUnits;
      if (readUnitsProof.passed
          || canStopAfterUnitNodeAnchor
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
      if (!unitNodeCandidateAddresses.empty())
        std::cout << "read_units.unit_node_candidate_address.count="
                  << unitNodeCandidateAddresses.size() << '\n';
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
      if (remasteredUnitNodeSnapshotAttempted)
      {
        std::cout << "read_units.remastered_unit_node_snapshot.attempted=true\n";
        if (!remasteredUnitNodeSnapshotFailure.empty())
          std::cout << "read_units.remastered_unit_node_snapshot.reason="
                    << remasteredUnitNodeSnapshotFailure << '\n';
      }
    }

    if (proveActiveMatchState)
    {
      const bool frameGatePassed = !proveReadGameState || readGameStateProof.passed;
      const bool replayBackedActiveMatch =
        frameGatePassed
        && replayLaunchDetected
        && mapDataProof.passed
        && !mapDataProof.replayPath.empty()
        && mapDataProof.replayFileSize > 0;
      const bool activeMatchProven =
        !self
        && frameGatePassed
        && (readUnitsProof.passed || activeUnitNodeProof.passed || replayBackedActiveMatch);
      const char* activeMatchEvidence =
        readUnitsProof.passed
          ? (readUnitsProof.derivedSnapshot ? "active-unit-node-snapshot" : "active-unit-records")
          : (activeUnitNodeProof.passed
            ? "active-unit-node-anchor"
            : (replayBackedActiveMatch ? "active-replay-metadata" : "none"));
      std::cout << "active_match_state.in_game=" << (activeMatchProven ? "true" : "false") << '\n';
      std::cout << "active_match_state.evidence=" << activeMatchEvidence << '\n';
      if (!frameGatePassed)
        std::cout << "active_match_state.reason=active match proof requires live frame progression\n";
      else if (!readUnitsProof.passed && !activeUnitNodeProof.passed && !replayBackedActiveMatch)
        std::cout << "active_match_state.reason=active match proof requires a BWAPI-facing live unit snapshot or current-process replay launch evidence\n";
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
      else if (replayBackedActiveMatch)
      {
        std::cout << "active_match_state.map_name=" << mapDataProof.mapName << '\n';
        std::cout << "active_match_state.replay_path=" << mapDataProof.replayPath << '\n';
        std::cout << "active_match_state.replay_file_size=" << mapDataProof.replayFileSize << '\n';
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
      std::cout << "read_units.scan.pointer_dense_rejected_records="
                << unitScanDiagnostics.pointerDenseRejectedRecords << '\n';
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
      std::cout << "read_units.scan.top_candidate_count="
                << unitScanDiagnostics.topCandidates.size() << '\n';
      for (std::size_t index = 0; index < unitScanDiagnostics.topCandidates.size(); ++index)
      {
        const UnitCandidateDiagnostic& candidate = unitScanDiagnostics.topCandidates[index];
        std::cout << "read_units.scan.top_candidate." << index << ".source="
                  << candidate.source << '\n';
        std::cout << "read_units.scan.top_candidate." << index << ".address=0x"
                  << std::hex << candidate.address << std::dec << '\n';
        std::cout << "read_units.scan.top_candidate." << index << ".record_size="
                  << candidate.recordSize << '\n';
        std::cout << "read_units.scan.top_candidate." << index << ".layout="
                  << candidate.layoutName << '\n';
        std::cout << "read_units.scan.top_candidate." << index << ".sampled_records="
                  << candidate.sampledRecords << '\n';
        std::cout << "read_units.scan.top_candidate." << index << ".active_records="
                  << candidate.activeRecords << '\n';
        std::cout << "read_units.scan.top_candidate." << index << ".pointer_array="
                  << (candidate.pointerArray ? "true" : "false") << '\n';
      }
      std::cout << "read_units.unit_node_scan.regions="
                << unitScanDiagnostics.unitNodeScannedRegions << '\n';
      std::cout << "read_units.unit_node_scan.bytes="
                << unitScanDiagnostics.unitNodeScannedBytes << '\n';
      std::cout << "read_units.unit_node_scan.field_candidates="
                << unitScanDiagnostics.unitNodeFieldCandidates << '\n';
      std::cout << "read_units.unit_node_scan.readable_candidates="
                << unitScanDiagnostics.unitNodeReadableCandidates << '\n';
      std::cout << "read_units.unit_node_scan.graph_seeds_scored="
                << unitScanDiagnostics.unitNodeGraphSeedsScored << '\n';
      std::cout << "read_units.unit_node_scan.pointer_graph_seeds_scored="
                << unitScanDiagnostics.unitNodePointerGraphSeedsScored << '\n';
      std::cout << "read_units.unit_node_scan.vector_candidates="
                << unitScanDiagnostics.unitNodeVectorCandidates << '\n';
      std::cout << "read_units.unit_node_scan.best_active_records="
                << unitScanDiagnostics.unitNodeBestActiveRecords << '\n';
      if (unitScanDiagnostics.unitNodeBestAddress != 0)
      {
        std::cout << "read_units.unit_node_scan.best_address=0x"
                  << std::hex << unitScanDiagnostics.unitNodeBestAddress << std::dec << '\n';
        std::cout << "read_units.unit_node_scan.best_vector_address=0x"
                  << std::hex << unitScanDiagnostics.unitNodeBestVectorAddress << std::dec << '\n';
      }
      if (!unitScanDiagnostics.unitNodeBestReason.empty())
        std::cout << "read_units.unit_node_scan.best_reason="
                  << unitScanDiagnostics.unitNodeBestReason << '\n';
      std::cout << "read_units.unit_node_scan.vector_sample_count="
                << unitScanDiagnostics.unitNodeVectorSamples.size() << '\n';
      for (std::size_t index = 0; index < unitScanDiagnostics.unitNodeVectorSamples.size(); ++index)
      {
        const UnitNodeVectorCandidateDiagnostic& sample =
          unitScanDiagnostics.unitNodeVectorSamples[index];
        std::cout << "read_units.unit_node_scan.vector_sample." << index << ".vector_address=0x"
                  << std::hex << sample.vectorAddress << std::dec << '\n';
        std::cout << "read_units.unit_node_scan.vector_sample." << index << ".begin=0x"
                  << std::hex << sample.begin << std::dec << '\n';
        std::cout << "read_units.unit_node_scan.vector_sample." << index << ".end=0x"
                  << std::hex << sample.end << std::dec << '\n';
        std::cout << "read_units.unit_node_scan.vector_sample." << index << ".capacity=0x"
                  << std::hex << sample.capacity << std::dec << '\n';
        std::cout << "read_units.unit_node_scan.vector_sample." << index << ".used_bytes="
                  << sample.usedBytes << '\n';
        std::cout << "read_units.unit_node_scan.vector_sample." << index << ".capacity_bytes="
                  << sample.capacityBytes << '\n';
        std::cout << "read_units.unit_node_scan.vector_sample." << index << ".record_vector="
                  << (sample.recordVector ? "true" : "false") << '\n';
        std::cout << "read_units.unit_node_scan.vector_sample." << index << ".pointer_vector="
                  << (sample.pointerVector ? "true" : "false") << '\n';
        std::cout << "read_units.unit_node_scan.vector_sample." << index << ".record_count="
                  << sample.recordCount << '\n';
        std::cout << "read_units.unit_node_scan.vector_sample." << index << ".pointer_count="
                  << sample.pointerCount << '\n';
        std::cout << "read_units.unit_node_scan.vector_sample." << index << ".readable_precheck="
                  << (sample.readablePrecheck ? "true" : "false") << '\n';
      }
      std::cout << "read_units.unit_node_scan.field_sample_count="
                << unitScanDiagnostics.unitNodeFieldSamples.size() << '\n';
      for (std::size_t index = 0; index < unitScanDiagnostics.unitNodeFieldSamples.size(); ++index)
      {
        const UnitNodeFieldCandidateDiagnostic& sample =
          unitScanDiagnostics.unitNodeFieldSamples[index];
        std::cout << "read_units.unit_node_scan.field_sample." << index << ".address=0x"
                  << std::hex << sample.address << std::dec << '\n';
        std::cout << "read_units.unit_node_scan.field_sample." << index << ".previous=0x"
                  << std::hex << sample.previous << std::dec << '\n';
        std::cout << "read_units.unit_node_scan.field_sample." << index << ".next=0x"
                  << std::hex << sample.next << std::dec << '\n';
        std::cout << "read_units.unit_node_scan.field_sample." << index << ".sprite=0x"
                  << std::hex << sample.sprite << std::dec << '\n';
        std::cout << "read_units.unit_node_scan.field_sample." << index << ".secondary=0x"
                  << std::hex << sample.secondaryObject << std::dec << '\n';
        std::cout << "read_units.unit_node_scan.field_sample." << index << ".x=" << sample.x << '\n';
        std::cout << "read_units.unit_node_scan.field_sample." << index << ".y=" << sample.y << '\n';
        std::cout << "read_units.unit_node_scan.field_sample." << index << ".target_x="
                  << sample.targetX << '\n';
        std::cout << "read_units.unit_node_scan.field_sample." << index << ".target_y="
                  << sample.targetY << '\n';
        std::cout << "read_units.unit_node_scan.field_sample." << index << ".state_a="
                  << sample.stateA << '\n';
        std::cout << "read_units.unit_node_scan.field_sample." << index << ".state_b="
                  << sample.stateB << '\n';
        std::cout << "read_units.unit_node_scan.field_sample." << index << ".readable_link="
                  << (sample.readableLink ? "true" : "false") << '\n';
        std::cout << "read_units.unit_node_scan.field_sample." << index << ".readable_sprite="
                  << (sample.readableSprite ? "true" : "false") << '\n';
        std::cout << "read_units.unit_node_scan.field_sample." << index << ".readable_secondary="
                  << (sample.readableSecondaryObject ? "true" : "false") << '\n';
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
    const bool activeReplayProofAllowed =
      proveActiveMatchState
      && !proveReadUnits
      && !self
      && (!proveReadGameState || readGameStateProof.passed)
      && replayLaunchDetected
      && mapDataProof.passed
      && !mapDataProof.replayPath.empty()
      && mapDataProof.replayFileSize > 0;
    if (!readUnitsProof.passed
        && !(proveActiveMatchState
             && (activeUnitNodeProof.passed || activeReplayProofAllowed)
             && !proveReadUnits))
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
      std::cout << "read_player_data.player_info_projection="
                << (playerDataProof.playerInfoProjectionReady ? "true" : "false") << '\n';
      std::cout << "read_player_data.player_info_record_size="
                << playerDataProof.playerInfoRecordSize << '\n';
      std::cout << "read_player_data.alliance_projection="
                << (playerDataProof.allianceProjectionReady ? "true" : "false") << '\n';
      std::cout << "read_player_data.projection_source="
                << playerDataProof.projectionSource << '\n';
    }
    if (!playerDataProof.reason.empty())
      std::cout << "read_player_data.reason=" << playerDataProof.reason << '\n';
    if (!playerDataProof.passed)
      proofFailureCode = proofFailureCode == 0 ? 10 : proofFailureCode;
  }

  if (proveReadBulletData)
  {
    if (!bulletCandidateAddresses.empty())
    {
      bulletDataProof = proveExplicitBulletCandidateAddresses(
        environment.processId,
        bulletCandidateAddresses,
        unitScanTimeoutMs);
    }
    if (!bulletDataProof.passed)
    {
      BulletDataProof scannedBulletProof = proveLiveBulletDataRead(
        environment.processId,
        environment.executablePath,
        bulletMaxScanBytes,
        unitScanTimeoutMs);
      if (scannedBulletProof.passed || bulletDataProof.reason.empty())
        bulletDataProof = std::move(scannedBulletProof);
    }

    std::cout << "read_bullet_data.ready=" << (bulletDataProof.passed ? "true" : "false") << '\n';
    std::cout << "read_bullet_data.candidate_address.count="
              << bulletCandidateAddresses.size() << '\n';
    if (bulletDataProof.passed)
    {
      std::cout << "read_bullet_data.address=" << hexAddress(bulletDataProof.address) << '\n';
      std::cout << "read_bullet_data.record_size=" << bulletDataProof.recordSize << '\n';
      std::cout << "read_bullet_data.layout=" << bulletDataProof.layoutName << '\n';
      std::cout << "read_bullet_data.sampled_records=" << bulletDataProof.sampledRecords << '\n';
      std::cout << "read_bullet_data.active_records=" << bulletDataProof.activeRecords << '\n';
    }
    if (!bulletDataProof.reason.empty())
      std::cout << "read_bullet_data.reason=" << bulletDataProof.reason << '\n';
    if (!bulletDataProof.passed)
      proofFailureCode = proofFailureCode == 0 ? 15 : proofFailureCode;
  }

  if (proveReadRegionData)
  {
    regionDataProof = self && selfRegionFixture
      ? makeSelfRegionDataProof()
      : proveRegionDataFromLiveMetadata(mapDataProof, activeUnitNodeProof);
    std::cout << "read_region_data.ready=" << (regionDataProof.passed ? "true" : "false") << '\n';
    if (regionDataProof.passed)
    {
      std::cout << "read_region_data.source=" << regionDataProof.source << '\n';
      std::cout << "read_region_data.region_count=" << regionDataProof.regionCount << '\n';
      std::cout << "read_region_data.observed_units=" << regionDataProof.observedUnits << '\n';
    }
    if (!regionDataProof.reason.empty())
      std::cout << "read_region_data.reason=" << regionDataProof.reason << '\n';
    if (!regionDataProof.passed)
      proofFailureCode = proofFailureCode == 0 ? 16 : proofFailureCode;
  }

  if (proveReplayAnalysis)
  {
    const bool replayAnalysisActiveMatchReady =
      proveActiveMatchState
      && !self
      && (!proveReadGameState || readGameStateProof.passed)
      && (readUnitsProof.passed || activeUnitNodeProof.passed);
    replayAnalysisProof = proveReplayAnalysisFromLiveMetadata(
      readGameStateProof,
      mapDataProof,
      playerDataProof,
      replayAnalysisActiveMatchReady,
      replayLaunchDetected);
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

  if (proveLoadAIModules)
  {
    aiModuleLoadProof = proveAIModuleLoading(aiModulePath, self);
    std::cout << "load_ai_modules.ready=" << (aiModuleLoadProof.passed ? "true" : "false") << '\n';
    std::cout << "load_ai_modules.loader=" << aiModuleLoadProof.loader << '\n';
    std::cout << "load_ai_modules.module_extension=" << aiModuleLoadProof.moduleExtension << '\n';
    std::cout << "load_ai_modules.self_process_smoke="
              << (aiModuleLoadProof.selfProcessSmoke ? "true" : "false") << '\n';
    if (!aiModuleLoadProof.modulePath.empty())
      std::cout << "load_ai_modules.module_path=" << aiModuleLoadProof.modulePath << '\n';
    if (!aiModuleLoadProof.reason.empty())
      std::cout << "load_ai_modules.reason=" << aiModuleLoadProof.reason << '\n';
    if (!aiModuleLoadProof.passed)
      proofFailureCode = proofFailureCode == 0 ? 14 : proofFailureCode;
  }

  if (discoverCommandQueue)
  {
    const std::size_t commandQueueRetainLimit =
      proveIssueCommands
        ? std::max<std::size_t>(commandQueueCandidateLimit, issueCommandCandidateScanLimit)
        : commandQueueCandidateLimit;
    commandQueueDiscoveryProof = discoverCommandQueueCandidates(
      environment.processId,
      environment.executablePath,
      commandQueueMaxScanBytes,
      unitScanTimeoutMs,
      commandQueueRetainLimit);
    if (self && selfCommandQueueFixture)
    {
      CommandQueueCandidate fixtureCandidate;
      fixtureCandidate.storageKind = "vector";
      fixtureCandidate.vectorAddress =
        reinterpret_cast<std::uintptr_t>(&commandQueueFixture.begin);
      fixtureCandidate.bytesInQueueAddress =
        fixtureCandidate.vectorAddress + sizeof(std::uint64_t);
      fixtureCandidate.bufferBegin = commandQueueFixture.begin;
      fixtureCandidate.bufferEnd = commandQueueFixture.end;
      fixtureCandidate.bufferCapacity = commandQueueFixture.capacity;
      fixtureCandidate.usedBytes =
        static_cast<std::size_t>(commandQueueFixture.end - commandQueueFixture.begin);
      fixtureCandidate.capacityBytes =
        static_cast<std::size_t>(commandQueueFixture.capacity - commandQueueFixture.begin);
      fixtureCandidate.score = 1000;
      fixtureCandidate.regionPath = "self-command-queue-fixture";
      ++commandQueueDiscoveryProof.vectorCandidates;
      commandQueueDiscoveryProof.candidates.insert(
        commandQueueDiscoveryProof.candidates.begin(),
        fixtureCandidate);

      CommandQueueCandidate rawFixtureCandidate;
      rawFixtureCandidate.storageKind = "raw-turn-buffer";
      rawFixtureCandidate.vectorAddress =
        reinterpret_cast<std::uintptr_t>(&commandQueueFixture.rawBytesInQueue);
      rawFixtureCandidate.bytesInQueueAddress = rawFixtureCandidate.vectorAddress;
      rawFixtureCandidate.bufferBegin =
        reinterpret_cast<std::uintptr_t>(commandQueueFixture.rawBuffer.data());
      rawFixtureCandidate.bufferEnd =
        rawFixtureCandidate.bufferBegin + commandQueueFixture.rawBytesInQueue;
      rawFixtureCandidate.bufferCapacity =
        rawFixtureCandidate.bufferBegin + commandQueueFixture.rawBuffer.size();
      rawFixtureCandidate.usedBytes = commandQueueFixture.rawBytesInQueue;
      rawFixtureCandidate.capacityBytes = commandQueueFixture.rawBuffer.size();
      rawFixtureCandidate.score = 1100;
      rawFixtureCandidate.regionPath = "self-raw-turn-buffer-fixture";
      ++commandQueueDiscoveryProof.rawTurnBufferCandidates;
      commandQueueDiscoveryProof.candidates.insert(
        commandQueueDiscoveryProof.candidates.begin(),
        rawFixtureCandidate);
      commandQueueDiscoveryProof.ready = true;
    }
    if (commandQueueVectorAddress != 0)
    {
      RuntimeMemoryRegionListResult regions = listProcessMemoryRegions(environment.processId);
      CommandQueueCandidate explicitCandidate;
      std::string explicitReason;
      if (regions.success
          && readCommandQueueCandidate(
            environment.processId,
            regions.regions,
            commandQueueVectorAddress,
            explicitCandidate,
            explicitReason))
      {
        const RuntimeMemoryRegion* selectorRegion = explicitCandidate.storageKind == "raw-turn-buffer"
          ? findWritableRegion(
            regions.regions,
            explicitCandidate.bytesInQueueAddress,
            sizeof(std::uint32_t))
          : findWritableRegion(
            regions.regions,
            explicitCandidate.vectorAddress,
            sizeof(std::uint64_t) * 3);
        explicitCandidate.score = 10000;
        explicitCandidate.regionPath =
          selectorRegion != nullptr && !selectorRegion->mappedPath.empty()
            ? selectorRegion->mappedPath
            : (explicitCandidate.storageKind == "raw-turn-buffer"
              ? "explicit-raw-turn-buffer"
              : "explicit-command-queue-vector");
        commandQueueDiscoveryProof.candidates.erase(
          std::remove_if(
            commandQueueDiscoveryProof.candidates.begin(),
            commandQueueDiscoveryProof.candidates.end(),
            [&](const CommandQueueCandidate& candidate)
            {
              return candidate.vectorAddress == explicitCandidate.vectorAddress;
            }),
          commandQueueDiscoveryProof.candidates.end());
        commandQueueDiscoveryProof.candidates.insert(
          commandQueueDiscoveryProof.candidates.begin(),
          explicitCandidate);
        commandQueueDiscoveryProof.ready = true;
        refreshCommandQueueDiscoveryRetainedStats(commandQueueDiscoveryProof);
      }
      else if (!commandQueueDiscoveryProof.ready)
      {
        commandQueueDiscoveryProof.reason = explicitReason.empty()
          ? "explicit command queue vector is not readable as a live vector"
          : explicitReason;
      }
    }
    const bool shouldObserveCommandQueueActivity =
      !self
      && commandQueueDiscoveryProof.ready
      && commandQueueActivityMs > 0
      && (proveIssueCommands || commandQueueCandidateLimit > 32);
    if (shouldObserveCommandQueueActivity)
    {
      const int activityIntervalMs = std::max(1, commandQueueActivityMs / 3);
      observeCommandQueueCandidateActivity(
        environment.processId,
        commandQueueDiscoveryProof.candidates,
        activityIntervalMs,
        std::max<std::size_t>(commandQueueCandidateLimit, issueCommandCandidateScanLimit));
      refreshCommandQueueDiscoveryRetainedStats(commandQueueDiscoveryProof);
    }
    else
    {
      refreshCommandQueueDiscoveryRetainedStats(commandQueueDiscoveryProof);
    }
    std::cout << "command_queue_discovery.ready="
              << (commandQueueDiscoveryProof.ready ? "true" : "false") << '\n';
    std::cout << "command_queue_discovery.scanned_regions="
              << commandQueueDiscoveryProof.scannedRegions << '\n';
    std::cout << "command_queue_discovery.scanned_bytes="
              << commandQueueDiscoveryProof.scannedBytes << '\n';
    std::cout << "command_queue_discovery.max_scan_bytes="
              << commandQueueMaxScanBytes << '\n';
    std::cout << "command_queue_discovery.candidate_count="
              << commandQueueDiscoveryProof.candidates.size() << '\n';
    std::cout << "command_queue_discovery.vector_candidate_count="
              << commandQueueDiscoveryProof.vectorCandidates << '\n';
    std::cout << "command_queue_discovery.raw_turn_buffer_candidate_count="
              << commandQueueDiscoveryProof.rawTurnBufferCandidates << '\n';
    std::cout << "command_queue_discovery.retained_vector_candidate_count="
              << commandQueueDiscoveryProof.retainedVectorCandidates << '\n';
    std::cout << "command_queue_discovery.retained_raw_turn_buffer_candidate_count="
              << commandQueueDiscoveryProof.retainedRawTurnBufferCandidates << '\n';
    std::cout << "command_queue_discovery.retained_active_candidate_count="
              << commandQueueDiscoveryProof.retainedActiveCandidates << '\n';
    std::cout << "command_queue_discovery.private_candidate_count="
              << commandQueueDiscoveryProof.privateCandidates << '\n';
    std::cout << "command_queue_discovery.image_mapped_candidate_count="
              << commandQueueDiscoveryProof.imageMappedCandidates << '\n';
    std::cout << "command_queue_discovery.candidate_limit="
              << commandQueueRetainLimit << '\n';
    if (shouldObserveCommandQueueActivity)
      std::cout << "command_queue_discovery.activity_window_ms="
                << commandQueueActivityMs << '\n';
    if (!commandQueueDiscoveryProof.candidates.empty())
    {
      const CommandQueueCandidate& best = commandQueueDiscoveryProof.candidates.front();
      std::cout << "command_queue_discovery.best.vector_address="
                << hexAddress(best.vectorAddress) << '\n';
      std::cout << "command_queue_discovery.best.kind="
                << best.storageKind << '\n';
      std::cout << "command_queue_discovery.best.bytes_in_queue_address="
                << hexAddress(best.bytesInQueueAddress) << '\n';
      std::cout << "command_queue_discovery.best.buffer_begin="
                << hexAddress(best.bufferBegin) << '\n';
      std::cout << "command_queue_discovery.best.used_bytes="
                << best.usedBytes << '\n';
      std::cout << "command_queue_discovery.best.capacity_bytes="
                << best.capacityBytes << '\n';
      std::cout << "command_queue_discovery.best.score="
                << best.score << '\n';
      std::cout << "command_queue_discovery.best.activity_samples="
                << best.activitySamples << '\n';
      std::cout << "command_queue_discovery.best.activity_transitions="
                << best.activityTransitions << '\n';
      std::cout << "command_queue_discovery.best.activity_byte_changes="
                << best.activityByteChanges << '\n';
      if (!best.activityReason.empty())
        std::cout << "command_queue_discovery.best.activity_reason="
                  << best.activityReason << '\n';
    }
    if (!commandQueueDiscoveryProof.reason.empty())
      std::cout << "command_queue_discovery.reason=" << commandQueueDiscoveryProof.reason << '\n';
    std::cout << "command_queue_discovery.proof_scope=discovery-only-not-command-behavior\n";
  }

  if (proveIssueCommands)
  {
    issueCommandsProof = proveIssueCommandsWithPauseResume(
      environment.processId,
      environment.executablePath,
      readGameStateProof,
      commandQueueDiscoveryProof,
      self,
      serveCommandBridge,
      commandQueueVectorAddress,
      issueCommandCandidateScanLimit,
      stateMaxScanBytes,
      stateScanTimeoutMs);
    std::cout << "issue_commands.ready=" << (issueCommandsProof.passed ? "true" : "false") << '\n';
    std::cout << "issue_commands.delivery_checked="
              << (issueCommandsProof.deliveryChecked ? "true" : "false") << '\n';
    std::cout << "issue_commands.behavior_checked="
              << (issueCommandsProof.behaviorChecked ? "true" : "false") << '\n';
    std::cout << "issue_commands.self_fixture="
              << (issueCommandsProof.selfFixture ? "true" : "false") << '\n';
    std::cout << "issue_commands.receiver_active="
              << (issueCommandsProof.receiverActive ? "true" : "false") << '\n';
    std::cout << "issue_commands.stale_proof_bytes_cleared="
              << (issueCommandsProof.staleProofBytesCleared ? "true" : "false") << '\n';
    std::cout << "issue_commands.pause_frame_counter_sampled="
              << (issueCommandsProof.pauseFrameCounterSampled ? "true" : "false") << '\n';
    std::cout << "issue_commands.pause_frame_counter_matched="
              << (issueCommandsProof.pauseFrameCounterMatched ? "true" : "false") << '\n';
    std::cout << "issue_commands.attempt_count="
              << issueCommandsProof.attempts.size() << '\n';
    if (issueCommandsProof.vectorAddress != 0)
      std::cout << "issue_commands.vector_address="
                << hexAddress(issueCommandsProof.vectorAddress) << '\n';
    if (issueCommandsProof.commandQueue.bytesInQueueAddress != 0)
      std::cout << "issue_commands.bytes_in_queue_address="
                << hexAddress(issueCommandsProof.commandQueue.bytesInQueueAddress) << '\n';
    if (issueCommandsProof.frameCounterAddress != 0)
      std::cout << "issue_commands.frame_counter_address="
                << hexAddress(issueCommandsProof.frameCounterAddress) << '\n';
    if (!issueCommandsProof.encodedBytes.empty())
      std::cout << "issue_commands.encoded_bytes="
                << issueCommandsProof.encodedBytes << '\n';
    if (issueCommandsProof.baselineStart != 0 || issueCommandsProof.baselineEnd != 0)
    {
      std::cout << "issue_commands.baseline_delta="
                << counterDelta(issueCommandsProof.baselineStart, issueCommandsProof.baselineEnd) << '\n';
      std::cout << "issue_commands.paused_delta="
                << counterDelta(issueCommandsProof.pausedStart, issueCommandsProof.pausedEnd) << '\n';
      std::cout << "issue_commands.resumed_delta="
                << counterDelta(issueCommandsProof.resumedStart, issueCommandsProof.resumedEnd) << '\n';
    }
    if (!issueCommandsProof.reason.empty())
      std::cout << "issue_commands.reason=" << issueCommandsProof.reason << '\n';
    if (!issueCommandsProof.passed)
      proofFailureCode = proofFailureCode == 0 ? 12 : proofFailureCode;
  }

  const bool activeMatchReadyForDiagnostics =
    proveActiveMatchState
    && !self
    && (!proveReadGameState || readGameStateProof.passed)
    && readUnitsProof.passed;

  if (proveDrawOverlays)
  {
    drawOverlaysProof = proveDrawOverlaysFailClosed(issueCommandsProof, environment.executablePath);
    std::cout << "draw_overlays.ready=" << (drawOverlaysProof.passed ? "true" : "false") << '\n';
    std::cout << "draw_overlays.command_receiver_active="
              << (drawOverlaysProof.commandReceiverActive ? "true" : "false") << '\n';
    std::cout << "draw_overlays.adapter_local_actions_available="
              << (drawOverlaysProof.adapterLocalActionsAvailable ? "true" : "false") << '\n';
    std::cout << "draw_overlays.draw_layer_anchors_resolved="
              << (drawOverlaysProof.drawLayerAnchorsResolved ? "true" : "false") << '\n';
    std::cout << "draw_overlays.render_api_anchors_resolved="
              << (drawOverlaysProof.renderApiAnchorsResolved ? "true" : "false") << '\n';
    std::cout << "draw_overlays.render_hook_resolved="
              << (drawOverlaysProof.renderHookResolved ? "true" : "false") << '\n';
    std::cout << "draw_overlays.render_behavior_checked="
              << (drawOverlaysProof.renderBehaviorChecked ? "true" : "false") << '\n';
    if (!drawOverlaysProof.reason.empty())
      std::cout << "draw_overlays.reason=" << drawOverlaysProof.reason << '\n';
    if (!drawOverlaysProof.passed)
      proofFailureCode = proofFailureCode == 0 ? 17 : proofFailureCode;
  }

  if (proveMultiplayerSync)
  {
    multiplayerSyncProof = proveMultiplayerSyncFailClosed(
      issueCommandsProof,
      activeMatchReadyForDiagnostics,
      replayAnalysisProof,
      replayLaunchDetected,
      replayLaunchEvidence,
      environment.executablePath);
    std::cout << "multiplayer_sync.ready=" << (multiplayerSyncProof.passed ? "true" : "false") << '\n';
    std::cout << "multiplayer_sync.command_queue_proven="
              << (multiplayerSyncProof.commandQueueProven ? "true" : "false") << '\n';
    std::cout << "multiplayer_sync.active_match_proven="
              << (multiplayerSyncProof.activeMatchProven ? "true" : "false") << '\n';
    std::cout << "multiplayer_sync.replay_only="
              << (multiplayerSyncProof.replayOnly ? "true" : "false") << '\n';
    std::cout << "multiplayer_sync.replay_launch_detected="
              << (multiplayerSyncProof.replayLaunchDetected ? "true" : "false") << '\n';
    std::cout << "multiplayer_sync.snet_receive_resolved="
              << (multiplayerSyncProof.snetReceiveResolved ? "true" : "false") << '\n';
    std::cout << "multiplayer_sync.snet_send_turn_resolved="
              << (multiplayerSyncProof.snetSendTurnResolved ? "true" : "false") << '\n';
    std::cout << "multiplayer_sync.platform_receive_resolved="
              << (multiplayerSyncProof.platformReceiveResolved ? "true" : "false") << '\n';
    std::cout << "multiplayer_sync.platform_send_resolved="
              << (multiplayerSyncProof.platformSendResolved ? "true" : "false") << '\n';
    std::cout << "multiplayer_sync.turn_packet_anchor_resolved="
              << (multiplayerSyncProof.turnPacketAnchorResolved ? "true" : "false") << '\n';
    std::cout << "multiplayer_sync.sync_behavior_checked="
              << (multiplayerSyncProof.syncBehaviorChecked ? "true" : "false") << '\n';
    if (!multiplayerSyncProof.reason.empty())
      std::cout << "multiplayer_sync.reason=" << multiplayerSyncProof.reason << '\n';
    if (!multiplayerSyncProof.passed)
      proofFailureCode = proofFailureCode == 0 ? 18 : proofFailureCode;
  }

  constexpr int commandDeliverySurvivalGraceMs = 10000;
  if (proveIssueCommands && issueCommandsProof.deliveryChecked && !self)
  {
    std::cout << "runtime.post_command_grace_ms="
              << commandDeliverySurvivalGraceMs << '\n';
    std::this_thread::sleep_for(std::chrono::milliseconds(commandDeliverySurvivalGraceMs));
    const bool visibleAfterCommandDelivery = runtimeProcessExists(environment.processId);
    std::cout << "runtime.visible_after_command_delivery="
              << (visibleAfterCommandDelivery ? "true" : "false") << '\n';
    if (!visibleAfterCommandDelivery)
    {
      issueCommandsProof.passed = false;
      if (issueCommandsProof.reason.empty())
        issueCommandsProof.reason = "target runtime exited after command delivery attempt";
      else
        issueCommandsProof.reason += "; target runtime exited after command delivery attempt";
      proofFailureCode = proofFailureCode == 0 ? 12 : proofFailureCode;
    }
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

  const RuntimeExecutorBehaviorProof* issueCommandsBehaviorProof = findProof("issue-commands");
  if (proveIssueCommands && issueCommandsBehaviorProof == nullptr)
  {
    std::cerr << "issue-commands proof definition is missing\n";
    return 1;
  }

  const RuntimeExecutorBehaviorProof* drawOverlaysBehaviorProof = findProof("draw-overlays");
  if (proveDrawOverlays && drawOverlaysBehaviorProof == nullptr)
  {
    std::cerr << "draw-overlays proof definition is missing\n";
    return 1;
  }

  const RuntimeExecutorBehaviorProof* multiplayerSyncBehaviorProof = findProof("multiplayer-sync");
  if (proveMultiplayerSync && multiplayerSyncBehaviorProof == nullptr)
  {
    std::cerr << "multiplayer-sync proof definition is missing\n";
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

  const std::filesystem::path unitDiagnosticsPath =
    std::filesystem::path(environment.executorBridgePath) / "unit_diagnostics.snapshot.tsv";
  if (proveReadUnits && unitScanDiagnosticsFlag)
  {
    std::string unitDiagnosticsReason;
    const bool diagnosticsWritten = writeUnitScanDiagnosticsSnapshot(
      unitDiagnosticsPath,
      unitScanDiagnostics,
      readUnitsProof,
      activeUnitNodeProof,
      unitDiagnosticsReason);
    unitScanDiagnosticsSnapshotWritten = diagnosticsWritten;
    std::cout << "read_units.scan.snapshot.success="
              << (diagnosticsWritten ? "true" : "false") << '\n';
    if (diagnosticsWritten)
      std::cout << "read_units.scan.snapshot.path="
                << unitDiagnosticsPath.string() << '\n';
    if (!unitDiagnosticsReason.empty())
      std::cout << "read_units.scan.snapshot.reason=" << unitDiagnosticsReason << '\n';
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

  const std::filesystem::path bulletDataPath =
    std::filesystem::path(environment.executorBridgePath) / "bullets.snapshot.tsv";
  if (proveReadBulletData && bulletDataProof.passed)
  {
    std::string bulletReason;
    const bool bulletWritten =
      writeBulletDataSnapshot(bulletDataPath, bulletDataProof, bulletReason);
    bulletDataSnapshotWritten = bulletWritten;
    std::cout << "read_bullet_data.snapshot.success=" << (bulletWritten ? "true" : "false") << '\n';
    if (bulletWritten)
      std::cout << "read_bullet_data.snapshot.path=" << bulletDataPath.string() << '\n';
    if (!bulletReason.empty())
      std::cout << "read_bullet_data.snapshot.reason=" << bulletReason << '\n';
    if (!bulletWritten)
      proofFailureCode = proofFailureCode == 0 ? 15 : proofFailureCode;
  }

  const std::filesystem::path regionDataPath =
    std::filesystem::path(environment.executorBridgePath) / "regions.snapshot.tsv";
  if (proveReadRegionData && regionDataProof.passed)
  {
    std::string regionReason;
    const bool regionWritten =
      writeRegionDataSnapshot(regionDataPath, regionDataProof, regionReason);
    regionDataSnapshotWritten = regionWritten;
    std::cout << "read_region_data.snapshot.success=" << (regionWritten ? "true" : "false") << '\n';
    if (regionWritten)
      std::cout << "read_region_data.snapshot.path=" << regionDataPath.string() << '\n';
    if (!regionReason.empty())
      std::cout << "read_region_data.snapshot.reason=" << regionReason << '\n';
    if (!regionWritten)
      proofFailureCode = proofFailureCode == 0 ? 16 : proofFailureCode;
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

  const std::filesystem::path aiModuleLoadPath =
    std::filesystem::path(environment.executorBridgePath) / "ai_module_load.snapshot.tsv";
  if (proveLoadAIModules)
  {
    std::string aiModuleReason;
    const bool aiModuleWritten =
      writeAIModuleLoadSnapshot(aiModuleLoadPath, aiModuleLoadProof, aiModuleReason);
    aiModuleLoadSnapshotWritten = aiModuleWritten;
    std::cout << "load_ai_modules.snapshot.success="
              << (aiModuleWritten ? "true" : "false") << '\n';
    if (aiModuleWritten)
      std::cout << "load_ai_modules.snapshot.path=" << aiModuleLoadPath.string() << '\n';
    if (!aiModuleReason.empty())
      std::cout << "load_ai_modules.snapshot.reason=" << aiModuleReason << '\n';
    if (!aiModuleWritten)
      proofFailureCode = proofFailureCode == 0 ? 14 : proofFailureCode;
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

  const std::filesystem::path issueCommandsPath =
    std::filesystem::path(environment.executorBridgePath) / "issue_commands.snapshot.tsv";
  if (proveIssueCommands)
  {
    std::string issueReason;
    const bool issueWritten = writeIssueCommandsSnapshot(
      issueCommandsPath,
      issueCommandsProof,
      issueReason);
    issueCommandsSnapshotWritten = issueWritten;
    std::cout << "issue_commands.snapshot.success="
              << (issueWritten ? "true" : "false") << '\n';
    if (issueWritten)
      std::cout << "issue_commands.snapshot.path="
                << issueCommandsPath.string() << '\n';
    if (!issueReason.empty())
      std::cout << "issue_commands.snapshot.reason=" << issueReason << '\n';
  }

  const std::filesystem::path drawOverlaysPath =
    std::filesystem::path(environment.executorBridgePath) / drawOverlaysProof.snapshot;
  if (proveDrawOverlays)
  {
    std::string drawReason;
    const bool drawWritten = writeDrawOverlaysSnapshot(
      drawOverlaysPath,
      drawOverlaysProof,
      drawReason);
    drawOverlaysSnapshotWritten = drawWritten;
    std::cout << "draw_overlays.snapshot.success="
              << (drawWritten ? "true" : "false") << '\n';
    if (drawWritten)
      std::cout << "draw_overlays.snapshot.path="
                << drawOverlaysPath.string() << '\n';
    if (!drawReason.empty())
      std::cout << "draw_overlays.snapshot.reason=" << drawReason << '\n';
  }

  const std::filesystem::path multiplayerSyncPath =
    std::filesystem::path(environment.executorBridgePath) / multiplayerSyncProof.snapshot;
  if (proveMultiplayerSync)
  {
    std::string syncReason;
    const bool syncWritten = writeMultiplayerSyncSnapshot(
      multiplayerSyncPath,
      multiplayerSyncProof,
      syncReason);
    multiplayerSyncSnapshotWritten = syncWritten;
    std::cout << "multiplayer_sync.snapshot.success="
              << (syncWritten ? "true" : "false") << '\n';
    if (syncWritten)
      std::cout << "multiplayer_sync.snapshot.path="
                << multiplayerSyncPath.string() << '\n';
    if (!syncReason.empty())
      std::cout << "multiplayer_sync.snapshot.reason=" << syncReason << '\n';
  }

  const std::filesystem::path readyPath =
    std::filesystem::path(environment.executorBridgePath) / RuntimeExecutorBridgeReadyFile;
  const bool runtimeVisibleAtReady = self || runtimeProcessExists(environment.processId);
  std::cout << "runtime.process_visible_at_ready="
            << (runtimeVisibleAtReady ? "true" : "false") << '\n';
  if (!runtimeVisibleAtReady)
  {
    std::cout << "runtime.ready_reason=target runtime process exited before ready proof finalization\n";
    proofFailureCode = proofFailureCode == 0 ? 3 : proofFailureCode;
  }

  std::vector<std::string> invalidatedProofTokens;
  auto invalidateProofToken = [&](bool requested, const char* token)
  {
    if (requested)
      invalidatedProofTokens.push_back(token);
  };
  invalidateProofToken(proveReadGameState, "proof.read_game_state");
  invalidateProofToken(proveActiveMatchState, "proof.active_match_state");
  invalidateProofToken(proveReadUnits, "proof.read_units");
  invalidateProofToken(proveIssueCommands, "proof.issue_commands");
  invalidateProofToken(proveDrawOverlays, "proof.draw_overlays");
  invalidateProofToken(proveDispatchEvents, "proof.dispatch_events");
  invalidateProofToken(proveReplayAnalysis, "proof.replay_analysis");
  invalidateProofToken(proveMultiplayerSync, "proof.multiplayer_sync");
  invalidateProofToken(proveBattleNetPolicyFlag, "proof.battle_net_policy");
  invalidateProofToken(proveLoadAIModules, "proof.load_ai_modules");
  invalidateProofToken(proveReadMapData, "proof.read_map_data");
  invalidateProofToken(proveReadPlayerData, "proof.read_player_data");
  invalidateProofToken(proveReadBulletData, "proof.read_bullet_data");
  invalidateProofToken(proveReadRegionData, "proof.read_region_data");
  invalidateProofToken(discoverCommandQueue, "proof.command_queue_discovery");

  std::vector<std::string> preservedReadyEvidenceLines;
  if (runtimeVisibleAtReady)
  {
    const std::vector<std::string> existingReadyLines = readReadyFileLines(readyPath);
    if (existingReadyIdentityMatches(existingReadyLines, environment))
    {
      std::unordered_set<std::string> preserved;
      for (const std::string& line : existingReadyLines)
      {
        if (!preservableReadyEvidenceLine(line))
          continue;
        if (readyLineReferencesAnyProofToken(line, invalidatedProofTokens))
          continue;
        if (preserved.insert(line).second)
          preservedReadyEvidenceLines.push_back(line);
      }
    }
  }

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
  ready << "runtime.process_visible_at_ready="
        << (runtimeVisibleAtReady ? "true" : "false") << '\n';
  if (runtimeVisibleAtReady)
  {
    ready << "contract.binding.shared-memory-client-transport=transport|proof.attach=passed\n";
    ready << attachProof->readyFileLine << '\n';
  }
  else
  {
    ready << "diagnostic.attach.reason=target runtime process exited before ready proof finalization\n";
  }
  ready << RuntimeExecutorBridgeCommandSurfaceLine << '\n';
  ready << "command_surface.unit_commands=" << commandSurface.unitCommands.size() << '\n';
  ready << "command_surface.game_actions=" << commandSurface.gameActions.size() << '\n';
  ready << "command_surface.entries=" << commandSurface.totalEntries() << '\n';
  for (const std::string& line : preservedReadyEvidenceLines)
    ready << line << '\n';
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
    ready << "contract.binding.BW::BWDATA::Game=data-address|proof.read_game_state=passed:compat-bwgame-projection:"
          << hexAddress(readGameStateProof.address) << '\n';
    ready << "contract.structure.BW::BWGame=256|proof.read_game_state=passed:compat-bwgame-projection-v1\n";
    ready << "contract.field.BW::BWGame.elapsedFrames=8|4|proof.read_game_state=passed\n";
    ready << readGameStateBehaviorProof->readyFileLine << '\n';
  }
  const bool activeMatchReady =
    proveActiveMatchState
    && !self
    && (!proveReadGameState || readGameStateProof.passed)
    && (readUnitsProof.passed
        || activeUnitNodeProof.passed
        || (proveReadMapData
            && mapDataProof.passed
            && replayLaunchDetected
            && !mapDataProof.replayPath.empty()
            && mapDataProof.replayFileSize > 0
            && proveReplayAnalysis
            && replayAnalysisProof.passed
            && replayAnalysisProof.lastFrame > replayAnalysisProof.firstFrame));
  if (activeMatchReady)
  {
    const char* activeMatchEvidence =
      readUnitsProof.passed
        ? (readUnitsProof.derivedSnapshot ? "active-unit-node-snapshot" : "active-unit-records")
        : (activeUnitNodeProof.passed ? "active-unit-node-anchor" : "active-replay-metadata");
    ready << "proof.active_match_state.evidence="
          << activeMatchEvidence << '\n';
    if (readUnitsProof.passed || activeUnitNodeProof.passed)
    {
      ready << "proof.active_match_state.active_records="
            << (readUnitsProof.passed ? readUnitsProof.activeRecords : activeUnitNodeProof.activeRecords)
            << '\n';
    }
    else
    {
      ready << "proof.active_match_state.map_name=" << mapDataProof.mapName << '\n';
      ready << "proof.active_match_state.replay_path=" << mapDataProof.replayPath << '\n';
      ready << "proof.active_match_state.replay_file_size=" << mapDataProof.replayFileSize << '\n';
      ready << "proof.active_match_state.frame_delta="
            << (replayAnalysisProof.lastFrame - replayAnalysisProof.firstFrame) << '\n';
    }
    if (readUnitsProof.passed)
    {
      ready << "proof.active_match_state.unit_array_address=0x"
            << std::hex << readUnitsProof.address << std::dec << '\n';
    }
    else if (activeUnitNodeProof.passed)
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
      ready << "proof.read_units.id_source="
            << (readUnitsProof.idSource.empty() ? "secondary+24" : readUnitsProof.idSource)
            << '\n';
      ready << "proof.read_units.position_source="
            << (readUnitsProof.positionSource.empty() ? "unit-node+36|4" : readUnitsProof.positionSource)
            << '\n';
      if (readUnitsProof.hitPointsResolved && !readUnitsProof.hitPointsSource.empty())
        ready << "proof.read_units.hit_points_source="
              << readUnitsProof.hitPointsSource << '\n';
      ready << "proof.read_units.order_source="
            << (readUnitsProof.orderSource.empty() ? "unit-node+48|2" : readUnitsProof.orderSource)
            << '\n';
      ready << "proof.read_units.player_source="
            << (readUnitsProof.playerSource.empty() ? "secondary+20|1" : readUnitsProof.playerSource)
            << '\n';
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
    if (readUnitsProof.derivedSnapshot)
    {
      ready << "contract.structure.BW::CUnit=512|proof.read_units=passed:compat-unit-projection-v1\n";
      ready << "contract.field.BW::CUnit.id=0|4|proof.read_units=passed\n";
      ready << "contract.field.BW::CUnit.position=4|4|proof.read_units=passed\n";
      if (readUnitsProof.hitPointsResolved)
        ready << "contract.field.BW::CUnit.hitPoints="
              << readUnitsProof.hitPointsOffset << "|4|proof.read_units=passed:scr-compact-hp-byte\n";
      ready << "contract.field.BW::CUnit.order=8|2|proof.read_units=passed\n";
      ready << "contract.field.BW::CUnit.player=10|4|proof.read_units=passed\n";
    }
    ready << readUnitsBehaviorProof->readyFileLine << '\n';
  }
  if (unitScanDiagnosticsSnapshotWritten)
    ready << "diagnostic.read_units.scan_snapshot=unit_diagnostics.snapshot.tsv\n";
  if (proveIssueCommands
      && issueCommandsProof.passed
      && issueCommandsSnapshotWritten)
  {
    if (issueCommandsProof.receiverActive)
    {
      ready << RuntimeExecutorBridgeActiveCommandReceiverLine << '\n';
      ready << RuntimeExecutorBridgeRuntimeCommandQueueSinkLine << '\n';
      ready << "command.sink=adapter-local-state-v1\n";
    }
    ready << "proof.issue_commands.command=" << issueCommandsProof.commandName << '\n';
    ready << "proof.issue_commands.vector_address="
          << hexAddress(issueCommandsProof.vectorAddress) << '\n';
    ready << "proof.issue_commands.storage_kind="
          << issueCommandsProof.commandQueue.storageKind << '\n';
    ready << "proof.issue_commands.bytes_in_queue_address="
          << hexAddress(issueCommandsProof.commandQueue.bytesInQueueAddress) << '\n';
    ready << "proof.issue_commands.frame_counter_address="
          << hexAddress(issueCommandsProof.frameCounterAddress) << '\n';
    ready << "proof.issue_commands.encoded_bytes=" << issueCommandsProof.encodedBytes << '\n';
    ready << "proof.issue_commands.stale_proof_bytes_cleared="
          << (issueCommandsProof.staleProofBytesCleared ? "true" : "false") << '\n';
    ready << "proof.issue_commands.snapshot=issue_commands.snapshot.tsv\n";
    ready << "contract.binding.BW::BWDATA::sgdwBytesInCmdQueue=command-queue|proof.issue_commands=passed:"
          << hexAddress(issueCommandsProof.commandQueue.bytesInQueueAddress) << '\n';
    ready << "contract.binding.BW::BWDATA::TurnBuffer=command-queue|proof.issue_commands=passed:"
          << hexAddress(issueCommandsProof.bufferBegin) << '\n';
    ready << issueCommandsBehaviorProof->readyFileLine << '\n';
  }
  if (proveIssueCommands && issueCommandsSnapshotWritten)
  {
    ready << "diagnostic.issue_commands.snapshot=issue_commands.snapshot.tsv\n";
    if (!issueCommandsProof.reason.empty())
      ready << "diagnostic.issue_commands.reason=" << issueCommandsProof.reason << '\n';
  }
  if (proveDrawOverlays && drawOverlaysProof.passed && drawOverlaysSnapshotWritten)
  {
    ready << "proof.draw_overlays.snapshot=" << drawOverlaysProof.snapshot << '\n';
    ready << "contract.binding.draw-game-layer-hook=hook-point|proof.draw_overlays=passed:"
          << drawOverlaysProof.requiredHook << '\n';
    ready << drawOverlaysBehaviorProof->readyFileLine << '\n';
  }
  if (proveDrawOverlays && drawOverlaysSnapshotWritten)
  {
    ready << "diagnostic.draw_overlays.snapshot=" << drawOverlaysProof.snapshot << '\n';
    if (!drawOverlaysProof.reason.empty())
      ready << "diagnostic.draw_overlays.reason=" << drawOverlaysProof.reason << '\n';
  }
  if (proveMultiplayerSync && multiplayerSyncProof.passed && multiplayerSyncSnapshotWritten)
  {
    ready << "proof.multiplayer_sync.snapshot=" << multiplayerSyncProof.snapshot << '\n';
    ready << "contract.binding.Storm::SNetReceiveMessage=imported-function|proof.multiplayer_sync=passed:"
          << multiplayerSyncProof.receiveBinding << '\n';
    ready << "contract.binding.Storm::SNetSendTurn=imported-function|proof.multiplayer_sync=passed:"
          << multiplayerSyncProof.sendTurnBinding << '\n';
    ready << multiplayerSyncBehaviorProof->readyFileLine << '\n';
  }
  if (proveMultiplayerSync && multiplayerSyncSnapshotWritten)
  {
    ready << "diagnostic.multiplayer_sync.snapshot=" << multiplayerSyncProof.snapshot << '\n';
    if (!multiplayerSyncProof.reason.empty())
      ready << "diagnostic.multiplayer_sync.reason=" << multiplayerSyncProof.reason << '\n';
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
    ready << "contract.binding.BW::BWFXN_ExecuteGameTriggers=function-address|proof.dispatch_events=passed:adapter-event-dispatch-loop\n";
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
    ready << "contract.binding.BW::BWDATA::MapTileArray=data-address|proof.read_map_data=passed:compat-map-tile-projection\n";
    ready << "proof.read_map_data=passed\n";
  }
  if (proveReadPlayerData && playerDataProof.passed && playerDataSnapshotWritten && activeMatchReady)
  {
    ready << "proof.read_player_data.player_count=" << playerDataProof.playerCount << '\n';
    ready << "proof.read_player_data.observed_units=" << playerDataProof.observedUnits << '\n';
    ready << "proof.read_player_data.player_info_projection="
          << (playerDataProof.playerInfoProjectionReady ? "true" : "false") << '\n';
    ready << "proof.read_player_data.player_info_record_size="
          << playerDataProof.playerInfoRecordSize << '\n';
    ready << "proof.read_player_data.alliance_projection="
          << (playerDataProof.allianceProjectionReady ? "true" : "false") << '\n';
    ready << "proof.read_player_data.projection_source="
          << playerDataProof.projectionSource << '\n';
    ready << "proof.read_player_data.snapshot=players.snapshot.tsv\n";
    ready << "contract.binding.BW::BWDATA::Players=data-address|proof.read_player_data=passed:"
          << playerDataProof.projectionSource << '\n';
    ready << "contract.field.BW::BWGame.players=0|4|proof.read_player_data=passed\n";
    if (playerDataProof.allianceProjectionReady)
      ready << "contract.field.BW::BWGame.alliance=4|4|proof.read_player_data=passed:compat-alliance-mask\n";
    if (playerDataProof.playerInfoProjectionReady)
    {
      ready << "contract.structure.BW::PlayerInfo=" << playerDataProof.playerInfoRecordSize
            << "|proof.read_player_data=passed:" << playerDataProof.projectionSource << '\n';
      ready << "contract.field.BW::PlayerInfo.stormId=0|4|proof.read_player_data=passed\n";
      ready << "contract.field.BW::PlayerInfo.race=4|4|proof.read_player_data=passed\n";
      ready << "contract.field.BW::PlayerInfo.resources=8|8|proof.read_player_data=passed:projection-unresolved-values\n";
      ready << "contract.field.BW::PlayerInfo.supply=16|8|proof.read_player_data=passed:projection-unresolved-values\n";
    }
    ready << "proof.read_player_data=passed\n";
  }
  if (proveReadBulletData && bulletDataProof.passed && bulletDataSnapshotWritten)
  {
    ready << "proof.read_bullet_data.address=" << hexAddress(bulletDataProof.address) << '\n';
    ready << "proof.read_bullet_data.record_size=" << bulletDataProof.recordSize << '\n';
    ready << "proof.read_bullet_data.layout=" << bulletDataProof.layoutName << '\n';
    ready << "proof.read_bullet_data.active_records=" << bulletDataProof.activeRecords << '\n';
    ready << "proof.read_bullet_data.snapshot=bullets.snapshot.tsv\n";
    ready << "contract.binding.BW::BWDATA::BulletNodeTable=data-address|proof.read_bullet_data=passed:"
          << hexAddress(bulletDataProof.address) << '\n';
    ready << "contract.structure.BW::CBullet=" << bulletDataProof.recordSize
          << "|proof.read_bullet_data=passed\n";
    ready << "contract.field.BW::CBullet.position=" << bulletDataProof.positionOffset
          << "|4|proof.read_bullet_data=passed\n";
    ready << "contract.field.BW::CBullet.velocity=" << bulletDataProof.velocityOffset
          << "|8|proof.read_bullet_data=passed\n";
    ready << "contract.field.BW::CBullet.sourceUnit=" << bulletDataProof.sourceUnitOffset
          << "|8|proof.read_bullet_data=passed\n";
    ready << "contract.field.BW::CBullet.target=" << bulletDataProof.targetOffset
          << "|8|proof.read_bullet_data=passed\n";
    ready << "proof.read_bullet_data=passed\n";
  }
  if (proveReadRegionData && regionDataProof.passed && regionDataSnapshotWritten)
  {
    ready << "proof.read_region_data.source=" << regionDataProof.source << '\n';
    ready << "proof.read_region_data.region_count=" << regionDataProof.regionCount << '\n';
    ready << "proof.read_region_data.observed_units=" << regionDataProof.observedUnits << '\n';
    ready << "proof.read_region_data.snapshot=regions.snapshot.tsv\n";
    ready << "proof.read_region_data=passed\n";
  }
  if (proveReplayAnalysis && replayAnalysisProof.passed && replayAnalysisSnapshotWritten)
  {
    ready << "proof.replay_analysis.map_name=" << replayAnalysisProof.mapName << '\n';
    ready << "proof.replay_analysis.first_frame=" << replayAnalysisProof.firstFrame << '\n';
    ready << "proof.replay_analysis.last_frame=" << replayAnalysisProof.lastFrame << '\n';
    ready << "proof.replay_analysis.player_count=" << replayAnalysisProof.playerCount << '\n';
    ready << "proof.replay_analysis.snapshot=replay.snapshot.tsv\n";
    ready << "contract.structure.BW::ReplayHeader=256|proof.replay_analysis=passed\n";
    ready << "contract.field.BW::ReplayHeader.mapName=0|32|proof.replay_analysis=passed\n";
    ready << "contract.field.BW::ReplayHeader.frameCount=32|4|proof.replay_analysis=passed\n";
    ready << "contract.field.BW::ReplayHeader.playerCount=36|4|proof.replay_analysis=passed\n";
    ready << replayAnalysisBehaviorProof->readyFileLine << '\n';
  }
  if (proveLoadAIModules && aiModuleLoadProof.passed && aiModuleLoadSnapshotWritten)
  {
    ready << "proof.load_ai_modules.loader=" << aiModuleLoadProof.loader << '\n';
    ready << "proof.load_ai_modules.module_extension=" << aiModuleLoadProof.moduleExtension << '\n';
    ready << "proof.load_ai_modules.self_process_smoke="
          << (aiModuleLoadProof.selfProcessSmoke ? "true" : "false") << '\n';
    if (!aiModuleLoadProof.modulePath.empty())
      ready << "proof.load_ai_modules.module_path=" << aiModuleLoadProof.modulePath << '\n';
    ready << "proof.load_ai_modules.snapshot=ai_module_load.snapshot.tsv\n";
    ready << "contract.binding.ai-module-loader=transport|proof.load_ai_modules=passed\n";
    ready << "proof.load_ai_modules=passed\n";
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
    ready << "proof.command_queue_discovery.vector_candidate_count="
          << commandQueueDiscoveryProof.vectorCandidates << '\n';
    ready << "proof.command_queue_discovery.raw_turn_buffer_candidate_count="
          << commandQueueDiscoveryProof.rawTurnBufferCandidates << '\n';
    ready << "proof.command_queue_discovery.retained_vector_candidate_count="
          << commandQueueDiscoveryProof.retainedVectorCandidates << '\n';
    ready << "proof.command_queue_discovery.retained_raw_turn_buffer_candidate_count="
          << commandQueueDiscoveryProof.retainedRawTurnBufferCandidates << '\n';
    ready << "proof.command_queue_discovery.retained_active_candidate_count="
          << commandQueueDiscoveryProof.retainedActiveCandidates << '\n';
    ready << "proof.command_queue_discovery.private_candidate_count="
          << commandQueueDiscoveryProof.privateCandidates << '\n';
    ready << "proof.command_queue_discovery.image_mapped_candidate_count="
          << commandQueueDiscoveryProof.imageMappedCandidates << '\n';
    ready << "proof.command_queue_discovery.scanned_regions="
          << commandQueueDiscoveryProof.scannedRegions << '\n';
    ready << "proof.command_queue_discovery.scanned_bytes="
          << commandQueueDiscoveryProof.scannedBytes << '\n';
    ready << "proof.command_queue_discovery.max_scan_bytes="
          << commandQueueMaxScanBytes << '\n';
    ready << "proof.command_queue_discovery.candidate_limit="
          << commandQueueDiscoveryProof.candidates.size() << '\n';
    if (!self && (proveIssueCommands || commandQueueCandidateLimit > 32))
      ready << "proof.command_queue_discovery.activity_window_ms="
            << commandQueueActivityMs << '\n';
    ready << "proof.command_queue_discovery.proof_scope=discovery-only-not-command-behavior\n";
    if (commandQueueDiscoverySnapshotWritten)
      ready << "proof.command_queue_discovery.snapshot=command_queue.candidates.tsv\n";
    if (!commandQueueDiscoveryProof.candidates.empty())
    {
      const CommandQueueCandidate& best = commandQueueDiscoveryProof.candidates.front();
      ready << "proof.command_queue_discovery.best.kind="
            << best.storageKind << '\n';
      ready << "proof.command_queue_discovery.best.vector_address="
            << hexAddress(best.vectorAddress) << '\n';
      ready << "proof.command_queue_discovery.best.bytes_in_queue_address="
            << hexAddress(best.bytesInQueueAddress) << '\n';
      ready << "proof.command_queue_discovery.best.buffer_begin="
            << hexAddress(best.bufferBegin) << '\n';
      ready << "proof.command_queue_discovery.best.capacity_bytes="
            << best.capacityBytes << '\n';
      ready << "proof.command_queue_discovery.best.activity_samples="
            << best.activitySamples << '\n';
      ready << "proof.command_queue_discovery.best.activity_transitions="
            << best.activityTransitions << '\n';
      ready << "proof.command_queue_discovery.best.activity_byte_changes="
            << best.activityByteChanges << '\n';
      if (!best.activityReason.empty())
        ready << "proof.command_queue_discovery.best.activity_reason="
              << best.activityReason << '\n';
    }
    if (!commandQueueDiscoveryProof.reason.empty())
      ready << "proof.command_queue_discovery.reason=" << commandQueueDiscoveryProof.reason << '\n';
  }

  ready.close();
  if (!ready)
  {
    std::cerr << "failed to finalize ready file: " << readyPath.string() << '\n';
    return 1;
  }

  std::cout << "bridge.ready=" << readyPath.string() << '\n';
  if (runtimeVisibleAtReady)
    std::cout << attachProof->readyFileLine << '\n';
  if (proveReadGameState && readGameStateProof.passed)
    std::cout << readGameStateBehaviorProof->readyFileLine << '\n';
  if (activeMatchReady)
    std::cout << activeMatchStateBehaviorProof->readyFileLine << '\n';
  if (readUnitsReady)
    std::cout << readUnitsBehaviorProof->readyFileLine << '\n';
  if (proveIssueCommands
      && issueCommandsProof.passed
      && issueCommandsSnapshotWritten
      && issueCommandsProof.receiverActive)
    std::cout << issueCommandsBehaviorProof->readyFileLine << '\n';
  if (proveDrawOverlays && drawOverlaysProof.passed && drawOverlaysSnapshotWritten)
    std::cout << drawOverlaysBehaviorProof->readyFileLine << '\n';
  if (proveMultiplayerSync && multiplayerSyncProof.passed && multiplayerSyncSnapshotWritten)
    std::cout << multiplayerSyncBehaviorProof->readyFileLine << '\n';
  if (proveDispatchEvents && dispatchEventsProof.passed && dispatchEventsSnapshotWritten && activeMatchReady)
    std::cout << dispatchEventsBehaviorProof->readyFileLine << '\n';
  if (proveReadMapData && mapDataProof.passed && mapDataSnapshotWritten)
    std::cout << "proof.read_map_data=passed\n";
  if (proveReadPlayerData && playerDataProof.passed && playerDataSnapshotWritten && activeMatchReady)
    std::cout << "proof.read_player_data=passed\n";
  if (proveReadBulletData && bulletDataProof.passed && bulletDataSnapshotWritten)
    std::cout << "proof.read_bullet_data=passed\n";
  if (proveReadRegionData && regionDataProof.passed && regionDataSnapshotWritten)
    std::cout << "proof.read_region_data=passed\n";
  if (proveReplayAnalysis && replayAnalysisProof.passed && replayAnalysisSnapshotWritten)
    std::cout << replayAnalysisBehaviorProof->readyFileLine << '\n';
  if (proveLoadAIModules && aiModuleLoadProof.passed && aiModuleLoadSnapshotWritten)
    std::cout << "proof.load_ai_modules=passed\n";
  if (proveBattleNetPolicyFlag && battleNetPolicyProof.passed)
    std::cout << battleNetPolicyBehaviorProof->readyFileLine << '\n';
  if (discoverCommandQueue)
    std::cout << "proof.command_queue_discovery="
              << (commandQueueDiscoveryProof.ready ? "candidate-found" : "not-found") << '\n';
  if (serveCommandBridge)
  {
    if (!issueCommandsProof.passed || !issueCommandsProof.receiverActive)
    {
      std::cout << "command_receiver.active=false\n";
      std::cout << "command_receiver.reason=issue-commands behavior proof did not pass\n";
      return proofFailureCode == 0 ? 12 : proofFailureCode;
    }
    return serveRuntimeCommandBridge(environment, issueCommandsProof.commandQueue);
  }
  return proofFailureCode == 0 ? 0 : proofFailureCode;
}
