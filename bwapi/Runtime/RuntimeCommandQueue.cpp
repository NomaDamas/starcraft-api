#include <BWAPI/Runtime/RuntimeCommandQueue.h>

#include <utility>

namespace BWAPI::Runtime
{
  namespace
  {
    RuntimeCommandQueueResult rejected(std::string reason, std::size_t queueSize)
    {
      RuntimeCommandQueueResult result;
      result.accepted = false;
      result.reason = std::move(reason);
      result.queueSize = queueSize;
      return result;
    }

    RuntimeCommandQueueResult accepted(std::size_t queueSize)
    {
      RuntimeCommandQueueResult result;
      result.accepted = true;
      result.queueSize = queueSize;
      return result;
    }
  }

  RuntimeCommandQueue::RuntimeCommandQueue()
    : surface_(makeBWAPICommandSurface())
  {
  }

  RuntimeCommandQueueResult RuntimeCommandQueue::enqueue(RuntimeCommandRequest request)
  {
    if (request.name.empty())
      return rejected("runtime command name is empty", pending_.size());

    if (request.kind == RuntimeCommandKind::UnitCommand)
    {
      if (!containsCommandSurfaceEntry(surface_.unitCommands, request.name))
        return rejected("unknown BWAPI unit command: " + request.name, pending_.size());
      if (request.targetUnitId < 0)
        return rejected("runtime unit command target id is negative", pending_.size());
    }
    else if (request.kind == RuntimeCommandKind::GameAction)
    {
      if (!containsCommandSurfaceEntry(surface_.gameActions, request.name))
        return rejected("unknown BWAPI game action: " + request.name, pending_.size());
    }
    else
    {
      return rejected("unknown runtime command kind", pending_.size());
    }

    pending_.push_back(std::move(request));
    return accepted(pending_.size());
  }

  std::vector<RuntimeCommandRequest> RuntimeCommandQueue::drain()
  {
    std::vector<RuntimeCommandRequest> drained;
    drained.swap(pending_);
    return drained;
  }

  bool RuntimeCommandQueue::empty() const
  {
    return pending_.empty();
  }

  std::size_t RuntimeCommandQueue::size() const
  {
    return pending_.size();
  }

  const char* toString(RuntimeCommandKind kind)
  {
    switch (kind)
    {
    case RuntimeCommandKind::UnitCommand: return "unit-command";
    case RuntimeCommandKind::GameAction: return "game-action";
    }
    return "unknown";
  }
}
