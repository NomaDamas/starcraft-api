#include <BWAPI/Runtime/RuntimeCommandQueue.h>

#include <cassert>
#include <string>
#include <utility>
#include <vector>

using namespace BWAPI::Runtime;

namespace
{
  RuntimeCommandRequest unitCommand(std::string name)
  {
    RuntimeCommandRequest request;
    request.kind = RuntimeCommandKind::UnitCommand;
    request.name = std::move(name);
    request.targetUnitId = 7;
    request.arguments = { 1, 2, 3 };
    return request;
  }

  RuntimeCommandRequest gameAction(std::string name)
  {
    RuntimeCommandRequest request;
    request.kind = RuntimeCommandKind::GameAction;
    request.name = std::move(name);
    request.arguments = { 4, 5 };
    return request;
  }
}

int main()
{
  RuntimeCommandSurface surface = makeBWAPICommandSurface();
  RuntimeCommandQueue queue;

  assert(queue.empty());
  assert(queue.size() == 0);

  for (const std::string& command : surface.unitCommands)
  {
    RuntimeCommandQueueResult result = queue.enqueue(unitCommand(command));
    assert(result.accepted);
    assert(result.queueSize == queue.size());
  }

  for (const std::string& action : surface.gameActions)
  {
    RuntimeCommandQueueResult result = queue.enqueue(gameAction(action));
    assert(result.accepted);
    assert(result.queueSize == queue.size());
  }

  assert(queue.size() == static_cast<std::size_t>(surface.totalEntries()));
  std::vector<RuntimeCommandRequest> drained = queue.drain();
  assert(drained.size() == static_cast<std::size_t>(surface.totalEntries()));
  assert(queue.empty());
  assert(drained.front().kind == RuntimeCommandKind::UnitCommand);
  assert(drained.front().name == surface.unitCommands.front());
  assert(drained.back().kind == RuntimeCommandKind::GameAction);
  assert(drained.back().name == surface.gameActions.back());

  RuntimeCommandQueueResult unknownUnit = queue.enqueue(unitCommand("Not_A_Command"));
  assert(!unknownUnit.accepted);
  assert(!unknownUnit.reason.empty());
  assert(queue.empty());

  RuntimeCommandQueueResult unknownAction = queue.enqueue(gameAction("notARealAction"));
  assert(!unknownAction.accepted);
  assert(!unknownAction.reason.empty());
  assert(queue.empty());

  RuntimeCommandRequest negativeTarget = unitCommand("Move");
  negativeTarget.targetUnitId = -1;
  RuntimeCommandQueueResult negativeTargetResult = queue.enqueue(negativeTarget);
  assert(!negativeTargetResult.accepted);
  assert(queue.empty());

  RuntimeCommandQueueResult emptyName = queue.enqueue(unitCommand(""));
  assert(!emptyName.accepted);
  assert(queue.empty());

  assert(std::string(toString(RuntimeCommandKind::GameAction)) == "game-action");

  return 0;
}
