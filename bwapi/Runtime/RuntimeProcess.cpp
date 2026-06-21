#include <BWAPI/Runtime/RuntimeProcess.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <limits.h>
#include <sstream>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <libproc.h>
#include <signal.h>
#include <sys/proc.h>
#include <sys/proc_info.h>
#elif defined(__linux__)
#include <signal.h>
#include <unistd.h>
#endif

namespace BWAPI::Runtime
{
  namespace
  {
    RuntimeProcessOpenResult processFailure(std::string reason)
    {
      RuntimeProcessOpenResult result;
      result.reason = std::move(reason);
      return result;
    }

#if defined(_WIN32)
    std::string lastErrorMessage(const char* operation)
    {
      std::ostringstream message;
      message << operation << " failed with Windows error " << GetLastError();
      return message.str();
    }
#endif

    std::string normalizePath(const std::string& path)
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

    bool pathMatches(const std::string& actual, const std::string& expected)
    {
      if (expected.empty())
        return true;
      if (actual.empty())
        return false;
      return normalizePath(actual) == normalizePath(expected);
    }
  }

  bool runtimeProcessExists(int processId)
  {
    if (processId <= 0)
      return false;

#if defined(_WIN32)
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(processId));
    if (process == nullptr)
      return false;
    CloseHandle(process);
    return true;
#elif defined(__APPLE__) || defined(__linux__)
    if (kill(processId, 0) == 0)
      return true;
    return errno == EPERM;
#else
    return false;
#endif
  }

  std::string runtimeProcessExecutablePath(int processId)
  {
    if (processId <= 0)
      return {};

#if defined(_WIN32)
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(processId));
    if (process == nullptr)
      return {};

    std::vector<char> buffer(32768);
    DWORD size = static_cast<DWORD>(buffer.size());
    std::string path;
    if (QueryFullProcessImageNameA(process, 0, buffer.data(), &size))
      path.assign(buffer.data(), size);
    CloseHandle(process);
    return path;
#elif defined(__APPLE__)
    std::vector<char> buffer(PROC_PIDPATHINFO_MAXSIZE);
    const int result = proc_pidpath(processId, buffer.data(), static_cast<uint32_t>(buffer.size()));
    if (result <= 0)
      return {};
    return std::string(buffer.data(), static_cast<std::size_t>(result));
#elif defined(__linux__)
    std::vector<char> buffer(PATH_MAX + 1);
    const std::string linkPath = "/proc/" + std::to_string(processId) + "/exe";
    const ssize_t count = readlink(linkPath.c_str(), buffer.data(), buffer.size() - 1);
    if (count < 0)
      return {};
    buffer[static_cast<std::size_t>(count)] = '\0';
    return std::string(buffer.data(), static_cast<std::size_t>(count));
#else
    return {};
#endif
  }

  RuntimeProcessStateResult inspectRuntimeProcessState(int processId)
  {
    RuntimeProcessStateResult result;
    if (processId <= 0)
    {
      result.reason = "process id must be positive";
      return result;
    }

#if defined(__APPLE__)
    proc_taskallinfo info;
    std::memset(&info, 0, sizeof(info));
    const int bytes = proc_pidinfo(
      processId,
      PROC_PIDTASKALLINFO,
      0,
      &info,
      static_cast<int>(sizeof(info)));
    if (bytes <= 0)
    {
      result.reason = "proc_pidinfo(PROC_PIDTASKALLINFO) failed";
      return result;
    }

    result.inspected = true;
    result.status = info.pbsd.pbi_status;
    result.threadCount = info.ptinfo.pti_threadnum;
    result.suspended = result.status == SSTOP;
    return result;
#else
    (void)processId;
    result.reason = "runtime process state inspection is unsupported on this platform";
    return result;
#endif
  }

  RuntimeProcessOpenResult openRuntimeProcess(const RuntimeEnvironment& environment)
  {
    if (environment.processId <= 0)
      return processFailure("runtime process id is empty");

#if defined(_WIN32)
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(environment.processId));
    if (process == nullptr)
      return processFailure(lastErrorMessage("OpenProcess"));
    CloseHandle(process);
#elif defined(__APPLE__) || defined(__linux__)
    if (!runtimeProcessExists(environment.processId))
      return processFailure("runtime process does not exist or is not visible: " + std::to_string(environment.processId));
#else
    return processFailure("runtime process attachment is unsupported on this platform");
#endif

    const std::string actualExecutablePath = runtimeProcessExecutablePath(environment.processId);
    if (!pathMatches(actualExecutablePath, environment.executablePath))
    {
      return processFailure(
        "runtime process executable does not match selected runtime: expected "
        + environment.executablePath
        + " actual "
        + (actualExecutablePath.empty() ? "<unknown>" : actualExecutablePath));
    }

    RuntimeProcessOpenResult result;
    result.opened = true;
    result.processId = environment.processId;
    result.executablePath = actualExecutablePath.empty() ? environment.executablePath : actualExecutablePath;
    return result;
  }
}
