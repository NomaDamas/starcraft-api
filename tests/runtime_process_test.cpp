#include <BWAPI/Runtime/RuntimeProcess.h>
#include <BWAPI/Runtime/RuntimeProcessMemory.h>

#include <cassert>

using namespace BWAPI::Runtime;

int main()
{
  RuntimeEnvironment environment = RuntimeEnvironment::detectHost();
  environment.processId = currentProcessId();

  assert(runtimeProcessExists(environment.processId));

  RuntimeProcessOpenResult opened = openRuntimeProcess(environment);
  assert(opened.opened);
  assert(opened.processId == environment.processId);
  assert(!runtimeProcessExecutablePath(environment.processId).empty());
  assert(!opened.executablePath.empty());

  RuntimeProcessStateResult state = inspectRuntimeProcessState(environment.processId);
  if (state.inspected)
  {
    assert(state.threadCount > 0);
    assert(!state.suspended);
  }
  else
  {
    assert(!state.reason.empty());
  }

  RuntimeProcessCommandLineResult commandLine =
    inspectRuntimeProcessCommandLine(environment.processId);
  if (commandLine.inspected)
  {
    assert(!commandLine.arguments.empty());
    assert(!commandLine.commandLine.empty());
  }
  else
  {
    assert(!commandLine.reason.empty());
  }

  RuntimeEnvironment mismatch = environment;
  mismatch.executablePath = "/definitely/not/the/current/runtime/process";
  RuntimeProcessOpenResult mismatchResult = openRuntimeProcess(mismatch);
  assert(!mismatchResult.opened);
  assert(mismatchResult.reason.find("executable does not match") != std::string::npos);

  RuntimeEnvironment missing;
  missing.processId = 0;
  RuntimeProcessOpenResult missingResult = openRuntimeProcess(missing);
  assert(!missingResult.opened);
  assert(!missingResult.reason.empty());

  return 0;
}
