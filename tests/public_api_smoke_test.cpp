#include <BWAPI.h>
#include <BWAPI/Interface.h>

#include <cassert>

namespace
{
  struct ClientInfoProbe : BWAPI::Interface<ClientInfoProbe>
  {
  };
}

int main()
{
  assert(BWAPI::BWAPI_getRevision() == 0);
  assert(BWAPI::Races::Terran.getName() == "Terran");
  assert(BWAPI::UnitTypes::Terran_Marine.getName() == "Terran_Marine");

  BWAPI::Position position(32, 64);
  assert(position.x == 32);
  assert(position.y == 64);

  ClientInfoProbe probe;
  int value = 7;
  probe.setClientInfo(&value, 1);
  assert(probe.getClientInfo(1) == &value);

  probe.setClientInfo(42, 2);
  assert(probe.getClientInfo<int>(2) == 42);

  return 0;
}
