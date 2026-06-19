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
}
