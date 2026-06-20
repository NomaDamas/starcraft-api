if(NOT DEFINED STARCRAFT_RUNTIME_GAP_REPORT)
  message(FATAL_ERROR "STARCRAFT_RUNTIME_GAP_REPORT is required")
endif()

if(NOT DEFINED STARCRAFT_API_TEST_FIXTURE_DIR)
  message(FATAL_ERROR "STARCRAFT_API_TEST_FIXTURE_DIR is required")
endif()

if(NOT DEFINED STARCRAFT_API_CLI_TEST_DIR)
  message(FATAL_ERROR "STARCRAFT_API_CLI_TEST_DIR is required")
endif()

set(evidence_path "${STARCRAFT_API_CLI_TEST_DIR}/runtime-gap-report.evidence")
set(log_dir "${STARCRAFT_API_CLI_TEST_DIR}/runtime-gap-report-logs")
set(process_snapshot "${STARCRAFT_API_CLI_TEST_DIR}/runtime-gap-report-processes.snapshot")
file(REMOVE "${evidence_path}")
file(MAKE_DIRECTORY "${log_dir}")
file(WRITE "${process_snapshot}" "")

execute_process(
  COMMAND
    "${CMAKE_COMMAND}" -E env
    "STARCRAFT_API_LOG_DIR=${log_dir}"
    "STARCRAFT_API_PROCESS_SNAPSHOT=${process_snapshot}"
    "${STARCRAFT_RUNTIME_GAP_REPORT}"
    --product starcraft-remastered
    --version test-build
    --executable "${STARCRAFT_API_TEST_FIXTURE_DIR}/remastered-complete.manifest"
    --evidence-out "${evidence_path}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE error
)

if(NOT result EQUAL 0)
  message(FATAL_ERROR "gap report evidence command failed: ${error}\n${output}")
endif()

if(NOT EXISTS "${evidence_path}")
  message(FATAL_ERROR "gap report did not write evidence file")
endif()

file(READ "${evidence_path}" evidence)
foreach(needle
    "evidence.schema=starcraft-api.runtime-evidence.v1"
    "runtime.reason=gap report did not launch runtime and no matching StarCraft game process is selected"
    "diagnosis.status=blocked-no-game-process"
    "diagnosis.ready_for_attach=false"
    "diagnosis.blocker_count=1")
  string(FIND "${evidence}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "gap report evidence missing '${needle}'\n${evidence}")
  endif()
endforeach()
