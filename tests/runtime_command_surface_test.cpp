#include <BWAPI/Runtime/RuntimeCommandSurface.h>

#include <cassert>
#include <string>

using namespace BWAPI::Runtime;

int main()
{
  RuntimeCommandSurface surface = makeBWAPICommandSurface();

  assert(surface.unitCommands.size() == 44);
  assert(surface.gameActions.size() == 28);
  assert(surface.totalEntries() == 72);
  assert(surface.unitCommands.front() == "Attack_Move");
  assert(surface.unitCommands.back() == "Place_COP");
  assert(surface.gameActions.front() == "setScreenPosition");
  assert(surface.gameActions.back() == "setRevealAll");
  assert(containsCommandSurfaceEntry(surface.unitCommands, "Attack_Unit"));
  assert(!containsCommandSurfaceEntry(surface.unitCommands, "Not_A_Command"));

  return 0;
}
