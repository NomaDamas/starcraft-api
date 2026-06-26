#include <BWAPI/Runtime/RuntimeInstallation.h>

#include <BWAPI/Runtime/RuntimeExecutor.h>
#include <BWAPI/Runtime/RuntimeProcess.h>
#include <BWAPI/Runtime/RuntimeProcessMemory.h>
#include <BWAPI/Runtime/RuntimeResidentBridge.h>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <libproc.h>
#include <signal.h>
#include <sys/proc_info.h>
#include <sys/wait.h>
#include <unistd.h>
#else
#include <signal.h>
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

    bool readKeyValueLine(
      const std::filesystem::path& path,
      const std::string& key,
      std::string& value)
    {
      std::ifstream input(path);
      const std::string prefix = key + '=';
      std::string line;
      while (std::getline(input, line))
      {
        if (line.rfind(prefix, 0) == 0)
        {
          value = line.substr(prefix.size());
          return true;
        }
      }
      return false;
    }

    bool readyFileHasLine(const std::filesystem::path& path, const std::string& expected)
    {
      std::ifstream input(path);
      std::string line;
      while (std::getline(input, line))
      {
        if (line == expected)
          return true;
      }
      return false;
    }

    std::string normalizedPathString(const std::string& path)
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

    bool liveBridgeMatchesEnvironment(
      const RuntimeEnvironment& environment,
      const std::filesystem::path& bridgePath)
    {
      std::error_code error;
      if (!std::filesystem::is_directory(bridgePath, error) || error)
        return false;

      const std::filesystem::path readyPath = bridgePath / RuntimeExecutorBridgeReadyFile;
      if (!std::filesystem::is_regular_file(readyPath, error) || error)
        return false;
      if (!readyFileHasLine(readyPath, std::string("protocol=") + RuntimeExecutorBridgeProtocol))
        return false;

      std::string value;
      if (!readKeyValueLine(readyPath, "mode", value)
          || value != RuntimeExecutorBridgeValidatedAdapterMode)
        return false;

      if (environment.product != Product::Unknown)
      {
        if (!readKeyValueLine(readyPath, "product", value)
            || parseProduct(value) != environment.product)
          return false;
      }

      if (!environment.version.empty())
      {
        if (!readKeyValueLine(readyPath, "version", value)
            || value != environment.version)
          return false;
      }

      if (environment.processId > 0)
      {
        if (!readKeyValueLine(readyPath, "process_id", value))
          return false;
        char* end = nullptr;
        errno = 0;
        const long parsed = std::strtol(value.c_str(), &end, 10);
        if (errno != 0 || end == value.c_str() || *end != '\0'
            || parsed != environment.processId)
          return false;
      }

      if (!environment.executablePath.empty())
      {
        if (!readKeyValueLine(readyPath, "executable", value)
            || normalizedPathString(value) != normalizedPathString(environment.executablePath))
          return false;
      }

      RuntimeEnvironment bridgeEnvironment = environment;
      bridgeEnvironment.executorBridgePath = bridgePath.string();
      RuntimeResidentBridgeValidationResult resident =
        validateRuntimeResidentBridgeReadyFile(bridgeEnvironment, readyPath);
      return resident.present && resident.active && resident.valid;
    }

    std::filesystem::path normalizedBridgeCandidatePath(const std::filesystem::path& path)
    {
      std::error_code error;
      std::filesystem::path normalized = std::filesystem::weakly_canonical(path, error);
      if (error)
        normalized = std::filesystem::absolute(path, error);
      if (error)
        normalized = path;
      return normalized.lexically_normal();
    }

    std::vector<std::filesystem::path> liveBridgeCandidatePaths()
    {
      std::vector<std::filesystem::path> candidates;

      const std::string explicitDiscoveryDir =
        getenvString("STARCRAFT_API_EXECUTOR_BRIDGE_DISCOVERY_DIR");
      if (!explicitDiscoveryDir.empty())
        candidates.emplace_back(explicitDiscoveryDir);

      const std::string explicitExecutorBridge =
        getenvString("STARCRAFT_API_EXECUTOR_BRIDGE_DIR");
      if (!explicitExecutorBridge.empty())
        candidates.emplace_back(explicitExecutorBridge);

      const std::string explicitResidentBridge =
        getenvString("STARCRAFT_API_RESIDENT_BRIDGE_DIR");
      if (!explicitResidentBridge.empty())
        candidates.emplace_back(explicitResidentBridge);

      candidates.emplace_back("/tmp/starcraft-api-live-bridge");

      std::error_code error;
      const std::filesystem::path tempBridge =
        std::filesystem::temp_directory_path(error) / "starcraft-api-live-bridge";
      if (!error)
        candidates.push_back(tempBridge);

      std::vector<std::filesystem::path> unique;
      for (const std::filesystem::path& candidate : candidates)
      {
        const std::filesystem::path normalized = normalizedBridgeCandidatePath(candidate);
        if (std::find(unique.begin(), unique.end(), normalized) == unique.end())
          unique.push_back(normalized);
      }
      return unique;
    }

    std::string findMatchingLiveBridgePath(const RuntimeEnvironment& environment)
    {
      // A live bridge is bound to one concrete process. If no current
      // StarCraft PID was resolved, a stale /tmp bridge must not be adopted
      // just because product/version/executable metadata still matches.
      if (environment.processId <= 0)
        return {};

      std::vector<std::filesystem::path> matches;
      for (const std::filesystem::path& bridgePath : liveBridgeCandidatePaths())
      {
        if (liveBridgeMatchesEnvironment(environment, bridgePath))
          matches.push_back(bridgePath);
      }
      if (matches.size() == 1)
        return matches.front().string();
      return {};
    }

    std::vector<std::string> splitExtraLaunchArguments(const std::string& value)
    {
      std::vector<std::string> arguments;
      std::string current;
      char quote = '\0';
      bool escaped = false;

      for (char ch : value)
      {
        if (escaped)
        {
          current.push_back(ch);
          escaped = false;
          continue;
        }
        if (ch == '\\')
        {
          escaped = true;
          continue;
        }
        if (quote != '\0')
        {
          if (ch == quote)
            quote = '\0';
          else
            current.push_back(ch);
          continue;
        }
        if (ch == '\'' || ch == '"')
        {
          quote = ch;
          continue;
        }
        if (std::isspace(static_cast<unsigned char>(ch)) != 0)
        {
          if (!current.empty())
          {
            arguments.push_back(current);
            current.clear();
          }
          continue;
        }
        current.push_back(ch);
      }

      if (escaped)
        current.push_back('\\');
      if (!current.empty())
        arguments.push_back(current);
      return arguments;
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

    bool hasBroodWarReplayExtension(const std::filesystem::path& path)
    {
      return lowerCase(path.extension().string()) == ".rep";
    }

    bool replayPathExistsForLaunch(
      const RuntimeInstallation& installation,
      const std::filesystem::path& replayPath)
    {
      if (fileExists(replayPath))
        return true;
      if (replayPath.is_relative() && !installation.installRoot.empty())
        return fileExists(std::filesystem::path(installation.installRoot) / replayPath);
      return false;
    }

    bool validateReplayLaunchRequest(
      const RuntimeInstallation& installation,
      const std::string& replayPath,
      std::string& reason)
    {
      if (replayPath.empty())
        return true;
      if (installation.product != Product::StarCraftRemastered)
      {
        reason = "play-replay requires StarCraft Remastered";
        return false;
      }

      const std::filesystem::path replayFile(replayPath);
      if (!hasBroodWarReplayExtension(replayFile))
      {
        reason = "play-replay requires a Brood War .rep replay file: " + replayPath;
        return false;
      }
      if (!replayPathExistsForLaunch(installation, replayFile))
      {
        reason = "play-replay replay file does not exist: " + replayPath;
        return false;
      }

      return true;
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

    bool startsWith(std::string_view value, std::string_view prefix)
    {
      return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
    }

    std::string normalizePathSeparators(std::string value)
    {
      std::replace(value.begin(), value.end(), '\\', '/');
      return value;
    }

    bool trimmedCommandStartsWithPath(std::string_view trimmed, const std::string& path)
    {
      if (path.empty())
        return false;

      if (startsWith(trimmed, path))
        return trimmed.size() == path.size() || std::isspace(static_cast<unsigned char>(trimmed[path.size()])) != 0;

      for (char quote : { '"', '\'' })
      {
        const std::string quotedPath = std::string(1, quote) + path + quote;
        if (startsWith(trimmed, quotedPath))
        {
          return trimmed.size() == quotedPath.size()
            || std::isspace(static_cast<unsigned char>(trimmed[quotedPath.size()])) != 0;
        }
      }

      return false;
    }

    bool commandStartsWithPath(const std::string& command, const std::string& path)
    {
      const std::size_t first = command.find_first_not_of(" \t\r\n");
      const std::string trimmed = first == std::string::npos ? std::string() : command.substr(first);
      if (trimmedCommandStartsWithPath(trimmed, path))
        return true;

      const std::string normalizedTrimmed = normalizePathSeparators(trimmed);
      const std::string normalizedPath = normalizePathSeparators(path);
      if (normalizedTrimmed == trimmed && normalizedPath == path)
        return false;
      return trimmedCommandStartsWithPath(normalizedTrimmed, normalizedPath);
    }

    std::string trimLeft(std::string value)
    {
      const std::size_t first = value.find_first_not_of(" \t\r\n");
      if (first == std::string::npos)
        return {};
      return value.substr(first);
    }

    bool isDigit(char ch)
    {
      return std::isdigit(static_cast<unsigned char>(ch)) != 0;
    }

    bool parseFixedInt(
      const std::string& value,
      std::size_t offset,
      std::size_t count,
      int& parsed)
    {
      if (offset + count > value.size())
        return false;

      int result = 0;
      for (std::size_t i = 0; i < count; ++i)
      {
        const char ch = value[offset + i];
        if (!isDigit(ch))
          return false;
        result = result * 10 + (ch - '0');
      }

      parsed = result;
      return true;
    }

    std::int64_t daysFromCivil(int year, unsigned month, unsigned day)
    {
      year -= month <= 2 ? 1 : 0;
      const int era = (year >= 0 ? year : year - 399) / 400;
      const unsigned yearOfEra = static_cast<unsigned>(year - era * 400);
      const unsigned adjustedMonth = month > 2 ? month - 3 : month + 9;
      const unsigned dayOfYear = (153 * adjustedMonth + 2) / 5 + day - 1;
      const unsigned dayOfEra =
        yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
      return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(dayOfEra);
    }

    bool parseLogTimestampMilliseconds(
      const std::string& line,
      std::string& timestamp,
      std::int64_t& milliseconds)
    {
      for (std::size_t offset = 0; offset + 19 <= line.size(); ++offset)
      {
        if (!isDigit(line[offset])
            || !isDigit(line[offset + 1])
            || !isDigit(line[offset + 2])
            || !isDigit(line[offset + 3])
            || line[offset + 4] != '-'
            || line[offset + 7] != '-'
            || line[offset + 10] != ' '
            || line[offset + 13] != ':'
            || line[offset + 16] != ':')
          continue;

        int year = 0;
        int month = 0;
        int day = 0;
        int hour = 0;
        int minute = 0;
        int second = 0;
        if (!parseFixedInt(line, offset, 4, year)
            || !parseFixedInt(line, offset + 5, 2, month)
            || !parseFixedInt(line, offset + 8, 2, day)
            || !parseFixedInt(line, offset + 11, 2, hour)
            || !parseFixedInt(line, offset + 14, 2, minute)
            || !parseFixedInt(line, offset + 17, 2, second))
          continue;

        if (month < 1 || month > 12 || day < 1 || day > 31
            || hour < 0 || hour > 23 || minute < 0 || minute > 59
            || second < 0 || second > 60)
          continue;

        int fractionalMilliseconds = 0;
        std::size_t timestampEnd = offset + 19;
        if (timestampEnd < line.size() && line[timestampEnd] == '.')
        {
          ++timestampEnd;
          int scale = 100;
          while (timestampEnd < line.size() && isDigit(line[timestampEnd]))
          {
            if (scale > 0)
            {
              fractionalMilliseconds += (line[timestampEnd] - '0') * scale;
              scale /= 10;
            }
            ++timestampEnd;
          }
        }

        const std::int64_t days =
          daysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
        milliseconds =
          (((days * 24 + hour) * 60 + minute) * 60 + second) * 1000
          + fractionalMilliseconds;
        timestamp = line.substr(offset, timestampEnd - offset);
        return true;
      }

      return false;
    }

    int parseIntegerAfterMarker(const std::string& line, const std::string& marker)
    {
      const std::size_t markerOffset = line.find(marker);
      if (markerOffset == std::string::npos)
        return 0;

      std::size_t offset = markerOffset + marker.size();
      while (offset < line.size() && line[offset] == ' ')
        ++offset;

      int value = 0;
      bool hasDigit = false;
      while (offset < line.size() && isDigit(line[offset]))
      {
        hasDigit = true;
        value = value * 10 + (line[offset] - '0');
        ++offset;
      }

      return hasDigit ? value : 0;
    }

    bool isBattleNetSupportCodeChar(char ch)
    {
      return std::isupper(static_cast<unsigned char>(ch)) != 0
        || std::isdigit(static_cast<unsigned char>(ch)) != 0;
    }

    bool isBattleNetSupportErrorLine(const std::string& line)
    {
      return line.find("/client/error/") != std::string::npos
        || line.find("battle.net/support") != std::string::npos
        || line.find("support.blizzard.com") != std::string::npos;
    }

    std::string extractBattleNetSupportCode(const std::string& line)
    {
      if (!isBattleNetSupportErrorLine(line))
        return {};

      std::size_t offset = line.find("BLZ");
      while (offset != std::string::npos)
      {
        std::size_t end = offset;
        while (end < line.size() && isBattleNetSupportCodeChar(line[end]))
          ++end;

        if (end - offset >= 8)
          return line.substr(offset, end - offset);

        offset = line.find("BLZ", offset + 3);
      }

      return {};
    }

    std::string extractSupportUrl(const std::string& line, const std::string& supportCode)
    {
      std::size_t codeOffset = supportCode.empty()
        ? std::string::npos
        : line.find(supportCode);
      if (codeOffset == std::string::npos)
        codeOffset = 0;

      std::size_t urlOffset = line.rfind("https://", codeOffset);
      if (urlOffset == std::string::npos)
        urlOffset = line.rfind("http://", codeOffset);
      if (urlOffset == std::string::npos)
      {
        urlOffset = line.find("https://");
        if (urlOffset == std::string::npos)
          urlOffset = line.find("http://");
      }
      if (urlOffset == std::string::npos)
        return {};

      std::size_t end = urlOffset;
      while (end < line.size() && !std::isspace(static_cast<unsigned char>(line[end])))
        ++end;

      std::string url = line.substr(urlOffset, end - urlOffset);
      while (!url.empty() && (url.back() == ')' || url.back() == ']' || url.back() == ';' || url.back() == ','))
        url.pop_back();
      return url;
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
      if (commandStartsWithPath(command, installation.executablePath)
          || commandStartsWithPath(command, "StarCraft.app/Contents/MacOS/StarCraft"))
      {
        return "starcraft-game";
      }

      const bool isBattleNetMain = lineContainsAny(command, {
        "Battle.net.app/Contents/MacOS/Battle.net",
        "Battle.net.exe"
      });
      const bool isBattleNetSupport = lineContainsAny(command, {
        "Battle.net Helper",
        "Agent.app/Contents/MacOS/Agent"
      });
      const bool isBattleNet = isBattleNetMain || isBattleNetSupport;

      if (isBattleNet && command.find("--game=s1") != std::string::npos)
        return "battle.net-handoff";

      if (isBattleNetMain)
        return "battle.net-main";

      if (isBattleNetSupport)
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

    bool parseObservedProcessLine(
      const RuntimeInstallation& installation,
      const std::string& line,
      RuntimeObservedProcess& process)
    {
      std::istringstream parser(line);
      if (!(parser >> process.processId >> process.parentProcessId))
        return false;
      if (process.processId <= 0)
        return false;

      std::getline(parser, process.command);
      process.command = trimLeft(process.command);
      process.category = categorizeProcessCommand(installation, process.command);
      return isRelevantObservedProcess(process);
    }

    std::vector<RuntimeObservedProcess> collectSnapshotObservedProcesses(
      const RuntimeInstallation& installation,
      const std::string& snapshotPath)
    {
      std::vector<RuntimeObservedProcess> processes;
      std::ifstream input(snapshotPath);
      if (!input)
        return processes;

      std::string line;
      while (std::getline(input, line))
      {
        RuntimeObservedProcess process;
        if (parseObservedProcessLine(installation, line, process))
          processes.push_back(process);
      }
      return processes;
    }

    std::vector<int> processIdsFromObservedProcesses(
      const std::vector<RuntimeObservedProcess>& processes,
      const std::vector<std::string>& categories)
    {
      std::vector<int> processIds;
      for (const RuntimeObservedProcess& process : processes)
      {
        if (lineContainsAny(process.category, categories))
          processIds.push_back(process.processId);
      }
      return processIds;
    }

#if !defined(_WIN32)
#if defined(__APPLE__)
    std::string commandLineForDarwinProcess(int processId, const std::string& executablePath)
    {
      RuntimeProcessCommandLineResult commandLine = inspectRuntimeProcessCommandLine(processId);
      if (!commandLine.inspected || commandLine.arguments.empty())
        return executablePath;

      std::ostringstream command;
      for (std::size_t index = 0; index < commandLine.arguments.size(); ++index)
      {
        if (index > 0)
          command << ' ';
        command << commandLine.arguments[index];
      }
      return command.str().empty() ? executablePath : command.str();
    }

    std::vector<RuntimeObservedProcess> collectDarwinObservedProcesses(
      const RuntimeInstallation& installation)
    {
      std::vector<RuntimeObservedProcess> processes;
      int byteCount = proc_listpids(PROC_ALL_PIDS, 0, nullptr, 0);
      if (byteCount <= 0)
        return processes;

      std::vector<pid_t> processIds(static_cast<std::size_t>(byteCount) / sizeof(pid_t) + 1024);
      byteCount = proc_listpids(
        PROC_ALL_PIDS,
        0,
        processIds.data(),
        static_cast<int>(processIds.size() * sizeof(pid_t)));
      if (byteCount <= 0)
        return processes;

      const std::size_t count = std::min(
        processIds.size(),
        static_cast<std::size_t>(byteCount) / sizeof(pid_t));
      for (std::size_t index = 0; index < count; ++index)
      {
        const int processId = static_cast<int>(processIds[index]);
        if (processId <= 0)
          continue;

        const std::string executablePath = runtimeProcessExecutablePath(processId);
        if (executablePath.empty())
          continue;

        RuntimeObservedProcess process;
        process.processId = processId;
        process.parentProcessId = 0;
        process.command = executablePath;
        process.category = categorizeProcessCommand(installation, process.command);
        if (process.category == "battle.net-main"
            || process.category == "battle.net-support"
            || process.category.empty())
        {
          process.command = commandLineForDarwinProcess(processId, executablePath);
          process.category = categorizeProcessCommand(installation, process.command);
        }

        proc_bsdinfo info;
        std::memset(&info, 0, sizeof(info));
        const int infoBytes = proc_pidinfo(
          processId,
          PROC_PIDTBSDINFO,
          0,
          &info,
          static_cast<int>(sizeof(info)));
        if (infoBytes == static_cast<int>(sizeof(info)))
          process.parentProcessId = static_cast<int>(info.pbi_ppid);

        if (isRelevantObservedProcess(process))
          processes.push_back(process);
      }
      return processes;
    }
#endif

    std::vector<RuntimeObservedProcess> collectPosixObservedProcesses(const RuntimeInstallation& installation)
    {
      const std::string snapshotPath = getenvString("STARCRAFT_API_PROCESS_SNAPSHOT");
      if (!snapshotPath.empty())
        return collectSnapshotObservedProcesses(installation, snapshotPath);

#if defined(__APPLE__)
      return collectDarwinObservedProcesses(installation);
#else
      std::vector<RuntimeObservedProcess> processes;
      FILE* pipe = popen("ps -axo pid=,ppid=,command= 2>/dev/null", "r");
      if (pipe == nullptr)
        return processes;

      char buffer[4096];
      while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr)
      {
        RuntimeObservedProcess process;
        if (parseObservedProcessLine(installation, buffer, process))
          processes.push_back(process);
      }

      pclose(pipe);
      return processes;
#endif
    }

    std::vector<int> processIdsForCategory(
      const RuntimeInstallation& installation,
      const std::vector<std::string>& categories)
    {
      return processIdsFromObservedProcesses(collectPosixObservedProcesses(installation), categories);
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

    bool runtimeProcessHasActiveResidentBridge(
      const RuntimeInstallation& installation,
      int processId,
      std::string& reason)
    {
      RuntimeEnvironment environment;
      environment.platform = installation.platform;
      environment.product = installation.product;
      environment.version = installation.version;
      environment.processId = processId;
      environment.executablePath = installation.executablePath;

      const std::string bridgePath = findMatchingLiveBridgePath(environment);
      if (bridgePath.empty())
        return false;

      environment.executorBridgePath = bridgePath;
      const std::filesystem::path readyPath =
        std::filesystem::path(bridgePath) / RuntimeExecutorBridgeReadyFile;
      RuntimeResidentBridgeValidationResult resident =
        validateRuntimeResidentBridgeReadyFile(environment, readyPath);
      if (resident.valid && resident.active)
        return true;

      if (!resident.errors.empty())
      {
        std::ostringstream message;
        message << "matching resident bridge is present but invalid";
        for (const std::string& error : resident.errors)
          message << "; " << error;
        reason = message.str();
      }
      return false;
    }

    bool runtimeProcessAppearsInitialized(
      const RuntimeInstallation& installation,
      int processId,
      std::string& reason)
    {
      std::string residentReason;
      if (runtimeProcessHasActiveResidentBridge(installation, processId, residentReason))
        return true;
      if (!residentReason.empty())
        reason = residentReason;

      RuntimeMemoryRegionListResult regions = listProcessMemoryRegions(processId);
      if (!regions.success)
      {
        if (reason.empty())
          reason = "runtime memory map inspection is unavailable: " + regions.reason;
        else
          reason += "; runtime memory map inspection is unavailable: " + regions.reason;
        return !getenvString("STARCRAFT_API_PROCESS_SNAPSHOT").empty();
      }

      std::size_t readableRegions = 0;
      std::size_t readableBytes = 0;
      for (const RuntimeMemoryRegion& region : regions.regions)
      {
        if (!region.readable || region.executable)
          continue;
        ++readableRegions;
        readableBytes += region.size;
      }

      constexpr std::size_t minInitializedReadableRegions = 4;
      constexpr std::size_t minInitializedReadableBytes = 4 * 1024 * 1024;
      if (readableRegions < minInitializedReadableRegions
          || readableBytes < minInitializedReadableBytes)
      {
        std::ostringstream message;
        message << "runtime process " << processId
                << " has only " << readableRegions
                << " non-executable readable region(s) and "
                << readableBytes
                << " readable byte(s); treating it as launched-suspended/not initialized";
        reason = message.str();
        return false;
      }

      return true;
    }

    int findStableProcessId(
      const RuntimeInstallation& installation,
      int stableMilliseconds,
      std::string* rejectionReason = nullptr)
    {
      const std::vector<int> firstPass = findRuntimeProcessIds(installation);
      if (firstPass.empty())
        return 0;

      const int processId = firstPass.front();
      if (stableMilliseconds > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(stableMilliseconds));

      const std::vector<int> secondPass = findRuntimeProcessIds(installation);
      if (containsProcessId(secondPass, processId))
      {
        std::string initializedReason;
        if (runtimeProcessAppearsInitialized(installation, processId, initializedReason))
          return processId;
        if (rejectionReason != nullptr)
          *rejectionReason = initializedReason;
      }
      return 0;
    }

#if defined(_WIN32)
    std::vector<int> findWindowsProcesses(const RuntimeInstallation& installation)
    {
      const std::string snapshotPath = getenvString("STARCRAFT_API_PROCESS_SNAPSHOT");
      if (snapshotPath.empty())
        return {};

      return processIdsFromObservedProcesses(
        collectSnapshotObservedProcesses(installation, snapshotPath),
        { "starcraft-game" });
    }

    std::vector<int> findWindowsBattleNetHandoffProcesses(const RuntimeInstallation& installation)
    {
      const std::string snapshotPath = getenvString("STARCRAFT_API_PROCESS_SNAPSHOT");
      if (snapshotPath.empty())
        return {};

      return processIdsFromObservedProcesses(
        collectSnapshotObservedProcesses(installation, snapshotPath),
        { "battle.net-handoff" });
    }
#endif

    std::vector<RuntimeObservedProcess> collectObservedProcesses(const RuntimeInstallation& installation)
    {
      const std::string snapshotPath = getenvString("STARCRAFT_API_PROCESS_SNAPSHOT");
      if (!snapshotPath.empty())
        return collectSnapshotObservedProcesses(installation, snapshotPath);

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

    std::filesystem::path appBundleForExecutablePath(const std::string& executablePath)
    {
      std::filesystem::path cursor(executablePath);
      while (!cursor.empty())
      {
        if (cursor.extension() == ".app")
          return cursor;

        const std::filesystem::path parent = cursor.parent_path();
        if (parent == cursor)
          break;
        cursor = parent;
      }

      return {};
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

    std::vector<RuntimeSupportError> collectRuntimeSupportErrors(
      const RuntimeInstallation& installation,
      std::size_t maxFiles,
      std::size_t maxErrors)
    {
      struct TimestampedSupportError
      {
        RuntimeSupportError error;
        std::int64_t milliseconds = 0;
        std::size_t index = 0;
        bool hasTimestamp = false;
      };

      std::vector<TimestampedSupportError> collected;
      std::size_t inspectedFiles = 0;
      std::size_t collectedIndex = 0;
      bool allErrorsTimestamped = true;
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
          const std::string supportCode = extractBattleNetSupportCode(sanitized);
          if (supportCode.empty())
            continue;

          TimestampedSupportError supportError;
          supportError.error.path = pathString(file.path);
          supportError.error.code = supportCode;
          supportError.error.url = extractSupportUrl(sanitized, supportCode);
          supportError.error.line = sanitized;
          supportError.index = collectedIndex++;

          std::string timestamp;
          supportError.hasTimestamp = parseLogTimestampMilliseconds(
            sanitized,
            timestamp,
            supportError.milliseconds);
          if (!supportError.hasTimestamp)
            allErrorsTimestamped = false;

          collected.push_back(std::move(supportError));
        }
      }

      if (allErrorsTimestamped)
      {
        std::stable_sort(
          collected.begin(),
          collected.end(),
          [](const TimestampedSupportError& left, const TimestampedSupportError& right) {
            if (left.milliseconds != right.milliseconds)
              return left.milliseconds < right.milliseconds;
            return left.index < right.index;
          });
      }
      else
      {
        std::stable_sort(
          collected.begin(),
          collected.end(),
          [](const TimestampedSupportError& left, const TimestampedSupportError& right) {
            return left.index < right.index;
          });
      }

      while (collected.size() > maxErrors)
        collected.erase(collected.begin());

      std::vector<RuntimeSupportError> errors;
      errors.reserve(collected.size());
      for (const TimestampedSupportError& supportError : collected)
        errors.push_back(supportError.error);
      return errors;
    }

    const RuntimeSupportError* latestRuntimeSupportError(const std::vector<RuntimeSupportError>& errors)
    {
      if (errors.empty())
        return nullptr;
      return &errors.back();
    }

    std::string categorizeSessionLogLine(
      const RuntimeInstallation& installation,
      const std::string& line)
    {
      const bool mentionsStarCraft = line.find("uid=s1") != std::string::npos
        || line.find("agentUid=s1") != std::string::npos
        || line.find("InstallState (s1)") != std::string::npos
        || line.find("Game is running: s1") != std::string::npos
        || line.find("Game is no longer running: s1") != std::string::npos
        || lineContainsAny(line, {
          "LaunchBinary: uid=s1",
          installation.appBundlePath,
          installation.executablePath
        });
      if (!mentionsStarCraft)
        return {};

      if (line.find("Launched ") != std::string::npos
          && line.find("pid:") != std::string::npos
          && lineContainsAny(line, {
            installation.appBundlePath,
            installation.executablePath,
            installation.installRoot
          }))
        return "starcraft-launch-process";

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
      struct TimestampedSessionEvent
      {
        RuntimeSessionEvent event;
        std::int64_t milliseconds = 0;
        std::size_t index = 0;
        bool hasTimestamp = false;
      };

      std::vector<TimestampedSessionEvent> collectedEvents;
      std::size_t inspectedFiles = 0;
      std::size_t collectedIndex = 0;
      bool allEventsTimestamped = true;
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
          const std::string category = categorizeSessionLogLine(installation, sanitized);
          if (category.empty())
            continue;

          TimestampedSessionEvent event;
          event.event.path = pathString(file.path);
          event.event.category = category;
          event.event.line = sanitized;
          event.index = collectedIndex++;
          std::string timestamp;
          event.hasTimestamp = parseLogTimestampMilliseconds(
            sanitized,
            timestamp,
            event.milliseconds);
          if (!event.hasTimestamp)
            allEventsTimestamped = false;
          collectedEvents.push_back(event);
        }
      }

      if (collectedEvents.size() > maxEvents)
      {
        if (allEventsTimestamped)
        {
          std::stable_sort(
            collectedEvents.begin(),
            collectedEvents.end(),
            [](const TimestampedSessionEvent& left, const TimestampedSessionEvent& right) {
              if (left.milliseconds != right.milliseconds)
                return left.milliseconds > right.milliseconds;
              return left.index > right.index;
            });
          collectedEvents.resize(maxEvents);
        }
        else
        {
          collectedEvents.resize(maxEvents);
        }
      }

      if (allEventsTimestamped)
      {
        std::stable_sort(
          collectedEvents.begin(),
          collectedEvents.end(),
          [](const TimestampedSessionEvent& left, const TimestampedSessionEvent& right) {
            if (left.milliseconds != right.milliseconds)
              return left.milliseconds < right.milliseconds;
            return left.index < right.index;
          });
      }
      else
      {
        std::stable_sort(
          collectedEvents.begin(),
          collectedEvents.end(),
          [](const TimestampedSessionEvent& left, const TimestampedSessionEvent& right) {
            return left.index < right.index;
          });
      }

      std::vector<RuntimeSessionEvent> events;
      events.reserve(collectedEvents.size());
      for (const TimestampedSessionEvent& event : collectedEvents)
        events.push_back(event.event);
      return events;
    }

    void updateSessionDurationRange(RuntimeSessionSummary& summary, int durationMilliseconds)
    {
      if (durationMilliseconds < 0)
        return;

      if (summary.shortestDurationMilliseconds < 0
          || durationMilliseconds < summary.shortestDurationMilliseconds)
        summary.shortestDurationMilliseconds = durationMilliseconds;

      if (summary.longestDurationMilliseconds < 0
          || durationMilliseconds > summary.longestDurationMilliseconds)
        summary.longestDurationMilliseconds = durationMilliseconds;

      summary.latestTransitionDurationMilliseconds = durationMilliseconds;
    }

    int timestampDeltaMilliseconds(const std::string& earlierTimestamp, const std::string& laterTimestamp)
    {
      std::string ignoredTimestamp;
      std::int64_t earlierMilliseconds = 0;
      std::int64_t laterMilliseconds = 0;
      if (!parseLogTimestampMilliseconds(earlierTimestamp, ignoredTimestamp, earlierMilliseconds)
          || !parseLogTimestampMilliseconds(laterTimestamp, ignoredTimestamp, laterMilliseconds))
        return -1;

      std::int64_t delta = laterMilliseconds - earlierMilliseconds;
      if (delta < 0)
        delta = 0;

      const int maxInt = (std::numeric_limits<int>::max)();
      return delta > maxInt ? maxInt : static_cast<int>(delta);
    }

    RuntimeSessionSummary summarizeRuntimeSessionEvents(
      const std::vector<RuntimeSessionEvent>& events)
    {
      struct TimestampedEvent
      {
        RuntimeSessionEvent event;
        std::string timestamp;
        std::int64_t milliseconds = 0;
        std::size_t index = 0;
        bool hasTimestamp = false;
      };

      std::vector<TimestampedEvent> orderedEvents;
      orderedEvents.reserve(events.size());
      bool allEventsTimestamped = true;
      for (std::size_t i = 0; i < events.size(); ++i)
      {
        TimestampedEvent timestamped;
        timestamped.event = events[i];
        timestamped.index = i;
        timestamped.hasTimestamp = parseLogTimestampMilliseconds(
          events[i].line,
          timestamped.timestamp,
          timestamped.milliseconds);
        if (!timestamped.hasTimestamp)
          allEventsTimestamped = false;
        orderedEvents.push_back(timestamped);
      }

      if (allEventsTimestamped)
      {
        std::stable_sort(
          orderedEvents.begin(),
          orderedEvents.end(),
          [](const TimestampedEvent& left, const TimestampedEvent& right) {
            if (left.milliseconds != right.milliseconds)
              return left.milliseconds < right.milliseconds;
            return left.index < right.index;
          });
      }

      RuntimeSessionSummary summary;
      RuntimeSessionEvent openStart;
      std::string openStartTimestamp;
      std::int64_t openStartMilliseconds = 0;
      bool hasOpenStart = false;
      bool hasOpenStartTimestamp = false;

      for (const TimestampedEvent& timestamped : orderedEvents)
      {
        const RuntimeSessionEvent& event = timestamped.event;
        if (timestamped.hasTimestamp)
          summary.latestObservedTimestamp = timestamped.timestamp;

        if (event.category == "starcraft-session-started")
        {
          ++summary.startedEventCount;
          summary.latestState = "running";
          summary.latestReason = "latest StarCraft s1 event reports a running session";

          if (!hasOpenStart)
          {
            openStart = event;
            hasOpenStart = true;
            openStartTimestamp = timestamped.timestamp;
            openStartMilliseconds = timestamped.milliseconds;
            hasOpenStartTimestamp = timestamped.hasTimestamp;
          }
          continue;
        }

        if (event.category == "starcraft-session-ended")
        {
          ++summary.endedEventCount;
          summary.latestState = "stopped";
          summary.latestReason = "latest StarCraft s1 event reports a stopped session";

          if (hasOpenStart)
          {
            RuntimeSessionTransition transition;
            transition.complete = true;
            transition.startPath = openStart.path;
            transition.endPath = event.path;
            transition.startLine = openStart.line;
            transition.endLine = event.line;
            transition.startTimestamp = openStartTimestamp;

            transition.endTimestamp = timestamped.timestamp;

            if (hasOpenStartTimestamp && timestamped.hasTimestamp)
            {
              std::int64_t duration = timestamped.milliseconds - openStartMilliseconds;
              if (duration < 0)
                duration = 0;

              const int maxInt = (std::numeric_limits<int>::max)();
              transition.durationMilliseconds = duration > maxInt
                ? maxInt
                : static_cast<int>(duration);
              transition.reason =
                "StarCraft s1 session ended after "
                + std::to_string(transition.durationMilliseconds)
                + " ms";
              updateSessionDurationRange(summary, transition.durationMilliseconds);
              summary.latestTransitionStartTimestamp = transition.startTimestamp;
              summary.latestTransitionEndTimestamp = transition.endTimestamp;
            }
            else
            {
              transition.reason = "StarCraft s1 session ended; timestamp unavailable";
            }

            ++summary.completeTransitionCount;
            summary.transitions.push_back(transition);
            hasOpenStart = false;
            hasOpenStartTimestamp = false;
          }
          continue;
        }

        if (event.category == "starcraft-session-preexisting")
        {
          ++summary.preexistingEventCount;
          summary.latestState = "preexisting";
          summary.latestReason = "Battle.net reported a pre-existing StarCraft s1 session";
          continue;
        }

        if (event.category == "starcraft-install-state")
        {
          ++summary.installStateEventCount;
          if (lineContainsAny(event.line, { "gameRunning=1", "isGameProcessRunning=1" }))
          {
            summary.latestState = "running";
            summary.latestReason = "latest StarCraft s1 install state reports a running game process";
          }
          else if (lineContainsAny(event.line, { "gameRunning=0", "isGameProcessRunning=0" }))
          {
            summary.latestState = "stopped";
            summary.latestReason = "latest StarCraft s1 install state reports no running game process";
          }
          else
          {
            summary.latestState = "handoff";
            summary.latestReason = "latest StarCraft s1 install state is a Battle.net handoff/update, not a running game session";
          }
          continue;
        }

        if (event.category == "starcraft-launch-process")
        {
          ++summary.launchProcessEventCount;
          const int processId = parseIntegerAfterMarker(event.line, "pid:");
          if (processId > 0)
            summary.latestLaunchProcessId = processId;
          summary.latestState = "launch-process";
          summary.latestReason = "latest StarCraft s1 event reports a launch process, but no running session is confirmed";
          continue;
        }

        ++summary.relatedEventCount;
        summary.latestState = "handoff";
        summary.latestReason = "latest StarCraft s1 event is a Battle.net handoff/update, not a running game session";
      }

      if (hasOpenStart)
      {
        RuntimeSessionTransition transition;
        transition.complete = false;
        transition.startPath = openStart.path;
        transition.startLine = openStart.line;
        transition.startTimestamp = openStartTimestamp;
        transition.reason = "StarCraft s1 session start has no matching stop event in collected logs";
        ++summary.incompleteTransitionCount;
        summary.transitions.push_back(transition);
        summary.latestState = "running";
        summary.latestReason = transition.reason;
      }

      if (summary.latestState.empty())
      {
        summary.latestState = "unknown";
        summary.latestReason = "no StarCraft s1 session start/stop events were collected";
      }

      return summary;
    }

    int countObservedCategory(
      const std::vector<RuntimeObservedProcess>& processes,
      const std::string& category)
    {
      return static_cast<int>(std::count_if(
        processes.begin(),
        processes.end(),
        [&](const RuntimeObservedProcess& process)
        {
          return process.category == category;
        }));
    }

    void addDiagnosisBlocker(RuntimeLaunchDiagnosis& diagnosis, std::string blocker)
    {
      diagnosis.blockers.push_back(std::move(blocker));
    }

    void addSupportErrorBlocker(RuntimeLaunchDiagnosis& diagnosis)
    {
      if (diagnosis.battleNetSupportCode.empty())
        return;

      std::string blocker =
        "Battle.net reported support error " + diagnosis.battleNetSupportCode;
      if (!diagnosis.battleNetSupportUrl.empty())
        blocker += ": " + diagnosis.battleNetSupportUrl;
      addDiagnosisBlocker(diagnosis, std::move(blocker));
    }

    bool latestObservedEventCompletesTransition(const RuntimeSessionSummary& summary)
    {
      if (summary.latestObservedTimestamp.empty())
        return false;

      return std::any_of(
        summary.transitions.begin(),
        summary.transitions.end(),
        [&](const RuntimeSessionTransition& transition)
        {
          return transition.complete
            && !transition.endTimestamp.empty()
            && transition.endTimestamp == summary.latestObservedTimestamp;
        });
    }

    int latestTransitionAgeMilliseconds(const RuntimeSessionSummary& summary)
    {
      if (summary.latestTransitionEndTimestamp.empty() || summary.latestObservedTimestamp.empty())
        return -1;

      return timestampDeltaMilliseconds(
        summary.latestTransitionEndTimestamp,
        summary.latestObservedTimestamp);
    }

    RuntimeLaunchDiagnosis diagnoseRuntimeEvidence(const RuntimeEvidence& evidence)
    {
      constexpr int ShortLivedSessionThresholdMilliseconds = 15000;

      RuntimeLaunchDiagnosis diagnosis;
      diagnosis.gameProcessCount = countObservedCategory(evidence.processes, "starcraft-game");
      diagnosis.battleNetMainCount = countObservedCategory(evidence.processes, "battle.net-main");
      diagnosis.battleNetHandoffCount = countObservedCategory(evidence.processes, "battle.net-handoff");
      diagnosis.battleNetSupportCount = countObservedCategory(evidence.processes, "battle.net-support");
      diagnosis.gameProcessVisible = diagnosis.gameProcessCount > 0;
      diagnosis.battleNetMainVisible = diagnosis.battleNetMainCount > 0;
      diagnosis.battleNetHandoffVisible = diagnosis.battleNetHandoffCount > 0;
      diagnosis.battleNetSupportVisible = diagnosis.battleNetSupportCount > 0;
      if (const RuntimeSupportError* supportError = latestRuntimeSupportError(evidence.supportErrors))
      {
        diagnosis.battleNetSupportCode = supportError->code;
        diagnosis.battleNetSupportUrl = supportError->url;
        diagnosis.battleNetSupportLine = supportError->line;
      }
      diagnosis.multipleBattleNetMainVisible = diagnosis.battleNetMainCount > 1;
      diagnosis.multipleBattleNetHandoffsVisible = diagnosis.battleNetHandoffCount > 1;
      const bool latestCompleteTransitionIsShortLived =
        evidence.sessionSummary.latestTransitionDurationMilliseconds >= 0
        && evidence.sessionSummary.latestTransitionDurationMilliseconds <= ShortLivedSessionThresholdMilliseconds;
      const int latestTransitionAge = latestTransitionAgeMilliseconds(evidence.sessionSummary);
      diagnosis.shortLivedSessionObserved =
        latestCompleteTransitionIsShortLived
        && (latestObservedEventCompletesTransition(evidence.sessionSummary)
            || (latestTransitionAge >= 0 && latestTransitionAge <= ShortLivedSessionThresholdMilliseconds));
      if (diagnosis.shortLivedSessionObserved)
        diagnosis.shortLivedSessionAgeMilliseconds = latestTransitionAge;
      diagnosis.staleHandoffSuspected =
        diagnosis.battleNetHandoffVisible
        && !diagnosis.gameProcessVisible
        && !evidence.launchResult.running;
      diagnosis.readyForAttach =
        evidence.installation.found
        && evidence.executable.exists
        && evidence.launchResult.running
        && evidence.launchResult.processId > 0
        && diagnosis.gameProcessVisible;

      if (!evidence.installation.found)
      {
        diagnosis.status = "blocked-installation-not-found";
        addDiagnosisBlocker(diagnosis, evidence.installation.reason.empty()
          ? "StarCraft Remastered installation is not configured"
          : evidence.installation.reason);
        return diagnosis;
      }

      if (!evidence.executable.exists)
      {
        diagnosis.status = "blocked-executable-missing";
        addDiagnosisBlocker(diagnosis, evidence.executable.reason.empty()
          ? "StarCraft executable is missing"
          : evidence.executable.reason);
        return diagnosis;
      }

      if (diagnosis.readyForAttach)
      {
        diagnosis.status = "runtime-process-visible";
        return diagnosis;
      }

      if (diagnosis.battleNetHandoffVisible && !diagnosis.gameProcessVisible)
      {
        const bool supportErrorObserved = !diagnosis.battleNetSupportCode.empty();
        if (diagnosis.multipleBattleNetHandoffsVisible)
        {
          if (supportErrorObserved)
          {
            diagnosis.status = diagnosis.shortLivedSessionObserved
              ? "blocked-multiple-battlenet-handoffs-short-lived-session-support-error"
              : "blocked-multiple-battlenet-handoffs-support-error";
          }
          else
          {
            diagnosis.status = diagnosis.shortLivedSessionObserved
              ? "blocked-multiple-battlenet-handoffs-short-lived-session"
              : "blocked-multiple-battlenet-handoffs-without-game";
          }
          addDiagnosisBlocker(
            diagnosis,
            "Multiple Battle.net StarCraft handoff processes are visible: "
              + std::to_string(diagnosis.battleNetHandoffCount));
        }
        else
        {
          if (supportErrorObserved)
          {
            diagnosis.status = diagnosis.shortLivedSessionObserved
              ? "blocked-battlenet-handoff-short-lived-session-support-error"
              : "blocked-battlenet-handoff-support-error";
          }
          else
          {
            diagnosis.status = diagnosis.shortLivedSessionObserved
              ? "blocked-battlenet-handoff-short-lived-session"
              : "blocked-battlenet-handoff-without-game";
          }
        }
        addDiagnosisBlocker(
          diagnosis,
          "Battle.net StarCraft handoff is visible, but the StarCraft game executable is not visible");
        if (diagnosis.shortLivedSessionObserved)
        {
          addDiagnosisBlocker(
            diagnosis,
            "Battle.net logs show the latest StarCraft session stopped after "
              + std::to_string(evidence.sessionSummary.latestTransitionDurationMilliseconds)
              + " ms");
        }
        addSupportErrorBlocker(diagnosis);
        return diagnosis;
      }

      if (diagnosis.multipleBattleNetMainVisible && !diagnosis.gameProcessVisible)
      {
        diagnosis.status = diagnosis.battleNetSupportCode.empty()
          ? "blocked-multiple-battlenet-main-processes-no-game"
          : "blocked-multiple-battlenet-main-processes-support-error-no-game";
        addDiagnosisBlocker(
          diagnosis,
          "Multiple Battle.net main processes are visible: " + std::to_string(diagnosis.battleNetMainCount));
        addDiagnosisBlocker(diagnosis, "StarCraft game executable is not visible");
        addSupportErrorBlocker(diagnosis);
        return diagnosis;
      }

      if (!diagnosis.gameProcessVisible)
      {
        if (!diagnosis.battleNetSupportCode.empty())
        {
          diagnosis.status = diagnosis.shortLivedSessionObserved
            ? "blocked-short-lived-session-support-error-no-game-process"
            : "blocked-battlenet-support-error-no-game-process";
        }
        else
        {
          diagnosis.status = diagnosis.shortLivedSessionObserved
            ? "blocked-short-lived-session-no-game-process"
            : "blocked-no-game-process";
        }
        addDiagnosisBlocker(diagnosis, "No stable StarCraft game executable process is visible");
        if (diagnosis.shortLivedSessionObserved)
        {
          addDiagnosisBlocker(
            diagnosis,
            "Battle.net logs show the latest StarCraft session stopped after "
              + std::to_string(evidence.sessionSummary.latestTransitionDurationMilliseconds)
              + " ms");
        }
        addSupportErrorBlocker(diagnosis);
        return diagnosis;
      }

      diagnosis.status = "blocked-game-process-not-selected";
      addDiagnosisBlocker(
        diagnosis,
        "A StarCraft process is visible, but launch/attach did not select a stable runtime process id");
      return diagnosis;
    }

#if !defined(_WIN32)
    std::string joinArgumentsForMessage(const std::vector<std::string>& arguments)
    {
      std::ostringstream message;
      for (std::size_t i = 0; i < arguments.size(); ++i)
      {
        if (i > 0)
          message << ' ';
        message << arguments[i];
      }
      return message.str();
    }

    bool forkExecProcess(
      const std::string& executable,
      const std::vector<std::string>& arguments,
      const std::string& workingDirectory,
      bool allowImmediateSuccessExit,
      pid_t& processId,
      std::string& reason)
    {
      if (executable.empty() || arguments.empty())
      {
        reason = "launch target is empty";
        return false;
      }

      processId = fork();
      if (processId < 0)
      {
        reason = "fork failed for " + executable;
        return false;
      }

      if (processId == 0)
      {
        // Keep GUI/game launch targets alive after the short-lived CLI exits.
        setsid();

        if (!workingDirectory.empty())
          chdir(workingDirectory.c_str());

        std::vector<char*> argv;
        argv.reserve(arguments.size() + 1);
        for (const std::string& argument : arguments)
          argv.push_back(const_cast<char*>(argument.c_str()));
        argv.push_back(nullptr);
        execv(executable.c_str(), argv.data());
        _exit(127);
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      int status = 0;
      const pid_t exited = waitpid(processId, &status, WNOHANG);
      if (exited == processId)
      {
        if (allowImmediateSuccessExit && WIFEXITED(status) && WEXITSTATUS(status) == 0)
          return true;

        std::ostringstream message;
        message << "process exited immediately for " << joinArgumentsForMessage(arguments) << " with status " << status;
        reason = message.str();
        return false;
      }

      return true;
    }
#endif

    struct RuntimeLaunchTarget
    {
      std::string kind;
      std::string executable;
      std::vector<std::string> arguments;
      bool allowImmediateSuccessExit = false;
    };

    bool envValueIsFalse(std::string value)
    {
      std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char ch)
        {
          return static_cast<char>(std::tolower(ch));
        });
      return value == "0" || value == "false" || value == "off" || value == "no";
    }

    bool remasteredWindowedLaunchEnabled()
    {
      const std::string value = getenvString("STARCRAFT_API_WINDOWED");
      return value.empty() || !envValueIsFalse(value);
    }

    std::string getenvOrDefault(const char* name, const char* fallback)
    {
      const std::string value = getenvString(name);
      return value.empty() ? fallback : value;
    }

    std::vector<std::string> makeExecutableLaunchArguments(
      const RuntimeInstallation& installation,
      const std::string& replayPath)
    {
      std::vector<std::string> executableArguments = { installation.executablePath };
      if (installation.product != Product::StarCraftRemastered)
        return executableArguments;

      executableArguments.push_back("-launch");
      executableArguments.push_back("-uid");
      executableArguments.push_back("s1");
      if (remasteredWindowedLaunchEnabled())
      {
        executableArguments.push_back("-displayMode");
        executableArguments.push_back("0");
        executableArguments.push_back("-windowwidth");
        executableArguments.push_back(getenvOrDefault("STARCRAFT_API_WINDOW_WIDTH", "1024"));
        executableArguments.push_back("-windowheight");
        executableArguments.push_back(getenvOrDefault("STARCRAFT_API_WINDOW_HEIGHT", "768"));
        executableArguments.push_back("-windowx");
        executableArguments.push_back(getenvOrDefault("STARCRAFT_API_WINDOW_X", "100"));
        executableArguments.push_back("-windowy");
        executableArguments.push_back(getenvOrDefault("STARCRAFT_API_WINDOW_Y", "100"));
      }
      if (!replayPath.empty())
      {
        executableArguments.push_back("playReplay");
        executableArguments.push_back(replayPath);
      }
      for (const std::string& argument : splitExtraLaunchArguments(getenvString("STARCRAFT_API_EXTRA_ARGS")))
        executableArguments.push_back(argument);
      return executableArguments;
    }

    RuntimeLaunchTarget makeExecutableLaunchTarget(
      const RuntimeInstallation& installation,
      const std::string& replayPath)
    {
      return {
        "executable",
        installation.executablePath,
        makeExecutableLaunchArguments(installation, replayPath),
        false
      };
    }

    std::vector<RuntimeLaunchTarget> makeRuntimeLaunchTargets(
      const RuntimeInstallation& installation,
      const std::string& replayPath)
    {
      std::vector<RuntimeLaunchTarget> launchTargets;

      if (installation.product == Product::StarCraftRemastered && remasteredWindowedLaunchEnabled())
        launchTargets.push_back(makeExecutableLaunchTarget(installation, replayPath));

      if (!installation.launcherPath.empty() && installation.product == Product::StarCraftRemastered)
      {
        if (installation.platform == Platform::MacOS && fileExists("/usr/bin/open"))
        {
          const std::filesystem::path launcherApp = appBundleForExecutablePath(installation.launcherPath);
          if (!launcherApp.empty())
          {
            launchTargets.push_back({
              "launcher-app",
              "/usr/bin/open",
              { "/usr/bin/open", launcherApp.string() },
              true
            });
          }
        }

        launchTargets.push_back({
          "launcher",
          installation.launcherPath,
          { installation.launcherPath },
          true
        });
      }

      if (installation.product != Product::StarCraftRemastered || !remasteredWindowedLaunchEnabled())
        launchTargets.push_back(makeExecutableLaunchTarget(installation, replayPath));
      return launchTargets;
    }

    std::string quoteCommandArgument(const std::string& argument)
    {
      std::string quoted = "\"";
      for (char ch : argument)
      {
        if (ch == '"')
          quoted += "\\\"";
        else
          quoted += ch;
      }
      quoted += '"';
      return quoted;
    }

    std::string makeCommandLine(const std::vector<std::string>& arguments)
    {
      std::ostringstream command;
      for (std::size_t i = 0; i < arguments.size(); ++i)
      {
        if (i > 0)
          command << ' ';
        command << quoteCommandArgument(arguments[i]);
      }
      return command.str();
    }

    RuntimeLaunchResult launchRuntimeTarget(
      const RuntimeInstallation& installation,
      const RuntimeLaunchTarget& target)
    {
      RuntimeLaunchResult result;

#if defined(_WIN32)
      STARTUPINFOA startupInfo;
      ZeroMemory(&startupInfo, sizeof(startupInfo));
      startupInfo.cb = sizeof(startupInfo);

      std::string command = makeCommandLine(target.arguments);
      PROCESS_INFORMATION processInfo;
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
        result.warnings.push_back(
          "runtime.launch_target_failed=" + target.kind + ": CreateProcess failed with Windows error "
          + std::to_string(GetLastError()));
        result.reason = result.warnings.back();
        return result;
      }

      result.launched = true;
      result.processId = static_cast<int>(processInfo.dwProcessId);
      result.warnings.push_back("runtime.launch_target=" + target.kind);
      CloseHandle(processInfo.hThread);
      CloseHandle(processInfo.hProcess);
#else
      pid_t pid = 0;
      std::string reason;
      if (!forkExecProcess(
            target.executable,
            target.arguments,
            installation.installRoot,
            target.allowImmediateSuccessExit,
            pid,
            reason))
      {
        result.warnings.push_back("runtime.launch_target_failed=" + target.kind + ": " + reason);
        result.reason = result.warnings.back();
        return result;
      }

      result.launched = true;
      result.processId = static_cast<int>(pid);
      result.warnings.push_back("runtime.launch_target=" + target.kind);
#endif

      return result;
    }

    std::vector<int> findBattleNetHandoffProcesses(const RuntimeInstallation& installation)
    {
#if defined(_WIN32)
      return findWindowsBattleNetHandoffProcesses(installation);
#else
      return findPosixBattleNetHandoffProcesses(installation);
#endif
    }

    bool terminateRuntimeProcessId(int processId, std::string& reason)
    {
      if (processId <= 0)
      {
        reason = "process id must be positive";
        return false;
      }

#if defined(_WIN32)
      HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(processId));
      if (process == nullptr)
      {
        reason = "OpenProcess(PROCESS_TERMINATE) failed with Windows error " + std::to_string(GetLastError());
        return false;
      }

      const BOOL terminated = TerminateProcess(process, 1);
      CloseHandle(process);
      if (!terminated)
      {
        reason = "TerminateProcess failed with Windows error " + std::to_string(GetLastError());
        return false;
      }
      return true;
#else
      const pid_t pid = static_cast<pid_t>(processId);
      auto reapExitedChild = [&]() -> bool
      {
        int status = 0;
        const pid_t exited = waitpid(pid, &status, WNOHANG);
        return exited == pid;
      };

      if (kill(pid, SIGTERM) != 0)
      {
        reason = "kill(SIGTERM) failed for process "
          + std::to_string(processId)
          + ": "
          + std::strerror(errno);
        return false;
      }

      for (int i = 0; i < 20; ++i)
      {
        if (reapExitedChild())
          return true;
        if (!runtimeProcessExists(processId))
          return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }

      if (kill(pid, SIGKILL) != 0)
      {
        reason = "kill(SIGKILL) failed for process "
          + std::to_string(processId)
          + ": "
          + std::strerror(errno);
        return false;
      }

      for (int i = 0; i < 20; ++i)
      {
        if (reapExitedChild())
          return true;
        if (!runtimeProcessExists(processId))
          return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }

      reason = "process is still visible after SIGTERM and SIGKILL: " + std::to_string(processId);
      return false;
#endif
    }

    bool terminateRuntimeProcessIds(
      const std::vector<int>& processIds,
      RuntimeLaunchResult& result,
      const std::string& warningPrefix,
      const std::string& failureContext)
    {
      for (int processId : processIds)
      {
        std::string reason;
        if (!terminateRuntimeProcessId(processId, reason))
        {
          result.reason = "unable to terminate " + failureContext + ": " + reason;
          return false;
        }
        result.warnings.push_back(warningPrefix + "=" + std::to_string(processId));
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(1000));
      return true;
    }

    bool terminateStaleHandoffProcessIds(const std::vector<int>& processIds, RuntimeLaunchResult& result)
    {
      return terminateRuntimeProcessIds(
        processIds,
        result,
        "battle.net.stale_handoff_terminated",
        "stale Battle.net handoff process");
    }

    bool terminateExistingGameProcessIds(const std::vector<int>& processIds, RuntimeLaunchResult& result)
    {
      return terminateRuntimeProcessIds(
        processIds,
        result,
        "runtime.existing_process_terminated",
        "existing StarCraft process");
    }

    void appendLaunchWarnings(RuntimeLaunchResult& result, const RuntimeLaunchResult& launched)
    {
      result.warnings.insert(result.warnings.end(), launched.warnings.begin(), launched.warnings.end());
    }

    int waitForStableRuntimeProcess(
      const RuntimeInstallation& installation,
      int waitMilliseconds,
      int stableMilliseconds,
      std::string* rejectionReason = nullptr)
    {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(waitMilliseconds);
      while (true)
      {
        std::string latestRejectionReason;
        const int stableProcessId =
          findStableProcessId(installation, stableMilliseconds, &latestRejectionReason);
        if (stableProcessId > 0)
          return stableProcessId;
        if (!latestRejectionReason.empty() && rejectionReason != nullptr)
          *rejectionReason = latestRejectionReason;

        if (waitMilliseconds <= 0 || std::chrono::steady_clock::now() >= deadline)
          return 0;

        std::this_thread::sleep_for(std::chrono::milliseconds(250));
      }
    }

    bool terminateLaunchTargetProcess(
      int processId,
      RuntimeLaunchResult& result,
      const std::string& targetKind)
    {
      if (processId <= 0 || !runtimeProcessExists(processId))
        return true;

      return terminateRuntimeProcessIds(
        { processId },
        result,
        "runtime.launch_target_process_terminated",
        "non-initialized " + targetKind + " launch target process");
    }

    void appendBattleNetHandoffWarnings(
      RuntimeLaunchResult& result,
      const std::vector<int>& handoffProcessIds,
      const std::string& suffix)
    {
      if (handoffProcessIds.empty())
        return;

      result.warnings.push_back(
        "battle.net.process_count" + suffix + "=" + std::to_string(handoffProcessIds.size()));
      result.warnings.push_back(
        "battle.net.process_id" + suffix + "=" + std::to_string(handoffProcessIds.front()));
      for (std::size_t i = 0; i < handoffProcessIds.size(); ++i)
      {
        result.warnings.push_back(
          "battle.net.process_id" + suffix + "." + std::to_string(i) + "="
          + std::to_string(handoffProcessIds[i]));
      }
    }

    RuntimeLaunchResult launchRuntimeProcessWithFallback(
      const RuntimeInstallation& installation,
      int waitMilliseconds,
      int stableMilliseconds,
      bool replaceStaleHandoff,
      const std::string& replayPath)
    {
      RuntimeLaunchResult result;
      result.requiredStableMilliseconds = stableMilliseconds;
      const std::vector<RuntimeLaunchTarget> launchTargets = makeRuntimeLaunchTargets(installation, replayPath);

      for (const RuntimeLaunchTarget& target : launchTargets)
      {
        RuntimeLaunchResult launched = launchRuntimeTarget(installation, target);
        appendLaunchWarnings(result, launched);
        if (!launched.launched)
          continue;

        result.launched = true;
        result.processId = launched.processId;

#if defined(_WIN32)
        if (target.kind == "executable" && launched.processId > 0)
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

        std::string stableRejectionReason;
        const int stableProcessId =
          waitForStableRuntimeProcess(installation, waitMilliseconds, stableMilliseconds, &stableRejectionReason);
        if (stableProcessId > 0)
        {
          result.running = true;
          result.processId = stableProcessId;
          result.observedStableMilliseconds = stableMilliseconds;
          return result;
        }

        if (!stableRejectionReason.empty())
          result.warnings.push_back("runtime.stable_process_rejected=" + stableRejectionReason);
        result.warnings.push_back("runtime.launch_target_no_game=" + target.kind);
        if (target.kind == "executable"
            && !stableRejectionReason.empty()
            && !terminateLaunchTargetProcess(launched.processId, result, target.kind))
          return result;

        const std::vector<int> handoffProcessIds = findBattleNetHandoffProcesses(installation);
        if (handoffProcessIds.empty())
          continue;

        appendBattleNetHandoffWarnings(result, handoffProcessIds, "_after_launch");
        if (!replaceStaleHandoff)
        {
          result.reason =
            "Battle.net StarCraft handoff became visible after launch target "
            + target.kind
            + "; not launching another Battle.net instance; no matching game process became visible before the wait timeout";
          return result;
        }

        if (!terminateStaleHandoffProcessIds(handoffProcessIds, result))
          return result;
      }

      result.reason = result.launched
        ? "StarCraft Remastered launch targets were tried, but no matching game process became visible before the wait timeout"
        : (result.warnings.empty()
#if defined(_WIN32)
          ? "CreateProcess failed for all launch targets"
#else
          ? "fork/exec failed for all launch targets"
#endif
          : result.warnings.back());
      return result;
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

  RuntimeEnvironment resolveRuntimeEnvironment(const RuntimeEnvironment& baseEnvironment)
  {
    RuntimeEnvironment environment = baseEnvironment;
    if (environment.product != Product::Unknown && environment.product != Product::StarCraftRemastered)
      return environment;

    const RuntimeInstallation installation = detectStarCraftInstallation(environment);
    if (!installation.found)
      return environment;

    if (environment.platform == Platform::Unknown)
      environment.platform = installation.platform;
    if (environment.product == Product::Unknown)
      environment.product = installation.product;
    if (environment.version.empty())
      environment.version = installation.version;
    if (environment.executablePath.empty())
      environment.executablePath = installation.executablePath;

    if (environment.processId <= 0)
    {
      const std::vector<int> processIds = findRuntimeProcessIds(installation);
      if (processIds.size() == 1)
        environment.processId = processIds.front();
    }

    if (environment.executorBridgePath.empty())
      environment.executorBridgePath = findMatchingLiveBridgePath(environment);

    return environment;
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
    int waitMilliseconds,
    int stableMilliseconds,
    bool replaceStaleHandoff,
    bool replaceRunning,
    const std::string& replayPath)
  {
    RuntimeLaunchResult result;
    result.requiredStableMilliseconds = stableMilliseconds;
    if (!installation.found)
    {
      result.reason = installation.reason;
      return result;
    }

    if (!replayPath.empty())
    {
      result.warnings.push_back("runtime.launch_replay=" + replayPath);
      std::string replayReason;
      if (!validateReplayLaunchRequest(installation, replayPath, replayReason))
      {
        result.requestAccepted = false;
        result.reason = replayReason;
        return result;
      }
    }

    if (replaceRunning)
    {
      if (!launchIfMissing)
      {
        result.reason = "replace-running requires launch";
        return result;
      }

      const std::vector<int> existingProcessIds = findRuntimeProcessIds(installation);
      if (!existingProcessIds.empty())
      {
        result.warnings.push_back("runtime.existing_process_count=" + std::to_string(existingProcessIds.size()));
        if (!terminateExistingGameProcessIds(existingProcessIds, result))
          return result;
      }
    }

    std::string stableRejectionReason;
    int stableProcessId = findStableProcessId(installation, stableMilliseconds, &stableRejectionReason);
    if (stableProcessId > 0)
    {
      if (!replayPath.empty() && !replaceRunning)
      {
        result.requestAccepted = false;
        result.running = true;
        result.processId = stableProcessId;
        result.reason =
          "play-replay requested but an existing StarCraft process is already running; use --replace-running to relaunch a single process with the replay";
        result.warnings.push_back("runtime.launch_replay_existing_process_requires_replace_running=true");
        return result;
      }
      result.running = true;
      result.processId = stableProcessId;
      result.observedStableMilliseconds = stableMilliseconds;
      return result;
    }
    if (!stableRejectionReason.empty())
    {
      result.warnings.push_back("runtime.stable_process_rejected=" + stableRejectionReason);
      const std::vector<int> visibleProcessIds = findRuntimeProcessIds(installation);
      if (!visibleProcessIds.empty() && !replaceRunning)
      {
        result.requestAccepted = false;
        result.processId = visibleProcessIds.front();
        result.reason =
          "a StarCraft process is visible but not initialized for attach; use --replace-running to terminate it before relaunching";
        result.warnings.push_back("runtime.existing_process_requires_replace_running=true");
        return result;
      }
    }

    if (!launchIfMissing)
    {
      result.reason = "StarCraft Remastered is installed but no running process was found";
      return result;
    }

    bool waitingOnExistingHandoff = false;
    const std::vector<int> handoffProcessIds = findBattleNetHandoffProcesses(installation);
    if (!handoffProcessIds.empty())
    {
      result.warnings.push_back("battle.net.process_count=" + std::to_string(handoffProcessIds.size()));
      result.warnings.push_back("battle.net.process_id=" + std::to_string(handoffProcessIds.front()));
      for (std::size_t i = 0; i < handoffProcessIds.size(); ++i)
        result.warnings.push_back("battle.net.process_id." + std::to_string(i) + "=" + std::to_string(handoffProcessIds[i]));

      if (replaceStaleHandoff)
      {
        if (!terminateStaleHandoffProcessIds(handoffProcessIds, result))
          return result;

        RuntimeLaunchResult launched =
          launchRuntimeProcessWithFallback(
            installation,
            waitMilliseconds,
            stableMilliseconds,
            replaceStaleHandoff,
            replayPath);
        appendLaunchWarnings(result, launched);
        result.launched = launched.launched;
        result.processId = launched.processId;
        result.running = launched.running;
        result.observedStableMilliseconds = launched.observedStableMilliseconds;
        if (!launched.running)
        {
          result.reason = launched.reason;
          return result;
        }
        return result;
      }
      else
      {
        waitingOnExistingHandoff = true;
      }
    }
    else
    {
      RuntimeLaunchResult launched =
        launchRuntimeProcessWithFallback(
          installation,
          waitMilliseconds,
          stableMilliseconds,
          replaceStaleHandoff,
          replayPath);
      appendLaunchWarnings(result, launched);
      result.launched = launched.launched;
      result.processId = launched.processId;
      result.running = launched.running;
      result.observedStableMilliseconds = launched.observedStableMilliseconds;
      if (!launched.running)
      {
        result.reason = launched.reason;
        return result;
      }
      return result;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(waitMilliseconds);
    while (std::chrono::steady_clock::now() < deadline)
    {
      stableProcessId = findStableProcessId(installation, stableMilliseconds);
      if (stableProcessId > 0)
      {
        result.running = true;
        result.processId = stableProcessId;
        result.observedStableMilliseconds = stableMilliseconds;
        return result;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    result.running = false;
    if (waitingOnExistingHandoff)
      result.reason =
        "Battle.net StarCraft handoff is already running; not launching another Battle.net instance; no matching game process became visible before the wait timeout";
    else
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
    evidence.supportErrors = collectRuntimeSupportErrors(installation, 8, 10);
    evidence.sessionEvents = collectRuntimeSessionEvents(installation, 8, 50);
    evidence.sessionSummary = summarizeRuntimeSessionEvents(evidence.sessionEvents);
    evidence.diagnosis = diagnoseRuntimeEvidence(evidence);
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
    writeEvidenceField(output, "runtime.request_accepted", evidence.launchResult.requestAccepted);
    writeEvidenceField(output, "runtime.process_id", evidence.launchResult.processId);
    writeEvidenceField(output, "runtime.required_stable_ms", evidence.launchResult.requiredStableMilliseconds);
    writeEvidenceField(output, "runtime.observed_stable_ms", evidence.launchResult.observedStableMilliseconds);
    writeEvidenceField(output, "runtime.reason", evidence.launchResult.reason);
    for (std::size_t i = 0; i < evidence.launchResult.warnings.size(); ++i)
      writeEvidenceField(output, "runtime.warning." + std::to_string(i), evidence.launchResult.warnings[i]);

    writeEvidenceField(output, "diagnosis.status", evidence.diagnosis.status);
    writeEvidenceField(output, "diagnosis.game_process_visible", evidence.diagnosis.gameProcessVisible);
    writeEvidenceField(output, "diagnosis.battle_net_main_visible", evidence.diagnosis.battleNetMainVisible);
    writeEvidenceField(output, "diagnosis.battle_net_handoff_visible", evidence.diagnosis.battleNetHandoffVisible);
    writeEvidenceField(output, "diagnosis.battle_net_support_visible", evidence.diagnosis.battleNetSupportVisible);
    writeEvidenceField(
      output,
      "diagnosis.multiple_battle_net_main_visible",
      evidence.diagnosis.multipleBattleNetMainVisible);
    writeEvidenceField(
      output,
      "diagnosis.multiple_battle_net_handoffs_visible",
      evidence.diagnosis.multipleBattleNetHandoffsVisible);
    writeEvidenceField(output, "diagnosis.game_process_count", evidence.diagnosis.gameProcessCount);
    writeEvidenceField(output, "diagnosis.battle_net_main_count", evidence.diagnosis.battleNetMainCount);
    writeEvidenceField(output, "diagnosis.battle_net_handoff_count", evidence.diagnosis.battleNetHandoffCount);
    writeEvidenceField(output, "diagnosis.battle_net_support_count", evidence.diagnosis.battleNetSupportCount);
    writeEvidenceField(output, "diagnosis.short_lived_session_observed", evidence.diagnosis.shortLivedSessionObserved);
    if (evidence.diagnosis.shortLivedSessionAgeMilliseconds >= 0)
      writeEvidenceField(
        output,
        "diagnosis.short_lived_session_age_ms",
        evidence.diagnosis.shortLivedSessionAgeMilliseconds);
    writeEvidenceField(output, "diagnosis.stale_handoff_suspected", evidence.diagnosis.staleHandoffSuspected);
    writeEvidenceField(output, "diagnosis.ready_for_attach", evidence.diagnosis.readyForAttach);
    writeEvidenceField(output, "diagnosis.battle_net_support_code", evidence.diagnosis.battleNetSupportCode);
    writeEvidenceField(output, "diagnosis.battle_net_support_url", evidence.diagnosis.battleNetSupportUrl);
    writeEvidenceField(output, "diagnosis.battle_net_support_line", evidence.diagnosis.battleNetSupportLine);
    writeEvidenceField(output, "diagnosis.blocker_count", static_cast<int>(evidence.diagnosis.blockers.size()));
    for (std::size_t i = 0; i < evidence.diagnosis.blockers.size(); ++i)
      writeEvidenceField(output, "diagnosis.blocker." + std::to_string(i), evidence.diagnosis.blockers[i]);

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

    writeEvidenceField(output, "support.error_count", static_cast<int>(evidence.supportErrors.size()));
    for (std::size_t i = 0; i < evidence.supportErrors.size(); ++i)
    {
      const RuntimeSupportError& supportError = evidence.supportErrors[i];
      const std::string prefix = "support.error." + std::to_string(i) + ".";
      writeEvidenceField(output, prefix + "path", supportError.path);
      writeEvidenceField(output, prefix + "code", supportError.code);
      writeEvidenceField(output, prefix + "url", supportError.url);
      writeEvidenceField(output, prefix + "line", supportError.line);
    }

    writeEvidenceField(output, "session.event_count", static_cast<int>(evidence.sessionEvents.size()));
    writeEvidenceField(output, "session.started_event_count", evidence.sessionSummary.startedEventCount);
    writeEvidenceField(output, "session.ended_event_count", evidence.sessionSummary.endedEventCount);
    writeEvidenceField(output, "session.preexisting_event_count", evidence.sessionSummary.preexistingEventCount);
    writeEvidenceField(output, "session.install_state_event_count", evidence.sessionSummary.installStateEventCount);
    writeEvidenceField(output, "session.launch_process_event_count", evidence.sessionSummary.launchProcessEventCount);
    if (evidence.sessionSummary.latestLaunchProcessId > 0)
      writeEvidenceField(output, "session.latest_launch_process_id", evidence.sessionSummary.latestLaunchProcessId);
    writeEvidenceField(output, "session.related_event_count", evidence.sessionSummary.relatedEventCount);
    writeEvidenceField(
      output,
      "session.transition_count",
      static_cast<int>(evidence.sessionSummary.transitions.size()));
    writeEvidenceField(
      output,
      "session.complete_transition_count",
      evidence.sessionSummary.completeTransitionCount);
    writeEvidenceField(
      output,
      "session.incomplete_transition_count",
      evidence.sessionSummary.incompleteTransitionCount);
    if (evidence.sessionSummary.shortestDurationMilliseconds >= 0)
      writeEvidenceField(
        output,
        "session.shortest_transition_duration_ms",
        evidence.sessionSummary.shortestDurationMilliseconds);
    if (evidence.sessionSummary.longestDurationMilliseconds >= 0)
      writeEvidenceField(
        output,
        "session.longest_transition_duration_ms",
        evidence.sessionSummary.longestDurationMilliseconds);
    if (evidence.sessionSummary.latestTransitionDurationMilliseconds >= 0)
      writeEvidenceField(
        output,
        "session.latest_transition_duration_ms",
        evidence.sessionSummary.latestTransitionDurationMilliseconds);
    writeEvidenceField(
      output,
      "session.latest_transition_start_timestamp",
      evidence.sessionSummary.latestTransitionStartTimestamp);
    writeEvidenceField(
      output,
      "session.latest_transition_end_timestamp",
      evidence.sessionSummary.latestTransitionEndTimestamp);
    writeEvidenceField(output, "session.latest_state", evidence.sessionSummary.latestState);
    writeEvidenceField(
      output,
      "session.latest_observed_timestamp",
      evidence.sessionSummary.latestObservedTimestamp);
    writeEvidenceField(output, "session.latest_reason", evidence.sessionSummary.latestReason);
    for (std::size_t i = 0; i < evidence.sessionSummary.transitions.size(); ++i)
    {
      const RuntimeSessionTransition& transition = evidence.sessionSummary.transitions[i];
      const std::string prefix = "session.transition." + std::to_string(i) + ".";
      writeEvidenceField(output, prefix + "complete", transition.complete);
      if (transition.durationMilliseconds >= 0)
        writeEvidenceField(output, prefix + "duration_ms", transition.durationMilliseconds);
      writeEvidenceField(output, prefix + "start_timestamp", transition.startTimestamp);
      writeEvidenceField(output, prefix + "end_timestamp", transition.endTimestamp);
      writeEvidenceField(output, prefix + "start_path", transition.startPath);
      writeEvidenceField(output, prefix + "end_path", transition.endPath);
      writeEvidenceField(output, prefix + "start_line", transition.startLine);
      writeEvidenceField(output, prefix + "end_line", transition.endLine);
      writeEvidenceField(output, prefix + "reason", transition.reason);
    }
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
    ready << "mode=" << RuntimeExecutorBridgeBootstrapMode << '\n';
    ready << "process_id=" << environment.processId << '\n';
    ready << "executable=" << environment.executablePath << '\n';
    return true;
  }
}
