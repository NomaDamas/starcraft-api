#include <BWAPI/Runtime/RuntimeBackend.h>
#include <BWAPI/Runtime/RuntimeContract.h>

#include <iostream>
#include <memory>

using namespace BWAPI::Runtime;

namespace
{
  RuntimeContract contractFor(const RuntimeEnvironment& environment)
  {
    if (environment.product == Product::StarCraftBroodWar1161)
      return makeBroodWar1161ParityContract();
    if (environment.product == Product::StarCraftRemastered)
      return makeRemasteredParityContract(environment.version.empty() ? "unknown" : environment.version);

    RuntimeContract contract;
    contract.product = Product::Unknown;
    return contract;
  }

  void printValidation(const ContractValidationResult& validation)
  {
    std::cout << "contract.valid=" << (validation.valid ? "true" : "false") << '\n';

    for (const std::string& error : validation.errors)
      std::cout << "contract.error=" << error << '\n';
    for (const std::string& warning : validation.warnings)
      std::cout << "contract.warning=" << warning << '\n';
  }
}

int main(int argc, char** argv)
{
  bool requireProduction = false;
  for (int i = 1; i < argc; ++i)
  {
    const std::string arg = argv[i];
    if (arg == "--require-production")
      requireProduction = true;
    else if (arg == "--help" || arg == "-h")
    {
      std::cout << "usage: starcraft-runtime-probe [--require-production]\n";
      return 0;
    }
    else
    {
      std::cerr << "unknown argument: " << arg << '\n';
      return 64;
    }
  }

  RuntimeEnvironment environment = RuntimeEnvironment::detectHost();
  std::unique_ptr<RuntimeBackend> backend = createRuntimeBackend(environment);

  std::cout << "platform=" << toString(environment.platform) << '\n';
  std::cout << "product=" << toString(environment.product) << '\n';
  std::cout << "version=" << (environment.version.empty() ? "unknown" : environment.version) << '\n';
  std::cout << "backend.name=" << backend->name() << '\n';

  RuntimeProbeResult probe = backend->probe();
  std::cout << "probe.supported=" << (probe.supported ? "true" : "false") << '\n';
  if (!probe.reason.empty())
    std::cout << "probe.reason=" << probe.reason << '\n';
  for (Capability capability : probe.capabilities)
    std::cout << "probe.capability=" << toString(capability) << '\n';
  std::cout << "probe.implemented_api_surface_methods=" << probe.implementedApiSurfaceMethods << '\n';

  RuntimeOpenResult open = backend->open();
  std::cout << "open.opened=" << (open.opened ? "true" : "false") << '\n';
  std::cout << "open.state=" << toString(open.state) << '\n';
  if (!open.reason.empty())
    std::cout << "open.reason=" << open.reason << '\n';
  backend->close();
  std::cout << "state.after_close=" << toString(backend->state()) << '\n';

  RuntimeContract contract = contractFor(environment);
  std::cout << "contract.required_api_surface_methods=" << contract.requiredApiSurfaceMethods << '\n';
  ContractValidationResult validation = validateRuntimeContract(contract);
  printValidation(validation);
  const bool productionSupported = canClaimProductionSupport(probe, contract);
  std::cout << "production.supported=" << (productionSupported ? "true" : "false") << '\n';

  if (requireProduction && !productionSupported)
    return 2;

  return 0;
}
