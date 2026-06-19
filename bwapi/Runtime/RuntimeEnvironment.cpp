#include <BWAPI/Runtime/RuntimeBackend.h>

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

    return environment;
  }
}
