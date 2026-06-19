#include <algorithm>
#include <fstream>
#include <iostream>
#include <regex>
#include <string>
#include <vector>

#ifndef STARCRAFT_API_SOURCE_DIR
#define STARCRAFT_API_SOURCE_DIR "."
#endif

namespace
{
  struct HeaderExpectation
  {
    const char* path;
    const char* interfaceName;
    int expectedPureVirtualMethods;
  };

  const HeaderExpectation expectations[] = {
    { "bwapi/include/BWAPI/Game.h", "Game", 107 },
    { "bwapi/include/BWAPI/Unit.h", "UnitInterface", 205 },
    { "bwapi/include/BWAPI/Player.h", "PlayerInterface", 44 },
    { "bwapi/include/BWAPI/Bullet.h", "BulletInterface", 13 },
    { "bwapi/include/BWAPI/Region.h", "RegionInterface", 13 },
    { "bwapi/include/BWAPI/Force.h", "ForceInterface", 3 },
  };

  int countPureVirtualMethods(const std::string& path)
  {
    std::ifstream input(path);
    if (!input)
      return -1;

    const std::regex pureVirtualPattern(R"(\bvirtual\b.*=\s*0\s*;)");
    int count = 0;
    std::string line;
    while (std::getline(input, line))
    {
      if (std::regex_search(line, pureVirtualPattern))
        ++count;
    }
    return count;
  }
}

int main()
{
  const std::string sourceDir = STARCRAFT_API_SOURCE_DIR;
  int total = 0;
  bool failed = false;

  for (const HeaderExpectation& expectation : expectations)
  {
    const std::string path = sourceDir + "/" + expectation.path;
    const int actual = countPureVirtualMethods(path);
    if (actual < 0)
    {
      std::cerr << "missing header: " << path << '\n';
      failed = true;
      continue;
    }

    std::cout << expectation.interfaceName << ".pure_virtual_methods=" << actual << '\n';
    total += actual;

    if (actual != expectation.expectedPureVirtualMethods)
    {
      std::cerr << "API surface drift for " << expectation.interfaceName
                << ": expected " << expectation.expectedPureVirtualMethods
                << ", got " << actual << '\n';
      failed = true;
    }
  }

  std::cout << "BWAPI.abstract_api_surface.total=" << total << '\n';
  if (total != 385)
  {
    std::cerr << "API surface total drift: expected 385, got " << total << '\n';
    failed = true;
  }

  return failed ? 1 : 0;
}
