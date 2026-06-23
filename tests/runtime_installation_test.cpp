#include <BWAPI/Runtime/RuntimeExecutor.h>
#include <BWAPI/Runtime/RuntimeInstallation.h>

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

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

  void clearWindowedLaunchEnv()
  {
    unsetEnvValue("STARCRAFT_API_WINDOWED");
    unsetEnvValue("STARCRAFT_API_WINDOW_WIDTH");
    unsetEnvValue("STARCRAFT_API_WINDOW_HEIGHT");
    unsetEnvValue("STARCRAFT_API_WINDOW_X");
    unsetEnvValue("STARCRAFT_API_WINDOW_Y");
    unsetEnvValue("STARCRAFT_API_EXTRA_ARGS");
  }

  void clearBridgeDiscoveryEnv()
  {
    unsetEnvValue("STARCRAFT_API_EXECUTOR_BRIDGE_DISCOVERY_DIR");
    unsetEnvValue("STARCRAFT_API_EXECUTOR_BRIDGE_DIR");
  }

  void writeFile(const std::filesystem::path& path, const std::string& content)
  {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path);
    output << content;
  }

#if !defined(_WIN32)
  void makeExecutable(const std::filesystem::path& path)
  {
    std::filesystem::permissions(
      path,
      std::filesystem::perms::owner_exec
        | std::filesystem::perms::group_exec
        | std::filesystem::perms::others_exec,
      std::filesystem::perm_options::add);
  }
#endif

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
  clearWindowedLaunchEnv();
  clearBridgeDiscoveryEnv();

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
      + "D 2026-06-19 08:00:02.000000 [Telemetry] Unrelated BLZIGNORED00000000 token\n"
      + "I 2026-06-19 08:00:06.350000 [InstallManager] Game is no longer running: s1\n"
      + "D 2026-06-19 08:00:06.450000 [UrlManager] {Main} Resolved URL: "
      + "key=/client/error/BLZBNTBNA00000005; region=KR; "
      + "endpoint=https://kr.battle.net/support/ko/article/BLZBNTBNA00000005?utm_medium=internal\n"
      + "launch handoff failed in test\n");

  clearWindowedLaunchEnv();
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
    "987654321 1 /Applications/Battle.net.app/Contents/MacOS/Battle.net --game=s1 --gamepath="
      + installRoot.string()
      + "/\n");
  const RuntimeLaunchResult replaceMissingHandoff =
    launchOrAttachRuntime(installation, true, 0, 0, true);
  assert(!replaceMissingHandoff.launched);
  assert(!replaceMissingHandoff.running);
  assert(containsText(replaceMissingHandoff.reason, "unable to terminate stale Battle.net handoff process"));

  writeFile(
    processSnapshot,
    "4428 1 /Applications/Battle.net.app/Contents/MacOS/Battle.net --game=s1 --gamepath="
      + installRoot.string()
      + "/\n"
      + "9000 1 build/starcraft-runtime-gap-report --executable "
      + executable.string()
      + " --require-production\n");
  assert(findRuntimeProcessIds(installation).empty());

  writeFile(
    processSnapshot,
    "4430 4428 " + executable.string() + " -launch -uid s1\n");
  const std::vector<int> gameProcessIds = findRuntimeProcessIds(installation);
  assert(gameProcessIds.size() == 1);
  assert(gameProcessIds.front() == 4430);

  writeFile(
    processSnapshot,
    "987654321 4428 " + executable.string() + " -launch -uid s1\n");
  const RuntimeLaunchResult replaceMissingGameProcess =
    launchOrAttachRuntime(installation, true, 0, 0, false, true);
  assert(!replaceMissingGameProcess.launched);
  assert(!replaceMissingGameProcess.running);
  assert(hasWarning(replaceMissingGameProcess, "runtime.existing_process_count=1"));
  assert(containsText(replaceMissingGameProcess.reason, "unable to terminate existing StarCraft process"));

  writeFile(
    processSnapshot,
    "4430 4428 " + executable.string() + " -launch -uid s1\n");

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
  assert(resolvedEnvironment.executorBridgePath.empty());

  const std::filesystem::path autoBridgeRoot = tempRoot / "auto-bridge";
  writeFile(
    autoBridgeRoot / "ready",
    "protocol=starcraft-api-file-bridge-v1\n"
    "product=starcraft-remastered\n"
    "version=2.0.13-test\n"
    "mode=validated-runtime-adapter\n"
    "process_id=4430\n"
    "executable=" + installation.executablePath + "\n"
    "proof.attach=passed\n");
  setEnvValue("STARCRAFT_API_EXECUTOR_BRIDGE_DISCOVERY_DIR", autoBridgeRoot.string());
  RuntimeEnvironment autoBridgeEnvironment = unresolvedEnvironment;
  const RuntimeEnvironment resolvedAutoBridgeEnvironment =
    resolveRuntimeEnvironment(autoBridgeEnvironment);
  assert(resolvedAutoBridgeEnvironment.executorBridgePath == autoBridgeRoot.string());

  writeFile(
    autoBridgeRoot / "ready",
    "protocol=starcraft-api-file-bridge-v1\n"
    "product=starcraft-remastered\n"
    "version=2.0.13-test\n"
    "mode=validated-runtime-adapter\n"
    "process_id=4431\n"
    "executable=" + installation.executablePath + "\n"
    "proof.attach=passed\n");
  RuntimeEnvironment staleAutoBridgeEnvironment = unresolvedEnvironment;
  const RuntimeEnvironment resolvedStaleAutoBridgeEnvironment =
    resolveRuntimeEnvironment(staleAutoBridgeEnvironment);
  assert(resolvedStaleAutoBridgeEnvironment.executorBridgePath.empty());
  clearBridgeDiscoveryEnv();

  writeFile(
    processSnapshot,
    "4428 1 /Applications/Battle.net.app/Contents/MacOS/Battle.net --game=s1 --gamepath="
      + installRoot.string()
      + "/\n");
  writeFile(
    autoBridgeRoot / "ready",
    "protocol=starcraft-api-file-bridge-v1\n"
    "product=starcraft-remastered\n"
    "version=2.0.13-test\n"
    "mode=validated-runtime-adapter\n"
    "process_id=4430\n"
    "executable=" + installation.executablePath + "\n"
    "proof.attach=passed\n");
  setEnvValue("STARCRAFT_API_EXECUTOR_BRIDGE_DISCOVERY_DIR", autoBridgeRoot.string());
  RuntimeEnvironment noPidBridgeEnvironment = unresolvedEnvironment;
  const RuntimeEnvironment resolvedNoPidBridgeEnvironment =
    resolveRuntimeEnvironment(noPidBridgeEnvironment);
  assert(resolvedNoPidBridgeEnvironment.processId == 0);
  assert(resolvedNoPidBridgeEnvironment.executorBridgePath.empty());
  clearBridgeDiscoveryEnv();

  writeFile(
    processSnapshot,
    "4430 4428 " + executable.string() + " -launch -uid s1\n");
  std::thread transientProcess(
    [&]()
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
      writeFile(
        processSnapshot,
        "4428 1 /Applications/Battle.net.app/Contents/MacOS/Battle.net --game=s1 --gamepath="
          + installRoot.string()
          + "/\n");
    });
  const RuntimeLaunchResult transientResult = launchOrAttachRuntime(installation, false, 0, 100);
  transientProcess.join();
  assert(!transientResult.running);
  assert(transientResult.processId == 0);
  assert(transientResult.requiredStableMilliseconds == 100);
  assert(transientResult.observedStableMilliseconds == 0);

  const std::filesystem::path windowedRoot = tempRoot / "windowed";
  const std::filesystem::path windowedLauncher = windowedRoot / "launcher";
  const std::filesystem::path windowedExecutable = windowedRoot / "StarCraft";
  const std::filesystem::path windowedSnapshot = windowedRoot / "processes.snapshot";
  writeFile(
    windowedLauncher,
    "#!/bin/sh\n"
    "exit 70\n");
  writeFile(
    windowedExecutable,
    "#!/bin/sh\n"
    "if [ \"$#\" -ne 13 ]; then exit 63; fi\n"
    "if [ \"$1\" != \"-launch\" ] || [ \"$2\" != \"-uid\" ] || [ \"$3\" != \"s1\" ]; then exit 64; fi\n"
    "if [ \"$4\" != \"-displayMode\" ] || [ \"$5\" != \"0\" ]; then exit 65; fi\n"
    "if [ \"$6\" != \"-windowwidth\" ] || [ \"$7\" != \"1280\" ]; then exit 66; fi\n"
    "if [ \"$8\" != \"-windowheight\" ] || [ \"$9\" != \"720\" ]; then exit 67; fi\n"
    "if [ \"${10}\" != \"-windowx\" ] || [ \"${11}\" != \"40\" ]; then exit 68; fi\n"
    "if [ \"${12}\" != \"-windowy\" ] || [ \"${13}\" != \"50\" ]; then exit 69; fi\n"
    "printf '%s 1 %s -launch -uid s1 -displayMode 0 -windowwidth 1280 -windowheight 720 -windowx 40 -windowy 50\\n' \"$$\" \"$STARCRAFT_API_TEST_EXECUTABLE\" > \"$STARCRAFT_API_PROCESS_SNAPSHOT\"\n"
    "sleep 2\n");
  makeExecutable(windowedLauncher);
  makeExecutable(windowedExecutable);
  writeFile(windowedSnapshot, "");
  setEnvValue("STARCRAFT_API_PROCESS_SNAPSHOT", windowedSnapshot.string());
  setEnvValue("STARCRAFT_API_TEST_EXECUTABLE", windowedExecutable.string());
  setEnvValue("STARCRAFT_API_WINDOWED", "1");
  setEnvValue("STARCRAFT_API_WINDOW_WIDTH", "1280");
  setEnvValue("STARCRAFT_API_WINDOW_HEIGHT", "720");
  setEnvValue("STARCRAFT_API_WINDOW_X", "40");
  setEnvValue("STARCRAFT_API_WINDOW_Y", "50");

  RuntimeInstallation windowedInstallation;
  windowedInstallation.found = true;
  windowedInstallation.product = Product::StarCraftRemastered;
  windowedInstallation.platform = Platform::Linux;
  windowedInstallation.installRoot = windowedRoot.string();
  windowedInstallation.executablePath = windowedExecutable.string();
  windowedInstallation.launcherPath = windowedLauncher.string();

  const RuntimeLaunchResult windowedResult = launchOrAttachRuntime(windowedInstallation, true, 0, 0);
  assert(windowedResult.launched);
  assert(windowedResult.running);
  assert(windowedResult.processId > 0);
  assert(hasWarning(windowedResult, "runtime.launch_target=executable"));
  assert(!hasWarning(windowedResult, "runtime.launch_target=launcher"));
  assert(!hasWarning(windowedResult, "runtime.launch_target_no_game=launcher"));
  clearWindowedLaunchEnv();

  const std::filesystem::path extraRoot = tempRoot / "windowed-extra-args";
  const std::filesystem::path extraLauncher = extraRoot / "launcher";
  const std::filesystem::path extraExecutable = extraRoot / "StarCraft";
  const std::filesystem::path extraSnapshot = extraRoot / "processes.snapshot";
  writeFile(
    extraLauncher,
    "#!/bin/sh\n"
    "exit 70\n");
  writeFile(
    extraExecutable,
    "#!/bin/sh\n"
    "if [ \"$#\" -ne 17 ]; then exit 63; fi\n"
    "if [ \"$1\" != \"-launch\" ] || [ \"$2\" != \"-uid\" ] || [ \"$3\" != \"s1\" ]; then exit 64; fi\n"
    "if [ \"$14\" != \"playReplay\" ]; then exit 65; fi\n"
    "if [ \"$15\" != \"Maps/Replays/test replay.rep\" ]; then exit 66; fi\n"
    "if [ \"$16\" != \"-testFlag\" ] || [ \"$17\" != \"quoted value\" ]; then exit 67; fi\n"
    "printf '%s 1 %s -launch -uid s1 playReplay\\n' \"$$\" \"$STARCRAFT_API_TEST_EXECUTABLE\" > \"$STARCRAFT_API_PROCESS_SNAPSHOT\"\n"
    "sleep 2\n");
  makeExecutable(extraLauncher);
  makeExecutable(extraExecutable);
  writeFile(extraSnapshot, "");
  setEnvValue("STARCRAFT_API_PROCESS_SNAPSHOT", extraSnapshot.string());
  setEnvValue("STARCRAFT_API_TEST_EXECUTABLE", extraExecutable.string());
  setEnvValue("STARCRAFT_API_WINDOWED", "1");
  setEnvValue("STARCRAFT_API_EXTRA_ARGS", "playReplay \"Maps/Replays/test replay.rep\" -testFlag 'quoted value'");

  RuntimeInstallation extraInstallation;
  extraInstallation.found = true;
  extraInstallation.product = Product::StarCraftRemastered;
  extraInstallation.platform = Platform::Linux;
  extraInstallation.installRoot = extraRoot.string();
  extraInstallation.executablePath = extraExecutable.string();
  extraInstallation.launcherPath = extraLauncher.string();

  const RuntimeLaunchResult extraResult = launchOrAttachRuntime(extraInstallation, true, 0, 0);
  assert(extraResult.launched);
  assert(extraResult.running);
  assert(extraResult.processId > 0);
  assert(hasWarning(extraResult, "runtime.launch_target=executable"));
  clearWindowedLaunchEnv();

  const std::filesystem::path replayRoot = tempRoot / "windowed-play-replay";
  const std::filesystem::path replayLauncher = replayRoot / "launcher";
  const std::filesystem::path replayExecutable = replayRoot / "StarCraft";
  const std::filesystem::path replaySnapshot = replayRoot / "processes.snapshot";
  writeFile(
    replayLauncher,
    "#!/bin/sh\n"
    "exit 70\n");
  writeFile(
    replayExecutable,
    "#!/bin/sh\n"
    "if [ \"$#\" -ne 15 ]; then exit 63; fi\n"
    "if [ \"$1\" != \"-launch\" ] || [ \"$2\" != \"-uid\" ] || [ \"$3\" != \"s1\" ]; then exit 64; fi\n"
    "if [ \"$4\" != \"-displayMode\" ] || [ \"$5\" != \"0\" ]; then exit 65; fi\n"
    "if [ \"$14\" != \"playReplay\" ]; then exit 66; fi\n"
    "if [ \"$15\" != \"Maps/Replays/test replay.rep\" ]; then exit 67; fi\n"
    "printf '%s 1 %s -launch -uid s1 playReplay %s\\n' \"$$\" \"$STARCRAFT_API_TEST_EXECUTABLE\" \"$15\" > \"$STARCRAFT_API_PROCESS_SNAPSHOT\"\n"
    "sleep 2\n");
  makeExecutable(replayLauncher);
  makeExecutable(replayExecutable);
  writeFile(replayRoot / "Maps" / "Replays" / "test replay.rep", "fake brood war replay");
  writeFile(replaySnapshot, "");
  setEnvValue("STARCRAFT_API_PROCESS_SNAPSHOT", replaySnapshot.string());
  setEnvValue("STARCRAFT_API_TEST_EXECUTABLE", replayExecutable.string());
  setEnvValue("STARCRAFT_API_WINDOWED", "1");

  RuntimeInstallation replayInstallation;
  replayInstallation.found = true;
  replayInstallation.product = Product::StarCraftRemastered;
  replayInstallation.platform = Platform::Linux;
  replayInstallation.installRoot = replayRoot.string();
  replayInstallation.executablePath = replayExecutable.string();
  replayInstallation.launcherPath = replayLauncher.string();

  const std::string replayPath = "Maps/Replays/test replay.rep";
  const RuntimeLaunchResult replayResult =
    launchOrAttachRuntime(replayInstallation, true, 0, 0, false, false, replayPath);
  assert(replayResult.launched);
  assert(replayResult.running);
  assert(replayResult.processId > 0);
  assert(hasWarning(replayResult, "runtime.launch_target=executable"));
  assert(hasWarning(replayResult, "runtime.launch_replay=" + replayPath));

  const RuntimeLaunchResult missingReplayResult =
    launchOrAttachRuntime(replayInstallation, true, 0, 0, false, false, "Maps/Replays/missing.rep");
  assert(!missingReplayResult.requestAccepted);
  assert(!missingReplayResult.launched);
  assert(containsText(missingReplayResult.reason, "replay file does not exist"));
  assert(hasWarning(missingReplayResult, "runtime.launch_replay=Maps/Replays/missing.rep"));

  const RuntimeLaunchResult wrongReplayTypeResult =
    launchOrAttachRuntime(replayInstallation, true, 0, 0, false, false, "Maps/Replays/test.SC2Replay");
  assert(!wrongReplayTypeResult.requestAccepted);
  assert(!wrongReplayTypeResult.launched);
  assert(containsText(wrongReplayTypeResult.reason, "Brood War .rep"));

  writeFile(
    replaySnapshot,
    "4430 4428 " + replayExecutable.string() + " -launch -uid s1\n");
  const RuntimeLaunchResult existingProcessReplayResult =
    launchOrAttachRuntime(replayInstallation, true, 0, 0, false, false, replayPath);
  assert(!existingProcessReplayResult.requestAccepted);
  assert(existingProcessReplayResult.running);
  assert(existingProcessReplayResult.processId == 4430);
  assert(containsText(existingProcessReplayResult.reason, "existing StarCraft process"));
  assert(hasWarning(
    existingProcessReplayResult,
    "runtime.launch_replay_existing_process_requires_replace_running=true"));
  clearWindowedLaunchEnv();

  const std::filesystem::path fallbackRoot = tempRoot / "fallback";
  const std::filesystem::path fallbackLauncher = fallbackRoot / "launcher";
  const std::filesystem::path fallbackExecutable = fallbackRoot / "StarCraft";
  const std::filesystem::path fallbackSnapshot = fallbackRoot / "processes.snapshot";
  writeFile(
    fallbackLauncher,
    "#!/bin/sh\n"
    "exit 0\n");
  writeFile(
    fallbackExecutable,
    "#!/bin/sh\n"
    "if [ \"$1\" != \"-launch\" ] || [ \"$2\" != \"-uid\" ] || [ \"$3\" != \"s1\" ]; then exit 64; fi\n"
    "printf '%s 1 %s -launch -uid s1\\n' \"$$\" \"$STARCRAFT_API_TEST_EXECUTABLE\" > \"$STARCRAFT_API_PROCESS_SNAPSHOT\"\n"
    "sleep 2\n");
  makeExecutable(fallbackLauncher);
  makeExecutable(fallbackExecutable);
  writeFile(fallbackSnapshot, "");
  setEnvValue("STARCRAFT_API_PROCESS_SNAPSHOT", fallbackSnapshot.string());
  setEnvValue("STARCRAFT_API_TEST_EXECUTABLE", fallbackExecutable.string());
  setEnvValue("STARCRAFT_API_WINDOWED", "0");

  RuntimeInstallation fallbackInstallation;
  fallbackInstallation.found = true;
  fallbackInstallation.product = Product::StarCraftRemastered;
  fallbackInstallation.platform = Platform::Linux;
  fallbackInstallation.installRoot = fallbackRoot.string();
  fallbackInstallation.executablePath = fallbackExecutable.string();
  fallbackInstallation.launcherPath = fallbackLauncher.string();

  const RuntimeLaunchResult fallbackResult = launchOrAttachRuntime(fallbackInstallation, true, 0, 0);
  assert(fallbackResult.launched);
  assert(fallbackResult.running);
  assert(fallbackResult.processId > 0);
  assert(fallbackResult.observedStableMilliseconds == 0);
  assert(hasWarning(fallbackResult, "runtime.launch_target=launcher"));
  assert(hasWarning(fallbackResult, "runtime.launch_target_no_game=launcher"));
  assert(hasWarning(fallbackResult, "runtime.launch_target=executable"));

  const std::filesystem::path handoffRoot = tempRoot / "handoff-after-launch";
  const std::filesystem::path handoffLauncher = handoffRoot / "launcher";
  const std::filesystem::path handoffExecutable = handoffRoot / "StarCraft";
  const std::filesystem::path handoffSnapshot = handoffRoot / "processes.snapshot";
  writeFile(
    handoffLauncher,
    "#!/bin/sh\n"
    "printf '9910 1 /Applications/Battle.net.app/Contents/MacOS/Battle.net --game=s1 --gamepath=%s/\\n' "
      "\"$STARCRAFT_API_TEST_INSTALL_ROOT\" > \"$STARCRAFT_API_PROCESS_SNAPSHOT\"\n"
    "sleep 2\n");
  writeFile(
    handoffExecutable,
    "#!/bin/sh\n"
    "if [ \"$1\" != \"-launch\" ] || [ \"$2\" != \"-uid\" ] || [ \"$3\" != \"s1\" ]; then exit 64; fi\n"
    "printf '%s 1 %s -launch -uid s1\\n' \"$$\" \"$STARCRAFT_API_TEST_EXECUTABLE\" > \"$STARCRAFT_API_PROCESS_SNAPSHOT\"\n"
    "sleep 2\n");
  makeExecutable(handoffLauncher);
  makeExecutable(handoffExecutable);
  writeFile(handoffSnapshot, "");
  setEnvValue("STARCRAFT_API_PROCESS_SNAPSHOT", handoffSnapshot.string());
  setEnvValue("STARCRAFT_API_TEST_EXECUTABLE", handoffExecutable.string());
  setEnvValue("STARCRAFT_API_TEST_INSTALL_ROOT", handoffRoot.string());
  setEnvValue("STARCRAFT_API_WINDOWED", "0");

  RuntimeInstallation handoffInstallation;
  handoffInstallation.found = true;
  handoffInstallation.product = Product::StarCraftRemastered;
  handoffInstallation.platform = Platform::Linux;
  handoffInstallation.installRoot = handoffRoot.string();
  handoffInstallation.executablePath = handoffExecutable.string();
  handoffInstallation.launcherPath = handoffLauncher.string();

  const RuntimeLaunchResult handoffAfterLaunchResult = launchOrAttachRuntime(handoffInstallation, true, 0, 0);
  assert(handoffAfterLaunchResult.launched);
  assert(!handoffAfterLaunchResult.running);
  assert(hasWarning(handoffAfterLaunchResult, "runtime.launch_target=launcher"));
  assert(hasWarning(handoffAfterLaunchResult, "runtime.launch_target_no_game=launcher"));
  assert(hasWarning(handoffAfterLaunchResult, "battle.net.process_count_after_launch=1"));
  assert(!hasWarning(handoffAfterLaunchResult, "runtime.launch_target=executable"));
  assert(containsText(handoffAfterLaunchResult.reason, "not launching another Battle.net instance"));

  writeFile(
    processSnapshot,
    "4428 1 /Applications/Battle.net.app/Contents/MacOS/Battle.net --game=s1 --gamepath="
      + installRoot.string()
      + "/\n");
  unsetEnvValue("STARCRAFT_API_TEST_EXECUTABLE");
  unsetEnvValue("STARCRAFT_API_TEST_INSTALL_ROOT");
  clearWindowedLaunchEnv();
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
  assert(evidence.sessionSummary.latestTransitionStartTimestamp == "2026-06-19 08:00:00.100000");
  assert(evidence.sessionSummary.latestTransitionEndTimestamp == "2026-06-19 08:00:06.350000");
  assert(evidence.sessionSummary.transitions.size() == 1);
  assert(evidence.sessionSummary.transitions.front().complete);
  assert(evidence.sessionSummary.transitions.front().durationMilliseconds == 6250);
  assert(evidence.supportErrors.size() == 1);
  assert(evidence.supportErrors.front().code == "BLZBNTBNA00000005");
  assert(containsText(evidence.supportErrors.front().url, "https://kr.battle.net/support/ko/article/BLZBNTBNA00000005"));
  assert(!evidence.diagnosis.readyForAttach);
  assert(!evidence.diagnosis.gameProcessVisible);
  assert(evidence.diagnosis.gameProcessCount == 0);
  assert(evidence.diagnosis.shortLivedSessionObserved);
  assert(evidence.diagnosis.shortLivedSessionAgeMilliseconds == 0);
  assert(evidence.diagnosis.battleNetSupportCode == "BLZBNTBNA00000005");
  assert(containsText(evidence.diagnosis.battleNetSupportUrl, "BLZBNTBNA00000005"));
  assert(containsText(evidence.diagnosis.battleNetSupportLine, "Resolved URL"));
  assert(!evidence.diagnosis.blockers.empty());

#if !defined(_WIN32)
  assert(evidence.diagnosis.battleNetHandoffVisible);
  assert(evidence.diagnosis.battleNetHandoffCount == 1);
  assert(!evidence.diagnosis.multipleBattleNetHandoffsVisible);
  assert(evidence.diagnosis.staleHandoffSuspected);
  assert(evidence.diagnosis.status == "blocked-battlenet-handoff-short-lived-session-support-error");
  unsetEnvValue("STARCRAFT_API_PROCESS_SNAPSHOT");
#else
  assert(evidence.diagnosis.status == "blocked-short-lived-session-support-error-no-game-process");
#endif

  const std::string report = makeRuntimeEvidenceReport(evidence);
  assert(report.find("evidence.schema=starcraft-api.runtime-evidence.v1") != std::string::npos);
  assert(report.find("runtime.reason=unit test launch did not run") != std::string::npos);
  assert(report.find("runtime.required_stable_ms=0") != std::string::npos);
  assert(report.find("runtime.observed_stable_ms=0") != std::string::npos);
  assert(report.find("diagnosis.status=") != std::string::npos);
  assert(report.find("diagnosis.ready_for_attach=false") != std::string::npos);
  assert(report.find("diagnosis.short_lived_session_observed=true") != std::string::npos);
  assert(report.find("diagnosis.short_lived_session_age_ms=0") != std::string::npos);
  assert(report.find("diagnosis.battle_net_support_code=BLZBNTBNA00000005") != std::string::npos);
  assert(report.find("diagnosis.battle_net_support_url=https://kr.battle.net/support/ko/article/BLZBNTBNA00000005") != std::string::npos);
  assert(report.find("support.error_count=1") != std::string::npos);
  assert(report.find("support.error.0.code=BLZBNTBNA00000005") != std::string::npos);
  assert(report.find("Battle.net reported support error BLZBNTBNA00000005") != std::string::npos);
  assert(report.find("diagnosis.blocker_count=") != std::string::npos);
  assert(report.find("executable.fnv1a64=") != std::string::npos);
  assert(report.find("launch handoff failed in test") != std::string::npos);
  assert(report.find("session.launch_process_event_count=1") != std::string::npos);
  assert(report.find("session.latest_launch_process_id=777") != std::string::npos);
  assert(report.find("session.complete_transition_count=1") != std::string::npos);
  assert(report.find("session.latest_observed_timestamp=2026-06-19 08:00:06.350000") != std::string::npos);
  assert(report.find("session.latest_transition_duration_ms=6250") != std::string::npos);
  assert(report.find("session.latest_transition_start_timestamp=2026-06-19 08:00:00.100000") != std::string::npos);
  assert(report.find("session.latest_transition_end_timestamp=2026-06-19 08:00:06.350000") != std::string::npos);
  assert(report.find("session.transition.0.duration_ms=6250") != std::string::npos);
  assert(report.find("session.event.0.category=starcraft-launch-process") != std::string::npos);
  assert(report.find("session.event.1.category=starcraft-session-started") != std::string::npos);
  assert(report.find("session.event.2.category=starcraft-session-ended") != std::string::npos);

  const std::filesystem::path evidencePath = tempRoot / "runtime.evidence";
  assert(writeRuntimeEvidenceReport(installation, launchResult, evidencePath.string(), error));
  assert(std::filesystem::is_regular_file(evidencePath));

#if !defined(_WIN32)
  writeFile(
    logRoot / "battle.net-same-run-handoff.log",
    "I 2026-06-19 08:00:08.250000 [GameController] {Main} Selecting game by uid. uid=s1 prioritizeUpdate=1\n");
  RuntimeEvidence handoffAfterShortSessionEvidence = collectRuntimeEvidence(installation, launchResult);
  assert(handoffAfterShortSessionEvidence.sessionSummary.latestObservedTimestamp == "2026-06-19 08:00:08.250000");
  assert(handoffAfterShortSessionEvidence.sessionSummary.latestState == "handoff");
  assert(handoffAfterShortSessionEvidence.diagnosis.shortLivedSessionObserved);
  assert(handoffAfterShortSessionEvidence.diagnosis.shortLivedSessionAgeMilliseconds == 1900);
  assert(handoffAfterShortSessionEvidence.diagnosis.status == "blocked-battlenet-handoff-short-lived-session-support-error");

  std::string noisyOldLog;
  for (int i = 0; i < 60; ++i)
  {
    noisyOldLog += "I 2026-06-18 07:00:";
    noisyOldLog += i < 10 ? "0" : "";
    noisyOldLog += std::to_string(i);
    noisyOldLog += ".000000 [InstallManager] Game is no longer running: s1\n";
  }
  writeFile(logRoot / "battle.net-old-noisy.log", noisyOldLog);
  std::this_thread::sleep_for(std::chrono::milliseconds(25));
  writeFile(
    logRoot / "battle.net-new-handoff.log",
    "I 2026-06-20 03:18:14.277562 [GameController] {Main} Selecting game by uid. uid=s1 prioritizeUpdate=1\n");

  writeFile(
    processSnapshot,
    "4428 1 /Applications/Battle.net.app/Contents/MacOS/Battle.net --game=s1 --gamepath="
      + installRoot.string()
      + "/\n");
  setEnvValue("STARCRAFT_API_PROCESS_SNAPSHOT", processSnapshot.string());

  RuntimeLaunchResult currentHandoffLaunch;
  RuntimeEvidence currentHandoffEvidence = collectRuntimeEvidence(installation, currentHandoffLaunch);
  assert(currentHandoffEvidence.sessionSummary.latestObservedTimestamp == "2026-06-20 03:18:14.277562");
  assert(currentHandoffEvidence.sessionSummary.latestState == "handoff");
  assert(!currentHandoffEvidence.diagnosis.shortLivedSessionObserved);
  assert(currentHandoffEvidence.diagnosis.status == "blocked-battlenet-handoff-support-error");

  writeFile(
    processSnapshot,
    "4428 1 /Applications/Battle.net.app/Contents/MacOS/Battle.net --game=s1 --gamepath="
      + installRoot.string()
      + "/\n"
      + "4431 1 /Applications/Battle.net.app/Contents/MacOS/Battle.net --game=s1 --gamepath="
      + installRoot.string()
      + "/\n");
  RuntimeEvidence duplicateHandoffEvidence = collectRuntimeEvidence(installation, currentHandoffLaunch);
  assert(!duplicateHandoffEvidence.diagnosis.readyForAttach);
  assert(duplicateHandoffEvidence.diagnosis.battleNetHandoffCount == 2);
  assert(duplicateHandoffEvidence.diagnosis.multipleBattleNetHandoffsVisible);
  assert(duplicateHandoffEvidence.diagnosis.status == "blocked-multiple-battlenet-handoffs-support-error");

  writeFile(
    processSnapshot,
    "5501 1 /Applications/Battle.net.app/Contents/MacOS/Battle.net\n"
    "5502 1 /Applications/Battle.net.app/Contents/MacOS/Battle.net\n");
  RuntimeEvidence duplicateMainEvidence = collectRuntimeEvidence(installation, currentHandoffLaunch);
  assert(!duplicateMainEvidence.diagnosis.readyForAttach);
  assert(duplicateMainEvidence.diagnosis.battleNetMainCount == 2);
  assert(duplicateMainEvidence.diagnosis.multipleBattleNetMainVisible);
  assert(duplicateMainEvidence.diagnosis.status == "blocked-multiple-battlenet-main-processes-support-error-no-game");
#endif

  std::filesystem::remove_all(tempRoot);
  return 0;
}
