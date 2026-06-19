#pragma once

#include <BWAPI/Runtime/RuntimeBackend.h>

namespace BWAPI::Runtime
{
  class Legacy1161RuntimeBackend final : public RuntimeBackend
  {
  public:
    explicit Legacy1161RuntimeBackend(RuntimeEnvironment environment);

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
