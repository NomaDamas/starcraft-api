if(NOT DEFINED STARCRAFT_RUNTIME_INJECT_RESIDENT)
  message(FATAL_ERROR "STARCRAFT_RUNTIME_INJECT_RESIDENT is required")
endif()
if(NOT DEFINED STARCRAFT_API_RESIDENT_DYLIB)
  message(FATAL_ERROR "STARCRAFT_API_RESIDENT_DYLIB is required")
endif()
if(NOT DEFINED STARCRAFT_API_CLI_TEST_DIR)
  message(FATAL_ERROR "STARCRAFT_API_CLI_TEST_DIR is required")
endif()

set(resolve_bridge_dir "${STARCRAFT_API_CLI_TEST_DIR}/runtime-inject-resident-resolve-bridge")
file(REMOVE_RECURSE "${resolve_bridge_dir}")

execute_process(
  COMMAND "${STARCRAFT_RUNTIME_INJECT_RESIDENT}"
    --self
    --adapter "${STARCRAFT_API_RESIDENT_DYLIB}"
    --bridge "${resolve_bridge_dir}"
    --resolve-only
    --wait-ready-ms 0
  RESULT_VARIABLE resolve_result
  OUTPUT_VARIABLE resolve_output
  ERROR_VARIABLE resolve_error
)
if(NOT resolve_result EQUAL 0)
  message(FATAL_ERROR "expected resolve-only injector validation to pass\nstdout:\n${resolve_output}\nstderr:\n${resolve_error}")
endif()
foreach(needle
    "inject.dlopen_source=local-dlsym-public-abi:dlopen"
    "inject.pthread_exit_source=local-dlsym-public-abi:pthread_exit"
    "inject.region_list.success=true"
    "inject.dlopen_abi_safe=true"
    "inject.pthread_exit_abi_safe=true"
    "inject.dlopen_region.found=true"
    "inject.pthread_exit_region.found=true"
    "inject.dlopen_region.executable=true"
    "inject.pthread_exit_region.executable=true"
    "inject.dlopen_readable=true"
    "inject.pthread_exit_readable=true"
    "inject.resolve_only=true"
    "inject.aborted_before_thread_create=true")
  string(FIND "${resolve_output}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "resolve-only injector output missing '${needle}'\n${resolve_output}")
  endif()
endforeach()

set(legacy_bridge_dir "${STARCRAFT_API_CLI_TEST_DIR}/runtime-inject-resident-legacy-bridge")
file(REMOVE_RECURSE "${legacy_bridge_dir}")

execute_process(
  COMMAND "${STARCRAFT_RUNTIME_INJECT_RESIDENT}"
    --self
    --adapter "${STARCRAFT_API_RESIDENT_DYLIB}"
    --bridge "${legacy_bridge_dir}"
    --dyld-base 0x100000000
    --wait-ready-ms 0
  RESULT_VARIABLE legacy_result
  OUTPUT_VARIABLE legacy_output
  ERROR_VARIABLE legacy_error
)
if(legacy_result EQUAL 0)
  message(FATAL_ERROR "expected legacy dyld injector path to fail closed\nstdout:\n${legacy_output}\nstderr:\n${legacy_error}")
endif()
foreach(needle
    "inject.dlopen_source=legacy-/usr/lib/dyld"
    "inject.pthread_exit_source=legacy-/usr/lib/dyld"
    "inject.dlopen_abi_safe=false"
    "inject.pthread_exit_abi_safe=false"
    "inject.dlopen_abi_reason=unsafe symbol source: legacy-/usr/lib/dyld"
    "inject.pthread_exit_abi_reason=unsafe symbol source: legacy-/usr/lib/dyld"
    "inject.aborted_before_thread_create=true")
  string(FIND "${legacy_output}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "legacy injector output missing '${needle}'\nstdout:\n${legacy_output}\nstderr:\n${legacy_error}")
  endif()
endforeach()
