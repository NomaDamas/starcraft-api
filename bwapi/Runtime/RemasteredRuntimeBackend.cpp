#include "RemasteredRuntimeBackend.h"

#include <BWAPI/Runtime/RuntimeCommandSurface.h>
#include <BWAPI/Runtime/RuntimeContract.h>
#include <BWAPI/Runtime/RuntimeExecutor.h>
#include <BWAPI/Runtime/RuntimeManifest.h>
#include <BWAPI/Runtime/RuntimeProcessMemory.h>

#include <algorithm>
#include <sstream>
#include <utility>

namespace BWAPI::Runtime
{
  namespace
  {
    std::string unsupportedReason(const RuntimeEnvironment& environment)
    {
      RuntimeContract contract = makeRemasteredParityContract(environment.version.empty() ? "unknown" : environment.version);
      if (!environment.manifestPath.empty())
      {
        RuntimeManifestLoadResult manifest = loadRuntimeManifestFile(environment.manifestPath);
        if (manifest.loaded)
          contract = manifest.manifest.contract;
      }

      contract = applyRuntimeExecutorBridgeContractProofs(environment, contract);
      ContractValidationResult validation = validateRuntimeContract(contract);
      RuntimeExecutorPreflightResult preflight = preflightRuntimeExecutor(environment, contract);

      std::ostringstream message;
      message << "StarCraft Remastered runtime backend is not production ready. "
              << "The parity contract currently has " << validation.errors.size()
              << " unresolved production gate error(s). "
              << "Executor preflight has " << preflight.errors.size()
              << " error(s) and " << preflight.warnings.size() << " warning(s).";
      if (contractContainsFixtureEvidence(contract))
        message << " Runtime contract uses fixture validation evidence and cannot claim production support.";
      if (!environment.manifestPath.empty())
        message << " Runtime manifest: " << environment.manifestPath << '.';
      return message.str();
    }

    void addCapabilityIfMissing(RuntimeProbeResult& result, Capability capability)
    {
      if (std::find(result.capabilities.begin(), result.capabilities.end(), capability) == result.capabilities.end())
        result.capabilities.push_back(capability);
    }

    bool contractRequiresCapability(const RuntimeContract& contract, Capability capability)
    {
      return std::find(
        contract.requiredCapabilities.begin(),
        contract.requiredCapabilities.end(),
        capability) != contract.requiredCapabilities.end();
    }

    void addValidatedBridgeCapabilities(
      RuntimeProbeResult& result,
      const RuntimeExecutorPreflightResult& preflight)
    {
      if (preflight.executorBridgeMode != RuntimeExecutorBridgeValidatedAdapterMode)
        return;

      for (Capability capability : preflight.provenCapabilities)
        addCapabilityIfMissing(result, capability);
    }

    bool preflightHasCapability(
      const RuntimeExecutorPreflightResult& preflight,
      Capability capability)
    {
      return std::find(
        preflight.provenCapabilities.begin(),
        preflight.provenCapabilities.end(),
        capability) != preflight.provenCapabilities.end();
    }

    void addBuiltInApiSurface(RuntimeProbeResult& result, const RuntimeContract& contract)
    {
      result.implementedApiSurfaceMethods = contract.requiredApiSurfaceMethods;
    }

    void addValidatedCommandSurface(
      RuntimeProbeResult& result,
      const RuntimeExecutorPreflightResult& preflight)
    {
      if (!preflightHasCapability(preflight, Capability::IssueCommands))
        return;

      const RuntimeCommandSurface surface = makeBWAPICommandSurface();
      result.implementedUnitCommands = surface.unitCommands;
      result.implementedGameActions = surface.gameActions;
      result.implementedCommandSurfaceEntries = surface.totalEntries();
    }
  }

  RemasteredRuntimeBackend::RemasteredRuntimeBackend(RuntimeEnvironment environment)
    : environment_(std::move(environment))
  {
  }

  const char* RemasteredRuntimeBackend::name() const
  {
    return "starcraft-remastered-runtime";
  }

  RuntimeEnvironment RemasteredRuntimeBackend::environment() const
  {
    return environment_;
  }

  RuntimeProbeResult RemasteredRuntimeBackend::probe() const
  {
    RuntimeProbeResult result;
    RuntimeContract contract = makeRemasteredParityContract(environment_.version.empty() ? "unknown" : environment_.version);
    bool manifestLoaded = false;
    if (!environment_.manifestPath.empty())
    {
      RuntimeManifestLoadResult manifest = loadRuntimeManifestFile(environment_.manifestPath);
      if (manifest.loaded)
      {
        manifestLoaded = true;
        contract = manifest.manifest.contract;
        result.capabilities = manifest.manifest.capabilities;
        result.implementedUnitCommands = manifest.manifest.unitCommands;
        result.implementedGameActions = manifest.manifest.gameActions;
        result.implementedApiSurfaceMethods = manifest.manifest.implementedApiSurfaceMethods;
        result.implementedCommandSurfaceEntries = manifest.manifest.implementedCommandSurfaceEntries;
      }
    }
    if (!manifestLoaded)
      addBuiltInApiSurface(result, contract);
    contract = applyRuntimeExecutorBridgeContractProofs(environment_, contract);

    const RuntimeMemoryAccessResult memoryAccess = openProcessMemoryAccess(environment_.processId);
    if (memoryAccess.accessible)
      addCapabilityIfMissing(result, Capability::SharedMemoryClient);

    RuntimeExecutorPreflightResult preflight = preflightRuntimeExecutor(environment_, contract);
    addValidatedBridgeCapabilities(result, preflight);
    if (!manifestLoaded)
      addValidatedCommandSurface(result, preflight);

    result.supported = true;
    result.supported = canClaimProductionSupport(result, contract)
      && preflight.executorAvailable
      && (!contractRequiresCapability(contract, Capability::SharedMemoryClient) || preflight.memoryAccessible)
      && preflight.errors.empty();
    if (!result.supported)
      result.reason = unsupportedReason(environment_);
    return result;
  }

  RuntimeOpenResult RemasteredRuntimeBackend::open()
  {
    RuntimeOpenResult result;
    RuntimeProbeResult probeResult = probe();
    if (!probeResult.supported)
    {
      state_ = RuntimeSessionState::Failed;
      result.opened = false;
      result.state = state_;
      result.reason = probeResult.reason.empty() ? unsupportedReason(environment_) : probeResult.reason;
      return result;
    }

    state_ = RuntimeSessionState::Open;
    result.opened = true;
    result.state = state_;
    result.reason = "StarCraft Remastered runtime backend opened";
    return result;
  }

  void RemasteredRuntimeBackend::close()
  {
    state_ = RuntimeSessionState::Closed;
  }

  RuntimeSessionState RemasteredRuntimeBackend::state() const
  {
    return state_;
  }
}
