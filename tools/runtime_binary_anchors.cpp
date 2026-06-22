#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace
{
  volatile const char* selfFixtureAnchor = "starcraft-api-binary-anchor-self-fixture";

  struct SectionInfo
  {
    std::string segment;
    std::string section;
    std::uint64_t address = 0;
    std::uint64_t size = 0;
    std::uint64_t fileOffset = 0;
    std::uint32_t flags = 0;
  };

  struct SegmentInfo
  {
    std::string name;
    std::uint64_t vmaddr = 0;
    std::uint64_t vmsize = 0;
    std::uint64_t fileoff = 0;
    std::uint64_t filesize = 0;
  };

  struct MachOInfo
  {
    bool detected = false;
    bool fat = false;
    bool littleEndian64 = false;
    std::uint64_t sliceOffset = 0;
    std::uint32_t commandCount = 0;
    std::vector<SegmentInfo> segments;
    std::vector<SectionInfo> sections;
    std::string reason;
  };

  struct Xref
  {
    std::uint64_t fileOffset = 0;
    std::uint64_t vmaddr = 0;
    std::int32_t displacement = 0;
  };

  struct PointerRef
  {
    std::uint64_t fileOffset = 0;
    std::uint64_t vmaddr = 0;
    std::vector<Xref> xrefs;
  };

  struct ItaniumVtableCandidate
  {
    std::uint64_t typeInfoVmaddr = 0;
    std::uint64_t namePointerVmaddr = 0;
    std::uint64_t typeInfoSlotVmaddr = 0;
    std::uint64_t vptrCandidateVmaddr = 0;
    std::uint64_t firstFunctionVmaddr = 0;
    std::uint64_t offsetToTop = 0;
    bool hasOffsetToTop = false;
    bool hasFirstFunction = false;
  };

  struct AnchorOccurrence
  {
    std::uint64_t fileOffset = 0;
    bool hasVmaddr = false;
    std::uint64_t vmaddr = 0;
    std::string segment;
    std::string section;
    std::uint64_t stringStartFileOffset = 0;
    bool hasStringStartVmaddr = false;
    std::uint64_t stringStartVmaddr = 0;
    std::string stringStartSegment;
    std::string stringStartSection;
    std::string enclosingString;
    std::vector<Xref> xrefs;
    std::vector<PointerRef> pointerRefs;
    std::vector<Xref> stringStartXrefs;
    std::vector<PointerRef> stringStartPointerRefs;
  };

  struct AnchorResult
  {
    std::string anchor;
    bool found = false;
    std::uint64_t fileOffset = 0;
    bool hasVmaddr = false;
    std::uint64_t vmaddr = 0;
    std::string segment;
    std::string section;
    std::vector<Xref> xrefs;
    std::vector<PointerRef> pointerRefs;
    std::vector<AnchorOccurrence> occurrences;
  };

  struct PrintableString
  {
    std::uint64_t fileOffset = 0;
    std::string value;
    bool hasVmaddr = false;
    std::uint64_t vmaddr = 0;
    std::string segment;
    std::string section;
  };

  struct MachHeader64
  {
    std::uint32_t magic = 0;
    std::uint32_t cputype = 0;
    std::uint32_t cpusubtype = 0;
    std::uint32_t filetype = 0;
    std::uint32_t ncmds = 0;
    std::uint32_t sizeofcmds = 0;
    std::uint32_t flags = 0;
    std::uint32_t reserved = 0;
  };

  struct LoadCommand
  {
    std::uint32_t cmd = 0;
    std::uint32_t cmdsize = 0;
  };

  struct SegmentCommand64
  {
    std::uint32_t cmd = 0;
    std::uint32_t cmdsize = 0;
    char segname[16] = {};
    std::uint64_t vmaddr = 0;
    std::uint64_t vmsize = 0;
    std::uint64_t fileoff = 0;
    std::uint64_t filesize = 0;
    std::uint32_t maxprot = 0;
    std::uint32_t initprot = 0;
    std::uint32_t nsects = 0;
    std::uint32_t flags = 0;
  };

  struct Section64
  {
    char sectname[16] = {};
    char segname[16] = {};
    std::uint64_t addr = 0;
    std::uint64_t size = 0;
    std::uint32_t offset = 0;
    std::uint32_t align = 0;
    std::uint32_t reloff = 0;
    std::uint32_t nreloc = 0;
    std::uint32_t flags = 0;
    std::uint32_t reserved1 = 0;
    std::uint32_t reserved2 = 0;
    std::uint32_t reserved3 = 0;
  };

  constexpr std::uint32_t MachHeaderMagic64 = 0xfeedfacf;
  constexpr std::uint32_t LoadCommandSegment64 = 0x19;
  constexpr std::uint32_t FatMagic = 0xcafebabe;
  constexpr std::uint32_t FatMagic64 = 0xcafebabf;

  void keepSelfFixtureAnchorAlive()
  {
    if (selfFixtureAnchor[0] == '\0')
      std::cerr << selfFixtureAnchor << '\n';
  }

  void printUsage()
  {
    std::cout
      << "usage: starcraft-runtime-binary-anchors --executable <path> --anchor <text> [options]\n"
      << "  --executable <path>          target runtime executable or binary image\n"
      << "  --anchor <text>              ASCII anchor to find; repeat for multiple anchors\n"
      << "  --default-remastered-anchors scan built-in SC:R state/unit diagnostic anchors\n"
      << "  --string-match <text>        list printable binary strings containing text; repeatable\n"
      << "  --max-string-matches <count> maximum listed strings per matcher (default: 64)\n"
      << "  --max-xrefs <count>          maximum xrefs printed per anchor (default: 16)\n"
      << "  --emit-itanium-vtables       emit Itanium ABI typeinfo/vptr candidates for anchor strings\n"
      << "  --require-anchor             return non-zero unless every requested anchor is found\n"
      << "  --help                       show this help\n";
  }

  bool parseSize(const std::string& value, std::size_t& output)
  {
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0')
      return false;
    output = static_cast<std::size_t>(parsed);
    return static_cast<unsigned long long>(output) == parsed;
  }

  std::string fixedString(const char* value, std::size_t size)
  {
    std::size_t length = 0;
    while (length < size && value[length] != '\0')
      ++length;
    return std::string(value, value + length);
  }

  bool readFile(const std::filesystem::path& path, std::vector<unsigned char>& bytes, std::string& reason)
  {
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
      reason = "unable to open binary: " + path.string();
      return false;
    }

    bytes.assign(
      std::istreambuf_iterator<char>(input),
      std::istreambuf_iterator<char>());
    if (input.bad())
    {
      reason = "unable to read complete binary: " + path.string();
      return false;
    }
    return true;
  }

  template <typename T>
  bool readPod(const std::vector<unsigned char>& bytes, std::uint64_t offset, T& output)
  {
    if (offset > bytes.size() || sizeof(T) > bytes.size() - static_cast<std::size_t>(offset))
      return false;
    std::memcpy(&output, bytes.data() + offset, sizeof(T));
    return true;
  }

  std::uint32_t readBE32(const std::vector<unsigned char>& bytes, std::uint64_t offset)
  {
    if (offset + 4 > bytes.size())
      return 0;
    return (static_cast<std::uint32_t>(bytes[static_cast<std::size_t>(offset)]) << 24)
      | (static_cast<std::uint32_t>(bytes[static_cast<std::size_t>(offset + 1)]) << 16)
      | (static_cast<std::uint32_t>(bytes[static_cast<std::size_t>(offset + 2)]) << 8)
      | static_cast<std::uint32_t>(bytes[static_cast<std::size_t>(offset + 3)]);
  }

  bool findMachOSlice(const std::vector<unsigned char>& bytes, std::uint64_t& sliceOffset, bool& fat)
  {
    MachHeader64 header;
    if (readPod(bytes, 0, header) && header.magic == MachHeaderMagic64)
    {
      sliceOffset = 0;
      fat = false;
      return true;
    }

    const std::uint32_t fatMagic = readBE32(bytes, 0);
    if (fatMagic != FatMagic && fatMagic != FatMagic64)
      return false;

    const std::uint32_t archCount = readBE32(bytes, 4);
    const std::uint64_t archSize = fatMagic == FatMagic64 ? 32 : 20;
    for (std::uint32_t i = 0; i < archCount; ++i)
    {
      const std::uint64_t archOffset = 8 + static_cast<std::uint64_t>(i) * archSize;
      const std::uint64_t candidateOffset = readBE32(bytes, archOffset + 8);
      if (candidateOffset >= bytes.size())
        continue;

      MachHeader64 candidate;
      if (readPod(bytes, candidateOffset, candidate) && candidate.magic == MachHeaderMagic64)
      {
        sliceOffset = candidateOffset;
        fat = true;
        return true;
      }
    }

    return false;
  }

  MachOInfo parseMachO(const std::vector<unsigned char>& bytes)
  {
    MachOInfo info;
    if (!findMachOSlice(bytes, info.sliceOffset, info.fat))
    {
      info.reason = "binary is not a little-endian 64-bit Mach-O image";
      return info;
    }

    MachHeader64 header;
    if (!readPod(bytes, info.sliceOffset, header) || header.magic != MachHeaderMagic64)
    {
      info.reason = "unable to read Mach-O header";
      return info;
    }

    info.detected = true;
    info.littleEndian64 = true;
    info.commandCount = header.ncmds;

    std::uint64_t commandOffset = info.sliceOffset + sizeof(MachHeader64);
    for (std::uint32_t i = 0; i < header.ncmds; ++i)
    {
      LoadCommand command;
      if (!readPod(bytes, commandOffset, command) || command.cmdsize < sizeof(LoadCommand))
      {
        info.reason = "truncated Mach-O load command";
        break;
      }

      if (command.cmd == LoadCommandSegment64 && command.cmdsize >= sizeof(SegmentCommand64))
      {
        SegmentCommand64 segmentCommand;
        if (!readPod(bytes, commandOffset, segmentCommand))
        {
          info.reason = "truncated Mach-O segment command";
          break;
        }

        SegmentInfo segment;
        segment.name = fixedString(segmentCommand.segname, sizeof(segmentCommand.segname));
        segment.vmaddr = segmentCommand.vmaddr;
        segment.vmsize = segmentCommand.vmsize;
        segment.fileoff = info.sliceOffset + segmentCommand.fileoff;
        segment.filesize = segmentCommand.filesize;
        info.segments.push_back(segment);

        std::uint64_t sectionOffset = commandOffset + sizeof(SegmentCommand64);
        for (std::uint32_t sectionIndex = 0; sectionIndex < segmentCommand.nsects; ++sectionIndex)
        {
          if (sectionOffset + sizeof(Section64) > commandOffset + command.cmdsize)
            break;

          Section64 sectionCommand;
          if (!readPod(bytes, sectionOffset, sectionCommand))
            break;

          SectionInfo section;
          section.segment = fixedString(sectionCommand.segname, sizeof(sectionCommand.segname));
          section.section = fixedString(sectionCommand.sectname, sizeof(sectionCommand.sectname));
          section.address = sectionCommand.addr;
          section.size = sectionCommand.size;
          section.fileOffset = info.sliceOffset + sectionCommand.offset;
          section.flags = sectionCommand.flags;
          info.sections.push_back(section);
          sectionOffset += sizeof(Section64);
        }
      }

      commandOffset += command.cmdsize;
      if (commandOffset > bytes.size())
      {
        info.reason = "Mach-O load commands exceed file size";
        break;
      }
    }

    return info;
  }

  std::string hexValue(std::uint64_t value)
  {
    std::ostringstream output;
    output << "0x" << std::hex << value;
    return output.str();
  }

  bool sectionContainsFileOffset(const SectionInfo& section, std::uint64_t fileOffset, std::size_t size)
  {
    if (fileOffset < section.fileOffset)
      return false;
    const std::uint64_t relative = fileOffset - section.fileOffset;
    return relative <= section.size && size <= section.size - relative;
  }

  bool segmentContainsFileOffset(const SegmentInfo& segment, std::uint64_t fileOffset, std::size_t size)
  {
    if (fileOffset < segment.fileoff)
      return false;
    const std::uint64_t relative = fileOffset - segment.fileoff;
    return relative <= segment.filesize && size <= segment.filesize - relative;
  }

  bool mapFileOffsetToVmaddr(
    const MachOInfo& macho,
    std::uint64_t fileOffset,
    std::size_t size,
    std::uint64_t& vmaddr,
    std::string& segmentName,
    std::string& sectionName)
  {
    for (const SectionInfo& section : macho.sections)
    {
      if (!sectionContainsFileOffset(section, fileOffset, size))
        continue;

      vmaddr = section.address + (fileOffset - section.fileOffset);
      segmentName = section.segment;
      sectionName = section.section;
      return true;
    }

    for (const SegmentInfo& segment : macho.segments)
    {
      if (!segmentContainsFileOffset(segment, fileOffset, size))
        continue;

      vmaddr = segment.vmaddr + (fileOffset - segment.fileoff);
      segmentName = segment.name;
      sectionName.clear();
      return true;
    }

    return false;
  }

  bool readI32(const std::vector<unsigned char>& bytes, std::uint64_t offset, std::int32_t& value)
  {
    if (offset > bytes.size() || sizeof(value) > bytes.size() - static_cast<std::size_t>(offset))
      return false;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return true;
  }

  bool readU64(const std::vector<unsigned char>& bytes, std::uint64_t offset, std::uint64_t& value)
  {
    if (offset > bytes.size() || sizeof(value) > bytes.size() - static_cast<std::size_t>(offset))
      return false;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return true;
  }

  bool isTextSection(const SectionInfo& section)
  {
    if (section.segment != "__TEXT")
      return false;
    return section.section == "__text"
      || section.section == "__INIT0"
      || section.section == "allocator"
      || section.section == "__stubs"
      || section.section == "__stub_helper";
  }

  std::vector<Xref> findRipRelativeXrefs(
    const std::vector<unsigned char>& bytes,
    const MachOInfo& macho,
    std::uint64_t targetVmaddr,
    std::size_t maxXrefs)
  {
    std::vector<Xref> xrefs;
    for (const SectionInfo& section : macho.sections)
    {
      if (!isTextSection(section))
        continue;
      if (section.fileOffset >= bytes.size())
        continue;

      const std::uint64_t sectionEnd =
        std::min<std::uint64_t>(section.fileOffset + section.size, bytes.size());
      for (std::uint64_t fileOffset = section.fileOffset; fileOffset + 4 <= sectionEnd; ++fileOffset)
      {
        std::int32_t displacement = 0;
        if (!readI32(bytes, fileOffset, displacement))
          break;

        const std::uint64_t displacementVmaddr = section.address + (fileOffset - section.fileOffset);
        const std::int64_t nextInstruction =
          static_cast<std::int64_t>(displacementVmaddr + sizeof(displacement));
        const std::int64_t referenced = nextInstruction + static_cast<std::int64_t>(displacement);
        if (referenced != static_cast<std::int64_t>(targetVmaddr))
          continue;

        Xref xref;
        xref.fileOffset = fileOffset;
        xref.vmaddr = displacementVmaddr;
        xref.displacement = displacement;
        xrefs.push_back(xref);
        if (xrefs.size() >= maxXrefs)
          return xrefs;
      }
    }
    return xrefs;
  }

  std::vector<PointerRef> findPointerRefs(
    const std::vector<unsigned char>& bytes,
    const MachOInfo& macho,
    std::uint64_t targetVmaddr,
    std::size_t maxRefs)
  {
    std::vector<PointerRef> refs;
    for (const SectionInfo& section : macho.sections)
    {
      if (isTextSection(section) || section.fileOffset >= bytes.size())
        continue;

      const std::uint64_t sectionEnd =
        std::min<std::uint64_t>(section.fileOffset + section.size, bytes.size());
      for (std::uint64_t fileOffset = section.fileOffset; fileOffset + 8 <= sectionEnd; fileOffset += 8)
      {
        std::uint64_t value = 0;
        if (!readU64(bytes, fileOffset, value))
          break;
        if (value != targetVmaddr)
          continue;

        PointerRef ref;
        ref.fileOffset = fileOffset;
        ref.vmaddr = section.address + (fileOffset - section.fileOffset);
        ref.xrefs = findRipRelativeXrefs(bytes, macho, ref.vmaddr, maxRefs);
        refs.push_back(ref);
        if (refs.size() >= maxRefs)
          return refs;
      }
    }
    return refs;
  }

  std::vector<ItaniumVtableCandidate> findItaniumVtableCandidatesForNamePointer(
    const std::vector<unsigned char>& bytes,
    const MachOInfo& macho,
    const PointerRef& namePointerRef,
    std::size_t maxRefs)
  {
    std::vector<ItaniumVtableCandidate> candidates;
    if (!macho.detected || namePointerRef.vmaddr < sizeof(std::uint64_t))
      return candidates;

    const std::uint64_t typeInfoVmaddr = namePointerRef.vmaddr - sizeof(std::uint64_t);
    const std::vector<PointerRef> typeInfoRefs =
      findPointerRefs(bytes, macho, typeInfoVmaddr, maxRefs);

    for (const PointerRef& typeInfoRef : typeInfoRefs)
    {
      ItaniumVtableCandidate candidate;
      candidate.typeInfoVmaddr = typeInfoVmaddr;
      candidate.namePointerVmaddr = namePointerRef.vmaddr;
      candidate.typeInfoSlotVmaddr = typeInfoRef.vmaddr;
      candidate.vptrCandidateVmaddr = typeInfoRef.vmaddr + sizeof(std::uint64_t);

      if (typeInfoRef.fileOffset >= sizeof(std::uint64_t))
      {
        candidate.hasOffsetToTop =
          readU64(bytes, typeInfoRef.fileOffset - sizeof(std::uint64_t), candidate.offsetToTop);
      }

      candidate.hasFirstFunction =
        readU64(bytes, typeInfoRef.fileOffset + sizeof(std::uint64_t), candidate.firstFunctionVmaddr);

      candidates.push_back(candidate);
      if (candidates.size() >= maxRefs)
        return candidates;
    }

    return candidates;
  }

  std::vector<std::uint64_t> findAsciiOffsets(
    const std::vector<unsigned char>& bytes,
    const std::string& anchor)
  {
    std::vector<std::uint64_t> offsets;
    if (anchor.empty())
      return offsets;

    auto begin = bytes.begin();
    while (true)
    {
      auto match = std::search(begin, bytes.end(), anchor.begin(), anchor.end());
      if (match == bytes.end())
        break;

      offsets.push_back(static_cast<std::uint64_t>(std::distance(bytes.begin(), match)));
      begin = match + 1;
    }
    return offsets;
  }

  bool printableAscii(unsigned char ch)
  {
    return ch >= 0x20 && ch <= 0x7e;
  }

  std::uint64_t enclosingCStringStart(
    const std::vector<unsigned char>& bytes,
    std::uint64_t fileOffset)
  {
    std::uint64_t start = fileOffset;
    while (start > 0 && printableAscii(bytes[static_cast<std::size_t>(start - 1)]))
      --start;
    return start;
  }

  std::string enclosingCStringValue(
    const std::vector<unsigned char>& bytes,
    std::uint64_t stringStartFileOffset)
  {
    std::uint64_t end = stringStartFileOffset;
    while (end < bytes.size() && printableAscii(bytes[static_cast<std::size_t>(end)]))
      ++end;
    return std::string(
      reinterpret_cast<const char*>(bytes.data() + stringStartFileOffset),
      reinterpret_cast<const char*>(bytes.data() + end));
  }

  std::vector<PrintableString> findMatchingPrintableStrings(
    const std::vector<unsigned char>& bytes,
    const MachOInfo& macho,
    const std::string& needle,
    std::size_t maxMatches)
  {
    std::vector<PrintableString> matches;
    if (needle.empty())
      return matches;

    std::size_t offset = 0;
    while (offset < bytes.size() && matches.size() < maxMatches)
    {
      while (offset < bytes.size() && !printableAscii(bytes[offset]))
        ++offset;
      const std::size_t start = offset;
      while (offset < bytes.size() && printableAscii(bytes[offset]))
        ++offset;
      if (offset <= start)
        continue;

      const std::size_t length = offset - start;
      if (length < 4)
        continue;

      std::string value(
        reinterpret_cast<const char*>(bytes.data() + start),
        reinterpret_cast<const char*>(bytes.data() + offset));
      if (value.find(needle) == std::string::npos)
        continue;

      PrintableString match;
      match.fileOffset = start;
      match.value = std::move(value);
      if (macho.detected)
      {
        match.hasVmaddr = mapFileOffsetToVmaddr(
          macho,
          match.fileOffset,
          length,
          match.vmaddr,
          match.segment,
          match.section);
      }
      matches.push_back(std::move(match));
    }

    return matches;
  }

  void addDefaultRemasteredAnchors(std::vector<std::string>& anchors)
  {
    const char* defaults[] = {
      "gGameHeader:",
      "CUnit::sgUnitsMem",
      "Invalid sgUnitsMem array size",
      "eud_cunit_array_adapter_t",
      "eud_cobject_array_adapter_tI5CUnit",
      "eud_cbullet_array_adapter_t",
      "eud_cobject_array_adapter_tI7CBullet",
      "eud_playerdata_adapter_t",
      "CBullet: Damage",
      "Invalid order for action command",
      "Net Data:",
      "Player Data:",
      "GetTurnPackets",
      "netmgr_process_messages",
      "PlaySingleOperation",
      "StartGame",
      "LaunchGame",
      "CreateGame_MultiPlayer()"
    };

    for (const char* value : defaults)
    {
      if (std::find(anchors.begin(), anchors.end(), value) == anchors.end())
        anchors.push_back(value);
    }
  }

  AnchorResult analyzeAnchor(
    const std::vector<unsigned char>& bytes,
    const MachOInfo& macho,
    const std::string& anchor,
    std::size_t maxXrefs)
  {
    AnchorResult result;
    result.anchor = anchor;

    const std::vector<std::uint64_t> offsets = findAsciiOffsets(bytes, anchor);
    if (offsets.empty())
      return result;

    result.found = true;
    result.fileOffset = offsets.front();
    for (std::uint64_t offset : offsets)
    {
      AnchorOccurrence occurrence;
      occurrence.fileOffset = offset;
      occurrence.stringStartFileOffset = enclosingCStringStart(bytes, offset);
      occurrence.enclosingString = enclosingCStringValue(bytes, occurrence.stringStartFileOffset);
      if (macho.detected)
      {
        occurrence.hasVmaddr = mapFileOffsetToVmaddr(
          macho,
          occurrence.fileOffset,
          anchor.size(),
          occurrence.vmaddr,
          occurrence.segment,
          occurrence.section);
        occurrence.hasStringStartVmaddr = mapFileOffsetToVmaddr(
          macho,
          occurrence.stringStartFileOffset,
          occurrence.enclosingString.empty() ? anchor.size() : occurrence.enclosingString.size(),
          occurrence.stringStartVmaddr,
          occurrence.stringStartSegment,
          occurrence.stringStartSection);
        if (occurrence.hasVmaddr)
        {
          occurrence.xrefs = findRipRelativeXrefs(bytes, macho, occurrence.vmaddr, maxXrefs);
          occurrence.pointerRefs = findPointerRefs(bytes, macho, occurrence.vmaddr, maxXrefs);
        }
        if (occurrence.hasStringStartVmaddr)
        {
          occurrence.stringStartXrefs =
            findRipRelativeXrefs(bytes, macho, occurrence.stringStartVmaddr, maxXrefs);
          occurrence.stringStartPointerRefs =
            findPointerRefs(bytes, macho, occurrence.stringStartVmaddr, maxXrefs);
        }
      }
      result.occurrences.push_back(std::move(occurrence));
    }
    if (macho.detected)
    {
      result.hasVmaddr = mapFileOffsetToVmaddr(
        macho,
        result.fileOffset,
        anchor.size(),
        result.vmaddr,
        result.segment,
        result.section);
      if (result.hasVmaddr)
      {
        result.xrefs = result.occurrences.front().xrefs;
        result.pointerRefs = result.occurrences.front().pointerRefs;
      }
    }

    return result;
  }

  void printMachOInfo(const MachOInfo& macho)
  {
    std::cout << "macho.detected=" << (macho.detected ? "true" : "false") << '\n';
    std::cout << "macho.fat=" << (macho.fat ? "true" : "false") << '\n';
    std::cout << "macho.little_endian_64=" << (macho.littleEndian64 ? "true" : "false") << '\n';
    if (!macho.reason.empty())
      std::cout << "macho.reason=" << macho.reason << '\n';
    if (!macho.detected)
      return;

    std::cout << "macho.slice_offset=" << macho.sliceOffset << '\n';
    std::cout << "macho.command_count=" << macho.commandCount << '\n';
    std::cout << "macho.segment_count=" << macho.segments.size() << '\n';
    std::cout << "macho.section_count=" << macho.sections.size() << '\n';
    for (std::size_t i = 0; i < macho.sections.size(); ++i)
    {
      const SectionInfo& section = macho.sections[i];
      if (section.segment != "__TEXT" && section.segment != "__DATA" && section.segment != "__DATA_CONST")
        continue;
      std::cout << "macho.section." << i << ".segment=" << section.segment << '\n';
      std::cout << "macho.section." << i << ".name=" << section.section << '\n';
      std::cout << "macho.section." << i << ".vmaddr=" << hexValue(section.address) << '\n';
      std::cout << "macho.section." << i << ".file_offset=" << section.fileOffset << '\n';
      std::cout << "macho.section." << i << ".size=" << section.size << '\n';
    }
  }

  void printAnchorResult(
    const std::vector<unsigned char>& bytes,
    const MachOInfo& macho,
    const AnchorResult& result,
    std::size_t index,
    bool emitItaniumVtables,
    std::size_t maxXrefs)
  {
    std::cout << "anchor." << index << ".name=" << result.anchor << '\n';
    std::cout << "anchor." << index << ".found=" << (result.found ? "true" : "false") << '\n';
    if (!result.found)
      return;

    std::cout << "anchor." << index << ".file_offset=" << result.fileOffset << '\n';
    std::cout << "anchor." << index << ".file_offset.hex=" << hexValue(result.fileOffset) << '\n';
    std::cout << "anchor." << index << ".vmaddr.mapped=" << (result.hasVmaddr ? "true" : "false") << '\n';
    if (result.hasVmaddr)
    {
      std::cout << "anchor." << index << ".vmaddr=" << hexValue(result.vmaddr) << '\n';
      std::cout << "anchor." << index << ".segment=" << result.segment << '\n';
      std::cout << "anchor." << index << ".section=" << result.section << '\n';
      std::cout << "anchor." << index << ".xref.rip_relative.count=" << result.xrefs.size() << '\n';
      for (std::size_t i = 0; i < result.xrefs.size(); ++i)
      {
        const Xref& xref = result.xrefs[i];
        std::cout << "anchor." << index << ".xref." << i << ".file_offset=" << xref.fileOffset << '\n';
        std::cout << "anchor." << index << ".xref." << i << ".vmaddr=" << hexValue(xref.vmaddr) << '\n';
        std::cout << "anchor." << index << ".xref." << i << ".displacement=" << xref.displacement << '\n';
      }
      std::cout << "anchor." << index << ".pointer_ref.count=" << result.pointerRefs.size() << '\n';
      for (std::size_t i = 0; i < result.pointerRefs.size(); ++i)
      {
        const PointerRef& ref = result.pointerRefs[i];
        std::cout << "anchor." << index << ".pointer_ref." << i << ".file_offset=" << ref.fileOffset << '\n';
        std::cout << "anchor." << index << ".pointer_ref." << i << ".vmaddr=" << hexValue(ref.vmaddr) << '\n';
        std::cout << "anchor." << index << ".pointer_ref." << i << ".xref.rip_relative.count="
                  << ref.xrefs.size() << '\n';
        for (std::size_t xrefIndex = 0; xrefIndex < ref.xrefs.size(); ++xrefIndex)
        {
          const Xref& xref = ref.xrefs[xrefIndex];
          std::cout << "anchor." << index << ".pointer_ref." << i << ".xref." << xrefIndex
                    << ".file_offset=" << xref.fileOffset << '\n';
          std::cout << "anchor." << index << ".pointer_ref." << i << ".xref." << xrefIndex
                    << ".vmaddr=" << hexValue(xref.vmaddr) << '\n';
          std::cout << "anchor." << index << ".pointer_ref." << i << ".xref." << xrefIndex
                    << ".displacement=" << xref.displacement << '\n';
        }
      }
    }

    std::cout << "anchor." << index << ".occurrence_count=" << result.occurrences.size() << '\n';
    for (std::size_t occurrenceIndex = 0; occurrenceIndex < result.occurrences.size(); ++occurrenceIndex)
    {
      const AnchorOccurrence& occurrence = result.occurrences[occurrenceIndex];
      std::cout << "anchor." << index << ".occurrence." << occurrenceIndex
                << ".file_offset=" << occurrence.fileOffset << '\n';
      std::cout << "anchor." << index << ".occurrence." << occurrenceIndex
                << ".file_offset.hex=" << hexValue(occurrence.fileOffset) << '\n';
      std::cout << "anchor." << index << ".occurrence." << occurrenceIndex
                << ".vmaddr.mapped=" << (occurrence.hasVmaddr ? "true" : "false") << '\n';
      if (occurrence.hasVmaddr)
      {
        std::cout << "anchor." << index << ".occurrence." << occurrenceIndex
                  << ".vmaddr=" << hexValue(occurrence.vmaddr) << '\n';
      }
      std::cout << "anchor." << index << ".occurrence." << occurrenceIndex
                << ".string_start.file_offset=" << occurrence.stringStartFileOffset << '\n';
      std::cout << "anchor." << index << ".occurrence." << occurrenceIndex
                << ".string_start.file_offset.hex=" << hexValue(occurrence.stringStartFileOffset) << '\n';
      std::cout << "anchor." << index << ".occurrence." << occurrenceIndex
                << ".string_start.vmaddr.mapped="
                << (occurrence.hasStringStartVmaddr ? "true" : "false") << '\n';
      if (occurrence.hasStringStartVmaddr)
      {
        std::cout << "anchor." << index << ".occurrence." << occurrenceIndex
                  << ".string_start.vmaddr=" << hexValue(occurrence.stringStartVmaddr) << '\n';
        std::cout << "anchor." << index << ".occurrence." << occurrenceIndex
                  << ".string_start.segment=" << occurrence.stringStartSegment << '\n';
        std::cout << "anchor." << index << ".occurrence." << occurrenceIndex
                  << ".string_start.section=" << occurrence.stringStartSection << '\n';
      }
      std::cout << "anchor." << index << ".occurrence." << occurrenceIndex
                << ".string_start.xref.rip_relative.count="
                << occurrence.stringStartXrefs.size() << '\n';
      for (std::size_t xrefIndex = 0; xrefIndex < occurrence.stringStartXrefs.size(); ++xrefIndex)
      {
        const Xref& xref = occurrence.stringStartXrefs[xrefIndex];
        std::cout << "anchor." << index << ".occurrence." << occurrenceIndex
                  << ".string_start.xref." << xrefIndex
                  << ".file_offset=" << xref.fileOffset << '\n';
        std::cout << "anchor." << index << ".occurrence." << occurrenceIndex
                  << ".string_start.xref." << xrefIndex
                  << ".vmaddr=" << hexValue(xref.vmaddr) << '\n';
      }
      std::cout << "anchor." << index << ".occurrence." << occurrenceIndex
                << ".string_start.pointer_ref.count="
                << occurrence.stringStartPointerRefs.size() << '\n';
      for (std::size_t refIndex = 0; refIndex < occurrence.stringStartPointerRefs.size(); ++refIndex)
      {
        const PointerRef& ref = occurrence.stringStartPointerRefs[refIndex];
        std::cout << "anchor." << index << ".occurrence." << occurrenceIndex
                  << ".string_start.pointer_ref." << refIndex
                  << ".file_offset=" << ref.fileOffset << '\n';
        std::cout << "anchor." << index << ".occurrence." << occurrenceIndex
                  << ".string_start.pointer_ref." << refIndex
                  << ".vmaddr=" << hexValue(ref.vmaddr) << '\n';
        if (emitItaniumVtables)
        {
          const std::vector<ItaniumVtableCandidate> candidates =
            findItaniumVtableCandidatesForNamePointer(bytes, macho, ref, maxXrefs);
          std::cout << "anchor." << index << ".occurrence." << occurrenceIndex
                    << ".string_start.pointer_ref." << refIndex
                    << ".itanium_vtable.count=" << candidates.size() << '\n';
          for (std::size_t candidateIndex = 0; candidateIndex < candidates.size(); ++candidateIndex)
          {
            const ItaniumVtableCandidate& candidate = candidates[candidateIndex];
            std::cout << "anchor." << index << ".occurrence." << occurrenceIndex
                      << ".string_start.pointer_ref." << refIndex
                      << ".itanium_vtable." << candidateIndex
                      << ".typeinfo_vmaddr=" << hexValue(candidate.typeInfoVmaddr) << '\n';
            std::cout << "anchor." << index << ".occurrence." << occurrenceIndex
                      << ".string_start.pointer_ref." << refIndex
                      << ".itanium_vtable." << candidateIndex
                      << ".typeinfo_slot_vmaddr=" << hexValue(candidate.typeInfoSlotVmaddr) << '\n';
            std::cout << "anchor." << index << ".occurrence." << occurrenceIndex
                      << ".string_start.pointer_ref." << refIndex
                      << ".itanium_vtable." << candidateIndex
                      << ".vptr_candidate_vmaddr=" << hexValue(candidate.vptrCandidateVmaddr) << '\n';
            std::cout << "anchor." << index << ".occurrence." << occurrenceIndex
                      << ".string_start.pointer_ref." << refIndex
                      << ".itanium_vtable." << candidateIndex
                      << ".first_function_vmaddr="
                      << (candidate.hasFirstFunction ? hexValue(candidate.firstFunctionVmaddr) : std::string("unmapped"))
                      << '\n';
            std::cout << "anchor." << index << ".occurrence." << occurrenceIndex
                      << ".string_start.pointer_ref." << refIndex
                      << ".itanium_vtable." << candidateIndex
                      << ".offset_to_top="
                      << (candidate.hasOffsetToTop ? hexValue(candidate.offsetToTop) : std::string("unmapped"))
                      << '\n';
          }
        }
      }
      std::cout << "anchor." << index << ".occurrence." << occurrenceIndex
                << ".string_start.value=" << occurrence.enclosingString << '\n';
    }
  }

  void printStringMatches(
    const std::vector<unsigned char>& bytes,
    const MachOInfo& macho,
    const std::vector<std::string>& matchers,
    std::size_t maxMatches)
  {
    for (std::size_t matcherIndex = 0; matcherIndex < matchers.size(); ++matcherIndex)
    {
      const std::vector<PrintableString> matches =
        findMatchingPrintableStrings(bytes, macho, matchers[matcherIndex], maxMatches);
      std::cout << "string_match." << matcherIndex << ".needle=" << matchers[matcherIndex] << '\n';
      std::cout << "string_match." << matcherIndex << ".count=" << matches.size() << '\n';
      for (std::size_t matchIndex = 0; matchIndex < matches.size(); ++matchIndex)
      {
        const PrintableString& match = matches[matchIndex];
        std::cout << "string_match." << matcherIndex << ".match." << matchIndex
                  << ".file_offset=" << match.fileOffset << '\n';
        if (match.hasVmaddr)
        {
          std::cout << "string_match." << matcherIndex << ".match." << matchIndex
                    << ".vmaddr=" << hexValue(match.vmaddr) << '\n';
          std::cout << "string_match." << matcherIndex << ".match." << matchIndex
                    << ".segment=" << match.segment << '\n';
          std::cout << "string_match." << matcherIndex << ".match." << matchIndex
                    << ".section=" << match.section << '\n';
        }
        std::cout << "string_match." << matcherIndex << ".match." << matchIndex
                  << ".value=" << match.value << '\n';
      }
    }
  }
}

int main(int argc, char** argv)
{
  keepSelfFixtureAnchorAlive();

  std::string executablePath;
  std::vector<std::string> anchors;
  std::vector<std::string> stringMatchers;
  bool requireAnchor = false;
  bool defaultRemasteredAnchors = false;
  bool emitItaniumVtables = false;
  std::size_t maxXrefs = 16;
  std::size_t maxStringMatches = 64;

  for (int i = 1; i < argc; ++i)
  {
    const std::string arg = argv[i];
    if (arg == "--help" || arg == "-h")
    {
      printUsage();
      return 0;
    }
    if (arg == "--executable")
    {
      if (i + 1 >= argc)
      {
        std::cerr << "--executable requires a path\n";
        return 64;
      }
      executablePath = argv[++i];
      continue;
    }
    if (arg == "--anchor")
    {
      if (i + 1 >= argc)
      {
        std::cerr << "--anchor requires a value\n";
        return 64;
      }
      anchors.push_back(argv[++i]);
      continue;
    }
    if (arg == "--string-match")
    {
      if (i + 1 >= argc)
      {
        std::cerr << "--string-match requires a value\n";
        return 64;
      }
      stringMatchers.push_back(argv[++i]);
      continue;
    }
    if (arg == "--max-string-matches")
    {
      if (i + 1 >= argc || !parseSize(argv[++i], maxStringMatches))
      {
        std::cerr << "--max-string-matches requires a non-negative integer\n";
        return 64;
      }
      continue;
    }
    if (arg == "--default-remastered-anchors")
    {
      defaultRemasteredAnchors = true;
      continue;
    }
    if (arg == "--max-xrefs")
    {
      if (i + 1 >= argc || !parseSize(argv[++i], maxXrefs))
      {
        std::cerr << "--max-xrefs requires a non-negative integer\n";
        return 64;
      }
      continue;
    }
    if (arg == "--emit-itanium-vtables")
    {
      emitItaniumVtables = true;
      continue;
    }
    if (arg == "--require-anchor")
    {
      requireAnchor = true;
      continue;
    }

    std::cerr << "unknown argument: " << arg << '\n';
    return 64;
  }

  if (executablePath.empty())
  {
    std::cerr << "--executable is required\n";
    return 64;
  }
  if (defaultRemasteredAnchors)
    addDefaultRemasteredAnchors(anchors);
  if (anchors.empty() && stringMatchers.empty())
  {
    std::cerr << "at least one --anchor, --default-remastered-anchors, or --string-match is required\n";
    return 64;
  }

  std::vector<unsigned char> bytes;
  std::string reason;
  if (!readFile(executablePath, bytes, reason))
  {
    std::cerr << reason << '\n';
    return 2;
  }

  const MachOInfo macho = parseMachO(bytes);
  std::cout << "binary.path=" << executablePath << '\n';
  std::cout << "binary.size=" << bytes.size() << '\n';
  printMachOInfo(macho);

  std::size_t foundCount = 0;
  for (std::size_t i = 0; i < anchors.size(); ++i)
  {
    const AnchorResult result = analyzeAnchor(bytes, macho, anchors[i], maxXrefs);
    if (result.found)
      ++foundCount;
    printAnchorResult(bytes, macho, result, i, emitItaniumVtables, maxXrefs);
  }

  std::cout << "anchor.requested_count=" << anchors.size() << '\n';
  std::cout << "anchor.found_count=" << foundCount << '\n';
  if (!stringMatchers.empty())
    printStringMatches(bytes, macho, stringMatchers, maxStringMatches);
  const bool success = foundCount == anchors.size();
  std::cout << "analysis.success=" << (success ? "true" : "false") << '\n';
  if (requireAnchor && !success)
    return 3;
  return 0;
}
