#include <BWAPI/Runtime/RuntimeInstallation.h>

#include <BWAPI/Runtime/RuntimeExecutor.h>
#include <BWAPI/Runtime/RuntimeProcess.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace BWAPI::Runtime
{
  namespace
  {
    std::string getenvString(const char* name)
    {
      if (const char* value = std::getenv(name))
      {
        if (*value != '\0')
          return value;
      }
      return {};
    }

    std::string pathString(const std::filesystem::path& path)
    {
      std::error_code error;
      const std::filesystem::path absolute = std::filesystem::absolute(path, error);
      if (!error)
        return absolute.lexically_normal().string();
      return path.lexically_normal().string();
    }

    bool fileExists(const std::filesystem::path& path)
    {
      std::error_code error;
      return std::filesystem::is_regular_file(path, error) && !error;
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

    std::string detectMacBundleVersion(const std::filesystem::path& appBundlePath)
    {
      const std::filesystem::path plistPath = appBundlePath / "Contents" / "Info.plist";
      std::string version = readPlistStringValue(plistPath, "CFBundleShortVersionString");
      if (!version.empty())
        return version;
      version = readPlistStringValue(plistPath, "CFBundleVersion");
      if (!version.empty())
        return version;
      return "unknown";
    }

    RuntimeInstallation makeFoundInstallation(
      Platform platform,
      Product product,
      const std::filesystem::path& installRoot,
      const std::filesystem::path& executablePath,
      const std::filesystem::path& appBundlePath,
      const std::filesystem::path& launcherPath)
    {
      RuntimeInstallation installation;
      installation.found = true;
      installation.platform = platform;
      installation.product = product;
      installation.installRoot = pathString(installRoot);
      installation.executablePath = pathString(executablePath);
      if (!appBundlePath.empty())
      {
        installation.appBundlePath = pathString(appBundlePath);
        installation.version = detectMacBundleVersion(appBundlePath);
      }
      else
      {
        installation.version = "unknown";
      }
      if (!launcherPath.empty())
        installation.launcherPath = pathString(launcherPath);
      return installation;
    }

    bool tryMacRoot(const std::filesystem::path& root, RuntimeInstallation& installation)
    {
      const std::filesystem::path app = root / "x86_64" / "StarCraft.app";
      const std::filesystem::path executable = app / "Contents" / "MacOS" / "StarCraft";
      const std::filesystem::path launcher =
        root / "StarCraft Launcher.app" / "Contents" / "MacOS" / "StarCraft Launcher";
      if (!fileExists(executable))
        return false;

      installation = makeFoundInstallation(
        Platform::MacOS,
        Product::StarCraftRemastered,
        root,
        executable,
        app,
        fileExists(launcher) ? launcher : std::filesystem::path());
      return true;
    }

    bool tryMacAppBundle(const std::filesystem::path& app, RuntimeInstallation& installation)
    {
      const std::filesystem::path executable = app / "Contents" / "MacOS" / "StarCraft";
      if (!fileExists(executable))
        return false;

      const std::filesystem::path root = app.parent_path().parent_path();
      const std::filesystem::path launcher =
        root / "StarCraft Launcher.app" / "Contents" / "MacOS" / "StarCraft Launcher";
      installation = makeFoundInstallation(
        Platform::MacOS,
        Product::StarCraftRemastered,
        root,
        executable,
        app,
        fileExists(launcher) ? launcher : std::filesystem::path());
      return true;
    }

    bool tryWindowsRoot(const std::filesystem::path& root, Platform platform, RuntimeInstallation& installation)
    {
      const std::filesystem::path executable = root / "StarCraft.exe";
      const std::filesystem::path launcher = root / "StarCraft Launcher.exe";
      if (!fileExists(executable))
        return false;

      installation = makeFoundInstallation(
        platform,
        Product::StarCraftRemastered,
        root,
        executable,
        {},
        fileExists(launcher) ? launcher : std::filesystem::path());
      return true;
    }

    bool tryExecutableOverride(const RuntimeEnvironment& environment, RuntimeInstallation& installation)
    {
      if (environment.executablePath.empty())
        return false;

      const std::filesystem::path executable(environment.executablePath);
      if (!fileExists(executable))
        return false;

      const Product product =
        environment.product == Product::Unknown ? Product::StarCraftRemastered : environment.product;
      const Platform platform =
        environment.platform == Platform::Unknown ? RuntimeEnvironment::detectHost().platform : environment.platform;

      std::filesystem::path appBundle;
      std::filesystem::path cursor = executable.parent_path();
      while (!cursor.empty())
      {
        if (cursor.extension() == ".app")
        {
          appBundle = cursor;
          break;
        }
        const std::filesystem::path parent = cursor.parent_path();
        if (parent == cursor)
          break;
        cursor = parent;
      }

      const std::filesystem::path root = appBundle.empty()
        ? executable.parent_path()
        : appBundle.parent_path().parent_path();

      installation = makeFoundInstallation(
        platform,
        product,
        root,
        executable,
        appBundle,
        {});
      if (!environment.version.empty())
        installation.version = environment.version;
      return true;
    }

    void addCandidate(std::vector<std::filesystem::path>& candidates, const std::filesystem::path& path)
    {
      if (path.empty())
        return;
      candidates.push_back(path);
    }

    std::vector<std::filesystem::path> installCandidates()
    {
      std::vector<std::filesystem::path> candidates;

      addCandidate(candidates, getenvString("STARCRAFT_API_INSTALL_DIR"));
      addCandidate(candidates, getenvString("STARCRAFT_API_STARCRAFT_DIR"));

      const std::string home = getenvString("HOME");
      if (!home.empty())
      {
        addCandidate(candidates, std::filesystem::path(home) / "Desktop" / "Starcraft1" / "StarCraft");
        addCandidate(candidates, std::filesystem::path(home) / "Desktop" / "StarCraft1" / "StarCraft");
        addCandidate(candidates, std::filesystem::path(home) / "Desktop" / "StarCraft" / "StarCraft");
        addCandidate(candidates, std::filesystem::path(home) / "Desktop" / "StarCraft");
        addCandidate(candidates, std::filesystem::path(home) / "Applications" / "StarCraft");
      }

      addCandidate(candidates, "/Applications/StarCraft");
      addCandidate(candidates, "/Applications/StarCraft Remastered");
      addCandidate(candidates, "/Applications/Starcraft1/StarCraft");
      addCandidate(candidates, "/Program Files (x86)/StarCraft");
      addCandidate(candidates, "/Program Files/StarCraft");

      return candidates;
    }

    bool lineContainsAny(std::string_view line, const std::vector<std::string>& needles)
    {
      for (const std::string& needle : needles)
      {
        if (!needle.empty() && line.find(needle) != std::string_view::npos)
          return true;
      }
      return false;
    }

#if !defined(_WIN32)
    std::vector<int> parseProcessListMatching(const std::vector<std::string>& needles)
    {
      std::vector<int> processIds;
      FILE* pipe = popen("ps -axo pid=,command=", "r");
      if (pipe == nullptr)
        return processIds;

      char buffer[4096];
      while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr)
      {
        std::string line(buffer);
        const std::size_t firstDigit = line.find_first_of("0123456789");
        if (firstDigit == std::string::npos)
          continue;
        const std::size_t afterPid = line.find_first_not_of("0123456789", firstDigit);
        if (afterPid == std::string::npos)
          continue;

        const int processId = std::atoi(line.substr(firstDigit, afterPid - firstDigit).c_str());
        if (processId <= 0)
          continue;

        const std::string command = line.substr(afterPid);
        if (lineContainsAny(command, needles))
          processIds.push_back(processId);
      }

      pclose(pipe);
      return processIds;
    }

    std::vector<int> findPosixProcesses(const RuntimeInstallation& installation)
    {
      return parseProcessListMatching({
        installation.executablePath,
        "StarCraft.app/Contents/MacOS/StarCraft"
      });
    }

    std::vector<int> findPosixBattleNetHandoffProcesses(const RuntimeInstallation& installation)
    {
      return parseProcessListMatching({
        "Battle.net.app/Contents/MacOS/Battle.net --game=s1",
        "Battle.net.exe --game=s1",
        installation.installRoot
      });
    }
#endif

    bool containsProcessId(const std::vector<int>& processIds, int processId)
    {
      for (int candidate : processIds)
      {
        if (candidate == processId)
          return true;
      }
      return false;
    }

    int findStableProcessId(const RuntimeInstallation& installation)
    {
      const std::vector<int> firstPass = findRuntimeProcessIds(installation);
      if (firstPass.empty())
        return 0;

      const int processId = firstPass.front();
      std::this_thread::sleep_for(std::chrono::milliseconds(5000));

      const std::vector<int> secondPass = findRuntimeProcessIds(installation);
      if (containsProcessId(secondPass, processId))
        return processId;
      return 0;
    }

#if defined(_WIN32)
    std::vector<int> findWindowsProcesses(const RuntimeInstallation& installation)
    {
      (void)installation;
      return {};
    }

    std::vector<int> findWindowsBattleNetHandoffProcesses()
    {
      return {};
    }
#endif

#if !defined(_WIN32)
    bool forkExecProcess(
      const std::string& target,
      const std::string& workingDirectory,
      pid_t& processId,
      std::string& reason)
    {
      processId = fork();
      if (processId < 0)
      {
        reason = "fork failed for " + target;
        return false;
      }

      if (processId == 0)
      {
        if (!workingDirectory.empty())
          chdir(workingDirectory.c_str());

        char* const argv[] = {
          const_cast<char*>(target.c_str()),
          nullptr
        };
        execv(target.c_str(), argv);
        _exit(127);
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      int status = 0;
      const pid_t exited = waitpid(processId, &status, WNOHANG);
      if (exited == processId)
      {
        std::ostringstream message;
        message << "process exited immediately for " << target << " with status " << status;
        reason = message.str();
        return false;
      }

      return true;
    }
#endif

    RuntimeLaunchResult launchRuntimeProcess(const RuntimeInstallation& installation)
    {
      RuntimeLaunchResult result;

#if defined(_WIN32)
      std::string command = "\"" + installation.executablePath + "\"";
      STARTUPINFOA startupInfo;
      PROCESS_INFORMATION processInfo;
      ZeroMemory(&startupInfo, sizeof(startupInfo));
      ZeroMemory(&processInfo, sizeof(processInfo));
      startupInfo.cb = sizeof(startupInfo);

      if (!CreateProcessA(
            nullptr,
            command.data(),
            nullptr,
            nullptr,
            FALSE,
            0,
            nullptr,
            installation.installRoot.empty() ? nullptr : installation.installRoot.c_str(),
            &startupInfo,
            &processInfo))
      {
        if (installation.launcherPath.empty())
        {
          result.reason = "CreateProcess failed with Windows error " + std::to_string(GetLastError());
          return result;
        }

        command = "\"" + installation.launcherPath + "\"";
        ZeroMemory(&processInfo, sizeof(processInfo));
        if (!CreateProcessA(
              nullptr,
              command.data(),
              nullptr,
              nullptr,
              FALSE,
              0,
              nullptr,
              installation.installRoot.empty() ? nullptr : installation.installRoot.c_str(),
              &startupInfo,
              &processInfo))
        {
          result.reason = "CreateProcess launcher fallback failed with Windows error " + std::to_string(GetLastError());
          return result;
        }
      }

      result.launched = true;
      result.processId = static_cast<int>(processInfo.dwProcessId);
      CloseHandle(processInfo.hThread);
      CloseHandle(processInfo.hProcess);
#else
      pid_t pid = 0;
      std::string reason;
      if (!forkExecProcess(installation.executablePath, installation.installRoot, pid, reason))
      {
        if (installation.launcherPath.empty())
        {
          result.reason = reason;
          return result;
        }

        if (!forkExecProcess(installation.launcherPath, installation.installRoot, pid, reason))
        {
          result.reason = "launcher fallback failed: " + reason;
          return result;
        }
      }

      result.launched = true;
      result.processId = static_cast<int>(pid);
#endif

      return result;
    }

    std::vector<int> findBattleNetHandoffProcesses(const RuntimeInstallation& installation)
    {
#if defined(_WIN32)
      return findWindowsBattleNetHandoffProcesses();
#else
      return findPosixBattleNetHandoffProcesses(installation);
#endif
    }

    void writeAllCommandSurface(std::ostringstream& output)
    {
      output << "# Command surface is intentionally not claimed in the bootstrap manifest.\n";
      output << "# Add validated unit-command and game-action directives only after the in-game adapter proves them.\n";
    }
  }

  RuntimeInstallation detectStarCraftInstallation(const RuntimeEnvironment& environment)
  {
    RuntimeInstallation installation;
    installation.platform = environment.platform == Platform::Unknown
      ? RuntimeEnvironment::detectHost().platform
      : environment.platform;
    installation.product = Product::StarCraftRemastered;

    if (tryExecutableOverride(environment, installation))
      return installation;

    const std::vector<std::filesystem::path> candidates = installCandidates();
    for (const std::filesystem::path& candidate : candidates)
    {
      installation.searchedPaths.push_back(pathString(candidate));

      RuntimeInstallation found;
      if (tryMacRoot(candidate, found))
        return found;
      if (tryMacRoot(candidate / "StarCraft", found))
        return found;
      if (tryMacAppBundle(candidate, found))
        return found;
      if (tryMacAppBundle(candidate / "x86_64" / "StarCraft.app", found))
        return found;
      if (tryWindowsRoot(candidate, installation.platform, found))
        return found;
      if (tryWindowsRoot(candidate / "StarCraft", installation.platform, found))
        return found;
    }

    installation.found = false;
    installation.reason = "StarCraft Remastered installation was not found. Set STARCRAFT_API_INSTALL_DIR or STARCRAFT_API_EXECUTABLE.";
    return installation;
  }

  std::vector<int> findRuntimeProcessIds(const RuntimeInstallation& installation)
  {
    if (!installation.found)
      return {};

#if defined(_WIN32)
    return findWindowsProcesses(installation);
#else
    return findPosixProcesses(installation);
#endif
  }

  RuntimeLaunchResult launchOrAttachRuntime(
    const RuntimeInstallation& installation,
    bool launchIfMissing,
    int waitMilliseconds)
  {
    RuntimeLaunchResult result;
    if (!installation.found)
    {
      result.reason = installation.reason;
      return result;
    }

    int stableProcessId = findStableProcessId(installation);
    if (stableProcessId > 0)
    {
      result.running = true;
      result.processId = stableProcessId;
      return result;
    }

    if (!launchIfMissing)
    {
      result.reason = "StarCraft Remastered is installed but no running process was found";
      return result;
    }

    const std::vector<int> handoffProcessIds = findBattleNetHandoffProcesses(installation);
    if (!handoffProcessIds.empty())
    {
      result.reason =
        "Battle.net StarCraft handoff is already running; not launching another Battle.net instance";
      result.warnings.push_back("battle.net.process_id=" + std::to_string(handoffProcessIds.front()));
      return result;
    }

    RuntimeLaunchResult launched = launchRuntimeProcess(installation);
    result.launched = launched.launched;
    result.processId = launched.processId;
    if (!launched.launched)
    {
      result.reason = launched.reason;
      return result;
    }

#if defined(_WIN32)
    if (launched.processId > 0)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(5000));
      if (runtimeProcessExists(launched.processId))
      {
        result.running = true;
        result.processId = launched.processId;
        return result;
      }
    }
#endif

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(waitMilliseconds);
    while (std::chrono::steady_clock::now() < deadline)
    {
      stableProcessId = findStableProcessId(installation);
      if (stableProcessId > 0)
      {
        result.running = true;
        result.processId = stableProcessId;
        return result;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    result.running = false;
    result.reason = "StarCraft Remastered launch was requested, but no matching game process became visible before the wait timeout";
    return result;
  }

  RuntimeEnvironment makeRuntimeEnvironmentForInstallation(
    const RuntimeEnvironment& baseEnvironment,
    const RuntimeInstallation& installation,
    int processId)
  {
    RuntimeEnvironment environment = baseEnvironment;
    environment.platform = installation.platform;
    environment.product = installation.product;
    environment.version = installation.version;
    environment.executablePath = installation.executablePath;
    environment.processId = processId;
    return environment;
  }

  std::string makeRuntimeBootstrapManifest(const RuntimeInstallation& installation)
  {
    std::ostringstream output;
    output << "# Generated StarCraft API runtime bootstrap manifest.\n";
    output << "# This manifest records local installation identity only.\n";
    output << "# It must not be treated as BWAPI parity evidence until validated in-game bindings are added.\n";
    output << "product " << toString(installation.product) << '\n';
    output << "version " << (installation.version.empty() ? "unknown" : installation.version) << '\n';
    output << "api-surface-methods 0\n";
    output << "command-surface-entries 0\n";
    writeAllCommandSurface(output);
    return output.str();
  }

  bool writeRuntimeBootstrapManifest(
    const RuntimeInstallation& installation,
    const std::string& path,
    std::string& error)
  {
    std::ofstream output(path);
    if (!output)
    {
      error = "unable to write runtime bootstrap manifest: " + path;
      return false;
    }
    output << makeRuntimeBootstrapManifest(installation);
    return true;
  }

  bool writeRuntimeExecutorReadyFile(
    const RuntimeEnvironment& environment,
    const std::string& bridgePath,
    std::string& error)
  {
    std::error_code fsError;
    std::filesystem::create_directories(bridgePath, fsError);
    if (fsError)
    {
      error = "unable to create runtime bridge directory: " + fsError.message();
      return false;
    }

    const std::filesystem::path readyPath =
      std::filesystem::path(bridgePath) / RuntimeExecutorBridgeReadyFile;
    std::ofstream ready(readyPath);
    if (!ready)
    {
      error = "unable to write runtime bridge ready file: " + readyPath.string();
      return false;
    }

    ready << "protocol=" << RuntimeExecutorBridgeProtocol << '\n';
    ready << "product=" << toString(environment.product) << '\n';
    ready << "version=" << (environment.version.empty() ? "unknown" : environment.version) << '\n';
    ready << "executor=starcraft-api-local-runtime\n";
    ready << "mode=launch-attach-bootstrap\n";
    ready << "process_id=" << environment.processId << '\n';
    ready << "executable=" << environment.executablePath << '\n';
    return true;
  }
}
