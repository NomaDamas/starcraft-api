#include <BWAPI/Runtime/RuntimeBackend.h>

#include <algorithm>
#include <cctype>
#include <string>

namespace BWAPI::Runtime
{
  namespace
  {
    std::string normalized(std::string value)
    {
      std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char ch)
        {
          if (ch == '_' || ch == ' ' || ch == '.')
            return '-';
          return static_cast<char>(std::tolower(ch));
        });
      return value;
    }
  }

  RuntimeBackend::~RuntimeBackend() = default;

  const char* toString(Platform platform)
  {
    switch (platform)
    {
    case Platform::Windows: return "windows";
    case Platform::MacOS: return "macos";
    case Platform::Linux: return "linux";
    case Platform::Unknown: return "unknown";
    }
    return "unknown";
  }

  const char* toString(Product product)
  {
    switch (product)
    {
    case Product::StarCraftBroodWar1161: return "starcraft-brood-war-1.16.1";
    case Product::StarCraftRemastered: return "starcraft-remastered";
    case Product::Unknown: return "unknown";
    }
    return "unknown";
  }

  const char* toString(Capability capability)
  {
    switch (capability)
    {
    case Capability::ReadGameState: return "read-game-state";
    case Capability::ReadMapData: return "read-map-data";
    case Capability::ReadUnitData: return "read-unit-data";
    case Capability::ReadBulletData: return "read-bullet-data";
    case Capability::ReadPlayerData: return "read-player-data";
    case Capability::ReadRegionData: return "read-region-data";
    case Capability::IssueCommands: return "issue-commands";
    case Capability::DrawOverlays: return "draw-overlays";
    case Capability::DispatchEvents: return "dispatch-events";
    case Capability::ReplayAnalysis: return "replay-analysis";
    case Capability::MultiplayerSync: return "multiplayer-sync";
    case Capability::BattleNet: return "battle-net";
    case Capability::LoadAIModules: return "load-ai-modules";
    case Capability::SharedMemoryClient: return "shared-memory-client";
    }
    return "unknown";
  }

  const char* toString(RuntimeSessionState state)
  {
    switch (state)
    {
    case RuntimeSessionState::Closed: return "closed";
    case RuntimeSessionState::Open: return "open";
    case RuntimeSessionState::Failed: return "failed";
    }
    return "unknown";
  }

  Platform parsePlatform(const std::string& value)
  {
    const std::string key = normalized(value);
    if (key == "windows" || key == "win32" || key == "win64")
      return Platform::Windows;
    if (key == "macos" || key == "mac-os" || key == "darwin" || key == "osx")
      return Platform::MacOS;
    if (key == "linux")
      return Platform::Linux;
    return Platform::Unknown;
  }

  Product parseProduct(const std::string& value)
  {
    const std::string key = normalized(value);
    if (key == "starcraft-remastered" || key == "remastered" || key == "scr")
      return Product::StarCraftRemastered;
    if (key == "starcraft-brood-war-1-16-1" || key == "brood-war-1-16-1" || key == "bw-1-16-1" || key == "1161")
      return Product::StarCraftBroodWar1161;
    return Product::Unknown;
  }
}
