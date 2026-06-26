#include <algorithm>
#include <fstream>
#include <iostream>
#include <regex>
#include <set>
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

  std::vector<std::string> pureVirtualMethodNames(const std::string& path)
  {
    std::ifstream input(path);
    if (!input)
      return {};

    const std::regex methodPattern(
      R"(\bvirtual\b.*\b([A-Za-z_][A-Za-z0-9_]*)\s*\([^;]*\)\s*(?:const\s*)?=\s*0\s*;)");
    std::vector<std::string> names;
    std::string line;
    while (std::getline(input, line))
    {
      std::smatch match;
      if (std::regex_search(line, match, methodPattern))
        names.push_back(match[1].str());
    }
    return names;
  }

  std::vector<std::string> unitCommandEnumNames(const std::string& path)
  {
    std::ifstream input(path);
    if (!input)
      return {};

    const std::regex enumEntryPattern(R"(^\s*([A-Za-z_][A-Za-z0-9_]*)\s*(?:=[^,]+)?\s*,)");
    std::vector<std::string> names;
    bool inEnum = false;
    bool sawEnumDeclaration = false;
    std::string line;
    while (std::getline(input, line))
    {
      if (!inEnum)
      {
        if (line.find("enum Enum") != std::string::npos)
          sawEnumDeclaration = true;
        if (sawEnumDeclaration && line.find('{') != std::string::npos)
          inEnum = true;
        continue;
      }

      std::smatch match;
      if (!std::regex_search(line, match, enumEntryPattern))
      {
        if (line.find('}') != std::string::npos)
          break;
        continue;
      }

      const std::string name = match[1].str();
      if (name == "None")
        break;
      if (name != "Unknown" && name != "MAX")
        names.push_back(name);
    }
    return names;
  }

  std::vector<std::string> runtimeCommandSurfaceList(
    const std::string& path,
    const std::string& memberName)
  {
    std::ifstream input(path);
    if (!input)
      return {};

    const std::regex stringPattern(R"REGEX("([^"]+)")REGEX");
    const std::string assignment = "surface." + memberName + " =";
    std::vector<std::string> names;
    bool inList = false;
    std::string line;
    while (std::getline(input, line))
    {
      if (!inList)
      {
        if (line.find(assignment) == std::string::npos)
          continue;
        inList = true;
      }

      for (std::sregex_iterator it(line.begin(), line.end(), stringPattern), end; it != end; ++it)
        names.push_back((*it)[1].str());

      if (inList && line.find("};") != std::string::npos)
        break;
    }
    return names;
  }

  bool containsDuplicates(const std::vector<std::string>& names)
  {
    std::set<std::string> seen;
    for (const std::string& name : names)
    {
      if (!seen.insert(name).second)
        return true;
    }
    return false;
  }

  bool sameList(
    const std::vector<std::string>& expected,
    const std::vector<std::string>& actual,
    const std::string& label)
  {
    if (expected == actual)
      return true;

    std::cerr << label << " drift: expected " << expected.size()
              << " entries, got " << actual.size() << '\n';
    const std::size_t count = std::max(expected.size(), actual.size());
    for (std::size_t index = 0; index < count; ++index)
    {
      const std::string expectedName = index < expected.size() ? expected[index] : "<missing>";
      const std::string actualName = index < actual.size() ? actual[index] : "<missing>";
      if (expectedName != actualName)
      {
        std::cerr << label << " first mismatch at " << index
                  << ": expected " << expectedName
                  << ", got " << actualName << '\n';
        break;
      }
    }
    return false;
  }

  bool allNamesInSet(
    const std::vector<std::string>& required,
    const std::vector<std::string>& available,
    const std::string& label)
  {
    const std::set<std::string> availableNames(available.begin(), available.end());
    bool ok = true;
    for (const std::string& name : required)
    {
      if (availableNames.find(name) == availableNames.end())
      {
        std::cerr << label << " missing from public Game interface: " << name << '\n';
        ok = false;
      }
    }
    return ok;
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

  const std::string unitCommandHeader = sourceDir + "/bwapi/include/BWAPI/UnitCommandType.h";
  const std::string gameHeader = sourceDir + "/bwapi/include/BWAPI/Game.h";
  const std::string runtimeSurfaceSource = sourceDir + "/bwapi/Runtime/RuntimeCommandSurface.cpp";
  const std::vector<std::string> headerUnitCommands =
    unitCommandEnumNames(unitCommandHeader);
  const std::vector<std::string> runtimeUnitCommands =
    runtimeCommandSurfaceList(runtimeSurfaceSource, "unitCommands");
  const std::vector<std::string> gameMethods =
    pureVirtualMethodNames(gameHeader);
  const std::vector<std::string> runtimeGameActions =
    runtimeCommandSurfaceList(runtimeSurfaceSource, "gameActions");

  std::cout << "BWAPI.command_surface.unit_commands.header="
            << headerUnitCommands.size() << '\n';
  std::cout << "BWAPI.command_surface.unit_commands.runtime="
            << runtimeUnitCommands.size() << '\n';
  std::cout << "BWAPI.command_surface.game_actions.runtime="
            << runtimeGameActions.size() << '\n';

  if (headerUnitCommands.empty())
  {
    std::cerr << "missing unit command enum entries from " << unitCommandHeader << '\n';
    failed = true;
  }
  if (runtimeUnitCommands.empty() || runtimeGameActions.empty())
  {
    std::cerr << "missing runtime command surface lists from " << runtimeSurfaceSource << '\n';
    failed = true;
  }
  if (containsDuplicates(runtimeUnitCommands))
  {
    std::cerr << "runtime unit command surface contains duplicate entries\n";
    failed = true;
  }
  if (containsDuplicates(runtimeGameActions))
  {
    std::cerr << "runtime game action surface contains duplicate entries\n";
    failed = true;
  }
  if (!sameList(headerUnitCommands, runtimeUnitCommands, "unit command surface"))
    failed = true;
  if (!allNamesInSet(runtimeGameActions, gameMethods, "game action surface"))
    failed = true;

  return failed ? 1 : 0;
}
