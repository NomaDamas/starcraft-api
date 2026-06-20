#include <BWAPI/Runtime/RuntimeImplementationGap.h>
#include <BWAPI/Runtime/RuntimeCommandSurface.h>

#include <algorithm>
#include <sstream>
#include <utility>

namespace BWAPI::Runtime
{
  namespace
  {
    void addGap(
      std::vector<RuntimeImplementationGap>& gaps,
      std::string category,
      std::string id,
      std::string detail)
    {
      gaps.push_back({ std::move(category), std::move(id), std::move(detail) });
    }

    std::string countDetail(int actual, int required)
    {
      std::ostringstream out;
      out << "implemented=" << actual << " required=" << required;
      return out.str();
    }

    bool containsValue(const std::vector<std::string>& values, const std::string& value)
    {
      return std::find(values.begin(), values.end(), value) != values.end();
    }
  }

  std::vector<RuntimeImplementationGap> collectRuntimeImplementationGaps(
    const RuntimeProbeResult& probe,
    const RuntimeContract& contract,
    const RuntimeExecutorPreflightResult& preflight)
  {
    std::vector<RuntimeImplementationGap> gaps;

    if (!probe.supported)
    {
      addGap(
        gaps,
        "backend",
        "runtime-backend",
        probe.reason.empty() ? "backend does not claim support" : probe.reason);
    }

    if (probe.implementedApiSurfaceMethods < contract.requiredApiSurfaceMethods)
    {
      addGap(
        gaps,
        "api-surface",
        "BWAPI.abstract-methods",
        countDetail(probe.implementedApiSurfaceMethods, contract.requiredApiSurfaceMethods));
    }
    if (probe.implementedCommandSurfaceEntries < contract.requiredCommandSurfaceEntries)
    {
      addGap(
        gaps,
        "command-surface",
        "BWAPI.command-action-entries",
        countDetail(probe.implementedCommandSurfaceEntries, contract.requiredCommandSurfaceEntries));
    }

    const RuntimeCommandSurface surface = makeBWAPICommandSurface();
    for (const std::string& command : surface.unitCommands)
    {
      if (!containsCommandSurfaceEntry(probe.implementedUnitCommands, command))
        addGap(gaps, "unit-command", command, "BWAPI unit command is not implemented by the runtime adapter");
    }
    for (const std::string& action : surface.gameActions)
    {
      if (!containsCommandSurfaceEntry(probe.implementedGameActions, action))
        addGap(gaps, "game-action", action, "BWAPI game action is not implemented by the runtime adapter");
    }

    for (Capability capability : contract.requiredCapabilities)
    {
      if (!hasCapability(probe, capability))
        addGap(gaps, "capability", toString(capability), "required runtime capability is not implemented");
    }

    for (const RuntimeBinding& binding : contract.bindings)
    {
      if (binding.requirement == BindingRequirement::Required && !binding.resolved)
        addGap(gaps, toString(binding.kind), binding.name, "required runtime binding is unresolved");
    }

    for (const StructureLayout& structure : contract.structures)
    {
      if (structure.requirement == BindingRequirement::Required && structure.size == 0)
        addGap(gaps, "structure-layout", structure.name, "required runtime structure size is unresolved");

      for (const StructureField& field : structure.fields)
      {
        if (structure.requirement == BindingRequirement::Required && !field.resolved)
        {
          addGap(
            gaps,
            "structure-field",
            structure.name + "." + field.name,
            "required runtime structure field is unresolved");
        }
      }
    }

    if (!preflight.processIdentified)
      addGap(gaps, "executor-preflight", "runtime-process-identified", "target runtime process is not identified");
    if (!preflight.targetLocated)
      addGap(gaps, "executor-preflight", "runtime-target-located", "target executable or runtime image is not located");
    if (!preflight.executorAvailable)
      addGap(gaps, "executor-preflight", "executor-available", "authorized runtime executor is not available");
    if (!preflight.executorBridgeMode.empty()
        && preflight.executorBridgeMode != RuntimeExecutorBridgeValidatedAdapterMode)
    {
      addGap(
        gaps,
        "executor-bridge-mode",
        preflight.executorBridgeMode,
        "runtime executor bridge mode is not validated-runtime-adapter");
    }
    for (const std::string& proof : preflight.missingBehaviorProofs)
    {
      addGap(
        gaps,
        "executor-behavior-proof",
        proof,
        "required runtime adapter behavior proof is missing");
    }

    const ContractValidationResult validation = validateRuntimeContract(contract);
    for (std::size_t i = 0; i < preflight.errors.size(); ++i)
    {
      if (containsValue(validation.errors, preflight.errors[i]))
        continue;
      addGap(gaps, "executor-preflight-error", "error." + std::to_string(i), preflight.errors[i]);
    }

    return gaps;
  }

  std::vector<RuntimeImplementationGapCategoryCount> summarizeRuntimeImplementationGapsByCategory(
    const std::vector<RuntimeImplementationGap>& gaps)
  {
    std::vector<RuntimeImplementationGapCategoryCount> counts;
    for (const RuntimeImplementationGap& gap : gaps)
    {
      auto it = std::find_if(
        counts.begin(),
        counts.end(),
        [&](const RuntimeImplementationGapCategoryCount& count)
        {
          return count.category == gap.category;
        });

      if (it == counts.end())
        counts.push_back({ gap.category, 1 });
      else
        ++it->count;
    }
    return counts;
  }

  std::size_t countRuntimeImplementationGapsByCategory(
    const std::vector<RuntimeImplementationGap>& gaps,
    const std::string& category)
  {
    return static_cast<std::size_t>(std::count_if(
      gaps.begin(),
      gaps.end(),
      [&](const RuntimeImplementationGap& gap)
      {
        return gap.category == category;
      }));
  }
}
