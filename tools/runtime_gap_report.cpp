#include <BWAPI/Runtime/RuntimeBackend.h>
#include <BWAPI/Runtime/RuntimeContract.h>
#include <BWAPI/Runtime/RuntimeExecutor.h>
#include <BWAPI/Runtime/RuntimeManifest.h>
#include <BWAPI/Runtime/RuntimeReadiness.h>

#include <cstdlib>
#include <iostream>
#include <limits>
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

  int parsePositiveInt(const std::string& value, const char* label)
  {
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0' || parsed <= 0 || parsed > std::numeric_limits<int>::max())
    {
      std::cerr << label << " requires a positive integer\n";
      return -1;
    }
    return static_cast<int>(parsed);
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
    else if (arg == "--product"
             || arg == "--version"
             || arg == "--process-id"
             || arg == "--executable"
             || arg == "--bridge")
    {
      if (i + 1 >= argc)
      {
        std::cerr << arg << " requires a value\n";
        return 64;
      }
      ++i;
    }
    else if (arg == "--help" || arg == "-h")
    {
      std::cout
        << "usage: starcraft-runtime-gap-report [options]\n"
        << "  --manifest <path>        load a runtime manifest or bootstrap manifest\n"
        << "  --product <name>         override runtime product\n"
        << "  --version <version>      override runtime version\n"
        << "  --process-id <pid>       override runtime process id\n"
        << "  --executable <path>      override runtime executable path\n"
        << "  --bridge <path>          override runtime executor bridge directory\n"
        << "  --require-production     return non-zero unless production readiness passes\n";
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

  for (int i = 1; i < argc; ++i)
  {
    const std::string arg = argv[i];
    if (arg == "--manifest")
    {
      ++i;
    }
    else if (arg == "--require-production")
    {
    }
    else if (arg == "--product")
    {
      if (i + 1 >= argc)
      {
        std::cerr << "--product requires a value\n";
        return 64;
      }
      const Product product = parseProduct(argv[++i]);
      if (product == Product::Unknown)
      {
        std::cerr << "--product requires a known runtime product\n";
        return 64;
      }
      environment.product = product;
    }
    else if (arg == "--version")
    {
      if (i + 1 >= argc)
      {
        std::cerr << "--version requires a value\n";
        return 64;
      }
      environment.version = argv[++i];
    }
    else if (arg == "--process-id")
    {
      if (i + 1 >= argc)
      {
        std::cerr << "--process-id requires a value\n";
        return 64;
      }
      const int processId = parsePositiveInt(argv[++i], "--process-id");
      if (processId <= 0)
        return 64;
      environment.processId = processId;
    }
    else if (arg == "--executable")
    {
      if (i + 1 >= argc)
      {
        std::cerr << "--executable requires a path\n";
        return 64;
      }
      environment.executablePath = argv[++i];
    }
    else if (arg == "--bridge")
    {
      if (i + 1 >= argc)
      {
        std::cerr << "--bridge requires a path\n";
        return 64;
      }
      environment.executorBridgePath = argv[++i];
    }
    else if (arg == "--help" || arg == "-h")
    {
    }
    else
    {
      std::cerr << "unknown argument: " << arg << '\n';
      return 64;
    }
  }

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
    if (manifest.loaded || manifest.manifest.contract.product != Product::Unknown)
    {
      contract = manifest.manifest.contract;
      if (environment.product == Product::Unknown)
        environment.product = manifest.manifest.contract.product;
      if (environment.version.empty())
        environment.version = manifest.manifest.contract.version;
    }
  }

  if (contract.product == Product::Unknown)
    contract = contractFor(environment);

  std::unique_ptr<RuntimeBackend> backend = createRuntimeBackend(environment);
  RuntimeProbeResult probe = backend->probe();
  RuntimeExecutorPreflightResult preflight = preflightRuntimeExecutor(environment, contract);
  RuntimeReadinessReport report = evaluateProductionReadiness(probe, contract, preflight);
  std::vector<RuntimeReadinessCheck> gaps = blockingReadinessGaps(report);

  std::cout << "platform=" << toString(environment.platform) << '\n';
  std::cout << "product=" << toString(environment.product) << '\n';
  std::cout << "version=" << (environment.version.empty() ? "unknown" : environment.version) << '\n';
  if (environment.processId > 0)
    std::cout << "process.id=" << environment.processId << '\n';
  if (!environment.executablePath.empty())
    std::cout << "executable.path=" << environment.executablePath << '\n';
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
