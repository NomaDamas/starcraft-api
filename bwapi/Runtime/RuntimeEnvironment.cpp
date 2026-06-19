#include <BWAPI/Runtime/RuntimeBackend.h>

#include <cstdlib>

namespace BWAPI::Runtime
{
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
    if (const char* executable = std::getenv("STARCRAFT_API_EXECUTABLE"))
      environment.executablePath = executable;

    return environment;
  }
}
