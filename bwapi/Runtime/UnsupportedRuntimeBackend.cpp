#include "UnsupportedRuntimeBackend.h"

#include <utility>

namespace BWAPI::Runtime
{
  UnsupportedRuntimeBackend::UnsupportedRuntimeBackend(
    RuntimeEnvironment environment,
    std::string name,
    std::string reason)
    : environment_(std::move(environment))
    , name_(std::move(name))
    , reason_(std::move(reason))
  {
  }

  const char* UnsupportedRuntimeBackend::name() const
  {
    return name_.c_str();
  }

  RuntimeEnvironment UnsupportedRuntimeBackend::environment() const
  {
    return environment_;
  }

  RuntimeProbeResult UnsupportedRuntimeBackend::probe() const
  {
    RuntimeProbeResult result;
    result.supported = false;
    result.reason = reason_;
    return result;
  }

  RuntimeOpenResult UnsupportedRuntimeBackend::open()
  {
    state_ = RuntimeSessionState::Failed;

    RuntimeOpenResult result;
    result.opened = false;
    result.state = state_;
    result.reason = reason_;
    return result;
  }

  void UnsupportedRuntimeBackend::close()
  {
    state_ = RuntimeSessionState::Closed;
  }

  RuntimeSessionState UnsupportedRuntimeBackend::state() const
  {
    return state_;
  }
}
