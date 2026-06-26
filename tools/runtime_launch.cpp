#include <BWAPI/Runtime/RuntimeInstallation.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>

using namespace BWAPI::Runtime;

namespace
{
  int parseNonNegativeInt(const std::string& value, const char* label)
  {
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0' || parsed < 0 || parsed > std::numeric_limits<int>::max())
    {
      std::cerr << label << " requires a non-negative integer\n";
      return -1;
    }
    return static_cast<int>(parsed);
  }

  void printUsage()
  {
    std::cout
      << "usage: starcraft-runtime-launch [options]\n"
      << "  --launch                 launch StarCraft if no matching process is running\n"
      << "  --replace-running        terminate visible StarCraft game process(es) before --launch\n"
      << "  --replace-stale-handoff  terminate existing Battle.net s1 handoff before one launch retry\n"
      << "  --require-running        return non-zero unless a matching process is visible\n"
      << "  --wait-ms <milliseconds> wait after launch while scanning for the process (default: 10000)\n"
      << "  --stable-ms <milliseconds> require the same StarCraft process to survive this long (default: 5000)\n"
      << "  --play-replay <path>     launch Remastered with an existing .rep via playReplay <path>\n"
      << "  env STARCRAFT_API_WINDOWED=0 disables the default partial-screen executable launch\n"
      << "  env STARCRAFT_API_WINDOW_WIDTH/HEIGHT/X/Y changes the default 1024x768+100+100 window\n"
      << "  env STARCRAFT_API_EXTRA_ARGS appends quoted extra args after the Remastered launch args\n"
      << "  --resident-adapter <path>\n"
      << "                           inject the in-process resident adapter into a direct StarCraft launch\n"
      << "  --manifest-out <path>    write a local bootstrap manifest\n"
      << "  --evidence-out <path>    write a launch/attach diagnostic evidence report\n"
      << "  --bridge <path>          write a filesystem bridge ready file\n"
      << "  --print-env              print shell environment exports for follow-up tools\n"
      << "  --help                   show this help\n";
  }

  void printExport(const char* name, const std::string& value)
  {
    if (!value.empty())
      std::cout << "export " << name << "='" << value << "'\n";
  }

  bool setEnvironmentVariable(const char* name, const std::string& value)
  {
#if defined(_WIN32)
    return _putenv_s(name, value.c_str()) == 0;
#else
    return setenv(name, value.c_str(), 1) == 0;
#endif
  }
}

int main(int argc, char** argv)
{
  bool launch = false;
  bool replaceRunning = false;
  bool replaceStaleHandoff = false;
  bool requireRunning = false;
  bool printEnv = false;
  int waitMilliseconds = 10000;
  int stableMilliseconds = 5000;
  std::string manifestOut;
  std::string evidenceOut;
  std::string bridgePath;
  std::string replayPath;
  std::string residentAdapterPath;

  for (int i = 1; i < argc; ++i)
  {
    const std::string arg = argv[i];
    if (arg == "--launch")
      launch = true;
    else if (arg == "--replace-running")
      replaceRunning = true;
    else if (arg == "--replace-stale-handoff")
      replaceStaleHandoff = true;
    else if (arg == "--require-running")
      requireRunning = true;
    else if (arg == "--print-env")
      printEnv = true;
    else if (arg == "--wait-ms")
    {
      if (i + 1 >= argc)
      {
        std::cerr << "--wait-ms requires a value\n";
        return 64;
      }
      waitMilliseconds = parseNonNegativeInt(argv[++i], "--wait-ms");
      if (waitMilliseconds < 0)
        return 64;
    }
    else if (arg == "--stable-ms")
    {
      if (i + 1 >= argc)
      {
        std::cerr << "--stable-ms requires a value\n";
        return 64;
      }
      stableMilliseconds = parseNonNegativeInt(argv[++i], "--stable-ms");
      if (stableMilliseconds < 0)
        return 64;
    }
    else if (arg == "--play-replay")
    {
      if (i + 1 >= argc)
      {
        std::cerr << "--play-replay requires a path\n";
        return 64;
      }
      replayPath = argv[++i];
      if (replayPath.empty())
      {
        std::cerr << "--play-replay requires a non-empty path\n";
        return 64;
      }
    }
    else if (arg == "--manifest-out")
    {
      if (i + 1 >= argc)
      {
        std::cerr << "--manifest-out requires a path\n";
        return 64;
      }
      manifestOut = argv[++i];
    }
    else if (arg == "--evidence-out")
    {
      if (i + 1 >= argc)
      {
        std::cerr << "--evidence-out requires a path\n";
        return 64;
      }
      evidenceOut = argv[++i];
    }
    else if (arg == "--resident-adapter")
    {
      if (i + 1 >= argc)
      {
        std::cerr << "--resident-adapter requires a path\n";
        return 64;
      }
      residentAdapterPath = argv[++i];
      if (residentAdapterPath.empty())
      {
        std::cerr << "--resident-adapter requires a non-empty path\n";
        return 64;
      }
    }
    else if (arg == "--bridge")
    {
      if (i + 1 >= argc)
      {
        std::cerr << "--bridge requires a path\n";
        return 64;
      }
      bridgePath = argv[++i];
    }
    else if (arg == "--help" || arg == "-h")
    {
      printUsage();
      return 0;
    }
    else
    {
      std::cerr << "unknown argument: " << arg << '\n';
      return 64;
    }
  }

  RuntimeEnvironment host = RuntimeEnvironment::detectHost();
  RuntimeInstallation installation = detectStarCraftInstallation(host);

  std::cout << "install.found=" << (installation.found ? "true" : "false") << '\n';
  std::cout << "platform=" << toString(installation.platform) << '\n';
  std::cout << "product=" << toString(installation.product) << '\n';
  if (!installation.version.empty())
    std::cout << "version=" << installation.version << '\n';
  if (!installation.installRoot.empty())
    std::cout << "install.root=" << installation.installRoot << '\n';
  if (!installation.appBundlePath.empty())
    std::cout << "install.app_bundle=" << installation.appBundlePath << '\n';
  if (!installation.executablePath.empty())
    std::cout << "install.executable=" << installation.executablePath << '\n';
  if (!installation.launcherPath.empty())
    std::cout << "install.launcher=" << installation.launcherPath << '\n';
  if (!installation.reason.empty())
    std::cout << "install.reason=" << installation.reason << '\n';
  for (const std::string& searchedPath : installation.searchedPaths)
    std::cout << "install.searched_path=" << searchedPath << '\n';

  if (!installation.found)
  {
    if (!evidenceOut.empty())
    {
      RuntimeLaunchResult launchResult;
      launchResult.requiredStableMilliseconds = stableMilliseconds;
      launchResult.reason = installation.reason;
      std::string error;
      if (!writeRuntimeEvidenceReport(installation, launchResult, evidenceOut, error))
      {
        std::cerr << error << '\n';
        return 1;
      }
      std::cout << "evidence.path=" << evidenceOut << '\n';
    }
    return 2;
  }

  if (!residentAdapterPath.empty())
  {
    std::error_code error;
    std::filesystem::path normalizedResidentAdapterPath =
      std::filesystem::absolute(residentAdapterPath, error);
    if (!error)
      normalizedResidentAdapterPath = normalizedResidentAdapterPath.lexically_normal();
    if (error || !std::filesystem::is_regular_file(normalizedResidentAdapterPath, error) || error)
    {
      std::cerr << "resident adapter dylib does not exist: " << residentAdapterPath << '\n';
      return 64;
    }
    residentAdapterPath = normalizedResidentAdapterPath.string();

    if (bridgePath.empty())
      bridgePath = (std::filesystem::temp_directory_path() / "starcraft-api-live-bridge").string();
    if (!setEnvironmentVariable("STARCRAFT_API_RESIDENT_ENABLE", "1")
        || !setEnvironmentVariable("STARCRAFT_API_EXECUTOR_BRIDGE_DIR", bridgePath))
    {
      std::cerr << "failed to set resident adapter environment\n";
      return 1;
    }
#if defined(__APPLE__)
    if (!setEnvironmentVariable("DYLD_INSERT_LIBRARIES", residentAdapterPath))
    {
      std::cerr << "failed to set DYLD_INSERT_LIBRARIES\n";
      return 1;
    }
#elif defined(__linux__)
    if (!setEnvironmentVariable("LD_PRELOAD", residentAdapterPath))
    {
      std::cerr << "failed to set LD_PRELOAD\n";
      return 1;
    }
#endif
  }

  RuntimeLaunchResult launchResult =
    launchOrAttachRuntime(
      installation,
      launch,
      waitMilliseconds,
      stableMilliseconds,
      replaceStaleHandoff,
      replaceRunning,
      replayPath);
  std::cout << "runtime.launched=" << (launchResult.launched ? "true" : "false") << '\n';
  std::cout << "runtime.running=" << (launchResult.running ? "true" : "false") << '\n';
  std::cout << "runtime.required_stable_ms=" << launchResult.requiredStableMilliseconds << '\n';
  std::cout << "runtime.observed_stable_ms=" << launchResult.observedStableMilliseconds << '\n';
  if (launchResult.running && launchResult.processId > 0)
    std::cout << "runtime.process_id=" << launchResult.processId << '\n';
  else if (launchResult.launched && launchResult.processId > 0)
    std::cout << "runtime.launch_process_id=" << launchResult.processId << '\n';
  if (!launchResult.reason.empty())
    std::cout << "runtime.reason=" << launchResult.reason << '\n';
  for (const std::string& warning : launchResult.warnings)
    std::cout << "runtime.warning=" << warning << '\n';
  if (!launchResult.requestAccepted)
    std::cout << "runtime.request_accepted=false\n";

  if (!evidenceOut.empty())
  {
    std::string error;
    if (!writeRuntimeEvidenceReport(installation, launchResult, evidenceOut, error))
    {
      std::cerr << error << '\n';
      return 1;
    }
    std::cout << "evidence.path=" << evidenceOut << '\n';
  }

  RuntimeEnvironment runtimeEnvironment =
    makeRuntimeEnvironmentForInstallation(host, installation, launchResult.running ? launchResult.processId : 0);

  if (!manifestOut.empty())
  {
    std::string error;
    if (!writeRuntimeBootstrapManifest(installation, manifestOut, error))
    {
      std::cerr << error << '\n';
      return 1;
    }
    runtimeEnvironment.manifestPath = manifestOut;
    std::cout << "manifest.path=" << manifestOut << '\n';
  }

  if (!bridgePath.empty())
  {
    if (residentAdapterPath.empty())
    {
      std::string error;
      if (!writeRuntimeExecutorReadyFile(runtimeEnvironment, bridgePath, error))
      {
        std::cerr << error << '\n';
        return 1;
      }
    }
    runtimeEnvironment.executorBridgePath = bridgePath;
    std::cout << "executor.bridge_path=" << bridgePath << '\n';
    if (!residentAdapterPath.empty())
      std::cout << "executor.resident_adapter=" << residentAdapterPath << '\n';
  }

  if (printEnv)
  {
    printExport("STARCRAFT_API_PRODUCT", toString(runtimeEnvironment.product));
    printExport("STARCRAFT_API_VERSION", runtimeEnvironment.version);
    if (runtimeEnvironment.processId > 0)
      printExport("STARCRAFT_API_PROCESS_ID", std::to_string(runtimeEnvironment.processId));
    printExport("STARCRAFT_API_EXECUTABLE", runtimeEnvironment.executablePath);
    printExport("STARCRAFT_API_MANIFEST", runtimeEnvironment.manifestPath);
    printExport("STARCRAFT_API_EXECUTOR_BRIDGE_DIR", runtimeEnvironment.executorBridgePath);
  }

  if (!launchResult.requestAccepted)
    return 64;

  if (requireRunning && !launchResult.running)
    return 3;

  return 0;
}
