#pragma once

#include <BWAPI/Runtime/RuntimeBackend.h>

#include <cstdint>
#include <string>
#include <vector>

namespace BWAPI::Runtime
{
  struct RuntimeInstallation
  {
    bool found = false;
    Product product = Product::Unknown;
    Platform platform = Platform::Unknown;
    std::string installRoot;
    std::string appBundlePath;
    std::string executablePath;
    std::string launcherPath;
    std::string version;
    std::string reason;
    std::vector<std::string> searchedPaths;
  };

  struct RuntimeLaunchResult
  {
    bool launched = false;
    bool running = false;
    int processId = 0;
    std::string reason;
    std::vector<std::string> warnings;
  };

  struct RuntimeFileIdentity
  {
    bool exists = false;
    std::uintmax_t size = 0;
    std::string fnv1a64;
    std::string path;
    std::string reason;
  };

  struct RuntimeObservedProcess
  {
    int processId = 0;
    int parentProcessId = 0;
    std::string category;
    std::string command;
  };

  struct RuntimeLogExcerpt
  {
    std::string path;
    std::vector<std::string> lines;
    std::string reason;
  };

  struct RuntimeSessionEvent
  {
    std::string path;
    std::string category;
    std::string line;
  };

  struct RuntimeSessionTransition
  {
    bool complete = false;
    int durationMilliseconds = -1;
    std::string startTimestamp;
    std::string endTimestamp;
    std::string startPath;
    std::string endPath;
    std::string startLine;
    std::string endLine;
    std::string reason;
  };

  struct RuntimeSessionSummary
  {
    int startedEventCount = 0;
    int endedEventCount = 0;
    int preexistingEventCount = 0;
    int installStateEventCount = 0;
    int relatedEventCount = 0;
    int completeTransitionCount = 0;
    int incompleteTransitionCount = 0;
    int shortestDurationMilliseconds = -1;
    int longestDurationMilliseconds = -1;
    int latestTransitionDurationMilliseconds = -1;
    std::string latestObservedTimestamp;
    std::string latestState;
    std::string latestReason;
    std::vector<RuntimeSessionTransition> transitions;
  };

  struct RuntimeEvidence
  {
    RuntimeInstallation installation;
    RuntimeLaunchResult launchResult;
    RuntimeFileIdentity executable;
    std::vector<RuntimeObservedProcess> processes;
    std::vector<RuntimeLogExcerpt> logs;
    std::vector<RuntimeSessionEvent> sessionEvents;
    RuntimeSessionSummary sessionSummary;
  };

  RuntimeInstallation detectStarCraftInstallation(const RuntimeEnvironment& environment);
  std::vector<int> findRuntimeProcessIds(const RuntimeInstallation& installation);
  RuntimeLaunchResult launchOrAttachRuntime(
    const RuntimeInstallation& installation,
    bool launchIfMissing,
    int waitMilliseconds);
  RuntimeEvidence collectRuntimeEvidence(
    const RuntimeInstallation& installation,
    const RuntimeLaunchResult& launchResult);
  std::string makeRuntimeEvidenceReport(const RuntimeEvidence& evidence);
  bool writeRuntimeEvidenceReport(
    const RuntimeInstallation& installation,
    const RuntimeLaunchResult& launchResult,
    const std::string& path,
    std::string& error);
  RuntimeEnvironment makeRuntimeEnvironmentForInstallation(
    const RuntimeEnvironment& baseEnvironment,
    const RuntimeInstallation& installation,
    int processId);
  std::string makeRuntimeBootstrapManifest(const RuntimeInstallation& installation);
  bool writeRuntimeBootstrapManifest(
    const RuntimeInstallation& installation,
    const std::string& path,
    std::string& error);
  bool writeRuntimeExecutorReadyFile(
    const RuntimeEnvironment& environment,
    const std::string& bridgePath,
    std::string& error);
}
