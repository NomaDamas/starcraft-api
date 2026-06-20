#include <BWAPI/Runtime/RuntimeBackend.h>
#include <BWAPI/Runtime/RuntimeContract.h>
#include <BWAPI/Runtime/RuntimeExecutor.h>
#include <BWAPI/Runtime/RuntimeImplementationGap.h>
#include <BWAPI/Runtime/RuntimeInstallation.h>
#include <BWAPI/Runtime/RuntimeManifest.h>
#include <BWAPI/Runtime/RuntimeReadiness.h>

#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

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
  bool summaryOnly = false;
  std::string manifestPath;
  std::string evidenceOut;
  std::string categoryFilter;

  for (int i = 1; i < argc; ++i)
  {
    const std::string arg = argv[i];
    if (arg == "--require-production")
      requireProduction = true;
    else if (arg == "--summary-only")
      summaryOnly = true;
    else if (arg == "--manifest")
    {
      if (i + 1 >= argc)
      {
        std::cerr << "--manifest requires a path\n";
        return 64;
      }
      manifestPath = argv[++i];
    }
    else if (arg == "--evidence-out")
    {
      if (i + 1 >= argc)
      {
        std::cerr << "--evidence-out requires a path\n";
        return 64;
      }
      evidenceOut = argv[++i];
    }
    else if (arg == "--category")
    {
      if (i + 1 >= argc)
      {
        std::cerr << "--category requires a value\n";
        return 64;
      }
      categoryFilter = argv[++i];
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
        << "  --evidence-out <path>    write launch/attach diagnostic evidence\n"
        << "  --category <name>        print only implementation gaps for one category\n"
        << "  --summary-only           omit per-check and per-gap detail rows\n"
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
    else if (arg == "--evidence-out")
    {
      ++i;
    }
    else if (arg == "--category")
    {
      ++i;
    }
    else if (arg == "--require-production")
    {
    }
    else if (arg == "--summary-only")
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

  RuntimeContract contract;
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

  environment = resolveRuntimeEnvironment(environment);

  if (!evidenceOut.empty())
  {
    RuntimeInstallation installation = detectStarCraftInstallation(environment);
    RuntimeLaunchResult launchResult;
    launchResult.running = environment.processId > 0;
    launchResult.processId = environment.processId;
    launchResult.reason = launchResult.running
      ? "gap report selected an existing runtime process id"
      : "gap report did not launch runtime and no matching StarCraft game process is selected";

    std::string error;
    if (!writeRuntimeEvidenceReport(installation, launchResult, evidenceOut, error))
    {
      std::cerr << error << '\n';
      return 1;
    }
    std::cout << "evidence.path=" << evidenceOut << '\n';
  }

  if (manifest.loaded)
    contract = manifest.manifest.contract;
  else
    contract = contractFor(environment);

  std::unique_ptr<RuntimeBackend> backend = createRuntimeBackend(environment);
  RuntimeProbeResult probe = backend->probe();
  RuntimeExecutorPreflightResult preflight = preflightRuntimeExecutor(environment, contract);
  RuntimeReadinessReport report = evaluateProductionReadiness(probe, contract, preflight);
  std::vector<RuntimeReadinessCheck> gaps = blockingReadinessGaps(report);
  std::vector<RuntimeImplementationGap> implementationGaps =
    collectRuntimeImplementationGaps(probe, contract, preflight);
  std::vector<RuntimeImplementationGapCategoryCount> implementationGapCategories =
    summarizeRuntimeImplementationGapsByCategory(implementationGaps);
  std::vector<RuntimeImplementationGap> displayedImplementationGaps;
  for (const RuntimeImplementationGap& gap : implementationGaps)
  {
    if (categoryFilter.empty() || gap.category == categoryFilter)
      displayedImplementationGaps.push_back(gap);
  }

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
  std::cout << "implementation_gap.count=" << implementationGaps.size() << '\n';
  std::cout << "implementation_gap.category_count=" << implementationGapCategories.size() << '\n';
  if (!categoryFilter.empty())
  {
    std::cout << "implementation_gap.filter.category=" << categoryFilter << '\n';
    std::cout << "implementation_gap.filtered_count=" << displayedImplementationGaps.size() << '\n';
  }

  if (!summaryOnly)
  {
    for (const RuntimeReadinessCheck& check : report.checks)
      printCheck(check);

    for (const RuntimeReadinessCheck& gap : gaps)
      std::cout << "readiness.blocking_gap=" << gap.id << '\n';
  }

  for (const RuntimeImplementationGapCategoryCount& category : implementationGapCategories)
    std::cout << "implementation_gap.category." << category.category << ".count=" << category.count << '\n';

  if (summaryOnly)
  {
    if (requireProduction && !report.productionReady)
      return 2;
    return 0;
  }

  for (std::size_t i = 0; i < displayedImplementationGaps.size(); ++i)
  {
    const RuntimeImplementationGap& gap = displayedImplementationGaps[i];
    std::cout << "implementation_gap." << i << ".category=" << gap.category << '\n';
    std::cout << "implementation_gap." << i << ".id=" << gap.id << '\n';
    if (!gap.detail.empty())
      std::cout << "implementation_gap." << i << ".detail=" << gap.detail << '\n';
  }

  if (requireProduction && !report.productionReady)
    return 2;

  return 0;
}
