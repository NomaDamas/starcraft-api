#include <BWAPI/Runtime/RuntimeBackend.h>
#include <BWAPI/Runtime/RuntimeInstallation.h>
#include <BWAPI/Runtime/RuntimeProcess.h>
#include <BWAPI/Runtime/RuntimeProcessMemory.h>

#include <cstdlib>
#include <algorithm>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

using namespace BWAPI::Runtime;

namespace
{
  void printUsage()
  {
    std::cout
      << "usage: starcraft-runtime-memory-probe [options]\n"
      << "  --process-id <pid>       override target runtime process id\n"
      << "  --executable <path>      override target executable path\n"
      << "  --self                   probe this CLI process instead of resolving StarCraft\n"
      << "  --address <address>      read from an explicit target address, decimal or 0x-prefixed\n"
      << "  --size <bytes>           read size when --address is provided (default: 16)\n"
      << "  --require-open           return non-zero unless process open succeeds\n"
      << "  --require-access         return non-zero unless process memory access succeeds\n"
      << "  --require-read           return non-zero unless requested memory read succeeds\n"
      << "  --help                   show this help\n";
  }

  bool parsePositiveInt(const std::string& value, int& output)
  {
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0' || parsed <= 0 || parsed > std::numeric_limits<int>::max())
      return false;
    output = static_cast<int>(parsed);
    return true;
  }

  bool parseSize(const std::string& value, std::size_t& output)
  {
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0' || parsed == 0)
      return false;
    output = static_cast<std::size_t>(parsed);
    return static_cast<unsigned long long>(output) == parsed;
  }

  bool parseAddress(const std::string& value, std::uintptr_t& output)
  {
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value.c_str(), &end, 0);
    if (end == value.c_str() || *end != '\0' || parsed == 0)
      return false;
    output = static_cast<std::uintptr_t>(parsed);
    return static_cast<unsigned long long>(output) == parsed;
  }

  std::string hexBytes(const std::vector<unsigned char>& bytes)
  {
    std::ostringstream output;
    const std::size_t limit = std::min<std::size_t>(bytes.size(), 64);
    for (std::size_t i = 0; i < limit; ++i)
    {
      if (i > 0)
        output << ' ';
      output << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(bytes[i]);
    }
    if (bytes.size() > limit)
      output << " ...";
    return output.str();
  }
}

int main(int argc, char** argv)
{
  bool requireOpen = false;
  bool requireAccess = false;
  bool requireRead = false;
  bool readRequested = false;
  bool self = false;
  int processIdOverride = 0;
  std::uintptr_t address = 0;
  std::size_t size = 16;
  std::string executableOverride;

  for (int i = 1; i < argc; ++i)
  {
    const std::string arg = argv[i];
    if (arg == "--require-open")
    {
      requireOpen = true;
    }
    else if (arg == "--require-access")
    {
      requireAccess = true;
    }
    else if (arg == "--self")
    {
      self = true;
    }
    else if (arg == "--require-read")
    {
      requireRead = true;
      readRequested = true;
    }
    else if (arg == "--process-id")
    {
      if (i + 1 >= argc || !parsePositiveInt(argv[++i], processIdOverride))
      {
        std::cerr << "--process-id requires a positive integer\n";
        return 64;
      }
    }
    else if (arg == "--executable")
    {
      if (i + 1 >= argc)
      {
        std::cerr << "--executable requires a path\n";
        return 64;
      }
      executableOverride = argv[++i];
    }
    else if (arg == "--address")
    {
      if (i + 1 >= argc || !parseAddress(argv[++i], address))
      {
        std::cerr << "--address requires a positive integer address\n";
        return 64;
      }
      readRequested = true;
    }
    else if (arg == "--size")
    {
      if (i + 1 >= argc || !parseSize(argv[++i], size))
      {
        std::cerr << "--size requires a positive integer byte count\n";
        return 64;
      }
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

  RuntimeEnvironment environment = RuntimeEnvironment::detectHost();
  if (processIdOverride > 0)
    environment.processId = processIdOverride;
  if (self)
    environment.processId = currentProcessId();
  if (!executableOverride.empty())
    environment.executablePath = executableOverride;
  if (!self)
    environment = resolveRuntimeEnvironment(environment);
  if (!self && environment.processId <= 0)
  {
    const RuntimeInstallation installation = detectStarCraftInstallation(environment);
    if (installation.found)
    {
      const std::vector<int> processIds = findRuntimeProcessIds(installation);
      if (!processIds.empty())
      {
        environment = makeRuntimeEnvironmentForInstallation(environment, installation, processIds.front());
      }
      else
      {
        RuntimeLaunchResult launchResult;
        const RuntimeEvidence evidence = collectRuntimeEvidence(installation, launchResult);
        for (const RuntimeObservedProcess& process : evidence.processes)
        {
          if (process.category == "starcraft-game")
          {
            environment = makeRuntimeEnvironmentForInstallation(environment, installation, process.processId);
            break;
          }
        }
      }
    }
  }

  std::cout << "platform=" << toString(environment.platform) << '\n';
  std::cout << "product=" << toString(environment.product) << '\n';
  if (!environment.version.empty())
    std::cout << "version=" << environment.version << '\n';
  if (environment.processId > 0)
    std::cout << "process.id=" << environment.processId << '\n';
  if (!environment.executablePath.empty())
    std::cout << "executable.path=" << environment.executablePath << '\n';

  const RuntimeProcessOpenResult open = openRuntimeProcess(environment);
  std::cout << "memory.opened=" << (open.opened ? "true" : "false") << '\n';
  if (!open.reason.empty())
    std::cout << "memory.open.reason=" << open.reason << '\n';
  const RuntimeMemoryAccessResult access = openProcessMemoryAccess(environment.processId);
  std::cout << "memory.accessible=" << (access.accessible ? "true" : "false") << '\n';
  if (!access.reason.empty())
    std::cout << "memory.access.reason=" << access.reason << '\n';

  if (!readRequested)
  {
    std::cout << "memory.read.requested=false\n";
    if (requireOpen && !open.opened)
      return 3;
    if (requireAccess && !access.accessible)
      return 4;
    return 0;
  }

  RuntimeMemoryReadResult read = readProcessMemory(environment.processId, address, size);
  std::cout << "memory.read.requested=true\n";
  std::cout << "memory.read.success=" << (read.success ? "true" : "false") << '\n';
  std::cout << "memory.read.bytes=" << read.bytesRead << '\n';
  if (!read.reason.empty())
    std::cout << "memory.read.reason=" << read.reason << '\n';
  if (read.success)
    std::cout << "memory.read.hex=" << hexBytes(read.bytes) << '\n';

  if (requireOpen && !open.opened)
    return 3;
  if (requireAccess && !access.accessible)
    return 4;
  if (requireRead && !read.success)
    return 5;
  return 0;
}
