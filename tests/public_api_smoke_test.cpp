#include <BWAPI.h>

#include <cassert>

int main()
{
  assert(BWAPI::BWAPI_getRevision() == 0);
  assert(BWAPI::Races::Terran.getName() == "Terran");
  assert(BWAPI::UnitTypes::Terran_Marine.getName() == "Terran_Marine");

  BWAPI::Position position(32, 64);
  assert(position.x == 32);
  assert(position.y == 64);

  return 0;
}
