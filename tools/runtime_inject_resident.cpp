#include <BWAPI/Runtime/RuntimeProcess.h>
#include <BWAPI/Runtime/RuntimeProcessMemory.h>
#include <BWAPI/Runtime/RuntimeExecutor.h>

#include <chrono>
#include <cstring>
#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <mach/i386/thread_status.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/vm_region.h>
#include <libproc.h>
#endif

using namespace BWAPI::Runtime;

namespace
{
  constexpr std::uintptr_t DefaultSharedCacheBase = 0x7FF800000000;
  constexpr std::uintptr_t DefaultLibDyldTextBase = 0x7FF801ED9000;
  constexpr std::uintptr_t DefaultLibSystemPthreadTextBase = 0x7FF801ECD000;
  constexpr std::uintptr_t DefaultLibDyldDlopenOffset = 0x12fc;
  constexpr std::uintptr_t DefaultLibSystemPthreadExitOffset = 0x3c1d;
  constexpr std::uintptr_t LegacyDyldDlopenOffset = 0x306e6;
  constexpr std::uintptr_t LegacyDyldPthreadExitOffset = 0x61e9a;
  constexpr int DefaultDlopenFlags = 0x2 | 0x4; // RTLD_NOW | RTLD_LOCAL

  void printUsage()
  {
    std::cout
      << "usage: starcraft-runtime-inject-resident [options]\n"
      << "  --process-id <pid>             target StarCraft process id\n"
      << "  --self                         target this helper process (tests only)\n"
      << "  --adapter <path>               resident adapter dylib path\n"
      << "  --bridge <path>                ready bridge to wait for (default: /tmp/starcraft-api-live-bridge)\n"
      << "  --dlopen-address <addr>        override resolved public dlopen address\n"
      << "  --pthread-exit-address <addr>  override resolved public pthread_exit address\n"
      << "  --shared-cache-slide <addr>    override x86_64 dyld shared cache slide\n"
      << "  --dyld-base <addr>             legacy /usr/lib/dyld base; unsafe unless explicit public symbol addresses are used\n"
      << "  --dlopen-offset <addr>         override dlopen offset (default: public libdyld 0x12fc)\n"
      << "  --pthread-exit-offset <addr>   override pthread_exit offset (default: libsystem_pthread 0x3c1d)\n"
      << "  --wait-ready-ms <ms>           wait for resident ready file (default: 5000)\n"
      << "  --reset-bridge                 remove stale bridge contents before injection\n"
      << "  --require-ready                return non-zero unless ready matches the target pid\n"
      << "  --resolve-only                 validate resolved public ABI symbols without creating a remote thread\n"
      << "  --help                         show this help\n";
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

  bool parseNonNegativeInt(const std::string& value, int& output)
  {
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0' || parsed < 0 || parsed > std::numeric_limits<int>::max())
      return false;
    output = static_cast<int>(parsed);
    return true;
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

  bool parseAddressAllowZero(const std::string& value, std::uintptr_t& output)
  {
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value.c_str(), &end, 0);
    if (end == value.c_str() || *end != '\0')
      return false;
    output = static_cast<std::uintptr_t>(parsed);
    return static_cast<unsigned long long>(output) == parsed;
  }

  std::string hexAddress(std::uintptr_t address)
  {
    std::ostringstream output;
    output << "0x" << std::hex << address;
    return output.str();
  }

  bool readyFileContainsLine(const std::filesystem::path& readyPath, const std::string& expected)
  {
    std::ifstream ready(readyPath);
    std::string line;
    while (std::getline(ready, line))
    {
      if (line == expected)
        return true;
    }
    return false;
  }

  bool readyFileMatchesPid(const std::filesystem::path& bridgePath, int processId)
  {
    const std::filesystem::path readyPath = bridgePath / RuntimeExecutorBridgeReadyFile;
    return readyFileContainsLine(readyPath, "process_id=" + std::to_string(processId))
      && readyFileContainsLine(readyPath, "executor=starcraft-api-resident-adapter")
      && readyFileContainsLine(readyPath, "mode=validated-runtime-adapter");
  }

  bool mappedPathEndsWith(const std::string& mappedPath, const std::string& suffix)
  {
    return mappedPath.size() >= suffix.size()
      && mappedPath.compare(mappedPath.size() - suffix.size(), suffix.size(), suffix) == 0;
  }

  std::uintptr_t findMappedTextBase(
    int processId,
    const std::string& mappedPathSuffix,
    std::string& reason)
  {
    RuntimeMemoryRegionListResult regions = listProcessMemoryRegions(processId);
    if (!regions.success)
    {
      reason = regions.reason;
      return 0;
    }

    std::uintptr_t best = 0;
    for (const RuntimeMemoryRegion& region : regions.regions)
    {
      if (!region.executable || !region.readable)
        continue;
      if (!mappedPathEndsWith(region.mappedPath, mappedPathSuffix))
        continue;
      if (best == 0 || region.address < best)
        best = region.address;
    }

    if (best == 0)
      reason = "no executable mapping found for " + mappedPathSuffix;
    return best;
  }

  std::uintptr_t findSharedCacheSlide(int processId, std::string& reason)
  {
    RuntimeMemoryRegionListResult regions = listProcessMemoryRegions(processId);
    if (!regions.success)
    {
      reason = regions.reason;
      return 0;
    }

    std::uintptr_t best = 0;
    for (const RuntimeMemoryRegion& region : regions.regions)
    {
      if (!region.executable || !region.readable)
        continue;
      if (region.mappedPath.find("dyld_shared_cache") == std::string::npos)
        continue;
      if (best == 0 || region.address < best)
        best = region.address;
    }

    if (best == 0)
    {
      reason = "no executable dyld shared cache mapping found";
      return 0;
    }
    return best - DefaultSharedCacheBase;
  }

  std::uintptr_t resolveSharedCacheSymbolAddress(
    int processId,
    const std::string& mappedPathSuffix,
    std::uintptr_t staticTextBase,
    std::uintptr_t symbolOffset,
    bool sharedCacheSlideOverridden,
    std::uintptr_t sharedCacheSlide,
    std::string& source,
    std::string& reason)
  {
    const std::uintptr_t mappedBase = findMappedTextBase(processId, mappedPathSuffix, reason);
    if (mappedBase != 0)
    {
      source = "mapped-image:" + mappedPathSuffix;
      return mappedBase + symbolOffset;
    }

    if (!sharedCacheSlideOverridden)
    {
      std::string slideReason;
      const std::uintptr_t detectedSlide = findSharedCacheSlide(processId, slideReason);
      if (!slideReason.empty() && reason.empty())
        reason = slideReason;
      sharedCacheSlide = detectedSlide;
    }

    source = sharedCacheSlideOverridden
      ? "static-shared-cache-map+override-slide"
      : "static-shared-cache-map+detected-slide";
    return staticTextBase + sharedCacheSlide + symbolOffset;
  }

  std::uintptr_t resolveLocalPublicSymbolAddress(
    const char* symbolName,
    std::string& source,
    std::string& reason)
  {
    dlerror();
    void* symbol = dlsym(RTLD_DEFAULT, symbolName);
    const char* error = dlerror();
    if (symbol == nullptr || error != nullptr)
    {
      reason = std::string("local dlsym(") + symbolName + ") failed"
        + (error == nullptr ? "" : ": " + std::string(error));
      return 0;
    }

    source = std::string("local-dlsym-public-abi:") + symbolName;
    return reinterpret_cast<std::uintptr_t>(symbol);
  }

  bool regionContainsAddress(const RuntimeMemoryRegion& region, std::uintptr_t address)
  {
    return address >= region.address && address - region.address < region.size;
  }

  const RuntimeMemoryRegion* findRegionContainingAddress(
    const std::vector<RuntimeMemoryRegion>& regions,
    std::uintptr_t address)
  {
    for (const RuntimeMemoryRegion& region : regions)
    {
      if (regionContainsAddress(region, address))
        return &region;
    }
    return nullptr;
  }

  std::string protectionString(bool readable, bool writable, bool executable)
  {
    std::string protection;
    protection.push_back(readable ? 'r' : '-');
    protection.push_back(writable ? 'w' : '-');
    protection.push_back(executable ? 'x' : '-');
    return protection;
  }

  std::string protectionString(const RuntimeMemoryRegion& region)
  {
    return protectionString(region.readable, region.writable, region.executable);
  }

  struct AddressRegionEvidence
  {
    bool found = false;
    std::uintptr_t address = 0;
    std::size_t size = 0;
    bool readable = false;
    bool writable = false;
    bool executable = false;
    bool currentReadable = false;
    bool currentWritable = false;
    bool currentExecutable = false;
    bool maxReadable = false;
    bool maxWritable = false;
    bool maxExecutable = false;
    std::string mappedPath;
    std::string reason;
  };

  AddressRegionEvidence fallbackAddressRegionEvidence(
    std::uintptr_t address,
    const RuntimeMemoryRegionListResult& regions,
    const std::string& reason)
  {
    AddressRegionEvidence evidence;
    evidence.reason = reason;
    const RuntimeMemoryRegion* region =
      findRegionContainingAddress(regions.regions, address);
    if (region == nullptr)
      return evidence;

    evidence.found = true;
    evidence.address = region->address;
    evidence.size = region->size;
    evidence.currentReadable = region->readable;
    evidence.currentWritable = region->writable;
    evidence.currentExecutable = region->executable;
    evidence.maxReadable = region->readable;
    evidence.maxWritable = region->writable;
    evidence.maxExecutable = region->executable;
    evidence.readable = region->readable;
    evidence.writable = region->writable;
    evidence.executable = region->executable;
    evidence.mappedPath = region->mappedPath;
    return evidence;
  }

#if defined(__APPLE__)
  AddressRegionEvidence queryAddressRegionEvidence(
    int processId,
    std::uintptr_t targetAddress,
    const RuntimeMemoryRegionListResult& fallbackRegions)
  {
    mach_port_t task = MACH_PORT_NULL;
    if (processId == currentProcessId())
    {
      task = mach_task_self();
    }
    else
    {
      const kern_return_t taskResult = task_for_pid(mach_task_self(), processId, &task);
      if (taskResult != KERN_SUCCESS)
      {
        return fallbackAddressRegionEvidence(
          targetAddress,
          fallbackRegions,
          "precise mach_vm_region_recurse task_for_pid failed: " + std::string(mach_error_string(taskResult)));
      }
    }

    auto finish = [&](AddressRegionEvidence evidence)
    {
      if (task != MACH_PORT_NULL && processId != currentProcessId())
        mach_port_deallocate(mach_task_self(), task);
      return evidence;
    };

    mach_vm_address_t cursor = static_cast<mach_vm_address_t>(targetAddress);
    natural_t depth = 0;
    for (int guard = 0; guard < 4096; ++guard)
    {
      mach_vm_size_t regionSize = 0;
      vm_region_submap_info_data_64_t info;
      mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
      const kern_return_t regionResult = mach_vm_region_recurse(
        task,
        &cursor,
        &regionSize,
        &depth,
        reinterpret_cast<vm_region_recurse_info_t>(&info),
        &count);
      if (regionResult != KERN_SUCCESS)
      {
        return finish(fallbackAddressRegionEvidence(
          targetAddress,
          fallbackRegions,
          "precise mach_vm_region_recurse failed: " + std::string(mach_error_string(regionResult))));
      }
      if (regionSize == 0)
        break;

      const bool contains =
        targetAddress >= cursor && targetAddress - cursor < regionSize;
      if (info.is_submap)
      {
        if (contains)
        {
          ++depth;
          cursor = static_cast<mach_vm_address_t>(targetAddress);
          continue;
        }
        if (cursor > targetAddress)
          break;
        const mach_vm_address_t next = cursor + regionSize;
        if (next <= cursor)
          break;
        cursor = next;
        continue;
      }

      if (contains)
      {
        AddressRegionEvidence evidence;
        evidence.found = true;
        evidence.address = static_cast<std::uintptr_t>(cursor);
        evidence.size = static_cast<std::size_t>(regionSize);
        evidence.currentReadable = (info.protection & VM_PROT_READ) != 0;
        evidence.currentWritable = (info.protection & VM_PROT_WRITE) != 0;
        evidence.currentExecutable = (info.protection & VM_PROT_EXECUTE) != 0;
        evidence.maxReadable = (info.max_protection & VM_PROT_READ) != 0;
        evidence.maxWritable = (info.max_protection & VM_PROT_WRITE) != 0;
        evidence.maxExecutable = (info.max_protection & VM_PROT_EXECUTE) != 0;
        evidence.readable = evidence.currentReadable || evidence.maxReadable;
        evidence.writable = evidence.currentWritable || evidence.maxWritable;
        evidence.executable = evidence.currentExecutable || evidence.maxExecutable;

        char mappedPath[PROC_PIDPATHINFO_MAXSIZE] = {};
        if (proc_regionfilename(processId, cursor, mappedPath, sizeof(mappedPath)) > 0)
          evidence.mappedPath = mappedPath;
        return finish(evidence);
      }

      if (cursor > targetAddress)
        break;
      const mach_vm_address_t next = cursor + regionSize;
      if (next <= cursor)
        break;
      cursor = next;
    }

    return finish(fallbackAddressRegionEvidence(
      targetAddress,
      fallbackRegions,
      "precise mach_vm_region_recurse did not find the address"));
  }
#else
  AddressRegionEvidence queryAddressRegionEvidence(
    int,
    std::uintptr_t targetAddress,
    const RuntimeMemoryRegionListResult& fallbackRegions)
  {
    return fallbackAddressRegionEvidence(targetAddress, fallbackRegions, {});
  }
#endif

  bool sourceIsPublicAbiSafe(const std::string& source)
  {
    return source == "explicit"
      || source.rfind("local-dlsym-public-abi:", 0) == 0;
  }

  bool validateResolvedPublicAbiAddress(
    const char* name,
    int processId,
    std::uintptr_t address,
    const std::string& source,
    const RuntimeMemoryRegionListResult& regions)
  {
    bool valid = true;
    const std::string key = std::string("inject.") + name;
    const bool sourceSafe = sourceIsPublicAbiSafe(source);
    std::cout << key << "_abi_safe=" << (sourceSafe ? "true" : "false") << '\n';
    if (!sourceSafe)
    {
      std::cout << key << "_abi_reason=unsafe symbol source: " << source
                << "; only local dlsym public ABI symbols or explicit verified public symbol addresses are allowed\n";
      valid = false;
    }

    const AddressRegionEvidence region =
      queryAddressRegionEvidence(processId, address, regions);
    std::cout << key << "_region.found=" << (region.found ? "true" : "false") << '\n';
    if (!region.reason.empty())
      std::cout << key << "_region.probe_reason=" << region.reason << '\n';
    if (!region.found)
    {
      std::cout << key << "_region.reason=address is not inside a discovered target mapping\n";
      return false;
    }

    std::cout << key << "_region.address=" << hexAddress(region.address) << '\n';
    std::cout << key << "_region.size=" << region.size << '\n';
    std::cout << key << "_region.protection="
              << protectionString(region.currentReadable, region.currentWritable, region.currentExecutable) << '\n';
    std::cout << key << "_region.max_protection="
              << protectionString(region.maxReadable, region.maxWritable, region.maxExecutable) << '\n';
    std::cout << key << "_region.readable=" << (region.readable ? "true" : "false") << '\n';
    std::cout << key << "_region.writable=" << (region.writable ? "true" : "false") << '\n';
    std::cout << key << "_region.executable=" << (region.executable ? "true" : "false") << '\n';
    std::cout << key << "_region.mapped_path="
              << (region.mappedPath.empty() ? "<anonymous>" : region.mappedPath) << '\n';
    if (!region.readable || !region.executable)
    {
      std::cout << key << "_region.reason=address is not in a readable executable target mapping\n";
      valid = false;
    }

    return valid;
  }

#if defined(__APPLE__)
#if __DARWIN_UNIX03
#define SCAPI_X86_RDI __rdi
#define SCAPI_X86_RSI __rsi
#define SCAPI_X86_RBP __rbp
#define SCAPI_X86_RSP __rsp
#define SCAPI_X86_RIP __rip
#define SCAPI_X86_RFLAGS __rflags
#define SCAPI_X86_CS __cs
#define SCAPI_X86_FS __fs
#define SCAPI_X86_GS __gs
#else
#define SCAPI_X86_RDI rdi
#define SCAPI_X86_RSI rsi
#define SCAPI_X86_RBP rbp
#define SCAPI_X86_RSP rsp
#define SCAPI_X86_RIP rip
#define SCAPI_X86_RFLAGS rflags
#define SCAPI_X86_CS cs
#define SCAPI_X86_FS fs
#define SCAPI_X86_GS gs
#endif

  std::string machError(const char* operation, kern_return_t result)
  {
    return std::string(operation) + " failed: " + mach_error_string(result);
  }

  void printThreadFlavorList(thread_act_t thread, mach_msg_type_number_t index)
  {
    thread_state_data_t flavors;
    std::memset(&flavors, 0, sizeof(flavors));
    mach_msg_type_number_t flavorCount = THREAD_STATE_MAX;
    const kern_return_t result = thread_get_state(
      thread,
      THREAD_STATE_FLAVOR_LIST,
      reinterpret_cast<thread_state_t>(&flavors),
      &flavorCount);

    std::cout << "inject.thread_state.existing.thread." << index
              << ".flavor_list_result=" << mach_error_string(result) << '\n';
    std::cout << "inject.thread_state.existing.thread." << index
              << ".flavor_list_count=" << flavorCount << '\n';
    if (result != KERN_SUCCESS)
      return;

    std::cout << "inject.thread_state.existing.thread." << index
              << ".flavor_list=";
    for (mach_msg_type_number_t flavorIndex = 0; flavorIndex < flavorCount; ++flavorIndex)
    {
      if (flavorIndex != 0)
        std::cout << ',';
      std::cout << flavors[flavorIndex];
    }
    std::cout << '\n';
  }

  bool readExistingX86ThreadState(mach_port_t task, x86_thread_state64_t& state)
  {
    thread_act_array_t threads = nullptr;
    mach_msg_type_number_t threadCount = 0;
    kern_return_t result = task_threads(task, &threads, &threadCount);
    if (result != KERN_SUCCESS)
    {
      std::cout << "inject.thread_state.existing.task_threads_result="
                << mach_error_string(result) << '\n';
      return false;
    }

    std::cout << "inject.thread_state.existing.thread_count=" << threadCount << '\n';
    bool found = false;
    for (mach_msg_type_number_t i = 0; i < threadCount && !found; ++i)
    {
      mach_msg_type_number_t stateCount = x86_THREAD_STATE64_COUNT;
      x86_thread_state64_t candidate;
      std::memset(&candidate, 0, sizeof(candidate));
      const kern_return_t suspendResult = thread_suspend(threads[i]);
      std::cout << "inject.thread_state.existing.thread." << i
                << ".suspend_result=" << mach_error_string(suspendResult) << '\n';
      if (suspendResult != KERN_SUCCESS)
        continue;
      result = thread_get_state(
        threads[i],
        x86_THREAD_STATE64,
        reinterpret_cast<thread_state_t>(&candidate),
        &stateCount);
      if (result != KERN_SUCCESS)
      {
        x86_thread_state_t selfDescribingState;
        std::memset(&selfDescribingState, 0, sizeof(selfDescribingState));
        mach_msg_type_number_t selfDescribingCount = x86_THREAD_STATE_COUNT;
        const kern_return_t selfDescribingResult = thread_get_state(
          threads[i],
          x86_THREAD_STATE,
          reinterpret_cast<thread_state_t>(&selfDescribingState),
          &selfDescribingCount);
        std::cout << "inject.thread_state.existing.thread." << i
                  << ".get_self_describing_state_result="
                  << mach_error_string(selfDescribingResult) << '\n';
        std::cout << "inject.thread_state.existing.thread." << i
                  << ".self_describing_state_count=" << selfDescribingCount << '\n';
        std::cout << "inject.thread_state.existing.thread." << i
                  << ".self_describing_state_flavor="
                  << selfDescribingState.tsh.flavor << '\n';
        std::cout << "inject.thread_state.existing.thread." << i
                  << ".self_describing_state_inner_count="
                  << selfDescribingState.tsh.count << '\n';
        if (selfDescribingResult == KERN_SUCCESS
          && selfDescribingState.tsh.flavor == x86_THREAD_STATE64)
        {
          candidate = selfDescribingState.uts.ts64;
          stateCount = x86_THREAD_STATE64_COUNT;
          result = KERN_SUCCESS;
        }
      }
      const kern_return_t resumeResult = thread_resume(threads[i]);
      std::cout << "inject.thread_state.existing.thread." << i
                << ".get_state_result=" << mach_error_string(result) << '\n';
      std::cout << "inject.thread_state.existing.thread." << i
                << ".state_count=" << stateCount << '\n';
      std::cout << "inject.thread_state.existing.thread." << i
                << ".resume_result=" << mach_error_string(resumeResult) << '\n';
      if (result != KERN_SUCCESS && i < 4)
        printThreadFlavorList(threads[i], i);
      if (result == KERN_SUCCESS && stateCount == x86_THREAD_STATE64_COUNT)
      {
        state = candidate;
        found = true;
      }
    }

    for (mach_msg_type_number_t i = 0; i < threadCount; ++i)
      mach_port_deallocate(mach_task_self(), threads[i]);
    if (threads != nullptr)
    {
      vm_deallocate(
        mach_task_self(),
        reinterpret_cast<vm_address_t>(threads),
        static_cast<vm_size_t>(threadCount * sizeof(thread_act_t)));
    }
    return found;
  }

  int injectResidentOnMacOS(
    int processId,
    const std::filesystem::path& adapterPath,
    std::uintptr_t dlopenAddress,
    std::uintptr_t pthreadExitAddress)
  {
    mach_port_t task = MACH_PORT_NULL;
    kern_return_t result = task_for_pid(mach_task_self(), processId, &task);
    if (result != KERN_SUCCESS)
    {
      std::cerr << machError("task_for_pid", result) << '\n';
      return 1;
    }

    auto deallocateRemote = [&](mach_vm_address_t address, mach_vm_size_t size)
    {
      if (address != 0 && size != 0)
        mach_vm_deallocate(task, address, size);
    };

    const std::string adapterPathText = adapterPath.string();
    mach_vm_address_t remotePath = 0;
    const mach_vm_size_t remotePathSize =
      static_cast<mach_vm_size_t>(adapterPathText.size() + 1);
    result = mach_vm_allocate(task, &remotePath, remotePathSize, VM_FLAGS_ANYWHERE);
    if (result != KERN_SUCCESS)
    {
      std::cerr << machError("mach_vm_allocate(path)", result) << '\n';
      return 1;
    }

    result = mach_vm_write(
      task,
      remotePath,
      reinterpret_cast<vm_offset_t>(const_cast<char*>(adapterPathText.c_str())),
      static_cast<mach_msg_type_number_t>(remotePathSize));
    if (result != KERN_SUCCESS)
    {
      std::cerr << machError("mach_vm_write(path)", result) << '\n';
      deallocateRemote(remotePath, remotePathSize);
      return 1;
    }

    constexpr mach_vm_size_t StackSize = 64 * 1024;
    mach_vm_address_t remoteStack = 0;
    result = mach_vm_allocate(task, &remoteStack, StackSize, VM_FLAGS_ANYWHERE);
    if (result != KERN_SUCCESS)
    {
      std::cerr << machError("mach_vm_allocate(stack)", result) << '\n';
      deallocateRemote(remotePath, remotePathSize);
      return 1;
    }

    const std::uint64_t stackTop =
      static_cast<std::uint64_t>((remoteStack + StackSize) & ~mach_vm_address_t { 0xf });
    const std::uint64_t stackPointer = stackTop - sizeof(std::uint64_t);
    const std::uint64_t returnAddress = static_cast<std::uint64_t>(pthreadExitAddress);
    result = mach_vm_write(
      task,
      static_cast<mach_vm_address_t>(stackPointer),
      reinterpret_cast<vm_offset_t>(const_cast<std::uint64_t*>(&returnAddress)),
      sizeof(returnAddress));
    if (result != KERN_SUCCESS)
    {
      std::cerr << machError("mach_vm_write(stack-return)", result) << '\n';
      deallocateRemote(remoteStack, StackSize);
      deallocateRemote(remotePath, remotePathSize);
      return 1;
    }

    x86_thread_state64_t state;
    std::memset(&state, 0, sizeof(state));
    x86_thread_state64_t existingState;
    std::memset(&existingState, 0, sizeof(existingState));
    const bool copiedExistingState = readExistingX86ThreadState(task, existingState);
    if (copiedExistingState)
    {
      state.SCAPI_X86_CS = existingState.SCAPI_X86_CS;
      state.SCAPI_X86_FS = existingState.SCAPI_X86_FS;
      state.SCAPI_X86_GS = existingState.SCAPI_X86_GS;
      state.SCAPI_X86_RFLAGS = existingState.SCAPI_X86_RFLAGS;
    }
    state.SCAPI_X86_RIP = static_cast<std::uint64_t>(dlopenAddress);
    state.SCAPI_X86_RDI = static_cast<std::uint64_t>(remotePath);
    state.SCAPI_X86_RSI = static_cast<std::uint64_t>(DefaultDlopenFlags);
    state.SCAPI_X86_RSP = static_cast<std::uint64_t>(stackPointer);
    state.SCAPI_X86_RBP = 0;
    if (state.SCAPI_X86_CS == 0)
      state.SCAPI_X86_CS = 0x2b;
    if (state.SCAPI_X86_RFLAGS == 0)
      state.SCAPI_X86_RFLAGS = 0x202;

    std::cout << "inject.thread_state.copied_existing="
              << (copiedExistingState ? "true" : "false") << '\n';
    std::cout << "inject.thread_state.cs="
              << hexAddress(static_cast<std::uintptr_t>(state.SCAPI_X86_CS)) << '\n';
    std::cout << "inject.thread_state.fs=" << hexAddress(static_cast<std::uintptr_t>(state.SCAPI_X86_FS)) << '\n';
    std::cout << "inject.thread_state.gs=" << hexAddress(static_cast<std::uintptr_t>(state.SCAPI_X86_GS)) << '\n';
    std::cout << "inject.thread_state.rflags="
              << hexAddress(static_cast<std::uintptr_t>(state.SCAPI_X86_RFLAGS)) << '\n';

    thread_act_t thread = MACH_PORT_NULL;
    auto createRemoteThread = [&]()
    {
      return thread_create_running(
        task,
        x86_THREAD_STATE64,
        reinterpret_cast<thread_state_t>(&state),
        x86_THREAD_STATE64_COUNT,
        &thread);
    };
    const kern_return_t suspendResult = task_suspend(task);
    std::cout << "inject.task_suspended_for_thread_create="
              << (suspendResult == KERN_SUCCESS ? "true" : "false") << '\n';
    if (suspendResult != KERN_SUCCESS)
      std::cout << "inject.task_suspend_reason="
                << machError("task_suspend", suspendResult) << '\n';
    result = createRemoteThread();
    if (result == KERN_INVALID_ARGUMENT && !copiedExistingState && state.SCAPI_X86_CS == 0x2b)
    {
      state.SCAPI_X86_CS = 0x17;
      std::cout << "inject.thread_state.retry_cs="
                << hexAddress(static_cast<std::uintptr_t>(state.SCAPI_X86_CS)) << '\n';
      result = createRemoteThread();
    }
    if (result == KERN_INVALID_ARGUMENT)
    {
      x86_thread_state_t selfDescribingState;
      std::memset(&selfDescribingState, 0, sizeof(selfDescribingState));
      selfDescribingState.tsh.flavor = x86_THREAD_STATE64;
      selfDescribingState.tsh.count = x86_THREAD_STATE64_COUNT;
      selfDescribingState.uts.ts64 = state;
      std::cout << "inject.thread_state.retry_flavor=x86_THREAD_STATE\n";
      result = thread_create_running(
        task,
        x86_THREAD_STATE,
        reinterpret_cast<thread_state_t>(&selfDescribingState),
        x86_THREAD_STATE_COUNT,
        &thread);
    }
    if (result == KERN_INVALID_ARGUMENT)
    {
      std::cout << "inject.thread_state.retry_create_set_resume=true\n";
      result = thread_create(task, &thread);
      std::cout << "inject.thread_create.result=" << mach_error_string(result) << '\n';
      if (result == KERN_SUCCESS)
      {
        result = thread_set_state(
          thread,
          x86_THREAD_STATE64,
          reinterpret_cast<thread_state_t>(&state),
          x86_THREAD_STATE64_COUNT);
        std::cout << "inject.thread_set_state.result=" << mach_error_string(result) << '\n';
        if (result == KERN_SUCCESS)
        {
          result = thread_resume(thread);
          std::cout << "inject.thread_resume.result=" << mach_error_string(result) << '\n';
        }
      }
    }
    if (suspendResult == KERN_SUCCESS)
      task_resume(task);
    if (result != KERN_SUCCESS)
    {
      if (thread != MACH_PORT_NULL)
      {
        const kern_return_t terminateResult = thread_terminate(thread);
        std::cout << "inject.thread_terminate_after_failure.result="
                  << mach_error_string(terminateResult) << '\n';
        mach_port_deallocate(mach_task_self(), thread);
      }
      std::cerr << machError("thread_create_running(x86_THREAD_STATE64)", result) << '\n';
      deallocateRemote(remoteStack, StackSize);
      deallocateRemote(remotePath, remotePathSize);
      return 1;
    }

    std::cout << "inject.thread_created=true\n";
    std::cout << "inject.remote_path=" << hexAddress(static_cast<std::uintptr_t>(remotePath)) << '\n';
    std::cout << "inject.remote_stack=" << hexAddress(static_cast<std::uintptr_t>(remoteStack)) << '\n';
    if (thread != MACH_PORT_NULL)
      mach_port_deallocate(mach_task_self(), thread);
    return 0;
  }
#undef SCAPI_X86_RDI
#undef SCAPI_X86_RSI
#undef SCAPI_X86_RBP
#undef SCAPI_X86_RSP
#undef SCAPI_X86_RIP
#undef SCAPI_X86_RFLAGS
#undef SCAPI_X86_CS
#undef SCAPI_X86_FS
#undef SCAPI_X86_GS
#endif
}

int main(int argc, char** argv)
{
  int processId = 0;
  int waitReadyMs = 5000;
  bool requireReady = false;
  bool resetBridge = false;
  bool selfTarget = false;
  bool resolveOnly = false;
  bool dlopenAddressOverridden = false;
  bool pthreadExitAddressOverridden = false;
  bool sharedCacheSlideOverridden = false;
  bool dlopenOffsetOverridden = false;
  bool pthreadExitOffsetOverridden = false;
  std::uintptr_t dyldBase = 0;
  std::uintptr_t dlopenAddress = 0;
  std::uintptr_t pthreadExitAddress = 0;
  std::uintptr_t sharedCacheSlide = 0;
  std::uintptr_t dlopenOffset = DefaultLibDyldDlopenOffset;
  std::uintptr_t pthreadExitOffset = DefaultLibSystemPthreadExitOffset;
  std::filesystem::path adapterPath;
  std::filesystem::path bridgePath = "/tmp/starcraft-api-live-bridge";

  for (int i = 1; i < argc; ++i)
  {
    const std::string arg = argv[i];
    if (arg == "--process-id")
    {
      if (i + 1 >= argc || !parsePositiveInt(argv[++i], processId))
      {
        std::cerr << "--process-id requires a positive integer\n";
        return 64;
      }
    }
    else if (arg == "--self")
    {
      selfTarget = true;
    }
    else if (arg == "--adapter")
    {
      if (i + 1 >= argc)
      {
        std::cerr << "--adapter requires a path\n";
        return 64;
      }
      adapterPath = argv[++i];
    }
    else if (arg == "--bridge")
    {
      if (i + 1 >= argc)
      {
        std::cerr << "--bridge requires a path\n";
        return 64;
      }
      bridgePath = argv[++i];
    }
    else if (arg == "--dyld-base")
    {
      if (i + 1 >= argc || !parseAddress(argv[++i], dyldBase))
      {
        std::cerr << "--dyld-base requires a positive integer address\n";
        return 64;
      }
    }
    else if (arg == "--dlopen-address")
    {
      if (i + 1 >= argc || !parseAddress(argv[++i], dlopenAddress))
      {
        std::cerr << "--dlopen-address requires a positive integer address\n";
        return 64;
      }
      dlopenAddressOverridden = true;
    }
    else if (arg == "--pthread-exit-address")
    {
      if (i + 1 >= argc || !parseAddress(argv[++i], pthreadExitAddress))
      {
        std::cerr << "--pthread-exit-address requires a positive integer address\n";
        return 64;
      }
      pthreadExitAddressOverridden = true;
    }
    else if (arg == "--shared-cache-slide")
    {
      if (i + 1 >= argc || !parseAddressAllowZero(argv[++i], sharedCacheSlide))
      {
        std::cerr << "--shared-cache-slide requires a non-negative integer address\n";
        return 64;
      }
      sharedCacheSlideOverridden = true;
    }
    else if (arg == "--dlopen-offset")
    {
      if (i + 1 >= argc || !parseAddress(argv[++i], dlopenOffset))
      {
        std::cerr << "--dlopen-offset requires a positive integer address\n";
        return 64;
      }
      dlopenOffsetOverridden = true;
    }
    else if (arg == "--pthread-exit-offset")
    {
      if (i + 1 >= argc || !parseAddress(argv[++i], pthreadExitOffset))
      {
        std::cerr << "--pthread-exit-offset requires a positive integer address\n";
        return 64;
      }
      pthreadExitOffsetOverridden = true;
    }
    else if (arg == "--wait-ready-ms")
    {
      if (i + 1 >= argc || !parseNonNegativeInt(argv[++i], waitReadyMs))
      {
        std::cerr << "--wait-ready-ms requires a non-negative integer\n";
        return 64;
      }
    }
    else if (arg == "--reset-bridge")
    {
      resetBridge = true;
    }
    else if (arg == "--require-ready")
    {
      requireReady = true;
    }
    else if (arg == "--resolve-only")
    {
      resolveOnly = true;
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

  if (selfTarget)
    processId = currentProcessId();

  if (processId <= 0)
  {
    std::cerr << "--process-id is required\n";
    return 64;
  }
  if (adapterPath.empty())
  {
    std::cerr << "--adapter is required\n";
    return 64;
  }

  std::error_code error;
  adapterPath = std::filesystem::absolute(adapterPath, error).lexically_normal();
  if (error || !std::filesystem::is_regular_file(adapterPath, error) || error)
  {
    std::cerr << "adapter dylib does not exist: " << adapterPath << '\n';
    return 64;
  }

  std::cout << "inject.process_id=" << processId << '\n';
  std::cout << "inject.adapter=" << adapterPath.string() << '\n';
  std::cout << "inject.bridge=" << bridgePath.string() << '\n';

  if (resetBridge)
  {
    std::filesystem::remove_all(bridgePath, error);
    if (error)
    {
      std::cerr << "unable to reset bridge: " << error.message() << '\n';
      return 1;
    }
  }
  std::filesystem::create_directories(bridgePath, error);
  if (error)
  {
    std::cerr << "unable to create bridge: " << error.message() << '\n';
    return 1;
  }

  std::string dlopenSource;
  std::string pthreadExitSource;
  if (dyldBase != 0)
  {
    if (!dlopenOffsetOverridden)
      dlopenOffset = LegacyDyldDlopenOffset;
    if (!pthreadExitOffsetOverridden)
      pthreadExitOffset = LegacyDyldPthreadExitOffset;
    if (!dlopenAddressOverridden)
    {
      dlopenAddress = dyldBase + dlopenOffset;
      dlopenSource = "legacy-/usr/lib/dyld";
    }
    else
    {
      dlopenSource = "explicit";
    }
    if (!pthreadExitAddressOverridden)
    {
      pthreadExitAddress = dyldBase + pthreadExitOffset;
      pthreadExitSource = "legacy-/usr/lib/dyld";
    }
    else
    {
      pthreadExitSource = "explicit";
    }
  }
  else
  {
    if (!dlopenAddressOverridden)
    {
      std::string reason;
      dlopenAddress = resolveLocalPublicSymbolAddress("dlopen", dlopenSource, reason);
      if (!reason.empty())
        std::cout << "inject.dlopen_resolve_note=" << reason << '\n';
      if (dlopenAddress == 0)
      {
        reason.clear();
        dlopenAddress = resolveSharedCacheSymbolAddress(
          processId,
          "/usr/lib/system/libdyld.dylib",
          DefaultLibDyldTextBase,
          dlopenOffset,
          sharedCacheSlideOverridden,
          sharedCacheSlide,
          dlopenSource,
          reason);
        if (!reason.empty())
          std::cout << "inject.dlopen_resolve_note=" << reason << '\n';
      }
    }
    else
    {
      dlopenSource = "explicit";
    }

    if (!pthreadExitAddressOverridden)
    {
      std::string reason;
      pthreadExitAddress = resolveLocalPublicSymbolAddress("pthread_exit", pthreadExitSource, reason);
      if (!reason.empty())
        std::cout << "inject.pthread_exit_resolve_note=" << reason << '\n';
      if (pthreadExitAddress == 0)
      {
        reason.clear();
        pthreadExitAddress = resolveSharedCacheSymbolAddress(
          processId,
          "/usr/lib/system/libsystem_pthread.dylib",
          DefaultLibSystemPthreadTextBase,
          pthreadExitOffset,
          sharedCacheSlideOverridden,
          sharedCacheSlide,
          pthreadExitSource,
          reason);
        if (!reason.empty())
          std::cout << "inject.pthread_exit_resolve_note=" << reason << '\n';
      }
    }
    else
    {
      pthreadExitSource = "explicit";
    }
  }

  if (dyldBase != 0)
    std::cout << "inject.dyld_base=" << hexAddress(dyldBase) << '\n';
  std::cout << "inject.dlopen_source=" << dlopenSource << '\n';
  std::cout << "inject.pthread_exit_source=" << pthreadExitSource << '\n';
  std::cout << "inject.dlopen_address=" << hexAddress(dlopenAddress) << '\n';
  std::cout << "inject.pthread_exit_address=" << hexAddress(pthreadExitAddress) << '\n';

  RuntimeMemoryRegionListResult regions = listProcessMemoryRegions(processId);
  std::cout << "inject.region_list.success=" << (regions.success ? "true" : "false") << '\n';
  if (!regions.reason.empty())
    std::cout << "inject.region_list.reason=" << regions.reason << '\n';
  if (!regions.success)
  {
    std::cout << "inject.aborted_before_thread_create=true\n";
    return 1;
  }

  const bool dlopenAddressValid =
    validateResolvedPublicAbiAddress("dlopen", processId, dlopenAddress, dlopenSource, regions);
  const bool pthreadExitAddressValid =
    validateResolvedPublicAbiAddress("pthread_exit", processId, pthreadExitAddress, pthreadExitSource, regions);
  if (!dlopenAddressValid || !pthreadExitAddressValid)
  {
    std::cout << "inject.aborted_before_thread_create=true\n";
    return 1;
  }

  RuntimeMemoryReadResult dlopenRead = readProcessMemory(processId, dlopenAddress, 8);
  std::cout << "inject.dlopen_readable=" << (dlopenRead.success ? "true" : "false") << '\n';
  if (!dlopenRead.reason.empty())
    std::cout << "inject.dlopen_read_reason=" << dlopenRead.reason << '\n';
  if (!dlopenRead.success)
    return 1;

  RuntimeMemoryReadResult pthreadExitRead = readProcessMemory(processId, pthreadExitAddress, 8);
  std::cout << "inject.pthread_exit_readable=" << (pthreadExitRead.success ? "true" : "false") << '\n';
  if (!pthreadExitRead.reason.empty())
    std::cout << "inject.pthread_exit_read_reason=" << pthreadExitRead.reason << '\n';
  if (!pthreadExitRead.success)
    return 1;

  if (resolveOnly)
  {
    std::cout << "inject.resolve_only=true\n";
    std::cout << "inject.aborted_before_thread_create=true\n";
    return 0;
  }

#if defined(__APPLE__)
  const int injectResult =
    injectResidentOnMacOS(processId, adapterPath, dlopenAddress, pthreadExitAddress);
  if (injectResult != 0)
    return injectResult;
#else
  std::cerr << "resident injection is currently implemented only on macOS\n";
  return 1;
#endif

  bool ready = readyFileMatchesPid(bridgePath, processId);
  const auto deadline =
    std::chrono::steady_clock::now() + std::chrono::milliseconds(waitReadyMs);
  while (!ready && std::chrono::steady_clock::now() < deadline)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ready = readyFileMatchesPid(bridgePath, processId);
  }

  std::cout << "inject.ready=" << (ready ? "true" : "false") << '\n';
  std::cout << "inject.ready_path="
            << (bridgePath / RuntimeExecutorBridgeReadyFile).string() << '\n';

  if (requireReady && !ready)
    return 2;
  return 0;
}
