#include "Legacy1161RuntimeBackend.h"

#include <BWAPI/Runtime/RuntimeCommandSurface.h>
#include <BWAPI/Runtime/RuntimeContract.h>
#include <BWAPI/Runtime/RuntimeInstallation.h>
#include <BWAPI/Runtime/RuntimeProcess.h>

#include <sstream>
#include <utility>

namespace BWAPI::Runtime
{
  namespace
  {
    void addBuiltInApiSurface(RuntimeProbeResult& result)
    {
      result.implementedApiSurfaceMethods =
        makeBroodWar1161ParityContract().requiredApiSurfaceMethods;
    }

    void addBuiltInCommandSurface(RuntimeProbeResult& result)
    {
      const RuntimeCommandSurface surface = makeBWAPICommandSurface();
      result.implementedUnitCommands = surface.unitCommands;
      result.implementedGameActions = surface.gameActions;
      result.implementedUnitCommandEvidence = surface.unitCommandEvidence;
      result.implementedGameActionEvidence = surface.gameActionEvidence;
      result.implementedCommandSurfaceEntries = surface.totalEntries();
    }

    bool hostNeedsWine(Platform platform)
    {
      return platform == Platform::MacOS || platform == Platform::Linux;
    }

    std::string unsupportedReason(
      const RuntimeEnvironment& environment,
      const RuntimeInstallation& installation,
      const RuntimeProcessOpenResult& process)
    {
      std::ostringstream message;
      message << "StarCraft Brood War 1.16.1 runtime is staged but not production ready. "
              << "The portable backend can identify a Win32 1.16.1 runtime target, "
              << "but production support still requires a validated in-game BWAPI bridge "
              << "with live read/command/event/render proofs.";
      if (!installation.found)
      {
        message << " Installation is not configured: "
                << (installation.reason.empty() ? "set STARCRAFT_API_BW1161_DIR or STARCRAFT_API_BW1161_EXE" : installation.reason)
                << '.';
      }
      else
      {
        message << " Executable: " << installation.executablePath << '.';
        if (hostNeedsWine(installation.platform))
        {
          if (installation.compatibilityRuntimePath.empty())
            message << " Wine is required on this host; set STARCRAFT_API_WINE.";
          else
            message << " Wine: " << installation.compatibilityRuntimePath << '.';
        }
      }
      if (environment.processId > 0 && !process.opened)
        message << " Process attach check failed: " << process.reason << '.';
      return message.str();
    }
  }

  Legacy1161RuntimeBackend::Legacy1161RuntimeBackend(RuntimeEnvironment environment)
    : environment_(std::move(environment))
  {
  }

  const char* Legacy1161RuntimeBackend::name() const
  {
    return "legacy-bwapi-1.16.1-runtime";
  }

  RuntimeEnvironment Legacy1161RuntimeBackend::environment() const
  {
    return environment_;
  }

  RuntimeProbeResult Legacy1161RuntimeBackend::probe() const
  {
    RuntimeProbeResult result;
    addBuiltInApiSurface(result);
    addBuiltInCommandSurface(result);

    RuntimeInstallation installation = detectStarCraftInstallation(environment_);
    RuntimeProcessOpenResult process;
    if (environment_.processId > 0)
      process = openRuntimeProcess(environment_);

    result.supported = false;
    result.reason = unsupportedReason(environment_, installation, process);
    return result;
  }

  RuntimeOpenResult Legacy1161RuntimeBackend::open()
  {
    RuntimeProbeResult probeResult = probe();
    state_ = RuntimeSessionState::Failed;

    RuntimeOpenResult result;
    result.opened = false;
    result.state = state_;
    result.reason = probeResult.reason;
    return result;
  }

  void Legacy1161RuntimeBackend::close()
  {
    state_ = RuntimeSessionState::Closed;
  }

  RuntimeSessionState Legacy1161RuntimeBackend::state() const
  {
    return state_;
  }
}
