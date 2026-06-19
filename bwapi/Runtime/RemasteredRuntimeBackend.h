#pragma once

#include <BWAPI/Runtime/RuntimeBackend.h>

namespace BWAPI::Runtime
{
  class RemasteredRuntimeBackend final : public RuntimeBackend
  {
  public:
    explicit RemasteredRuntimeBackend(RuntimeEnvironment environment);

    const char* name() const override;
    RuntimeEnvironment environment() const override;
    RuntimeProbeResult probe() const override;
    RuntimeOpenResult open() override;
    void close() override;
    RuntimeSessionState state() const override;

  private:
    RuntimeEnvironment environment_;
    RuntimeSessionState state_ = RuntimeSessionState::Closed;
  };
}
