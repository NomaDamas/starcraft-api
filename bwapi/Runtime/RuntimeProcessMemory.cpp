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
      return accessFailure("task_for_pid failed: " + std::string(mach_error_string(taskResult)));
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
        return writeFailure("task_for_pid failed: " + std::string(mach_error_string(taskResult)));
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
