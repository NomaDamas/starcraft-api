if(NOT DEFINED STARCRAFT_RUNTIME_MEMORY_PROBE)
  message(FATAL_ERROR "STARCRAFT_RUNTIME_MEMORY_PROBE is required")
endif()

set(self_needle "starcraft-api-memory-probe-self-fixture")

execute_process(
  COMMAND "${STARCRAFT_RUNTIME_MEMORY_PROBE}"
    --self
    --find-ascii "${self_needle}"
    --find-writable-only
    --find-non-executable-only
    --find-max-scan-mb 1024
    --require-open
    --require-access
    --require-find
  RESULT_VARIABLE find_result
  OUTPUT_VARIABLE find_output
  ERROR_VARIABLE find_error
)
if(NOT find_result EQUAL 0)
  message(FATAL_ERROR "expected filtered self memory find to pass\nstdout:\n${find_output}\nstderr:\n${find_error}")
endif()

foreach(needle
    "memory.find.requested=true"
    "memory.find.scan_success=true"
    "memory.find.success=true"
    "memory.find.needle=${self_needle}"
    "memory.find.filter.writable_only=true"
    "memory.find.filter.non_executable_only=true"
    "memory.find.candidate_regions="
    "memory.find.scanned_regions="
    "memory.find.match.count=")
  string(FIND "${find_output}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "memory probe output missing '${needle}'\n${find_output}")
  endif()
endforeach()

string(FIND "${find_output}" "memory.find.match.count=0" zero_match_index)
if(NOT zero_match_index EQUAL -1)
  message(FATAL_ERROR "filtered self memory find did not locate the fixture\n${find_output}")
endif()

execute_process(
  COMMAND "${STARCRAFT_RUNTIME_MEMORY_PROBE}"
    --self
    --find-u64 0x7ffafefdfcfbfafa
    --find-writable-only
    --find-non-executable-only
    --find-max-scan-mb 1
    --require-find
  RESULT_VARIABLE absent_result
  OUTPUT_VARIABLE absent_output
  ERROR_VARIABLE absent_error
)
if(absent_result EQUAL 0)
  message(FATAL_ERROR "expected --require-find to fail for absent fixture\nstdout:\n${absent_output}\nstderr:\n${absent_error}")
endif()
foreach(needle
    "memory.find.scan_success=true"
    "memory.find.success=false"
    "memory.find.match.count=0")
  string(FIND "${absent_output}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "absent memory probe output missing '${needle}'\n${absent_output}")
  endif()
endforeach()

execute_process(
  COMMAND "${STARCRAFT_RUNTIME_MEMORY_PROBE}"
    --self
    --process-state
    --region-summary
    --require-open
    --require-access
  RESULT_VARIABLE diagnostic_result
  OUTPUT_VARIABLE diagnostic_output
  ERROR_VARIABLE diagnostic_error
)
if(NOT diagnostic_result EQUAL 0)
  message(FATAL_ERROR "expected self process diagnostics to pass\nstdout:\n${diagnostic_output}\nstderr:\n${diagnostic_error}")
endif()

foreach(needle
    "process.state.inspected="
    "process.state.suspended="
    "process.state.status="
    "process.state.thread_count="
    "memory.region_summary.requested=true"
    "memory.region_summary.success="
    "memory.region_summary.total_regions="
    "memory.region_summary.readable_regions="
    "memory.region_summary.readable_non_executable_bytes="
    "memory.region_summary.mapped_file_regions="
    "memory.region_summary.target_executable_mapped_regions=")
  string(FIND "${diagnostic_output}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "diagnostic memory probe output missing '${needle}'\n${diagnostic_output}")
  endif()
endforeach()
