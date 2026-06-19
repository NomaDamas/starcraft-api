#include <BWAPI/Runtime/RuntimeBackend.h>
#include <BWAPI/Runtime/RuntimeExecutor.h>
#include <BWAPI/Runtime/RuntimeInstallation.h>

#include <cstdlib>
#include <iostream>
#include <string>

using namespace BWAPI::Runtime;

namespace
{
  bool parseInt(const std::string& value, int& parsed)
  {
    char* end = nullptr;
    const long result = std::strtol(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0')
      return false;
    parsed = static_cast<int>(result);
    return true;
  }

  void printUsage()
  {
    std::cout
      << "usage: starcraft-runtime-submit-command [--product <product>] [--version <version>] "
      << "--bridge <dir> (--unit-command <name> --unit <id> | --game-action <name>) [--arg <int>...]\n";
  }
}

int main(int argc, char** argv)
{
  RuntimeEnvironment environment = RuntimeEnvironment::detectHost();
  RuntimeCommandRequest command;
  bool commandKindSelected = false;

  for (int i = 1; i < argc; ++i)
  {
    const std::string arg = argv[i];
    if (arg == "--help" || arg == "-h")
    {
      printUsage();
      return 0;
    }
    if (arg == "--product")
    {
      if (i + 1 >= argc)
      {
        std::cerr << "--product requires a value\n";
        return 64;
      }
      environment.product = parseProduct(argv[++i]);
      continue;
    }
    if (arg == "--version")
    {
      if (i + 1 >= argc)
      {
        std::cerr << "--version requires a value\n";
        return 64;
      }
      environment.version = argv[++i];
      continue;
    }
    if (arg == "--bridge")
    {
      if (i + 1 >= argc)
      {
        std::cerr << "--bridge requires a directory\n";
        return 64;
      }
      environment.executorBridgePath = argv[++i];
      continue;
    }
    if (arg == "--unit-command")
    {
      if (i + 1 >= argc)
      {
        std::cerr << "--unit-command requires a name\n";
        return 64;
      }
      command.kind = RuntimeCommandKind::UnitCommand;
      command.name = argv[++i];
      commandKindSelected = true;
      continue;
    }
    if (arg == "--game-action")
    {
      if (i + 1 >= argc)
      {
        std::cerr << "--game-action requires a name\n";
        return 64;
      }
      command.kind = RuntimeCommandKind::GameAction;
      command.name = argv[++i];
      commandKindSelected = true;
      continue;
    }
    if (arg == "--unit")
    {
      if (i + 1 >= argc || !parseInt(argv[++i], command.targetUnitId))
      {
        std::cerr << "--unit requires an integer id\n";
        return 64;
      }
      continue;
    }
    if (arg == "--arg")
    {
      int value = 0;
      if (i + 1 >= argc || !parseInt(argv[++i], value))
      {
        std::cerr << "--arg requires an integer value\n";
        return 64;
      }
      command.arguments.push_back(value);
      continue;
    }

    std::cerr << "unknown argument: " << arg << '\n';
    return 64;
  }

  environment = resolveRuntimeEnvironment(environment);

  if (environment.product == Product::Unknown)
  {
    std::cerr << "runtime product is required\n";
    return 64;
  }
  if (environment.version.empty())
  {
    std::cerr << "runtime version is required\n";
    return 64;
  }
  if (environment.executorBridgePath.empty())
  {
    std::cerr << "runtime executor bridge directory is required\n";
    return 64;
  }
  if (!commandKindSelected)
  {
    std::cerr << "a unit command or game action is required\n";
    return 64;
  }
  if (command.kind == RuntimeCommandKind::UnitCommand && command.targetUnitId < 0)
  {
    std::cerr << "unit command target id must be non-negative\n";
    return 64;
  }

  RuntimeExecutorSubmitResult submitted = submitRuntimeCommands(environment, { command });
  std::cout << "submitted=" << (submitted.submitted ? "true" : "false") << '\n';
  std::cout << "submitted.commands=" << submitted.submittedCommands << '\n';
  if (!submitted.reason.empty())
    std::cout << "reason=" << submitted.reason << '\n';
  for (const std::string& error : submitted.errors)
    std::cout << "error=" << error << '\n';
  for (const std::string& warning : submitted.warnings)
    std::cout << "warning=" << warning << '\n';

  return submitted.submitted ? 0 : 2;
}
