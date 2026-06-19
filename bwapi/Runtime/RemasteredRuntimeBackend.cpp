#include "RemasteredRuntimeBackend.h"

#include <BWAPI/Runtime/RuntimeContract.h>

#include <sstream>
#include <utility>

namespace BWAPI::Runtime
{
  namespace
  {
    std::string unsupportedReason(const RuntimeEnvironment& environment)
    {
      RuntimeContract contract = makeRemasteredParityContract(environment.version.empty() ? "unknown" : environment.version);
      ContractValidationResult validation = validateRuntimeContract(contract);

      std::ostringstream message;
      message << "StarCraft Remastered runtime adapter is not implemented. "
              << "The parity contract currently has " << validation.errors.size()
              << " unresolved production gate error(s).";
      return message.str();
    }
  }

  RemasteredRuntimeBackend::RemasteredRuntimeBackend(RuntimeEnvironment environment)
    : environment_(std::move(environment))
  {
  }

  const char* RemasteredRuntimeBackend::name() const
  {
    return "starcraft-remastered-runtime";
  }

  RuntimeEnvironment RemasteredRuntimeBackend::environment() const
  {
    return environment_;
  }

  RuntimeProbeResult RemasteredRuntimeBackend::probe() const
  {
    RuntimeProbeResult result;
    result.supported = false;
    result.reason = unsupportedReason(environment_);
    return result;
  }

  RuntimeOpenResult RemasteredRuntimeBackend::open()
  {
    state_ = RuntimeSessionState::Failed;

    RuntimeOpenResult result;
    result.opened = false;
    result.state = state_;
    result.reason = unsupportedReason(environment_);
    return result;
  }

  void RemasteredRuntimeBackend::close()
  {
    state_ = RuntimeSessionState::Closed;
  }

  RuntimeSessionState RemasteredRuntimeBackend::state() const
  {
    return state_;
  }
}
