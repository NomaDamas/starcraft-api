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
  };

  struct StructureLayout
  {
    std::string name;
    std::size_t size = 0;
    BindingRequirement requirement = BindingRequirement::Required;
    std::vector<StructureField> fields;
  };

  struct RuntimeContract
  {
    Product product = Product::Unknown;
    std::string version;
    int requiredApiSurfaceMethods = 0;
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

  RuntimeContract makeBroodWar1161ParityContract();
  RuntimeContract makeRemasteredParityContract(std::string version);
  ContractValidationResult validateRuntimeContract(const RuntimeContract& contract);
  bool hasCapability(const RuntimeProbeResult& probe, Capability capability);
  bool canClaimProductionSupport(const RuntimeProbeResult& probe, const RuntimeContract& contract);
}
