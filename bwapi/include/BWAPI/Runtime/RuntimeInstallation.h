#pragma once

#include <BWAPI/Runtime/RuntimeBackend.h>

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

  RuntimeInstallation detectStarCraftInstallation(const RuntimeEnvironment& environment);
  std::vector<int> findRuntimeProcessIds(const RuntimeInstallation& installation);
  RuntimeLaunchResult launchOrAttachRuntime(
    const RuntimeInstallation& installation,
    bool launchIfMissing,
    int waitMilliseconds);
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
