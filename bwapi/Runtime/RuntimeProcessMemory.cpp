#include <BWAPI/Runtime/RuntimeProcessMemory.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <sstream>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/vm_region.h>
#include <unistd.h>
#elif defined(__linux__)
#include <algorithm>
#include <fstream>
#include <fcntl.h>
#include <sys/uio.h>
#include <unistd.h>
#endif

namespace BWAPI::Runtime
{
  namespace
  {
    RuntimeMemoryReadResult failure(std::string reason)
    {
      RuntimeMemoryReadResult result;
      result.reason = std::move(reason);
      return result;
    }

    RuntimeMemoryWriteResult writeFailure(std::string reason)
    {
      RuntimeMemoryWriteResult result;
      result.reason = std::move(reason);
      return result;
    }

    RuntimeMemoryAccessResult accessFailure(std::string reason)
    {
      RuntimeMemoryAccessResult result;
      result.reason = std::move(reason);
      return result;
    }

    RuntimeMemoryRegionResult regionFailure(std::string reason)
    {
      RuntimeMemoryRegionResult result;
      result.reason = std::move(reason);
      return result;
    }

    std::string errnoMessage(const char* operation)
    {
      std::ostringstream message;
      message << operation << " failed: " << std::strerror(errno);
      return message.str();
    }

#if defined(__APPLE__)
    std::string taskForPidFailureMessage(kern_return_t result)
    {
      return "task_for_pid failed: " + std::string(mach_error_string(result))
        + "; macOS denied target task access, so use an authorized debugger-signed helper "
        + "or approved in-process runtime adapter before claiming BWAPI parity";
    }
#endif

#if defined(_WIN32)
    bool windowsPageReadable(DWORD protect)
    {
      if ((protect & PAGE_GUARD) != 0 || (protect & PAGE_NOACCESS) != 0)
        return false;

      const DWORD baseProtect = protect & 0xff;
      return baseProtect == PAGE_READONLY
        || baseProtect == PAGE_READWRITE
        || baseProtect == PAGE_WRITECOPY
        || baseProtect == PAGE_EXECUTE_READ
        || baseProtect == PAGE_EXECUTE_READWRITE
        || baseProtect == PAGE_EXECUTE_WRITECOPY;
    }

    bool windowsPageWritable(DWORD protect)
    {
      if ((protect & PAGE_GUARD) != 0 || (protect & PAGE_NOACCESS) != 0)
        return false;

      const DWORD baseProtect = protect & 0xff;
      return baseProtect == PAGE_READWRITE
        || baseProtect == PAGE_WRITECOPY
        || baseProtect == PAGE_EXECUTE_READWRITE
        || baseProtect == PAGE_EXECUTE_WRITECOPY;
    }

    bool windowsPageExecutable(DWORD protect)
    {
      if ((protect & PAGE_GUARD) != 0 || (protect & PAGE_NOACCESS) != 0)
        return false;

      const DWORD baseProtect = protect & 0xff;
      return baseProtect == PAGE_EXECUTE
        || baseProtect == PAGE_EXECUTE_READ
        || baseProtect == PAGE_EXECUTE_READWRITE
        || baseProtect == PAGE_EXECUTE_WRITECOPY;
    }
#endif
  }

  int currentProcessId()
  {
#if defined(_WIN32)
    return static_cast<int>(GetCurrentProcessId());
#elif defined(__APPLE__) || defined(__linux__)
    return static_cast<int>(getpid());
#else
    return 0;
#endif
  }

  RuntimeMemoryAccessResult openProcessMemoryAccess(int processId)
  {
    if (processId <= 0)
      return accessFailure("process id must be positive");

#if defined(_WIN32)
    HANDLE process = OpenProcess(
      PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION,
      FALSE,
      static_cast<DWORD>(processId));
    if (process == nullptr)
      return accessFailure("OpenProcess VM access failed");
    CloseHandle(process);

    RuntimeMemoryAccessResult result;
    result.accessible = true;
    return result;
#elif defined(__APPLE__)
    if (processId == currentProcessId())
    {
      RuntimeMemoryAccessResult result;
      result.accessible = true;
      return result;
    }

    mach_port_t task = MACH_PORT_NULL;
    const kern_return_t taskResult = task_for_pid(mach_task_self(), processId, &task);
    if (taskResult != KERN_SUCCESS)
      return accessFailure(taskForPidFailureMessage(taskResult));
    if (task != MACH_PORT_NULL)
      mach_port_deallocate(mach_task_self(), task);

    RuntimeMemoryAccessResult result;
    result.accessible = true;
    return result;
#elif defined(__linux__)
    if (processId == currentProcessId())
    {
      RuntimeMemoryAccessResult result;
      result.accessible = true;
      return result;
    }

    const std::string memPath = "/proc/" + std::to_string(processId) + "/mem";
    const int fd = open(memPath.c_str(), O_RDONLY);
    if (fd < 0)
      return accessFailure(errnoMessage("open /proc/<pid>/mem"));
    close(fd);

    RuntimeMemoryAccessResult result;
    result.accessible = true;
    return result;
#else
    return accessFailure("process memory access is unsupported on this platform");
#endif
  }

  RuntimeMemoryRegionListResult listProcessMemoryRegions(int processId)
  {
    RuntimeMemoryRegionListResult result;
    if (processId <= 0)
    {
      result.reason = "process id must be positive";
      return result;
    }

#if defined(_WIN32)
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, static_cast<DWORD>(processId));
    if (process == nullptr)
    {
      result.reason = "OpenProcess query failed";
      return result;
    }

    unsigned char* address = nullptr;
    MEMORY_BASIC_INFORMATION info;
    while (VirtualQueryEx(process, address, &info, sizeof(info)) == sizeof(info))
    {
      if (info.State == MEM_COMMIT && info.RegionSize > 0)
      {
        RuntimeMemoryRegion region;
        region.address = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        region.size = static_cast<std::size_t>(info.RegionSize);
        region.readable = windowsPageReadable(info.Protect);
        region.writable = windowsPageWritable(info.Protect);
        region.executable = windowsPageExecutable(info.Protect);
        result.regions.push_back(region);
      }

      const auto base = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
      const auto next = base + info.RegionSize;
      if (next <= base)
        break;
      address = reinterpret_cast<unsigned char*>(next);
    }

    CloseHandle(process);
    result.success = true;
    return result;
#elif defined(__APPLE__)
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
        result.reason = taskForPidFailureMessage(taskResult);
        return result;
      }
    }

    mach_vm_address_t address = 1;
    while (true)
    {
      mach_vm_size_t regionSize = 0;
      vm_region_basic_info_data_64_t info;
      mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
      mach_port_t objectName = MACH_PORT_NULL;

      const kern_return_t regionResult = mach_vm_region(
        task,
        &address,
        &regionSize,
        VM_REGION_BASIC_INFO_64,
        reinterpret_cast<vm_region_info_t>(&info),
        &count,
        &objectName);

      if (objectName != MACH_PORT_NULL)
        mach_port_deallocate(mach_task_self(), objectName);

      if (regionResult != KERN_SUCCESS)
        break;

      if (regionSize > 0)
      {
        RuntimeMemoryRegion region;
        region.address = static_cast<std::uintptr_t>(address);
        region.size = static_cast<std::size_t>(regionSize);
        region.readable = (info.protection & VM_PROT_READ) != 0;
        region.writable = (info.protection & VM_PROT_WRITE) != 0;
        region.executable = (info.protection & VM_PROT_EXECUTE) != 0;
        result.regions.push_back(region);
      }

      const mach_vm_address_t next = address + regionSize;
      if (next <= address)
        break;
      address = next;
    }

    if (task != MACH_PORT_NULL && task != mach_task_self())
      mach_port_deallocate(mach_task_self(), task);
    result.success = true;
    return result;
#elif defined(__linux__)
    const std::string mapsPath = "/proc/" + std::to_string(processId) + "/maps";
    std::ifstream maps(mapsPath);
    if (!maps)
    {
      result.reason = errnoMessage("open /proc/<pid>/maps");
      return result;
    }

    std::string line;
    while (std::getline(maps, line))
    {
      const std::size_t dash = line.find('-');
      const std::size_t space = line.find(' ');
      if (dash == std::string::npos || space == std::string::npos || dash > space)
        continue;
      if (space + 4 >= line.size())
        continue;

      const std::string startText = line.substr(0, dash);
      const std::string endText = line.substr(dash + 1, space - dash - 1);
      char* startEnd = nullptr;
      char* endEnd = nullptr;
      const unsigned long long start = std::strtoull(startText.c_str(), &startEnd, 16);
      const unsigned long long end = std::strtoull(endText.c_str(), &endEnd, 16);
      if (*startEnd != '\0' || *endEnd != '\0' || end <= start)
        continue;

      RuntimeMemoryRegion region;
      region.address = static_cast<std::uintptr_t>(start);
      region.size = static_cast<std::size_t>(
        std::min<unsigned long long>(end - start, static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max())));
      region.readable = line[space + 1] == 'r';
      region.writable = line[space + 2] == 'w';
      region.executable = line[space + 3] == 'x';
      result.regions.push_back(region);
    }

    result.success = true;
    return result;
#else
    result.reason = "memory region discovery is unsupported on this platform";
    return result;
#endif
  }

  RuntimeMemoryRegionResult findFirstReadableProcessMemoryRegion(int processId)
  {
    RuntimeMemoryRegionListResult list = listProcessMemoryRegions(processId);
    if (!list.success)
      return regionFailure(list.reason);

    for (const RuntimeMemoryRegion& region : list.regions)
    {
      if (region.readable && region.size > 0)
      {
        RuntimeMemoryRegionResult result;
        result.found = true;
        result.address = region.address;
        result.size = region.size;
        return result;
      }
    }

    return regionFailure("no readable memory region found");
  }

  RuntimeMemoryReadResult readProcessMemory(
    int processId,
    std::uintptr_t address,
    std::size_t size)
  {
    if (processId <= 0)
      return failure("process id must be positive");
    if (address == 0)
      return failure("memory address must be non-zero");
    if (size == 0)
      return failure("read size must be positive");

    RuntimeMemoryReadResult result;
    result.bytes.resize(size);

#if defined(_WIN32)
    HANDLE process = OpenProcess(PROCESS_VM_READ, FALSE, static_cast<DWORD>(processId));
    if (process == nullptr)
      return failure("OpenProcess failed");

    SIZE_T bytesRead = 0;
    const BOOL ok = ReadProcessMemory(
      process,
      reinterpret_cast<LPCVOID>(address),
      result.bytes.data(),
      size,
      &bytesRead);
    CloseHandle(process);

    if (!ok)
      return failure("ReadProcessMemory failed");

    result.success = true;
    result.bytesRead = static_cast<std::size_t>(bytesRead);
    result.bytes.resize(result.bytesRead);
    return result;
#elif defined(__APPLE__)
    mach_port_t task = MACH_PORT_NULL;
    if (processId == currentProcessId())
    {
      task = mach_task_self();
    }
    else
    {
      const kern_return_t taskResult = task_for_pid(mach_task_self(), processId, &task);
      if (taskResult != KERN_SUCCESS)
        return failure(taskForPidFailureMessage(taskResult));
    }

    mach_vm_size_t bytesRead = 0;
    const kern_return_t readResult = mach_vm_read_overwrite(
      task,
      static_cast<mach_vm_address_t>(address),
      static_cast<mach_vm_size_t>(size),
      reinterpret_cast<mach_vm_address_t>(result.bytes.data()),
      &bytesRead);

    if (task != MACH_PORT_NULL && task != mach_task_self())
      mach_port_deallocate(mach_task_self(), task);

    if (readResult != KERN_SUCCESS)
      return failure("mach_vm_read_overwrite failed: " + std::string(mach_error_string(readResult)));

    result.success = true;
    result.bytesRead = static_cast<std::size_t>(bytesRead);
    result.bytes.resize(result.bytesRead);
    return result;
#elif defined(__linux__)
    iovec local;
    local.iov_base = result.bytes.data();
    local.iov_len = size;

    iovec remote;
    remote.iov_base = reinterpret_cast<void*>(address);
    remote.iov_len = size;

    const ssize_t bytesRead = process_vm_readv(
      static_cast<pid_t>(processId),
      &local,
      1,
      &remote,
      1,
      0);

    if (bytesRead < 0)
      return failure(errnoMessage("process_vm_readv"));

    result.success = true;
    result.bytesRead = static_cast<std::size_t>(bytesRead);
    result.bytes.resize(result.bytesRead);
    return result;
#else
    return failure("process memory reads are unsupported on this platform");
#endif
  }

  RuntimeMemoryWriteResult writeProcessMemory(
    int processId,
    std::uintptr_t address,
    const void* bytes,
    std::size_t size)
  {
    if (processId <= 0)
      return writeFailure("process id must be positive");
    if (address == 0)
      return writeFailure("memory address must be non-zero");
    if (bytes == nullptr)
      return writeFailure("write buffer must be non-null");
    if (size == 0)
      return writeFailure("write size must be positive");

#if defined(_WIN32)
    HANDLE process = OpenProcess(PROCESS_VM_WRITE | PROCESS_VM_OPERATION, FALSE, static_cast<DWORD>(processId));
    if (process == nullptr)
      return writeFailure("OpenProcess failed");

    SIZE_T bytesWritten = 0;
    const BOOL ok = WriteProcessMemory(
      process,
      reinterpret_cast<LPVOID>(address),
      bytes,
      size,
      &bytesWritten);
    CloseHandle(process);

    if (!ok)
      return writeFailure("WriteProcessMemory failed");

    RuntimeMemoryWriteResult result;
    result.success = true;
    result.bytesWritten = static_cast<std::size_t>(bytesWritten);
    return result;
#elif defined(__APPLE__)
    mach_port_t task = MACH_PORT_NULL;
    if (processId == currentProcessId())
    {
      task = mach_task_self();
    }
    else
    {
      const kern_return_t taskResult = task_for_pid(mach_task_self(), processId, &task);
      if (taskResult != KERN_SUCCESS)
        return writeFailure(taskForPidFailureMessage(taskResult));
    }

    const kern_return_t writeResult = mach_vm_write(
      task,
      static_cast<mach_vm_address_t>(address),
      reinterpret_cast<vm_offset_t>(const_cast<void*>(bytes)),
      static_cast<mach_msg_type_number_t>(size));

    if (task != MACH_PORT_NULL && task != mach_task_self())
      mach_port_deallocate(mach_task_self(), task);

    if (writeResult != KERN_SUCCESS)
      return writeFailure("mach_vm_write failed: " + std::string(mach_error_string(writeResult)));

    RuntimeMemoryWriteResult result;
    result.success = true;
    result.bytesWritten = size;
    return result;
#elif defined(__linux__)
    iovec local;
    local.iov_base = const_cast<void*>(bytes);
    local.iov_len = size;

    iovec remote;
    remote.iov_base = reinterpret_cast<void*>(address);
    remote.iov_len = size;

    const ssize_t bytesWritten = process_vm_writev(
      static_cast<pid_t>(processId),
      &local,
      1,
      &remote,
      1,
      0);

    if (bytesWritten < 0)
      return writeFailure(errnoMessage("process_vm_writev"));

    RuntimeMemoryWriteResult result;
    result.success = true;
    result.bytesWritten = static_cast<std::size_t>(bytesWritten);
    return result;
#else
    return writeFailure("process memory writes are unsupported on this platform");
#endif
  }
}
