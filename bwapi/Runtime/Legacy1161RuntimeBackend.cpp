#include "Legacy1161RuntimeBackend.h"

#include <utility>

namespace BWAPI::Runtime
{
  namespace
  {
    const char* unsupportedReason()
    {
      return "The legacy BWAPI 1.16.1 runtime still lives in the Windows DLL injection backend and "
             "has not yet been moved behind the portable RuntimeBackend interface.";
    }
  }

  Legacy1161RuntimeBackend::Legacy1161RuntimeBackend(RuntimeEnvironment environment)
    : environment_(std::move(environment))
  {
  }

  const char* Legacy1161RuntimeBackend::name() const
  {
    return "legacy-bwapi-1.16.1-runtime";
  }

  RuntimeEnvironment Legacy1161RuntimeBackend::environment() const
  {
    return environment_;
  }

  RuntimeProbeResult Legacy1161RuntimeBackend::probe() const
  {
    RuntimeProbeResult result;
    result.supported = false;
    result.reason = unsupportedReason();
    return result;
  }

  RuntimeOpenResult Legacy1161RuntimeBackend::open()
  {
    state_ = RuntimeSessionState::Failed;

    RuntimeOpenResult result;
    result.opened = false;
    result.state = state_;
    result.reason = unsupportedReason();
    return result;
  }

  void Legacy1161RuntimeBackend::close()
  {
    state_ = RuntimeSessionState::Closed;
  }

  RuntimeSessionState Legacy1161RuntimeBackend::state() const
  {
    return state_;
  }
}
