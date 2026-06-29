#include <BWAPI/Runtime/RuntimeBackend.h>
#include <BWAPI/Runtime/RuntimeInstallation.h>
#include <BWAPI/Runtime/RuntimeProcess.h>
#include <BWAPI/Runtime/RuntimeProcessMemory.h>

#include <cstdlib>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace BWAPI::Runtime;

namespace
{
  volatile char selfWritableFindFixture[] = "starcraft-api-memory-probe-self-fixture";
  volatile std::uint32_t selfWritableU32FindFixture = 0x5ca1ab1e;

  struct FindNeedle
  {
    std::string kind;
    std::string printable;
    std::vector<unsigned char> bytes;
    std::size_t matchCount = 0;
  };

  struct RipXrefNeedle
  {
    std::uintptr_t target = 0;
    std::size_t matchCount = 0;
  };

  struct CodeAnalysisRequest
  {
    std::uintptr_t address = 0;
  };

  struct CounterSnapshot
  {
    std::uintptr_t address = 0;
    std::vector<unsigned char> bytes;
    RuntimeMemoryRegion region;
  };

  struct CounterCandidate
  {
    std::uintptr_t address = 0;
    std::uint32_t first = 0;
    std::uint32_t second = 0;
    std::uint32_t third = 0;
    RuntimeMemoryRegion region;
    int score = 0;
  };

  struct DiffSnapshot
  {
    std::uintptr_t address = 0;
    std::vector<unsigned char> bytes;
    RuntimeMemoryRegion region;
  };

  struct DiffRange
  {
    std::uintptr_t address = 0;
    std::size_t size = 0;
    std::vector<unsigned char> before;
    std::vector<unsigned char> after;
    RuntimeMemoryRegion region;
  };

  void keepSelfWritableFindFixtureAlive()
  {
    const char first = selfWritableFindFixture[0];
    selfWritableFindFixture[0] = first;
    selfWritableU32FindFixture ^= 0;
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
      << "  --region-list            print readable/writable/executable memory regions\n"
      << "  --region-list-limit <n>  maximum regions printed by --region-list (default: 64)\n"
      << "  --region-around <addr>   print only the region containing addr\n"
      << "  --region-path-contains <text>\n"
      << "                           print only regions whose mapped path contains text\n"
      << "  --region-readable-only   print only readable regions in --region-list\n"
      << "  --region-writable-only   print only writable regions in --region-list\n"
      << "  --region-executable-only print only executable regions in --region-list\n"
      << "  --find-ascii <text>      scan readable target memory for an ASCII string\n"
      << "  --find-u32 <value>       scan readable target memory for a 32-bit little-endian value\n"
      << "  --find-u64 <value>       scan readable target memory for a 64-bit little-endian value\n"
      << "                           repeat --find-* to scan multiple needles in one pass\n"
      << "  --find-max-scan-mb <mb>  maximum readable memory scanned by --find-* (default: 128)\n"
      << "  --find-writable-only     scan only readable+writable memory regions\n"
      << "  --find-non-executable-only\n"
      << "                           scan only readable non-executable memory regions\n"
      << "  --find-target-executable-only\n"
      << "                           scan only regions mapped from --executable\n"
      << "  --find-rip-xrefs <addr> scan live executable memory for RIP-relative references\n"
      << "                           to addr; repeatable\n"
      << "  --analyze-code-around <addr>\n"
      << "                           decode direct calls/jumps and common RIP-relative refs\n"
      << "                           around addr; repeatable\n"
      << "  --code-before <bytes>    bytes before each code analysis address (default: 96)\n"
      << "  --code-size <bytes>      bytes read for each code analysis window (default: 512)\n"
      << "  --code-event-limit <n>   maximum decoded refs printed per request (default: 256)\n"
      << "  --code-compact          omit target region detail from code analysis events\n"
      << "  --diff-memory           snapshot readable memory, wait, and report changed byte ranges\n"
      << "  --diff-address <addr>    diff an explicit address range instead of scanning regions\n"
      << "  --diff-size <bytes>      byte count for --diff-address (default: --size)\n"
      << "  --diff-delay-ms <ms>     delay between diff snapshots (default: 3000)\n"
      << "  --diff-max-scan-mb <mb>  maximum diff snapshot bytes (default: --find-max-scan-mb)\n"
      << "  --diff-result-limit <n>  maximum changed ranges printed (default: 128)\n"
      << "  --diff-out <path>        write all changed ranges to a TSV file\n"
      << "  --diff-compact          omit repeated region detail from diff ranges\n"
      << "  --scan-u32-counters      scan readable memory for increasing u32 counters\n"
      << "  --counter-sample-delay-ms <ms>\n"
      << "                           delay between counter samples (default: 1000)\n"
      << "  --counter-max-scan-mb <mb>\n"
      << "                           maximum counter scan bytes (default: --find-max-scan-mb)\n"
      << "  --counter-result-limit <n>\n"
      << "                           maximum counter candidates printed (default: 32)\n"
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

  template <typename T>
  std::vector<unsigned char> littleEndianBytes(T value)
  {
    std::vector<unsigned char> bytes(sizeof(value));
    std::memcpy(bytes.data(), &value, sizeof(value));
    return bytes;
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

  std::string normalizedPathForCompare(const std::string& path)
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

  bool sameMappedFile(const std::string& lhs, const std::string& rhs)
  {
    if (lhs.empty() || rhs.empty())
      return false;
    return normalizedPathForCompare(lhs) == normalizedPathForCompare(rhs);
  }

  std::string regionShareModeName(int shareMode)
  {
    switch (shareMode)
    {
      case 1:
        return "cow";
      case 2:
        return "private";
      case 3:
        return "empty";
      case 4:
        return "shared";
      case 5:
        return "true-shared";
      case 6:
        return "private-aliased";
      case 7:
        return "shared-aliased";
      case 8:
        return "large-page";
      default:
        return "unknown";
    }
  }

  std::uint32_t readU32(const std::vector<unsigned char>& bytes, std::size_t offset)
  {
    std::uint32_t value = 0;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
  }

  std::int32_t readS32(const std::vector<unsigned char>& bytes, std::size_t offset)
  {
    std::int32_t value = 0;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
  }

  bool regionContains(
    const RuntimeMemoryRegion& region,
    std::uintptr_t address,
    std::size_t size)
  {
    if (address < region.address)
      return false;
    const std::uintptr_t offset = address - region.address;
    return offset <= region.size && size <= region.size - offset;
  }

  const RuntimeMemoryRegion* findRegionContaining(
    const std::vector<RuntimeMemoryRegion>& regions,
    std::uintptr_t address,
    std::size_t size)
  {
    for (const RuntimeMemoryRegion& region : regions)
    {
      if (regionContains(region, address, size))
        return &region;
    }
    return nullptr;
  }

  std::string asciiPreview(const std::vector<unsigned char>& bytes)
  {
    std::string preview;
    for (unsigned char byte : bytes)
    {
      if (byte == 0)
        break;
      if (byte < 0x20 || byte > 0x7e)
      {
        if (preview.size() >= 4)
          break;
        preview.clear();
        break;
      }
      preview.push_back(static_cast<char>(byte));
      if (preview.size() >= 80)
        break;
    }
    return preview.size() >= 4 ? preview : std::string {};
  }

  std::string codeBytesHex(
    const std::vector<unsigned char>& bytes,
    std::size_t offset,
    std::size_t length)
  {
    std::ostringstream output;
    const std::size_t end = std::min(bytes.size(), offset + length);
    for (std::size_t i = offset; i < end; ++i)
    {
      if (i > offset)
        output << ' ';
      output << std::hex << std::setw(2) << std::setfill('0')
             << static_cast<int>(bytes[i]);
    }
    return output.str();
  }

  std::string classifyRipRef(
    const std::vector<unsigned char>& bytes,
    std::size_t offset)
  {
    const unsigned char first = bytes[offset];
    const unsigned char second = offset + 1 < bytes.size() ? bytes[offset + 1] : 0;
    const unsigned char third = offset + 2 < bytes.size() ? bytes[offset + 2] : 0;

    if (first == 0xe8)
      return "call-rel32";
    if (first == 0xe9)
      return "jmp-rel32";
    if (first == 0x0f && second >= 0x80 && second <= 0x8f)
      return "jcc-rel32";
    if (first >= 0x40 && first <= 0x4f)
    {
      if (second == 0x8d)
        return "lea-rip";
      if (second == 0x8b)
        return "mov-load-rip";
      if (second == 0x89)
        return "mov-store-rip";
      if (second == 0x39 || second == 0x3b)
        return "cmp-rip";
      if (second == 0xc6 || second == 0xc7)
        return "mov-imm-store-rip";
      if (second == 0x80 || second == 0x81 || second == 0x83)
        return "op-imm-rip";
      if (second == 0x0f && (third == 0xb6 || third == 0xb7 || third == 0xbe || third == 0xbf))
        return "movx-rip";
    }
    if (first == 0x8d)
      return "lea-rip";
    if (first == 0x8b)
      return "mov-load-rip";
    if (first == 0x89)
      return "mov-store-rip";
    if (first == 0x39 || first == 0x3b)
      return "cmp-rip";
    if (first == 0xc6 || first == 0xc7)
      return "mov-imm-store-rip";
    if (first == 0x80 || first == 0x81 || first == 0x83)
      return "op-imm-rip";
    return "rip-rel32";
  }

  bool decodeCodeReference(
    const std::vector<unsigned char>& bytes,
    std::size_t offset,
    std::uintptr_t instructionAddress,
    std::uintptr_t& target,
    std::size_t& instructionLength,
    std::string& kind)
  {
    if (offset + 5 > bytes.size())
      return false;

    const unsigned char first = bytes[offset];
    const unsigned char second = offset + 1 < bytes.size() ? bytes[offset + 1] : 0;
    const unsigned char third = offset + 2 < bytes.size() ? bytes[offset + 2] : 0;
    auto resolveRel = [&](std::size_t displacementOffset, std::size_t length)
    {
      if (offset + displacementOffset + sizeof(std::int32_t) > bytes.size())
        return false;
      const std::int32_t displacement = readS32(bytes, offset + displacementOffset);
      instructionLength = length;
      target = static_cast<std::uintptr_t>(
        static_cast<std::int64_t>(instructionAddress + instructionLength) + displacement);
      kind = classifyRipRef(bytes, offset);
      return true;
    };

    if (first == 0xe8 || first == 0xe9)
      return resolveRel(1, 5);
    if (first == 0x0f && second >= 0x80 && second <= 0x8f)
      return resolveRel(2, 6);

    auto modRmIsRipRelative = [](unsigned char modRm)
    {
      return (modRm & 0xc7) == 0x05;
    };
    auto immediateSize = [](unsigned char opcode)
    {
      if (opcode == 0xc6 || opcode == 0x80 || opcode == 0x83)
        return std::size_t { 1 };
      if (opcode == 0xc7 || opcode == 0x81)
        return std::size_t { 4 };
      return std::size_t { 0 };
    };

    if ((first == 0x8b || first == 0x89 || first == 0x8d
          || first == 0x39 || first == 0x3b)
        && offset + 6 <= bytes.size()
        && modRmIsRipRelative(second))
      return resolveRel(2, 6);
    if ((first == 0xc6 || first == 0xc7 || first == 0x80
          || first == 0x81 || first == 0x83)
        && offset + 6 + immediateSize(first) <= bytes.size()
        && modRmIsRipRelative(second))
      return resolveRel(2, 6 + immediateSize(first));

    if (first >= 0x40 && first <= 0x4f && offset + 7 <= bytes.size())
    {
      if ((second == 0x8b || second == 0x89 || second == 0x8d
            || second == 0x39 || second == 0x3b)
          && modRmIsRipRelative(third))
        return resolveRel(3, 7);
      if ((second == 0xc6 || second == 0xc7 || second == 0x80
            || second == 0x81 || second == 0x83)
          && offset + 7 + immediateSize(second) <= bytes.size()
          && modRmIsRipRelative(third))
        return resolveRel(3, 7 + immediateSize(second));
      if (second == 0x0f
          && (third == 0xb6 || third == 0xb7 || third == 0xbe || third == 0xbf)
          && offset + 8 <= bytes.size()
          && modRmIsRipRelative(bytes[offset + 3]))
        return resolveRel(4, 8);
    }

    return false;
  }

  std::string ripXrefInstructionHint(const std::vector<unsigned char>& bytes, std::size_t offset)
  {
    if (offset >= bytes.size())
      return "unknown";

    const unsigned char first = bytes[offset];
    const unsigned char second = offset + 1 < bytes.size() ? bytes[offset + 1] : 0;
    const unsigned char third = offset + 2 < bytes.size() ? bytes[offset + 2] : 0;
    if (first == 0xe8)
      return "call-rel32";
    if (first == 0xe9)
      return "jmp-rel32";
    if ((first == 0x48 || first == 0x4c) && second == 0x8d)
      return "lea-rip";
    if ((first == 0x48 || first == 0x4c) && second == 0x8b)
      return "mov-rip";
    if (first == 0x8b || first == 0x89)
      return "mov-rip";
    if (first == 0x0f && (second >= 0x80 && second <= 0x8f))
      return "jcc-rel32";
    if ((first == 0x48 || first == 0x4c) && second == 0x0f && (third == 0xb6 || third == 0xb7))
      return "movzx-rip";
    return "rip-rel32";
  }

  bool plausibleCounterDelta(std::uint32_t before, std::uint32_t after)
  {
    if (after <= before)
      return false;
    return after - before <= 10000;
  }

  int counterScore(
    std::uint32_t first,
    std::uint32_t second,
    std::uint32_t third,
    int sampleDelayMs)
  {
    const int firstDelta = static_cast<int>(second - first);
    const int secondDelta = static_cast<int>(third - second);
    const int expectedDelta = std::max(1, (sampleDelayMs * 24) / 1000);
    const int minimumFrameLikeDelta = std::max(2, expectedDelta / 3);
    const int maximumFrameLikeDelta = std::max(12, expectedDelta * 4);
    const bool frameLike =
      firstDelta >= minimumFrameLikeDelta
      && secondDelta >= minimumFrameLikeDelta
      && firstDelta <= maximumFrameLikeDelta
      && secondDelta <= maximumFrameLikeDelta;
    return (frameLike ? 0 : 100000)
      + std::abs(firstDelta - expectedDelta)
      + std::abs(secondDelta - expectedDelta)
      + std::abs(firstDelta - secondDelta);
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
  bool regionListRequested = false;
  bool findRequested = false;
  bool findWritableOnly = false;
  bool findNonExecutableOnly = false;
  bool findTargetExecutableOnly = false;
  bool ripXrefRequested = false;
  bool codeAnalysisRequested = false;
  bool codeAnalysisCompact = false;
  bool diffMemoryRequested = false;
  bool diffCompact = false;
  bool counterScanRequested = false;
  bool regionReadableOnly = false;
  bool regionWritableOnly = false;
  bool regionExecutableOnly = false;
  bool requireFind = false;
  bool self = false;
  int processIdOverride = 0;
  std::uintptr_t address = 0;
  std::uintptr_t diffAddress = 0;
  std::uintptr_t regionAroundAddress = 0;
  std::size_t size = 16;
  std::size_t diffSize = 0;
  std::size_t regionListLimit = 64;
  std::size_t findMaxScanBytes = 128 * 1024 * 1024;
  std::size_t counterMaxScanBytes = 0;
  std::size_t counterResultLimit = 32;
  std::size_t codeAnalysisBeforeBytes = 96;
  std::size_t codeAnalysisSize = 512;
  std::size_t codeAnalysisEventLimit = 256;
  std::size_t diffMaxScanBytes = 0;
  std::size_t diffResultLimit = 128;
  int diffDelayMs = 3000;
  int counterSampleDelayMs = 1000;
  Product productOverride = Product::Unknown;
  std::string versionOverride;
  std::string executableOverride;
  std::string dumpOut;
  std::string diffOutPath;
  std::string regionPathContains;
  std::vector<FindNeedle> findNeedles;
  std::vector<RipXrefNeedle> ripXrefNeedles;
  std::vector<CodeAnalysisRequest> codeAnalysisRequests;

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
    else if (arg == "--region-list")
    {
      regionListRequested = true;
    }
    else if (arg == "--region-list-limit")
    {
      if (i + 1 >= argc || !parseSize(argv[++i], regionListLimit))
      {
        std::cerr << "--region-list-limit requires a positive integer\n";
        return 64;
      }
      regionListRequested = true;
    }
    else if (arg == "--region-around")
    {
      if (i + 1 >= argc || !parseAddress(argv[++i], regionAroundAddress))
      {
        std::cerr << "--region-around requires a positive integer address\n";
        return 64;
      }
      regionListRequested = true;
    }
    else if (arg == "--region-path-contains")
    {
      if (i + 1 >= argc)
      {
        std::cerr << "--region-path-contains requires a value\n";
        return 64;
      }
      regionPathContains = argv[++i];
      if (regionPathContains.empty())
      {
        std::cerr << "--region-path-contains requires a non-empty value\n";
        return 64;
      }
      regionListRequested = true;
    }
    else if (arg == "--region-readable-only")
    {
      regionReadableOnly = true;
      regionListRequested = true;
    }
    else if (arg == "--region-writable-only")
    {
      regionWritableOnly = true;
      regionListRequested = true;
    }
    else if (arg == "--region-executable-only")
    {
      regionExecutableOnly = true;
      regionListRequested = true;
    }
    else if (arg == "--find-ascii")
    {
      if (i + 1 >= argc)
      {
        std::cerr << "--find-ascii requires a value\n";
        return 64;
      }
      std::string value = argv[++i];
      FindNeedle needle;
      needle.kind = "ascii";
      needle.printable = value;
      needle.bytes.assign(value.begin(), value.end());
      findNeedles.push_back(std::move(needle));
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
      const std::uint64_t value = static_cast<std::uint64_t>(parsed);
      std::ostringstream printable;
      printable << "0x" << std::hex << value;
      FindNeedle needle;
      needle.kind = "u64";
      needle.printable = printable.str();
      needle.bytes = littleEndianBytes(value);
      findNeedles.push_back(std::move(needle));
      findRequested = true;
    }
    else if (arg == "--find-u32")
    {
      std::uintptr_t parsed = 0;
      if (i + 1 >= argc || !parseAddress(argv[++i], parsed) || parsed > std::numeric_limits<std::uint32_t>::max())
      {
        std::cerr << "--find-u32 requires a positive 32-bit integer value\n";
        return 64;
      }
      const std::uint32_t value = static_cast<std::uint32_t>(parsed);
      std::ostringstream printable;
      printable << "0x" << std::hex << value;
      FindNeedle needle;
      needle.kind = "u32";
      needle.printable = printable.str();
      needle.bytes = littleEndianBytes(value);
      findNeedles.push_back(std::move(needle));
      findRequested = true;
    }
    else if (arg == "--find-writable-only")
    {
      findWritableOnly = true;
    }
    else if (arg == "--find-non-executable-only")
    {
      findNonExecutableOnly = true;
    }
    else if (arg == "--find-target-executable-only")
    {
      findTargetExecutableOnly = true;
    }
    else if (arg == "--find-rip-xrefs")
    {
      std::uintptr_t parsed = 0;
      if (i + 1 >= argc || !parseAddress(argv[++i], parsed))
      {
        std::cerr << "--find-rip-xrefs requires a positive integer address\n";
        return 64;
      }
      RipXrefNeedle needle;
      needle.target = parsed;
      ripXrefNeedles.push_back(needle);
      ripXrefRequested = true;
    }
    else if (arg == "--analyze-code-around")
    {
      std::uintptr_t parsed = 0;
      if (i + 1 >= argc || !parseAddress(argv[++i], parsed))
      {
        std::cerr << "--analyze-code-around requires a positive integer address\n";
        return 64;
      }
      CodeAnalysisRequest request;
      request.address = parsed;
      codeAnalysisRequests.push_back(request);
      codeAnalysisRequested = true;
    }
    else if (arg == "--code-before")
    {
      if (i + 1 >= argc || !parseSize(argv[++i], codeAnalysisBeforeBytes))
      {
        std::cerr << "--code-before requires a positive integer byte count\n";
        return 64;
      }
    }
    else if (arg == "--code-size")
    {
      if (i + 1 >= argc || !parseSize(argv[++i], codeAnalysisSize))
      {
        std::cerr << "--code-size requires a positive integer byte count\n";
        return 64;
      }
    }
    else if (arg == "--code-event-limit")
    {
      if (i + 1 >= argc || !parseSize(argv[++i], codeAnalysisEventLimit))
      {
        std::cerr << "--code-event-limit requires a positive integer count\n";
        return 64;
      }
    }
    else if (arg == "--code-compact")
    {
      codeAnalysisCompact = true;
    }
    else if (arg == "--diff-memory")
    {
      diffMemoryRequested = true;
    }
    else if (arg == "--diff-address")
    {
      if (i + 1 >= argc || !parseAddress(argv[++i], diffAddress))
      {
        std::cerr << "--diff-address requires a positive integer address\n";
        return 64;
      }
      diffMemoryRequested = true;
    }
    else if (arg == "--diff-size")
    {
      if (i + 1 >= argc || !parseSize(argv[++i], diffSize))
      {
        std::cerr << "--diff-size requires a positive integer byte count\n";
        return 64;
      }
      diffMemoryRequested = true;
    }
    else if (arg == "--diff-delay-ms")
    {
      if (i + 1 >= argc || !parsePositiveInt(argv[++i], diffDelayMs))
      {
        std::cerr << "--diff-delay-ms requires a positive integer millisecond count\n";
        return 64;
      }
      diffMemoryRequested = true;
    }
    else if (arg == "--diff-max-scan-mb")
    {
      std::size_t megabytes = 0;
      if (i + 1 >= argc || !parseSize(argv[++i], megabytes))
      {
        std::cerr << "--diff-max-scan-mb requires a positive integer megabyte count\n";
        return 64;
      }
      diffMaxScanBytes = megabytes * 1024 * 1024;
      diffMemoryRequested = true;
    }
    else if (arg == "--diff-result-limit")
    {
      if (i + 1 >= argc || !parseSize(argv[++i], diffResultLimit))
      {
        std::cerr << "--diff-result-limit requires a positive integer count\n";
        return 64;
      }
      diffMemoryRequested = true;
    }
    else if (arg == "--diff-out")
    {
      if (i + 1 >= argc)
      {
        std::cerr << "--diff-out requires a path\n";
        return 64;
      }
      diffOutPath = argv[++i];
      diffMemoryRequested = true;
    }
    else if (arg == "--diff-compact")
    {
      diffCompact = true;
      diffMemoryRequested = true;
    }
    else if (arg == "--scan-u32-counters")
    {
      counterScanRequested = true;
    }
    else if (arg == "--counter-sample-delay-ms")
    {
      if (i + 1 >= argc || !parsePositiveInt(argv[++i], counterSampleDelayMs))
      {
        std::cerr << "--counter-sample-delay-ms requires a positive integer millisecond count\n";
        return 64;
      }
      counterScanRequested = true;
    }
    else if (arg == "--counter-max-scan-mb")
    {
      std::size_t megabytes = 0;
      if (i + 1 >= argc || !parseSize(argv[++i], megabytes))
      {
        std::cerr << "--counter-max-scan-mb requires a positive integer megabyte count\n";
        return 64;
      }
      counterMaxScanBytes = megabytes * 1024 * 1024;
      counterScanRequested = true;
    }
    else if (arg == "--counter-result-limit")
    {
      if (i + 1 >= argc || !parseSize(argv[++i], counterResultLimit))
      {
        std::cerr << "--counter-result-limit requires a positive integer\n";
        return 64;
      }
      counterScanRequested = true;
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
    std::size_t mappedFileRegions = 0;
    std::size_t targetExecutableMappedRegions = 0;
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
      if (!region.mappedPath.empty())
        ++mappedFileRegions;
      if (sameMappedFile(region.mappedPath, environment.executablePath))
        ++targetExecutableMappedRegions;
    }
    std::cout << "memory.region_summary.total_regions=" << regions.regions.size() << '\n';
    std::cout << "memory.region_summary.readable_regions=" << readableRegions << '\n';
    std::cout << "memory.region_summary.writable_regions=" << writableRegions << '\n';
    std::cout << "memory.region_summary.executable_regions=" << executableRegions << '\n';
    std::cout << "memory.region_summary.readable_non_executable_regions="
              << readableNonExecutableRegions << '\n';
    std::cout << "memory.region_summary.readable_non_executable_bytes="
              << readableNonExecutableBytes << '\n';
    std::cout << "memory.region_summary.mapped_file_regions=" << mappedFileRegions << '\n';
    std::cout << "memory.region_summary.target_executable_mapped_regions="
              << targetExecutableMappedRegions << '\n';
  }

  if (regionListRequested)
  {
    RuntimeMemoryRegionListResult regions = listProcessMemoryRegions(environment.processId);
    std::cout << "memory.region_list.requested=true\n";
    std::cout << "memory.region_list.success=" << (regions.success ? "true" : "false") << '\n';
    if (!regions.reason.empty())
      std::cout << "memory.region_list.reason=" << regions.reason << '\n';
    std::cout << "memory.region_list.filter.address=0x"
              << std::hex << regionAroundAddress << std::dec << '\n';
    if (!regionPathContains.empty())
      std::cout << "memory.region_list.filter.path_contains="
                << regionPathContains << '\n';
    std::cout << "memory.region_list.filter.readable_only="
              << (regionReadableOnly ? "true" : "false") << '\n';
    std::cout << "memory.region_list.filter.writable_only="
              << (regionWritableOnly ? "true" : "false") << '\n';
    std::cout << "memory.region_list.filter.executable_only="
              << (regionExecutableOnly ? "true" : "false") << '\n';
    if (regions.success)
    {
      std::size_t printed = 0;
      std::size_t matched = 0;
      for (const RuntimeMemoryRegion& region : regions.regions)
      {
        const bool containsAddress =
          regionAroundAddress != 0
          && region.address <= regionAroundAddress
          && regionAroundAddress - region.address < region.size;
        if (regionAroundAddress != 0 && !containsAddress)
          continue;
        if (!regionPathContains.empty()
            && region.mappedPath.find(regionPathContains) == std::string::npos)
          continue;
        if (regionReadableOnly && !region.readable)
          continue;
        if (regionWritableOnly && !region.writable)
          continue;
        if (regionExecutableOnly && !region.executable)
          continue;
        ++matched;
        if (printed >= regionListLimit)
          continue;
        std::cout << "memory.region_list.region." << printed << ".address=0x"
                  << std::hex << region.address << std::dec << '\n';
        std::cout << "memory.region_list.region." << printed << ".end=0x"
                  << std::hex << (region.address + region.size) << std::dec << '\n';
        std::cout << "memory.region_list.region." << printed << ".size=" << region.size << '\n';
        std::cout << "memory.region_list.region." << printed << ".readable="
                  << (region.readable ? "true" : "false") << '\n';
        std::cout << "memory.region_list.region." << printed << ".writable="
                  << (region.writable ? "true" : "false") << '\n';
        std::cout << "memory.region_list.region." << printed << ".executable="
                  << (region.executable ? "true" : "false") << '\n';
        std::cout << "memory.region_list.region." << printed << ".target_executable="
                  << (sameMappedFile(region.mappedPath, environment.executablePath) ? "true" : "false") << '\n';
        std::cout << "memory.region_list.region." << printed << ".user_tag="
                  << region.userTag << '\n';
        std::cout << "memory.region_list.region." << printed << ".share_mode="
                  << region.shareMode << '\n';
        std::cout << "memory.region_list.region." << printed << ".share_mode_name="
                  << regionShareModeName(region.shareMode) << '\n';
        if (!region.mappedPath.empty())
          std::cout << "memory.region_list.region." << printed << ".mapped_path="
                    << region.mappedPath << '\n';
        ++printed;
      }
      std::cout << "memory.region_list.match_count=" << matched << '\n';
      std::cout << "memory.region_list.printed_count=" << printed << '\n';
      std::cout << "memory.region_list.limit=" << regionListLimit << '\n';
    }
  }

  if (codeAnalysisRequested)
  {
    RuntimeMemoryRegionListResult regions = listProcessMemoryRegions(environment.processId);
    std::cout << "memory.code_analysis.requested=true\n";
    std::cout << "memory.code_analysis.scan_success=" << (regions.success ? "true" : "false") << '\n';
    if (!regions.reason.empty())
      std::cout << "memory.code_analysis.reason=" << regions.reason << '\n';
    std::cout << "memory.code_analysis.request_count=" << codeAnalysisRequests.size() << '\n';
    std::cout << "memory.code_analysis.before_bytes=" << codeAnalysisBeforeBytes << '\n';
    std::cout << "memory.code_analysis.window_bytes=" << codeAnalysisSize << '\n';
    std::cout << "memory.code_analysis.event_limit=" << codeAnalysisEventLimit << '\n';
    std::cout << "memory.code_analysis.compact=" << (codeAnalysisCompact ? "true" : "false") << '\n';
    if (regions.success)
    {
      for (std::size_t requestIndex = 0; requestIndex < codeAnalysisRequests.size(); ++requestIndex)
      {
        const CodeAnalysisRequest& request = codeAnalysisRequests[requestIndex];
        const std::uintptr_t start =
          request.address > codeAnalysisBeforeBytes
            ? request.address - codeAnalysisBeforeBytes
            : request.address;
        const RuntimeMemoryRegion* startRegion =
          findRegionContaining(regions.regions, start, 1);
        const std::size_t boundedSize =
          startRegion == nullptr
            ? codeAnalysisSize
            : std::min(
              codeAnalysisSize,
              static_cast<std::size_t>(
                (startRegion->address + startRegion->size) - start));
        RuntimeMemoryReadResult read =
          readProcessMemory(environment.processId, start, boundedSize);
        std::cout << "memory.code_analysis.request." << requestIndex << ".address=0x"
                  << std::hex << request.address << std::dec << '\n';
        std::cout << "memory.code_analysis.request." << requestIndex << ".start=0x"
                  << std::hex << start << std::dec << '\n';
        std::cout << "memory.code_analysis.request." << requestIndex << ".read_success="
                  << (read.success ? "true" : "false") << '\n';
        std::cout << "memory.code_analysis.request." << requestIndex << ".bytes="
                  << read.bytesRead << '\n';
        if (!read.reason.empty())
          std::cout << "memory.code_analysis.request." << requestIndex << ".read_reason="
                    << read.reason << '\n';
        if (startRegion != nullptr)
        {
          std::cout << "memory.code_analysis.request." << requestIndex << ".region.address=0x"
                    << std::hex << startRegion->address << std::dec << '\n';
          std::cout << "memory.code_analysis.request." << requestIndex << ".region.size="
                    << startRegion->size << '\n';
          std::cout << "memory.code_analysis.request." << requestIndex << ".region.executable="
                    << (startRegion->executable ? "true" : "false") << '\n';
          std::cout << "memory.code_analysis.request." << requestIndex << ".region.target_executable="
                    << (sameMappedFile(startRegion->mappedPath, environment.executablePath) ? "true" : "false")
                    << '\n';
        }
        if (!read.success || read.bytesRead < 5)
          continue;

        std::size_t eventCount = 0;
        for (std::size_t offset = 0; offset + 5 <= read.bytes.size(); ++offset)
        {
          std::uintptr_t target = 0;
          std::size_t instructionLength = 0;
          std::string kind;
          const std::uintptr_t instructionAddress = start + offset;
          if (!decodeCodeReference(
                read.bytes,
                offset,
                instructionAddress,
                target,
                instructionLength,
                kind))
            continue;

          if (eventCount < codeAnalysisEventLimit)
          {
            const RuntimeMemoryRegion* targetRegion =
              findRegionContaining(regions.regions, target, 1);
            std::cout << "memory.code_analysis.request." << requestIndex
                      << ".event." << eventCount << ".address=0x"
                      << std::hex << instructionAddress << std::dec << '\n';
            std::cout << "memory.code_analysis.request." << requestIndex
                      << ".event." << eventCount << ".offset="
                      << offset << '\n';
            std::cout << "memory.code_analysis.request." << requestIndex
                      << ".event." << eventCount << ".kind="
                      << kind << '\n';
            std::cout << "memory.code_analysis.request." << requestIndex
                      << ".event." << eventCount << ".target=0x"
                      << std::hex << target << std::dec << '\n';
            std::cout << "memory.code_analysis.request." << requestIndex
                      << ".event." << eventCount << ".bytes="
                      << codeBytesHex(read.bytes, offset, instructionLength) << '\n';
            if (targetRegion != nullptr && !codeAnalysisCompact)
            {
              std::cout << "memory.code_analysis.request." << requestIndex
                        << ".event." << eventCount << ".target_region.address=0x"
                        << std::hex << targetRegion->address << std::dec << '\n';
              std::cout << "memory.code_analysis.request." << requestIndex
                        << ".event." << eventCount << ".target_region.size="
                        << targetRegion->size << '\n';
              std::cout << "memory.code_analysis.request." << requestIndex
                        << ".event." << eventCount << ".target_region.readable="
                        << (targetRegion->readable ? "true" : "false") << '\n';
              std::cout << "memory.code_analysis.request." << requestIndex
                        << ".event." << eventCount << ".target_region.writable="
                        << (targetRegion->writable ? "true" : "false") << '\n';
              std::cout << "memory.code_analysis.request." << requestIndex
                        << ".event." << eventCount << ".target_region.executable="
                        << (targetRegion->executable ? "true" : "false") << '\n';
              std::cout << "memory.code_analysis.request." << requestIndex
                        << ".event." << eventCount << ".target_region.target_executable="
                        << (sameMappedFile(targetRegion->mappedPath, environment.executablePath) ? "true" : "false")
                        << '\n';
              std::cout << "memory.code_analysis.request." << requestIndex
                        << ".event." << eventCount << ".target_region.share_mode_name="
                        << regionShareModeName(targetRegion->shareMode) << '\n';
              if (!targetRegion->mappedPath.empty())
                std::cout << "memory.code_analysis.request." << requestIndex
                          << ".event." << eventCount << ".target_region.mapped_path="
                          << targetRegion->mappedPath << '\n';
              if (targetRegion->readable && !targetRegion->executable)
              {
                RuntimeMemoryReadResult targetRead =
                  readProcessMemory(environment.processId, target, 96);
                if (targetRead.success)
                {
                  const std::string preview = asciiPreview(targetRead.bytes);
                  if (!preview.empty())
                    std::cout << "memory.code_analysis.request." << requestIndex
                              << ".event." << eventCount << ".target_ascii="
                              << preview << '\n';
                }
              }
            }
          }
          ++eventCount;
          if (instructionLength > 1)
            offset += instructionLength - 1;
        }
        std::cout << "memory.code_analysis.request." << requestIndex
                  << ".event_count=" << eventCount << '\n';
        std::cout << "memory.code_analysis.request." << requestIndex
                  << ".event_printed=" << std::min(eventCount, codeAnalysisEventLimit) << '\n';
      }
    }
  }

  if (ripXrefRequested)
  {
    RuntimeMemoryRegionListResult regions = listProcessMemoryRegions(environment.processId);
    std::cout << "memory.rip_xref.requested=true\n";
    std::cout << "memory.rip_xref.scan_success=" << (regions.success ? "true" : "false") << '\n';
    if (!regions.reason.empty())
      std::cout << "memory.rip_xref.reason=" << regions.reason << '\n';
    if (!regions.success)
    {
      if (requireOpen && !open.opened)
        return 3;
      if (requireAccess && !access.accessible)
        return 4;
      return 0;
    }

    constexpr std::size_t maxRegionReadBytes = 32 * 1024 * 1024;
    constexpr std::size_t maxPrintedMatches = 256;
    std::size_t scannedBytes = 0;
    std::size_t scannedRegions = 0;
    std::size_t candidateRegions = 0;
    std::size_t matchCount = 0;

    const auto recordMatch =
      [&](std::uintptr_t instructionAddress,
          std::uintptr_t referenceTarget,
          std::size_t needleIndex,
          const std::string& hint,
          const RuntimeMemoryRegion& region)
      {
        RipXrefNeedle& needle = ripXrefNeedles[needleIndex];
        if (referenceTarget != needle.target)
          return;
        if (matchCount < maxPrintedMatches)
        {
          std::cout << "memory.rip_xref.match." << matchCount << ".needle_index="
                    << needleIndex << '\n';
          std::cout << "memory.rip_xref.match." << matchCount << ".target=0x"
                    << std::hex << needle.target << std::dec << '\n';
          std::cout << "memory.rip_xref.match." << matchCount << ".instruction=0x"
                    << std::hex << instructionAddress << std::dec << '\n';
          std::cout << "memory.rip_xref.match." << matchCount << ".kind=" << hint << '\n';
          std::cout << "memory.rip_xref.match." << matchCount << ".region.address=0x"
                    << std::hex << region.address << std::dec << '\n';
          std::cout << "memory.rip_xref.match." << matchCount << ".region.size="
                    << region.size << '\n';
          std::cout << "memory.rip_xref.match." << matchCount << ".region.target_executable="
                    << (sameMappedFile(region.mappedPath, environment.executablePath) ? "true" : "false")
                    << '\n';
          std::cout << "memory.rip_xref.match." << matchCount << ".region.user_tag="
                    << region.userTag << '\n';
          std::cout << "memory.rip_xref.match." << matchCount << ".region.share_mode="
                    << region.shareMode << '\n';
          std::cout << "memory.rip_xref.match." << matchCount << ".region.share_mode_name="
                    << regionShareModeName(region.shareMode) << '\n';
          if (!region.mappedPath.empty())
            std::cout << "memory.rip_xref.match." << matchCount << ".region.mapped_path="
                      << region.mappedPath << '\n';
        }
        ++needle.matchCount;
        ++matchCount;
      };

    for (const RuntimeMemoryRegion& region : regions.regions)
    {
      if (!region.readable || !region.executable || region.size < 5)
        continue;
      if (!sameMappedFile(region.mappedPath, environment.executablePath))
        continue;
      ++candidateRegions;

      for (std::size_t offset = 0; offset < region.size; offset += maxRegionReadBytes)
      {
        const std::size_t remainingRegionBytes = region.size - offset;
        const std::size_t bytesToRead = std::min(remainingRegionBytes, maxRegionReadBytes);
        RuntimeMemoryReadResult read =
          readProcessMemory(environment.processId, region.address + offset, bytesToRead);
        if (!read.success || read.bytesRead < 5)
          continue;

        ++scannedRegions;
        scannedBytes += read.bytesRead;
        const std::vector<unsigned char>& bytes = read.bytes;
        const std::uintptr_t chunkAddress = region.address + offset;

        for (std::size_t i = 0; i + 5 <= bytes.size(); ++i)
        {
          const std::uintptr_t instructionAddress = chunkAddress + i;
          auto testReference = [&](std::size_t displacementOffset, std::size_t instructionLength)
          {
            if (i + displacementOffset + sizeof(std::int32_t) > bytes.size())
              return;
            const std::int32_t displacement = readS32(bytes, i + displacementOffset);
            const std::uintptr_t target = static_cast<std::uintptr_t>(
              static_cast<std::int64_t>(instructionAddress + instructionLength) + displacement);
            for (std::size_t needleIndex = 0; needleIndex < ripXrefNeedles.size(); ++needleIndex)
              recordMatch(
                instructionAddress,
                target,
                needleIndex,
                ripXrefInstructionHint(bytes, i),
                region);
          };

          const unsigned char first = bytes[i];
          const unsigned char second = i + 1 < bytes.size() ? bytes[i + 1] : 0;
          const unsigned char third = i + 2 < bytes.size() ? bytes[i + 2] : 0;
          if (first == 0xe8 || first == 0xe9)
          {
            testReference(1, 5);
            continue;
          }
          if (first == 0x0f && second >= 0x80 && second <= 0x8f)
          {
            testReference(2, 6);
            continue;
          }

          auto modRmIsRipRelative = [](unsigned char modRm)
          {
            return (modRm & 0xc7) == 0x05;
          };

          if ((first == 0x8b || first == 0x89 || first == 0x8d)
              && i + 6 <= bytes.size()
              && modRmIsRipRelative(second))
          {
            testReference(2, 6);
            continue;
          }

          if (first >= 0x40 && first <= 0x4f && i + 7 <= bytes.size())
          {
            if ((second == 0x8b || second == 0x89 || second == 0x8d)
                && modRmIsRipRelative(third))
            {
              testReference(3, 7);
              continue;
            }
            if (second == 0x0f
                && (third == 0xb6 || third == 0xb7)
                && i + 8 <= bytes.size()
                && modRmIsRipRelative(bytes[i + 3]))
            {
              testReference(4, 8);
              continue;
            }
          }
        }
      }
    }

    std::cout << "memory.rip_xref.needle_count=" << ripXrefNeedles.size() << '\n';
    for (std::size_t index = 0; index < ripXrefNeedles.size(); ++index)
    {
      const RipXrefNeedle& needle = ripXrefNeedles[index];
      std::cout << "memory.rip_xref.needle." << index << ".target=0x"
                << std::hex << needle.target << std::dec << '\n';
      std::cout << "memory.rip_xref.needle." << index << ".match.count="
                << needle.matchCount << '\n';
    }
    std::cout << "memory.rip_xref.candidate_regions=" << candidateRegions << '\n';
    std::cout << "memory.rip_xref.scanned_regions=" << scannedRegions << '\n';
    std::cout << "memory.rip_xref.scanned_bytes=" << scannedBytes << '\n';
    std::cout << "memory.rip_xref.match.count=" << matchCount << '\n';
    std::cout << "memory.rip_xref.match.printed=" << std::min(matchCount, maxPrintedMatches) << '\n';
    std::cout << "memory.rip_xref.success=" << (matchCount > 0 ? "true" : "false") << '\n';
  }

  if (counterScanRequested)
  {
    if (counterMaxScanBytes == 0)
      counterMaxScanBytes = findMaxScanBytes;
    RuntimeMemoryRegionListResult regions = listProcessMemoryRegions(environment.processId);
    std::cout << "memory.counter_scan.requested=true\n";
    std::cout << "memory.counter_scan.scan_success=" << (regions.success ? "true" : "false") << '\n';
    if (!regions.reason.empty())
      std::cout << "memory.counter_scan.reason=" << regions.reason << '\n';
    std::cout << "memory.counter_scan.sample_delay_ms=" << counterSampleDelayMs << '\n';
    std::cout << "memory.counter_scan.max_scan_bytes=" << counterMaxScanBytes << '\n';
    std::cout << "memory.counter_scan.filter.writable_only=" << (findWritableOnly ? "true" : "false") << '\n';
    std::cout << "memory.counter_scan.filter.non_executable_only="
              << (findNonExecutableOnly ? "true" : "false") << '\n';
    std::cout << "memory.counter_scan.filter.target_executable_only="
              << (findTargetExecutableOnly ? "true" : "false") << '\n';
    if (regions.success)
    {
      constexpr std::size_t maxRegionReadBytes = 4 * 1024 * 1024;
      std::size_t scannedBytes = 0;
      std::size_t scannedRegions = 0;
      std::size_t candidateRegions = 0;
      std::size_t skippedWritableFilter = 0;
      std::size_t skippedExecutableFilter = 0;
      std::size_t skippedTargetExecutableFilter = 0;
      std::vector<CounterSnapshot> snapshots;

      for (const RuntimeMemoryRegion& region : regions.regions)
      {
        if (!region.readable || region.size < sizeof(std::uint32_t) || scannedBytes >= counterMaxScanBytes)
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
        if (findTargetExecutableOnly && !sameMappedFile(region.mappedPath, environment.executablePath))
        {
          ++skippedTargetExecutableFilter;
          continue;
        }

        ++candidateRegions;
        for (std::size_t offset = 0; offset < region.size && scannedBytes < counterMaxScanBytes; offset += maxRegionReadBytes)
        {
          const std::size_t remainingRegionBytes = region.size - offset;
          const std::size_t bytesToRead = std::min(
            remainingRegionBytes,
            std::min(maxRegionReadBytes, counterMaxScanBytes - scannedBytes));
          if (bytesToRead < sizeof(std::uint32_t))
            break;
          RuntimeMemoryReadResult read =
            readProcessMemory(environment.processId, region.address + offset, bytesToRead);
          if (!read.success || read.bytesRead < sizeof(std::uint32_t))
            break;

          CounterSnapshot snapshot;
          snapshot.address = region.address + offset;
          snapshot.bytes = std::move(read.bytes);
          snapshot.region = region;
          snapshots.push_back(std::move(snapshot));
          ++scannedRegions;
          scannedBytes += snapshots.back().bytes.size();
          if (snapshots.back().bytes.size() < bytesToRead)
            break;
        }
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(counterSampleDelayMs));

      std::vector<CounterCandidate> preliminary;
      for (const CounterSnapshot& snapshot : snapshots)
      {
        RuntimeMemoryReadResult second =
          readProcessMemory(environment.processId, snapshot.address, snapshot.bytes.size());
        if (!second.success || second.bytesRead != snapshot.bytes.size())
          continue;
        for (std::size_t offset = 0; offset + sizeof(std::uint32_t) <= snapshot.bytes.size(); offset += sizeof(std::uint32_t))
        {
          const std::uint32_t firstValue = readU32(snapshot.bytes, offset);
          const std::uint32_t secondValue = readU32(second.bytes, offset);
          if (!plausibleCounterDelta(firstValue, secondValue))
            continue;
          CounterCandidate candidate;
          candidate.address = snapshot.address + offset;
          candidate.first = firstValue;
          candidate.second = secondValue;
          candidate.region = snapshot.region;
          candidate.score =
            std::abs(static_cast<int>(secondValue - firstValue)
              - std::max(1, (counterSampleDelayMs * 24) / 1000));
          preliminary.push_back(std::move(candidate));
          if (preliminary.size() >= 4096)
            break;
        }
        if (preliminary.size() >= 4096)
          break;
      }

      std::sort(
        preliminary.begin(),
        preliminary.end(),
        [](const CounterCandidate& lhs, const CounterCandidate& rhs)
        {
          if (lhs.score != rhs.score)
            return lhs.score < rhs.score;
          return lhs.address < rhs.address;
        });

      std::this_thread::sleep_for(std::chrono::milliseconds(counterSampleDelayMs));

      std::vector<CounterCandidate> candidates;
      const std::size_t validationLimit =
        std::min<std::size_t>(preliminary.size(), std::max<std::size_t>(512, counterResultLimit * 8));
      for (std::size_t i = 0; i < validationLimit; ++i)
      {
        CounterCandidate candidate = preliminary[i];
        RuntimeMemoryReadResult third =
          readProcessMemory(environment.processId, candidate.address, sizeof(std::uint32_t));
        if (!third.success || third.bytesRead != sizeof(std::uint32_t))
          continue;
        candidate.third = readU32(third.bytes, 0);
        if (!plausibleCounterDelta(candidate.second, candidate.third))
          continue;
        candidate.score = counterScore(
          candidate.first,
          candidate.second,
          candidate.third,
          counterSampleDelayMs);
        candidates.push_back(std::move(candidate));
      }

      std::sort(
        candidates.begin(),
        candidates.end(),
        [](const CounterCandidate& lhs, const CounterCandidate& rhs)
        {
          if (lhs.score != rhs.score)
            return lhs.score < rhs.score;
          return lhs.address < rhs.address;
        });

      std::cout << "memory.counter_scan.candidate_regions=" << candidateRegions << '\n';
      std::cout << "memory.counter_scan.scanned_regions=" << scannedRegions << '\n';
      std::cout << "memory.counter_scan.scanned_bytes=" << scannedBytes << '\n';
      std::cout << "memory.counter_scan.skipped_writable_filter=" << skippedWritableFilter << '\n';
      std::cout << "memory.counter_scan.skipped_executable_filter=" << skippedExecutableFilter << '\n';
      std::cout << "memory.counter_scan.skipped_target_executable_filter="
                << skippedTargetExecutableFilter << '\n';
      std::cout << "memory.counter_scan.preliminary_count=" << preliminary.size() << '\n';
      std::cout << "memory.counter_scan.validated_count=" << candidates.size() << '\n';
      const std::size_t printed = std::min(counterResultLimit, candidates.size());
      for (std::size_t i = 0; i < printed; ++i)
      {
        const CounterCandidate& candidate = candidates[i];
        std::cout << "memory.counter_scan.candidate." << i << ".address=0x"
                  << std::hex << candidate.address << std::dec << '\n';
        std::cout << "memory.counter_scan.candidate." << i << ".sample.0=" << candidate.first << '\n';
        std::cout << "memory.counter_scan.candidate." << i << ".sample.1=" << candidate.second << '\n';
        std::cout << "memory.counter_scan.candidate." << i << ".sample.2=" << candidate.third << '\n';
        std::cout << "memory.counter_scan.candidate." << i << ".delta.0="
                  << (candidate.second - candidate.first) << '\n';
        std::cout << "memory.counter_scan.candidate." << i << ".delta.1="
                  << (candidate.third - candidate.second) << '\n';
        std::cout << "memory.counter_scan.candidate." << i << ".score=" << candidate.score << '\n';
        std::cout << "memory.counter_scan.candidate." << i << ".region.address=0x"
                  << std::hex << candidate.region.address << std::dec << '\n';
        std::cout << "memory.counter_scan.candidate." << i << ".region.size="
                  << candidate.region.size << '\n';
        std::cout << "memory.counter_scan.candidate." << i << ".region.writable="
                  << (candidate.region.writable ? "true" : "false") << '\n';
        std::cout << "memory.counter_scan.candidate." << i << ".region.target_executable="
                  << (sameMappedFile(candidate.region.mappedPath, environment.executablePath) ? "true" : "false") << '\n';
        std::cout << "memory.counter_scan.candidate." << i << ".region.user_tag="
                  << candidate.region.userTag << '\n';
        std::cout << "memory.counter_scan.candidate." << i << ".region.share_mode="
                  << candidate.region.shareMode << '\n';
        std::cout << "memory.counter_scan.candidate." << i << ".region.share_mode_name="
                  << regionShareModeName(candidate.region.shareMode) << '\n';
        if (!candidate.region.mappedPath.empty())
          std::cout << "memory.counter_scan.candidate." << i << ".region.mapped_path="
                    << candidate.region.mappedPath << '\n';
      }
      std::cout << "memory.counter_scan.printed_count=" << printed << '\n';
    }
  }

  if (diffMemoryRequested)
  {
    if (diffMaxScanBytes == 0)
      diffMaxScanBytes = findMaxScanBytes;
    RuntimeMemoryRegionListResult regions = listProcessMemoryRegions(environment.processId);
    std::cout << "memory.diff.requested=true\n";
    std::cout << "memory.diff.scan_success=" << (regions.success ? "true" : "false") << '\n';
    if (!regions.reason.empty())
      std::cout << "memory.diff.reason=" << regions.reason << '\n';
    std::cout << "memory.diff.delay_ms=" << diffDelayMs << '\n';
    std::cout << "memory.diff.max_scan_bytes=" << diffMaxScanBytes << '\n';
    std::cout << "memory.diff.result_limit=" << diffResultLimit << '\n';
    std::cout << "memory.diff.compact=" << (diffCompact ? "true" : "false") << '\n';
    std::cout << "memory.diff.address=0x" << std::hex << diffAddress << std::dec << '\n';
    std::cout << "memory.diff.size=" << (diffSize == 0 ? size : diffSize) << '\n';
    std::cout << "memory.diff.filter.writable_only=" << (findWritableOnly ? "true" : "false") << '\n';
    std::cout << "memory.diff.filter.non_executable_only="
              << (findNonExecutableOnly ? "true" : "false") << '\n';
    std::cout << "memory.diff.filter.target_executable_only="
              << (findTargetExecutableOnly ? "true" : "false") << '\n';
    std::ofstream diffOut;
    if (!diffOutPath.empty())
    {
      diffOut.open(diffOutPath);
      std::cout << "memory.diff.out.path=" << diffOutPath << '\n';
      std::cout << "memory.diff.out.opened=" << (diffOut ? "true" : "false") << '\n';
      if (diffOut)
      {
        diffOut << "address\tsize\tbefore\tafter\tregion_address\tregion_size\t"
                << "region_writable\tregion_executable\tregion_target_executable\t"
                << "region_user_tag\tregion_share_mode\tregion_share_mode_name\tregion_mapped_path\n";
      }
    }
    if (regions.success)
    {
      constexpr std::size_t maxRegionReadBytes = 4 * 1024 * 1024;
      std::size_t scannedBytes = 0;
      std::size_t scannedRegions = 0;
      std::size_t candidateRegions = 0;
      std::size_t skippedWritableFilter = 0;
      std::size_t skippedExecutableFilter = 0;
      std::size_t skippedTargetExecutableFilter = 0;
      std::vector<DiffSnapshot> snapshots;

      if (diffAddress != 0)
      {
        const std::size_t explicitSize = diffSize == 0 ? size : diffSize;
        const RuntimeMemoryRegion* region =
          findRegionContaining(regions.regions, diffAddress, explicitSize);
        if (region == nullptr || !region->readable)
        {
          std::cout << "memory.diff.explicit_range.readable=false\n";
        }
        else
        {
          RuntimeMemoryReadResult read =
            readProcessMemory(environment.processId, diffAddress, explicitSize);
          if (read.success && read.bytesRead > 0)
          {
            DiffSnapshot snapshot;
            snapshot.address = diffAddress;
            snapshot.bytes = std::move(read.bytes);
            snapshot.region = *region;
            snapshots.push_back(std::move(snapshot));
            ++candidateRegions;
            ++scannedRegions;
            scannedBytes += snapshots.back().bytes.size();
            std::cout << "memory.diff.explicit_range.readable=true\n";
          }
          else
          {
            std::cout << "memory.diff.explicit_range.readable=false\n";
            if (!read.reason.empty())
              std::cout << "memory.diff.explicit_range.reason=" << read.reason << '\n';
          }
        }
      }
      else
      {
        for (const RuntimeMemoryRegion& region : regions.regions)
        {
          if (!region.readable || region.size == 0 || scannedBytes >= diffMaxScanBytes)
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
          if (findTargetExecutableOnly && !sameMappedFile(region.mappedPath, environment.executablePath))
          {
            ++skippedTargetExecutableFilter;
            continue;
          }

          ++candidateRegions;
          for (std::size_t offset = 0; offset < region.size && scannedBytes < diffMaxScanBytes; offset += maxRegionReadBytes)
          {
            const std::size_t remainingRegionBytes = region.size - offset;
            const std::size_t bytesToRead = std::min(
              remainingRegionBytes,
              std::min(maxRegionReadBytes, diffMaxScanBytes - scannedBytes));
            if (bytesToRead == 0)
              break;

            RuntimeMemoryReadResult read =
              readProcessMemory(environment.processId, region.address + offset, bytesToRead);
            if (!read.success || read.bytesRead == 0)
              break;

            DiffSnapshot snapshot;
            snapshot.address = region.address + offset;
            snapshot.bytes = std::move(read.bytes);
            snapshot.region = region;
            snapshots.push_back(std::move(snapshot));
            ++scannedRegions;
            scannedBytes += snapshots.back().bytes.size();
            if (snapshots.back().bytes.size() < bytesToRead)
              break;
          }
        }
      }

      std::cout << "memory.diff.baseline_ready=true\n";
      std::cout << "memory.diff.snapshot_count=" << snapshots.size() << '\n';
      std::cout.flush();
      std::this_thread::sleep_for(std::chrono::milliseconds(diffDelayMs));

      std::size_t changedRangeCount = 0;
      std::size_t changedByteCount = 0;
      std::size_t rereadFailures = 0;
      std::vector<DiffRange> printedRanges;
      for (const DiffSnapshot& snapshot : snapshots)
      {
        RuntimeMemoryReadResult after =
          readProcessMemory(environment.processId, snapshot.address, snapshot.bytes.size());
        if (!after.success || after.bytesRead != snapshot.bytes.size())
        {
          ++rereadFailures;
          continue;
        }

        const std::size_t compareSize = std::min(snapshot.bytes.size(), after.bytes.size());
        for (std::size_t offset = 0; offset < compareSize;)
        {
          if (snapshot.bytes[offset] == after.bytes[offset])
          {
            ++offset;
            continue;
          }

          const std::size_t rangeStart = offset;
          while (offset < compareSize && snapshot.bytes[offset] != after.bytes[offset])
            ++offset;
          const std::size_t rangeSize = offset - rangeStart;
          ++changedRangeCount;
          changedByteCount += rangeSize;

          if (printedRanges.size() < diffResultLimit || diffOut)
          {
            DiffRange range;
            range.address = snapshot.address + rangeStart;
            range.size = rangeSize;
            const std::size_t previewSize = std::min<std::size_t>(rangeSize, 64);
            range.before.insert(
              range.before.end(),
              snapshot.bytes.begin() + static_cast<std::ptrdiff_t>(rangeStart),
              snapshot.bytes.begin() + static_cast<std::ptrdiff_t>(rangeStart + previewSize));
            range.after.insert(
              range.after.end(),
              after.bytes.begin() + static_cast<std::ptrdiff_t>(rangeStart),
              after.bytes.begin() + static_cast<std::ptrdiff_t>(rangeStart + previewSize));
            range.region = snapshot.region;
            if (diffOut)
            {
              diffOut << "0x" << std::hex << range.address << std::dec << '\t'
                      << range.size << '\t'
                      << hexBytes(range.before) << '\t'
                      << hexBytes(range.after) << '\t'
                      << "0x" << std::hex << range.region.address << std::dec << '\t'
                      << range.region.size << '\t'
                      << (range.region.writable ? "true" : "false") << '\t'
                      << (range.region.executable ? "true" : "false") << '\t'
                      << (sameMappedFile(range.region.mappedPath, environment.executablePath) ? "true" : "false") << '\t'
                      << range.region.userTag << '\t'
                      << range.region.shareMode << '\t'
                      << regionShareModeName(range.region.shareMode) << '\t'
                      << range.region.mappedPath << '\n';
            }
            if (printedRanges.size() < diffResultLimit)
              printedRanges.push_back(std::move(range));
          }
        }
      }

      std::cout << "memory.diff.candidate_regions=" << candidateRegions << '\n';
      std::cout << "memory.diff.scanned_regions=" << scannedRegions << '\n';
      std::cout << "memory.diff.scanned_bytes=" << scannedBytes << '\n';
      std::cout << "memory.diff.snapshot_count=" << snapshots.size() << '\n';
      std::cout << "memory.diff.reread_failures=" << rereadFailures << '\n';
      std::cout << "memory.diff.skipped_writable_filter=" << skippedWritableFilter << '\n';
      std::cout << "memory.diff.skipped_executable_filter=" << skippedExecutableFilter << '\n';
      std::cout << "memory.diff.skipped_target_executable_filter="
                << skippedTargetExecutableFilter << '\n';
      std::cout << "memory.diff.changed_range_count=" << changedRangeCount << '\n';
      std::cout << "memory.diff.changed_byte_count=" << changedByteCount << '\n';
      if (!diffOutPath.empty())
        std::cout << "memory.diff.out.written=" << (diffOut ? "true" : "false") << '\n';
      std::cout << "memory.diff.printed_count=" << printedRanges.size() << '\n';
      for (std::size_t i = 0; i < printedRanges.size(); ++i)
      {
        const DiffRange& range = printedRanges[i];
        std::cout << "memory.diff.range." << i << ".address=0x"
                  << std::hex << range.address << std::dec << '\n';
        std::cout << "memory.diff.range." << i << ".size=" << range.size << '\n';
        std::cout << "memory.diff.range." << i << ".before=" << hexBytes(range.before) << '\n';
        std::cout << "memory.diff.range." << i << ".after=" << hexBytes(range.after) << '\n';
        if (diffCompact)
          continue;
        std::cout << "memory.diff.range." << i << ".region.address=0x"
                  << std::hex << range.region.address << std::dec << '\n';
        std::cout << "memory.diff.range." << i << ".region.size="
                  << range.region.size << '\n';
        std::cout << "memory.diff.range." << i << ".region.writable="
                  << (range.region.writable ? "true" : "false") << '\n';
        std::cout << "memory.diff.range." << i << ".region.executable="
                  << (range.region.executable ? "true" : "false") << '\n';
        std::cout << "memory.diff.range." << i << ".region.target_executable="
                  << (sameMappedFile(range.region.mappedPath, environment.executablePath) ? "true" : "false") << '\n';
        std::cout << "memory.diff.range." << i << ".region.user_tag="
                  << range.region.userTag << '\n';
        std::cout << "memory.diff.range." << i << ".region.share_mode="
                  << range.region.shareMode << '\n';
        std::cout << "memory.diff.range." << i << ".region.share_mode_name="
                  << regionShareModeName(range.region.shareMode) << '\n';
        if (!range.region.mappedPath.empty())
          std::cout << "memory.diff.range." << i << ".region.mapped_path="
                    << range.region.mappedPath << '\n';
      }
    }
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

    const auto emptyNeedle = std::find_if(
      findNeedles.begin(),
      findNeedles.end(),
      [](const FindNeedle& needle)
      {
        return needle.bytes.empty();
      });
    if (findNeedles.empty() || emptyNeedle != findNeedles.end())
    {
      std::cerr << "find needle must be non-empty\n";
      return 64;
    }
    std::size_t minimumNeedleSize = std::numeric_limits<std::size_t>::max();
    for (const FindNeedle& needle : findNeedles)
      minimumNeedleSize = std::min(minimumNeedleSize, needle.bytes.size());

    constexpr std::size_t maxRegionReadBytes = 32 * 1024 * 1024;
    constexpr std::size_t maxPrintedMatches = 128;
    std::size_t scannedBytes = 0;
    std::size_t scannedRegions = 0;
    std::size_t candidateRegions = 0;
    std::size_t skippedWritableFilter = 0;
    std::size_t skippedExecutableFilter = 0;
    std::size_t skippedTargetExecutableFilter = 0;
    std::size_t matchCount = 0;

    for (const RuntimeMemoryRegion& region : regions.regions)
    {
      if (!region.readable || region.size < minimumNeedleSize || scannedBytes >= findMaxScanBytes)
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
      if (findTargetExecutableOnly && !sameMappedFile(region.mappedPath, environment.executablePath))
      {
        ++skippedTargetExecutableFilter;
        continue;
      }

      ++candidateRegions;

      const std::size_t bytesToRead = std::min(
        region.size,
        std::min(maxRegionReadBytes, findMaxScanBytes - scannedBytes));
      RuntimeMemoryReadResult read = readProcessMemory(environment.processId, region.address, bytesToRead);
      if (!read.success || read.bytesRead < minimumNeedleSize)
        continue;

      ++scannedRegions;
      scannedBytes += read.bytesRead;
      for (std::size_t needleIndex = 0; needleIndex < findNeedles.size(); ++needleIndex)
      {
        FindNeedle& needle = findNeedles[needleIndex];
        if (read.bytes.size() < needle.bytes.size())
          continue;

        auto begin = read.bytes.begin();
        while (true)
        {
          auto match = std::search(begin, read.bytes.end(), needle.bytes.begin(), needle.bytes.end());
          if (match == read.bytes.end())
            break;

          const std::size_t offset = static_cast<std::size_t>(std::distance(read.bytes.begin(), match));
          if (matchCount < maxPrintedMatches)
          {
            std::cout << "memory.find.match." << matchCount << ".needle_index=" << needleIndex << '\n';
            std::cout << "memory.find.match." << matchCount << ".needle_kind=" << needle.kind << '\n';
            std::cout << "memory.find.match." << matchCount << ".needle=" << needle.printable << '\n';
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
            std::cout << "memory.find.match." << matchCount << ".region.user_tag="
                      << region.userTag << '\n';
            std::cout << "memory.find.match." << matchCount << ".region.share_mode="
                      << region.shareMode << '\n';
            std::cout << "memory.find.match." << matchCount << ".region.share_mode_name="
                      << regionShareModeName(region.shareMode) << '\n';
            if (!region.mappedPath.empty())
              std::cout << "memory.find.match." << matchCount << ".region.mapped_path="
                        << region.mappedPath << '\n';
          }
          ++needle.matchCount;
          ++matchCount;
          begin = match + 1;
        }
      }
    }

    std::cout << "memory.find.needle_count=" << findNeedles.size() << '\n';
    for (std::size_t index = 0; index < findNeedles.size(); ++index)
    {
      const FindNeedle& needle = findNeedles[index];
      std::cout << "memory.find.needle." << index << ".kind=" << needle.kind << '\n';
      std::cout << "memory.find.needle." << index << ".value=" << needle.printable << '\n';
      std::cout << "memory.find.needle." << index << ".size=" << needle.bytes.size() << '\n';
      std::cout << "memory.find.needle." << index << ".match.count=" << needle.matchCount << '\n';
    }
    if (findNeedles.size() == 1)
    {
      const FindNeedle& needle = findNeedles.front();
      if (needle.kind == "u64")
        std::cout << "memory.find.needle.u64=" << needle.printable << '\n';
      else if (needle.kind == "u32")
        std::cout << "memory.find.needle.u32=" << needle.printable << '\n';
      else
        std::cout << "memory.find.needle=" << needle.printable << '\n';
    }
    std::cout << "memory.find.filter.writable_only=" << (findWritableOnly ? "true" : "false") << '\n';
    std::cout << "memory.find.filter.non_executable_only=" << (findNonExecutableOnly ? "true" : "false") << '\n';
    std::cout << "memory.find.filter.target_executable_only="
              << (findTargetExecutableOnly ? "true" : "false") << '\n';
    std::cout << "memory.find.candidate_regions=" << candidateRegions << '\n';
    std::cout << "memory.find.skipped_writable_filter=" << skippedWritableFilter << '\n';
    std::cout << "memory.find.skipped_executable_filter=" << skippedExecutableFilter << '\n';
    std::cout << "memory.find.skipped_target_executable_filter="
              << skippedTargetExecutableFilter << '\n';
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
