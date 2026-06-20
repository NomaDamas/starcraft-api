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

foreach(needle
    "implementation_gap.count="
    "implementation_gap.category_count="
    "implementation_gap.category.backend.count=1"
    "implementation_gap.category.api-surface.count=1"
    "implementation_gap.category.command-surface.count=1"
    "implementation_gap.category.unit-command.count="
    "implementation_gap.category.game-action.count="
    "implementation_gap.category.data-address.count="
    "implementation_gap.category.structure-layout.count="
    "implementation_gap.category.structure-field.count="
    "implementation_gap.category.executor-preflight.count="
    "implementation_gap.0.category=backend"
    "implementation_gap.1.category=api-surface"
    "implementation_gap.1.id=BWAPI.abstract-methods"
    "implementation_gap.3.category=unit-command"
    "implementation_gap.3.id=Attack_Move"
    "category=data-address"
    "id=BW::BWDATA::Game"
    "category=executor-preflight"
    "id=runtime-process-identified")
  string(FIND "${output}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "gap report output missing '${needle}'\n${output}")
  endif()
endforeach()

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
