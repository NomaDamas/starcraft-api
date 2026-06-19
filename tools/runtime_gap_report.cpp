#include <BWAPI/Runtime/RuntimeBackend.h>
#include <BWAPI/Runtime/RuntimeContract.h>
#include <BWAPI/Runtime/RuntimeExecutor.h>
#include <BWAPI/Runtime/RuntimeManifest.h>
#include <BWAPI/Runtime/RuntimeReadiness.h>

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

  void printCheck(const RuntimeReadinessCheck& check)
  {
    std::cout << "readiness.check.id=" << check.id << '\n';
    std::cout << "readiness.check.passed=" << (check.passed ? "true" : "false") << '\n';
    std::cout << "readiness.check.severity=" << toString(check.severity) << '\n';
    if (!check.detail.empty())
      std::cout << "readiness.check.detail=" << check.detail << '\n';
  }
}

int main(int argc, char** argv)
{
  bool requireProduction = false;
  std::string manifestPath;

  for (int i = 1; i < argc; ++i)
  {
    const std::string arg = argv[i];
    if (arg == "--require-production")
      requireProduction = true;
    else if (arg == "--manifest")
    {
      if (i + 1 >= argc)
      {
        std::cerr << "--manifest requires a path\n";
        return 64;
      }
      manifestPath = argv[++i];
    }
    else if (arg == "--help" || arg == "-h")
    {
      std::cout << "usage: starcraft-runtime-gap-report [--manifest <path>] [--require-production]\n";
      return 0;
    }
    else
    {
      std::cerr << "unknown argument: " << arg << '\n';
      return 64;
    }
  }

  RuntimeEnvironment environment = RuntimeEnvironment::detectHost();
  if (!manifestPath.empty())
    environment.manifestPath = manifestPath;

  RuntimeContract contract = contractFor(environment);
  RuntimeManifestLoadResult manifest;
  if (!environment.manifestPath.empty())
  {
    manifest = loadRuntimeManifestFile(environment.manifestPath);
    std::cout << "manifest.loaded=" << (manifest.loaded ? "true" : "false") << '\n';
    for (const std::string& error : manifest.errors)
      std::cout << "manifest.error=" << error << '\n';
    for (const std::string& warning : manifest.warnings)
      std::cout << "manifest.warning=" << warning << '\n';
    if (manifest.loaded)
    {
      contract = manifest.manifest.contract;
      environment.product = manifest.manifest.contract.product;
      environment.version = manifest.manifest.contract.version;
    }
  }

  std::unique_ptr<RuntimeBackend> backend = createRuntimeBackend(environment);
  RuntimeProbeResult probe = backend->probe();
  RuntimeExecutorPreflightResult preflight = preflightRuntimeExecutor(environment, contract);
  RuntimeReadinessReport report = evaluateProductionReadiness(probe, contract, preflight);
  std::vector<RuntimeReadinessCheck> gaps = blockingReadinessGaps(report);

  std::cout << "platform=" << toString(environment.platform) << '\n';
  std::cout << "product=" << toString(environment.product) << '\n';
  if (!environment.executorBridgePath.empty())
    std::cout << "executor.bridge_path=" << environment.executorBridgePath << '\n';
  if (!preflight.executorName.empty())
    std::cout << "executor.name=" << preflight.executorName << '\n';
  std::cout << "backend.name=" << backend->name() << '\n';
  std::cout << "readiness.production_ready=" << (report.productionReady ? "true" : "false") << '\n';
  std::cout << "readiness.blocking_gap_count=" << gaps.size() << '\n';

  for (const RuntimeReadinessCheck& check : report.checks)
    printCheck(check);

  for (const RuntimeReadinessCheck& gap : gaps)
    std::cout << "readiness.blocking_gap=" << gap.id << '\n';

  if (requireProduction && !report.productionReady)
    return 2;

  return 0;
}
