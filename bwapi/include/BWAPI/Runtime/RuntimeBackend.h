#pragma once

#include <memory>
#include <string>
#include <vector>

namespace BWAPI::Runtime
{
  enum class Platform
  {
    Windows,
    MacOS,
    Linux,
    Unknown
  };

  enum class Product
  {
    StarCraftBroodWar1161,
    StarCraftRemastered,
    Unknown
  };

  enum class Capability
  {
    ReadGameState,
    ReadMapData,
    ReadUnitData,
    ReadBulletData,
    ReadPlayerData,
    ReadRegionData,
    IssueCommands,
    DrawOverlays,
    DispatchEvents,
    ReplayAnalysis,
    MultiplayerSync,
    BattleNet,
    LoadAIModules,
    SharedMemoryClient
  };

  struct RuntimeEnvironment
  {
    Platform platform = Platform::Unknown;
    Product product = Product::Unknown;
    std::string version;
    std::string executablePath;

    static RuntimeEnvironment detectHost();
  };

  struct RuntimeProbeResult
  {
    bool supported = false;
    std::string reason;
    std::vector<Capability> capabilities;
    int implementedApiSurfaceMethods = 0;
  };

  enum class RuntimeSessionState
  {
    Closed,
    Open,
    Failed
  };

  struct RuntimeOpenResult
  {
    bool opened = false;
    RuntimeSessionState state = RuntimeSessionState::Closed;
    std::string reason;
  };

  class RuntimeBackend
  {
  public:
    virtual ~RuntimeBackend();

    virtual const char* name() const = 0;
    virtual RuntimeEnvironment environment() const = 0;
    virtual RuntimeProbeResult probe() const = 0;
    virtual RuntimeOpenResult open() = 0;
    virtual void close() = 0;
    virtual RuntimeSessionState state() const = 0;
  };

  const char* toString(Platform platform);
  const char* toString(Product product);
  const char* toString(Capability capability);
  const char* toString(RuntimeSessionState state);
  Platform parsePlatform(const std::string& value);
  Product parseProduct(const std::string& value);

  std::unique_ptr<RuntimeBackend> createRuntimeBackend(const RuntimeEnvironment& environment);
}
