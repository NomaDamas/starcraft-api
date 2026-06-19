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

int main()
{
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

  RuntimeOpenResult open = backend->open();
  std::cout << "open.opened=" << (open.opened ? "true" : "false") << '\n';
  std::cout << "open.state=" << toString(open.state) << '\n';
  if (!open.reason.empty())
    std::cout << "open.reason=" << open.reason << '\n';
  backend->close();
  std::cout << "state.after_close=" << toString(backend->state()) << '\n';

  RuntimeContract contract = contractFor(environment);
  ContractValidationResult validation = validateRuntimeContract(contract);
  printValidation(validation);
  std::cout << "production.supported=" << (canClaimProductionSupport(probe, contract) ? "true" : "false") << '\n';

  return 0;
}
