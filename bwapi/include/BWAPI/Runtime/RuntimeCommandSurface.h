#pragma once

#include <string>
#include <vector>

namespace BWAPI::Runtime
{
  struct RuntimeCommandSurface
  {
    std::vector<std::string> unitCommands;
    std::vector<std::string> gameActions;

    int totalEntries() const;
  };

  RuntimeCommandSurface makeBWAPICommandSurface();
  bool containsCommandSurfaceEntry(const std::vector<std::string>& entries, const std::string& name);
}
