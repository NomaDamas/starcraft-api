#include <BWAPI/Runtime/RuntimeContract.h>
#include <BWAPI/Runtime/RuntimeCommandSurface.h>

#include <algorithm>
#include <sstream>
#include <utility>

namespace BWAPI::Runtime
{
  namespace
  {
    std::vector<Capability> fullParityCapabilities()
    {
      return {
        Capability::ReadGameState,
        Capability::ReadMapData,
        Capability::ReadUnitData,
        Capability::ReadBulletData,
        Capability::ReadPlayerData,
        Capability::ReadRegionData,
        Capability::IssueCommands,
        Capability::DrawOverlays,
        Capability::DispatchEvents,
        Capability::ReplayAnalysis,
        Capability::MultiplayerSync,
        Capability::BattleNet,
        Capability::LoadAIModules,
        Capability::SharedMemoryClient
      };
    }

    RuntimeBinding requiredBinding(std::string name, BindingKind kind)
    {
      RuntimeBinding binding;
      binding.name = std::move(name);
      binding.kind = kind;
      binding.requirement = BindingRequirement::Required;
      return binding;
    }

    StructureLayout requiredStructure(std::string name, std::vector<StructureField> fields)
    {
      StructureLayout structure;
      structure.name = std::move(name);
      structure.requirement = BindingRequirement::Required;
      structure.fields = std::move(fields);
      return structure;
    }

    StructureField field(std::string name)
    {
      StructureField structureField;
      structureField.name = std::move(name);
      return structureField;
    }

    void addError(ContractValidationResult& result, std::string message)
    {
      result.errors.push_back(std::move(message));
    }

    bool startsWith(const std::string& value, const std::string& prefix)
    {
      return value.rfind(prefix, 0) == 0;
    }
  }

  const char* toString(BindingKind kind)
  {
    switch (kind)
    {
    case BindingKind::DataAddress: return "data-address";
    case BindingKind::FunctionAddress: return "function-address";
    case BindingKind::ImportedFunction: return "imported-function";
    case BindingKind::StructureLayout: return "structure-layout";
    case BindingKind::HookPoint: return "hook-point";
    case BindingKind::CommandQueue: return "command-queue";
    case BindingKind::Transport: return "transport";
    }
    return "unknown";
  }

  const char* toString(BindingRequirement requirement)
  {
    switch (requirement)
    {
    case BindingRequirement::Required: return "required";
    case BindingRequirement::Optional: return "optional";
    }
    return "unknown";
  }

  bool parseBindingKind(const std::string& value, BindingKind& kind)
  {
    if (value == "data-address")
      kind = BindingKind::DataAddress;
    else if (value == "function-address")
      kind = BindingKind::FunctionAddress;
    else if (value == "imported-function")
      kind = BindingKind::ImportedFunction;
    else if (value == "structure-layout")
      kind = BindingKind::StructureLayout;
    else if (value == "hook-point")
      kind = BindingKind::HookPoint;
    else if (value == "command-queue")
      kind = BindingKind::CommandQueue;
    else if (value == "transport")
      kind = BindingKind::Transport;
    else
      return false;

    return true;
  }

  bool parseBindingRequirement(const std::string& value, BindingRequirement& requirement)
  {
    if (value == "required")
      requirement = BindingRequirement::Required;
    else if (value == "optional")
      requirement = BindingRequirement::Optional;
    else
      return false;

    return true;
  }

  RuntimeContract makeBroodWar1161ParityContract()
  {
    RuntimeContract contract;
    contract.product = Product::StarCraftBroodWar1161;
    contract.version = "1.16.1";
    contract.requiredApiSurfaceMethods = 385;
    contract.requiredCommandSurfaceEntries = makeBWAPICommandSurface().totalEntries();
    contract.requiredCapabilities = fullParityCapabilities();
    contract.bindings = {
      requiredBinding("BW::BWDATA::Game", BindingKind::DataAddress),
      requiredBinding("BW::BWDATA::Players", BindingKind::DataAddress),
      requiredBinding("BW::BWDATA::UnitNodeTable", BindingKind::DataAddress),
      requiredBinding("BW::BWDATA::BulletNodeTable", BindingKind::DataAddress),
      requiredBinding("BW::BWDATA::MapTileArray", BindingKind::DataAddress),
      requiredBinding("BW::BWDATA::sgdwBytesInCmdQueue", BindingKind::CommandQueue),
      requiredBinding("BW::BWDATA::TurnBuffer", BindingKind::CommandQueue),
      requiredBinding("BW::BWFXN_ExecuteGameTriggers", BindingKind::FunctionAddress),
      requiredBinding("Storm::SNetReceiveMessage", BindingKind::ImportedFunction),
      requiredBinding("Storm::SNetSendTurn", BindingKind::ImportedFunction),
      requiredBinding("draw-game-layer-hook", BindingKind::HookPoint),
      requiredBinding("ai-module-loader", BindingKind::Transport),
      requiredBinding("shared-memory-client-transport", BindingKind::Transport)
    };
    contract.structures = {
      requiredStructure("BW::BWGame", { field("players"), field("alliance"), field("elapsedFrames") }),
      requiredStructure("BW::CUnit", { field("id"), field("position"), field("hitPoints"), field("order"), field("player") }),
      requiredStructure("BW::CBullet", { field("position"), field("velocity"), field("sourceUnit"), field("target") }),
      requiredStructure("BW::PlayerInfo", { field("stormId"), field("race"), field("resources"), field("supply") }),
      requiredStructure("BW::ReplayHeader", { field("mapName"), field("frameCount"), field("playerCount") })
    };
    return contract;
  }

  RuntimeContract makeRemasteredParityContract(std::string version)
  {
    RuntimeContract contract = makeBroodWar1161ParityContract();
    contract.product = Product::StarCraftRemastered;
    contract.version = std::move(version);

    for (auto& binding : contract.bindings)
    {
      binding.resolved = false;
      binding.evidence.clear();
    }
    for (auto& structure : contract.structures)
    {
      structure.size = 0;
      structure.evidence.clear();
      for (auto& structureField : structure.fields)
      {
        structureField.offset = 0;
        structureField.size = 0;
        structureField.resolved = false;
        structureField.evidence.clear();
      }
    }
    return contract;
  }

  const RuntimeBinding* findRuntimeBinding(
    const RuntimeContract& contract,
    const std::string& name,
    BindingKind kind)
  {
    auto it = std::find_if(
      contract.bindings.begin(),
      contract.bindings.end(),
      [&](const RuntimeBinding& binding)
      {
        return binding.name == name && binding.kind == kind;
      });
    return it == contract.bindings.end() ? nullptr : &*it;
  }

  const StructureLayout* findStructureLayout(
    const RuntimeContract& contract,
    const std::string& name)
  {
    auto it = std::find_if(
      contract.structures.begin(),
      contract.structures.end(),
      [&](const StructureLayout& structure)
      {
        return structure.name == name;
      });
    return it == contract.structures.end() ? nullptr : &*it;
  }

  const StructureField* findStructureField(
    const RuntimeContract& contract,
    const std::string& structureName,
    const std::string& fieldName)
  {
    const StructureLayout* structure = findStructureLayout(contract, structureName);
    if (structure == nullptr)
      return nullptr;

    auto it = std::find_if(
      structure->fields.begin(),
      structure->fields.end(),
      [&](const StructureField& field)
      {
        return field.name == fieldName;
      });
    return it == structure->fields.end() ? nullptr : &*it;
  }

  ContractValidationResult validateRuntimeContract(const RuntimeContract& contract)
  {
    ContractValidationResult result;

    if (contract.product == Product::Unknown)
      addError(result, "runtime product is unknown");
    if (contract.version.empty())
      addError(result, "runtime version is empty");
    if (contract.requiredCapabilities.empty())
      addError(result, "runtime contract has no required capabilities");
    if (contract.requiredApiSurfaceMethods <= 0)
      addError(result, "runtime contract has no required API surface method count");
    if (contract.requiredCommandSurfaceEntries <= 0)
      addError(result, "runtime contract has no required command surface entry count");

    for (const RuntimeBinding& binding : contract.bindings)
    {
      if (binding.name.empty())
      {
        addError(result, "runtime binding has an empty name");
        continue;
      }
      if (binding.requirement == BindingRequirement::Required && !binding.resolved)
      {
        std::ostringstream message;
        message << "required " << toString(binding.kind) << " binding is unresolved: " << binding.name;
        addError(result, message.str());
      }
      if (binding.resolved && binding.evidence.empty())
      {
        std::ostringstream message;
        message << "resolved binding is missing validation evidence: " << binding.name;
        result.warnings.push_back(message.str());
      }
      if (binding.resolved && startsWith(binding.evidence, "fixture:"))
      {
        std::ostringstream message;
        message << "resolved binding uses fixture validation evidence: " << binding.name;
        result.warnings.push_back(message.str());
      }
    }

    for (const StructureLayout& structure : contract.structures)
    {
      if (structure.name.empty())
      {
        addError(result, "structure layout has an empty name");
        continue;
      }
      if (structure.requirement == BindingRequirement::Required && structure.size == 0)
        addError(result, "required structure has no validated size: " + structure.name);

      for (const StructureField& structureField : structure.fields)
      {
        if (structureField.name.empty())
        {
          addError(result, "structure field has an empty name in: " + structure.name);
          continue;
        }
        if (structure.requirement == BindingRequirement::Required && !structureField.resolved)
        {
          addError(result, "required structure field is unresolved: " + structure.name + "." + structureField.name);
        }
        if (structureField.resolved && structureField.size == 0)
        {
          addError(result, "resolved structure field has no size: " + structure.name + "." + structureField.name);
        }
      }
    }

    result.valid = result.errors.empty();
    return result;
  }

  bool contractContainsFixtureEvidence(const RuntimeContract& contract)
  {
    const auto fixtureEvidence = [](const std::string& evidence)
    {
      return startsWith(evidence, "fixture:")
        || startsWith(evidence, "unit-test:")
        || startsWith(evidence, "mock:")
        || startsWith(evidence, "self-fixture:")
        || startsWith(evidence, "diagnostic.")
        || startsWith(evidence, "static-anchor:")
        || startsWith(evidence, "scr-platform-anchor:")
        || startsWith(evidence, "static-layout:");
    };

    if (std::any_of(
      contract.bindings.begin(),
      contract.bindings.end(),
      [&](const RuntimeBinding& binding)
      {
        return binding.resolved && fixtureEvidence(binding.evidence);
      }))
      return true;

    for (const StructureLayout& structure : contract.structures)
    {
      if (structure.size != 0 && fixtureEvidence(structure.evidence))
        return true;
      for (const StructureField& field : structure.fields)
      {
        if (field.resolved && fixtureEvidence(field.evidence))
          return true;
      }
    }
    return false;
  }

  bool hasCapability(const RuntimeProbeResult& probe, Capability capability)
  {
    return std::find(probe.capabilities.begin(), probe.capabilities.end(), capability) != probe.capabilities.end();
  }

  bool canClaimProductionSupport(const RuntimeProbeResult& probe, const RuntimeContract& contract)
  {
    if (!probe.supported)
      return false;

    const ContractValidationResult validation = validateRuntimeContract(contract);
    if (!validation.valid)
      return false;
    if (contractContainsFixtureEvidence(contract))
      return false;
    if (probe.implementedApiSurfaceMethods < contract.requiredApiSurfaceMethods)
      return false;
    if (probe.implementedCommandSurfaceEntries < contract.requiredCommandSurfaceEntries)
      return false;

    const RuntimeCommandSurface commandSurface = makeBWAPICommandSurface();
    for (const std::string& unitCommand : commandSurface.unitCommands)
    {
      if (!containsCommandSurfaceEntry(probe.implementedUnitCommands, unitCommand))
        return false;
    }
    for (const std::string& gameAction : commandSurface.gameActions)
    {
      if (!containsCommandSurfaceEntry(probe.implementedGameActions, gameAction))
        return false;
    }

    return std::all_of(
      contract.requiredCapabilities.begin(),
      contract.requiredCapabilities.end(),
      [&](Capability capability)
      {
        return hasCapability(probe, capability);
      });
  }
}
