#pragma once

#include <BWAPI/Runtime/RuntimeCommandSurface.h>

#include <cstddef>
#include <string>
#include <vector>

namespace BWAPI::Runtime
{
  enum class RuntimeCommandKind
  {
    UnitCommand,
    GameAction
  };

  struct RuntimeCommandRequest
  {
    RuntimeCommandKind kind = RuntimeCommandKind::UnitCommand;
    std::string name;
    int targetUnitId = 0;
    std::vector<int> arguments;
  };

  struct RuntimeCommandQueueResult
  {
    bool accepted = false;
    std::string reason;
    std::size_t queueSize = 0;
  };

  class RuntimeCommandQueue
  {
  public:
    RuntimeCommandQueue();

    RuntimeCommandQueueResult enqueue(RuntimeCommandRequest request);
    std::vector<RuntimeCommandRequest> drain();
    bool empty() const;
    std::size_t size() const;

  private:
    RuntimeCommandSurface surface_;
    std::vector<RuntimeCommandRequest> pending_;
  };

  const char* toString(RuntimeCommandKind kind);
}
