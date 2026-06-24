#include <BWAPI/Runtime/RuntimeResidentBridge.h>

extern "C"
{
  const char* starcraft_api_resident_adapter_abi()
  {
    return BWAPI::Runtime::RuntimeResidentAdapterAbi;
  }

  int starcraft_api_resident_adapter_entry()
  {
    return 0;
  }
}
