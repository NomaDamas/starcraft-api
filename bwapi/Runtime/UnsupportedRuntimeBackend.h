#pragma once

#include <BWAPI/Runtime/RuntimeBackend.h>

namespace BWAPI::Runtime
{
  class UnsupportedRuntimeBackend final : public RuntimeBackend
  {
  public:
    UnsupportedRuntimeBackend(RuntimeEnvironment environment, std::string name, std::string reason);

    const char* name() const override;
    RuntimeEnvironment environment() const override;
    RuntimeProbeResult probe() const override;
    RuntimeOpenResult open() override;
    void close() override;
    RuntimeSessionState state() const override;

  private:
    RuntimeEnvironment environment_;
    std::string name_;
    std::string reason_;
    RuntimeSessionState state_ = RuntimeSessionState::Closed;
  };
}
