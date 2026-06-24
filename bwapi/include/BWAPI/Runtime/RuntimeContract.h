#pragma once

#include <BWAPI/Runtime/RuntimeBackend.h>

#include <cstddef>
#include <string>
#include <vector>

namespace BWAPI::Runtime
{
  enum class BindingKind
  {
    DataAddress,
    FunctionAddress,
    ImportedFunction,
    StructureLayout,
    HookPoint,
    CommandQueue,
    Transport
  };

  enum class BindingRequirement
  {
    Required,
    Optional
  };

  struct RuntimeBinding
  {
    std::string name;
    BindingKind kind = BindingKind::DataAddress;
    BindingRequirement requirement = BindingRequirement::Required;
    bool resolved = false;
    std::string evidence;
  };

  struct StructureField
  {
    std::string name;
    std::size_t offset = 0;
    std::size_t size = 0;
    bool resolved = false;
    std::string evidence;
  };

  struct StructureLayout
  {
    std::string name;
    std::size_t size = 0;
    BindingRequirement requirement = BindingRequirement::Required;
    std::string evidence;
    std::vector<StructureField> fields;
  };

  struct RuntimeContract
  {
    Product product = Product::Unknown;
    std::string version;
    int requiredApiSurfaceMethods = 0;
    int requiredCommandSurfaceEntries = 0;
    std::vector<Capability> requiredCapabilities;
    std::vector<RuntimeBinding> bindings;
    std::vector<StructureLayout> structures;
  };

  struct ContractValidationResult
  {
    bool valid = false;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
  };

  const char* toString(BindingKind kind);
  const char* toString(BindingRequirement requirement);
  bool parseBindingKind(const std::string& value, BindingKind& kind);
  bool parseBindingRequirement(const std::string& value, BindingRequirement& requirement);

  RuntimeContract makeBroodWar1161ParityContract();
  RuntimeContract makeRemasteredParityContract(std::string version);
  const RuntimeBinding* findRuntimeBinding(
    const RuntimeContract& contract,
    const std::string& name,
    BindingKind kind);
  const StructureLayout* findStructureLayout(
    const RuntimeContract& contract,
    const std::string& name);
  const StructureField* findStructureField(
    const RuntimeContract& contract,
    const std::string& structureName,
    const std::string& fieldName);
  ContractValidationResult validateRuntimeContract(const RuntimeContract& contract);
  bool contractContainsFixtureEvidence(const RuntimeContract& contract);
  std::vector<std::string> contractProductionEvidenceErrors(const RuntimeContract& contract);
  bool hasCapability(const RuntimeProbeResult& probe, Capability capability);
  bool canClaimProductionSupport(const RuntimeProbeResult& probe, const RuntimeContract& contract);
}
