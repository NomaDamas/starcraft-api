#include <BWAPI/Runtime/RuntimeProcessMemory.h>

#include <cerrno>
#include <cstring>
#include <sstream>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <unistd.h>
#elif defined(__linux__)
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

    std::string errnoMessage(const char* operation)
    {
      std::ostringstream message;
      message << operation << " failed: " << std::strerror(errno);
      return message.str();
    }
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
        return failure("task_for_pid failed: " + std::string(mach_error_string(taskResult)));
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
}
