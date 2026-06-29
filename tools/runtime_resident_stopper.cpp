#include <chrono>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace
{
  std::string getenvString(const char* name)
  {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0')
      return {};
    return value;
  }

  std::filesystem::path bridgePath()
  {
    std::string path = getenvString("STARCRAFT_API_RESIDENT_BRIDGE_DIR");
    if (!path.empty())
      return path;

    path = getenvString("STARCRAFT_API_EXECUTOR_BRIDGE_DIR");
    if (!path.empty())
      return path;

    return "/tmp/starcraft-api-live-bridge";
  }

  std::uint64_t tickMilliseconds()
  {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
  }

  void appendLog(const std::string& message)
  {
    const std::filesystem::path path = bridgePath();
    std::error_code error;
    std::filesystem::create_directories(path, error);
    if (error)
      return;

    std::ofstream log(path / "resident-adapter.log", std::ios::app);
    if (!log)
      return;

    log << "pid=" << getpid()
        << " tick_ms=" << tickMilliseconds()
        << ' ' << message << '\n';
  }

  using StopResidentAdapter = int (*)();
}

extern "C"
{
  const char* starcraft_api_resident_stopper_abi()
  {
    return "starcraft-api-resident-stopper-v1";
  }
}

__attribute__((constructor))
static void starcraft_api_resident_stopper_constructor()
{
  appendLog("resident.stopper.enter");

  dlerror();
  void* symbol = dlsym(RTLD_DEFAULT, "starcraft_api_resident_adapter_stop");
  const char* error = dlerror();
  if (symbol == nullptr)
  {
    appendLog(
      std::string("resident.stopper.symbol_missing reason=")
      + (error == nullptr ? "dlsym returned null" : error));
    return;
  }

  Dl_info info;
  std::memset(&info, 0, sizeof(info));
  std::string image = "unknown";
  if (dladdr(symbol, &info) != 0 && info.dli_fname != nullptr && *info.dli_fname != '\0')
    image = info.dli_fname;

  appendLog("resident.stopper.symbol_found image=" + image);
  const int result = reinterpret_cast<StopResidentAdapter>(symbol)();
  appendLog("resident.stopper.stop_result=" + std::to_string(result));
}
