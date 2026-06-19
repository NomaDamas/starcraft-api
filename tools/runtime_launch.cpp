#include <BWAPI/Runtime/RuntimeInstallation.h>

#include <cstdlib>
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
      << "  --require-running        return non-zero unless a matching process is visible\n"
      << "  --wait-ms <milliseconds> wait after launch while scanning for the process (default: 10000)\n"
      << "  --manifest-out <path>    write a local bootstrap manifest\n"
      << "  --bridge <path>          write a filesystem bridge ready file\n"
      << "  --print-env              print shell environment exports for follow-up tools\n"
      << "  --help                   show this help\n";
  }

  void printExport(const char* name, const std::string& value)
  {
    if (!value.empty())
      std::cout << "export " << name << "='" << value << "'\n";
  }
}

int main(int argc, char** argv)
{
  bool launch = false;
  bool requireRunning = false;
  bool printEnv = false;
  int waitMilliseconds = 10000;
  std::string manifestOut;
  std::string bridgePath;

  for (int i = 1; i < argc; ++i)
  {
    const std::string arg = argv[i];
    if (arg == "--launch")
      launch = true;
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
    else if (arg == "--manifest-out")
    {
      if (i + 1 >= argc)
      {
        std::cerr << "--manifest-out requires a path\n";
        return 64;
      }
      manifestOut = argv[++i];
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
    return 2;

  RuntimeLaunchResult launchResult = launchOrAttachRuntime(installation, launch, waitMilliseconds);
  std::cout << "runtime.launched=" << (launchResult.launched ? "true" : "false") << '\n';
  std::cout << "runtime.running=" << (launchResult.running ? "true" : "false") << '\n';
  if (launchResult.running && launchResult.processId > 0)
    std::cout << "runtime.process_id=" << launchResult.processId << '\n';
  else if (launchResult.launched && launchResult.processId > 0)
    std::cout << "runtime.launch_process_id=" << launchResult.processId << '\n';
  if (!launchResult.reason.empty())
    std::cout << "runtime.reason=" << launchResult.reason << '\n';
  for (const std::string& warning : launchResult.warnings)
    std::cout << "runtime.warning=" << warning << '\n';

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
    std::string error;
    if (!writeRuntimeExecutorReadyFile(runtimeEnvironment, bridgePath, error))
    {
      std::cerr << error << '\n';
      return 1;
    }
    runtimeEnvironment.executorBridgePath = bridgePath;
    std::cout << "executor.bridge_path=" << bridgePath << '\n';
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

  if (requireRunning && !launchResult.running)
    return 3;

  return 0;
}
