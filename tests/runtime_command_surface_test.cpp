#include <BWAPI/Runtime/RuntimeCommandSurface.h>
#include <BWAPI/UnitCommandType.h>

#include <cassert>
#include <string>

using namespace BWAPI::Runtime;

int main()
{
  RuntimeCommandSurface surface = makeBWAPICommandSurface();

  static_assert(BWAPI::UnitCommandTypes::Enum::Attack_Move == 0);
  static_assert(BWAPI::UnitCommandTypes::Enum::None == 44);
  assert(surface.unitCommands.size() == 44);
  assert(surface.gameActions.size() == 28);
  assert(surface.totalEntries() == 72);
  assert(surface.unitCommands.front() == "Attack_Move");
  assert(surface.unitCommands.back() == "Place_COP");
  assert(surface.gameActions.front() == "setScreenPosition");
  assert(surface.gameActions.back() == "setRevealAll");
  assert(containsCommandSurfaceEntry(surface.unitCommands, "Attack_Unit"));
  assert(!containsCommandSurfaceEntry(surface.unitCommands, "Not_A_Command"));
  assert(surface.unitCommandEvidence.size() == surface.unitCommands.size());
  assert(surface.gameActionEvidence.size() == surface.gameActions.size());
  assert(commandEvidenceStatusFor(surface.unitCommandEvidence, "Attack_Move") == RuntimeCommandEvidenceStatus::MockTested);
  assert(commandEvidenceStatusFor(surface.gameActionEvidence, "pauseGame") == RuntimeCommandEvidenceStatus::MockTested);
  assert(commandEvidenceStatusFor(surface.gameActionEvidence, "drawBox") == RuntimeCommandEvidenceStatus::AdapterLocal);
  assert(commandEvidenceStatusFor(surface.gameActionEvidence, "Not_A_Command") == RuntimeCommandEvidenceStatus::Unknown);
  assert(std::string(toString(RuntimeCommandEvidenceStatus::LiveProven)) == "live-proven");
  RuntimeCommandEvidenceStatus parsed = RuntimeCommandEvidenceStatus::Unknown;
  assert(parseRuntimeCommandEvidenceStatus("fail-closed", parsed));
  assert(parsed == RuntimeCommandEvidenceStatus::FailClosed);
  assert(!parseRuntimeCommandEvidenceStatus("fixture", parsed));

  return 0;
}
