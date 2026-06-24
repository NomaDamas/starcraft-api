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

execute_process(
  COMMAND "${STARCRAFT_RUNTIME_MEMORY_PROBE}"
    --self
    --scan-u32-counters
    --find-writable-only
    --find-non-executable-only
    --counter-max-scan-mb 1
    --counter-sample-delay-ms 1
    --counter-result-limit 2
    --require-open
    --require-access
  RESULT_VARIABLE counter_scan_result
  OUTPUT_VARIABLE counter_scan_output
  ERROR_VARIABLE counter_scan_error
)
if(NOT counter_scan_result EQUAL 0)
  message(FATAL_ERROR "expected self counter scan to run\nstdout:\n${counter_scan_output}\nstderr:\n${counter_scan_error}")
endif()

foreach(needle
    "memory.counter_scan.requested=true"
    "memory.counter_scan.scan_success=true"
    "memory.counter_scan.sample_delay_ms=1"
    "memory.counter_scan.max_scan_bytes=1048576"
    "memory.counter_scan.filter.writable_only=true"
    "memory.counter_scan.filter.non_executable_only=true"
    "memory.counter_scan.candidate_regions="
    "memory.counter_scan.scanned_regions="
    "memory.counter_scan.preliminary_count="
    "memory.counter_scan.validated_count="
    "memory.counter_scan.printed_count=")
  string(FIND "${counter_scan_output}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "counter-scan memory probe output missing '${needle}'\n${counter_scan_output}")
  endif()
endforeach()

execute_process(
  COMMAND "${STARCRAFT_RUNTIME_MEMORY_PROBE}"
    --self
    --region-list
    --region-list-limit 2
    --require-open
    --require-access
  RESULT_VARIABLE region_list_result
  OUTPUT_VARIABLE region_list_output
  ERROR_VARIABLE region_list_error
)
if(NOT region_list_result EQUAL 0)
  message(FATAL_ERROR "expected self region list to pass\nstdout:\n${region_list_output}\nstderr:\n${region_list_error}")
endif()

foreach(needle
    "memory.region_list.requested=true"
    "memory.region_list.success=true"
    "memory.region_list.region.0.address=0x"
    "memory.region_list.region.0.end=0x"
    "memory.region_list.region.0.readable="
    "memory.region_list.region.0.writable="
    "memory.region_list.region.0.executable="
    "memory.region_list.match_count="
    "memory.region_list.printed_count="
    "memory.region_list.limit=2")
  string(FIND "${region_list_output}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "region-list memory probe output missing '${needle}'\n${region_list_output}")
  endif()
endforeach()

string(FIND "${find_output}" "memory.find.match.count=0" zero_match_index)
if(NOT zero_match_index EQUAL -1)
  message(FATAL_ERROR "filtered self memory find did not locate the fixture\n${find_output}")
endif()

execute_process(
  COMMAND "${STARCRAFT_RUNTIME_MEMORY_PROBE}"
    --self
    --find-u32 0x5ca1ab1e
    --find-writable-only
    --find-non-executable-only
    --find-max-scan-mb 1024
    --require-open
    --require-access
    --require-find
  RESULT_VARIABLE find_u32_result
  OUTPUT_VARIABLE find_u32_output
  ERROR_VARIABLE find_u32_error
)
if(NOT find_u32_result EQUAL 0)
  message(FATAL_ERROR "expected filtered self u32 memory find to pass\nstdout:\n${find_u32_output}\nstderr:\n${find_u32_error}")
endif()
foreach(needle
    "memory.find.requested=true"
    "memory.find.scan_success=true"
    "memory.find.success=true"
    "memory.find.needle.u32=0x5ca1ab1e"
    "memory.find.filter.writable_only=true"
    "memory.find.filter.non_executable_only=true"
    "memory.find.match.count=")
  string(FIND "${find_u32_output}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "u32 memory probe output missing '${needle}'\n${find_u32_output}")
  endif()
endforeach()

string(FIND "${find_u32_output}" "memory.find.match.count=0" zero_u32_match_index)
if(NOT zero_u32_match_index EQUAL -1)
  message(FATAL_ERROR "filtered self u32 memory find did not locate the fixture\n${find_u32_output}")
endif()

execute_process(
  COMMAND "${STARCRAFT_RUNTIME_MEMORY_PROBE}"
    --self
    --find-ascii "${self_needle}"
    --find-u32 0x5ca1ab1e
    --find-writable-only
    --find-non-executable-only
    --find-max-scan-mb 1024
    --require-open
    --require-access
    --require-find
  RESULT_VARIABLE find_multi_result
  OUTPUT_VARIABLE find_multi_output
  ERROR_VARIABLE find_multi_error
)
if(NOT find_multi_result EQUAL 0)
  message(FATAL_ERROR "expected filtered self multi-needle memory find to pass\nstdout:\n${find_multi_output}\nstderr:\n${find_multi_error}")
endif()
foreach(needle
    "memory.find.requested=true"
    "memory.find.scan_success=true"
    "memory.find.success=true"
    "memory.find.needle_count=2"
    "memory.find.needle.0.kind=ascii"
    "memory.find.needle.0.value=${self_needle}"
    "memory.find.needle.0.match.count="
    "memory.find.needle.1.kind=u32"
    "memory.find.needle.1.value=0x5ca1ab1e"
    "memory.find.needle.1.match.count="
    "memory.find.match.count=")
  string(FIND "${find_multi_output}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "multi-needle memory probe output missing '${needle}'\n${find_multi_output}")
  endif()
endforeach()

foreach(unexpected
    "memory.find.needle.0.match.count=0"
    "memory.find.needle.1.match.count=0"
    "memory.find.match.count=0")
  string(FIND "${find_multi_output}" "${unexpected}" unexpected_index)
  if(NOT unexpected_index EQUAL -1)
    message(FATAL_ERROR "multi-needle memory probe did not locate all fixtures: '${unexpected}'\n${find_multi_output}")
  endif()
endforeach()

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
