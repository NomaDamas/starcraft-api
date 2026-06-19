#include <BWAPI/Runtime/RuntimeBackend.h>

#include <cerrno>
#include <cstdlib>
#include <limits>

namespace BWAPI::Runtime
{
  namespace
  {
    int parseProcessId(const char* value)
    {
      if (value == nullptr || *value == '\0')
        return 0;

      char* end = nullptr;
      errno = 0;
      const long parsed = std::strtol(value, &end, 10);
      if (errno != 0 || end == value || *end != '\0' || parsed <= 0 || parsed > std::numeric_limits<int>::max())
        return 0;
      return static_cast<int>(parsed);
    }
  }

  RuntimeEnvironment RuntimeEnvironment::detectHost()
  {
    RuntimeEnvironment environment;

#if defined(_WIN32)
    environment.platform = Platform::Windows;
#elif defined(__APPLE__)
    environment.platform = Platform::MacOS;
#elif defined(__linux__)
    environment.platform = Platform::Linux;
#else
    environment.platform = Platform::Unknown;
#endif

    if (const char* product = std::getenv("STARCRAFT_API_PRODUCT"))
      environment.product = parseProduct(product);
    if (const char* version = std::getenv("STARCRAFT_API_VERSION"))
      environment.version = version;
    if (const char* processId = std::getenv("STARCRAFT_API_PROCESS_ID"))
      environment.processId = parseProcessId(processId);
    if (const char* executable = std::getenv("STARCRAFT_API_EXECUTABLE"))
      environment.executablePath = executable;
    if (const char* manifest = std::getenv("STARCRAFT_API_MANIFEST"))
      environment.manifestPath = manifest;

    return environment;
  }
}
