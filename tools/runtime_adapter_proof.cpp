#include <BWAPI/Runtime/RuntimeBackend.h>
#include <BWAPI/Runtime/RuntimeCommandEncoder.h>
#include <BWAPI/Runtime/RuntimeCommandSurface.h>
#include <BWAPI/Runtime/RuntimeExecutor.h>
#include <BWAPI/Runtime/RuntimeInstallation.h>
#include <BWAPI/Runtime/RuntimeProcess.h>
#include <BWAPI/Runtime/RuntimeProcessMemory.h>
#include <BWAPI/Runtime/RuntimeResidentBridge.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
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
      << "                           prove active match/replay state; requires resident read-game-state proof\n"
      << "  --prove-read-units       prove live unit reads by finding a BWAPI-compatible CUnit array\n"
      << "  --self-unit-fixture      allocate a self-test CUnit array before --prove-read-units\n"
      << "  --self-unit-node-fixture allocate a self-test SC:R unit-node graph before --prove-read-units\n"
      << "  --self-compact-unit-node-fixture\n"
      << "                           allocate a non-contiguous self-test SC:R compact unit-node graph\n"
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
      << "  --command-queue-buffer-address <address>\n"
      << "                           explicit raw turn-buffer start paired with --command-queue-vector-address byte count\n"
      << "  --append-game-action <name>\n"
      << "                           append one encoded game action to the explicit command queue vector, then exit\n"
      << "  --state-sample-delay-ms <ms>\n"
      << "                           delay between live state samples (default: 250)\n"
      << "  --state-counter-address <address>\n"
      << "                           validate an explicit live frame counter address before broad scans\n"
      << "  --state-scan-timeout-ms <ms>\n"
      << "                           maximum time for --prove-read-game-state scan (default: 30000)\n"
      << "  --state-max-scan-mb <mb> maximum readable writable memory to sample (default: 256)\n"
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

  std::uint64_t steadyTickMilliseconds()
  {
    return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
  }

  struct LiveCounterProof
  {
    bool passed = false;
    std::uintptr_t address = 0;
    std::uint32_t first = 0;
    std::uint32_t second = 0;
    std::uint32_t third = 0;
    std::string reason;
    std::uint64_t firstTick = 0;
    std::uint64_t secondTick = 0;
    std::uint64_t thirdTick = 0;
  };

  bool hasResidentGameStateProofTicks(const LiveCounterProof& proof)
  {
    return proof.passed
      && proof.firstTick > 0
      && proof.secondTick > proof.firstTick
      && proof.thirdTick > proof.secondTick;
  }

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
    bool taggedHandleDerived = false;
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
    bool staticAdapterAnchorsResolved = false;
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
    std::vector<std::string> resolvedAnchors;
    std::vector<std::string> missingAnchors;
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
    bool currentProcessReplay = false;
    bool activeMatchMetadata = false;
    std::string source;
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
    std::string discoverySource = "memory-scan";
    std::string storageKind = "vector";
    std::uintptr_t vectorAddress = 0;
    std::uintptr_t bytesInQueueAddress = 0;
    std::uintptr_t bufferBegin = 0;
    std::uintptr_t bufferEnd = 0;
    std::uintptr_t bufferCapacity = 0;
    std::size_t usedBytes = 0;
    std::size_t capacityBytes = 0;
    std::size_t counterOffset = 0;
    int score = 0;
    std::size_t activitySamples = 0;
    std::size_t activityTransitions = 0;
    std::size_t activityByteChanges = 0;
    std::size_t activityChangedByteTotal = 0;
    std::size_t activityLastChangeFirstOffset = 0;
    std::size_t activityLastChangeLastOffset = 0;
    std::size_t activityFirstUsedBytes = 0;
    std::size_t activityLastUsedBytes = 0;
    std::size_t activityMinUsedBytes = 0;
    std::size_t activityMaxUsedBytes = 0;
    std::uintptr_t activityFirstBufferEnd = 0;
    std::uintptr_t activityLastBufferEnd = 0;
    std::string activitySelectorFirstHex;
    std::string activitySelectorLastHex;
    std::string activityBufferFirstHex;
    std::string activityBufferLastHex;
    std::size_t prefixBytes = 0;
    std::size_t prefixNonZeroBytes = 0;
    std::size_t prefixDistinctBytes = 0;
    std::size_t prefixKnownOpcodeBytes = 0;
    std::size_t prefixPointerWords = 0;
    std::size_t prefixSmallIntegerWords = 0;
    std::uint32_t prefixEntropyMilli = 0;
    std::string prefixHex;
    std::string firstByteHex;
    std::string regionClass;
    std::string regionPath;
    std::string bufferRegionClass;
    std::string bufferRegionPath;
    std::string activityReason;
    std::string codeReferenceAnchor;
    std::string codeReferenceKind;
    std::uintptr_t codeReferenceAddress = 0;
    std::uintptr_t codeReferenceTarget = 0;
    std::string codeReferenceBytes;
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
    std::size_t retainedTransitionCandidates = 0;
    std::size_t retainedRawByteChangeOnlyCandidates = 0;
    std::size_t retainedBoundedTransitionCandidates = 0;
    std::size_t implicitWriteEligibleCandidates = 0;
    std::size_t liveCodeReferenceCount = 0;
    std::size_t liveCodeReferenceCandidateCount = 0;
    std::size_t liveCodeReferenceRejectedCount = 0;
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

  struct LiveAnchorDiagnostic
  {
    std::string anchor;
    std::uintptr_t stringAddress = 0;
    std::uintptr_t xrefAddress = 0;
    std::uintptr_t estimatedInstructionAddress = 0;
    std::string estimatedInstructionBytes;
    std::vector<std::uintptr_t> nearbyCallTargets;
    struct CodeEvent
    {
      std::uintptr_t address = 0;
      std::string kind;
      std::uintptr_t target = 0;
      bool targetInExecutable = false;
      std::string targetRegionClass;
      std::string targetMappedPath;
      std::string bytes;
    };
    std::vector<CodeEvent> nearbyCodeEvents;
    std::string reason;
  };

  struct LiveCallableDiagnostics
  {
    bool attempted = false;
    bool regionListAvailable = false;
    std::uintptr_t imageBase = 0;
    std::uintptr_t imageSlide = 0;
    std::vector<LiveAnchorDiagnostic> anchors;
    std::string reason;
  };

  struct LiveCallableEntryCandidate
  {
    std::string anchor;
    std::string source;
    std::uintptr_t address = 0;
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
    std::size_t frameCounterCandidateCount = 0;
    std::uint32_t baselineStart = 0;
    std::uint32_t baselineEnd = 0;
    std::uint32_t pausedStart = 0;
    std::uint32_t pausedEnd = 0;
    std::uint32_t resumedStart = 0;
    std::uint32_t resumedEnd = 0;
    std::string commandName;
    std::string encodedBytes;
    std::vector<IssueCommandsAttempt> attempts;
    LiveCallableDiagnostics liveDiagnostics;
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
    LiveCallableDiagnostics liveDiagnostics;
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
    LiveCallableDiagnostics liveDiagnostics;
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
    std::string kind = "unit-node-0x58";
    std::string rejectionReason;
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
    std::size_t nodePrefixNonZeroBytes = 0;
    std::size_t spritePrefixNonZeroBytes = 0;
    std::size_t secondaryPrefixNonZeroBytes = 0;
    std::string nodePrefixHex;
    std::string spritePrefixHex;
    std::string secondaryPrefixHex;
  };

  struct UnitScanRegionDiagnostic
  {
    std::string stage;
    std::string decision;
    std::string reason;
    std::uintptr_t address = 0;
    std::size_t size = 0;
    std::size_t bytesRead = 0;
    int priority = 0;
    bool readable = false;
    bool writable = false;
    bool executable = false;
    bool targetExecutable = false;
    int userTag = -1;
    int shareMode = -1;
    std::string mappedPath;
  };

  struct UnitDynamicRegionDiagnostic
  {
    std::uintptr_t address = 0;
    std::size_t size = 0;
    std::size_t bytesRead = 0;
    std::size_t changedBytes = 0;
    std::size_t changedRanges = 0;
    std::uintptr_t firstChangedAddress = 0;
    std::size_t firstChangedSize = 0;
    int priority = 0;
    bool targetExecutable = false;
    int userTag = -1;
    int shareMode = -1;
    std::string mappedPath;
  };

  struct UnitDynamicFieldCandidateDiagnostic
  {
    std::uintptr_t address = 0;
    std::size_t windowSize = 0;
    std::size_t changedBytes = 0;
    std::size_t readablePointerWords = 0;
    std::size_t taggedHandleWords = 0;
    std::size_t coordinateOffset = 0;
    std::int16_t x = 0;
    std::int16_t y = 0;
    std::size_t hitPointsOffset = 0;
    std::uint32_t hitPoints = 0;
    std::size_t playerOffset = 0;
    int player = -1;
    std::size_t typeOffset = 0;
    std::uint16_t typeHint = 0;
    std::string prefixHex;
  };

  struct UnitPointerArrayCandidateDiagnostic
  {
    std::uintptr_t vectorAddress = 0;
    std::uintptr_t begin = 0;
    std::uintptr_t end = 0;
    std::uintptr_t capacity = 0;
    std::uintptr_t firstPointer = 0;
    std::uintptr_t secondPointer = 0;
    std::size_t usedBytes = 0;
    std::size_t pointerCount = 0;
    std::size_t readablePointers = 0;
    std::size_t recordSnapshots = 0;
    std::size_t firstRecordNonZeroBytes = 0;
    std::size_t firstRecordPointerWords = 0;
    std::string firstRecordPrefixHex;
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

  struct SgUnitsMemDiagnostic
  {
    bool attempted = false;
    bool descriptorRead = false;
    std::string descriptorReadReason;
    std::uintptr_t descriptorAddress = 0;
    std::uintptr_t nativeBase = 0;
    std::uint64_t recordCount = 0;
    std::uint64_t capacity = 0;
    bool regionFound = false;
    std::uintptr_t regionAddress = 0;
    std::size_t regionSize = 0;
    bool regionReadable = false;
    bool regionWritable = false;
    bool regionExecutable = false;
    bool regionTargetExecutable = false;
    int regionUserTag = -1;
    int regionShareMode = -1;
    std::string regionMappedPath;
    bool usableStorage = false;
    std::string rejectionReason;
    std::size_t prefixBytesRead = 0;
    std::size_t prefixNonZeroBytes = 0;
    std::size_t prefixDistinctBytes = 0;
    std::size_t prefixPointerWords = 0;
    std::string prefixHex;
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
    std::vector<UnitPointerArrayCandidateDiagnostic> pointerArraySamples;
    std::vector<UnitScanRegionDiagnostic> regionSamples;
    std::size_t dynamicSampledRegions = 0;
    std::size_t dynamicSampledBytes = 0;
    std::size_t dynamicChangedRegions = 0;
    std::size_t dynamicChangedBytes = 0;
    std::size_t dynamicWindowsScored = 0;
    std::string dynamicScanReason;
    std::vector<UnitDynamicRegionDiagnostic> dynamicRegionSamples;
    std::vector<UnitDynamicFieldCandidateDiagnostic> dynamicFieldCandidates;
    SgUnitsMemDiagnostic sgUnitsMem;
    bool timedOut = false;
    bool byteLimitReached = false;
  };

  struct StateScanDiagnostics
  {
    std::size_t readableCandidateRegions = 0;
    std::size_t readableWritableFileBackedRegions = 0;
    std::size_t readableWritableRegions = 0;
    std::size_t skippedFileBackedReadOnlyRegions = 0;
    std::size_t skippedFileBackedNonTargetRegions = 0;
    std::size_t skippedExecutableRegions = 0;
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

  constexpr std::array<std::size_t, 9> candidateUnitRecordSizes = {
    336, 384, 416, 432, 448, 480, 512, 672, 768
  };

  constexpr std::size_t minActiveUnitRecords = 4;
  constexpr std::size_t minRemasteredSnapshotUnitRecords = 3;
  constexpr std::size_t minActiveBulletRecords = 1;

  bool plausibleRemasteredUnitTypeHint(std::uint16_t typeHint)
  {
    // SC:R unit-node side objects expose a wider internal type/sprite hint
    // than the classic 8-bit CUnit id field. Keep the bound tight enough to
    // reject pointer fragments and flags, but do not reject live units whose
    // internal hint is above 255.
    return typeHint != 0 && typeHint < 1024;
  }

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

  bool parseUnsignedStrict(const std::string& value, std::uint64_t& output)
  {
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value.c_str(), &end, 0);
    if (end == value.c_str() || *end != '\0')
      return false;
    output = static_cast<std::uint64_t>(parsed);
    return static_cast<unsigned long long>(output) == parsed;
  }

  std::string readyValue(const std::vector<std::string>& lines, const std::string& key);

  struct BwGameProjectionProof
  {
    bool passed = false;
    std::uintptr_t address = 0;
    std::size_t size = 0;
    std::size_t elapsedFramesOffset = 0;
    std::uint32_t firstFrame = 0;
    std::uint32_t secondFrame = 0;
    std::string reason;
  };

  BwGameProjectionProof validateResidentBwGameProjection(
    int processId,
    const std::vector<std::string>& readyLines)
  {
    BwGameProjectionProof proof;
    if (readyValue(readyLines, "resident.projection.bwgame.validation")
        != "resident-bwgame-projection-v1")
    {
      proof.reason = "resident BWGame projection metadata is absent";
      return proof;
    }

    if (!parseAddress(readyValue(readyLines, "resident.projection.bwgame.address"), proof.address))
    {
      proof.reason = "resident BWGame projection address is missing or malformed";
      return proof;
    }

    std::uint64_t parsed = 0;
    if (!parseUnsignedStrict(readyValue(readyLines, "resident.projection.bwgame.size"), parsed)
        || parsed < 12
        || parsed > 65536)
    {
      proof.reason = "resident BWGame projection size is missing or implausible";
      return proof;
    }
    proof.size = static_cast<std::size_t>(parsed);

    if (!parseUnsignedStrict(
          readyValue(readyLines, "resident.projection.bwgame.elapsedFrames_offset"),
          parsed)
        || parsed + sizeof(std::uint32_t) > proof.size)
    {
      proof.reason = "resident BWGame projection elapsedFrames offset is missing or out of range";
      return proof;
    }
    proof.elapsedFramesOffset = static_cast<std::size_t>(parsed);

    if (readyValue(readyLines, "resident.projection.bwgame.elapsedFrames_bytes") != "4")
    {
      proof.reason = "resident BWGame projection elapsedFrames field is not a 32-bit counter";
      return proof;
    }

    RuntimeMemoryReadResult firstRead =
      readProcessMemory(
        processId,
        proof.address + proof.elapsedFramesOffset,
        sizeof(std::uint32_t));
    if (!firstRead.success || firstRead.bytesRead != sizeof(std::uint32_t))
    {
      proof.reason = firstRead.reason.empty()
        ? "unable to read resident BWGame projection first elapsedFrames sample"
        : firstRead.reason;
      return proof;
    }
    proof.firstFrame = readU32(firstRead.bytes, 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    RuntimeMemoryReadResult secondRead =
      readProcessMemory(
        processId,
        proof.address + proof.elapsedFramesOffset,
        sizeof(std::uint32_t));
    if (!secondRead.success || secondRead.bytesRead != sizeof(std::uint32_t))
    {
      proof.reason = secondRead.reason.empty()
        ? "unable to read resident BWGame projection second elapsedFrames sample"
        : secondRead.reason;
      return proof;
    }
    proof.secondFrame = readU32(secondRead.bytes, 0);
    if (proof.firstFrame == 0 || proof.secondFrame <= proof.firstFrame)
    {
      proof.reason = "resident BWGame projection elapsedFrames did not advance during live re-sampling";
      return proof;
    }
    if (proof.secondFrame - proof.firstFrame > 10000)
    {
      proof.reason = "resident BWGame projection elapsedFrames advanced implausibly";
      return proof;
    }

    proof.passed = true;
    return proof;
  }

  std::size_t firstAlignedOffset(std::uintptr_t baseAddress, std::size_t alignment)
  {
    if (alignment == 0)
      return 0;
    const std::size_t remainder = static_cast<std::size_t>(baseAddress % alignment);
    return remainder == 0 ? 0 : alignment - remainder;
  }

  std::string bytesHexPrefixAt(
    const std::vector<unsigned char>& bytes,
    std::size_t offset,
    std::size_t maxBytes)
  {
    if (offset >= bytes.size())
      return {};

    std::ostringstream output;
    const std::size_t limit = std::min(maxBytes, bytes.size() - offset);
    for (std::size_t i = 0; i < limit; ++i)
    {
      if (i > 0)
        output << ' ';
      output << std::hex << std::setfill('0') << std::setw(2)
             << static_cast<unsigned int>(bytes[offset + i]);
    }
    return output.str();
  }

  std::size_t countNonZeroBytesAt(
    const std::vector<unsigned char>& bytes,
    std::size_t offset,
    std::size_t maxBytes)
  {
    if (offset >= bytes.size())
      return 0;

    const std::size_t limit = std::min(maxBytes, bytes.size() - offset);
    return static_cast<std::size_t>(std::count_if(
      bytes.begin() + static_cast<std::ptrdiff_t>(offset),
      bytes.begin() + static_cast<std::ptrdiff_t>(offset + limit),
      [](unsigned char byte)
      {
        return byte != 0;
      }));
  }

  std::string hexAddress(std::uintptr_t address)
  {
    std::ostringstream output;
    output << "0x" << std::hex << address;
    return output.str();
  }

  std::string byteHex(unsigned char byte)
  {
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(2)
           << static_cast<unsigned int>(byte);
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

  constexpr std::uint64_t remasteredTaggedNullHandle = 0xffffffff00000001ULL;

  bool plausibleRemasteredTaggedHandleValue(std::uint64_t value)
  {
    if (value == 0 || value == remasteredTaggedNullHandle)
      return false;

    const std::uint32_t low = static_cast<std::uint32_t>(value & 0xffffffffu);
    const std::uint32_t high = static_cast<std::uint32_t>(value >> 32);
    if (high == 0 || high == 0xffffffffu || high > 0xffffu)
      return false;

    // Live SC:R compact unit-related tables use packed 64-bit handles whose
    // low dword is a small tag instead of a process pointer.
    return low == 0x8u || low == 0x200u;
  }

  bool plausibleRemasteredCompactObjectValue(std::uint64_t value)
  {
    return plausibleRuntimeObjectPointerValue(value)
      || plausibleRemasteredTaggedHandleValue(value);
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

  int initialFrameCounterCandidateScore(
    std::uint32_t first,
    std::uint32_t second,
    int regionPriority,
    int sampleDelayMs)
  {
    const int delta = static_cast<int>(second - first);
    const int expectedDelta = std::max(1, (sampleDelayMs * 24) / 1000);
    const int minimumFrameLikeDelta = std::max(2, expectedDelta / 3);
    const int maximumFrameLikeDelta = std::max(12, expectedDelta * 4);
    const bool frameLike = delta >= minimumFrameLikeDelta && delta <= maximumFrameLikeDelta;
    return (regionPriority * 1000000)
      + (frameLike ? 0 : 100000)
      + std::abs(delta - expectedDelta);
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
    return region != nullptr
      && region->writable
      && !region->executable
      && region->mappedPath.empty();
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

    static std::unordered_map<std::string, std::string> cache;
    const auto existing = cache.find(path);
    if (existing != cache.end())
      return existing->second;

    std::error_code error;
    std::filesystem::path normalized = std::filesystem::weakly_canonical(path, error);
    if (error)
      normalized = std::filesystem::absolute(path, error);
    if (error)
      normalized = path;
    std::string result = normalized.lexically_normal().string();
    cache.emplace(path, result);
    return result;
  }

  bool sameMappedFile(const std::string& lhs, const std::string& rhs)
  {
    if (lhs.empty() || rhs.empty())
      return false;
    return normalizedPathForCompare(lhs) == normalizedPathForCompare(rhs);
  }

  std::string regionShareModeName(int shareMode)
  {
    switch (shareMode)
    {
      case 1:
        return "cow";
      case 2:
        return "private";
      case 3:
        return "empty";
      case 4:
        return "shared";
      case 5:
        return "true-shared";
      case 6:
        return "private-aliased";
      case 7:
        return "shared-aliased";
      case 8:
        return "large-page";
      default:
        return "unknown";
    }
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
    for (const char* volatileProof : {
           "proof.active_match_state",
           "proof.read_bullet_data",
           "proof.issue_commands",
           "proof.draw_overlays",
           "proof.multiplayer_sync" })
    {
      if (line.find(volatileProof) != std::string::npos)
        return false;
    }
    if ((startsWith(line, "contract.binding.")
         || startsWith(line, "contract.structure.")
         || startsWith(line, "contract.field."))
        && line.find("|proof.") == std::string::npos)
    {
      return false;
    }
    return startsWith(line, "proof.")
      || startsWith(line, "contract.binding.")
      || startsWith(line, "contract.structure.")
      || startsWith(line, "contract.field.");
  }

  bool readyWasWrittenByResidentAdapter(const std::vector<std::string>& lines)
  {
    return readyValue(lines, "executor") == "starcraft-api-resident-adapter"
      && readyValue(lines, "resident.adapter") == "active";
  }

  bool preservableResidentEvidenceLine(const std::string& line)
  {
    return startsWith(line, "resident.adapter")
      || startsWith(line, "resident.queue.")
      || startsWith(line, "resident.projection.");
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

  bool eligibleStateCounterScanRegion(
    const RuntimeMemoryRegion& region,
    const std::string& executablePath,
    StateScanDiagnostics* diagnostics = nullptr)
  {
    if (!region.readable)
    {
      if (diagnostics != nullptr)
        ++diagnostics->skippedNonReadableRegions;
      return false;
    }
    if (region.executable)
    {
      if (diagnostics != nullptr)
        ++diagnostics->skippedExecutableRegions;
      return false;
    }

    const bool fileBackedNonTarget = fileBackedNonTargetRegion(region, executablePath);
    if (fileBackedNonTarget)
    {
      if (diagnostics != nullptr)
      {
        if (!region.writable)
          ++diagnostics->skippedFileBackedReadOnlyRegions;
        ++diagnostics->skippedFileBackedNonTargetRegions;
      }
      return false;
    }

    if (diagnostics != nullptr)
    {
      ++diagnostics->readableCandidateRegions;
      if (region.writable)
        ++diagnostics->readableWritableRegions;
    }
    return true;
  }

  bool stateCounterAddressAllowed(
    int processId,
    std::uintptr_t address,
    const std::string& executablePath,
    std::string& reason)
  {
    RuntimeMemoryRegionListResult regions = listProcessMemoryRegions(processId);
    if (!regions.success)
    {
      reason = regions.reason;
      return false;
    }

    for (const RuntimeMemoryRegion& region : regions.regions)
    {
      if (!regionContains(region, address, sizeof(std::uint32_t)))
        continue;
      if (!region.readable)
      {
        reason = "explicit frame counter address is not in a readable region";
        return false;
      }
      if (region.executable)
      {
        reason = "explicit frame counter address is in an executable region";
        return false;
      }
      if (fileBackedNonTargetRegion(region, executablePath))
      {
        reason =
          "explicit frame counter address is in non-StarCraft file-backed mapping: "
          + region.mappedPath;
        return false;
      }
      return true;
    }

    reason = "explicit frame counter address is not inside a known readable memory region";
    return false;
  }

  std::uintptr_t findTargetImageBase(
    const std::vector<RuntimeMemoryRegion>& regions,
    const std::string& executablePath)
  {
    std::uintptr_t imageBase = 0;
    for (const RuntimeMemoryRegion& region : regions)
    {
      if (!sameMappedFile(region.mappedPath, executablePath))
        continue;
      if (imageBase == 0 || region.address < imageBase)
        imageBase = region.address;
    }
    return imageBase;
  }

  bool addSignedOffset(std::uintptr_t base, std::int32_t offset, std::uintptr_t& output)
  {
    if (offset >= 0)
    {
      const std::uintptr_t positive = static_cast<std::uintptr_t>(offset);
      if (base > std::numeric_limits<std::uintptr_t>::max() - positive)
        return false;
      output = base + positive;
      return true;
    }

    const std::uintptr_t negative = static_cast<std::uintptr_t>(
      static_cast<std::uint32_t>(-static_cast<std::int64_t>(offset)));
    if (base < negative)
      return false;
    output = base - negative;
    return true;
  }

  bool targetExecutableAddress(
    const std::vector<RuntimeMemoryRegion>& regions,
    const std::string& executablePath,
    std::uintptr_t address)
  {
    const RuntimeMemoryRegion* region = findReadableRegion(regions, address, 1);
    return region != nullptr
      && region->executable
      && sameMappedFile(region->mappedPath, executablePath);
  }

  std::vector<std::uintptr_t> findLiveAnchorStringAddresses(
    int processId,
    const std::vector<RuntimeMemoryRegion>& regions,
    const std::string& executablePath,
    const std::string& anchor,
    std::size_t maxMatches)
  {
    std::vector<std::uintptr_t> matches;
    if (anchor.empty() || maxMatches == 0)
      return matches;

    const std::vector<unsigned char> needle(anchor.begin(), anchor.end());
    constexpr std::size_t stepBytes = 1024 * 1024;
    const std::size_t overlapBytes = std::max<std::size_t>(needle.size(), 1) - 1;

    for (const RuntimeMemoryRegion& region : regions)
    {
      if (!region.readable || !sameMappedFile(region.mappedPath, executablePath))
        continue;

      for (std::size_t regionOffset = 0; regionOffset < region.size; regionOffset += stepBytes)
      {
        const std::uintptr_t chunkAddress = region.address + regionOffset;
        const std::size_t remaining = region.size - regionOffset;
        const std::size_t bytesToRead = std::min(remaining, stepBytes + overlapBytes);
        RuntimeMemoryReadResult read = readProcessMemory(processId, chunkAddress, bytesToRead);
        if (!read.success || read.bytesRead < needle.size())
          continue;

        for (std::size_t offset = 0; offset + needle.size() <= read.bytesRead; ++offset)
        {
          if (!std::equal(needle.begin(), needle.end(), read.bytes.begin() + static_cast<std::ptrdiff_t>(offset)))
            continue;

          const std::uintptr_t address = chunkAddress + offset;
          if (std::find(matches.begin(), matches.end(), address) == matches.end())
          {
            matches.push_back(address);
            if (matches.size() >= maxMatches)
              return matches;
          }
        }
      }
    }

    return matches;
  }

  std::vector<std::uintptr_t> findRipRelativeXrefsToAddress(
    int processId,
    const std::vector<RuntimeMemoryRegion>& regions,
    const std::string& executablePath,
    std::uintptr_t targetAddress,
    std::size_t maxMatches)
  {
    std::vector<std::uintptr_t> matches;
    if (targetAddress == 0 || maxMatches == 0)
      return matches;

    constexpr std::size_t stepBytes = 1024 * 1024;
    constexpr std::size_t overlapBytes = 4;

    for (const RuntimeMemoryRegion& region : regions)
    {
      if (!region.readable
          || !region.executable
          || !sameMappedFile(region.mappedPath, executablePath))
        continue;

      for (std::size_t regionOffset = 0; regionOffset < region.size; regionOffset += stepBytes)
      {
        const std::uintptr_t chunkAddress = region.address + regionOffset;
        const std::size_t remaining = region.size - regionOffset;
        const std::size_t bytesToRead = std::min(remaining, stepBytes + overlapBytes);
        RuntimeMemoryReadResult read = readProcessMemory(processId, chunkAddress, bytesToRead);
        if (!read.success || read.bytesRead < sizeof(std::int32_t))
          continue;

        for (std::size_t offset = 0; offset + sizeof(std::int32_t) <= read.bytesRead; ++offset)
        {
          std::uintptr_t resolved = 0;
          if (!addSignedOffset(
                chunkAddress + offset + sizeof(std::int32_t),
                readS32(read.bytes, offset),
                resolved))
          {
            continue;
          }
          if (resolved != targetAddress)
            continue;

          const std::uintptr_t displacementAddress = chunkAddress + offset;
          if (std::find(matches.begin(), matches.end(), displacementAddress) == matches.end())
          {
            matches.push_back(displacementAddress);
            if (matches.size() >= maxMatches)
              return matches;
          }
        }
      }
    }

    return matches;
  }

  std::vector<std::uintptr_t> collectNearbyCallTargets(
    int processId,
    const std::vector<RuntimeMemoryRegion>& regions,
    const std::string& executablePath,
    std::uintptr_t xrefAddress,
    std::size_t maxTargets)
  {
    std::vector<std::uintptr_t> targets;
    if (xrefAddress == 0 || maxTargets == 0)
      return targets;

    const RuntimeMemoryRegion* region = findReadableRegion(regions, xrefAddress, 1);
    if (region == nullptr || !region->executable)
      return targets;

    constexpr std::uintptr_t radius = 384;
    const std::uintptr_t regionEnd = region->address + region->size;
    const std::uintptr_t begin =
      xrefAddress > region->address + radius ? xrefAddress - radius : region->address;
    const std::uintptr_t end =
      std::min(regionEnd, xrefAddress > std::numeric_limits<std::uintptr_t>::max() - radius
        ? regionEnd
        : xrefAddress + radius);
    if (end <= begin)
      return targets;

    RuntimeMemoryReadResult read = readProcessMemory(
      processId,
      begin,
      static_cast<std::size_t>(end - begin));
    if (!read.success || read.bytesRead < 5)
      return targets;

    for (std::size_t offset = 0; offset + 5 <= read.bytesRead; ++offset)
    {
      const unsigned char opcode = read.bytes[offset];
      if (opcode != 0xe8 && opcode != 0xe9)
        continue;

      std::uintptr_t target = 0;
      if (!addSignedOffset(begin + offset + 5, readS32(read.bytes, offset + 1), target))
        continue;
      if (!targetExecutableAddress(regions, executablePath, target))
        continue;
      if (std::find(targets.begin(), targets.end(), target) != targets.end())
        continue;

      targets.push_back(target);
      if (targets.size() >= maxTargets)
        break;
    }

    return targets;
  }

  std::string joinHexAddresses(const std::vector<std::uintptr_t>& addresses, const char* separator)
  {
    std::ostringstream output;
    for (std::size_t i = 0; i < addresses.size(); ++i)
    {
      if (i > 0)
        output << separator;
      output << hexAddress(addresses[i]);
    }
    return output.str();
  }

  std::string bytesHexSpan(
    const std::vector<unsigned char>& bytes,
    std::size_t offset,
    std::size_t size)
  {
    std::ostringstream output;
    const std::size_t end = std::min(bytes.size(), offset + size);
    for (std::size_t i = offset; i < end; ++i)
    {
      if (i > offset)
        output << ' ';
      output << byteHex(bytes[i]);
    }
    return output.str();
  }

  std::uintptr_t staticVmAddress(std::uintptr_t liveAddress, std::uintptr_t imageSlide)
  {
    if (liveAddress == 0 || imageSlide == 0 || liveAddress < imageSlide)
      return 0;
    return liveAddress - imageSlide;
  }

  std::string joinStaticVmAddresses(
    const std::vector<std::uintptr_t>& liveAddresses,
    std::uintptr_t imageSlide,
    const char* separator)
  {
    std::vector<std::uintptr_t> staticAddresses;
    staticAddresses.reserve(liveAddresses.size());
    for (std::uintptr_t liveAddress : liveAddresses)
    {
      const std::uintptr_t staticAddress = staticVmAddress(liveAddress, imageSlide);
      if (staticAddress != 0)
        staticAddresses.push_back(staticAddress);
    }
    return joinHexAddresses(staticAddresses, separator);
  }

  std::string liveCodeTargetRegionClass(
    const RuntimeMemoryRegion* region,
    const std::string& executablePath)
  {
    if (region == nullptr)
      return "unmapped";
    if (sameMappedFile(region->mappedPath, executablePath))
      return region->executable ? "target-executable" : "target-data";
    if (region->mappedPath.empty())
      return region->executable ? "private-executable" : "private-data";
    return region->executable ? "mapped-executable" : "mapped-data";
  }

  std::string classifyRipReferenceInstruction(
    const std::vector<unsigned char>& bytes,
    std::size_t offset)
  {
    const unsigned char first = bytes[offset];
    const unsigned char second = offset + 1 < bytes.size() ? bytes[offset + 1] : 0;
    const unsigned char third = offset + 2 < bytes.size() ? bytes[offset + 2] : 0;

    if (first == 0xe8)
      return "call-rel32";
    if (first == 0xe9)
      return "jmp-rel32";
    if (first == 0x0f && second >= 0x80 && second <= 0x8f)
      return "jcc-rel32";
    if (first >= 0x40 && first <= 0x4f)
    {
      if (second == 0x8d)
        return "lea-rip";
      if (second == 0x8b)
        return "mov-load-rip";
      if (second == 0x89)
        return "mov-store-rip";
      if (second == 0x39 || second == 0x3b)
        return "cmp-rip";
      if (second == 0xc6 || second == 0xc7)
        return "mov-imm-store-rip";
      if (second == 0x80 || second == 0x81 || second == 0x83)
        return "op-imm-rip";
      if (second == 0x0f
          && (third == 0xb6 || third == 0xb7 || third == 0xbe || third == 0xbf))
        return "movx-rip";
    }
    if (first == 0x8d)
      return "lea-rip";
    if (first == 0x8b)
      return "mov-load-rip";
    if (first == 0x89)
      return "mov-store-rip";
    if (first == 0x39 || first == 0x3b)
      return "cmp-rip";
    if (first == 0xc6 || first == 0xc7)
      return "mov-imm-store-rip";
    if (first == 0x80 || first == 0x81 || first == 0x83)
      return "op-imm-rip";
    return "rip-rel32";
  }

  bool decodeRipReferenceInstruction(
    const std::vector<unsigned char>& bytes,
    std::size_t offset,
    std::uintptr_t instructionAddress,
    std::uintptr_t& target,
    std::size_t& instructionLength,
    std::string& kind)
  {
    if (offset + 5 > bytes.size())
      return false;

    const unsigned char first = bytes[offset];
    const unsigned char second = offset + 1 < bytes.size() ? bytes[offset + 1] : 0;
    const unsigned char third = offset + 2 < bytes.size() ? bytes[offset + 2] : 0;
    const auto resolveRel =
      [&](std::size_t displacementOffset, std::size_t length) -> bool
      {
        if (offset + displacementOffset + sizeof(std::int32_t) > bytes.size())
          return false;
        if (!addSignedOffset(
              instructionAddress + length,
              readS32(bytes, offset + displacementOffset),
              target))
          return false;
        instructionLength = length;
        kind = classifyRipReferenceInstruction(bytes, offset);
        return true;
      };

    if (first == 0xe8 || first == 0xe9)
      return resolveRel(1, 5);
    if (first == 0x0f && second >= 0x80 && second <= 0x8f)
      return resolveRel(2, 6);

    const auto modRmIsRipRelative =
      [](unsigned char modRm)
      {
        return (modRm & 0xc7) == 0x05;
      };
    const auto immediateSize =
      [](unsigned char opcode) -> std::size_t
      {
        if (opcode == 0xc6 || opcode == 0x80 || opcode == 0x83)
          return 1;
        if (opcode == 0xc7 || opcode == 0x81)
          return 4;
        return 0;
      };

    if ((first == 0x8b || first == 0x89 || first == 0x8d
          || first == 0x39 || first == 0x3b)
        && offset + 6 <= bytes.size()
        && modRmIsRipRelative(second))
      return resolveRel(2, 6);
    if ((first == 0xc6 || first == 0xc7 || first == 0x80
          || first == 0x81 || first == 0x83)
        && offset + 6 + immediateSize(first) <= bytes.size()
        && modRmIsRipRelative(second))
      return resolveRel(2, 6 + immediateSize(first));

    if (first >= 0x40 && first <= 0x4f && offset + 7 <= bytes.size())
    {
      if ((second == 0x8b || second == 0x89 || second == 0x8d
            || second == 0x39 || second == 0x3b)
          && modRmIsRipRelative(third))
        return resolveRel(3, 7);
      if ((second == 0xc6 || second == 0xc7 || second == 0x80
            || second == 0x81 || second == 0x83)
          && offset + 7 + immediateSize(second) <= bytes.size()
          && modRmIsRipRelative(third))
        return resolveRel(3, 7 + immediateSize(second));
      if (second == 0x0f
          && (third == 0xb6 || third == 0xb7 || third == 0xbe || third == 0xbf)
          && offset + 8 <= bytes.size()
          && modRmIsRipRelative(bytes[offset + 3]))
        return resolveRel(4, 8);
    }

    return false;
  }

  std::vector<LiveAnchorDiagnostic::CodeEvent> collectNearbyCodeEvents(
    int processId,
    const std::vector<RuntimeMemoryRegion>& regions,
    const std::string& executablePath,
    std::uintptr_t xrefAddress,
    std::size_t maxEvents)
  {
    std::vector<LiveAnchorDiagnostic::CodeEvent> events;
    if (xrefAddress == 0 || maxEvents == 0)
      return events;

    const RuntimeMemoryRegion* region = findReadableRegion(regions, xrefAddress, 1);
    if (region == nullptr || !region->executable)
      return events;

    constexpr std::uintptr_t beforeBytes = 128;
    constexpr std::uintptr_t afterBytes = 256;
    const std::uintptr_t regionEnd = region->address + region->size;
    const std::uintptr_t begin =
      xrefAddress > region->address + beforeBytes ? xrefAddress - beforeBytes : region->address;
    const std::uintptr_t end =
      std::min(regionEnd, xrefAddress > std::numeric_limits<std::uintptr_t>::max() - afterBytes
        ? regionEnd
        : xrefAddress + afterBytes);
    if (end <= begin)
      return events;

    RuntimeMemoryReadResult read = readProcessMemory(
      processId,
      begin,
      static_cast<std::size_t>(end - begin));
    if (!read.success || read.bytesRead < 5)
      return events;

    const auto appendEvent =
      [&](std::size_t offset,
          std::size_t instructionLength,
          const std::string& kind,
          std::uintptr_t target) -> bool
      {
        if (events.size() >= maxEvents)
          return false;
        LiveAnchorDiagnostic::CodeEvent event;
        event.address = begin + offset;
        event.kind = kind;
        event.target = target;
        event.bytes = bytesHexSpan(read.bytes, offset, instructionLength);
        const RuntimeMemoryRegion* targetRegion = findReadableRegion(regions, target, 1);
        event.targetInExecutable = targetRegion != nullptr
          && targetRegion->executable
          && sameMappedFile(targetRegion->mappedPath, executablePath);
        event.targetRegionClass = liveCodeTargetRegionClass(targetRegion, executablePath);
        if (targetRegion != nullptr)
          event.targetMappedPath = targetRegion->mappedPath;
        events.push_back(std::move(event));
        return true;
      };

    for (std::size_t offset = 0; offset + 5 <= read.bytesRead && events.size() < maxEvents; ++offset)
    {
      std::uintptr_t target = 0;
      std::size_t instructionLength = 0;
      std::string kind;
      if (!decodeRipReferenceInstruction(
            read.bytes,
            offset,
            begin + offset,
            target,
            instructionLength,
            kind))
        continue;

      appendEvent(offset, instructionLength, kind, target);
      if (instructionLength > 1)
        offset += instructionLength - 1;
    }

    return events;
  }

  LiveCallableDiagnostics discoverLiveCallableDiagnostics(
    int processId,
    const std::string& executablePath,
    const std::vector<std::string>& anchors)
  {
    LiveCallableDiagnostics diagnostics;
    diagnostics.attempted = true;
    if (processId <= 0)
    {
      diagnostics.reason = "process id must be positive";
      return diagnostics;
    }
    if (executablePath.empty())
    {
      diagnostics.reason = "target executable path is empty";
      return diagnostics;
    }

    RuntimeMemoryRegionListResult regions = listProcessMemoryRegions(processId);
    if (!regions.success)
    {
      diagnostics.reason = regions.reason;
      return diagnostics;
    }
    diagnostics.regionListAvailable = true;
    diagnostics.imageBase = findTargetImageBase(regions.regions, executablePath);
    if (diagnostics.imageBase >= 0x100000000ULL)
      diagnostics.imageSlide = diagnostics.imageBase - 0x100000000ULL;
    if (diagnostics.imageBase == 0)
    {
      diagnostics.reason = "target executable image mapping was not found in process memory";
      return diagnostics;
    }

    for (const std::string& anchor : anchors)
    {
      LiveAnchorDiagnostic anchorDiagnostic;
      anchorDiagnostic.anchor = anchor;
      const std::vector<std::uintptr_t> stringAddresses =
        findLiveAnchorStringAddresses(processId, regions.regions, executablePath, anchor, 1);
      if (stringAddresses.empty())
      {
        anchorDiagnostic.reason = "anchor string was not found in the live target image";
        diagnostics.anchors.push_back(std::move(anchorDiagnostic));
        continue;
      }

      anchorDiagnostic.stringAddress = stringAddresses.front();
      const std::vector<std::uintptr_t> xrefs = findRipRelativeXrefsToAddress(
        processId,
        regions.regions,
        executablePath,
        anchorDiagnostic.stringAddress,
        1);
      if (xrefs.empty())
      {
        anchorDiagnostic.reason = "anchor string was found, but no live RIP-relative code xref was found";
        diagnostics.anchors.push_back(std::move(anchorDiagnostic));
        continue;
      }

      anchorDiagnostic.xrefAddress = xrefs.front();
      anchorDiagnostic.estimatedInstructionAddress =
        anchorDiagnostic.xrefAddress >= 3 ? anchorDiagnostic.xrefAddress - 3 : anchorDiagnostic.xrefAddress;
      RuntimeMemoryReadResult instructionRead =
        readProcessMemory(processId, anchorDiagnostic.estimatedInstructionAddress, 16);
      if (instructionRead.success && instructionRead.bytesRead > 0)
      {
        if (instructionRead.bytes.size() > instructionRead.bytesRead)
          instructionRead.bytes.resize(instructionRead.bytesRead);
        anchorDiagnostic.estimatedInstructionBytes =
          bytesHexSpan(instructionRead.bytes, 0, instructionRead.bytes.size());
      }
      anchorDiagnostic.nearbyCallTargets = collectNearbyCallTargets(
        processId,
        regions.regions,
        executablePath,
        anchorDiagnostic.xrefAddress,
        8);
      anchorDiagnostic.nearbyCodeEvents = collectNearbyCodeEvents(
        processId,
        regions.regions,
        executablePath,
        anchorDiagnostic.xrefAddress,
        16);
      if (anchorDiagnostic.nearbyCallTargets.empty())
        anchorDiagnostic.reason = "live xref found, but no nearby target-image call targets were resolved";
      diagnostics.anchors.push_back(std::move(anchorDiagnostic));
    }

    return diagnostics;
  }

  void writeLiveCallableDiagnosticsFields(
    std::ofstream& output,
    const char* prefix,
    const LiveCallableDiagnostics& diagnostics)
  {
    output << prefix << "_live_diagnostics_attempted\t"
           << (diagnostics.attempted ? "true" : "false") << '\n';
    output << prefix << "_live_diagnostics_region_list_available\t"
           << (diagnostics.regionListAvailable ? "true" : "false") << '\n';
    output << prefix << "_live_diagnostics_image_base\t"
           << hexAddress(diagnostics.imageBase) << '\n';
    output << prefix << "_live_diagnostics_image_slide\t"
           << hexAddress(diagnostics.imageSlide) << '\n';
    output << prefix << "_live_diagnostics_anchor_count\t"
           << diagnostics.anchors.size() << '\n';
    if (!diagnostics.reason.empty())
      output << prefix << "_live_diagnostics_reason\t" << diagnostics.reason << '\n';
    for (std::size_t i = 0; i < diagnostics.anchors.size(); ++i)
    {
      const LiveAnchorDiagnostic& anchor = diagnostics.anchors[i];
      output << prefix << "_live_anchor_" << i << "_name\t" << anchor.anchor << '\n';
      output << prefix << "_live_anchor_" << i << "_string_address\t"
             << hexAddress(anchor.stringAddress) << '\n';
      output << prefix << "_live_anchor_" << i << "_xref_address\t"
             << hexAddress(anchor.xrefAddress) << '\n';
      output << prefix << "_live_anchor_" << i << "_estimated_instruction_address\t"
             << hexAddress(anchor.estimatedInstructionAddress) << '\n';
      output << prefix << "_live_anchor_" << i << "_estimated_static_vm_address\t"
             << hexAddress(staticVmAddress(
                  anchor.estimatedInstructionAddress,
                  diagnostics.imageSlide)) << '\n';
      if (!anchor.estimatedInstructionBytes.empty())
        output << prefix << "_live_anchor_" << i << "_estimated_instruction_bytes\t"
               << anchor.estimatedInstructionBytes << '\n';
      output << prefix << "_live_anchor_" << i << "_nearby_call_targets\t"
             << joinHexAddresses(anchor.nearbyCallTargets, ",") << '\n';
      output << prefix << "_live_anchor_" << i << "_nearby_call_target_static_vm_addresses\t"
             << joinStaticVmAddresses(
                  anchor.nearbyCallTargets,
                  diagnostics.imageSlide,
                  ",") << '\n';
      output << prefix << "_live_anchor_" << i << "_nearby_code_event_count\t"
             << anchor.nearbyCodeEvents.size() << '\n';
      for (std::size_t eventIndex = 0; eventIndex < anchor.nearbyCodeEvents.size(); ++eventIndex)
      {
        const LiveAnchorDiagnostic::CodeEvent& event = anchor.nearbyCodeEvents[eventIndex];
        output << prefix << "_live_anchor_" << i << "_nearby_code_event_" << eventIndex
               << "_address\t" << hexAddress(event.address) << '\n';
        output << prefix << "_live_anchor_" << i << "_nearby_code_event_" << eventIndex
               << "_static_vm_address\t"
               << hexAddress(staticVmAddress(event.address, diagnostics.imageSlide)) << '\n';
        output << prefix << "_live_anchor_" << i << "_nearby_code_event_" << eventIndex
               << "_kind\t" << event.kind << '\n';
        output << prefix << "_live_anchor_" << i << "_nearby_code_event_" << eventIndex
               << "_target\t" << hexAddress(event.target) << '\n';
        output << prefix << "_live_anchor_" << i << "_nearby_code_event_" << eventIndex
               << "_target_static_vm_address\t"
               << hexAddress(staticVmAddress(event.target, diagnostics.imageSlide)) << '\n';
        output << prefix << "_live_anchor_" << i << "_nearby_code_event_" << eventIndex
               << "_target_in_executable\t"
               << (event.targetInExecutable ? "true" : "false") << '\n';
        output << prefix << "_live_anchor_" << i << "_nearby_code_event_" << eventIndex
               << "_target_region_class\t" << event.targetRegionClass << '\n';
        if (!event.targetMappedPath.empty())
          output << prefix << "_live_anchor_" << i << "_nearby_code_event_" << eventIndex
                 << "_target_mapped_path\t" << event.targetMappedPath << '\n';
        output << prefix << "_live_anchor_" << i << "_nearby_code_event_" << eventIndex
               << "_bytes\t" << event.bytes << '\n';
      }
      if (!anchor.reason.empty())
        output << prefix << "_live_anchor_" << i << "_reason\t" << anchor.reason << '\n';
    }
  }

  std::vector<LiveCallableEntryCandidate> collectLiveCallableEntryCandidates(
    const LiveCallableDiagnostics& diagnostics,
    std::size_t maxCandidates)
  {
    std::vector<LiveCallableEntryCandidate> candidates;
    std::unordered_set<std::uintptr_t> seen;
    if (maxCandidates == 0)
      return candidates;

    const auto appendCandidate =
      [&](const std::string& anchor, const std::string& source, std::uintptr_t address)
      {
        if (address == 0 || candidates.size() >= maxCandidates || !seen.insert(address).second)
          return;
        candidates.push_back(LiveCallableEntryCandidate { anchor, source, address });
      };

    for (const LiveAnchorDiagnostic& anchor : diagnostics.anchors)
    {
      for (std::uintptr_t target : anchor.nearbyCallTargets)
        appendCandidate(anchor.anchor, "nearby-call-target", target);

      for (const LiveAnchorDiagnostic::CodeEvent& event : anchor.nearbyCodeEvents)
      {
        if (event.targetInExecutable && event.kind == "call-rel32")
          appendCandidate(anchor.anchor, event.kind, event.target);
      }
    }

    return candidates;
  }

  constexpr const char* residentAdapterAbi()
  {
    return "starcraft-api-resident-adapter-v1";
  }

  void writeRequiredResidentAdapterFields(
    std::ofstream& output,
    const char* prefix,
    const char* requiredBehavior,
    const LiveCallableDiagnostics& diagnostics)
  {
    constexpr const char* targetThreadPolicy = "execute-on-target-runtime-thread";
    constexpr std::size_t maxEntryCandidates = 16;

    const std::vector<LiveCallableEntryCandidate> entryCandidates =
      collectLiveCallableEntryCandidates(diagnostics, maxEntryCandidates);

    output << prefix << "_required_adapter_abi\t" << residentAdapterAbi() << '\n';
    output << prefix << "_required_adapter_location\tin-process-target-runtime\n";
    output << prefix << "_required_adapter_thread_policy\t" << targetThreadPolicy << '\n';
    output << prefix << "_required_adapter_behavior\t" << requiredBehavior << '\n';
    output << prefix << "_required_adapter_promotion_rule\t"
           << "do-not-emit-production-proof-until-live-behavior-is-observed\n";
    output << prefix << "_live_callable_entry_candidate_count\t"
           << entryCandidates.size() << '\n';

    for (std::size_t i = 0; i < entryCandidates.size(); ++i)
    {
      const LiveCallableEntryCandidate& candidate = entryCandidates[i];
      output << prefix << "_live_callable_entry_" << i << "_anchor\t"
             << candidate.anchor << '\n';
      output << prefix << "_live_callable_entry_" << i << "_source\t"
             << candidate.source << '\n';
      output << prefix << "_live_callable_entry_" << i << "_address\t"
             << hexAddress(candidate.address) << '\n';
      output << prefix << "_live_callable_entry_" << i << "_static_vm_address\t"
             << hexAddress(staticVmAddress(candidate.address, diagnostics.imageSlide)) << '\n';
    }
  }

  struct StarCraftImageSectionHints
  {
    std::uintptr_t commonAddress = 0;
    std::size_t commonSize = 0;
    std::uintptr_t bssAddress = 0;
    std::size_t bssSize = 0;
    std::uintptr_t cunitSgUnitsMemAddress = 0;
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
    hints.cunitSgUnitsMemAddress = 0x100f96c10ULL + slide;
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
    bool includeTargetImageMappedRegions = false)
  {
    if (!region.readable || !region.writable || region.executable)
      return false;
    if (fileBackedNonTargetRegion(region, executablePath))
      return false;

    const bool targetImageRegion = sameMappedFile(region.mappedPath, executablePath);
    const StarCraftImageSectionHints hints = starCraftImageSectionHints(targetImageBase);
    if (targetImageRegion
        && !includeTargetImageMappedRegions
        && !regionIntersectsStarCraftRuntimeData(region, hints))
      return false;

    const bool likelyTargetTextMapping =
      targetImageRegion
      && targetImageBase != 0
      && region.address == targetImageBase
      && region.size >= 8 * 1024 * 1024;
    return includeTargetImageMappedRegions || !likelyTargetTextMapping;
  }

  const RuntimeMemoryRegion* findUsableUnitStorageRegion(
    const std::vector<RuntimeMemoryRegion>& regions,
    std::uintptr_t address,
    std::size_t size,
    const std::string& executablePath,
    std::uintptr_t targetImageBase,
    bool includeTargetImageMappedRegions = false)
  {
    const RuntimeMemoryRegion* region = findReadableRegion(regions, address, size);
    if (region == nullptr)
      return nullptr;
    return usableUnitStorageRegion(
      *region,
      executablePath,
      targetImageBase,
      includeTargetImageMappedRegions)
      ? region
      : nullptr;
  }

  bool readableLiveDataObjectPointerValue(
    const std::vector<RuntimeMemoryRegion>& regions,
    std::uint64_t address,
    std::size_t size,
    std::size_t alignment,
    const std::string& executablePath,
    std::uintptr_t targetImageBase)
  {
    if (address == 0 || !addressFits(address))
      return false;
    if (alignment > 1 && (address % alignment) != 0)
      return false;
    return findUsableUnitStorageRegion(
      regions,
      static_cast<std::uintptr_t>(address),
      size,
      executablePath,
      targetImageBase)
      != nullptr;
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

  void rememberUnitScanRegionSample(
    UnitScanDiagnostics* diagnostics,
    const RuntimeMemoryRegion& region,
    const std::string& executablePath,
    std::uintptr_t targetImageBase,
    const std::string& stage,
    const std::string& decision,
    const std::string& reason,
    std::size_t bytesRead = 0)
  {
    if (diagnostics == nullptr)
      return;

    constexpr std::size_t maxRegionSamples = 96;
    UnitScanRegionDiagnostic sample;
    sample.stage = stage;
    sample.decision = decision;
    sample.reason = reason;
    sample.address = region.address;
    sample.size = region.size;
    sample.bytesRead = bytesRead;
    sample.priority = stage == "unit-node"
      ? unitNodeScanRegionPriority(region, executablePath, targetImageBase)
      : unitScanRegionPriority(region, executablePath, targetImageBase);
    sample.readable = region.readable;
    sample.writable = region.writable;
    sample.executable = region.executable;
    sample.targetExecutable = sameMappedFile(region.mappedPath, executablePath);
    sample.userTag = region.userTag;
    sample.shareMode = region.shareMode;
    sample.mappedPath = region.mappedPath;

    if (diagnostics->regionSamples.size() < maxRegionSamples)
    {
      diagnostics->regionSamples.push_back(std::move(sample));
      return;
    }

    if (decision == "scan" || decision == "read-failed")
    {
      auto replace = std::find_if(
        diagnostics->regionSamples.begin(),
        diagnostics->regionSamples.end(),
        [](const UnitScanRegionDiagnostic& existing)
        {
          return existing.decision != "scan" && existing.decision != "read-failed";
        });
      if (replace != diagnostics->regionSamples.end())
        *replace = std::move(sample);
    }
  }

  std::string unitBytesHexPrefix(const std::vector<unsigned char>& bytes, std::size_t maxBytes);

  int dynamicUnitScanRegionPriority(
    const RuntimeMemoryRegion& region,
    const std::string& executablePath,
    std::uintptr_t targetImageBase)
  {
    const StarCraftImageSectionHints hints = starCraftImageSectionHints(targetImageBase);
    if (regionIntersectsStarCraftRuntimeData(region, hints))
      return 0;
    if (region.mappedPath.empty())
      return 1;
    const bool targetImageRegion = sameMappedFile(region.mappedPath, executablePath);
    if (targetImageRegion)
      return 2;
    if (!fileBackedNonTargetRegion(region, executablePath))
      return 3;
    return 4;
  }

  std::vector<std::pair<std::size_t, std::size_t>> collectChangedRanges(
    const std::vector<unsigned char>& before,
    const std::vector<unsigned char>& after,
    std::size_t& changedBytes,
    std::size_t maxRanges)
  {
    changedBytes = 0;
    std::vector<std::pair<std::size_t, std::size_t>> ranges;
    const std::size_t size = std::min(before.size(), after.size());
    for (std::size_t offset = 0; offset < size;)
    {
      if (before[offset] == after[offset])
      {
        ++offset;
        continue;
      }

      const std::size_t rangeStart = offset;
      while (offset < size && before[offset] != after[offset])
      {
        ++changedBytes;
        ++offset;
      }
      if (ranges.size() < maxRanges)
        ranges.emplace_back(rangeStart, offset - rangeStart);
    }
    return ranges;
  }

  std::size_t countChangedBytesInRange(
    const std::vector<unsigned char>& before,
    const std::vector<unsigned char>& after,
    std::size_t offset,
    std::size_t size)
  {
    if (offset >= before.size() || offset >= after.size())
      return 0;
    const std::size_t limit =
      std::min(size, std::min(before.size() - offset, after.size() - offset));
    std::size_t changed = 0;
    for (std::size_t i = 0; i < limit; ++i)
    {
      if (before[offset + i] != after[offset + i])
        ++changed;
    }
    return changed;
  }

  std::size_t countReadablePointerWordsAt(
    const std::vector<unsigned char>& bytes,
    std::size_t offset,
    std::size_t size,
    const std::vector<RuntimeMemoryRegion>& regions)
  {
    if (offset >= bytes.size())
      return 0;
    const std::size_t limit = std::min(size, bytes.size() - offset);
    std::size_t count = 0;
    for (std::size_t cursor = 0; cursor + sizeof(std::uint64_t) <= limit; cursor += sizeof(std::uint64_t))
    {
      if (readablePointerValue(regions, readU64(bytes, offset + cursor), 8))
        ++count;
    }
    return count;
  }

  std::size_t countTaggedHandleWordsAt(
    const std::vector<unsigned char>& bytes,
    std::size_t offset,
    std::size_t size)
  {
    if (offset >= bytes.size())
      return 0;
    const std::size_t limit = std::min(size, bytes.size() - offset);
    std::size_t count = 0;
    for (std::size_t cursor = 0; cursor + sizeof(std::uint64_t) <= limit; cursor += sizeof(std::uint64_t))
    {
      if (plausibleRemasteredTaggedHandleValue(readU64(bytes, offset + cursor)))
        ++count;
    }
    return count;
  }

  bool findCoordinatePairInCandidate(
    const std::vector<unsigned char>& bytes,
    std::size_t offset,
    std::size_t size,
    std::size_t& coordinateOffset,
    std::int16_t& x,
    std::int16_t& y)
  {
    const std::size_t limit = std::min(size, bytes.size() - offset);
    for (std::size_t cursor = 0; cursor + sizeof(std::int16_t) * 2 <= limit; cursor += 2)
    {
      const std::int16_t candidateX = readS16(bytes, offset + cursor);
      const std::int16_t candidateY = readS16(bytes, offset + cursor + sizeof(std::int16_t));
      if (candidateX >= 16 && candidateX <= 16384 && candidateY >= 16 && candidateY <= 16384)
      {
        coordinateOffset = cursor;
        x = candidateX;
        y = candidateY;
        return true;
      }
    }

    for (std::size_t cursor = 0; cursor + sizeof(std::int32_t) * 2 <= limit; cursor += 4)
    {
      const std::int32_t candidateX = readS32(bytes, offset + cursor);
      const std::int32_t candidateY = readS32(bytes, offset + cursor + sizeof(std::int32_t));
      if (candidateX >= 16 && candidateX <= 16384 && candidateY >= 16 && candidateY <= 16384)
      {
        coordinateOffset = cursor;
        x = static_cast<std::int16_t>(candidateX);
        y = static_cast<std::int16_t>(candidateY);
        return true;
      }
    }
    return false;
  }

  bool findHitPointsCandidateInRecord(
    const std::vector<unsigned char>& bytes,
    std::size_t offset,
    std::size_t size,
    std::size_t& hitPointsOffset,
    std::uint32_t& hitPoints)
  {
    const std::size_t limit = std::min(size, bytes.size() - offset);
    for (std::size_t cursor = 0; cursor + sizeof(std::uint32_t) <= limit; cursor += 4)
    {
      const std::uint32_t candidate = readU32(bytes, offset + cursor);
      if (candidate >= 256 && candidate <= 1000000 && (candidate % 64) == 0)
      {
        hitPointsOffset = cursor;
        hitPoints = candidate;
        return true;
      }
    }

    for (std::size_t cursor = 0; cursor + 1 < limit; ++cursor)
    {
      const unsigned char current = bytes[offset + cursor];
      const unsigned char maximum = bytes[offset + cursor + 1];
      if (current != 0 && maximum != 0 && current <= maximum)
      {
        hitPointsOffset = cursor;
        hitPoints = static_cast<std::uint32_t>(current) * 256u;
        return true;
      }
    }
    return false;
  }

  bool findPlayerAndTypeHintInRecord(
    const std::vector<unsigned char>& bytes,
    std::size_t offset,
    std::size_t size,
    std::size_t& playerOffset,
    int& player,
    std::size_t& typeOffset,
    std::uint16_t& typeHint)
  {
    const std::size_t limit = std::min(size, bytes.size() - offset);
    bool foundPlayer = false;
    bool foundType = false;
    for (std::size_t cursor = 0; cursor < limit; ++cursor)
    {
      const unsigned char candidate = bytes[offset + cursor];
      if (candidate < 12)
      {
        playerOffset = cursor;
        player = static_cast<int>(candidate);
        foundPlayer = true;
        break;
      }
    }
    for (std::size_t cursor = 0; cursor + sizeof(std::uint16_t) <= limit; cursor += 2)
    {
      const std::uint16_t candidate = readU16(bytes, offset + cursor);
      if (plausibleRemasteredUnitTypeHint(candidate))
      {
        typeOffset = cursor;
        typeHint = candidate;
        foundType = true;
        break;
      }
    }
    return foundPlayer && foundType;
  }

  void rememberDynamicRegionSample(
    UnitScanDiagnostics* diagnostics,
    const RuntimeMemoryRegion& region,
    const std::string& executablePath,
    std::uintptr_t targetImageBase,
    std::size_t bytesRead,
    std::size_t changedBytes,
    const std::vector<std::pair<std::size_t, std::size_t>>& ranges)
  {
    if (diagnostics == nullptr)
      return;

    UnitDynamicRegionDiagnostic sample;
    sample.address = region.address;
    sample.size = region.size;
    sample.bytesRead = bytesRead;
    sample.changedBytes = changedBytes;
    sample.changedRanges = ranges.size();
    if (!ranges.empty())
    {
      sample.firstChangedAddress = region.address + ranges.front().first;
      sample.firstChangedSize = ranges.front().second;
    }
    sample.priority = dynamicUnitScanRegionPriority(region, executablePath, targetImageBase);
    sample.targetExecutable = sameMappedFile(region.mappedPath, executablePath);
    sample.userTag = region.userTag;
    sample.shareMode = region.shareMode;
    sample.mappedPath = region.mappedPath;

    diagnostics->dynamicRegionSamples.push_back(std::move(sample));
    std::stable_sort(
      diagnostics->dynamicRegionSamples.begin(),
      diagnostics->dynamicRegionSamples.end(),
      [](const UnitDynamicRegionDiagnostic& lhs, const UnitDynamicRegionDiagnostic& rhs)
      {
        if (lhs.changedBytes != rhs.changedBytes)
          return lhs.changedBytes > rhs.changedBytes;
        if (lhs.priority != rhs.priority)
          return lhs.priority < rhs.priority;
        return lhs.address < rhs.address;
      });
    constexpr std::size_t maxDynamicRegionSamples = 24;
    if (diagnostics->dynamicRegionSamples.size() > maxDynamicRegionSamples)
      diagnostics->dynamicRegionSamples.resize(maxDynamicRegionSamples);
  }

  void collectDynamicFieldCandidateSamples(
    UnitScanDiagnostics* diagnostics,
    const std::vector<unsigned char>& before,
    const std::vector<unsigned char>& after,
    std::uintptr_t baseAddress,
    const std::vector<RuntimeMemoryRegion>& regions)
  {
    if (diagnostics == nullptr || before.size() != after.size() || after.size() < 64)
      return;

    constexpr std::size_t maxDynamicFieldCandidates = 24;
    constexpr std::size_t candidateSize = 256;
    for (std::size_t offset = 0;
         offset + 64 <= after.size() && diagnostics->dynamicFieldCandidates.size() < maxDynamicFieldCandidates;
         offset += 8)
    {
      const std::size_t windowSize = std::min(candidateSize, after.size() - offset);
      const std::size_t changedBytes = countChangedBytesInRange(before, after, offset, windowSize);
      if (changedBytes == 0)
        continue;

      const std::size_t readablePointerWords =
        countReadablePointerWordsAt(after, offset, windowSize, regions);
      const std::size_t taggedHandleWords =
        countTaggedHandleWordsAt(after, offset, windowSize);
      if (readablePointerWords + taggedHandleWords < 2)
        continue;

      UnitDynamicFieldCandidateDiagnostic sample;
      sample.address = baseAddress + offset;
      sample.windowSize = windowSize;
      sample.changedBytes = changedBytes;
      sample.readablePointerWords = readablePointerWords;
      sample.taggedHandleWords = taggedHandleWords;
      if (!findCoordinatePairInCandidate(
            after,
            offset,
            windowSize,
            sample.coordinateOffset,
            sample.x,
            sample.y))
        continue;
      if (!findHitPointsCandidateInRecord(
            after,
            offset,
            windowSize,
            sample.hitPointsOffset,
            sample.hitPoints))
        continue;
      if (!findPlayerAndTypeHintInRecord(
            after,
            offset,
            windowSize,
            sample.playerOffset,
            sample.player,
            sample.typeOffset,
            sample.typeHint))
        continue;

      const std::size_t prefixSize = std::min<std::size_t>(windowSize, 64);
      std::vector<unsigned char> prefix(
        after.begin() + static_cast<std::vector<unsigned char>::difference_type>(offset),
        after.begin() + static_cast<std::vector<unsigned char>::difference_type>(offset + prefixSize));
      sample.prefixHex = unitBytesHexPrefix(prefix, prefix.size());
      diagnostics->dynamicFieldCandidates.push_back(std::move(sample));
    }
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

  bool plausibleCompactUnitNodeAnchorFields(
    const std::vector<unsigned char>& bytes,
    std::size_t offset)
  {
    constexpr std::size_t recordSize = 0x28;
    if (offset + recordSize > bytes.size())
      return false;
    if (containsLongPrintableAsciiRun(bytes, offset, recordSize))
      return false;

    const std::uint64_t previous = readU64(bytes, offset);
    const std::uint64_t next = readU64(bytes, offset + 0x08);
    const std::int32_t x = readS32(bytes, offset + 0x10);
    const std::int32_t y = readS32(bytes, offset + 0x14);
    const std::uint64_t sprite = readU64(bytes, offset + 0x18);
    const std::uint64_t secondaryObject = readU64(bytes, offset + 0x20);

    const bool previousLooksLikeObject =
      previous != 0 && plausibleRemasteredCompactObjectValue(previous);
    const bool nextLooksLikeObject =
      next != 0 && plausibleRemasteredCompactObjectValue(next);
    const bool linked = previousLooksLikeObject || nextLooksLikeObject;
    return linked
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
    const std::vector<RuntimeMemoryRegion>& regions)
  {
    if (!plausibleCompactUnitNodeAnchorFields(bytes, offset))
      return false;

    const std::uint64_t previous = readU64(bytes, offset);
    const std::uint64_t next = readU64(bytes, offset + 0x08);
    const std::uint64_t sprite = readU64(bytes, offset + 0x18);
    const std::uint64_t secondaryObject = readU64(bytes, offset + 0x20);
    const bool readableLink =
      readableRuntimeObjectPointerValue(regions, previous, 16)
      || readableRuntimeObjectPointerValue(regions, next, 16);
    const bool taggedLink =
      plausibleRemasteredTaggedHandleValue(previous)
      || plausibleRemasteredTaggedHandleValue(next);
    const bool readableSprite = readableRuntimeObjectPointerValue(regions, sprite, 0xd0);
    const bool readableSecondary = readableRuntimeObjectPointerValue(regions, secondaryObject, 0xe0);
    const bool taggedSprite = plausibleRemasteredTaggedHandleValue(sprite);
    const bool taggedSecondary = plausibleRemasteredTaggedHandleValue(secondaryObject);
    const bool metadataObjectReadable = readableSprite || readableSecondary;
    return (readableLink || taggedLink)
      && (readableSprite || taggedSprite)
      && (readableSecondary || taggedSecondary)
      && metadataObjectReadable;
  }

  bool parseRemasteredCompactUnitSnapshotRecord(
    int processId,
    const std::vector<unsigned char>& bytes,
    std::size_t offset,
    std::uintptr_t nodeAddress,
    const std::vector<RuntimeMemoryRegion>& regions,
    RemasteredUnitSnapshotRecord& record)
  {
    if (!plausibleCompactUnitNodeAnchorRecord(bytes, offset, regions))
      return false;

    const std::uint64_t spriteAddress64 = readU64(bytes, offset + 0x18);
    const std::uint64_t secondaryAddress64 = readU64(bytes, offset + 0x20);
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
    record.x = static_cast<std::int16_t>(readS32(bytes, offset + 0x10));
    record.y = static_cast<std::int16_t>(readS32(bytes, offset + 0x14));
    record.targetX = record.x;
    record.targetY = record.y;
    record.order = 0;
    record.state = 0;
    record.player = 0;
    record.typeHint = 1;
    record.metadataDerived = true;
    record.taggedHandleDerived = taggedSprite || taggedSecondary;

    constexpr std::size_t spriteSnapshotBytes = 0xd0;
    if (!taggedSprite && readableRuntimeObjectPointerValue(regions, spriteAddress64, spriteSnapshotBytes))
    {
      RuntimeMemoryReadResult spriteRead =
        readProcessMemory(processId, record.spriteAddress, spriteSnapshotBytes);
      if (spriteRead.success && spriteRead.bytesRead >= spriteSnapshotBytes)
      {
        const std::uint32_t primaryPlayer = readU32(spriteRead.bytes, 0x6c);
        const unsigned char secondaryPlayer = spriteRead.bytes[0xc0];
        if (primaryPlayer < 12)
          record.player = static_cast<int>(primaryPlayer);
        else if (secondaryPlayer < 12)
          record.player = static_cast<int>(secondaryPlayer);

        const std::uint16_t typeHint = static_cast<std::uint16_t>(readU32(spriteRead.bytes, 0x68) & 0xffffu);
        if (plausibleRemasteredUnitTypeHint(typeHint))
          record.typeHint = typeHint;

        const std::uint32_t hitPoints = readU32(spriteRead.bytes, 0x80);
        if (hitPoints > 0 && hitPoints <= 1000000)
        {
          record.hitPoints = hitPoints;
          record.hitPointsResolved = true;
          record.hitPointsSource = "sprite+0x80 hp-raw";
        }
      }
    }

    constexpr std::size_t secondarySnapshotBytes = 0xe0;
    if (!taggedSecondary
        && readableRuntimeObjectPointerValue(regions, secondaryAddress64, secondarySnapshotBytes))
    {
      RuntimeMemoryReadResult secondaryRead =
        readProcessMemory(processId, record.secondaryAddress, secondarySnapshotBytes);
      if (secondaryRead.success && secondaryRead.bytesRead >= secondarySnapshotBytes)
      {
        const unsigned char rawPlayer = secondaryRead.bytes[0x14];
        std::uint16_t typeHint = readU16(secondaryRead.bytes, 0x10);
        if (!plausibleRemasteredUnitTypeHint(typeHint))
          typeHint = readU16(secondaryRead.bytes, 0x20);
        if ((rawPlayer < 12 || rawPlayer == 255) && plausibleRemasteredUnitTypeHint(typeHint))
        {
          record.player = rawPlayer == 255 ? 11 : static_cast<int>(rawPlayer);
          record.typeHint = typeHint;
          const unsigned char currentHitPoints = secondaryRead.bytes[0x1a];
          const unsigned char maxHitPoints = secondaryRead.bytes[0x1b];
          if (currentHitPoints != 0 && maxHitPoints != 0 && currentHitPoints <= maxHitPoints)
          {
            record.hitPoints = static_cast<std::uint32_t>(currentHitPoints) * 256u;
            record.hitPointsResolved = true;
            record.hitPointsSource = "secondary+0x1a compact-hp-byte";
          }
        }

        const unsigned char metadataPlayer = secondaryRead.bytes[0xc0];
        const std::uint16_t metadataType = readU16(secondaryRead.bytes, 0xd0);
        if (metadataPlayer < 12 && plausibleRemasteredUnitTypeHint(metadataType))
        {
          record.player = static_cast<int>(metadataPlayer);
          record.typeHint = metadataType;
          if (!record.hitPointsResolved)
          {
            const unsigned char currentHitPoints = secondaryRead.bytes[0x1a];
            const unsigned char maxHitPoints = secondaryRead.bytes[0x1b];
            if (currentHitPoints != 0 && maxHitPoints != 0 && currentHitPoints <= maxHitPoints)
            {
              record.hitPoints = static_cast<std::uint32_t>(currentHitPoints) * 256u;
              record.hitPointsResolved = true;
              record.hitPointsSource = "secondary+0x1a compact-hp-byte";
            }
          }
        }
      }
    }

    return record.id != 0 && record.player >= 0 && record.player < 12;
  }

  bool remasteredCompactRecordsHaveBwapiMetadata(
    const std::vector<RemasteredUnitSnapshotRecord>& records)
  {
    return std::any_of(
      records.begin(),
      records.end(),
      [](const RemasteredUnitSnapshotRecord& record)
      {
        return !record.taggedHandleDerived || record.hitPointsResolved;
      });
  }

  void rememberUnitNodeFieldSample(
    UnitScanDiagnostics* diagnostics,
    const std::vector<unsigned char>& bytes,
    std::size_t offset,
    std::uintptr_t address,
    const std::vector<RuntimeMemoryRegion>& regions,
    const char* kind = "unit-node-0x58",
    std::string rejectionReason = {})
  {
    if (diagnostics == nullptr || diagnostics->unitNodeFieldSamples.size() >= 16)
      return;
    const auto duplicate = std::find_if(
      diagnostics->unitNodeFieldSamples.begin(),
      diagnostics->unitNodeFieldSamples.end(),
      [&](const UnitNodeFieldCandidateDiagnostic& existing)
      {
        return existing.kind == kind && existing.address == address;
      });
    if (duplicate != diagnostics->unitNodeFieldSamples.end())
      return;

    UnitNodeFieldCandidateDiagnostic sample;
    sample.kind = kind;
    sample.rejectionReason = std::move(rejectionReason);
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
    sample.nodePrefixNonZeroBytes = countNonZeroBytesAt(bytes, offset, 64);
    sample.nodePrefixHex = bytesHexPrefixAt(bytes, offset, 64);
    diagnostics->unitNodeFieldSamples.push_back(sample);
  }

  void rememberCompactUnitNodeFieldSample(
    int processId,
    UnitScanDiagnostics* diagnostics,
    const std::vector<unsigned char>& bytes,
    std::size_t offset,
    std::uintptr_t address,
    const std::vector<RuntimeMemoryRegion>& regions,
    std::string rejectionReason = {})
  {
    if (diagnostics == nullptr || diagnostics->unitNodeFieldSamples.size() >= 16)
      return;
    const auto duplicate = std::find_if(
      diagnostics->unitNodeFieldSamples.begin(),
      diagnostics->unitNodeFieldSamples.end(),
      [&](const UnitNodeFieldCandidateDiagnostic& existing)
      {
        return existing.kind == "compact-unit-node-0x28" && existing.address == address;
      });
    if (duplicate != diagnostics->unitNodeFieldSamples.end())
      return;

    UnitNodeFieldCandidateDiagnostic sample;
    sample.kind = "compact-unit-node-0x28";
    sample.rejectionReason = std::move(rejectionReason);
    sample.address = address;
    sample.previous = readU64(bytes, offset);
    sample.next = readU64(bytes, offset + 0x08);
    sample.x = static_cast<std::int16_t>(readS32(bytes, offset + 0x10));
    sample.y = static_cast<std::int16_t>(readS32(bytes, offset + 0x14));
    sample.targetX = sample.x;
    sample.targetY = sample.y;
    sample.sprite = readU64(bytes, offset + 0x18);
    sample.secondaryObject = readU64(bytes, offset + 0x20);
    sample.readableLink =
      readableRuntimeObjectPointerValue(regions, sample.previous, 16)
      || readableRuntimeObjectPointerValue(regions, sample.next, 16);
    sample.readableSprite = readableRuntimeObjectPointerValue(regions, sample.sprite, 0xd0);
    sample.readableSecondaryObject =
      readableRuntimeObjectPointerValue(regions, sample.secondaryObject, 0xe0);
    sample.nodePrefixNonZeroBytes = countNonZeroBytesAt(bytes, offset, 64);
    sample.nodePrefixHex = bytesHexPrefixAt(bytes, offset, 64);
    if (sample.readableSprite)
    {
      RuntimeMemoryReadResult spriteRead =
        readProcessMemory(processId, static_cast<std::uintptr_t>(sample.sprite), 64);
      if (spriteRead.success)
      {
        sample.spritePrefixNonZeroBytes = countNonZeroBytesAt(spriteRead.bytes, 0, 64);
        sample.spritePrefixHex = bytesHexPrefixAt(spriteRead.bytes, 0, 64);
      }
    }
    if (sample.readableSecondaryObject)
    {
      RuntimeMemoryReadResult secondaryRead =
        readProcessMemory(processId, static_cast<std::uintptr_t>(sample.secondaryObject), 64);
      if (secondaryRead.success)
      {
        sample.secondaryPrefixNonZeroBytes = countNonZeroBytesAt(secondaryRead.bytes, 0, 64);
        sample.secondaryPrefixHex = bytesHexPrefixAt(secondaryRead.bytes, 0, 64);
      }
    }
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

    const auto duplicate = std::find_if(
      diagnostics->unitNodeVectorSamples.begin(),
      diagnostics->unitNodeVectorSamples.end(),
      [&](const UnitNodeVectorCandidateDiagnostic& existing)
      {
        return existing.vectorAddress == vectorAddress
          && existing.begin == begin
          && existing.end == end
          && existing.capacity == capacity;
      });
    if (duplicate != diagnostics->unitNodeVectorSamples.end())
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

  std::string unitBytesHexPrefix(const std::vector<unsigned char>& bytes, std::size_t maxBytes)
  {
    std::ostringstream output;
    const std::size_t limit = std::min(bytes.size(), maxBytes);
    for (std::size_t i = 0; i < limit; ++i)
    {
      if (i > 0)
        output << ' ';
      output << byteHex(bytes[i]);
    }
    return output.str();
  }

  std::size_t countNonZeroBytes(const std::vector<unsigned char>& bytes, std::size_t maxBytes)
  {
    const std::size_t limit = std::min(bytes.size(), maxBytes);
    return static_cast<std::size_t>(std::count_if(
      bytes.begin(),
      bytes.begin() + static_cast<std::ptrdiff_t>(limit),
      [](unsigned char byte)
      {
        return byte != 0;
      }));
  }

  std::size_t countReadablePointerWords(
    const std::vector<unsigned char>& bytes,
    const std::vector<RuntimeMemoryRegion>& regions,
    std::size_t maxBytes)
  {
    const std::size_t limit = std::min(bytes.size(), maxBytes);
    std::size_t count = 0;
    for (std::size_t offset = 0; offset + sizeof(std::uint64_t) <= limit; offset += sizeof(std::uint64_t))
    {
      if (readablePointerValue(regions, readU64(bytes, offset), 8))
        ++count;
    }
    return count;
  }

  void rememberUnitPointerArraySample(
    UnitScanDiagnostics* diagnostics,
    std::uintptr_t vectorAddress,
    std::uintptr_t begin,
    std::uintptr_t end,
    std::uintptr_t capacity,
    std::size_t pointerCount,
    std::size_t readablePointers,
    const std::vector<std::vector<unsigned char>>& recordSnapshots,
    std::uintptr_t firstPointer,
    std::uintptr_t secondPointer,
    const std::vector<RuntimeMemoryRegion>& regions)
  {
    if (diagnostics == nullptr || diagnostics->pointerArraySamples.size() >= 16)
      return;

    UnitPointerArrayCandidateDiagnostic sample;
    sample.vectorAddress = vectorAddress;
    sample.begin = begin;
    sample.end = end;
    sample.capacity = capacity;
    sample.usedBytes = end > begin ? static_cast<std::size_t>(end - begin) : 0;
    sample.pointerCount = pointerCount;
    sample.readablePointers = readablePointers;
    sample.recordSnapshots = recordSnapshots.size();
    sample.firstPointer = firstPointer;
    sample.secondPointer = secondPointer;
    if (!recordSnapshots.empty())
    {
      sample.firstRecordNonZeroBytes = countNonZeroBytes(recordSnapshots.front(), 128);
      sample.firstRecordPointerWords = countReadablePointerWords(recordSnapshots.front(), regions, 128);
      sample.firstRecordPrefixHex = unitBytesHexPrefix(recordSnapshots.front(), 32);
    }
    diagnostics->pointerArraySamples.push_back(std::move(sample));
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
        if (consecutiveActiveRecords > proof.activeRecords)
        {
          proof.activeRecords = consecutiveActiveRecords;
          proof.address = firstConsecutiveAddress;
        }
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

  LiveUnitNodeProof scoreCompactUnitNodeAnchorArray(
    int processId,
    const std::vector<unsigned char>& bytes,
    std::uintptr_t baseAddress,
    std::size_t offset,
    const std::vector<RuntimeMemoryRegion>& regions,
    const std::chrono::steady_clock::time_point& deadline,
    bool& scanTimedOut,
    UnitScanDiagnostics* diagnostics)
  {
    constexpr std::size_t recordSize = 0x28;
    constexpr std::size_t maxSampledRecords = 4096;

    LiveUnitNodeProof proof;
    proof.recordSize = recordSize;
    proof.sampledRecords = std::min(maxSampledRecords, (bytes.size() - offset) / recordSize);
    std::vector<RemasteredUnitSnapshotRecord> consecutiveRecords;
    std::uintptr_t firstConsecutiveAddress = 0;

    for (std::size_t i = 0; i < proof.sampledRecords; ++i)
    {
      if ((i % 32) == 0 && timedOut(deadline))
      {
        scanTimedOut = true;
        return {};
      }

      const std::size_t recordOffset = offset + i * recordSize;
      RemasteredUnitSnapshotRecord record;
      const std::uintptr_t nodeAddress = baseAddress + recordOffset;
      if (parseRemasteredCompactUnitSnapshotRecord(
            processId,
            bytes,
            recordOffset,
            nodeAddress,
            regions,
            record))
      {
        if (diagnostics != nullptr)
          ++diagnostics->unitNodeReadableCandidates;
        if (consecutiveRecords.empty())
          firstConsecutiveAddress = nodeAddress;
        record.index = consecutiveRecords.size();
        consecutiveRecords.push_back(record);
        if (consecutiveRecords.size() > proof.activeRecords)
        {
          proof.activeRecords = consecutiveRecords.size();
          proof.address = firstConsecutiveAddress;
          proof.records = consecutiveRecords;
        }
        if (consecutiveRecords.size() >= minRemasteredSnapshotUnitRecords)
        {
          if (remasteredCompactRecordsHaveBwapiMetadata(consecutiveRecords))
          {
            proof.address = firstConsecutiveAddress;
            proof.passed = true;
            proof.records = std::move(consecutiveRecords);
            return proof;
          }
          proof.address = firstConsecutiveAddress;
          proof.records = consecutiveRecords;
          proof.reason =
            "candidate compact SC:R tagged-handle records did not resolve BWAPI metadata/hit-points";
        }
      }
      else
      {
        consecutiveRecords.clear();
        firstConsecutiveAddress = 0;
      }
    }

    if (proof.reason.empty())
      proof.reason = "candidate compact SC:R unit-node anchor array did not contain enough active records";
    rememberBestUnitNodeCandidate(diagnostics, proof);
    return proof;
  }

  LiveUnitNodeProof scoreCompactLinkedUnitNodeGraph(
    int processId,
    std::uintptr_t seedAddress,
    const std::vector<RuntimeMemoryRegion>& regions,
    const std::chrono::steady_clock::time_point& deadline,
    bool& scanTimedOut,
    UnitScanDiagnostics* diagnostics)
  {
    constexpr std::size_t recordSize = 0x28;
    constexpr std::size_t maxGraphRecords = 256;

    LiveUnitNodeProof proof;
    proof.address = seedAddress;
    proof.recordSize = recordSize;
    if (!readableRuntimeObjectPointerValue(regions, seedAddress, recordSize))
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
      if ((index % 16) == 0 && timedOut(deadline))
      {
        scanTimedOut = true;
        return {};
      }

      const std::uintptr_t nodeAddress = pending[index];
      if (!readableRuntimeObjectPointerValue(regions, nodeAddress, recordSize))
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
        if (!plausibleRemasteredCompactObjectValue(link) || !addressFits(link))
          continue;
        const auto linkedAddress = static_cast<std::uintptr_t>(link);
        if (linkedAddress == 0
            || !readableRuntimeObjectPointerValue(regions, linkedAddress, recordSize))
          continue;
        if (queued.insert(linkedAddress).second)
          pending.push_back(linkedAddress);
      }

      RemasteredUnitSnapshotRecord record;
      record.index = proof.records.size();
      if (!parseRemasteredCompactUnitSnapshotRecord(
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
      if (proof.records.size() >= minRemasteredSnapshotUnitRecords
          && remasteredCompactRecordsHaveBwapiMetadata(proof.records))
      {
        proof.passed = true;
        return proof;
      }
    }

    if (proof.activeRecords > 0)
    {
      proof.reason = "compact SC:R unit-node graph found "
        + std::to_string(proof.activeRecords)
        + " active records below required="
        + std::to_string(minRemasteredSnapshotUnitRecords);
    }
    else
    {
      proof.reason = "candidate compact SC:R unit-node graph did not contain active records";
    }
    rememberBestUnitNodeCandidate(diagnostics, proof);
    return proof;
  }

  LiveUnitNodeProof proveCompactUnitNodeGraphsInBytes(
    int processId,
    const std::vector<unsigned char>& bytes,
    std::uintptr_t baseAddress,
    const std::vector<RuntimeMemoryRegion>& regions,
    const std::chrono::steady_clock::time_point& deadline,
    bool& scanTimedOut,
    UnitScanDiagnostics* diagnostics)
  {
    constexpr std::size_t recordSize = 0x28;
    constexpr std::size_t maxGraphSeedsToScore = 512;
    std::size_t graphSeedsScored = 0;
    std::unordered_set<std::uintptr_t> scoredSeeds;

    for (std::size_t offset = firstAlignedOffset(baseAddress, alignof(std::uint64_t));
         offset + recordSize <= bytes.size();
         offset += alignof(std::uint64_t))
    {
      if ((offset % (16 * 1024)) == 0 && timedOut(deadline))
      {
        scanTimedOut = true;
        return {};
      }
      if (!plausibleCompactUnitNodeAnchorFields(bytes, offset))
        continue;

      const bool readableCandidate =
        plausibleCompactUnitNodeAnchorRecord(bytes, offset, regions);
      rememberCompactUnitNodeFieldSample(
        processId,
        diagnostics,
        bytes,
        offset,
        baseAddress + offset,
        regions,
        readableCandidate ? std::string() : "compact graph seed fields matched but link/sprite/secondary pointers were not readable");
      if (!readableCandidate)
        continue;

      const std::uintptr_t seedAddress = baseAddress + offset;
      if (!scoredSeeds.insert(seedAddress).second)
        continue;
      if (diagnostics != nullptr)
        ++diagnostics->unitNodeGraphSeedsScored;
      if (++graphSeedsScored > maxGraphSeedsToScore)
        return {};

      LiveUnitNodeProof proof = scoreCompactLinkedUnitNodeGraph(
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

  LiveUnitNodeProof proveCompactUnitNodeAnchorsInBytes(
    int processId,
    const std::vector<unsigned char>& bytes,
    std::uintptr_t baseAddress,
    const std::vector<RuntimeMemoryRegion>& regions,
    const std::chrono::steady_clock::time_point& deadline,
    bool& scanTimedOut,
    UnitScanDiagnostics* diagnostics)
  {
    constexpr std::size_t recordSize = 0x28;
    if (recordSize * minRemasteredSnapshotUnitRecords > bytes.size())
      return {};

    std::vector<std::size_t> plausibleByResidue(recordSize, 0);
    for (std::size_t recordOffset = firstAlignedOffset(baseAddress, alignof(std::uint64_t));
         recordOffset + recordSize <= bytes.size();
         recordOffset += alignof(std::uint64_t))
    {
      if ((recordOffset % (4 * 1024)) == 0 && timedOut(deadline))
      {
        scanTimedOut = true;
        return {};
      }
      if (plausibleCompactUnitNodeAnchorFields(bytes, recordOffset))
      {
        if (diagnostics != nullptr)
          ++diagnostics->unitNodeFieldCandidates;
        const bool readableCandidate =
          plausibleCompactUnitNodeAnchorRecord(bytes, recordOffset, regions);
        rememberCompactUnitNodeFieldSample(
          processId,
          diagnostics,
          bytes,
          recordOffset,
          baseAddress + recordOffset,
          regions,
          readableCandidate ? std::string() : "compact anchor fields matched but link/sprite/secondary pointers were not readable");
        if (readableCandidate)
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
      LiveUnitNodeProof proof = scoreCompactUnitNodeAnchorArray(
        processId,
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
    for (std::size_t recordOffset = firstAlignedOffset(baseAddress, alignof(std::uint64_t));
         recordOffset + recordSize <= bytes.size();
         recordOffset += alignof(std::uint64_t))
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
        const bool readableCandidate =
          plausibleUnitNodeAnchorRecord(bytes, recordOffset, regions);
        rememberUnitNodeFieldSample(
          diagnostics,
          bytes,
          recordOffset,
          baseAddress + recordOffset,
          regions,
          "unit-node-0x58",
          readableCandidate ? std::string() : "anchor fields matched but link/sprite/secondary pointers were not readable");
        if (readableCandidate)
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
    const std::string& executablePath,
    std::uintptr_t targetImageBase,
    bool includeTargetImageMappedRegions,
    const std::chrono::steady_clock::time_point& deadline,
    bool& scanTimedOut,
  UnitScanDiagnostics* diagnostics)
  {
    constexpr std::size_t recordSize = 0x58;
    constexpr std::size_t compactRecordSize = 0x28;
    constexpr std::size_t maxUnitNodeRecords = 4096;
    constexpr std::size_t maxVectorCandidatesToRead = 256;
    constexpr std::size_t maxVectorOffsetsToInspect = 1024 * 1024;
    constexpr std::size_t maxCollectedVectorCandidates = 4096;
    constexpr std::size_t maxVectorCapacityBytes = 32 * 1024 * 1024;
    struct Candidate
    {
      std::uintptr_t vectorAddress = 0;
      std::uintptr_t begin = 0;
      std::uintptr_t end = 0;
      std::uintptr_t capacity = 0;
      std::size_t usedBytes = 0;
      std::size_t recordCount = 0;
      std::size_t compactRecordCount = 0;
      std::size_t pointerCount = 0;
      bool recordVector = false;
      bool compactRecordVector = false;
      bool pointerVector = false;
      int score = 0;
    };
    std::vector<Candidate> candidates;
    std::size_t vectorOffsetsInspected = 0;
    for (std::size_t offset = firstAlignedOffset(baseAddress, alignof(std::uint64_t));
         offset + sizeof(std::uint64_t) * 3 <= bytes.size();
         offset += alignof(std::uint64_t))
    {
      if (++vectorOffsetsInspected > maxVectorOffsetsToInspect)
        break;
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
      const std::size_t capacityBytes = static_cast<std::size_t>(capacity - begin);
      if (capacityBytes == 0 || capacityBytes > maxVectorCapacityBytes)
        continue;
      if (findUsableUnitStorageRegion(
            regions,
            capacity - 1,
            1,
            executablePath,
            targetImageBase,
            includeTargetImageMappedRegions) == nullptr)
        continue;

      const bool nodeRecordVector =
        usedBytes >= recordSize * minActiveUnitRecords
        && (usedBytes % recordSize) == 0;
      const bool compactNodeRecordVector =
        usedBytes >= compactRecordSize * minRemasteredSnapshotUnitRecords
        && (usedBytes % compactRecordSize) == 0;
      const bool nodePointerVector =
        usedBytes >= sizeof(std::uint64_t) * minActiveUnitRecords
        && (usedBytes % sizeof(std::uint64_t)) == 0;
      if (!nodeRecordVector && !compactNodeRecordVector && !nodePointerVector)
        continue;
      if (diagnostics != nullptr)
        ++diagnostics->unitNodeVectorCandidates;

      const std::size_t recordCount = nodeRecordVector ? usedBytes / recordSize : 0;
      const std::size_t compactRecordCount =
        compactNodeRecordVector ? usedBytes / compactRecordSize : 0;
      const std::size_t pointerCount = nodePointerVector ? usedBytes / sizeof(std::uint64_t) : 0;
      if (recordCount > maxUnitNodeRecords
          || compactRecordCount > maxUnitNodeRecords
          || pointerCount > maxUnitNodeRecords)
        continue;
      const std::size_t readablePrecheckBytes =
        nodeRecordVector
          ? recordSize * minActiveUnitRecords
          : (compactNodeRecordVector
              ? compactRecordSize * minRemasteredSnapshotUnitRecords
              : sizeof(std::uint64_t) * minActiveUnitRecords);
      const RuntimeMemoryRegion* beginRegion = findUsableUnitStorageRegion(
        regions,
        begin,
        std::min<std::size_t>(usedBytes, readablePrecheckBytes),
        executablePath,
        targetImageBase,
        includeTargetImageMappedRegions);
      bool readablePrecheck = beginRegion != nullptr;
      if (readablePrecheck && nodePointerVector)
      {
        const std::size_t pointerPrecheckBytes =
          std::min<std::size_t>(usedBytes, sizeof(std::uint64_t) * 32);
        RuntimeMemoryReadResult pointerPrecheck =
          readProcessMemory(processId, begin, pointerPrecheckBytes);
        readablePrecheck =
          pointerPrecheck.success
          && pointerPrecheck.bytesRead >= sizeof(std::uint64_t) * minActiveUnitRecords
          && countReadableDynamicPointers(
               pointerPrecheck.bytes,
               regions,
               minActiveUnitRecords,
               32) >= minActiveUnitRecords;
      }
      rememberUnitNodeVectorSample(
        diagnostics,
        baseAddress + offset,
        begin,
        end,
        capacity,
        nodeRecordVector || compactNodeRecordVector,
        nodePointerVector,
        nodeRecordVector ? recordCount : compactRecordCount,
        pointerCount,
        readablePrecheck);
      if (!readablePrecheck)
        continue;

      Candidate candidate;
      candidate.vectorAddress = baseAddress + offset;
      candidate.begin = begin;
      candidate.end = end;
      candidate.capacity = capacity;
      candidate.usedBytes = usedBytes;
      candidate.recordCount = recordCount;
      candidate.compactRecordCount = compactRecordCount;
      candidate.pointerCount = pointerCount;
      candidate.recordVector = nodeRecordVector;
      candidate.compactRecordVector = compactNodeRecordVector;
      candidate.pointerVector = nodePointerVector;
      const std::size_t logicalCount =
        nodePointerVector
          ? pointerCount
          : (compactNodeRecordVector ? compactRecordCount : recordCount);
      const bool exactCapacity = capacity == end;
      const bool compactLiveCount = logicalCount >= minActiveUnitRecords && logicalCount <= 256;
      candidate.score =
        (nodePointerVector ? 0 : 100000)
        + (compactNodeRecordVector ? 0 : 10000)
        + (compactLiveCount ? 0 : 20000)
        + (beginRegion != nullptr && beginRegion->mappedPath.empty() ? 0 : 5000)
        + (exactCapacity ? 0 : 1000)
        + static_cast<int>(std::min<std::size_t>(logicalCount, 8192))
        + static_cast<int>(std::min<std::size_t>(capacityBytes / 1024, 8192));
      candidates.push_back(candidate);
      if (candidates.size() >= maxCollectedVectorCandidates)
        break;
    }

    std::stable_sort(
      candidates.begin(),
      candidates.end(),
      [](const Candidate& lhs, const Candidate& rhs)
      {
        if (lhs.score != rhs.score)
          return lhs.score < rhs.score;
        return lhs.vectorAddress < rhs.vectorAddress;
      });

    const std::size_t candidatesToRead =
      std::min<std::size_t>(candidates.size(), maxVectorCandidatesToRead);
    for (std::size_t candidateIndex = 0; candidateIndex < candidatesToRead; ++candidateIndex)
    {
      const Candidate& candidate = candidates[candidateIndex];
      if (timedOut(deadline))
      {
        scanTimedOut = true;
        return {};
      }

      if (candidate.pointerVector)
      {
        const std::size_t pointerBytesToRead =
          std::min<std::size_t>(candidate.usedBytes, sizeof(std::uint64_t) * 4096);
        RuntimeMemoryReadResult pointerRead =
          readProcessMemory(processId, candidate.begin, pointerBytesToRead);
        if (pointerRead.success && pointerRead.bytesRead >= sizeof(std::uint64_t) * minActiveUnitRecords)
        {
          if (countReadableDynamicPointers(
                pointerRead.bytes,
                regions,
                minActiveUnitRecords,
                256) >= minActiveUnitRecords)
          {
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
              pointerProof.vectorAddress = candidate.vectorAddress;
              pointerProof.sampledRecords = candidate.pointerCount;
              return pointerProof;
            }
          }
          else if (!candidate.recordVector && !candidate.compactRecordVector)
          {
            continue;
          }
        }
      }

      if (!candidate.recordVector)
      {
        if (!candidate.compactRecordVector)
          continue;

        const std::size_t bytesToRead =
          std::min<std::size_t>(candidate.usedBytes, compactRecordSize * 256);
        RuntimeMemoryReadResult read =
          readProcessMemory(processId, candidate.begin, bytesToRead);
        if (!read.success
            || read.bytesRead < compactRecordSize * minRemasteredSnapshotUnitRecords)
          continue;

        LiveUnitNodeProof proof = scoreCompactUnitNodeAnchorArray(
          processId,
          read.bytes,
          candidate.begin,
          0,
          regions,
          deadline,
          scanTimedOut,
          diagnostics);
        if (scanTimedOut)
          return {};
        if (proof.passed)
        {
          proof.vectorAddress = candidate.vectorAddress;
          proof.sampledRecords = candidate.compactRecordCount;
          return proof;
        }
        rememberBestUnitNodeCandidate(diagnostics, proof);
        continue;
      }

      const std::size_t bytesToRead =
        std::min<std::size_t>(candidate.usedBytes, recordSize * 256);
      RuntimeMemoryReadResult read =
        readProcessMemory(processId, candidate.begin, bytesToRead);
      if (!read.success || read.bytesRead < recordSize * minActiveUnitRecords)
        continue;

      LiveUnitNodeProof proof = scoreUnitNodeAnchorArray(
        read.bytes,
        candidate.begin,
        0,
        regions,
        deadline,
        scanTimedOut,
        diagnostics);
      if (scanTimedOut)
        return {};
      if (proof.passed)
      {
        proof.vectorAddress = candidate.vectorAddress;
        proof.sampledRecords = candidate.recordCount;
        return proof;
      }
    }

    return {};
  }

  void rememberBestCandidate(
    UnitScanDiagnostics* diagnostics,
    const LiveUnitsProof& proof,
    const std::vector<unsigned char>* bytes,
    std::size_t offset,
    std::size_t recordSize,
    const std::string& source);

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

  LiveUnitsProof scoreAnchoredCUnitArray(
    const std::vector<unsigned char>& bytes,
    std::uintptr_t baseAddress,
    std::size_t recordSize,
    const UnitRecordLayout& layout,
    const std::vector<RuntimeMemoryRegion>& regions,
    const std::chrono::steady_clock::time_point& deadline,
    bool& scanTimedOut,
    bool requireReadableSprite,
    UnitScanDiagnostics* diagnostics,
    std::size_t maxSampledRecords)
  {
    LiveUnitsProof proof;
    proof.address = baseAddress;
    proof.recordSize = recordSize;
    proof.idOffset = layout.idOffset;
    proof.positionOffset = layout.positionOffset;
    proof.hitPointsOffset = layout.hitPointsOffset;
    proof.orderOffset = layout.orderOffset;
    proof.playerOffset = layout.playerOffset;
    proof.layoutName = std::string(layout.name) + "-scr-sgUnitsMem-native";

    const std::size_t availableRecords = bytes.size() / recordSize;
    proof.sampledRecords = std::min(maxSampledRecords, availableRecords);
    for (std::size_t i = 0; i < proof.sampledRecords; ++i)
    {
      if ((i % 32) == 0 && timedOut(deadline))
      {
        scanTimedOut = true;
        return {};
      }

      if (plausibleUnitRecordWithDiagnostics(
            bytes,
            i * recordSize,
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

    proof.reason = "anchored SC:R sgUnitsMem CUnit array did not contain enough active records";
    return proof;
  }

  LiveUnitsProof proveRemasteredSgUnitsMemCUnitArray(
    int processId,
    const std::string& executablePath,
    std::size_t maxScanBytes,
    int scanTimeoutMs,
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

    const StarCraftImageSectionHints hints = starCraftImageSectionHints(targetImageBase);
    if (hints.cunitSgUnitsMemAddress == 0)
      return failedUnitsProof("unable to resolve SC:R sgUnitsMem anchor without target image base");
    if (diagnostics != nullptr)
    {
      diagnostics->sgUnitsMem.attempted = true;
      diagnostics->sgUnitsMem.descriptorAddress = hints.cunitSgUnitsMemAddress;
    }

    constexpr std::size_t descriptorBytes = 24;
    RuntimeMemoryReadResult descriptor =
      readProcessMemory(processId, hints.cunitSgUnitsMemAddress, descriptorBytes);
    if (diagnostics != nullptr)
    {
      diagnostics->sgUnitsMem.descriptorRead =
        descriptor.success && descriptor.bytesRead >= descriptorBytes;
      diagnostics->sgUnitsMem.descriptorReadReason = descriptor.reason;
    }
    if (!descriptor.success || descriptor.bytesRead < descriptorBytes)
      return failedUnitsProof(descriptor.reason.empty()
        ? "unable to read SC:R sgUnitsMem descriptor"
        : descriptor.reason);

    const std::uintptr_t nativeBase =
      static_cast<std::uintptr_t>(readU64(descriptor.bytes, 0));
    const std::uint64_t recordCount = readU64(descriptor.bytes, 8);
    const std::uint64_t capacity = readU64(descriptor.bytes, 16);
    if (diagnostics != nullptr)
    {
      diagnostics->sgUnitsMem.nativeBase = nativeBase;
      diagnostics->sgUnitsMem.recordCount = recordCount;
      diagnostics->sgUnitsMem.capacity = capacity;
    }
    if (nativeBase == 0 || recordCount < minActiveUnitRecords || recordCount > 8192)
    {
      if (diagnostics != nullptr)
        diagnostics->sgUnitsMem.rejectionReason =
          "SC:R sgUnitsMem descriptor did not expose a plausible live CUnit array";
      return failedUnitsProof(
        "SC:R sgUnitsMem descriptor did not expose a plausible live CUnit array");
    }

    constexpr std::size_t nativeRecordSize = 0x1e0;
    constexpr std::size_t maxAnchoredRecords = 4096;
    const std::size_t recordsToRead = static_cast<std::size_t>(
      std::min<std::uint64_t>(recordCount, maxAnchoredRecords));
    const std::size_t requestedBytes =
      std::min(maxScanBytes, recordsToRead * nativeRecordSize);
    if (requestedBytes < nativeRecordSize * minActiveUnitRecords)
    {
      if (diagnostics != nullptr)
        diagnostics->sgUnitsMem.rejectionReason =
          "SC:R sgUnitsMem read budget is too small for unit proof";
      return failedUnitsProof("SC:R sgUnitsMem read budget is too small for unit proof");
    }

    const RuntimeMemoryRegion* readableRegion =
      findReadableRegion(regions.regions, nativeBase, nativeRecordSize * minActiveUnitRecords);
    if (diagnostics != nullptr && readableRegion != nullptr)
    {
      diagnostics->sgUnitsMem.regionFound = true;
      diagnostics->sgUnitsMem.regionAddress = readableRegion->address;
      diagnostics->sgUnitsMem.regionSize = readableRegion->size;
      diagnostics->sgUnitsMem.regionReadable = readableRegion->readable;
      diagnostics->sgUnitsMem.regionWritable = readableRegion->writable;
      diagnostics->sgUnitsMem.regionExecutable = readableRegion->executable;
      diagnostics->sgUnitsMem.regionTargetExecutable =
        sameMappedFile(readableRegion->mappedPath, executablePath);
      diagnostics->sgUnitsMem.regionUserTag = readableRegion->userTag;
      diagnostics->sgUnitsMem.regionShareMode = readableRegion->shareMode;
      diagnostics->sgUnitsMem.regionMappedPath = readableRegion->mappedPath;

      const std::size_t prefixBytesToRead = std::min<std::size_t>(requestedBytes, 256);
      RuntimeMemoryReadResult prefixRead =
        readProcessMemory(processId, nativeBase, prefixBytesToRead);
      if (prefixRead.success)
      {
        diagnostics->sgUnitsMem.prefixBytesRead = prefixRead.bytesRead;
        diagnostics->sgUnitsMem.prefixNonZeroBytes =
          countNonZeroBytes(prefixRead.bytes, prefixRead.bytes.size());
        std::array<bool, 256> seenBytes {};
        for (unsigned char byte : prefixRead.bytes)
          seenBytes[byte] = true;
        diagnostics->sgUnitsMem.prefixDistinctBytes =
          static_cast<std::size_t>(std::count(seenBytes.begin(), seenBytes.end(), true));
        diagnostics->sgUnitsMem.prefixPointerWords =
          countReadablePointerWords(prefixRead.bytes, regions.regions, prefixRead.bytes.size());
        diagnostics->sgUnitsMem.prefixHex = unitBytesHexPrefix(prefixRead.bytes, 64);
      }
    }

    const RuntimeMemoryRegion* containingRegion =
      readableRegion != nullptr
        && usableUnitStorageRegion(*readableRegion, executablePath, targetImageBase)
          ? readableRegion
          : nullptr;
    if (diagnostics != nullptr)
      diagnostics->sgUnitsMem.usableStorage = containingRegion != nullptr;
    if (containingRegion == nullptr)
    {
      if (diagnostics != nullptr)
      {
        if (readableRegion == nullptr)
        {
          diagnostics->sgUnitsMem.rejectionReason =
            "SC:R sgUnitsMem native CUnit base is not readable";
        }
        else if (readableRegion->executable)
        {
          diagnostics->sgUnitsMem.rejectionReason =
            "SC:R sgUnitsMem native CUnit base is in executable memory";
        }
        else if (!readableRegion->writable)
        {
          diagnostics->sgUnitsMem.rejectionReason =
            "SC:R sgUnitsMem native CUnit base is not writable";
        }
        else if (fileBackedNonTargetRegion(*readableRegion, executablePath))
        {
          diagnostics->sgUnitsMem.rejectionReason =
            "SC:R sgUnitsMem native CUnit base is in non-StarCraft file-backed mapping";
        }
        else
        {
          diagnostics->sgUnitsMem.rejectionReason =
            "SC:R sgUnitsMem native CUnit base is not accepted as StarCraft unit storage";
        }
      }
      return failedUnitsProof(
        "SC:R sgUnitsMem native CUnit base is not writable StarCraft unit storage");
    }

    const std::uintptr_t regionEnd = containingRegion->address + containingRegion->size;
    const std::size_t regionBytes =
      regionEnd > nativeBase ? static_cast<std::size_t>(regionEnd - nativeBase) : 0;
    const std::size_t bytesToRead = std::min(requestedBytes, regionBytes);
    if (bytesToRead < nativeRecordSize * minActiveUnitRecords)
    {
      if (diagnostics != nullptr)
        diagnostics->sgUnitsMem.rejectionReason =
          "SC:R sgUnitsMem native CUnit region is too small for proof";
      return failedUnitsProof("SC:R sgUnitsMem native CUnit region is too small for proof");
    }

    RuntimeMemoryReadResult read = readProcessMemory(processId, nativeBase, bytesToRead);
    if (!read.success || read.bytesRead < nativeRecordSize * minActiveUnitRecords)
    {
      if (diagnostics != nullptr)
      {
        diagnostics->sgUnitsMem.rejectionReason = read.reason.empty()
          ? "unable to read SC:R sgUnitsMem native CUnit records"
          : read.reason;
      }
      return failedUnitsProof(read.reason.empty()
        ? "unable to read SC:R sgUnitsMem native CUnit records"
        : read.reason);
    }

    if (diagnostics != nullptr)
    {
      if (containingRegion->writable)
        ++diagnostics->readableWritableRegions;
      else
        ++diagnostics->scannedReadableOnlyRegions;
      ++diagnostics->scannedRegions;
      diagnostics->scannedBytes += read.bytesRead;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(scanTimeoutMs);
    for (const UnitRecordLayout& layout : unitRecordLayouts)
    {
      bool scanTimedOut = false;
      LiveUnitsProof proof = scoreAnchoredCUnitArray(
        read.bytes,
        nativeBase,
        nativeRecordSize,
        layout,
        regions.regions,
        deadline,
        scanTimedOut,
        false,
        diagnostics,
        recordsToRead);
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
      rememberBestCandidate(diagnostics, proof, &read.bytes, 0, nativeRecordSize, "scr-sgUnitsMem-anchor");
    }

    if (diagnostics != nullptr)
      diagnostics->sgUnitsMem.rejectionReason =
        "SC:R sgUnitsMem anchor was readable but contained no BWAPI-compatible active CUnit records";
    return failedUnitsProof("SC:R sgUnitsMem anchor was readable but contained no BWAPI-compatible active CUnit records");
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
        for (std::size_t recordOffset = firstAlignedOffset(baseAddress, alignof(std::uint64_t));
             recordOffset + recordSize <= bytes.size();
             recordOffset += alignof(std::uint64_t))
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

    for (std::size_t offset = firstAlignedOffset(baseAddress, alignof(std::uint64_t));
         offset + sizeof(std::uint64_t) * 3 <= bytes.size();
         offset += alignof(std::uint64_t))
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
            const std::uintptr_t firstPointer =
              pointerRead.bytesRead >= sizeof(std::uint64_t)
                ? static_cast<std::uintptr_t>(readU64(pointerRead.bytes, 0))
                : 0;
            const std::uintptr_t secondPointer =
              pointerRead.bytesRead >= sizeof(std::uint64_t) * 2
                ? static_cast<std::uintptr_t>(readU64(pointerRead.bytes, sizeof(std::uint64_t)))
                : 0;
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
              rememberUnitPointerArraySample(
                diagnostics,
                baseAddress + offset,
                begin,
                end,
                capacity,
                pointerCount,
                readablePointers,
                recordSnapshots,
                firstPointer,
                secondPointer,
                regions);
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
      {
        rememberUnitScanRegionSample(
          diagnostics,
          region,
          executablePath,
          targetImageBase,
          "unit-array",
          "skip",
          !region.readable ? "not-readable" : "too-small");
        continue;
      }
      if (fileBackedNonTargetRegion(region, executablePath))
      {
        rememberUnitScanRegionSample(
          diagnostics,
          region,
          executablePath,
          targetImageBase,
          "unit-array",
          "skip",
          "file-backed-non-target");
        continue;
      }
      if (region.executable)
      {
        if (diagnostics != nullptr)
          ++diagnostics->executableReadableRegions;
        rememberUnitScanRegionSample(
          diagnostics,
          region,
          executablePath,
          targetImageBase,
          "unit-array",
          "skip",
          "executable");
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
        rememberUnitScanRegionSample(
          diagnostics,
          region,
          executablePath,
          targetImageBase,
          "unit-array",
          "skip",
          "target-text-image-mapping");
        continue;
      }
      if (shouldSkipImageMappedRegion(region, executablePath, includeImageMappedRegions))
      {
        if (diagnostics != nullptr)
          ++diagnostics->skippedImageMappedRegions;
        rememberUnitScanRegionSample(
          diagnostics,
          region,
          executablePath,
          targetImageBase,
          "unit-array",
          "skip",
          "target-image-readonly-mapping");
        continue;
      }
      if (!region.writable)
      {
        if (diagnostics != nullptr)
          ++diagnostics->readableOnlyRegions;
        if (!includeReadableOnlyRegions)
        {
          rememberUnitScanRegionSample(
            diagnostics,
            region,
            executablePath,
            targetImageBase,
            "unit-array",
            "skip",
            "readable-only-disabled");
          continue;
        }
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
      {
        rememberUnitScanRegionSample(
          diagnostics,
          region,
          executablePath,
          targetImageBase,
          "unit-array",
          "read-failed",
          read.reason.empty() ? "short-read" : read.reason,
          read.bytesRead);
        continue;
      }
      if (diagnostics != nullptr)
      {
        ++diagnostics->scannedRegions;
        diagnostics->scannedBytes += read.bytesRead;
      }
      rememberUnitScanRegionSample(
        diagnostics,
        region,
        executablePath,
        targetImageBase,
        "unit-array",
        "scan",
        "read",
        read.bytesRead);

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

  void rememberBestCandidate(
    UnitScanDiagnostics* diagnostics,
    const LiveUnitsProof& proof,
    const std::vector<unsigned char>* bytes,
    std::size_t offset,
    std::size_t recordSize,
    const std::string& source);

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

  LiveUnitNodeProof proveDynamicUnitNodeAnchors(
    int processId,
    const std::string& executablePath,
    const std::vector<RuntimeMemoryRegion>& regions,
    std::uintptr_t targetImageBase,
    std::size_t maxScanBytes,
    bool includeTargetImageMappedRegions,
    const std::chrono::steady_clock::time_point& deadline,
    UnitScanDiagnostics* diagnostics)
  {
    if (diagnostics != nullptr)
      diagnostics->dynamicScanReason.clear();

    struct RegionSnapshot
    {
      RuntimeMemoryRegion region;
      std::vector<unsigned char> before;
    };

    std::vector<RuntimeMemoryRegion> candidateRegions;
    candidateRegions.reserve(regions.size());
    for (const RuntimeMemoryRegion& region : regions)
    {
      if (!usableUnitStorageRegion(
            region,
            executablePath,
            targetImageBase,
            includeTargetImageMappedRegions))
        continue;
      if (region.size < 0x58 * minActiveUnitRecords)
        continue;
      candidateRegions.push_back(region);
    }

    std::stable_sort(
      candidateRegions.begin(),
      candidateRegions.end(),
      [&](const RuntimeMemoryRegion& lhs, const RuntimeMemoryRegion& rhs)
      {
        const int lhsPriority = dynamicUnitScanRegionPriority(lhs, executablePath, targetImageBase);
        const int rhsPriority = dynamicUnitScanRegionPriority(rhs, executablePath, targetImageBase);
        if (lhsPriority != rhsPriority)
          return lhsPriority < rhsPriority;
        if (lhs.size != rhs.size)
          return lhs.size < rhs.size;
        return lhs.address < rhs.address;
      });

    constexpr std::size_t maxRegionBytes = 8 * 1024 * 1024;
    constexpr std::size_t dynamicProbeMaxBytes = 96 * 1024 * 1024;
    const std::size_t scanBudget = std::min(maxScanBytes, dynamicProbeMaxBytes);
    std::vector<RegionSnapshot> snapshots;
    std::size_t sampledBytes = 0;
    for (const RuntimeMemoryRegion& region : candidateRegions)
    {
      if (sampledBytes >= scanBudget || timedOut(deadline))
        break;

      const std::size_t bytesToRead =
        std::min(region.size, std::min(maxRegionBytes, scanBudget - sampledBytes));
      RuntimeMemoryReadResult read = readProcessMemory(processId, region.address, bytesToRead);
      if (!read.success || read.bytesRead < 0x58 * minActiveUnitRecords)
        continue;

      if (diagnostics != nullptr)
      {
        ++diagnostics->dynamicSampledRegions;
        diagnostics->dynamicSampledBytes += read.bytesRead;
      }
      sampledBytes += read.bytesRead;
      snapshots.push_back(RegionSnapshot { region, std::move(read.bytes) });
    }

    if (snapshots.empty())
    {
      if (diagnostics != nullptr)
        diagnostics->dynamicScanReason = "no readable writable StarCraft live-memory regions were sampled";
      return failedUnitNodeProof("no changing SC:R unit-node live-memory region was sampled");
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    constexpr std::size_t maxChangedRangesPerRegion = 64;
    constexpr std::size_t maxDynamicWindows = 96;
    constexpr std::size_t windowRadius = 64 * 1024;
    std::size_t windowsScored = 0;
    std::unordered_set<std::uintptr_t> scoredWindowStarts;
    for (const RegionSnapshot& snapshot : snapshots)
    {
      if (timedOut(deadline))
      {
        if (diagnostics != nullptr)
        {
          diagnostics->timedOut = true;
          diagnostics->dynamicScanReason = "dynamic SC:R unit-node scan timed out before proof";
        }
        return failedUnitNodeProof("dynamic SC:R unit-node scan timed out before proof");
      }

      RuntimeMemoryReadResult afterRead =
        readProcessMemory(processId, snapshot.region.address, snapshot.before.size());
      if (!afterRead.success || afterRead.bytesRead != snapshot.before.size())
        continue;

      std::size_t changedBytes = 0;
      const std::vector<std::pair<std::size_t, std::size_t>> changedRanges =
        collectChangedRanges(snapshot.before, afterRead.bytes, changedBytes, maxChangedRangesPerRegion);
      rememberDynamicRegionSample(
        diagnostics,
        snapshot.region,
        executablePath,
        targetImageBase,
        afterRead.bytesRead,
        changedBytes,
        changedRanges);
      if (changedBytes == 0)
        continue;

      if (diagnostics != nullptr)
      {
        ++diagnostics->dynamicChangedRegions;
        diagnostics->dynamicChangedBytes += changedBytes;
      }

      for (const auto& range : changedRanges)
      {
        if (windowsScored >= maxDynamicWindows || timedOut(deadline))
          break;

        const std::size_t windowStart =
          range.first > windowRadius ? range.first - windowRadius : 0;
        const std::size_t windowEnd = std::min(
          afterRead.bytes.size(),
          range.first + range.second + windowRadius);
        if (windowEnd <= windowStart || windowEnd - windowStart < 0x58 * minActiveUnitRecords)
          continue;

        const std::uintptr_t windowAddress = snapshot.region.address + windowStart;
        if (!scoredWindowStarts.insert(windowAddress).second)
          continue;

        std::vector<unsigned char> beforeWindow(
          snapshot.before.begin() + static_cast<std::vector<unsigned char>::difference_type>(windowStart),
          snapshot.before.begin() + static_cast<std::vector<unsigned char>::difference_type>(windowEnd));
        std::vector<unsigned char> afterWindow(
          afterRead.bytes.begin() + static_cast<std::vector<unsigned char>::difference_type>(windowStart),
          afterRead.bytes.begin() + static_cast<std::vector<unsigned char>::difference_type>(windowEnd));
        collectDynamicFieldCandidateSamples(
          diagnostics,
          beforeWindow,
          afterWindow,
          windowAddress,
          regions);

        ++windowsScored;
        if (diagnostics != nullptr)
          ++diagnostics->dynamicWindowsScored;

        bool scanTimedOut = false;
        LiveUnitNodeProof compactProof = proveCompactUnitNodeAnchorsInBytes(
          processId,
          afterWindow,
          windowAddress,
          regions,
          deadline,
          scanTimedOut,
          diagnostics);
        if (scanTimedOut)
        {
          if (diagnostics != nullptr)
          {
            diagnostics->timedOut = true;
            diagnostics->dynamicScanReason = "dynamic compact SC:R unit-node scan timed out before proof";
          }
          return failedUnitNodeProof("dynamic compact SC:R unit-node scan timed out before proof");
        }
        if (compactProof.passed)
          return compactProof;
        rememberBestUnitNodeCandidate(diagnostics, compactProof);

        LiveUnitNodeProof compactGraphProof = proveCompactUnitNodeGraphsInBytes(
          processId,
          afterWindow,
          windowAddress,
          regions,
          deadline,
          scanTimedOut,
          diagnostics);
        if (scanTimedOut)
        {
          if (diagnostics != nullptr)
          {
            diagnostics->timedOut = true;
            diagnostics->dynamicScanReason = "dynamic compact SC:R unit-node graph scan timed out before proof";
          }
          return failedUnitNodeProof("dynamic compact SC:R unit-node graph scan timed out before proof");
        }
        if (compactGraphProof.passed)
          return compactGraphProof;
        rememberBestUnitNodeCandidate(diagnostics, compactGraphProof);

        LiveUnitNodeProof graphProof = proveUnitNodeGraphsInBytes(
          processId,
          afterWindow,
          windowAddress,
          regions,
          deadline,
          scanTimedOut,
          diagnostics);
        if (scanTimedOut)
        {
          if (diagnostics != nullptr)
          {
            diagnostics->timedOut = true;
            diagnostics->dynamicScanReason = "dynamic SC:R unit-node graph scan timed out before proof";
          }
          return failedUnitNodeProof("dynamic SC:R unit-node graph scan timed out before proof");
        }
        if (graphProof.passed)
          return graphProof;
        rememberBestUnitNodeCandidate(diagnostics, graphProof);

        LiveUnitNodeProof collectedProof = collectBucketVerifiedUnitNodesInBytes(
          processId,
          afterWindow,
          windowAddress,
          regions,
          deadline,
          scanTimedOut,
          diagnostics);
        if (scanTimedOut)
        {
          if (diagnostics != nullptr)
          {
            diagnostics->timedOut = true;
            diagnostics->dynamicScanReason = "dynamic SC:R bucket-verified unit-node scan timed out before proof";
          }
          return failedUnitNodeProof("dynamic SC:R bucket-verified unit-node scan timed out before proof");
        }
        if (collectedProof.passed)
          return collectedProof;
        rememberBestUnitNodeCandidate(diagnostics, collectedProof);

        LiveUnitNodeProof anchorProof = proveUnitNodeAnchorsInBytes(
          afterWindow,
          windowAddress,
          regions,
          deadline,
          scanTimedOut,
          diagnostics);
        if (scanTimedOut)
        {
          if (diagnostics != nullptr)
          {
            diagnostics->timedOut = true;
            diagnostics->dynamicScanReason = "dynamic SC:R unit-node anchor scan timed out before proof";
          }
          return failedUnitNodeProof("dynamic SC:R unit-node anchor scan timed out before proof");
        }
        if (anchorProof.passed)
          return anchorProof;
        rememberBestUnitNodeCandidate(diagnostics, anchorProof);

        LiveUnitNodeProof vectorProof = proveUnitNodeVectorsInBytes(
          processId,
          afterWindow,
          windowAddress,
          regions,
          executablePath,
          targetImageBase,
          includeTargetImageMappedRegions,
          deadline,
          scanTimedOut,
          diagnostics);
        if (scanTimedOut)
        {
          if (diagnostics != nullptr)
          {
            diagnostics->timedOut = true;
            diagnostics->dynamicScanReason = "dynamic SC:R unit-node vector scan timed out before proof";
          }
          return failedUnitNodeProof("dynamic SC:R unit-node vector scan timed out before proof");
        }
        if (vectorProof.passed)
          return vectorProof;
        rememberBestUnitNodeCandidate(diagnostics, vectorProof);
      }
    }

    if (diagnostics != nullptr)
    {
      diagnostics->dynamicScanReason =
        diagnostics->dynamicChangedRegions == 0
          ? "no changing readable writable StarCraft live-memory regions were observed"
          : "changing live-memory regions did not contain a BWAPI-facing SC:R unit-node proof";
    }
    return failedUnitNodeProof("no active SC:R unit-node anchor found in changing live-memory regions");
  }

  LiveUnitNodeProof proveLiveUnitNodeAnchors(
    int processId,
    const std::string& executablePath,
    std::size_t maxScanBytes,
    int scanTimeoutMs,
    bool includeTargetImageMappedRegions,
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
    LiveUnitNodeProof dynamicProof = proveDynamicUnitNodeAnchors(
      processId,
      executablePath,
      regions.regions,
      targetImageBase,
      maxScanBytes,
      includeTargetImageMappedRegions,
      deadline,
      diagnostics);
    if (dynamicProof.passed)
      return dynamicProof;
    rememberBestUnitNodeCandidate(diagnostics, dynamicProof);

    const bool scanStaticSectionsFirst =
      (hints.bssAddress != 0 && hints.bssSize >= 0x58 * minActiveUnitRecords)
      || (hints.commonAddress != 0 && hints.commonSize >= 0x58 * minActiveUnitRecords);
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
      if (const RuntimeMemoryRegion* sectionRegion =
            findReadableRegion(regions.regions, section.first, read.bytesRead))
      {
        rememberUnitScanRegionSample(
          diagnostics,
          *sectionRegion,
          executablePath,
          targetImageBase,
          "unit-node",
          "scan",
          "priority-image-section",
          read.bytesRead);
      }

      bool scanTimedOut = false;
      LiveUnitNodeProof compactProof = proveCompactUnitNodeAnchorsInBytes(
        processId,
        read.bytes,
        section.first,
        regions.regions,
        deadline,
        scanTimedOut,
        diagnostics);
      if (scanTimedOut)
        return failedUnitNodeProof("compact SC:R unit-node section scan timed out before proof");
      if (compactProof.passed)
        return compactProof;
      rememberBestUnitNodeCandidate(diagnostics, compactProof);

      LiveUnitNodeProof compactGraphProof = proveCompactUnitNodeGraphsInBytes(
        processId,
        read.bytes,
        section.first,
        regions.regions,
        deadline,
        scanTimedOut,
        diagnostics);
      if (scanTimedOut)
        return failedUnitNodeProof("compact SC:R unit-node linked graph section scan timed out before proof");
      if (compactGraphProof.passed)
        return compactGraphProof;
      rememberBestUnitNodeCandidate(diagnostics, compactGraphProof);

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
        executablePath,
        targetImageBase,
        includeTargetImageMappedRegions,
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
      if (!usableUnitStorageRegion(
            region,
            executablePath,
            targetImageBase,
            includeTargetImageMappedRegions))
      {
        rememberUnitScanRegionSample(
          diagnostics,
          region,
          executablePath,
          targetImageBase,
          "unit-node",
          "skip",
          "not-usable-unit-storage");
        continue;
      }
      if (region.size < 0x58 * minActiveUnitRecords)
      {
        rememberUnitScanRegionSample(
          diagnostics,
          region,
          executablePath,
          targetImageBase,
          "unit-node",
          "skip",
          "too-small");
        continue;
      }
      if (scanned >= maxScanBytes)
        return failedUnitNodeProof("no active SC:R unit-node anchor found before scan byte limit");

      const std::size_t bytesToRead =
        std::min(region.size, std::min(maxRegionBytes, maxScanBytes - scanned));
      RuntimeMemoryReadResult read = readProcessMemory(processId, region.address, bytesToRead);
      if (!read.success || read.bytesRead < 0x58 * minActiveUnitRecords)
      {
        rememberUnitScanRegionSample(
          diagnostics,
          region,
          executablePath,
          targetImageBase,
          "unit-node",
          "read-failed",
          read.reason.empty() ? "short-read" : read.reason,
          read.bytesRead);
        continue;
      }
      if (diagnostics != nullptr)
      {
        ++diagnostics->unitNodeScannedRegions;
        diagnostics->unitNodeScannedBytes += read.bytesRead;
      }
      rememberUnitScanRegionSample(
        diagnostics,
        region,
        executablePath,
        targetImageBase,
        "unit-node",
        "scan",
        "read",
        read.bytesRead);

      bool scanTimedOut = false;
      LiveUnitNodeProof compactProof = proveCompactUnitNodeAnchorsInBytes(
        processId,
        read.bytes,
        region.address,
        regions.regions,
        deadline,
        scanTimedOut,
        diagnostics);
      if (scanTimedOut)
        return failedUnitNodeProof("compact SC:R unit-node scan timed out before proof");
      if (compactProof.passed)
        return compactProof;
      rememberBestUnitNodeCandidate(diagnostics, compactProof);

      LiveUnitNodeProof compactGraphProof = proveCompactUnitNodeGraphsInBytes(
        processId,
        read.bytes,
        region.address,
        regions.regions,
        deadline,
        scanTimedOut,
        diagnostics);
      if (scanTimedOut)
        return failedUnitNodeProof("compact SC:R unit-node linked graph scan timed out before proof");
      if (compactGraphProof.passed)
        return compactGraphProof;
      rememberBestUnitNodeCandidate(diagnostics, compactGraphProof);

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
        executablePath,
        targetImageBase,
        includeTargetImageMappedRegions,
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
    constexpr std::size_t compactRecordSize = 0x28;
    constexpr std::size_t maxSnapshotRecords = 256;
    for (std::uintptr_t candidateAddress : candidateAddresses)
    {
      if (timedOut(deadline))
        return failedUnitNodeProof("explicit SC:R unit-node candidate scan timed out before proof");

      const RuntimeMemoryRegion* compactContainingRegion =
        findReadableRegion(
          regions.regions,
          candidateAddress,
          compactRecordSize * minRemasteredSnapshotUnitRecords);
      if (compactContainingRegion != nullptr)
      {
        const std::uintptr_t regionEnd =
          compactContainingRegion->address + compactContainingRegion->size;
        const std::size_t regionBytes =
          regionEnd > candidateAddress
            ? static_cast<std::size_t>(regionEnd - candidateAddress)
            : 0;
        const std::size_t bytesToRead =
          std::min(regionBytes, compactRecordSize * maxSnapshotRecords);
        if (bytesToRead >= compactRecordSize * minRemasteredSnapshotUnitRecords)
        {
          RuntimeMemoryReadResult read = readProcessMemory(processId, candidateAddress, bytesToRead);
          if (read.success
              && read.bytesRead >= compactRecordSize * minRemasteredSnapshotUnitRecords)
          {
            bool scanTimedOut = false;
            LiveUnitNodeProof compactProof = scoreCompactUnitNodeAnchorArray(
              processId,
              read.bytes,
              candidateAddress,
              0,
              regions.regions,
              deadline,
              scanTimedOut,
              diagnostics);
            if (scanTimedOut)
              return failedUnitNodeProof("explicit compact SC:R unit-node candidate scan timed out before proof");
            if (compactProof.passed)
              return compactProof;
            rememberBestUnitNodeCandidate(diagnostics, compactProof);

            LiveUnitNodeProof compactGraphProof = scoreCompactLinkedUnitNodeGraph(
              processId,
              candidateAddress,
              regions.regions,
              deadline,
              scanTimedOut,
              diagnostics);
            if (scanTimedOut)
              return failedUnitNodeProof("explicit compact SC:R unit-node graph candidate scan timed out before proof");
            if (compactGraphProof.passed)
              return compactGraphProof;
            rememberBestUnitNodeCandidate(diagnostics, compactGraphProof);
          }
        }
      }

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
          if (!plausibleRemasteredUnitTypeHint(typeHint))
            typeHint = readU16(secondaryRead.bytes, 0x20);

          if ((rawPlayer < 12 || rawPlayer == 255) && plausibleRemasteredUnitTypeHint(typeHint))
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
    if (metadataPlayer >= 12 || !plausibleRemasteredUnitTypeHint(metadataType))
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

    for (std::size_t offset = firstAlignedOffset(baseAddress, alignof(std::uint64_t));
         offset + recordSize <= bytes.size();
         offset += alignof(std::uint64_t))
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

    for (std::size_t offset = firstAlignedOffset(baseAddress, alignof(std::uint64_t));
         offset + recordSize <= bytes.size();
         offset += alignof(std::uint64_t))
    {
      if ((offset % (16 * 1024)) == 0 && timedOut(deadline))
      {
        scanTimedOut = true;
        return {};
      }
      if (!plausibleUnitNodeAnchorFields(bytes, offset))
        continue;
      const bool readableCandidate =
        plausibleUnitNodeAnchorRecord(bytes, offset, regions);
      rememberUnitNodeFieldSample(
        diagnostics,
        bytes,
        offset,
        baseAddress + offset,
        regions,
        "unit-node-0x58",
        readableCandidate ? std::string() : "graph seed fields matched but link/sprite/secondary pointers were not readable");
      if (!readableCandidate)
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
        : proveLiveUnitNodeAnchors(
            processId,
            executablePath,
            maxScanBytes,
            scanTimeoutMs,
            false,
            diagnostics);
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
    const bool usesTaggedHandles = std::any_of(
      records.begin(),
      records.end(),
      [](const RemasteredUnitSnapshotRecord& record)
      {
        return record.taggedHandleDerived;
      });
    const bool usesCompactFields = nodeProof.recordSize == 0x28;
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
    proof.positionOffset = usesCompactFields ? 0x10 : 0x24;
    proof.hitPointsOffset = hitPointsResolved ? 12 : 0;
    proof.orderOffset = usesCompactFields ? 0 : 0x30;
    proof.sampledRecords = availableRecords;
    proof.activeRecords = records.size();
    proof.derivedSnapshot = true;
    proof.hitPointsResolved = hitPointsResolved;
    proof.layoutName = usesCompactFields
      ? (usesTaggedHandles ? "scr-tagged-compact-unit-node-snapshot" : "scr-compact-unit-node-object-graph")
      : "scr-unit-node-object-graph";
    proof.idSource = usesMetadataFields
      ? (usesCompactFields
          ? (usesTaggedHandles
              ? "stable-node-handle|tagged-compact-handle"
              : "stable-node-handle|compact-node sprite metadata")
          : "stable-node-handle|unit-node+0x40 metadata")
      : "stable-node-handle";
    proof.positionSource = usesCompactFields
      ? (usesTaggedHandles ? "tagged-compact-node+0x10|8 packed-xy" : "unit-node+0x10|8 compact-xy")
      : "unit-node+36|4";
    proof.hitPointsSource =
      hitPointsResolved
        ? (usesCompactFields
            ? "sprite+0x80 hp-raw"
            : (usesMetadataFields ? "metadata+0x1a compact-hp-byte -> bwapi-hp-raw" : "secondary+0x1a compact-hp-byte -> bwapi-hp-raw"))
        : "";
    proof.orderSource = usesCompactFields
      ? (usesTaggedHandles ? "tagged-compact-node:adapter-default-order" : "compact-unit-node:adapter-default-order")
      : "unit-node+48|2";
    proof.playerSource = usesCompactFields
      ? (usesTaggedHandles
          ? "tagged-compact-node:adapter-default-player"
          : "compact-unit-node:sprite+0x6c|4-or-sprite+0xc0|1")
      : (usesMetadataFields ? "unit-node+0x40 metadata+0xc0|1" : "unit-node+0x50 secondary+0x14|1");
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

    // BWAPI UnitTypes are grouped by race for the classic ids. Wider SC:R
    // internal hints intentionally fall through to Unknown unless a stable
    // classic mapping is known.
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
    proof.currentProcessReplay = replayLaunchDetected;
    proof.activeMatchMetadata = activeMatchReady && playerProof.passed;
    proof.source = proof.currentProcessReplay
      ? "current-process-replay"
      : (proof.activeMatchMetadata ? "active-match-live-metadata" : "live-map-metadata");
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
    const std::string& executablePath,
    std::uintptr_t targetImageBase,
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

    const std::size_t pointerAlignment = layout.recordSize == bulletRecordLayouts.front().recordSize
      ? 4
      : 8;
    const std::uint64_t sprite = readPointerLike(bytes, offset + layout.spriteOffset);
    if (!readableLiveDataObjectPointerValue(
          regions,
          sprite,
          16,
          pointerAlignment,
          executablePath,
          targetImageBase))
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
    if (!readableLiveDataObjectPointerValue(
          regions,
          sourceUnit,
          16,
          pointerAlignment,
          executablePath,
          targetImageBase)
        && !readableLiveDataObjectPointerValue(
          regions,
          targetUnit,
          16,
          pointerAlignment,
          executablePath,
          targetImageBase))
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
    const std::string& executablePath,
    std::uintptr_t targetImageBase,
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
            executablePath,
            targetImageBase,
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
    const std::string& executablePath,
    std::uintptr_t targetImageBase,
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
              executablePath,
              targetImageBase,
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
          executablePath,
          targetImageBase,
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
    const std::string& executablePath,
    int scanTimeoutMs)
  {
    if (candidateAddresses.empty())
      return failedBulletDataProof("no explicit bullet candidate address was provided");

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
          executablePath,
          targetImageBase,
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
        executablePath,
        targetImageBase,
        deadline,
        scanTimedOut);
      if (scanTimedOut)
        return failedBulletDataProof("bullet array scan timed out before proof");
      if (proof.passed)
        return proof;
    }

    return failedBulletDataProof("no active BWAPI-compatible bullet array candidate found");
  }

  BulletDataProof proveStaticBulletAdapterCompatibility(const std::string& executablePath)
  {
    BulletDataProof proof;
    const ExecutableAnchorScan bulletAdapterScan = scanExecutableAnchors(
      executablePath,
      {
        "eud_cbullet_array_adapter_t",
        "eud_cobject_array_adapter_tI7CBullet",
        "CBullet: Damage"
      });

    proof.resolvedAnchors = bulletAdapterScan.found;
    proof.missingAnchors = bulletAdapterScan.missing;
    proof.staticAdapterAnchorsResolved =
      bulletAdapterScan.readable
      && std::find(
        bulletAdapterScan.found.begin(),
        bulletAdapterScan.found.end(),
        "eud_cbullet_array_adapter_t") != bulletAdapterScan.found.end()
      && std::find(
        bulletAdapterScan.found.begin(),
        bulletAdapterScan.found.end(),
        "eud_cobject_array_adapter_tI7CBullet") != bulletAdapterScan.found.end();

    if (!proof.staticAdapterAnchorsResolved)
    {
      proof.reason = bulletAdapterScan.reason.empty()
        ? "static SC:R bullet adapter anchors were not resolved"
        : bulletAdapterScan.reason;
      return proof;
    }

    const BulletRecordLayout& layout = bulletRecordLayouts[1];
    proof.recordSize = layout.recordSize;
    proof.positionOffset = layout.positionOffset;
    proof.velocityOffset = layout.velocityOffset;
    proof.sourceUnitOffset = layout.sourceUnitOffset;
    proof.targetOffset = layout.targetUnitOffset;
    proof.layoutName = layout.name;
    proof.reason =
      "static SC:R bullet adapter anchors resolved compatibility layout, "
      "but no active live bullet records were observed for behavior proof";
    return proof;
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
  constexpr std::array<std::size_t, 5> rawTurnBufferPrefixCounterOffsets = {
    0x10, 0x04, 0x08, 0x20, 0x40
  };
  constexpr std::array<unsigned char, 23> commonActionOpcodes = {
    0x08, 0x0a, 0x0b, 0x0c, 0x0e, 0x0f, 0x10, 0x11,
    0x13, 0x14, 0x18, 0x1a, 0x1e, 0x20, 0x21, 0x23,
    0x25, 0x26, 0x27, 0x2c, 0x30, 0x36, 0x58
  };

  bool isCommonActionOpcode(unsigned char byte)
  {
    return std::find(commonActionOpcodes.begin(), commonActionOpcodes.end(), byte)
      != commonActionOpcodes.end();
  }

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

  std::size_t countPrefixPointerWords(const std::vector<unsigned char>& bytes)
  {
    std::size_t count = 0;
    for (std::size_t cursor = 0; cursor + sizeof(std::uint64_t) <= bytes.size(); cursor += sizeof(std::uint64_t))
    {
      if (looksLikeUserSpacePointer(readU64(bytes, cursor)))
        ++count;
    }
    return count;
  }

  std::size_t countPrefixSmallIntegerWords(const std::vector<unsigned char>& bytes)
  {
    std::size_t count = 0;
    for (std::size_t cursor = 0; cursor + sizeof(std::uint64_t) <= bytes.size(); cursor += sizeof(std::uint64_t))
    {
      const std::uint64_t value = readU64(bytes, cursor);
      if (value != 0 && value <= 0x10000)
        ++count;
    }
    return count;
  }

  std::uint32_t byteEntropyMilli(const std::vector<unsigned char>& bytes)
  {
    if (bytes.empty())
      return 0;

    std::array<std::size_t, 256> counts = {};
    for (unsigned char byte : bytes)
      ++counts[byte];

    double entropy = 0.0;
    for (std::size_t count : counts)
    {
      if (count == 0)
        continue;
      const double probability = static_cast<double>(count) / static_cast<double>(bytes.size());
      entropy -= probability * std::log2(probability);
    }
    return static_cast<std::uint32_t>(std::llround(entropy * 1000.0));
  }

  std::string bytesHexPrefix(const std::vector<unsigned char>& bytes, std::size_t maxBytes)
  {
    std::ostringstream output;
    const std::size_t limit = std::min(bytes.size(), maxBytes);
    for (std::size_t i = 0; i < limit; ++i)
    {
      if (i > 0)
        output << ' ';
      output << byteHex(bytes[i]);
    }
    return output.str();
  }

  void populateCommandQueuePrefixDiagnostics(
    CommandQueueCandidate& candidate,
    const std::vector<unsigned char>& bytes)
  {
    candidate.prefixBytes = bytes.size();
    candidate.prefixNonZeroBytes =
      static_cast<std::size_t>(std::count_if(
        bytes.begin(),
        bytes.end(),
        [](unsigned char byte)
        {
          return byte != 0;
        }));
    std::array<bool, 256> seen = {};
    for (unsigned char byte : bytes)
      seen[byte] = true;
    candidate.prefixDistinctBytes =
      static_cast<std::size_t>(std::count(seen.begin(), seen.end(), true));
    const std::size_t usedPrefixBytes = std::min<std::size_t>(candidate.usedBytes, bytes.size());
    candidate.prefixKnownOpcodeBytes =
      static_cast<std::size_t>(std::count_if(
        bytes.begin(),
        bytes.begin() + static_cast<std::ptrdiff_t>(usedPrefixBytes),
        isCommonActionOpcode));
    candidate.prefixPointerWords = countPrefixPointerWords(bytes);
    candidate.prefixSmallIntegerWords = countPrefixSmallIntegerWords(bytes);
    candidate.prefixEntropyMilli = byteEntropyMilli(bytes);
    candidate.prefixHex = bytesHexPrefix(bytes, 16);
    candidate.firstByteHex = bytes.empty() ? "" : byteHex(bytes.front());
  }

  bool plausibleRawTurnBufferBytes(
    const std::vector<unsigned char>& bytes,
    std::size_t offset,
    std::size_t usedBytes)
  {
    if (offset + rawTurnBufferCapacity > bytes.size() || usedBytes > rawTurnBufferCapacity)
      return false;

    const unsigned char first = bytes[offset];
    const bool startsWithKnownAction =
      usedBytes > 0
      && isCommonActionOpcode(first);

    // SC:R globals contain many fixed-size pointer tables near the networking
    // state. Their first byte can look like a BW command opcode, but they are
    // not safe turn-buffer candidates. Apply this only before a live command
    // stream is present; short live opcode bursts such as 10 11 10 11 10 can
    // otherwise be misclassified as low user-space pointers.
    if (!startsWithKnownAction
        && usedBytes < sizeof(std::uint64_t)
        && looksLikeUserSpacePointer(readU64(bytes, offset)))
      return false;
    if (startsWithKnownAction
        && usedBytes > 1
        && usedBytes < sizeof(std::uint64_t)
        && looksLikeUserSpacePointer(readU64(bytes, offset)))
    {
      const bool compactOpcodeBurst = std::all_of(
        bytes.begin() + static_cast<std::ptrdiff_t>(offset),
        bytes.begin() + static_cast<std::ptrdiff_t>(offset + usedBytes),
        isCommonActionOpcode);
      if (!compactOpcodeBurst)
        return false;
    }
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

  std::string commandQueueRegionClass(
    const RuntimeMemoryRegion& region,
    const std::string& executablePath,
    const StarCraftImageSectionHints& hints)
  {
    if (sameMappedFile(region.mappedPath, executablePath))
      return regionIntersectsStarCraftRuntimeData(region, hints)
        ? "target-image-runtime-data"
        : "target-image-other";
    if (region.mappedPath.empty())
      return "private";
    return "mapped-file";
  }

  void annotateCommandQueueCandidateDiagnostics(
    int processId,
    const std::vector<RuntimeMemoryRegion>& regions,
    const std::string& executablePath,
    const StarCraftImageSectionHints& hints,
    std::vector<CommandQueueCandidate>& candidates)
  {
    for (CommandQueueCandidate& candidate : candidates)
    {
      const RuntimeMemoryRegion* selectorRegion = findReadableRegion(
        regions,
        candidate.bytesInQueueAddress != 0 ? candidate.bytesInQueueAddress : candidate.vectorAddress,
        candidate.storageKind == "raw-turn-buffer" ? sizeof(std::uint32_t) : sizeof(std::uint64_t));
      if (selectorRegion != nullptr)
      {
        candidate.regionClass = commandQueueRegionClass(*selectorRegion, executablePath, hints);
        if (candidate.regionPath.empty())
          candidate.regionPath = selectorRegion->mappedPath;
      }
      else if (candidate.regionClass.empty())
      {
        candidate.regionClass = "unknown";
      }

      const RuntimeMemoryRegion* bufferRegion = findReadableRegion(
        regions,
        candidate.bufferBegin,
        std::max<std::size_t>(1, std::min<std::size_t>(candidate.capacityBytes, 64)));
      if (bufferRegion != nullptr)
      {
        candidate.bufferRegionClass = commandQueueRegionClass(*bufferRegion, executablePath, hints);
        if (candidate.bufferRegionPath.empty())
          candidate.bufferRegionPath = bufferRegion->mappedPath;
      }
      else if (candidate.bufferRegionClass.empty())
      {
        candidate.bufferRegionClass = "unknown";
      }

      const std::size_t bytesToRead =
        std::min<std::size_t>(candidate.capacityBytes, 64);
      if (bytesToRead == 0 || candidate.bufferBegin == 0)
        continue;

      RuntimeMemoryReadResult prefix =
        readProcessMemory(processId, candidate.bufferBegin, bytesToRead);
      if (!prefix.success || prefix.bytesRead == 0)
        continue;
      if (prefix.bytes.size() > prefix.bytesRead)
        prefix.bytes.resize(prefix.bytesRead);
      populateCommandQueuePrefixDiagnostics(candidate, prefix.bytes);
    }
  }

  bool higherRankedCommandQueueCandidate(
    const CommandQueueCandidate& lhs,
    const CommandQueueCandidate& rhs)
  {
    if (lhs.score != rhs.score)
      return lhs.score > rhs.score;
    if (lhs.capacityBytes != rhs.capacityBytes)
      return lhs.capacityBytes < rhs.capacityBytes;
    return lhs.vectorAddress < rhs.vectorAddress;
  }

  void retainDiverseCommandQueueCandidates(
    std::vector<CommandQueueCandidate>& candidates,
    std::size_t maxCandidates)
  {
    if (maxCandidates == 0)
      maxCandidates = 32;
    if (candidates.size() <= maxCandidates)
      return;

    std::vector<CommandQueueCandidate> vectors;
    std::vector<CommandQueueCandidate> rawTurnBuffers;
    vectors.reserve(candidates.size());
    rawTurnBuffers.reserve(candidates.size());
    for (const CommandQueueCandidate& candidate : candidates)
    {
      if (candidate.storageKind == "raw-turn-buffer")
        rawTurnBuffers.push_back(candidate);
      else
        vectors.push_back(candidate);
    }

    const std::size_t targetPerKind = std::max<std::size_t>(1, maxCandidates / 2);
    std::vector<CommandQueueCandidate> retained;
    retained.reserve(maxCandidates);
    std::unordered_set<std::uintptr_t> retainedSelectors;

    const auto appendCandidate =
      [&](const CommandQueueCandidate& candidate) -> bool
      {
        if (retained.size() >= maxCandidates)
          return false;
        if (!retainedSelectors.insert(candidate.vectorAddress).second)
          return false;
        retained.push_back(candidate);
        return true;
      };

    const auto appendStratified =
      [&](const std::vector<CommandQueueCandidate>& source,
          std::size_t requested,
          const auto& predicate)
      {
        if (requested == 0 || source.empty() || retained.size() >= maxCandidates)
          return;

        const std::size_t stride =
          std::max<std::size_t>(1, source.size() / requested);
        std::size_t appended = 0;
        for (std::size_t index = 0;
             index < source.size() && appended < requested && retained.size() < maxCandidates;
             index += stride)
        {
          const CommandQueueCandidate& candidate = source[index];
          if (!predicate(candidate))
            continue;
          if (appendCandidate(candidate))
            ++appended;
        }
      };

    // Preserve both vector and raw turn-buffer shapes. SC:R live scans can
    // produce hundreds of thousands of plausible raw buffers; keeping only the
    // highest scoring entries biases toward private pointer-like buffers and
    // can drop the target-image runtime data slots before live activity is
    // sampled. Keep top-ranked entries, then stratify the raw candidates by
    // address/class so active-match input has a chance to promote the real sink.
    const std::size_t topPerKind = std::max<std::size_t>(1, maxCandidates / 4);
    for (const CommandQueueCandidate& candidate : vectors)
    {
      if (retained.size() >= topPerKind)
        break;
      appendCandidate(candidate);
    }
    const std::size_t vectorRetained = retained.size();
    for (const CommandQueueCandidate& candidate : rawTurnBuffers)
    {
      if (retained.size() - vectorRetained >= topPerKind)
        break;
      appendCandidate(candidate);
    }

    appendStratified(
      rawTurnBuffers,
      std::max<std::size_t>(1, maxCandidates / 4),
      [](const CommandQueueCandidate& candidate)
      {
        return candidate.regionClass == "target-image-runtime-data";
      });
    appendStratified(
      rawTurnBuffers,
      std::max<std::size_t>(1, maxCandidates / 8),
      [](const CommandQueueCandidate& candidate)
      {
        return candidate.regionClass == "private";
      });
    appendStratified(
      rawTurnBuffers,
      std::max<std::size_t>(1, maxCandidates / 8),
      [](const CommandQueueCandidate&)
      {
        return true;
      });

    for (const CommandQueueCandidate& candidate : candidates)
    {
      if (retained.size() >= maxCandidates)
        break;
      appendCandidate(candidate);
    }

    std::stable_sort(
      retained.begin(),
      retained.end(),
      higherRankedCommandQueueCandidate);
    candidates = std::move(retained);
  }

  bool implicitLiveCommandCandidateSafeForWrite(const CommandQueueCandidate& candidate)
  {
    return candidate.usedBytes <= implicitLiveCommandQueueMaxUsedBytes
      && candidate.capacityBytes >= 256
      && candidate.capacityBytes <= 16 * 1024;
  }

  bool implicitLiveCommandCandidateEligibleForWrite(
    const CommandQueueCandidate& candidate,
    std::string& reason)
  {
    const bool observedActivity = candidate.activityTransitions > 0;
    if (!observedActivity)
    {
      reason =
        "implicit live command writes require observed natural command-queue end/count activity; "
        "raw buffer byte changes without a byte-count transition are diagnostics only";
      return false;
    }
    if (!implicitLiveCommandCandidateSafeForWrite(candidate))
    {
      reason =
        "implicit live command candidate is outside bounded write limits";
      return false;
    }
    if (candidate.storageKind == "raw-turn-buffer")
    {
      reason =
        "implicit raw turn-buffer writes are disabled for live SC:R; "
        "the original 1.16.1 append path has produced destructive false positives, "
        "so use an explicit manually validated candidate or an in-process callable adapter";
      return false;
    }
    reason = "observed natural command-queue end/count activity within bounded write limits";
    return true;
  }

  bool liveCommandCandidateSelectorSafeForWrite(
    const CommandQueueCandidate& candidate,
    const std::string& executablePath,
    std::string& reason)
  {
    const bool targetImageVector =
      candidate.storageKind == "vector" && sameMappedFile(candidate.regionPath, executablePath);
    const bool nonTargetMappedBuffer =
      !candidate.bufferRegionPath.empty()
      && !sameMappedFile(candidate.bufferRegionPath, executablePath);
    if (nonTargetMappedBuffer)
    {
      reason =
        "refusing to write command queue candidate whose buffer is mapped from a non-StarCraft image: "
        + candidate.bufferRegionPath;
      return false;
    }
    if (targetImageVector && candidate.activityTransitions == 0)
    {
      reason =
        "refusing to write command queue vector selector stored in the target executable image; "
        "live proof requires observed natural end/count activity before using a target image selector";
      return false;
    }
    return true;
  }

  void refreshCommandQueueDiscoveryRetainedStats(CommandQueueDiscoveryProof& proof)
  {
    proof.retainedVectorCandidates = 0;
    proof.retainedRawTurnBufferCandidates = 0;
    proof.retainedActiveCandidates = 0;
    proof.retainedTransitionCandidates = 0;
    proof.retainedRawByteChangeOnlyCandidates = 0;
    proof.retainedBoundedTransitionCandidates = 0;
    proof.implicitWriteEligibleCandidates = 0;
    for (const CommandQueueCandidate& candidate : proof.candidates)
    {
      if (candidate.storageKind == "raw-turn-buffer")
        ++proof.retainedRawTurnBufferCandidates;
      else if (candidate.storageKind == "vector")
        ++proof.retainedVectorCandidates;

      const bool active =
        candidate.activityTransitions > 0
        || (candidate.storageKind == "raw-turn-buffer" && candidate.activityByteChanges > 0);
      if (active)
        ++proof.retainedActiveCandidates;
      if (candidate.activityTransitions > 0)
        ++proof.retainedTransitionCandidates;
      if (candidate.storageKind == "raw-turn-buffer"
          && candidate.activityTransitions == 0
          && candidate.activityByteChanges > 0)
        ++proof.retainedRawByteChangeOnlyCandidates;

      std::string reason;
      if (candidate.activityTransitions > 0 && implicitLiveCommandCandidateSafeForWrite(candidate))
        ++proof.retainedBoundedTransitionCandidates;
      if (implicitLiveCommandCandidateEligibleForWrite(candidate, reason))
        ++proof.implicitWriteEligibleCandidates;
    }
  }

  void preserveLiveCommandCandidateDiagnostics(
    CommandQueueCandidate& current,
    const CommandQueueCandidate& selected)
  {
    current.discoverySource = selected.discoverySource;
    current.score = selected.score;
    current.activitySamples = selected.activitySamples;
    current.activityTransitions = selected.activityTransitions;
    current.activityByteChanges = selected.activityByteChanges;
    current.activityChangedByteTotal = selected.activityChangedByteTotal;
    current.activityLastChangeFirstOffset = selected.activityLastChangeFirstOffset;
    current.activityLastChangeLastOffset = selected.activityLastChangeLastOffset;
    current.activityFirstUsedBytes = selected.activityFirstUsedBytes;
    current.activityLastUsedBytes = selected.activityLastUsedBytes;
    current.activityMinUsedBytes = selected.activityMinUsedBytes;
    current.activityMaxUsedBytes = selected.activityMaxUsedBytes;
    current.activityFirstBufferEnd = selected.activityFirstBufferEnd;
    current.activityLastBufferEnd = selected.activityLastBufferEnd;
    current.activitySelectorFirstHex = selected.activitySelectorFirstHex;
    current.activitySelectorLastHex = selected.activitySelectorLastHex;
    current.activityBufferFirstHex = selected.activityBufferFirstHex;
    current.activityBufferLastHex = selected.activityBufferLastHex;
    current.activityReason = selected.activityReason;
    current.regionClass = selected.regionClass;
    current.regionPath = selected.regionPath;
    current.bufferRegionClass = selected.bufferRegionClass;
    current.bufferRegionPath = selected.bufferRegionPath;
    current.prefixBytes = selected.prefixBytes;
    current.prefixNonZeroBytes = selected.prefixNonZeroBytes;
    current.prefixDistinctBytes = selected.prefixDistinctBytes;
    current.prefixKnownOpcodeBytes = selected.prefixKnownOpcodeBytes;
    current.prefixPointerWords = selected.prefixPointerWords;
    current.prefixSmallIntegerWords = selected.prefixSmallIntegerWords;
    current.prefixEntropyMilli = selected.prefixEntropyMilli;
    current.prefixHex = selected.prefixHex;
    current.firstByteHex = selected.firstByteHex;
    current.codeReferenceAnchor = selected.codeReferenceAnchor;
    current.codeReferenceKind = selected.codeReferenceKind;
    current.codeReferenceAddress = selected.codeReferenceAddress;
    current.codeReferenceTarget = selected.codeReferenceTarget;
    current.codeReferenceBytes = selected.codeReferenceBytes;
  }

  bool readCommandQueueCandidate(
    int processId,
    const std::vector<RuntimeMemoryRegion>& regions,
    std::uintptr_t vectorAddress,
    CommandQueueCandidate& candidate,
    std::string& reason);

  bool readExplicitRawTurnBufferCandidate(
    int processId,
    const std::vector<RuntimeMemoryRegion>& regions,
    std::uintptr_t bytesInQueueAddress,
    std::uintptr_t bufferAddress,
    CommandQueueCandidate& candidate,
    std::string& reason);

  bool liveCodeReferencePotentialCommandData(
    const RuntimeMemoryRegion* region,
    const std::string& executablePath)
  {
    if (region == nullptr)
      return false;
    if (!region->readable || !region->writable || region->executable)
      return false;
    return !fileBackedNonTargetRegion(*region, executablePath);
  }

  void annotateLiveCodeReferenceCandidate(
    CommandQueueCandidate& candidate,
    const std::string& anchor,
    const LiveAnchorDiagnostic::CodeEvent& event)
  {
    candidate.discoverySource = "live-code-reference";
    candidate.codeReferenceAnchor = anchor;
    candidate.codeReferenceKind = event.kind;
    candidate.codeReferenceAddress = event.address;
    candidate.codeReferenceTarget = event.target;
    candidate.codeReferenceBytes = event.bytes;
    candidate.activityReason =
      "candidate is derived from a live RIP-relative data reference near "
      + anchor
      + "; delivery still requires write-time revalidation and behavior proof";
    candidate.score += candidate.storageKind == "vector" ? 4500 : 2500;
  }

  void appendLiveCodeReferenceCommandQueueCandidate(
    CommandQueueDiscoveryProof& proof,
    std::unordered_set<std::uintptr_t>& seenSelectors,
    CommandQueueCandidate candidate,
    const std::string& anchor,
    const LiveAnchorDiagnostic::CodeEvent& event)
  {
    const std::uintptr_t selector = candidate.storageKind == "raw-turn-buffer"
      ? candidate.bytesInQueueAddress
      : candidate.vectorAddress;
    if (selector == 0 || !seenSelectors.insert(selector).second)
      return;

    annotateLiveCodeReferenceCandidate(candidate, anchor, event);
    if (candidate.storageKind == "raw-turn-buffer")
      ++proof.rawTurnBufferCandidates;
    else if (candidate.storageKind == "vector")
      ++proof.vectorCandidates;
    ++proof.liveCodeReferenceCandidateCount;
    proof.candidates.push_back(std::move(candidate));
  }

  void appendLiveCodeReferenceCommandQueueCandidates(
    int processId,
    const std::vector<RuntimeMemoryRegion>& regions,
    const std::string& executablePath,
    CommandQueueDiscoveryProof& proof,
    std::unordered_set<std::uintptr_t>& seenSelectors)
  {
    LiveCallableDiagnostics diagnostics = discoverLiveCallableDiagnostics(
      processId,
      executablePath,
      {
        "GetTurnPackets",
        "QueueGameCommand",
        "Invalid order for action command"
      });
    if (!diagnostics.regionListAvailable)
      return;

    const auto inspectEvent =
      [&](const std::string& anchor, const LiveAnchorDiagnostic::CodeEvent& event)
      {
        const RuntimeMemoryRegion* targetRegion =
          findReadableRegion(regions, event.target, 1);
        if (!liveCodeReferencePotentialCommandData(targetRegion, executablePath))
          return;

        ++proof.liveCodeReferenceCount;
        const std::size_t candidateCountBefore =
          proof.liveCodeReferenceCandidateCount;

        const std::array<std::ptrdiff_t, 5> vectorDeltas = { 0, -16, -8, 8, 16 };
        for (std::ptrdiff_t delta : vectorDeltas)
        {
          std::uintptr_t vectorAddress = 0;
          if (delta < 0)
          {
            const auto negative = static_cast<std::uintptr_t>(-delta);
            if (event.target < negative)
              continue;
            vectorAddress = event.target - negative;
          }
          else
          {
            const auto positive = static_cast<std::uintptr_t>(delta);
            if (event.target > std::numeric_limits<std::uintptr_t>::max() - positive)
              continue;
            vectorAddress = event.target + positive;
          }
          vectorAddress -= vectorAddress % sizeof(std::uint64_t);

          CommandQueueCandidate candidate;
          std::string reason;
          if (!readCommandQueueCandidate(
                processId,
                regions,
                vectorAddress,
                candidate,
                reason))
            continue;
          appendLiveCodeReferenceCommandQueueCandidate(
            proof,
            seenSelectors,
            std::move(candidate),
            anchor,
            event);
        }

        for (std::size_t counterOffset : rawTurnBufferCounterOffsets)
        {
          if (event.target < counterOffset)
            continue;
          CommandQueueCandidate candidate;
          std::string reason;
          if (!readExplicitRawTurnBufferCandidate(
                processId,
                regions,
                event.target,
                event.target - counterOffset,
                candidate,
                reason))
            continue;
          appendLiveCodeReferenceCommandQueueCandidate(
            proof,
            seenSelectors,
            std::move(candidate),
            anchor,
            event);
        }

        for (std::size_t counterOffset : rawTurnBufferPrefixCounterOffsets)
        {
          if (event.target > std::numeric_limits<std::uintptr_t>::max() - counterOffset)
            continue;
          CommandQueueCandidate candidate;
          std::string reason;
          if (!readExplicitRawTurnBufferCandidate(
                processId,
                regions,
                event.target,
                event.target + counterOffset,
                candidate,
                reason))
            continue;
          appendLiveCodeReferenceCommandQueueCandidate(
            proof,
            seenSelectors,
            std::move(candidate),
            anchor,
            event);
        }

        if (proof.liveCodeReferenceCandidateCount == candidateCountBefore)
          ++proof.liveCodeReferenceRejectedCount;
      };

    for (const LiveAnchorDiagnostic& anchor : diagnostics.anchors)
    {
      for (const LiveAnchorDiagnostic::CodeEvent& event : anchor.nearbyCodeEvents)
        inspectEvent(anchor.anchor, event);

      for (std::uintptr_t callTarget : anchor.nearbyCallTargets)
      {
        const std::vector<LiveAnchorDiagnostic::CodeEvent> callTargetEvents =
          collectNearbyCodeEvents(
            processId,
            regions,
            executablePath,
            callTarget,
            48);
        for (const LiveAnchorDiagnostic::CodeEvent& event : callTargetEvents)
          inspectEvent(anchor.anchor, event);
      }
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

      for (std::size_t regionOffset = 0;
           regionOffset < region.size && scanned < maxScanBytes;
           regionOffset += maxRegionBytes)
      {
        if (timedOut(deadline))
        {
          proof.reason = "command queue discovery timed out before scan completed";
          break;
        }

        const std::size_t remainingRegionBytes = region.size - regionOffset;
        const std::size_t bytesToRead =
          std::min(remainingRegionBytes, std::min(maxRegionBytes, maxScanBytes - scanned));
        if (bytesToRead < sizeof(std::uint64_t) * 3)
          break;

        RuntimeMemoryReadResult read =
          readProcessMemory(processId, region.address + regionOffset, bytesToRead);
        if (!read.success || read.bytesRead < sizeof(std::uint64_t) * 3)
          break;

        ++proof.scannedRegions;
        proof.scannedBytes += read.bytesRead;
        scanned += read.bytesRead;
        const std::uintptr_t chunkBase = region.address + regionOffset;

        for (std::size_t offset = 0; offset + sizeof(std::uint64_t) * 3 <= read.bytes.size(); offset += 8)
        {
          if ((offset % (16 * 1024)) == 0 && timedOut(deadline))
          {
            proof.reason = "command queue discovery timed out while scoring vector candidates";
            break;
          }

          const std::uintptr_t vectorAddress = chunkBase + offset;
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
          if (fileBackedNonTargetRegion(*bufferRegion, executablePath))
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
          candidate.counterOffset = sizeof(std::uint64_t);
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
          candidate.regionClass = commandQueueRegionClass(region, executablePath, hints);
          candidate.bufferRegionPath = bufferRegion->mappedPath;
          candidate.bufferRegionClass = commandQueueRegionClass(*bufferRegion, executablePath, hints);
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

        if (!proof.reason.empty())
          break;

        const bool targetImageRawRegion =
          sameMappedFile(region.mappedPath, executablePath)
          || regionIntersectsStarCraftRuntimeData(region, hints);
        const bool privateRawRegion = region.mappedPath.empty();
        const bool scanRawTurnBuffersInRegion =
          targetImageRawRegion || privateRawRegion;
        if (scanRawTurnBuffersInRegion)
        {
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
              if (privateRawRegion && !targetImageRawRegion && usedBytes == 0)
                continue;
              if (!plausibleRawTurnBufferBytes(read.bytes, bufferOffset, usedBytes))
                continue;

              const std::uintptr_t bufferBegin = chunkBase + bufferOffset;
              const std::uintptr_t bytesInQueueAddress = chunkBase + countOffset;
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
              candidate.counterOffset = counterOffset;
              candidate.score = rawCommandQueueCandidateScore(
                region,
                executablePath,
                hints,
                usedBytes,
                counterOffset);
              candidate.regionPath = region.mappedPath;
              candidate.regionClass = commandQueueRegionClass(region, executablePath, hints);
              candidate.bufferRegionPath = region.mappedPath;
              candidate.bufferRegionClass = commandQueueRegionClass(region, executablePath, hints);
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

        if (!proof.reason.empty() || read.bytesRead < bytesToRead)
          break;
      }

      if (!proof.reason.empty())
        break;
    }

    std::sort(
      proof.candidates.begin(),
      proof.candidates.end(),
      higherRankedCommandQueueCandidate);

    appendLiveCodeReferenceCommandQueueCandidates(
      processId,
      regions.regions,
      executablePath,
      proof,
      seenVectors);

    std::sort(
      proof.candidates.begin(),
      proof.candidates.end(),
      higherRankedCommandQueueCandidate);

    if (maxCandidates == 0)
      maxCandidates = 32;
    retainDiverseCommandQueueCandidates(proof.candidates, maxCandidates);
    annotateCommandQueueCandidateDiagnostics(
      processId,
      regions.regions,
      executablePath,
      hints,
      proof.candidates);
    refreshCommandQueueDiscoveryRetainedStats(proof);

    proof.ready = !proof.candidates.empty();
    if (!proof.ready && proof.reason.empty())
      proof.reason = "no command-queue-like live vector candidates were found";
    return proof;
  }

  bool writeCommandQueueDiscoverySnapshot(
    const std::filesystem::path& path,
    const CommandQueueDiscoveryProof& proof,
    const std::string& executablePath,
    std::string& reason)
  {
    std::ofstream output(path);
    if (!output)
    {
      reason = "unable to open command queue discovery snapshot output";
      return false;
    }

    output << "rank\tscore\tdiscovery_source\tkind\tselector_address\tbytes_in_queue_address\tbuffer_begin\tbuffer_end\tbuffer_capacity\tused_bytes\tcapacity_bytes\tcounter_offset\tprefix_bytes\tprefix_nonzero_bytes\tprefix_distinct_bytes\tprefix_known_opcode_bytes\tprefix_pointer_words\tprefix_small_integer_words\tprefix_entropy_milli\tfirst_byte\tprefix_hex\tactivity_samples\tactivity_transitions\tactivity_byte_changes\tactivity_changed_byte_total\tactivity_last_change_first_offset\tactivity_last_change_last_offset\tactivity_first_used_bytes\tactivity_last_used_bytes\tactivity_min_used_bytes\tactivity_max_used_bytes\tactivity_first_buffer_end\tactivity_last_buffer_end\tactivity_selector_first_hex\tactivity_selector_last_hex\tactivity_buffer_first_hex\tactivity_buffer_last_hex\timplicit_write_eligible\timplicit_write_reason\tlive_write_safe\tlive_write_reason\tactivity_reason\tregion_class\tregion_path\tbuffer_region_class\tbuffer_region_path\tcode_reference_anchor\tcode_reference_kind\tcode_reference_address\tcode_reference_target\tcode_reference_bytes\n";
    for (std::size_t i = 0; i < proof.candidates.size(); ++i)
    {
      const CommandQueueCandidate& candidate = proof.candidates[i];
      std::string implicitWriteReason;
      const bool implicitWriteEligible =
        implicitLiveCommandCandidateEligibleForWrite(candidate, implicitWriteReason);
      std::string liveWriteReason;
      const bool liveWriteSafe =
        implicitWriteEligible
        && liveCommandCandidateSelectorSafeForWrite(candidate, executablePath, liveWriteReason);
      output << i << '\t'
             << candidate.score << '\t'
             << candidate.discoverySource << '\t'
             << candidate.storageKind << '\t'
             << hexAddress(candidate.vectorAddress) << '\t'
             << hexAddress(candidate.bytesInQueueAddress) << '\t'
             << hexAddress(candidate.bufferBegin) << '\t'
             << hexAddress(candidate.bufferEnd) << '\t'
             << hexAddress(candidate.bufferCapacity) << '\t'
             << candidate.usedBytes << '\t'
             << candidate.capacityBytes << '\t'
             << candidate.counterOffset << '\t'
             << candidate.prefixBytes << '\t'
             << candidate.prefixNonZeroBytes << '\t'
             << candidate.prefixDistinctBytes << '\t'
             << candidate.prefixKnownOpcodeBytes << '\t'
             << candidate.prefixPointerWords << '\t'
             << candidate.prefixSmallIntegerWords << '\t'
             << candidate.prefixEntropyMilli << '\t'
             << candidate.firstByteHex << '\t'
             << candidate.prefixHex << '\t'
             << candidate.activitySamples << '\t'
             << candidate.activityTransitions << '\t'
             << candidate.activityByteChanges << '\t'
             << candidate.activityChangedByteTotal << '\t'
             << candidate.activityLastChangeFirstOffset << '\t'
             << candidate.activityLastChangeLastOffset << '\t'
             << candidate.activityFirstUsedBytes << '\t'
             << candidate.activityLastUsedBytes << '\t'
             << candidate.activityMinUsedBytes << '\t'
             << candidate.activityMaxUsedBytes << '\t'
             << hexAddress(candidate.activityFirstBufferEnd) << '\t'
             << hexAddress(candidate.activityLastBufferEnd) << '\t'
             << candidate.activitySelectorFirstHex << '\t'
             << candidate.activitySelectorLastHex << '\t'
             << candidate.activityBufferFirstHex << '\t'
             << candidate.activityBufferLastHex << '\t'
             << (implicitWriteEligible ? "true" : "false") << '\t'
             << implicitWriteReason << '\t'
             << (liveWriteSafe ? "true" : "false") << '\t'
             << liveWriteReason << '\t'
             << candidate.activityReason << '\t'
             << candidate.regionClass << '\t'
             << candidate.regionPath << '\t'
             << candidate.bufferRegionClass << '\t'
             << candidate.bufferRegionPath << '\t'
             << candidate.codeReferenceAnchor << '\t'
             << candidate.codeReferenceKind << '\t'
             << hexAddress(candidate.codeReferenceAddress) << '\t'
             << hexAddress(candidate.codeReferenceTarget) << '\t'
             << candidate.codeReferenceBytes << '\n';
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

      for (std::size_t counterOffset : rawTurnBufferPrefixCounterOffsets)
      {
        const std::uintptr_t bufferBegin = vectorAddress + counterOffset;
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
        candidate.counterOffset = counterOffset;
        return true;
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
        candidate.counterOffset = counterOffset;
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
    candidate.counterOffset = sizeof(std::uint64_t);
    return true;
  }

  bool readExplicitRawTurnBufferCandidate(
    int processId,
    const std::vector<RuntimeMemoryRegion>& regions,
    std::uintptr_t bytesInQueueAddress,
    std::uintptr_t bufferAddress,
    CommandQueueCandidate& candidate,
    std::string& reason)
  {
    RuntimeMemoryReadResult countRead =
      readProcessMemory(processId, bytesInQueueAddress, sizeof(std::uint32_t));
    if (!countRead.success || countRead.bytesRead != sizeof(std::uint32_t))
    {
      reason = countRead.reason.empty()
        ? "unable to read explicit raw turn-buffer byte count"
        : countRead.reason;
      return false;
    }

    const std::uint32_t usedBytes32 = readU32(countRead.bytes, 0);
    if (usedBytes32 > rawTurnBufferCapacity)
    {
      reason = "explicit raw turn-buffer byte count is outside capacity";
      return false;
    }

    if (!writableAddress(regions, bytesInQueueAddress, sizeof(std::uint32_t))
        || !writableAddress(regions, bufferAddress, rawTurnBufferCapacity))
    {
      reason = "explicit raw turn-buffer count/buffer storage is not writable";
      return false;
    }

    RuntimeMemoryReadResult bufferRead =
      readProcessMemory(processId, bufferAddress, rawTurnBufferCapacity);
    if (!bufferRead.success || bufferRead.bytesRead != rawTurnBufferCapacity)
    {
      reason = bufferRead.reason.empty()
        ? "unable to read explicit raw turn-buffer bytes"
        : bufferRead.reason;
      return false;
    }
    if (!plausibleRawTurnBufferBytes(bufferRead.bytes, 0, usedBytes32))
    {
      reason = "explicit raw turn-buffer bytes are not plausible for the current byte count";
      return false;
    }

    candidate.storageKind = "raw-turn-buffer";
    candidate.vectorAddress = bytesInQueueAddress;
    candidate.bytesInQueueAddress = bytesInQueueAddress;
    candidate.bufferBegin = bufferAddress;
    candidate.bufferEnd = bufferAddress + usedBytes32;
    candidate.bufferCapacity = bufferAddress + rawTurnBufferCapacity;
    candidate.usedBytes = usedBytes32;
    candidate.capacityBytes = rawTurnBufferCapacity;
    candidate.counterOffset =
      bufferAddress >= bytesInQueueAddress
        ? bufferAddress - bytesInQueueAddress
        : bytesInQueueAddress - bufferAddress;
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
    candidate.counterOffset =
      selectedCandidate.counterOffset != 0
        ? selectedCandidate.counterOffset
        : selectedCandidate.bytesInQueueAddress - selectedCandidate.bufferBegin;
    return true;
  }

  bool readSelectedCommandQueueCandidate(
    int processId,
    const std::vector<RuntimeMemoryRegion>& regions,
    const CommandQueueCandidate& selectedCandidate,
    CommandQueueCandidate& candidate,
    std::string& reason)
  {
    if (selectedCandidate.storageKind == "raw-turn-buffer"
        && selectedCandidate.bytesInQueueAddress != 0
        && selectedCandidate.bufferBegin != 0)
    {
      if (readKnownRawTurnBufferCandidate(
            processId,
            regions,
            selectedCandidate,
            candidate,
            reason))
        return true;
    }

    if (selectedCandidate.vectorAddress == 0)
    {
      if (reason.empty())
        reason = "selected command queue candidate has no selector address";
      return false;
    }

    return readCommandQueueCandidate(
      processId,
      regions,
      selectedCandidate.vectorAddress,
      candidate,
      reason);
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

  bool readCandidatePrefixBytes(
    int processId,
    const CommandQueueCandidate& candidate,
    std::vector<unsigned char>& bytes,
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
        ? "unable to read command queue candidate prefix bytes"
        : read.reason;
      return false;
    }

    bytes = std::move(read.bytes);
    if (bytes.size() > read.bytesRead)
      bytes.resize(read.bytesRead);
    return true;
  }

  bool readCandidateSelectorBytes(
    int processId,
    const CommandQueueCandidate& candidate,
    std::vector<unsigned char>& bytes,
    std::string& reason)
  {
    const std::uintptr_t selectorAddress =
      candidate.storageKind == "raw-turn-buffer"
        ? candidate.bytesInQueueAddress
        : candidate.vectorAddress;
    const std::size_t selectorBytes =
      candidate.storageKind == "raw-turn-buffer"
        ? sizeof(std::uint32_t)
        : sizeof(std::uint64_t) * 3;
    if (selectorAddress == 0)
    {
      reason = "command queue candidate has zero selector address";
      return false;
    }

    RuntimeMemoryReadResult read =
      readProcessMemory(processId, selectorAddress, selectorBytes);
    if (!read.success || read.bytesRead != selectorBytes)
    {
      reason = read.reason.empty()
        ? "unable to read command queue selector bytes"
        : read.reason;
      return false;
    }

    bytes = std::move(read.bytes);
    if (bytes.size() > read.bytesRead)
      bytes.resize(read.bytesRead);
    return true;
  }

  std::size_t changedByteCount(
    const std::vector<unsigned char>& previous,
    const std::vector<unsigned char>& current,
    std::size_t& firstChangedOffset,
    std::size_t& lastChangedOffset)
  {
    std::size_t changed = 0;
    firstChangedOffset = 0;
    lastChangedOffset = 0;
    const std::size_t limit = std::min(previous.size(), current.size());
    for (std::size_t i = 0; i < limit; ++i)
    {
      if (previous[i] == current[i])
        continue;
      if (changed == 0)
        firstChangedOffset = i;
      lastChangedOffset = i;
      ++changed;
    }

    const std::size_t larger = std::max(previous.size(), current.size());
    if (larger > limit)
    {
      if (changed == 0)
        firstChangedOffset = limit;
      lastChangedOffset = larger - 1;
      changed += larger - limit;
    }
    return changed;
  }

  void recordCommandQueueActivitySnapshot(
    CommandQueueCandidate& candidate,
    const CommandQueueCandidate& current,
    const std::vector<unsigned char>& selectorBytes,
    const std::vector<unsigned char>& bufferBytes,
    bool first)
  {
    const std::string selectorHex = bytesHexPrefix(selectorBytes, 32);
    const std::string bufferHex = bytesHexPrefix(bufferBytes, 32);
    if (first)
    {
      candidate.activityFirstUsedBytes = current.usedBytes;
      candidate.activityMinUsedBytes = current.usedBytes;
      candidate.activityMaxUsedBytes = current.usedBytes;
      candidate.activityFirstBufferEnd = current.bufferEnd;
      candidate.activitySelectorFirstHex = selectorHex;
      candidate.activityBufferFirstHex = bufferHex;
    }
    else
    {
      candidate.activityMinUsedBytes =
        std::min(candidate.activityMinUsedBytes, current.usedBytes);
      candidate.activityMaxUsedBytes =
        std::max(candidate.activityMaxUsedBytes, current.usedBytes);
    }
    candidate.activityLastUsedBytes = current.usedBytes;
    candidate.activityLastBufferEnd = current.bufferEnd;
    candidate.activitySelectorLastHex = selectorHex;
    candidate.activityBufferLastHex = bufferHex;
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
      std::vector<unsigned char> previousBytes;
    };

    std::vector<ObservedCandidate> observed;
    observed.reserve(limit);
    for (std::size_t index = 0; index < limit; ++index)
    {
      CommandQueueCandidate& candidate = candidates[index];
      ObservedCandidate state;
      state.index = index;
      std::string reason;
      if (!readSelectedCommandQueueCandidate(
            processId,
            regions.regions,
            candidate,
            state.previous,
            reason))
      {
        candidate.activityReason = reason;
        observed.push_back(state);
        continue;
      }

      if (!readCandidatePrefixBytes(processId, state.previous, state.previousBytes, reason))
      {
        candidate.activityReason = reason;
        observed.push_back(state);
        continue;
      }

      std::vector<unsigned char> selectorBytes;
      if (!readCandidateSelectorBytes(processId, state.previous, selectorBytes, reason))
      {
        candidate.activityReason = reason;
        observed.push_back(state);
        continue;
      }

      candidate.activitySamples = 1;
      recordCommandQueueActivitySnapshot(
        candidate,
        state.previous,
        selectorBytes,
        state.previousBytes,
        true);
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
        if (!readSelectedCommandQueueCandidate(
              processId,
              regions.regions,
              candidate,
              current,
              reason))
        {
          candidate.activityReason = reason;
          state.readable = false;
          continue;
        }

        std::vector<unsigned char> currentBytes;
        if (!readCandidatePrefixBytes(processId, current, currentBytes, reason))
        {
          candidate.activityReason = reason;
          state.readable = false;
          continue;
        }

        std::vector<unsigned char> selectorBytes;
        if (!readCandidateSelectorBytes(processId, current, selectorBytes, reason))
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
        std::size_t firstChangedOffset = 0;
        std::size_t lastChangedOffset = 0;
        const std::size_t changedBytes = changedByteCount(
          state.previousBytes,
          currentBytes,
          firstChangedOffset,
          lastChangedOffset);
        if (changedBytes > 0)
        {
          ++candidate.activityByteChanges;
          candidate.activityChangedByteTotal += changedBytes;
          candidate.activityLastChangeFirstOffset = firstChangedOffset;
          candidate.activityLastChangeLastOffset = lastChangedOffset;
        }
        recordCommandQueueActivitySnapshot(
          candidate,
          current,
          selectorBytes,
          currentBytes,
          false);

        state.previous = current;
        state.previousBytes = std::move(currentBytes);
      }
    }

    for (std::size_t index = 0; index < limit; ++index)
    {
      CommandQueueCandidate& candidate = candidates[index];
      const bool active =
        candidate.activityTransitions > 0
        || (candidate.storageKind == "raw-turn-buffer" && candidate.activityByteChanges > 0);
      if (active)
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
        const bool lhsActive =
          lhs.activityTransitions > 0
          || (lhs.storageKind == "raw-turn-buffer" && lhs.activityByteChanges > 0);
        const bool rhsActive =
          rhs.activityTransitions > 0
          || (rhs.storageKind == "raw-turn-buffer" && rhs.activityByteChanges > 0);
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
    if (!readSelectedCommandQueueCandidate(
          processId,
          regions.regions,
          selectedCandidate,
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

    result.candidate = current;
    result.tailAddress = current.bufferEnd;
    result.expectedEnd = current.bufferEnd + encodedBytes.size();
    result.originalTail = originalTail.bytes;
    result.appendedBytes = encodedBytes.size();

    const auto rollbackImmediateAppend = [&]() -> void
    {
      if (result.tailAddress == 0 || result.originalTail.empty())
        return;

      writeProcessMemory(
        processId,
        result.tailAddress,
        result.originalTail.data(),
        result.originalTail.size());
      if (current.storageKind == "raw-turn-buffer")
      {
        const std::uint32_t originalUsedBytes32 =
          static_cast<std::uint32_t>(current.usedBytes);
        writeProcessMemory(
          processId,
          current.bytesInQueueAddress,
          &originalUsedBytes32,
          sizeof(originalUsedBytes32));
      }
      else
      {
        const std::uint64_t originalEnd64 = static_cast<std::uint64_t>(current.bufferEnd);
        writeProcessMemory(
          processId,
          current.vectorAddress + sizeof(std::uint64_t),
          &originalEnd64,
          sizeof(originalEnd64));
      }
    };

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
        rollbackImmediateAppend();
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
        rollbackImmediateAppend();
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
      rollbackImmediateAppend();
      result.reason = readback.reason.empty()
        ? "unable to read back encoded command bytes"
        : readback.reason;
      return result;
    }
    if (!std::equal(encodedBytes.begin(), encodedBytes.end(), readback.bytes.begin()))
    {
      rollbackImmediateAppend();
      result.reason = "encoded command readback did not match written bytes";
      return result;
    }

    CommandQueueCandidate afterWrite;
    if (!readSelectedCommandQueueCandidate(
        processId,
        regions.regions,
        current,
        afterWrite,
        reason))
    {
      rollbackImmediateAppend();
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
        rollbackImmediateAppend();
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
        rollbackImmediateAppend();
        result.reason = "command queue self-fixture write passed but restore failed";
        return result;
      }
    }

    result.passed = true;
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
    if (!readSelectedCommandQueueCandidate(
          processId,
          regions.regions,
          append.candidate,
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

  bool restoreCommandQueueAppendsIfStillPresent(
    int processId,
    const CommandQueueAppendResult* first,
    const CommandQueueAppendResult* second = nullptr)
  {
    // Restore in reverse append order so a failed pause/resume proof cannot
    // leave adapter proof bytes queued in the live command sink.
    bool restoredAny = false;
    if (second != nullptr)
      restoredAny = restoreCommandQueueAppendIfStillPresent(processId, *second) || restoredAny;
    if (first != nullptr)
      restoredAny = restoreCommandQueueAppendIfStillPresent(processId, *first) || restoredAny;
    return restoredAny;
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
    if (!readSelectedCommandQueueCandidate(
          processId,
          regions.regions,
          selectedCandidate,
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
    return false;
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
      proof.firstTick = steadyTickMilliseconds();
      std::this_thread::sleep_for(std::chrono::milliseconds(sampleDelayMs));
      if (!readFrameCounterAt(processId, address, proof.second))
      {
        proof.reason = "unable to read explicit frame counter second sample";
        return proof;
      }
      proof.secondTick = steadyTickMilliseconds();
      std::this_thread::sleep_for(std::chrono::milliseconds(sampleDelayMs));
      if (!readFrameCounterAt(processId, address, proof.third))
      {
        proof.reason = "unable to read explicit frame counter third sample";
        return proof;
      }
      proof.thirdTick = steadyTickMilliseconds();
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
    proof.liveDiagnostics = discoverLiveCallableDiagnostics(
      processId,
      executablePath,
      {
        "Invalid order for action command",
        "GetTurnPackets",
        "QueueGameCommand"
      });

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
      bool skippedInactiveCandidate = false;
      bool skippedOutOfBoundsCandidate = false;
      bool skippedRawTurnBufferCandidate = false;
      for (std::size_t i = 0; i < limit; ++i)
      {
        const CommandQueueCandidate& candidate = discoveryProof.candidates[i];
        std::string implicitWriteReason;
        if (!implicitLiveCommandCandidateEligibleForWrite(candidate, implicitWriteReason))
        {
          if (implicitWriteReason.find("observed natural command-queue") != std::string::npos)
            skippedInactiveCandidate = true;
          else if (implicitWriteReason.find("raw turn-buffer writes are disabled") != std::string::npos)
            skippedRawTurnBufferCandidate = true;
          else
            skippedOutOfBoundsCandidate = true;
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
            "live issue-commands proof requires an implicit-write-eligible command queue candidate "
            "or --command-queue-vector-address before writing to a discovered candidate";
          if (skippedInactiveCandidate && discoveryProof.retainedTransitionCandidates == 0)
            proof.reason += "; discovery candidates had no observed natural activity";
          if (discoveryProof.retainedTransitionCandidates > 0)
            proof.reason += "; observed transition candidates="
              + std::to_string(discoveryProof.retainedTransitionCandidates);
          if (discoveryProof.retainedBoundedTransitionCandidates > 0)
            proof.reason += "; bounded transition candidates="
              + std::to_string(discoveryProof.retainedBoundedTransitionCandidates);
          if (skippedRawTurnBufferCandidate)
            proof.reason += "; raw turn-buffer candidates require an in-process callable adapter";
          if (skippedOutOfBoundsCandidate)
            proof.reason += "; some active candidates were outside bounded write limits";
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
      LiveCounterProof commandFrameProof =
        proveExplicitLiveCounterRead(processId, gameStateProof.address, frameSampleMs);
      std::string explicitFrameCounterReason;
      if (commandFrameProof.passed)
      {
        commandCounterCandidates.push_back(
          FrameCounterCandidate {
            commandFrameProof.address,
            commandFrameProof.first,
            commandFrameProof.second,
            commandFrameProof.third,
            0
          });
      }
      else
      {
        explicitFrameCounterReason =
          commandFrameProof.reason.empty()
            ? "explicit live game-state frame counter was not frame-like at command proof sample cadence"
            : commandFrameProof.reason;
      }
      proof.frameCounterCandidateCount = commandCounterCandidates.size();
      if (commandCounterCandidates.empty())
      {
        proof.reason =
          "issue-commands proof requires at least one live frame counter candidate";
        if (!explicitFrameCounterReason.empty())
          proof.reason += "; explicit_counter_reason=" + explicitFrameCounterReason;
        return proof;
      }
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
        const PauseSample* progressingDuringPause = nullptr;
        const int expectedDelta = std::max(1, (frameSampleMs * 24) / 1000);
        const std::uint32_t minimumActiveDelta =
          static_cast<std::uint32_t>(std::max(2, expectedDelta / 3));
        for (PauseSample& sample : samples)
        {
          if (!readFrameCounterAt(processId, sample.candidate->address, sample.end))
            continue;

          const std::uint32_t baselineDelta =
            counterDelta(sample.candidate->second, sample.candidate->third);
          if (baselineDelta < minimumActiveDelta)
            continue;

          const std::uint32_t pausedDelta = counterDelta(sample.start, sample.end);
          if (diagnostic == nullptr)
            diagnostic = &sample;
          if (pausedDelta >= minimumActiveDelta)
          {
            progressingDuringPause = &sample;
            continue;
          }
          if (pausedDelta != 0)
            continue;

          if (best == nullptr || sample.candidate->score < best->candidate->score)
            best = &sample;
        }
        if (progressingDuringPause != nullptr)
        {
          attempt.pauseFrameCounterSampled = true;
          attempt.frameCounterAddress = progressingDuringPause->candidate->address;
          attempt.baselineStart = progressingDuringPause->candidate->second;
          attempt.baselineEnd = progressingDuringPause->candidate->third;
          attempt.pausedStart = progressingDuringPause->start;
          attempt.pausedEnd = progressingDuringPause->end;
          return false;
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

      CommandQueueCandidate selectedCandidate;
      if (!self)
      {
        RuntimeMemoryRegionListResult regions = listProcessMemoryRegions(processId);
        if (!regions.success)
        {
          attempt.reason = "unable to list process regions before command write: " + regions.reason;
          proof.attempts.push_back(attempt);
          copyAttemptToProof(attempt);
          continue;
        }

        std::string revalidationReason;
        if (!readSelectedCommandQueueCandidate(
              processId,
              regions.regions,
              *rankedCandidate.second,
              selectedCandidate,
              revalidationReason))
        {
          attempt.reason =
            "selected command queue candidate failed write-time revalidation: "
            + revalidationReason;
          proof.attempts.push_back(attempt);
          copyAttemptToProof(attempt);
          continue;
        }
        preserveLiveCommandCandidateDiagnostics(
          selectedCandidate,
          *rankedCandidate.second);

        std::string unsafeReason;
        if (!liveCommandCandidateSelectorSafeForWrite(
              selectedCandidate,
              executablePath,
              unsafeReason))
        {
          attempt.reason = unsafeReason;
          proof.attempts.push_back(attempt);
          copyAttemptToProof(attempt);
          continue;
        }

        attempt.commandQueue = selectedCandidate;
        attempt.bufferBegin = selectedCandidate.bufferBegin;
        attempt.originalUsedBytes = selectedCandidate.usedBytes;
      }
      else
      {
        selectedCandidate = *rankedCandidate.second;
      }

      if (!self)
      {
        attempt.frameCounterAddress = commandCounterCandidates.front().address;
        attempt.baselineStart = commandCounterCandidates.front().second;
        attempt.baselineEnd = commandCounterCandidates.front().third;

        bool staleProofBytesCleared = false;
        std::string staleProofByteCleanupReason;
        if (!clearAdapterPauseResumeProofBytes(
              processId,
              selectedCandidate,
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
        selectedCandidate,
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
        restoreCommandQueueAppendsIfStillPresent(processId, &append);
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
        restoreCommandQueueAppendsIfStillPresent(processId, &append);
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
        restoreCommandQueueAppendsIfStillPresent(processId, &append, &resumeAppend);
        attempt.reason = "unable to sample live frame counter after resume command";
        proof.attempts.push_back(attempt);
        copyAttemptToProof(attempt);
        continue;
      }

      const std::uint32_t resumedDelta = counterDelta(attempt.resumedStart, attempt.resumedEnd);
      if (resumedDelta < 2)
      {
        restoreCommandQueueAppendsIfStillPresent(processId, &append, &resumeAppend);
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
    output << "frame_counter_candidate_count\t"
           << proof.frameCounterCandidateCount << '\n';
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
    writeRequiredResidentAdapterFields(
      output,
      "issue_commands",
      "encoded-bwapi-command-reaches-live-scr-command-path-and-changes-frame-behavior",
      proof.liveDiagnostics);
    writeLiveCallableDiagnosticsFields(output, "issue_commands", proof.liveDiagnostics);
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
    int processId,
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
    proof.liveDiagnostics = discoverLiveCallableDiagnostics(
      processId,
      executablePath,
      {
        "DrawLayer_GameUnits",
        "DrawLayer_UI",
        "DrawLayer_Cursor",
        "glDrawElements",
        "glDrawArrays",
        "CGLGetCurrentContext"
      });
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
    writeRequiredResidentAdapterFields(
      output,
      "draw_overlays",
      "bwapi-overlay-primitives-render-on-visible-game-frame",
      proof.liveDiagnostics);
    writeLiveCallableDiagnosticsFields(output, "draw_overlays", proof.liveDiagnostics);
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
    int processId,
    const std::string& executablePath)
  {
    MultiplayerSyncProof proof;
    proof.commandQueueProven = issueCommandsProof.passed;
    proof.activeMatchProven = activeMatchReady;
    proof.replayLaunchDetected = replayLaunchDetected;
    proof.replayLaunchEvidence = std::move(replayLaunchEvidence);
    proof.replayOnly = replayLaunchDetected
      || (replayAnalysisProof.passed && replayAnalysisProof.currentProcessReplay);
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
    proof.liveDiagnostics = discoverLiveCallableDiagnostics(
      processId,
      executablePath,
      {
        "TLSNetworkConnection::Recv",
        "TLSNetworkConnection::Send",
        "GetTurnPackets",
        "netTurnRate"
      });
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
    writeRequiredResidentAdapterFields(
      output,
      "multiplayer_sync",
      "turn-packet-send-receive-sync-observed-in-live-battle-net-path",
      proof.liveDiagnostics);
    writeLiveCallableDiagnosticsFields(output, "multiplayer_sync", proof.liveDiagnostics);
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
    output << "sg_units_mem_attempted\t" << (diagnostics.sgUnitsMem.attempted ? "true" : "false") << '\n';
    output << "sg_units_mem_descriptor_address\t"
           << hexAddress(diagnostics.sgUnitsMem.descriptorAddress) << '\n';
    output << "sg_units_mem_descriptor_read\t"
           << (diagnostics.sgUnitsMem.descriptorRead ? "true" : "false") << '\n';
    if (!diagnostics.sgUnitsMem.descriptorReadReason.empty())
      output << "sg_units_mem_descriptor_read_reason\t"
             << diagnostics.sgUnitsMem.descriptorReadReason << '\n';
    output << "sg_units_mem_native_base\t" << hexAddress(diagnostics.sgUnitsMem.nativeBase) << '\n';
    output << "sg_units_mem_record_count\t" << diagnostics.sgUnitsMem.recordCount << '\n';
    output << "sg_units_mem_capacity\t" << diagnostics.sgUnitsMem.capacity << '\n';
    output << "sg_units_mem_region_found\t"
           << (diagnostics.sgUnitsMem.regionFound ? "true" : "false") << '\n';
    output << "sg_units_mem_region_address\t"
           << hexAddress(diagnostics.sgUnitsMem.regionAddress) << '\n';
    output << "sg_units_mem_region_size\t" << diagnostics.sgUnitsMem.regionSize << '\n';
    output << "sg_units_mem_region_readable\t"
           << (diagnostics.sgUnitsMem.regionReadable ? "true" : "false") << '\n';
    output << "sg_units_mem_region_writable\t"
           << (diagnostics.sgUnitsMem.regionWritable ? "true" : "false") << '\n';
    output << "sg_units_mem_region_executable\t"
           << (diagnostics.sgUnitsMem.regionExecutable ? "true" : "false") << '\n';
    output << "sg_units_mem_region_target_executable\t"
           << (diagnostics.sgUnitsMem.regionTargetExecutable ? "true" : "false") << '\n';
    output << "sg_units_mem_region_user_tag\t" << diagnostics.sgUnitsMem.regionUserTag << '\n';
    output << "sg_units_mem_region_share_mode\t" << diagnostics.sgUnitsMem.regionShareMode << '\n';
    output << "sg_units_mem_region_share_mode_name\t"
           << regionShareModeName(diagnostics.sgUnitsMem.regionShareMode) << '\n';
    if (!diagnostics.sgUnitsMem.regionMappedPath.empty())
      output << "sg_units_mem_region_mapped_path\t"
             << diagnostics.sgUnitsMem.regionMappedPath << '\n';
    output << "sg_units_mem_usable_storage\t"
           << (diagnostics.sgUnitsMem.usableStorage ? "true" : "false") << '\n';
    if (!diagnostics.sgUnitsMem.rejectionReason.empty())
      output << "sg_units_mem_rejection_reason\t"
             << diagnostics.sgUnitsMem.rejectionReason << '\n';
    output << "sg_units_mem_prefix_bytes_read\t"
           << diagnostics.sgUnitsMem.prefixBytesRead << '\n';
    output << "sg_units_mem_prefix_non_zero_bytes\t"
           << diagnostics.sgUnitsMem.prefixNonZeroBytes << '\n';
    output << "sg_units_mem_prefix_distinct_bytes\t"
           << diagnostics.sgUnitsMem.prefixDistinctBytes << '\n';
    output << "sg_units_mem_prefix_pointer_words\t"
           << diagnostics.sgUnitsMem.prefixPointerWords << '\n';
    if (!diagnostics.sgUnitsMem.prefixHex.empty())
      output << "sg_units_mem_prefix_hex\t" << diagnostics.sgUnitsMem.prefixHex << '\n';
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
    output << "dynamic_scan_sampled_regions\t" << diagnostics.dynamicSampledRegions << '\n';
    output << "dynamic_scan_sampled_bytes\t" << diagnostics.dynamicSampledBytes << '\n';
    output << "dynamic_scan_changed_regions\t" << diagnostics.dynamicChangedRegions << '\n';
    output << "dynamic_scan_changed_bytes\t" << diagnostics.dynamicChangedBytes << '\n';
    output << "dynamic_scan_windows_scored\t" << diagnostics.dynamicWindowsScored << '\n';
    output << "dynamic_scan_reason\t" << diagnostics.dynamicScanReason << '\n';
    output << "dynamic_region_sample_count\t" << diagnostics.dynamicRegionSamples.size() << '\n';
    for (std::size_t index = 0; index < diagnostics.dynamicRegionSamples.size(); ++index)
    {
      const UnitDynamicRegionDiagnostic& sample = diagnostics.dynamicRegionSamples[index];
      output << "dynamic_region_sample_" << index << "_address\t" << hexAddress(sample.address) << '\n';
      output << "dynamic_region_sample_" << index << "_size\t" << sample.size << '\n';
      output << "dynamic_region_sample_" << index << "_bytes_read\t" << sample.bytesRead << '\n';
      output << "dynamic_region_sample_" << index << "_changed_bytes\t" << sample.changedBytes << '\n';
      output << "dynamic_region_sample_" << index << "_changed_ranges\t" << sample.changedRanges << '\n';
      output << "dynamic_region_sample_" << index << "_first_changed_address\t"
             << hexAddress(sample.firstChangedAddress) << '\n';
      output << "dynamic_region_sample_" << index << "_first_changed_size\t"
             << sample.firstChangedSize << '\n';
      output << "dynamic_region_sample_" << index << "_priority\t" << sample.priority << '\n';
      output << "dynamic_region_sample_" << index << "_target_executable\t"
             << (sample.targetExecutable ? "true" : "false") << '\n';
      output << "dynamic_region_sample_" << index << "_user_tag\t" << sample.userTag << '\n';
      output << "dynamic_region_sample_" << index << "_share_mode\t" << sample.shareMode << '\n';
      output << "dynamic_region_sample_" << index << "_share_mode_name\t"
             << regionShareModeName(sample.shareMode) << '\n';
      if (!sample.mappedPath.empty())
        output << "dynamic_region_sample_" << index << "_mapped_path\t"
               << sample.mappedPath << '\n';
    }
    output << "dynamic_field_candidate_count\t" << diagnostics.dynamicFieldCandidates.size() << '\n';
    for (std::size_t index = 0; index < diagnostics.dynamicFieldCandidates.size(); ++index)
    {
      const UnitDynamicFieldCandidateDiagnostic& sample = diagnostics.dynamicFieldCandidates[index];
      output << "dynamic_field_candidate_" << index << "_address\t"
             << hexAddress(sample.address) << '\n';
      output << "dynamic_field_candidate_" << index << "_window_size\t"
             << sample.windowSize << '\n';
      output << "dynamic_field_candidate_" << index << "_changed_bytes\t"
             << sample.changedBytes << '\n';
      output << "dynamic_field_candidate_" << index << "_readable_pointer_words\t"
             << sample.readablePointerWords << '\n';
      output << "dynamic_field_candidate_" << index << "_tagged_handle_words\t"
             << sample.taggedHandleWords << '\n';
      output << "dynamic_field_candidate_" << index << "_coordinate_offset\t"
             << sample.coordinateOffset << '\n';
      output << "dynamic_field_candidate_" << index << "_x\t" << sample.x << '\n';
      output << "dynamic_field_candidate_" << index << "_y\t" << sample.y << '\n';
      output << "dynamic_field_candidate_" << index << "_hit_points_offset\t"
             << sample.hitPointsOffset << '\n';
      output << "dynamic_field_candidate_" << index << "_hit_points\t"
             << sample.hitPoints << '\n';
      output << "dynamic_field_candidate_" << index << "_player_offset\t"
             << sample.playerOffset << '\n';
      output << "dynamic_field_candidate_" << index << "_player\t" << sample.player << '\n';
      output << "dynamic_field_candidate_" << index << "_type_offset\t"
             << sample.typeOffset << '\n';
      output << "dynamic_field_candidate_" << index << "_type_hint\t"
             << sample.typeHint << '\n';
      output << "dynamic_field_candidate_" << index << "_prefix_hex\t"
             << sample.prefixHex << '\n';
    }
    output << "region_sample_count\t" << diagnostics.regionSamples.size() << '\n';
    for (std::size_t index = 0; index < diagnostics.regionSamples.size(); ++index)
    {
      const UnitScanRegionDiagnostic& sample = diagnostics.regionSamples[index];
      output << "region_sample_" << index << "_stage\t" << sample.stage << '\n';
      output << "region_sample_" << index << "_decision\t" << sample.decision << '\n';
      output << "region_sample_" << index << "_reason\t" << sample.reason << '\n';
      output << "region_sample_" << index << "_address\t" << hexAddress(sample.address) << '\n';
      output << "region_sample_" << index << "_size\t" << sample.size << '\n';
      output << "region_sample_" << index << "_bytes_read\t" << sample.bytesRead << '\n';
      output << "region_sample_" << index << "_priority\t" << sample.priority << '\n';
      output << "region_sample_" << index << "_readable\t" << (sample.readable ? "true" : "false") << '\n';
      output << "region_sample_" << index << "_writable\t" << (sample.writable ? "true" : "false") << '\n';
      output << "region_sample_" << index << "_executable\t" << (sample.executable ? "true" : "false") << '\n';
      output << "region_sample_" << index << "_target_executable\t"
             << (sample.targetExecutable ? "true" : "false") << '\n';
      output << "region_sample_" << index << "_user_tag\t" << sample.userTag << '\n';
      output << "region_sample_" << index << "_share_mode\t" << sample.shareMode << '\n';
      output << "region_sample_" << index << "_share_mode_name\t"
             << regionShareModeName(sample.shareMode) << '\n';
      if (!sample.mappedPath.empty())
        output << "region_sample_" << index << "_mapped_path\t" << sample.mappedPath << '\n';
    }
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
    output << "pointer_array_sample_count\t" << diagnostics.pointerArraySamples.size() << '\n';
    for (std::size_t index = 0; index < diagnostics.pointerArraySamples.size(); ++index)
    {
      const UnitPointerArrayCandidateDiagnostic& sample = diagnostics.pointerArraySamples[index];
      output << "pointer_array_sample_" << index << "_vector_address\t"
             << hexAddress(sample.vectorAddress) << '\n';
      output << "pointer_array_sample_" << index << "_begin\t" << hexAddress(sample.begin) << '\n';
      output << "pointer_array_sample_" << index << "_end\t" << hexAddress(sample.end) << '\n';
      output << "pointer_array_sample_" << index << "_capacity\t"
             << hexAddress(sample.capacity) << '\n';
      output << "pointer_array_sample_" << index << "_used_bytes\t" << sample.usedBytes << '\n';
      output << "pointer_array_sample_" << index << "_pointer_count\t" << sample.pointerCount << '\n';
      output << "pointer_array_sample_" << index << "_readable_pointers\t"
             << sample.readablePointers << '\n';
      output << "pointer_array_sample_" << index << "_record_snapshots\t"
             << sample.recordSnapshots << '\n';
      output << "pointer_array_sample_" << index << "_first_pointer\t"
             << hexAddress(sample.firstPointer) << '\n';
      output << "pointer_array_sample_" << index << "_second_pointer\t"
             << hexAddress(sample.secondPointer) << '\n';
      output << "pointer_array_sample_" << index << "_first_record_nonzero_bytes\t"
             << sample.firstRecordNonZeroBytes << '\n';
      output << "pointer_array_sample_" << index << "_first_record_pointer_words\t"
             << sample.firstRecordPointerWords << '\n';
      output << "pointer_array_sample_" << index << "_first_record_prefix_hex\t"
             << sample.firstRecordPrefixHex << '\n';
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
      output << "unit_node_field_sample_" << index << "_kind\t" << sample.kind << '\n';
      output << "unit_node_field_sample_" << index << "_rejection_reason\t"
             << sample.rejectionReason << '\n';
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
      output << "unit_node_field_sample_" << index << "_node_prefix_nonzero_bytes\t"
             << sample.nodePrefixNonZeroBytes << '\n';
      output << "unit_node_field_sample_" << index << "_sprite_prefix_nonzero_bytes\t"
             << sample.spritePrefixNonZeroBytes << '\n';
      output << "unit_node_field_sample_" << index << "_secondary_prefix_nonzero_bytes\t"
             << sample.secondaryPrefixNonZeroBytes << '\n';
      if (!sample.nodePrefixHex.empty())
        output << "unit_node_field_sample_" << index << "_node_prefix_hex\t"
               << sample.nodePrefixHex << '\n';
      if (!sample.spritePrefixHex.empty())
        output << "unit_node_field_sample_" << index << "_sprite_prefix_hex\t"
               << sample.spritePrefixHex << '\n';
      if (!sample.secondaryPrefixHex.empty())
        output << "unit_node_field_sample_" << index << "_secondary_prefix_hex\t"
               << sample.secondaryPrefixHex << '\n';
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

    output << "source\tcurrent_process_replay\tactive_match_metadata\tmap_name\tfirst_frame\tlast_frame\tobserved_player_count\n";
    output << proof.source << '\t'
           << (proof.currentProcessReplay ? "true" : "false") << '\t'
           << (proof.activeMatchMetadata ? "true" : "false") << '\t'
           << proof.mapName << '\t'
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
      std::uint32_t third = 0;
      std::uint64_t firstTick = 0;
      std::uint64_t secondTick = 0;
      int regionPriority = 3;
      int score = std::numeric_limits<int>::max();
    };
    struct Snapshot
    {
      std::uintptr_t address = 0;
      int regionPriority = 3;
      std::uint64_t tick = 0;
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
      // SC:R state can live in target image data, anonymous heap, or Rosetta's
      // writable file-backed runtime mappings. Read-only non-target files remain
      // excluded to avoid static-data false positives.
      if (!eligibleStateCounterScanRegion(region, executablePath, diagnostics))
        continue;
      if (region.size < sizeof(std::uint32_t))
        continue;
      if (scanned >= maxScanBytes)
      {
        if (diagnostics != nullptr)
          diagnostics->byteLimitReached = true;
        break;
      }

      for (std::size_t regionOffset = 0;
           regionOffset < region.size && scanned < maxScanBytes;
           regionOffset += maxRegionBytes)
      {
        if (timedOut(deadline))
        {
          if (diagnostics != nullptr)
            diagnostics->timedOut = true;
          return { false, 0, 0, 0, 0, "state counter scan timed out before proof" };
        }

        const std::size_t remainingRegionBytes = region.size - regionOffset;
        const std::size_t bytesToRead =
          std::min(remainingRegionBytes, std::min(maxRegionBytes, maxScanBytes - scanned));
        if (bytesToRead < sizeof(std::uint32_t))
          break;

        RuntimeMemoryReadResult first =
          readProcessMemory(processId, region.address + regionOffset, bytesToRead);
        if (!first.success || first.bytesRead < sizeof(std::uint32_t))
          break;

        Snapshot snapshot;
        snapshot.address = region.address + regionOffset;
        snapshot.regionPriority =
          unitScanRegionPriority(region, executablePath, targetImageBase);
        snapshot.tick = steadyTickMilliseconds();
        snapshot.bytes = std::move(first.bytes);
        snapshots.push_back(std::move(snapshot));
        if (diagnostics != nullptr)
        {
          ++diagnostics->scannedRegions;
          diagnostics->scannedBytes += snapshots.back().bytes.size();
        }
        scanned += first.bytesRead;
        if (first.bytesRead < bytesToRead)
          break;
      }
    }

    if (snapshots.empty())
      return { false, 0, 0, 0, 0, "no eligible runtime memory snapshots could be captured" };

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
      const std::uint64_t secondTick = steadyTickMilliseconds();

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
          candidate.firstTick = snapshot.tick;
          candidate.secondTick = secondTick;
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

    std::sort(
      candidates.begin(),
      candidates.end(),
      [&](const Candidate& lhs, const Candidate& rhs)
      {
        const int lhsScore = initialFrameCounterCandidateScore(
          lhs.first,
          lhs.second,
          lhs.regionPriority,
          sampleDelayMs);
        const int rhsScore = initialFrameCounterCandidateScore(
          rhs.first,
          rhs.second,
          rhs.regionPriority,
          sampleDelayMs);
        if (lhsScore != rhsScore)
          return lhsScore < rhsScore;
        return lhs.address < rhs.address;
      });

    LiveCounterProof bestProof;
    int bestScore = std::numeric_limits<int>::max();
    constexpr std::size_t maxFreshCounterCandidates = 512;
    const std::size_t candidatesToValidate =
      std::min<std::size_t>(candidates.size(), maxFreshCounterCandidates);
    std::this_thread::sleep_for(std::chrono::milliseconds(sampleDelayMs));
    for (std::size_t candidateIndex = 0; candidateIndex < candidatesToValidate; ++candidateIndex)
    {
      if (timedOut(deadline))
      {
        if (bestProof.passed)
          return bestProof;
        if (diagnostics != nullptr)
          diagnostics->timedOut = true;
        return { false, 0, 0, 0, 0, "state counter scan timed out before proof" };
      }

      const Candidate& candidate = candidates[candidateIndex];
      RuntimeMemoryReadResult third =
        readProcessMemory(processId, candidate.address, sizeof(std::uint32_t));
      if (!third.success || third.bytesRead != sizeof(std::uint32_t))
        continue;
      const std::uint64_t thirdTick = steadyTickMilliseconds();
      const std::uint32_t thirdValue = readU32(third.bytes, 0);

      const int score =
        candidate.regionPriority * 1000000
        + frameCounterScore(
          candidate.first,
          candidate.second,
          thirdValue,
          sampleDelayMs);
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
          bestProof = {
            true,
            candidate.address,
            candidate.first,
            candidate.second,
            thirdValue,
            {}
          };
          bestProof.firstTick = candidate.firstTick;
          bestProof.secondTick = candidate.secondTick;
          bestProof.thirdTick = thirdTick;
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
      std::uint32_t third = 0;
      int regionPriority = 3;
      int score = std::numeric_limits<int>::max();
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
      if (!eligibleStateCounterScanRegion(region, executablePath))
        continue;
      if (region.size < sizeof(std::uint32_t))
        continue;

      for (std::size_t regionOffset = 0;
           regionOffset < region.size && scanned < maxScanBytes;
           regionOffset += maxRegionBytes)
      {
        if (timedOut(deadline))
          break;

        const std::size_t remainingRegionBytes = region.size - regionOffset;
        const std::size_t bytesToRead =
          std::min(remainingRegionBytes, std::min(maxRegionBytes, maxScanBytes - scanned));
        if (bytesToRead < sizeof(std::uint32_t))
          break;

        RuntimeMemoryReadResult read =
          readProcessMemory(processId, region.address + regionOffset, bytesToRead);
        if (!read.success || read.bytesRead < sizeof(std::uint32_t))
          break;

        Snapshot snapshot;
        snapshot.address = region.address + regionOffset;
        snapshot.regionPriority = unitScanRegionPriority(region, executablePath, targetImageBase);
        snapshot.bytes = std::move(read.bytes);
        scanned += read.bytesRead;
        snapshots.push_back(std::move(snapshot));
        if (read.bytesRead < bytesToRead)
          break;
      }
    }

    if (snapshots.empty())
      return result;

    std::this_thread::sleep_for(std::chrono::milliseconds(sampleDelayMs));

    std::vector<Candidate> candidates;
    constexpr std::size_t maxPreliminaryCounterCandidates = 32768;
    for (const Snapshot& snapshot : snapshots)
    {
      if (timedOut(deadline) || candidates.size() >= maxPreliminaryCounterCandidates)
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
        candidate.score = initialFrameCounterCandidateScore(
          candidate.first,
          candidate.second,
          candidate.regionPriority,
          sampleDelayMs);
        candidates.push_back(candidate);
        if (candidates.size() >= maxPreliminaryCounterCandidates)
          break;
      }
    }

    if (candidates.empty())
      return result;

    std::sort(
      candidates.begin(),
      candidates.end(),
      [&](const Candidate& lhs, const Candidate& rhs)
      {
        if (lhs.score != rhs.score)
          return lhs.score < rhs.score;
        return lhs.address < rhs.address;
      });

    const std::size_t validationLimit =
      std::min<std::size_t>(
        candidates.size(),
        std::max<std::size_t>(512, maxCandidates * 64));
    std::vector<std::size_t> validationOrder;
    validationOrder.reserve(validationLimit);
    std::unordered_set<std::uint64_t> selectedBuckets;
    std::unordered_set<std::uintptr_t> selectedAddresses;

    const auto bucketKey =
      [](const Candidate& candidate) -> std::uint64_t
      {
        return (static_cast<std::uint64_t>(candidate.regionPriority) << 56)
          ^ (static_cast<std::uint64_t>(candidate.address) >> 20);
      };

    for (std::size_t i = 0; i < candidates.size() && validationOrder.size() < validationLimit; ++i)
    {
      const std::uint64_t bucket = bucketKey(candidates[i]);
      if (!selectedBuckets.insert(bucket).second)
        continue;
      validationOrder.push_back(i);
      selectedAddresses.insert(candidates[i].address);
    }
    for (std::size_t i = 0; i < candidates.size() && validationOrder.size() < validationLimit; ++i)
    {
      if (!selectedAddresses.insert(candidates[i].address).second)
        continue;
      validationOrder.push_back(i);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(sampleDelayMs));

    for (std::size_t candidateIndex : validationOrder)
    {
      if (timedOut(deadline))
        break;

      Candidate& candidate = candidates[candidateIndex];
      RuntimeMemoryReadResult third =
        readProcessMemory(processId, candidate.address, sizeof(std::uint32_t));
      if (!third.success || third.bytesRead != sizeof(std::uint32_t))
        continue;

      candidate.third = readU32(third.bytes, 0);
      if (!frameCounterConfidencePassed(
          candidate.first,
          candidate.second,
          candidate.third,
          sampleDelayMs))
        continue;

      FrameCounterCandidate frameCandidate;
      frameCandidate.address = candidate.address;
      frameCandidate.first = candidate.first;
      frameCandidate.second = candidate.second;
      frameCandidate.third = candidate.third;
      frameCandidate.score =
        candidate.regionPriority * 1000000
        + frameCounterScore(
          candidate.first,
          candidate.second,
          candidate.third,
          sampleDelayMs);
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
    {
      std::vector<FrameCounterCandidate> diverse;
      diverse.reserve(maxCandidates);
      selectedBuckets.clear();
      selectedAddresses.clear();
      for (const FrameCounterCandidate& candidate : result)
      {
        const std::uint64_t bucket = static_cast<std::uint64_t>(candidate.address) >> 20;
        if (!selectedBuckets.insert(bucket).second)
          continue;
        diverse.push_back(candidate);
        selectedAddresses.insert(candidate.address);
        if (diverse.size() >= maxCandidates)
          break;
      }
      for (const FrameCounterCandidate& candidate : result)
      {
        if (diverse.size() >= maxCandidates)
          break;
        if (!selectedAddresses.insert(candidate.address).second)
          continue;
        diverse.push_back(candidate);
      }
      result = std::move(diverse);
    }
    return result;
  }

  struct SelfUnitFixture
  {
    alignas(8) std::array<std::array<unsigned char, 336>, 16> records;
    alignas(8) std::array<std::array<unsigned char, 64>, 16> sprites;
  };

  struct SelfUnitNodeFixture
  {
    alignas(8) std::array<std::array<unsigned char, 0x58>, 8> nodes;
    alignas(8) std::array<std::array<unsigned char, 0x50>, 8> secondaryObjects;
    alignas(8) std::array<std::array<unsigned char, 64>, 8> sprites;
  };

  struct SelfCompactUnitNodeFixture
  {
    alignas(8) std::array<std::array<unsigned char, 0x40>, 8> nodes;
    alignas(8) std::array<std::array<unsigned char, 0xe0>, 8> secondaryObjects;
    alignas(8) std::array<std::array<unsigned char, 0xd0>, 8> sprites;
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

  void initializeSelfUnitFixture(SelfUnitFixture& fixture)
  {
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

  void initializeSelfCompactUnitNodeFixture(SelfCompactUnitNodeFixture& fixture)
  {
    for (std::size_t i = 0; i < fixture.nodes.size(); ++i)
    {
      auto& node = fixture.nodes[i];
      auto& secondary = fixture.secondaryObjects[i];
      auto& sprite = fixture.sprites[i];
      node.fill(0);
      secondary.fill(0);
      sprite.fill(static_cast<unsigned char>(0x30 + i));

      const std::uintptr_t previous = i == 0
        ? 0
        : reinterpret_cast<std::uintptr_t>(fixture.nodes[i - 1].data());
      const std::uintptr_t next = i + 1 >= fixture.nodes.size()
        ? 0
        : reinterpret_cast<std::uintptr_t>(fixture.nodes[i + 1].data());
      writeU64(node, 0, static_cast<std::uint64_t>(previous));
      writeU64(node, 0x08, static_cast<std::uint64_t>(next));
      writeS32(node, 0x10, static_cast<std::int32_t>(224 + i * 18));
      writeS32(node, 0x14, static_cast<std::int32_t>(192 + i * 12));
      writeU64(
        node,
        0x18,
        static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(sprite.data())));
      writeU64(
        node,
        0x20,
        static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(secondary.data())));

      writeU32(sprite, 0x68, static_cast<std::uint32_t>((i % 228) + 1));
      writeU32(sprite, 0x6c, static_cast<std::uint32_t>(i % 4));
      writeU32(sprite, 0x80, 256u * static_cast<std::uint32_t>(55 + i));

      writeU16(secondary, 0x10, static_cast<std::uint16_t>((i % 228) + 1));
      secondary[0x14] = static_cast<unsigned char>(i % 4);
      secondary[0x1a] = static_cast<unsigned char>(55 + i);
      secondary[0x1b] = static_cast<unsigned char>(64 + i);
      writeU16(secondary, 0x20, static_cast<std::uint16_t>((i % 228) + 1));
      secondary[0xc0] = static_cast<unsigned char>(i % 4);
      writeU16(secondary, 0xd0, static_cast<std::uint16_t>((i % 228) + 1));
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
  bool selfCompactUnitNodeFixture = false;
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
  std::uintptr_t commandQueueBufferAddress = 0;
  std::size_t issueCommandCandidateScanLimit = 0;
  std::string appendGameAction;
  std::string aiModulePath;
  int stateSampleDelayMs = 250;
  std::size_t stateMaxScanBytes = 256 * 1024 * 1024;
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
    if (arg == "--self-compact-unit-node-fixture")
    {
      selfCompactUnitNodeFixture = true;
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
    if (arg == "--command-queue-buffer-address")
    {
      if (i + 1 >= argc || !parseAddress(argv[++i], commandQueueBufferAddress))
      {
        std::cerr << "--command-queue-buffer-address requires a positive integer address\n";
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
    if (commandQueueBufferAddress != 0)
    {
      candidate.storageKind = "raw-turn-buffer";
      candidate.vectorAddress = commandQueueVectorAddress;
      candidate.bytesInQueueAddress = commandQueueVectorAddress;
      candidate.bufferBegin = commandQueueBufferAddress;
      candidate.bufferCapacity = commandQueueBufferAddress + rawTurnBufferCapacity;
      candidate.capacityBytes = rawTurnBufferCapacity;
      candidate.counterOffset =
        commandQueueVectorAddress >= commandQueueBufferAddress
          ? commandQueueVectorAddress - commandQueueBufferAddress
          : commandQueueBufferAddress - commandQueueVectorAddress;
    }
    else
    {
      candidate.vectorAddress = commandQueueVectorAddress;
    }
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
    if (commandQueueBufferAddress != 0)
    {
      std::cout << "manual_command_append.storage_kind=raw-turn-buffer\n";
      std::cout << "manual_command_append.bytes_in_queue_address="
                << hexAddress(commandQueueVectorAddress) << '\n';
      std::cout << "manual_command_append.requested_buffer_address="
                << hexAddress(commandQueueBufferAddress) << '\n';
    }
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
      if (stateCounterAddress != 0)
      {
        std::uint32_t progressStart = 0;
        std::uint32_t progressEnd = 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        const bool sampled = sampleProgressingFrameCounter(
          environment.processId,
          stateCounterAddress,
          stateSampleDelayMs,
          6,
          2,
          progressStart,
          progressEnd);
        const std::uint32_t progressDelta = counterDelta(progressStart, progressEnd);
        std::cout << "manual_command_append.frame_counter_address="
                  << hexAddress(stateCounterAddress) << '\n';
        std::cout << "manual_command_append.frame_counter_sampled="
                  << (sampled ? "true" : "false") << '\n';
        std::cout << "manual_command_append.frame_counter_delta="
                  << progressDelta << '\n';
        if (!sampled || progressDelta < 2)
        {
          const bool restored =
            restoreCommandQueueAppendsIfStillPresent(environment.processId, &append);
          std::cout << "manual_command_append.restored_after_no_progress="
                    << (restored ? "true" : "false") << '\n';
          return 12;
        }
      }
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
  SelfCompactUnitNodeFixture compactUnitNodeFixture;
  SelfBulletFixture bulletFixture;
  SelfCommandQueueFixture commandQueueFixture;
  int proofFailureCode = 0;
  bool readGameStateUsedExplicitAddress = false;
  bool readGameStateUsedFallbackScan = false;
  std::string explicitFrameCounterFailure;
  bool remasteredUnitNodeSnapshotAttempted = false;
  std::string remasteredUnitNodeSnapshotFailure;
  if (self && selfUnitFixture)
  {
    initializeSelfUnitFixture(unitFixture);
    const std::uintptr_t fixtureAddress =
      reinterpret_cast<std::uintptr_t>(unitFixture.records.front().data());
    unitCandidateAddresses.erase(
      std::remove(unitCandidateAddresses.begin(), unitCandidateAddresses.end(), fixtureAddress),
      unitCandidateAddresses.end());
    unitCandidateAddresses.insert(unitCandidateAddresses.begin(), fixtureAddress);
  }
  if (self && selfUnitNodeFixture)
  {
    initializeSelfUnitNodeFixture(unitNodeFixture);
    unitNodeCandidateAddresses.push_back(
      reinterpret_cast<std::uintptr_t>(unitNodeFixture.nodes.front().data()));
  }
  if (self && selfCompactUnitNodeFixture)
  {
    initializeSelfCompactUnitNodeFixture(compactUnitNodeFixture);
    unitNodeCandidateAddresses.push_back(
      reinterpret_cast<std::uintptr_t>(compactUnitNodeFixture.nodes.front().data()));
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
      std::string explicitAddressGuardReason;
      if (!stateCounterAddressAllowed(
            environment.processId,
            stateCounterAddress,
            environment.executablePath,
            explicitAddressGuardReason))
      {
        readGameStateProof.reason = explicitAddressGuardReason;
      }
      else
      {
        readGameStateProof = proveExplicitLiveCounterRead(
          environment.processId,
          stateCounterAddress,
          stateSampleDelayMs);
      }
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
      std::cout << "read_game_state.scan.readable_candidate_regions="
                << stateScanDiagnostics.readableCandidateRegions << '\n';
      std::cout << "read_game_state.scan.readable_writable_regions="
                << stateScanDiagnostics.readableWritableRegions << '\n';
      std::cout << "read_game_state.scan.readable_writable_file_backed_regions="
                << stateScanDiagnostics.readableWritableFileBackedRegions << '\n';
      std::cout << "read_game_state.scan.skipped_executable_regions="
                << stateScanDiagnostics.skippedExecutableRegions << '\n';
      std::cout << "read_game_state.scan.skipped_non_readable_regions="
                << stateScanDiagnostics.skippedNonReadableRegions << '\n';
      std::cout << "read_game_state.scan.skipped_non_writable_regions="
                << stateScanDiagnostics.skippedNonWritableRegions << '\n';
      std::cout << "read_game_state.scan.skipped_file_backed_read_only_regions="
                << stateScanDiagnostics.skippedFileBackedReadOnlyRegions << '\n';
      std::cout << "read_game_state.scan.skipped_file_backed_non_target_regions="
                << stateScanDiagnostics.skippedFileBackedNonTargetRegions << '\n';
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
          && (!self || selfUnitNodeFixture || selfCompactUnitNodeFixture)
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
          unitScanIncludeImageRegionsFlag,
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
          && (!self || selfUnitNodeFixture || selfCompactUnitNodeFixture)
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
      if (proveReadUnits
          && !readUnitsProof.passed
          && !self
          && environment.product == Product::StarCraftRemastered)
      {
        const std::string previousReadUnitsReason = readUnitsProof.reason;
        LiveUnitsProof sgUnitsMemProof = proveRemasteredSgUnitsMemCUnitArray(
          environment.processId,
          environment.executablePath,
          unitMaxScanBytes,
          unitScanTimeoutMs,
          unitScanDiagnosticsFlag ? &unitScanDiagnostics : nullptr);
        if (sgUnitsMemProof.passed)
        {
          readUnitsProof = std::move(sgUnitsMemProof);
        }
        else
        {
          if (!previousReadUnitsReason.empty())
            sgUnitsMemProof.reason += "; remastered_unit_node_reason=" + previousReadUnitsReason;
          readUnitsProof = std::move(sgUnitsMemProof);
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
      if (unitScanDiagnosticsFlag && unitScanDiagnostics.sgUnitsMem.attempted)
      {
        const SgUnitsMemDiagnostic& sgUnitsMem = unitScanDiagnostics.sgUnitsMem;
        std::cout << "read_units.sg_units_mem.attempted=true\n";
        std::cout << "read_units.sg_units_mem.descriptor_address=0x"
                  << std::hex << sgUnitsMem.descriptorAddress << std::dec << '\n';
        std::cout << "read_units.sg_units_mem.descriptor_read="
                  << (sgUnitsMem.descriptorRead ? "true" : "false") << '\n';
        if (!sgUnitsMem.descriptorReadReason.empty())
          std::cout << "read_units.sg_units_mem.descriptor_read_reason="
                    << sgUnitsMem.descriptorReadReason << '\n';
        std::cout << "read_units.sg_units_mem.native_base=0x"
                  << std::hex << sgUnitsMem.nativeBase << std::dec << '\n';
        std::cout << "read_units.sg_units_mem.record_count="
                  << sgUnitsMem.recordCount << '\n';
        std::cout << "read_units.sg_units_mem.capacity="
                  << sgUnitsMem.capacity << '\n';
        std::cout << "read_units.sg_units_mem.region_found="
                  << (sgUnitsMem.regionFound ? "true" : "false") << '\n';
        if (sgUnitsMem.regionFound)
        {
          std::cout << "read_units.sg_units_mem.region_address=0x"
                    << std::hex << sgUnitsMem.regionAddress << std::dec << '\n';
          std::cout << "read_units.sg_units_mem.region_size="
                    << sgUnitsMem.regionSize << '\n';
          std::cout << "read_units.sg_units_mem.region_readable="
                    << (sgUnitsMem.regionReadable ? "true" : "false") << '\n';
          std::cout << "read_units.sg_units_mem.region_writable="
                    << (sgUnitsMem.regionWritable ? "true" : "false") << '\n';
          std::cout << "read_units.sg_units_mem.region_executable="
                    << (sgUnitsMem.regionExecutable ? "true" : "false") << '\n';
          std::cout << "read_units.sg_units_mem.region_target_executable="
                    << (sgUnitsMem.regionTargetExecutable ? "true" : "false") << '\n';
          std::cout << "read_units.sg_units_mem.region_user_tag="
                    << sgUnitsMem.regionUserTag << '\n';
          std::cout << "read_units.sg_units_mem.region_share_mode="
                    << sgUnitsMem.regionShareMode << '\n';
          std::cout << "read_units.sg_units_mem.region_share_mode_name="
                    << regionShareModeName(sgUnitsMem.regionShareMode) << '\n';
          if (!sgUnitsMem.regionMappedPath.empty())
            std::cout << "read_units.sg_units_mem.region_mapped_path="
                      << sgUnitsMem.regionMappedPath << '\n';
        }
        std::cout << "read_units.sg_units_mem.usable_storage="
                  << (sgUnitsMem.usableStorage ? "true" : "false") << '\n';
        if (!sgUnitsMem.rejectionReason.empty())
          std::cout << "read_units.sg_units_mem.rejection_reason="
                    << sgUnitsMem.rejectionReason << '\n';
        std::cout << "read_units.sg_units_mem.prefix_bytes_read="
                  << sgUnitsMem.prefixBytesRead << '\n';
        std::cout << "read_units.sg_units_mem.prefix_non_zero_bytes="
                  << sgUnitsMem.prefixNonZeroBytes << '\n';
        std::cout << "read_units.sg_units_mem.prefix_distinct_bytes="
                  << sgUnitsMem.prefixDistinctBytes << '\n';
        std::cout << "read_units.sg_units_mem.prefix_pointer_words="
                  << sgUnitsMem.prefixPointerWords << '\n';
        if (!sgUnitsMem.prefixHex.empty())
          std::cout << "read_units.sg_units_mem.prefix_hex="
                    << sgUnitsMem.prefixHex << '\n';
      }
    }

    if (proveActiveMatchState)
    {
      const bool frameGatePassed = hasResidentGameStateProofTicks(readGameStateProof);
      const bool replayMetadataAvailable =
        frameGatePassed
        && replayLaunchDetected
        && mapDataProof.passed
        && !mapDataProof.replayPath.empty()
        && mapDataProof.replayFileSize > 0;
      const bool liveUnitProofAvailable = readUnitsProof.passed;
      const bool activeMatchProven =
        !self
        && frameGatePassed
        && liveUnitProofAvailable;
      const char* activeMatchEvidence =
        readUnitsProof.passed
          ? (readUnitsProof.derivedSnapshot ? "active-unit-node-snapshot" : "active-unit-records")
          : "none";
      std::cout << "active_match_state.in_game=" << (activeMatchProven ? "true" : "false") << '\n';
      std::cout << "active_match_state.evidence=" << activeMatchEvidence << '\n';
      if (replayMetadataAvailable)
      {
        std::cout << "active_match_state.replay_metadata_available=true\n";
        std::cout << "active_match_state.replay_metadata_scope=diagnostic-only-not-active-match-proof\n";
      }
      if (!frameGatePassed)
        std::cout << "active_match_state.reason=active match proof requires resident live frame/tick progression\n";
      else if (!liveUnitProofAvailable)
        std::cout << "active_match_state.reason=active match proof requires validated read-units evidence\n";
      if (readUnitsProof.passed)
      {
        std::cout << "active_match_state.active_records=" << readUnitsProof.activeRecords << '\n';
        if (readUnitsProof.derivedSnapshot)
        {
          std::cout << "active_match_state.unit_node_address=0x"
                    << std::hex << readUnitsProof.address << std::dec << '\n';
          std::cout << "active_match_state.unit_node_record_size="
                    << readUnitsProof.recordSize << '\n';
        }
        else
        {
          std::cout << "active_match_state.unit_array_address=0x"
                    << std::hex << readUnitsProof.address << std::dec << '\n';
        }
      }
      else if (replayMetadataAvailable)
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
      std::cout << "read_units.dynamic_scan.sampled_regions="
                << unitScanDiagnostics.dynamicSampledRegions << '\n';
      std::cout << "read_units.dynamic_scan.sampled_bytes="
                << unitScanDiagnostics.dynamicSampledBytes << '\n';
      std::cout << "read_units.dynamic_scan.changed_regions="
                << unitScanDiagnostics.dynamicChangedRegions << '\n';
      std::cout << "read_units.dynamic_scan.changed_bytes="
                << unitScanDiagnostics.dynamicChangedBytes << '\n';
      std::cout << "read_units.dynamic_scan.windows_scored="
                << unitScanDiagnostics.dynamicWindowsScored << '\n';
      if (!unitScanDiagnostics.dynamicScanReason.empty())
        std::cout << "read_units.dynamic_scan.reason="
                  << unitScanDiagnostics.dynamicScanReason << '\n';
      std::cout << "read_units.dynamic_scan.region_sample_count="
                << unitScanDiagnostics.dynamicRegionSamples.size() << '\n';
      for (std::size_t index = 0; index < unitScanDiagnostics.dynamicRegionSamples.size(); ++index)
      {
        const UnitDynamicRegionDiagnostic& sample =
          unitScanDiagnostics.dynamicRegionSamples[index];
        std::cout << "read_units.dynamic_scan.region_sample." << index << ".address=0x"
                  << std::hex << sample.address << std::dec << '\n';
        std::cout << "read_units.dynamic_scan.region_sample." << index << ".size="
                  << sample.size << '\n';
        std::cout << "read_units.dynamic_scan.region_sample." << index << ".bytes_read="
                  << sample.bytesRead << '\n';
        std::cout << "read_units.dynamic_scan.region_sample." << index << ".changed_bytes="
                  << sample.changedBytes << '\n';
        std::cout << "read_units.dynamic_scan.region_sample." << index << ".changed_ranges="
                  << sample.changedRanges << '\n';
        std::cout << "read_units.dynamic_scan.region_sample." << index << ".first_changed_address=0x"
                  << std::hex << sample.firstChangedAddress << std::dec << '\n';
        std::cout << "read_units.dynamic_scan.region_sample." << index << ".first_changed_size="
                  << sample.firstChangedSize << '\n';
        std::cout << "read_units.dynamic_scan.region_sample." << index << ".priority="
                  << sample.priority << '\n';
        std::cout << "read_units.dynamic_scan.region_sample." << index << ".target_executable="
                  << (sample.targetExecutable ? "true" : "false") << '\n';
        std::cout << "read_units.dynamic_scan.region_sample." << index << ".user_tag="
                  << sample.userTag << '\n';
        std::cout << "read_units.dynamic_scan.region_sample." << index << ".share_mode="
                  << sample.shareMode << '\n';
        std::cout << "read_units.dynamic_scan.region_sample." << index << ".share_mode_name="
                  << regionShareModeName(sample.shareMode) << '\n';
        if (!sample.mappedPath.empty())
          std::cout << "read_units.dynamic_scan.region_sample." << index << ".mapped_path="
                    << sample.mappedPath << '\n';
      }
      std::cout << "read_units.dynamic_scan.field_candidate_count="
                << unitScanDiagnostics.dynamicFieldCandidates.size() << '\n';
      for (std::size_t index = 0; index < unitScanDiagnostics.dynamicFieldCandidates.size(); ++index)
      {
        const UnitDynamicFieldCandidateDiagnostic& sample =
          unitScanDiagnostics.dynamicFieldCandidates[index];
        std::cout << "read_units.dynamic_scan.field_candidate." << index << ".address=0x"
                  << std::hex << sample.address << std::dec << '\n';
        std::cout << "read_units.dynamic_scan.field_candidate." << index << ".window_size="
                  << sample.windowSize << '\n';
        std::cout << "read_units.dynamic_scan.field_candidate." << index << ".changed_bytes="
                  << sample.changedBytes << '\n';
        std::cout << "read_units.dynamic_scan.field_candidate." << index << ".readable_pointer_words="
                  << sample.readablePointerWords << '\n';
        std::cout << "read_units.dynamic_scan.field_candidate." << index << ".tagged_handle_words="
                  << sample.taggedHandleWords << '\n';
        std::cout << "read_units.dynamic_scan.field_candidate." << index << ".coordinate_offset="
                  << sample.coordinateOffset << '\n';
        std::cout << "read_units.dynamic_scan.field_candidate." << index << ".x="
                  << sample.x << '\n';
        std::cout << "read_units.dynamic_scan.field_candidate." << index << ".y="
                  << sample.y << '\n';
        std::cout << "read_units.dynamic_scan.field_candidate." << index << ".hit_points_offset="
                  << sample.hitPointsOffset << '\n';
        std::cout << "read_units.dynamic_scan.field_candidate." << index << ".hit_points="
                  << sample.hitPoints << '\n';
        std::cout << "read_units.dynamic_scan.field_candidate." << index << ".player_offset="
                  << sample.playerOffset << '\n';
        std::cout << "read_units.dynamic_scan.field_candidate." << index << ".player="
                  << sample.player << '\n';
        std::cout << "read_units.dynamic_scan.field_candidate." << index << ".type_offset="
                  << sample.typeOffset << '\n';
        std::cout << "read_units.dynamic_scan.field_candidate." << index << ".type_hint="
                  << sample.typeHint << '\n';
        std::cout << "read_units.dynamic_scan.field_candidate." << index << ".prefix_hex="
                  << sample.prefixHex << '\n';
      }
      std::cout << "read_units.scan.region_sample_count="
                << unitScanDiagnostics.regionSamples.size() << '\n';
      for (std::size_t index = 0; index < unitScanDiagnostics.regionSamples.size(); ++index)
      {
        const UnitScanRegionDiagnostic& sample = unitScanDiagnostics.regionSamples[index];
        std::cout << "read_units.scan.region_sample." << index << ".stage="
                  << sample.stage << '\n';
        std::cout << "read_units.scan.region_sample." << index << ".decision="
                  << sample.decision << '\n';
        std::cout << "read_units.scan.region_sample." << index << ".reason="
                  << sample.reason << '\n';
        std::cout << "read_units.scan.region_sample." << index << ".address=0x"
                  << std::hex << sample.address << std::dec << '\n';
        std::cout << "read_units.scan.region_sample." << index << ".size="
                  << sample.size << '\n';
        std::cout << "read_units.scan.region_sample." << index << ".bytes_read="
                  << sample.bytesRead << '\n';
        std::cout << "read_units.scan.region_sample." << index << ".priority="
                  << sample.priority << '\n';
        std::cout << "read_units.scan.region_sample." << index << ".readable="
                  << (sample.readable ? "true" : "false") << '\n';
        std::cout << "read_units.scan.region_sample." << index << ".writable="
                  << (sample.writable ? "true" : "false") << '\n';
        std::cout << "read_units.scan.region_sample." << index << ".executable="
                  << (sample.executable ? "true" : "false") << '\n';
        std::cout << "read_units.scan.region_sample." << index << ".target_executable="
                  << (sample.targetExecutable ? "true" : "false") << '\n';
        std::cout << "read_units.scan.region_sample." << index << ".user_tag="
                  << sample.userTag << '\n';
        std::cout << "read_units.scan.region_sample." << index << ".share_mode="
                  << sample.shareMode << '\n';
        std::cout << "read_units.scan.region_sample." << index << ".share_mode_name="
                  << regionShareModeName(sample.shareMode) << '\n';
        if (!sample.mappedPath.empty())
          std::cout << "read_units.scan.region_sample." << index << ".mapped_path="
                    << sample.mappedPath << '\n';
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
      std::cout << "read_units.scan.pointer_array_sample_count="
                << unitScanDiagnostics.pointerArraySamples.size() << '\n';
      for (std::size_t index = 0; index < unitScanDiagnostics.pointerArraySamples.size(); ++index)
      {
        const UnitPointerArrayCandidateDiagnostic& sample =
          unitScanDiagnostics.pointerArraySamples[index];
        std::cout << "read_units.scan.pointer_array_sample." << index << ".vector_address=0x"
                  << std::hex << sample.vectorAddress << std::dec << '\n';
        std::cout << "read_units.scan.pointer_array_sample." << index << ".begin=0x"
                  << std::hex << sample.begin << std::dec << '\n';
        std::cout << "read_units.scan.pointer_array_sample." << index << ".end=0x"
                  << std::hex << sample.end << std::dec << '\n';
        std::cout << "read_units.scan.pointer_array_sample." << index << ".capacity=0x"
                  << std::hex << sample.capacity << std::dec << '\n';
        std::cout << "read_units.scan.pointer_array_sample." << index << ".used_bytes="
                  << sample.usedBytes << '\n';
        std::cout << "read_units.scan.pointer_array_sample." << index << ".pointer_count="
                  << sample.pointerCount << '\n';
        std::cout << "read_units.scan.pointer_array_sample." << index << ".readable_pointers="
                  << sample.readablePointers << '\n';
        std::cout << "read_units.scan.pointer_array_sample." << index << ".record_snapshots="
                  << sample.recordSnapshots << '\n';
        std::cout << "read_units.scan.pointer_array_sample." << index << ".first_pointer=0x"
                  << std::hex << sample.firstPointer << std::dec << '\n';
        std::cout << "read_units.scan.pointer_array_sample." << index << ".second_pointer=0x"
                  << std::hex << sample.secondPointer << std::dec << '\n';
        std::cout << "read_units.scan.pointer_array_sample." << index << ".first_record_nonzero_bytes="
                  << sample.firstRecordNonZeroBytes << '\n';
        std::cout << "read_units.scan.pointer_array_sample." << index << ".first_record_pointer_words="
                  << sample.firstRecordPointerWords << '\n';
        std::cout << "read_units.scan.pointer_array_sample." << index << ".first_record_prefix_hex="
                  << sample.firstRecordPrefixHex << '\n';
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
        std::cout << "read_units.unit_node_scan.field_sample." << index << ".kind="
                  << sample.kind << '\n';
        std::cout << "read_units.unit_node_scan.field_sample." << index << ".rejection_reason="
                  << sample.rejectionReason << '\n';
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
        std::cout << "read_units.unit_node_scan.field_sample." << index << ".node_prefix_nonzero_bytes="
                  << sample.nodePrefixNonZeroBytes << '\n';
        std::cout << "read_units.unit_node_scan.field_sample." << index << ".sprite_prefix_nonzero_bytes="
                  << sample.spritePrefixNonZeroBytes << '\n';
        std::cout << "read_units.unit_node_scan.field_sample." << index << ".secondary_prefix_nonzero_bytes="
                  << sample.secondaryPrefixNonZeroBytes << '\n';
        if (!sample.nodePrefixHex.empty())
          std::cout << "read_units.unit_node_scan.field_sample." << index << ".node_prefix_hex="
                    << sample.nodePrefixHex << '\n';
        if (!sample.spritePrefixHex.empty())
          std::cout << "read_units.unit_node_scan.field_sample." << index << ".sprite_prefix_hex="
                    << sample.spritePrefixHex << '\n';
        if (!sample.secondaryPrefixHex.empty())
          std::cout << "read_units.unit_node_scan.field_sample." << index << ".secondary_prefix_hex="
                    << sample.secondaryPrefixHex << '\n';
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
      && hasResidentGameStateProofTicks(readGameStateProof)
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
        environment.executablePath,
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
    if (!bulletDataProof.passed && environment.product == Product::StarCraftRemastered)
    {
      BulletDataProof staticBulletProof =
        proveStaticBulletAdapterCompatibility(environment.executablePath);
      if (staticBulletProof.staticAdapterAnchorsResolved)
      {
        staticBulletProof.reason = bulletDataProof.reason.empty()
          ? staticBulletProof.reason
          : bulletDataProof.reason + "; " + staticBulletProof.reason;
        bulletDataProof = std::move(staticBulletProof);
      }
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
    if (bulletDataProof.staticAdapterAnchorsResolved)
    {
      std::cout << "read_bullet_data.static_adapter_anchors_resolved=true\n";
      std::cout << "read_bullet_data.static_layout=" << bulletDataProof.layoutName << '\n';
      std::cout << "read_bullet_data.static_record_size=" << bulletDataProof.recordSize << '\n';
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
      std::cout << "replay_analysis.source=" << replayAnalysisProof.source << '\n';
      std::cout << "replay_analysis.current_process_replay="
                << (replayAnalysisProof.currentProcessReplay ? "true" : "false") << '\n';
      std::cout << "replay_analysis.active_match_metadata="
                << (replayAnalysisProof.activeMatchMetadata ? "true" : "false") << '\n';
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
      fixtureCandidate.counterOffset = sizeof(std::uint64_t);
      fixtureCandidate.score = 1000;
      fixtureCandidate.regionClass = "self-fixture";
      fixtureCandidate.regionPath = "self-command-queue-fixture";
      fixtureCandidate.bufferRegionClass = "self-fixture";
      fixtureCandidate.bufferRegionPath = "self-command-queue-fixture-buffer";
      populateCommandQueuePrefixDiagnostics(
        fixtureCandidate,
        std::vector<unsigned char>(
          commandQueueFixture.buffer.begin(),
          commandQueueFixture.buffer.begin()
            + static_cast<std::ptrdiff_t>(
              std::min<std::size_t>(commandQueueFixture.buffer.size(), 64))));
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
      rawFixtureCandidate.counterOffset =
        rawFixtureCandidate.bytesInQueueAddress - rawFixtureCandidate.bufferBegin;
      rawFixtureCandidate.score = 1100;
      rawFixtureCandidate.regionClass = "self-fixture";
      rawFixtureCandidate.regionPath = "self-raw-turn-buffer-fixture";
      rawFixtureCandidate.bufferRegionClass = "self-fixture";
      rawFixtureCandidate.bufferRegionPath = "self-raw-turn-buffer-fixture-buffer";
      populateCommandQueuePrefixDiagnostics(
        rawFixtureCandidate,
        std::vector<unsigned char>(
          commandQueueFixture.rawBuffer.begin(),
          commandQueueFixture.rawBuffer.begin()
            + static_cast<std::ptrdiff_t>(
              std::min<std::size_t>(commandQueueFixture.rawBuffer.size(), 64))));
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
      bool explicitCandidateReadable = false;
      if (regions.success)
      {
        explicitCandidateReadable = commandQueueBufferAddress != 0
          ? readExplicitRawTurnBufferCandidate(
            environment.processId,
            regions.regions,
            commandQueueVectorAddress,
            commandQueueBufferAddress,
            explicitCandidate,
            explicitReason)
          : readCommandQueueCandidate(
            environment.processId,
            regions.regions,
            commandQueueVectorAddress,
            explicitCandidate,
            explicitReason);
      }
      if (regions.success && explicitCandidateReadable)
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
        const RuntimeMemoryRegion* bufferRegion = findWritableRegion(
          regions.regions,
          explicitCandidate.bufferBegin,
          std::max<std::size_t>(1, std::min<std::size_t>(explicitCandidate.capacityBytes, 64)));
        explicitCandidate.score = 10000;
        explicitCandidate.regionClass = selectorRegion != nullptr
          ? commandQueueRegionClass(*selectorRegion, environment.executablePath, starCraftImageSectionHints(0))
          : "unknown";
        explicitCandidate.regionPath =
          selectorRegion != nullptr && !selectorRegion->mappedPath.empty()
            ? selectorRegion->mappedPath
            : (explicitCandidate.storageKind == "raw-turn-buffer"
              ? "explicit-raw-turn-buffer"
              : "explicit-command-queue-vector");
        explicitCandidate.bufferRegionClass = bufferRegion != nullptr
          ? commandQueueRegionClass(*bufferRegion, environment.executablePath, starCraftImageSectionHints(0))
          : "unknown";
        explicitCandidate.bufferRegionPath =
          bufferRegion != nullptr && !bufferRegion->mappedPath.empty()
            ? bufferRegion->mappedPath
            : "";
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
          ? "explicit command queue vector/raw turn-buffer is not readable"
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
    std::cout << "command_queue_discovery.retained_transition_candidate_count="
              << commandQueueDiscoveryProof.retainedTransitionCandidates << '\n';
    std::cout << "command_queue_discovery.retained_raw_byte_change_only_candidate_count="
              << commandQueueDiscoveryProof.retainedRawByteChangeOnlyCandidates << '\n';
    std::cout << "command_queue_discovery.retained_bounded_transition_candidate_count="
              << commandQueueDiscoveryProof.retainedBoundedTransitionCandidates << '\n';
    std::cout << "command_queue_discovery.implicit_write_eligible_candidate_count="
              << commandQueueDiscoveryProof.implicitWriteEligibleCandidates << '\n';
    std::cout << "command_queue_discovery.live_code_reference_count="
              << commandQueueDiscoveryProof.liveCodeReferenceCount << '\n';
    std::cout << "command_queue_discovery.live_code_reference_candidate_count="
              << commandQueueDiscoveryProof.liveCodeReferenceCandidateCount << '\n';
    std::cout << "command_queue_discovery.live_code_reference_rejected_count="
              << commandQueueDiscoveryProof.liveCodeReferenceRejectedCount << '\n';
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
    std::cout << "issue_commands.frame_counter_candidate_count="
              << issueCommandsProof.frameCounterCandidateCount << '\n';
    std::cout << "issue_commands.attempt_count="
              << issueCommandsProof.attempts.size() << '\n';
    std::cout << "issue_commands.live_callable_anchor_count="
              << issueCommandsProof.liveDiagnostics.anchors.size() << '\n';
    if (issueCommandsProof.liveDiagnostics.imageBase != 0)
    {
      std::cout << "issue_commands.live_image_base="
                << hexAddress(issueCommandsProof.liveDiagnostics.imageBase) << '\n';
      std::cout << "issue_commands.live_image_slide="
                << hexAddress(issueCommandsProof.liveDiagnostics.imageSlide) << '\n';
    }
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
    && hasResidentGameStateProofTicks(readGameStateProof)
    && readUnitsProof.passed;

  if (proveDrawOverlays)
  {
    drawOverlaysProof = proveDrawOverlaysFailClosed(
      issueCommandsProof,
      environment.processId,
      environment.executablePath);
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
    std::cout << "draw_overlays.live_callable_anchor_count="
              << drawOverlaysProof.liveDiagnostics.anchors.size() << '\n';
    if (drawOverlaysProof.liveDiagnostics.imageBase != 0)
    {
      std::cout << "draw_overlays.live_image_base="
                << hexAddress(drawOverlaysProof.liveDiagnostics.imageBase) << '\n';
      std::cout << "draw_overlays.live_image_slide="
                << hexAddress(drawOverlaysProof.liveDiagnostics.imageSlide) << '\n';
    }
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
      environment.processId,
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
    std::cout << "multiplayer_sync.live_callable_anchor_count="
              << multiplayerSyncProof.liveDiagnostics.anchors.size() << '\n';
    if (multiplayerSyncProof.liveDiagnostics.imageBase != 0)
    {
      std::cout << "multiplayer_sync.live_image_base="
                << hexAddress(multiplayerSyncProof.liveDiagnostics.imageBase) << '\n';
      std::cout << "multiplayer_sync.live_image_slide="
                << hexAddress(multiplayerSyncProof.liveDiagnostics.imageSlide) << '\n';
    }
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
      environment.executablePath,
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

  constexpr bool externalAdapterProofCanPromoteResidentState = false;
  const bool willWriteReadGameStateProof =
    externalAdapterProofCanPromoteResidentState
    && proveReadGameState
    && readGameStateProof.passed
    && hasResidentGameStateProofTicks(readGameStateProof);
  const bool willWriteReadUnitsProof =
    proveReadUnits
    && readUnitsProof.passed
    && (!readUnitsProof.derivedSnapshot || unitSnapshotWritten)
    && (!self || !proveActiveMatchState);
  const bool willWriteActiveMatchProof =
    externalAdapterProofCanPromoteResidentState
    && proveActiveMatchState
    && !self
    && willWriteReadGameStateProof
    && willWriteReadUnitsProof;
  const bool willWriteIssueCommandsProof =
    proveIssueCommands
    && issueCommandsProof.passed
    && issueCommandsSnapshotWritten;
  const bool willWriteDrawOverlaysProof =
    proveDrawOverlays
    && drawOverlaysProof.passed
    && drawOverlaysSnapshotWritten;
  const bool willWriteMultiplayerSyncProof =
    proveMultiplayerSync
    && multiplayerSyncProof.passed
    && multiplayerSyncSnapshotWritten;
  const bool willWriteDispatchEventsProof =
    proveDispatchEvents
    && dispatchEventsProof.passed
    && dispatchEventsSnapshotWritten
    && willWriteActiveMatchProof;
  const bool willWriteReplayAnalysisProof =
    proveReplayAnalysis
    && replayAnalysisProof.passed
    && replayAnalysisSnapshotWritten;
  const bool willWriteBattleNetPolicyProof =
    proveBattleNetPolicyFlag
    && battleNetPolicyProof.passed;
  const bool willWriteLoadAIModulesProof =
    proveLoadAIModules
    && aiModuleLoadProof.passed
    && aiModuleLoadSnapshotWritten;
  const bool willWriteReadMapDataProof =
    proveReadMapData
    && mapDataProof.passed
    && mapDataSnapshotWritten;
  const bool willWriteReadPlayerDataProof =
    proveReadPlayerData
    && playerDataProof.passed
    && playerDataSnapshotWritten
    && (!self || !proveActiveMatchState || willWriteActiveMatchProof);
  const bool willWriteReadBulletDataProof =
    proveReadBulletData
    && bulletDataProof.passed
    && bulletDataSnapshotWritten;
  const bool willWriteReadRegionDataProof =
    proveReadRegionData
    && regionDataProof.passed
    && regionDataSnapshotWritten;

  std::vector<std::string> invalidatedProofTokens;
  auto invalidateProofToken = [&](bool replacementReady, const char* token)
  {
    if (replacementReady)
      invalidatedProofTokens.push_back(token);
  };
  invalidateProofToken(proveReadGameState, "proof.read_game_state");
  invalidateProofToken(proveActiveMatchState, "proof.active_match_state");
  invalidateProofToken(willWriteReadUnitsProof, "proof.read_units");
  invalidateProofToken(willWriteIssueCommandsProof, "proof.issue_commands");
  invalidateProofToken(willWriteDrawOverlaysProof, "proof.draw_overlays");
  invalidateProofToken(willWriteDispatchEventsProof, "proof.dispatch_events");
  invalidateProofToken(willWriteReplayAnalysisProof, "proof.replay_analysis");
  invalidateProofToken(willWriteMultiplayerSyncProof, "proof.multiplayer_sync");
  invalidateProofToken(willWriteBattleNetPolicyProof, "proof.battle_net_policy");
  invalidateProofToken(willWriteLoadAIModulesProof, "proof.load_ai_modules");
  invalidateProofToken(willWriteReadMapDataProof, "proof.read_map_data");
  invalidateProofToken(willWriteReadPlayerDataProof, "proof.read_player_data");
  invalidateProofToken(willWriteReadBulletDataProof, "proof.read_bullet_data");
  invalidateProofToken(willWriteReadRegionDataProof, "proof.read_region_data");
  invalidateProofToken(commandQueueDiscoveryProof.ready, "proof.command_queue_discovery");

  std::vector<std::string> existingReadyLines;
  std::vector<std::string> preservedReadyEvidenceLines;
  std::vector<std::string> preservedResidentEvidenceLines;
  bool existingReadyMatchesRuntime = false;
  bool existingReadyFromResidentAdapter = false;
  if (runtimeVisibleAtReady)
  {
    existingReadyLines = readReadyFileLines(readyPath);
    existingReadyMatchesRuntime = existingReadyIdentityMatches(existingReadyLines, environment);
    if (existingReadyMatchesRuntime)
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
      existingReadyFromResidentAdapter = readyWasWrittenByResidentAdapter(existingReadyLines);
      if (existingReadyFromResidentAdapter)
      {
        std::unordered_set<std::string> preservedResident;
        for (const std::string& line : existingReadyLines)
        {
          if (!preservableResidentEvidenceLine(line))
            continue;
          if (preservedResident.insert(line).second)
            preservedResidentEvidenceLines.push_back(line);
        }
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
  for (const std::string& line : preservedResidentEvidenceLines)
    ready << line << '\n';
  const bool residentReadGameStateReady = willWriteReadGameStateProof;
  if (proveReadGameState)
  {
    ready << "diagnostic.read_game_state.source=external-adapter-proof\n";
    ready << "diagnostic.read_game_state.production_proof=false\n";
    ready << "diagnostic.read_game_state.required_source=resident\n";
    ready << "diagnostic.read_game_state.required_adapter_abi="
          << residentAdapterAbi() << '\n';
    if (readGameStateProof.passed && hasResidentGameStateProofTicks(readGameStateProof))
    {
      ready << "diagnostic.read_game_state.address=0x"
            << std::hex << readGameStateProof.address << std::dec << '\n';
      ready << "diagnostic.read_game_state.samples="
            << readGameStateProof.first << ','
            << readGameStateProof.second << ','
            << readGameStateProof.third << '\n';
      ready << "diagnostic.read_game_state.delta="
            << (readGameStateProof.second - readGameStateProof.first) << ','
            << (readGameStateProof.third - readGameStateProof.second) << '\n';
      ready << "diagnostic.read_game_state.confidence=frame-like\n";
    }
    if (!readGameStateProof.reason.empty())
      ready << "diagnostic.read_game_state.reason="
            << readGameStateProof.reason << '\n';
  }
  const bool readUnitsReady =
    proveReadUnits
    && readUnitsProof.passed
    && (!readUnitsProof.derivedSnapshot || unitSnapshotWritten)
    && (!self || !proveActiveMatchState);
  const bool activeMatchReady = willWriteActiveMatchProof;
  if (proveActiveMatchState)
  {
    ready << "diagnostic.active_match_state.source=external-adapter-proof\n";
    ready << "diagnostic.active_match_state.production_proof=false\n";
    ready << "diagnostic.active_match_state.required_source=resident\n";
    ready << "diagnostic.active_match_state.required_mode=match\n";
    ready << "diagnostic.active_match_state.replay_launch_detected="
          << (replayLaunchDetected ? "true" : "false") << '\n';
    if (readUnitsProof.passed)
    {
      ready << "diagnostic.active_match_state.active_records="
            << readUnitsProof.activeRecords << '\n';
      ready << "diagnostic.active_match_state.evidence="
            << (readUnitsProof.derivedSnapshot
              ? "active-unit-node-snapshot"
              : "active-unit-records")
            << '\n';
    }
    if (!readUnitsProof.reason.empty())
      ready << "diagnostic.active_match_state.reason="
            << readUnitsProof.reason << '\n';
  }
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
    ready << "proof.issue_commands.source=live-sc-r-command-path\n";
    ready << "proof.issue_commands.delivery_checked="
          << (issueCommandsProof.deliveryChecked ? "true" : "false") << '\n';
    ready << "proof.issue_commands.behavior_checked="
          << (issueCommandsProof.behaviorChecked ? "true" : "false") << '\n';
    ready << "proof.issue_commands.self_fixture="
          << (issueCommandsProof.selfFixture ? "true" : "false") << '\n';
    ready << "proof.issue_commands.pause_frame_counter_matched="
          << (issueCommandsProof.pauseFrameCounterMatched ? "true" : "false") << '\n';
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
    ready << "diagnostic.issue_commands.required_adapter_abi="
          << residentAdapterAbi() << '\n';
    ready << "diagnostic.issue_commands.required_adapter_behavior="
          << "encoded-bwapi-command-reaches-live-scr-command-path-and-changes-frame-behavior\n";
    ready << "diagnostic.issue_commands.live_callable_entry_candidate_count="
          << collectLiveCallableEntryCandidates(issueCommandsProof.liveDiagnostics, 16).size()
          << '\n';
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
    ready << "diagnostic.draw_overlays.required_adapter_abi="
          << residentAdapterAbi() << '\n';
    ready << "diagnostic.draw_overlays.required_adapter_behavior="
          << "bwapi-overlay-primitives-render-on-visible-game-frame\n";
    ready << "diagnostic.draw_overlays.live_callable_entry_candidate_count="
          << collectLiveCallableEntryCandidates(drawOverlaysProof.liveDiagnostics, 16).size()
          << '\n';
    if (drawOverlaysProof.drawLayerAnchorsResolved || drawOverlaysProof.renderApiAnchorsResolved)
    {
      ready << "contract.binding.draw-game-layer-hook=hook-point|static-anchor:"
            << joinStrings(drawOverlaysProof.resolvedAnchors, "+") << '\n';
    }
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
    ready << "diagnostic.multiplayer_sync.required_adapter_abi="
          << residentAdapterAbi() << '\n';
    ready << "diagnostic.multiplayer_sync.required_adapter_behavior="
          << "turn-packet-send-receive-sync-observed-in-live-battle-net-path\n";
    ready << "diagnostic.multiplayer_sync.live_callable_entry_candidate_count="
          << collectLiveCallableEntryCandidates(multiplayerSyncProof.liveDiagnostics, 16).size()
          << '\n';
    if (multiplayerSyncProof.platformReceiveResolved)
    {
      ready << "contract.binding.Storm::SNetReceiveMessage=imported-function|scr-platform-anchor:"
            << multiplayerSyncProof.platformReceiveBinding << '\n';
    }
    if (multiplayerSyncProof.platformSendResolved && multiplayerSyncProof.turnPacketAnchorResolved)
    {
      ready << "contract.binding.Storm::SNetSendTurn=imported-function|scr-platform-anchor:"
            << multiplayerSyncProof.platformSendBinding << '+' << multiplayerSyncProof.turnPacketBinding << '\n';
    }
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
  if (proveReadPlayerData
      && playerDataProof.passed
      && playerDataSnapshotWritten
      && (!self || !proveActiveMatchState || activeMatchReady))
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
  else if (proveReadBulletData && bulletDataProof.staticAdapterAnchorsResolved)
  {
    ready << "diagnostic.read_bullet_data.static_adapter_anchors="
          << joinStrings(bulletDataProof.resolvedAnchors, ",") << '\n';
    ready << "diagnostic.read_bullet_data.reason=" << bulletDataProof.reason << '\n';
    ready << "contract.binding.BW::BWDATA::BulletNodeTable=data-address|static-anchor:"
          << joinStrings(bulletDataProof.resolvedAnchors, "+") << '\n';
    ready << "contract.structure.BW::CBullet=" << bulletDataProof.recordSize
          << "|static-layout:" << bulletDataProof.layoutName << '\n';
    ready << "contract.field.BW::CBullet.position=" << bulletDataProof.positionOffset
          << "|4|static-layout:" << bulletDataProof.layoutName << '\n';
    ready << "contract.field.BW::CBullet.velocity=" << bulletDataProof.velocityOffset
          << "|8|static-layout:" << bulletDataProof.layoutName << '\n';
    ready << "contract.field.BW::CBullet.sourceUnit=" << bulletDataProof.sourceUnitOffset
          << "|8|static-layout:" << bulletDataProof.layoutName << '\n';
    ready << "contract.field.BW::CBullet.target=" << bulletDataProof.targetOffset
          << "|8|static-layout:" << bulletDataProof.layoutName << '\n';
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
    ready << "proof.replay_analysis.source=" << replayAnalysisProof.source << '\n';
    ready << "proof.replay_analysis.current_process_replay="
          << (replayAnalysisProof.currentProcessReplay ? "true" : "false") << '\n';
    ready << "proof.replay_analysis.active_match_metadata="
          << (replayAnalysisProof.activeMatchMetadata ? "true" : "false") << '\n';
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
    ready << "proof.command_queue_discovery.retained_transition_candidate_count="
          << commandQueueDiscoveryProof.retainedTransitionCandidates << '\n';
    ready << "proof.command_queue_discovery.retained_raw_byte_change_only_candidate_count="
          << commandQueueDiscoveryProof.retainedRawByteChangeOnlyCandidates << '\n';
    ready << "proof.command_queue_discovery.retained_bounded_transition_candidate_count="
          << commandQueueDiscoveryProof.retainedBoundedTransitionCandidates << '\n';
    ready << "proof.command_queue_discovery.implicit_write_eligible_candidate_count="
          << commandQueueDiscoveryProof.implicitWriteEligibleCandidates << '\n';
    ready << "proof.command_queue_discovery.live_code_reference_count="
          << commandQueueDiscoveryProof.liveCodeReferenceCount << '\n';
    ready << "proof.command_queue_discovery.live_code_reference_candidate_count="
          << commandQueueDiscoveryProof.liveCodeReferenceCandidateCount << '\n';
    ready << "proof.command_queue_discovery.live_code_reference_rejected_count="
          << commandQueueDiscoveryProof.liveCodeReferenceRejectedCount << '\n';
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
  if (residentReadGameStateReady)
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
  if (proveReadPlayerData
      && playerDataProof.passed
      && playerDataSnapshotWritten
      && (!self || !proveActiveMatchState || activeMatchReady))
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
