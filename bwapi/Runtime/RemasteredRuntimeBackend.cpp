#include "RemasteredRuntimeBackend.h"

#include <BWAPI/Runtime/RuntimeCommandSurface.h>
#include <BWAPI/Runtime/RuntimeContract.h>
#include <BWAPI/Runtime/RuntimeExecutor.h>
#include <BWAPI/Runtime/RuntimeManifest.h>
#include <BWAPI/Runtime/RuntimeProcessMemory.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_set>
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

    void addBuiltInApiSurface(RuntimeProbeResult& result, const RuntimeContract& contract)
    {
      result.implementedApiSurfaceMethods = contract.requiredApiSurfaceMethods;
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

    std::vector<RuntimeCommandEvidence> staticManifestCommandEvidence(
      std::vector<RuntimeCommandEvidence> evidence)
    {
      for (RuntimeCommandEvidence& entry : evidence)
      {
        if (entry.status != RuntimeCommandEvidenceStatus::LiveProven)
          continue;

        entry.status = RuntimeCommandEvidenceStatus::DocumentedScenario;
        entry.detail = entry.detail.empty()
          ? "static-manifest-live-proof-requires-validated-resident-proof"
          : entry.detail + ":static-manifest-live-proof-requires-validated-resident-proof";
      }
      return evidence;
    }

    bool commandEvidenceProofIsProven(
      const RuntimeExecutorPreflightResult& preflight,
      const std::string& detail)
    {
      (void)preflight;
      (void)detail;
      // Aggregate proof lines such as proof.issue_commands=passed are not
      // command-specific evidence. Keep live rows fail-closed until the
      // resident adapter emits and validates per-command behavior snapshots.
      return false;
    }

    bool allAsciiDigits(const std::string& value)
    {
      return !value.empty()
        && std::all_of(
          value.begin(),
          value.end(),
          [](unsigned char c) { return std::isdigit(c) != 0; });
    }

    bool parseBridgeLiveCommandEvidenceValue(
      const std::string& value,
      RuntimeCommandEvidence& evidence)
    {
      const std::size_t first = value.find('|');
      const std::size_t second = first == std::string::npos
        ? std::string::npos
        : value.find('|', first + 1);
      if (first == std::string::npos
          || second == std::string::npos
          || value.find('|', second + 1) != std::string::npos)
      {
        return false;
      }

      evidence.name = value.substr(0, first);
      evidence.detail = value.substr(second + 1);
      RuntimeCommandEvidenceStatus status = RuntimeCommandEvidenceStatus::Unknown;
      if (evidence.name.empty()
          || evidence.detail.empty()
          || !parseRuntimeCommandEvidenceStatus(value.substr(first + 1, second - first - 1), status))
      {
        return false;
      }
      evidence.status = status;
      return true;
    }

    bool expectedCommandEvidenceName(
      const std::vector<std::string>& expectedNames,
      const std::string& name)
    {
      return std::find(expectedNames.begin(), expectedNames.end(), name) != expectedNames.end();
    }

    std::vector<RuntimeCommandEvidence> readProofBackedLiveCommandEvidence(
      const std::filesystem::path& readyPath,
      const std::string& keyPrefix,
      const std::vector<std::string>& expectedNames,
      const RuntimeExecutorPreflightResult& preflight)
    {
      std::vector<RuntimeCommandEvidence> result;
      std::unordered_set<std::string> seenNames;
      std::ifstream input(readyPath);
      std::string line;
      while (std::getline(input, line))
      {
        const std::size_t separator = line.find('=');
        if (separator == std::string::npos)
          continue;

        const std::string key = line.substr(0, separator);
        if (key.rfind(keyPrefix, 0) != 0)
          continue;
        if (!allAsciiDigits(key.substr(keyPrefix.size())))
          return {};

        RuntimeCommandEvidence evidence;
        if (!parseBridgeLiveCommandEvidenceValue(line.substr(separator + 1), evidence)
            || evidence.status != RuntimeCommandEvidenceStatus::LiveProven
            || !expectedCommandEvidenceName(expectedNames, evidence.name)
            || !commandEvidenceProofIsProven(preflight, evidence.detail)
            || !seenNames.insert(evidence.name).second)
        {
          return {};
        }
        result.push_back(std::move(evidence));
      }
      return result;
    }

    void mergeLiveCommandEvidence(
      std::vector<RuntimeCommandEvidence>& target,
      const std::vector<RuntimeCommandEvidence>& liveEvidence)
    {
      for (const RuntimeCommandEvidence& live : liveEvidence)
      {
        auto it = std::find_if(
          target.begin(),
          target.end(),
          [&](const RuntimeCommandEvidence& existing)
          {
            return existing.name == live.name;
          });
        if (it != target.end())
          *it = live;
      }
    }

    void applyBridgeLiveCommandEvidence(
      RuntimeProbeResult& result,
      const RuntimeEnvironment& environment,
      const RuntimeExecutorPreflightResult& preflight)
    {
      if (environment.executorBridgePath.empty()
          || preflight.executorBridgeMode != RuntimeExecutorBridgeValidatedAdapterMode)
      {
        return;
      }

      std::error_code error;
      const std::filesystem::path readyPath =
        std::filesystem::path(environment.executorBridgePath) / RuntimeExecutorBridgeReadyFile;
      if (!std::filesystem::is_regular_file(readyPath, error) || error)
        return;

      mergeLiveCommandEvidence(
        result.implementedUnitCommandEvidence,
        readProofBackedLiveCommandEvidence(
          readyPath,
          "command_surface.live_unit_command.",
          result.implementedUnitCommands,
          preflight));
      mergeLiveCommandEvidence(
        result.implementedGameActionEvidence,
        readProofBackedLiveCommandEvidence(
          readyPath,
          "command_surface.live_game_action.",
          result.implementedGameActions,
          preflight));
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
        result.implementedUnitCommandEvidence =
          staticManifestCommandEvidence(manifest.manifest.unitCommandEvidence);
        result.implementedGameActionEvidence =
          staticManifestCommandEvidence(manifest.manifest.gameActionEvidence);
        result.implementedApiSurfaceMethods = manifest.manifest.implementedApiSurfaceMethods;
        result.implementedCommandSurfaceEntries = manifest.manifest.implementedCommandSurfaceEntries;
      }
    }
    if (!manifestLoaded)
      addBuiltInApiSurface(result, contract);
    if (!manifestLoaded)
      addBuiltInCommandSurface(result);
    contract = applyRuntimeExecutorBridgeContractProofs(environment_, contract);

    const RuntimeMemoryAccessResult memoryAccess = openProcessMemoryAccess(environment_.processId);
    if (memoryAccess.accessible)
      addCapabilityIfMissing(result, Capability::SharedMemoryClient);

    RuntimeExecutorPreflightResult preflight = preflightRuntimeExecutor(environment_, contract);
    addValidatedBridgeCapabilities(result, preflight);
    applyBridgeLiveCommandEvidence(result, environment_, preflight);

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
