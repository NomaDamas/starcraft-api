#include <BWAPI/Runtime/RuntimeProcess.h>

#include <cerrno>
#include <cstring>
#include <sstream>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__) || defined(__linux__)
#include <signal.h>
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

    RuntimeProcessOpenResult result;
    result.opened = true;
    result.processId = environment.processId;
    result.executablePath = environment.executablePath;
    return result;
  }
}
