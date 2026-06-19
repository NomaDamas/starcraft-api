#include <BWAPI/Runtime/RuntimeExecutor.h>
#include <BWAPI/Runtime/RuntimeInstallation.h>

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#if defined(_WIN32)
#include <stdlib.h>
#endif

using namespace BWAPI::Runtime;

namespace
{
  void setEnvValue(const char* name, const std::string& value)
  {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
  }

  void unsetEnvValue(const char* name)
  {
#if defined(_WIN32)
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
  }

  void writeFile(const std::filesystem::path& path, const std::string& content)
  {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path);
    output << content;
  }

  bool containsText(const std::string& text, const std::string& needle)
  {
    return text.find(needle) != std::string::npos;
  }

  bool hasWarning(const RuntimeLaunchResult& result, const std::string& warning)
  {
    for (const std::string& candidate : result.warnings)
    {
      if (candidate == warning)
        return true;
    }
    return false;
  }
}

int main()
{
  const std::filesystem::path tempRoot =
    std::filesystem::temp_directory_path() / "starcraft-api-runtime-installation-test";
  std::filesystem::remove_all(tempRoot);

  const std::filesystem::path installRoot = tempRoot / "StarCraft";
  const std::filesystem::path appBundle = installRoot / "x86_64" / "StarCraft.app";
  const std::filesystem::path executable = appBundle / "Contents" / "MacOS" / "StarCraft";
  const std::filesystem::path plist = appBundle / "Contents" / "Info.plist";
  const std::filesystem::path launcher =
    installRoot / "StarCraft Launcher.app" / "Contents" / "MacOS" / "StarCraft Launcher";
  const std::filesystem::path logRoot = tempRoot / "logs";
  const std::filesystem::path logFile = logRoot / "battle.net-test.log";

  writeFile(executable, "fake executable");
  writeFile(
    plist,
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<plist version=\"1.0\"><dict>\n"
    "<key>CFBundleShortVersionString</key>\n"
    "<string>2.0.13-test</string>\n"
    "</dict></plist>\n");
  writeFile(launcher, "fake launcher");
  writeFile(
    logFile,
    std::string("first line\n")
      + "I 2026-06-19 08:00:00.000000 [InstallManager] {Main} Launched "
      + appBundle.string()
      + " with args: -launch -uid s1, pid: 777\n"
      + "I 2026-06-19 08:00:00.100000 [InstallManager] Setting Process Running: true uid=s1 binaryType=game any=true\n"
      + "I 2026-06-19 08:00:06.350000 [InstallManager] Game is no longer running: s1\n"
      + "launch handoff failed in test\n");

  setEnvValue("STARCRAFT_API_INSTALL_DIR", installRoot.string());
  setEnvValue("STARCRAFT_API_LOG_DIR", logRoot.string());

  RuntimeEnvironment environment = RuntimeEnvironment::detectHost();
  environment.platform = Platform::MacOS;
  RuntimeInstallation installation = detectStarCraftInstallation(environment);

  assert(installation.found);
  assert(installation.product == Product::StarCraftRemastered);
  assert(installation.platform == Platform::MacOS);
  assert(installation.version == "2.0.13-test");
  assert(installation.executablePath.find("StarCraft.app") != std::string::npos);
  assert(installation.launcherPath.find("StarCraft Launcher.app") != std::string::npos);

#if !defined(_WIN32)
  const std::filesystem::path processSnapshot = tempRoot / "processes.snapshot";
  writeFile(
    processSnapshot,
    "4428 1 /Applications/Battle.net.app/Contents/MacOS/Battle.net --game=s1 --gamepath="
      + installRoot.string()
      + "/\n");
  setEnvValue("STARCRAFT_API_PROCESS_SNAPSHOT", processSnapshot.string());

  const RuntimeLaunchResult existingHandoffResult = launchOrAttachRuntime(installation, true, 0);
  assert(!existingHandoffResult.launched);
  assert(!existingHandoffResult.running);
  assert(existingHandoffResult.processId == 0);
  assert(hasWarning(existingHandoffResult, "battle.net.process_count=1"));
  assert(hasWarning(existingHandoffResult, "battle.net.process_id=4428"));
  assert(containsText(existingHandoffResult.reason, "not launching another Battle.net instance"));
  assert(containsText(existingHandoffResult.reason, "wait timeout"));

  writeFile(
    processSnapshot,
    "4430 4428 " + executable.string() + " -launch -uid s1\n");
  const std::vector<int> gameProcessIds = findRuntimeProcessIds(installation);
  assert(gameProcessIds.size() == 1);
  assert(gameProcessIds.front() == 4430);

  RuntimeEnvironment unresolvedEnvironment = RuntimeEnvironment::detectHost();
  unresolvedEnvironment.platform = Platform::MacOS;
  unresolvedEnvironment.product = Product::Unknown;
  unresolvedEnvironment.version.clear();
  unresolvedEnvironment.executablePath.clear();
  unresolvedEnvironment.processId = 0;
  const RuntimeEnvironment resolvedEnvironment = resolveRuntimeEnvironment(unresolvedEnvironment);
  assert(resolvedEnvironment.platform == Platform::MacOS);
  assert(resolvedEnvironment.product == Product::StarCraftRemastered);
  assert(resolvedEnvironment.version == "2.0.13-test");
  assert(resolvedEnvironment.executablePath == installation.executablePath);
  assert(resolvedEnvironment.processId == 4430);

  unsetEnvValue("STARCRAFT_API_PROCESS_SNAPSHOT");
#endif

  const std::string manifest = makeRuntimeBootstrapManifest(installation);
  assert(manifest.find("product starcraft-remastered") != std::string::npos);
  assert(manifest.find("version 2.0.13-test") != std::string::npos);
  assert(manifest.find("api-surface-methods 0") != std::string::npos);

  const std::filesystem::path manifestPath = tempRoot / "bootstrap.manifest";
  std::string error;
  assert(writeRuntimeBootstrapManifest(installation, manifestPath.string(), error));
  assert(error.empty());
  assert(std::filesystem::is_regular_file(manifestPath));

  RuntimeEnvironment runtimeEnvironment =
    makeRuntimeEnvironmentForInstallation(environment, installation, 12345);
  const std::filesystem::path bridgePath = tempRoot / "bridge";
  assert(writeRuntimeExecutorReadyFile(runtimeEnvironment, bridgePath.string(), error));
  assert(std::filesystem::is_regular_file(bridgePath / RuntimeExecutorBridgeReadyFile));

  RuntimeLaunchResult launchResult;
  launchResult.reason = "unit test launch did not run";
  RuntimeEvidence evidence = collectRuntimeEvidence(installation, launchResult);
  assert(evidence.executable.exists);
  assert(evidence.executable.size > 0);
  assert(!evidence.executable.fnv1a64.empty());
  assert(!evidence.logs.empty());
  assert(evidence.logs.front().path.find("battle.net-test.log") != std::string::npos);
  assert(evidence.sessionEvents.size() == 3);
  assert(evidence.sessionEvents[1].category == "starcraft-session-started");
  assert(evidence.sessionSummary.launchProcessEventCount == 1);
  assert(evidence.sessionSummary.latestLaunchProcessId == 777);
  assert(evidence.sessionSummary.startedEventCount == 1);
  assert(evidence.sessionSummary.endedEventCount == 1);
  assert(evidence.sessionSummary.completeTransitionCount == 1);
  assert(evidence.sessionSummary.incompleteTransitionCount == 0);
  assert(evidence.sessionSummary.latestState == "stopped");
  assert(evidence.sessionSummary.latestObservedTimestamp == "2026-06-19 08:00:06.350000");
  assert(evidence.sessionSummary.latestTransitionDurationMilliseconds == 6250);
  assert(evidence.sessionSummary.transitions.size() == 1);
  assert(evidence.sessionSummary.transitions.front().complete);
  assert(evidence.sessionSummary.transitions.front().durationMilliseconds == 6250);

  const std::string report = makeRuntimeEvidenceReport(evidence);
  assert(report.find("evidence.schema=starcraft-api.runtime-evidence.v1") != std::string::npos);
  assert(report.find("runtime.reason=unit test launch did not run") != std::string::npos);
  assert(report.find("executable.fnv1a64=") != std::string::npos);
  assert(report.find("launch handoff failed in test") != std::string::npos);
  assert(report.find("session.launch_process_event_count=1") != std::string::npos);
  assert(report.find("session.latest_launch_process_id=777") != std::string::npos);
  assert(report.find("session.complete_transition_count=1") != std::string::npos);
  assert(report.find("session.latest_observed_timestamp=2026-06-19 08:00:06.350000") != std::string::npos);
  assert(report.find("session.latest_transition_duration_ms=6250") != std::string::npos);
  assert(report.find("session.transition.0.duration_ms=6250") != std::string::npos);
  assert(report.find("session.event.0.category=starcraft-launch-process") != std::string::npos);
  assert(report.find("session.event.1.category=starcraft-session-started") != std::string::npos);
  assert(report.find("session.event.2.category=starcraft-session-ended") != std::string::npos);

  const std::filesystem::path evidencePath = tempRoot / "runtime.evidence";
  assert(writeRuntimeEvidenceReport(installation, launchResult, evidencePath.string(), error));
  assert(std::filesystem::is_regular_file(evidencePath));

  std::filesystem::remove_all(tempRoot);
  return 0;
}
