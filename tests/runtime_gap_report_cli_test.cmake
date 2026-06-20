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
    "executor.behavior_proof.missing_count="
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
    "diagnosis.game_process_count=0"
    "diagnosis.blocker_count=1")
  string(FIND "${evidence}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "gap report evidence missing '${needle}'\n${evidence}")
  endif()
endforeach()

execute_process(
  COMMAND
    "${STARCRAFT_RUNTIME_GAP_REPORT}"
    --product starcraft-remastered
    --version test-build
    --executable "${STARCRAFT_API_TEST_FIXTURE_DIR}/remastered-complete.manifest"
    --category unit-command
  RESULT_VARIABLE filtered_result
  OUTPUT_VARIABLE filtered_output
  ERROR_VARIABLE filtered_error
)

if(NOT filtered_result EQUAL 0)
  message(FATAL_ERROR "gap report category command failed: ${filtered_error}\n${filtered_output}")
endif()

foreach(needle
    "implementation_gap.filter.category=unit-command"
    "implementation_gap.filtered_count="
    "implementation_gap.0.category=unit-command"
    "implementation_gap.0.id=Attack_Move")
  string(FIND "${filtered_output}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "gap report category output missing '${needle}'\n${filtered_output}")
  endif()
endforeach()

string(FIND "${filtered_output}" "implementation_gap.0.category=backend" backend_index)
if(NOT backend_index EQUAL -1)
  message(FATAL_ERROR "gap report category output included an unfiltered backend gap\n${filtered_output}")
endif()

execute_process(
  COMMAND
    "${STARCRAFT_RUNTIME_GAP_REPORT}"
    --product starcraft-remastered
    --version test-build
    --executable "${STARCRAFT_API_TEST_FIXTURE_DIR}/remastered-complete.manifest"
    --summary-only
  RESULT_VARIABLE summary_result
  OUTPUT_VARIABLE summary_output
  ERROR_VARIABLE summary_error
)

if(NOT summary_result EQUAL 0)
  message(FATAL_ERROR "gap report summary command failed: ${summary_error}\n${summary_output}")
endif()

foreach(needle
    "implementation_gap.count="
    "executor.behavior_proof.missing_count="
    "implementation_gap.category.backend.count=1")
  string(FIND "${summary_output}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "gap report summary output missing '${needle}'\n${summary_output}")
  endif()
endforeach()

set(bridge_dir "${STARCRAFT_API_CLI_TEST_DIR}/runtime-gap-report-bridge")
file(REMOVE_RECURSE "${bridge_dir}")
file(MAKE_DIRECTORY "${bridge_dir}")
file(WRITE "${bridge_dir}/ready"
  "protocol=starcraft-api-file-bridge-v1\n"
  "product=starcraft-remastered\n"
  "version=test-build\n"
  "mode=validated-runtime-adapter\n"
  "proof.attach=passed\n"
  "proof.read_game_state=passed\n"
  "proof.read_units=passed\n"
  "proof.issue_commands=passed\n"
  "proof.draw_overlays=passed\n"
  "proof.dispatch_events=passed\n"
  "proof.replay_analysis=passed\n"
  "proof.battle_net_policy=passed\n")

execute_process(
  COMMAND
    "${STARCRAFT_RUNTIME_GAP_REPORT}"
    --product starcraft-remastered
    --version test-build
    --executable "${STARCRAFT_API_TEST_FIXTURE_DIR}/remastered-complete.manifest"
    --bridge "${bridge_dir}"
  RESULT_VARIABLE bridge_result
  OUTPUT_VARIABLE bridge_output
  ERROR_VARIABLE bridge_error
)

if(NOT bridge_result EQUAL 0)
  message(FATAL_ERROR "gap report bridge proof command failed: ${bridge_error}\n${bridge_output}")
endif()

foreach(needle
    "executor.bridge_mode=validated-runtime-adapter"
    "executor.behavior_proof.missing_count=1"
    "executor.behavior_proof.missing=proof.multiplayer_sync=passed"
    "readiness.blocking_gap=executor-behavior-proof-complete"
    "implementation_gap.category.executor-behavior-proof.count=1")
  string(FIND "${bridge_output}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "gap report bridge proof output missing '${needle}'\n${bridge_output}")
  endif()
endforeach()

string(FIND "${summary_output}" "readiness.check.id=" check_index)
if(NOT check_index EQUAL -1)
  message(FATAL_ERROR "gap report summary output included readiness detail rows\n${summary_output}")
endif()

string(FIND "${summary_output}" "implementation_gap.0.category=" gap_index)
if(NOT gap_index EQUAL -1)
  message(FATAL_ERROR "gap report summary output included implementation gap detail rows\n${summary_output}")
endif()
