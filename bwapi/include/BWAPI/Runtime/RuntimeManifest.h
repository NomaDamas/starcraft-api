#pragma once

#include <BWAPI/Runtime/RuntimeContract.h>

#include <istream>
#include <string>
#include <vector>

namespace BWAPI::Runtime
{
  struct RuntimeManifest
  {
    RuntimeContract contract;
    std::vector<Capability> capabilities;
    std::vector<std::string> unitCommands;
    std::vector<std::string> gameActions;
    int implementedApiSurfaceMethods = 0;
    int implementedCommandSurfaceEntries = 0;
  };

  struct RuntimeManifestLoadResult
  {
    bool loaded = false;
    RuntimeManifest manifest;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
  };

  RuntimeManifestLoadResult loadRuntimeManifest(std::istream& input, const std::string& sourceName);
  RuntimeManifestLoadResult loadRuntimeManifestFile(const std::string& path);
}
