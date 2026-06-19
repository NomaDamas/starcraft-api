#include <BWAPI/Runtime/RuntimeInstallation.h>

#include <BWAPI/Runtime/RuntimeExecutor.h>
#include <BWAPI/Runtime/RuntimeProcess.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
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

    std::string trimLeft(std::string value)
    {
      const std::size_t first = value.find_first_not_of(" \t\r\n");
      if (first == std::string::npos)
        return {};
      return value.substr(first);
    }

    std::string sanitizeEvidenceValue(std::string value)
    {
      for (char& ch : value)
      {
        if (ch == '\n' || ch == '\r' || ch == '\t')
          ch = ' ';
      }

      constexpr std::size_t MaxEvidenceValueLength = 2000;
      if (value.size() > MaxEvidenceValueLength)
        value = value.substr(0, MaxEvidenceValueLength) + "...";
      return value;
    }

    void writeEvidenceField(std::ostringstream& output, const std::string& key, const std::string& value)
    {
      if (!value.empty())
        output << key << '=' << sanitizeEvidenceValue(value) << '\n';
    }

    void writeEvidenceField(std::ostringstream& output, const std::string& key, const char* value)
    {
      if (value != nullptr)
        writeEvidenceField(output, key, std::string(value));
    }

    void writeEvidenceField(std::ostringstream& output, const std::string& key, bool value)
    {
      output << key << '=' << (value ? "true" : "false") << '\n';
    }

    void writeEvidenceField(std::ostringstream& output, const std::string& key, int value)
    {
      output << key << '=' << value << '\n';
    }

    void writeEvidenceField(std::ostringstream& output, const std::string& key, std::uintmax_t value)
    {
      output << key << '=' << value << '\n';
    }

    RuntimeFileIdentity identifyRuntimeFile(const std::string& path)
    {
      RuntimeFileIdentity identity;
      identity.path = path;
      if (path.empty())
      {
        identity.reason = "path is empty";
        return identity;
      }

      const std::filesystem::path filePath(path);
      if (!fileExists(filePath))
      {
        identity.reason = "file does not exist";
        return identity;
      }

      std::error_code error;
      identity.size = std::filesystem::file_size(filePath, error);
      if (error)
      {
        identity.reason = "unable to stat file: " + error.message();
        return identity;
      }

      std::ifstream input(filePath, std::ios::binary);
      if (!input)
      {
        identity.reason = "unable to read file";
        return identity;
      }

      std::uint64_t hash = 14695981039346656037ull;
      char buffer[8192];
      while (input)
      {
        input.read(buffer, sizeof(buffer));
        const std::streamsize count = input.gcount();
        for (std::streamsize i = 0; i < count; ++i)
        {
          hash ^= static_cast<unsigned char>(buffer[i]);
          hash *= 1099511628211ull;
        }
      }

      std::ostringstream hex;
      hex << std::hex << std::setfill('0') << std::setw(16) << hash;
      identity.exists = true;
      identity.fnv1a64 = hex.str();
      return identity;
    }

    std::string categorizeProcessCommand(const RuntimeInstallation& installation, const std::string& command)
    {
      if (lineContainsAny(command, { installation.executablePath, "StarCraft.app/Contents/MacOS/StarCraft" }))
        return "starcraft-game";

      const bool isBattleNet = lineContainsAny(command, {
        "Battle.net.app/Contents/MacOS/Battle.net",
        "Battle.net.exe",
        "Battle.net Helper",
        "Agent.app/Contents/MacOS/Agent"
      });

      if (isBattleNet && command.find("--game=s1") != std::string::npos)
        return "battle.net-handoff";

      if (isBattleNet)
        return "battle.net-support";

      if (lineContainsAny(command, {
        installation.installRoot,
        "StarCraft Launcher.app/Contents/MacOS/StarCraft Launcher",
        "StarCraft Launcher.exe"
      }))
        return "starcraft-related";

      return {};
    }

    bool isRelevantObservedProcess(const RuntimeObservedProcess& process)
    {
      return !process.category.empty();
    }

#if !defined(_WIN32)
    std::vector<RuntimeObservedProcess> collectPosixObservedProcesses(const RuntimeInstallation& installation)
    {
      std::vector<RuntimeObservedProcess> processes;
      FILE* pipe = popen("ps -axo pid=,ppid=,command= 2>/dev/null", "r");
      if (pipe == nullptr)
        return processes;

      char buffer[4096];
      while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr)
      {
        std::string line(buffer);
        std::istringstream parser(line);
        RuntimeObservedProcess process;
        if (!(parser >> process.processId >> process.parentProcessId))
          continue;
        if (process.processId <= 0)
          continue;

        std::getline(parser, process.command);
        process.command = trimLeft(process.command);
        process.category = categorizeProcessCommand(installation, process.command);
        if (isRelevantObservedProcess(process))
          processes.push_back(process);
      }

      pclose(pipe);
      return processes;
    }

    std::vector<int> processIdsForCategory(
      const RuntimeInstallation& installation,
      const std::vector<std::string>& categories)
    {
      std::vector<int> processIds;
      for (const RuntimeObservedProcess& process : collectPosixObservedProcesses(installation))
      {
        if (lineContainsAny(process.category, categories))
          processIds.push_back(process.processId);
      }
      return processIds;
    }

    std::vector<int> findPosixProcesses(const RuntimeInstallation& installation)
    {
      return processIdsForCategory(installation, { "starcraft-game" });
    }

    std::vector<int> findPosixBattleNetHandoffProcesses(const RuntimeInstallation& installation)
    {
      return processIdsForCategory(installation, { "battle.net-handoff" });
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

    std::vector<RuntimeObservedProcess> collectObservedProcesses(const RuntimeInstallation& installation)
    {
#if defined(_WIN32)
      (void)installation;
      return {};
#else
      return collectPosixObservedProcesses(installation);
#endif
    }

    void addLogCandidate(std::vector<std::filesystem::path>& candidates, const std::filesystem::path& path)
    {
      if (!path.empty())
        candidates.push_back(path);
    }

    struct CandidateLogFile
    {
      std::filesystem::path path;
      std::filesystem::file_time_type lastWriteTime;
    };

    std::vector<std::filesystem::path> runtimeLogCandidates(const RuntimeInstallation& installation)
    {
      std::vector<std::filesystem::path> candidates;

      const std::string overrideLogDir = getenvString("STARCRAFT_API_LOG_DIR");
      if (!overrideLogDir.empty())
      {
        addLogCandidate(candidates, overrideLogDir);
        return candidates;
      }

      const std::string home = getenvString("HOME");
      if (!home.empty())
      {
        addLogCandidate(candidates, std::filesystem::path(home) / "Library" / "Application Support" / "Battle.net" / "Logs");
        addLogCandidate(candidates, std::filesystem::path(home) / "Library" / "Application Support" / "Blizzard" / "StarCraft");
      }

      const std::string userProfile = getenvString("USERPROFILE");
      if (!userProfile.empty())
      {
        addLogCandidate(candidates, std::filesystem::path(userProfile) / "AppData" / "Local" / "Battle.net" / "Logs");
        addLogCandidate(candidates, std::filesystem::path(userProfile) / "Documents" / "StarCraft");
      }

      addLogCandidate(candidates, std::filesystem::path(installation.installRoot) / "Logs");
      return candidates;
    }

    std::vector<std::string> tailFileLines(const std::filesystem::path& path, std::size_t maxLines)
    {
      std::ifstream input(path);
      if (!input)
        return {};

      std::deque<std::string> tail;
      std::string line;
      while (std::getline(input, line))
      {
        tail.push_back(sanitizeEvidenceValue(line));
        while (tail.size() > maxLines)
          tail.pop_front();
      }

      return { tail.begin(), tail.end() };
    }

    std::vector<CandidateLogFile> collectRuntimeLogFiles(const RuntimeInstallation& installation)
    {
      std::vector<CandidateLogFile> files;
      for (const std::filesystem::path& root : runtimeLogCandidates(installation))
      {
        std::error_code error;
        if (!std::filesystem::is_directory(root, error) || error)
          continue;

        for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(root, error))
        {
          if (error)
            break;

          std::error_code entryError;
          if (!entry.is_regular_file(entryError) || entryError)
            continue;

          const std::filesystem::path path = entry.path();
          const std::string extension = path.extension().string();
          if (extension != ".log" && extension != ".txt")
            continue;

          std::error_code timeError;
          const std::filesystem::file_time_type lastWriteTime = entry.last_write_time(timeError);
          if (timeError)
            continue;

          files.push_back({ path, lastWriteTime });
        }
      }

      std::sort(files.begin(), files.end(), [](const CandidateLogFile& left, const CandidateLogFile& right) {
        return left.lastWriteTime > right.lastWriteTime;
      });
      return files;
    }

    std::vector<RuntimeLogExcerpt> collectRuntimeLogExcerpts(
      const RuntimeInstallation& installation,
      std::size_t maxFiles,
      std::size_t maxLines)
    {
      std::vector<RuntimeLogExcerpt> excerpts;
      for (const CandidateLogFile& file : collectRuntimeLogFiles(installation))
      {
        if (excerpts.size() >= maxFiles)
          break;

        RuntimeLogExcerpt excerpt;
        excerpt.path = pathString(file.path);
        excerpt.lines = tailFileLines(file.path, maxLines);
        if (excerpt.lines.empty())
          excerpt.reason = "log file is empty or unreadable";
        excerpts.push_back(excerpt);
      }

      return excerpts;
    }

    std::string categorizeSessionLogLine(const std::string& line)
    {
      const bool mentionsStarCraft = line.find("uid=s1") != std::string::npos
        || line.find("agentUid=s1") != std::string::npos
        || line.find("InstallState (s1)") != std::string::npos
        || line.find("Game is running: s1") != std::string::npos
        || line.find("Game is no longer running: s1") != std::string::npos;
      if (!mentionsStarCraft)
        return {};

      if (line.find("Game is running: s1") != std::string::npos
          || line.find("Setting Process Running: true uid=s1") != std::string::npos
          || line.find("opStatus=On") != std::string::npos)
        return "starcraft-session-started";

      if (line.find("Game is no longer running: s1") != std::string::npos
          || line.find("Setting Process Running: false uid=s1") != std::string::npos
          || line.find("opStatus=Off") != std::string::npos)
        return "starcraft-session-ended";

      if (line.find("Pre-existing game session detected") != std::string::npos)
        return "starcraft-session-preexisting";

      if (line.find("InstallState (s1)") != std::string::npos)
        return "starcraft-install-state";

      return "starcraft-session-related";
    }

    std::vector<RuntimeSessionEvent> collectRuntimeSessionEvents(
      const RuntimeInstallation& installation,
      std::size_t maxFiles,
      std::size_t maxEvents)
    {
      std::deque<RuntimeSessionEvent> tail;
      std::size_t inspectedFiles = 0;
      for (const CandidateLogFile& file : collectRuntimeLogFiles(installation))
      {
        if (inspectedFiles >= maxFiles)
          break;
        ++inspectedFiles;

        std::ifstream input(file.path);
        if (!input)
          continue;

        std::string line;
        while (std::getline(input, line))
        {
          const std::string sanitized = sanitizeEvidenceValue(line);
          const std::string category = categorizeSessionLogLine(sanitized);
          if (category.empty())
            continue;

          RuntimeSessionEvent event;
          event.path = pathString(file.path);
          event.category = category;
          event.line = sanitized;
          tail.push_back(event);
          while (tail.size() > maxEvents)
            tail.pop_front();
        }
      }

      return { tail.begin(), tail.end() };
    }

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
      result.warnings.push_back("battle.net.process_count=" + std::to_string(handoffProcessIds.size()));
      result.warnings.push_back("battle.net.process_id=" + std::to_string(handoffProcessIds.front()));
      for (std::size_t i = 0; i < handoffProcessIds.size(); ++i)
        result.warnings.push_back("battle.net.process_id." + std::to_string(i) + "=" + std::to_string(handoffProcessIds[i]));
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

  RuntimeEvidence collectRuntimeEvidence(
    const RuntimeInstallation& installation,
    const RuntimeLaunchResult& launchResult)
  {
    RuntimeEvidence evidence;
    evidence.installation = installation;
    evidence.launchResult = launchResult;
    evidence.executable = identifyRuntimeFile(installation.executablePath);
    evidence.processes = collectObservedProcesses(installation);
    evidence.logs = collectRuntimeLogExcerpts(installation, 4, 20);
    evidence.sessionEvents = collectRuntimeSessionEvents(installation, 8, 50);
    return evidence;
  }

  std::string makeRuntimeEvidenceReport(const RuntimeEvidence& evidence)
  {
    std::ostringstream output;
    output << "# Generated StarCraft API runtime evidence report.\n";
    output << "# This report is diagnostic evidence only. It does not claim BWAPI parity.\n";
    writeEvidenceField(output, "evidence.schema", "starcraft-api.runtime-evidence.v1");

    writeEvidenceField(output, "install.found", evidence.installation.found);
    writeEvidenceField(output, "platform", toString(evidence.installation.platform));
    writeEvidenceField(output, "product", toString(evidence.installation.product));
    writeEvidenceField(output, "version", evidence.installation.version);
    writeEvidenceField(output, "install.root", evidence.installation.installRoot);
    writeEvidenceField(output, "install.app_bundle", evidence.installation.appBundlePath);
    writeEvidenceField(output, "install.executable", evidence.installation.executablePath);
    writeEvidenceField(output, "install.launcher", evidence.installation.launcherPath);
    writeEvidenceField(output, "install.reason", evidence.installation.reason);

    writeEvidenceField(output, "executable.exists", evidence.executable.exists);
    writeEvidenceField(output, "executable.path", evidence.executable.path);
    writeEvidenceField(output, "executable.size", evidence.executable.size);
    writeEvidenceField(output, "executable.fnv1a64", evidence.executable.fnv1a64);
    writeEvidenceField(output, "executable.reason", evidence.executable.reason);

    writeEvidenceField(output, "runtime.launched", evidence.launchResult.launched);
    writeEvidenceField(output, "runtime.running", evidence.launchResult.running);
    writeEvidenceField(output, "runtime.process_id", evidence.launchResult.processId);
    writeEvidenceField(output, "runtime.reason", evidence.launchResult.reason);
    for (std::size_t i = 0; i < evidence.launchResult.warnings.size(); ++i)
      writeEvidenceField(output, "runtime.warning." + std::to_string(i), evidence.launchResult.warnings[i]);

    writeEvidenceField(output, "process.count", static_cast<int>(evidence.processes.size()));
    for (std::size_t i = 0; i < evidence.processes.size(); ++i)
    {
      const RuntimeObservedProcess& process = evidence.processes[i];
      const std::string prefix = "process." + std::to_string(i) + ".";
      writeEvidenceField(output, prefix + "pid", process.processId);
      writeEvidenceField(output, prefix + "ppid", process.parentProcessId);
      writeEvidenceField(output, prefix + "category", process.category);
      writeEvidenceField(output, prefix + "command", process.command);
    }

    writeEvidenceField(output, "log.count", static_cast<int>(evidence.logs.size()));
    for (std::size_t i = 0; i < evidence.logs.size(); ++i)
    {
      const RuntimeLogExcerpt& log = evidence.logs[i];
      const std::string prefix = "log." + std::to_string(i) + ".";
      writeEvidenceField(output, prefix + "path", log.path);
      writeEvidenceField(output, prefix + "reason", log.reason);
      writeEvidenceField(output, prefix + "line_count", static_cast<int>(log.lines.size()));
      for (std::size_t line = 0; line < log.lines.size(); ++line)
        writeEvidenceField(output, prefix + "line." + std::to_string(line), log.lines[line]);
    }

    writeEvidenceField(output, "session.event_count", static_cast<int>(evidence.sessionEvents.size()));
    for (std::size_t i = 0; i < evidence.sessionEvents.size(); ++i)
    {
      const RuntimeSessionEvent& event = evidence.sessionEvents[i];
      const std::string prefix = "session.event." + std::to_string(i) + ".";
      writeEvidenceField(output, prefix + "category", event.category);
      writeEvidenceField(output, prefix + "path", event.path);
      writeEvidenceField(output, prefix + "line", event.line);
    }

    return output.str();
  }

  bool writeRuntimeEvidenceReport(
    const RuntimeInstallation& installation,
    const RuntimeLaunchResult& launchResult,
    const std::string& path,
    std::string& error)
  {
    std::ofstream output(path);
    if (!output)
    {
      error = "unable to write runtime evidence report: " + path;
      return false;
    }
    output << makeRuntimeEvidenceReport(collectRuntimeEvidence(installation, launchResult));
    return true;
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
