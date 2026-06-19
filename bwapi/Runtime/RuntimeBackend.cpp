#include <BWAPI/Runtime/RuntimeBackend.h>

namespace BWAPI::Runtime
{
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
}
