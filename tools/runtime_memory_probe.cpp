#include <BWAPI/Runtime/RuntimeBackend.h>
#include <BWAPI/Runtime/RuntimeInstallation.h>
#include <BWAPI/Runtime/RuntimeProcess.h>
#include <BWAPI/Runtime/RuntimeProcessMemory.h>

#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

using namespace BWAPI::Runtime;

namespace
{
  volatile char selfWritableFindFixture[] = "starcraft-api-memory-probe-self-fixture";

  void keepSelfWritableFindFixtureAlive()
  {
    const char first = selfWritableFindFixture[0];
    selfWritableFindFixture[0] = first;
  }

  void printUsage()
  {
    std::cout
      << "usage: starcraft-runtime-memory-probe [options]\n"
      << "  --product <name>         override runtime product\n"
      << "  --version <version>      override runtime version\n"
      << "  --process-id <pid>       override target runtime process id\n"
      << "  --executable <path>      override target executable path\n"
      << "  --self                   probe this CLI process instead of resolving StarCraft\n"
      << "  --address <address>      read from an explicit target address, decimal or 0x-prefixed\n"
      << "  --read-first-readable    find and read the first readable target memory region\n"
      << "  --process-state          print OS process status/thread diagnostics\n"
      << "  --region-summary         print process memory region counters\n"
      << "  --find-ascii <text>      scan readable target memory for an ASCII string\n"
      << "  --find-u64 <value>       scan readable target memory for a 64-bit little-endian value\n"
      << "  --find-max-scan-mb <mb>  maximum readable memory scanned by --find-ascii (default: 128)\n"
      << "  --find-writable-only     scan only readable+writable memory regions\n"
      << "  --find-non-executable-only\n"
      << "                           scan only readable non-executable memory regions\n"
      << "  --size <bytes>           read size when --address is provided (default: 16)\n"
      << "  --dump-out <path>        write requested bytes to a binary file when read succeeds\n"
      << "  --require-open           return non-zero unless process open succeeds\n"
      << "  --require-access         return non-zero unless process memory access succeeds\n"
      << "  --require-read           return non-zero unless requested memory read succeeds\n"
      << "  --require-find           return non-zero unless requested memory scan finds a match\n"
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
  bool readFirstReadable = false;
  bool processStateRequested = false;
  bool regionSummaryRequested = false;
  bool findRequested = false;
  bool findU64Requested = false;
  bool findWritableOnly = false;
  bool findNonExecutableOnly = false;
  bool requireFind = false;
  bool self = false;
  int processIdOverride = 0;
  std::uintptr_t address = 0;
  std::uint64_t findU64 = 0;
  std::size_t size = 16;
  std::size_t findMaxScanBytes = 128 * 1024 * 1024;
  Product productOverride = Product::Unknown;
  std::string versionOverride;
  std::string executableOverride;
  std::string dumpOut;
  std::string findAscii;

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
    else if (arg == "--require-find")
    {
      requireFind = true;
    }
    else if (arg == "--process-id")
    {
      if (i + 1 >= argc || !parsePositiveInt(argv[++i], processIdOverride))
      {
        std::cerr << "--process-id requires a positive integer\n";
        return 64;
      }
    }
    else if (arg == "--product")
    {
      if (i + 1 >= argc)
      {
        std::cerr << "--product requires a value\n";
        return 64;
      }
      productOverride = parseProduct(argv[++i]);
      if (productOverride == Product::Unknown)
      {
        std::cerr << "--product requires a known runtime product\n";
        return 64;
      }
    }
    else if (arg == "--version")
    {
      if (i + 1 >= argc)
      {
        std::cerr << "--version requires a value\n";
        return 64;
      }
      versionOverride = argv[++i];
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
    else if (arg == "--read-first-readable")
    {
      readFirstReadable = true;
      readRequested = true;
    }
    else if (arg == "--process-state")
    {
      processStateRequested = true;
    }
    else if (arg == "--region-summary")
    {
      regionSummaryRequested = true;
    }
    else if (arg == "--find-ascii")
    {
      if (i + 1 >= argc)
      {
        std::cerr << "--find-ascii requires a value\n";
        return 64;
      }
      findAscii = argv[++i];
      findRequested = true;
    }
    else if (arg == "--find-u64")
    {
      std::uintptr_t parsed = 0;
      if (i + 1 >= argc || !parseAddress(argv[++i], parsed))
      {
        std::cerr << "--find-u64 requires a positive integer value\n";
        return 64;
      }
      findU64 = static_cast<std::uint64_t>(parsed);
      findRequested = true;
      findU64Requested = true;
    }
    else if (arg == "--find-writable-only")
    {
      findWritableOnly = true;
    }
    else if (arg == "--find-non-executable-only")
    {
      findNonExecutableOnly = true;
    }
    else if (arg == "--find-max-scan-mb")
    {
      std::size_t megabytes = 0;
      if (i + 1 >= argc || !parseSize(argv[++i], megabytes))
      {
        std::cerr << "--find-max-scan-mb requires a positive integer megabyte count\n";
        return 64;
      }
      findMaxScanBytes = megabytes * 1024 * 1024;
    }
    else if (arg == "--size")
    {
      if (i + 1 >= argc || !parseSize(argv[++i], size))
      {
        std::cerr << "--size requires a positive integer byte count\n";
        return 64;
      }
    }
    else if (arg == "--dump-out")
    {
      if (i + 1 >= argc)
      {
        std::cerr << "--dump-out requires a path\n";
        return 64;
      }
      dumpOut = argv[++i];
      readRequested = true;
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
  if (productOverride != Product::Unknown)
    environment.product = productOverride;
  if (!versionOverride.empty())
    environment.version = versionOverride;
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
  if (self)
    keepSelfWritableFindFixtureAlive();

  const RuntimeProcessOpenResult open = openRuntimeProcess(environment);
  std::cout << "memory.opened=" << (open.opened ? "true" : "false") << '\n';
  if (!open.reason.empty())
    std::cout << "memory.open.reason=" << open.reason << '\n';
  const RuntimeMemoryAccessResult access = openProcessMemoryAccess(environment.processId);
  std::cout << "memory.accessible=" << (access.accessible ? "true" : "false") << '\n';
  if (!access.reason.empty())
    std::cout << "memory.access.reason=" << access.reason << '\n';

  if (processStateRequested)
  {
    const RuntimeProcessStateResult state = inspectRuntimeProcessState(environment.processId);
    std::cout << "process.state.inspected=" << (state.inspected ? "true" : "false") << '\n';
    std::cout << "process.state.suspended=" << (state.suspended ? "true" : "false") << '\n';
    std::cout << "process.state.status=" << state.status << '\n';
    std::cout << "process.state.thread_count=" << state.threadCount << '\n';
    if (!state.reason.empty())
      std::cout << "process.state.reason=" << state.reason << '\n';
  }

  if (regionSummaryRequested)
  {
    RuntimeMemoryRegionListResult regions = listProcessMemoryRegions(environment.processId);
    std::cout << "memory.region_summary.requested=true\n";
    std::cout << "memory.region_summary.success=" << (regions.success ? "true" : "false") << '\n';
    if (!regions.reason.empty())
      std::cout << "memory.region_summary.reason=" << regions.reason << '\n';
    std::size_t readableRegions = 0;
    std::size_t writableRegions = 0;
    std::size_t executableRegions = 0;
    std::size_t readableNonExecutableRegions = 0;
    std::size_t readableNonExecutableBytes = 0;
    for (const RuntimeMemoryRegion& region : regions.regions)
    {
      if (region.readable)
        ++readableRegions;
      if (region.writable)
        ++writableRegions;
      if (region.executable)
        ++executableRegions;
      if (region.readable && !region.executable)
      {
        ++readableNonExecutableRegions;
        readableNonExecutableBytes += region.size;
      }
    }
    std::cout << "memory.region_summary.total_regions=" << regions.regions.size() << '\n';
    std::cout << "memory.region_summary.readable_regions=" << readableRegions << '\n';
    std::cout << "memory.region_summary.writable_regions=" << writableRegions << '\n';
    std::cout << "memory.region_summary.executable_regions=" << executableRegions << '\n';
    std::cout << "memory.region_summary.readable_non_executable_regions="
              << readableNonExecutableRegions << '\n';
    std::cout << "memory.region_summary.readable_non_executable_bytes="
              << readableNonExecutableBytes << '\n';
  }

  if (findRequested)
  {
    RuntimeMemoryRegionListResult regions = listProcessMemoryRegions(environment.processId);
    std::cout << "memory.find.requested=true\n";
    std::cout << "memory.find.scan_success=" << (regions.success ? "true" : "false") << '\n';
    if (!regions.reason.empty())
      std::cout << "memory.find.reason=" << regions.reason << '\n';
    if (!regions.success)
    {
      std::cout << "memory.find.success=false\n";
      if (requireOpen && !open.opened)
        return 3;
      if (requireAccess && !access.accessible)
        return 4;
      return requireRead ? 5 : 0;
    }

    std::vector<unsigned char> needle;
    if (findU64Requested)
    {
      needle.resize(sizeof(findU64));
      std::memcpy(needle.data(), &findU64, sizeof(findU64));
    }
    else
    {
      needle.assign(findAscii.begin(), findAscii.end());
    }
    if (needle.empty())
    {
      std::cerr << "find needle must be non-empty\n";
      return 64;
    }
    constexpr std::size_t maxRegionReadBytes = 32 * 1024 * 1024;
    constexpr std::size_t maxPrintedMatches = 128;
    std::size_t scannedBytes = 0;
    std::size_t scannedRegions = 0;
    std::size_t candidateRegions = 0;
    std::size_t skippedWritableFilter = 0;
    std::size_t skippedExecutableFilter = 0;
    std::size_t matchCount = 0;

    for (const RuntimeMemoryRegion& region : regions.regions)
    {
      if (!region.readable || region.size < needle.size() || scannedBytes >= findMaxScanBytes)
        continue;
      if (findWritableOnly && !region.writable)
      {
        ++skippedWritableFilter;
        continue;
      }
      if (findNonExecutableOnly && region.executable)
      {
        ++skippedExecutableFilter;
        continue;
      }

      ++candidateRegions;

      const std::size_t bytesToRead = std::min(
        region.size,
        std::min(maxRegionReadBytes, findMaxScanBytes - scannedBytes));
      RuntimeMemoryReadResult read = readProcessMemory(environment.processId, region.address, bytesToRead);
      if (!read.success || read.bytesRead < needle.size())
        continue;

      ++scannedRegions;
      scannedBytes += read.bytesRead;
      auto begin = read.bytes.begin();
      while (true)
      {
        auto match = std::search(begin, read.bytes.end(), needle.begin(), needle.end());
        if (match == read.bytes.end())
          break;

        const std::size_t offset = static_cast<std::size_t>(std::distance(read.bytes.begin(), match));
        if (matchCount < maxPrintedMatches)
        {
          std::cout << "memory.find.match." << matchCount << ".address=0x"
                    << std::hex << (region.address + offset) << std::dec << '\n';
          std::cout << "memory.find.match." << matchCount << ".region.address=0x"
                    << std::hex << region.address << std::dec << '\n';
          std::cout << "memory.find.match." << matchCount << ".region.size=" << region.size << '\n';
          std::cout << "memory.find.match." << matchCount << ".region.readable="
                    << (region.readable ? "true" : "false") << '\n';
          std::cout << "memory.find.match." << matchCount << ".region.writable="
                    << (region.writable ? "true" : "false") << '\n';
          std::cout << "memory.find.match." << matchCount << ".region.executable="
                    << (region.executable ? "true" : "false") << '\n';
        }
        ++matchCount;
        begin = match + 1;
      }
    }

    if (findU64Requested)
      std::cout << "memory.find.needle.u64=0x" << std::hex << findU64 << std::dec << '\n';
    else
      std::cout << "memory.find.needle=" << findAscii << '\n';
    std::cout << "memory.find.filter.writable_only=" << (findWritableOnly ? "true" : "false") << '\n';
    std::cout << "memory.find.filter.non_executable_only=" << (findNonExecutableOnly ? "true" : "false") << '\n';
    std::cout << "memory.find.candidate_regions=" << candidateRegions << '\n';
    std::cout << "memory.find.skipped_writable_filter=" << skippedWritableFilter << '\n';
    std::cout << "memory.find.skipped_executable_filter=" << skippedExecutableFilter << '\n';
    std::cout << "memory.find.scanned_regions=" << scannedRegions << '\n';
    std::cout << "memory.find.scanned_bytes=" << scannedBytes << '\n';
    std::cout << "memory.find.match.count=" << matchCount << '\n';
    std::cout << "memory.find.match.printed=" << std::min(matchCount, maxPrintedMatches) << '\n';
    std::cout << "memory.find.success=" << (matchCount > 0 ? "true" : "false") << '\n';
    if (requireFind && matchCount == 0)
      return 7;
  }

  if (!readRequested)
  {
    std::cout << "memory.read.requested=false\n";
    if (requireOpen && !open.opened)
      return 3;
    if (requireAccess && !access.accessible)
      return 4;
    return 0;
  }

  if (readFirstReadable)
  {
    RuntimeMemoryRegionResult region = findFirstReadableProcessMemoryRegion(environment.processId);
    std::cout << "memory.read.region.found=" << (region.found ? "true" : "false") << '\n';
    if (region.found)
    {
      address = region.address;
      std::cout << "memory.read.region.address=0x" << std::hex << region.address << std::dec << '\n';
      std::cout << "memory.read.region.size=" << region.size << '\n';
    }
    if (!region.reason.empty())
      std::cout << "memory.read.region.reason=" << region.reason << '\n';
    if (!region.found)
    {
      if (requireOpen && !open.opened)
        return 3;
      if (requireAccess && !access.accessible)
        return 4;
      return requireRead ? 5 : 0;
    }
  }

  RuntimeMemoryReadResult read = readProcessMemory(environment.processId, address, size);
  std::cout << "memory.read.requested=true\n";
  std::cout << "memory.read.success=" << (read.success ? "true" : "false") << '\n';
  std::cout << "memory.read.bytes=" << read.bytesRead << '\n';
  if (!read.reason.empty())
    std::cout << "memory.read.reason=" << read.reason << '\n';
  if (read.success)
  {
    std::cout << "memory.read.hex=" << hexBytes(read.bytes) << '\n';
    if (!dumpOut.empty())
    {
      std::ofstream output(dumpOut, std::ios::binary);
      if (!output)
      {
        std::cerr << "unable to open dump output: " << dumpOut << '\n';
        return 6;
      }
      output.write(
        reinterpret_cast<const char*>(read.bytes.data()),
        static_cast<std::streamsize>(read.bytes.size()));
      if (!output)
      {
        std::cerr << "unable to write dump output: " << dumpOut << '\n';
        return 6;
      }
      std::cout << "memory.dump.path=" << dumpOut << '\n';
      std::cout << "memory.dump.bytes=" << read.bytes.size() << '\n';
    }
  }

  if (requireOpen && !open.opened)
    return 3;
  if (requireAccess && !access.accessible)
    return 4;
  if (requireRead && !read.success)
    return 5;
  return 0;
}
