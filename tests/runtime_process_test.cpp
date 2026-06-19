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

  RuntimeEnvironment missing;
  missing.processId = 0;
  RuntimeProcessOpenResult missingResult = openRuntimeProcess(missing);
  assert(!missingResult.opened);
  assert(!missingResult.reason.empty());

  return 0;
}
