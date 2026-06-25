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
    "implementation_gap.category.capability.count="
    "implementation_gap.category.data-address.count="
    "implementation_gap.category.structure-layout.count="
    "implementation_gap.category.structure-field.count="
    "implementation_gap.category.executor-preflight.count="
    "diagnosis.status=blocked-no-game-process"
    "diagnosis.ready_for_attach=false"
    "diagnosis.game_process_count=0"
    "diagnosis.blocker_count=1"
    "implementation_gap.0.category=backend"
    "category=data-address"
    "id=BW::BWDATA::Game"
    "category=executor-preflight"
    "id=runtime-process-identified")
  string(FIND "${output}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "gap report output missing '${needle}'\n${output}")
  endif()
endforeach()

foreach(forbidden
    "implementation_gap.category.command-surface.count="
    "implementation_gap.category.unit-command.count="
    "implementation_gap.category.game-action.count="
    "category=command-surface"
    "category=unit-command"
    "category=game-action")
  string(FIND "${output}" "${forbidden}" forbidden_index)
  if(NOT forbidden_index EQUAL -1)
    message(FATAL_ERROR "gap report output should not include command surface implementation gap '${forbidden}'\n${output}")
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

set(support_evidence_path "${STARCRAFT_API_CLI_TEST_DIR}/runtime-gap-report-support.evidence")
set(support_log_dir "${STARCRAFT_API_CLI_TEST_DIR}/runtime-gap-report-support-logs")
file(REMOVE "${support_evidence_path}")
file(MAKE_DIRECTORY "${support_log_dir}")
file(WRITE
  "${support_log_dir}/battle.net-support.log"
  "D 2026-06-19 08:00:06.450000 [UrlManager] {Main} Resolved URL: key=/client/error/BLZBNTBNA00000005; region=KR; endpoint=https://kr.battle.net/support/ko/article/BLZBNTBNA00000005?utm_medium=internal\n")

execute_process(
  COMMAND
    "${CMAKE_COMMAND}" -E env
    "STARCRAFT_API_LOG_DIR=${support_log_dir}"
    "STARCRAFT_API_PROCESS_SNAPSHOT=${process_snapshot}"
    "${STARCRAFT_RUNTIME_GAP_REPORT}"
    --product starcraft-remastered
    --version test-build
    --executable "${STARCRAFT_API_TEST_FIXTURE_DIR}/remastered-complete.manifest"
    --evidence-out "${support_evidence_path}"
    --summary-only
  RESULT_VARIABLE support_result
  OUTPUT_VARIABLE support_output
  ERROR_VARIABLE support_error
)

if(NOT support_result EQUAL 0)
  message(FATAL_ERROR "gap report support evidence command failed: ${support_error}\n${support_output}")
endif()

foreach(needle
    "diagnosis.status=blocked-battlenet-support-error-no-game-process"
    "diagnosis.battle_net_support_code=BLZBNTBNA00000005"
    "diagnosis.battle_net_support_url=https://kr.battle.net/support/ko/article/BLZBNTBNA00000005"
    "diagnosis.blocker.1=Battle.net reported support error BLZBNTBNA00000005")
  string(FIND "${support_output}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "gap report support output missing '${needle}'\n${support_output}")
  endif()
endforeach()

execute_process(
  COMMAND
    "${STARCRAFT_RUNTIME_GAP_REPORT}"
    --product starcraft-remastered
    --version test-build
    --executable "${STARCRAFT_API_TEST_FIXTURE_DIR}/remastered-complete.manifest"
    --category capability
  RESULT_VARIABLE filtered_result
  OUTPUT_VARIABLE filtered_output
  ERROR_VARIABLE filtered_error
)

if(NOT filtered_result EQUAL 0)
  message(FATAL_ERROR "gap report category command failed: ${filtered_error}\n${filtered_output}")
endif()

foreach(needle
    "implementation_gap.filter.category=capability"
    "implementation_gap.filtered_count="
    "implementation_gap.0.category=capability"
    "implementation_gap.0.id=read-game-state")
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

execute_process(
  COMMAND
    "${STARCRAFT_RUNTIME_GAP_REPORT}"
    --manifest "${STARCRAFT_API_TEST_FIXTURE_DIR}/remastered-complete.manifest"
    --product starcraft-remastered
    --version test-build
    --process-id 1
    --executable "${STARCRAFT_API_TEST_FIXTURE_DIR}/remastered-complete.manifest"
    --bridge "${STARCRAFT_API_CLI_TEST_DIR}/fixture-production-forbidden-bridge"
    --require-production
  RESULT_VARIABLE fixture_production_result
  OUTPUT_VARIABLE fixture_production_output
  ERROR_VARIABLE fixture_production_error
)

if(fixture_production_result EQUAL 0)
  message(FATAL_ERROR
    "fixture manifest under tests/fixtures must not satisfy --require-production\n${fixture_production_output}\n${fixture_production_error}")
endif()

foreach(needle
    "readiness.production_ready=false"
    "implementation_gap.count=1"
    "implementation_gap.0.id=fixture-manifest-production-forbidden")
  string(FIND "${fixture_production_output}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "fixture production rejection output missing '${needle}'\n${fixture_production_output}")
  endif()
endforeach()

set(bridge_dir "${STARCRAFT_API_CLI_TEST_DIR}/runtime-gap-report-bridge")
file(REMOVE_RECURSE "${bridge_dir}")
file(MAKE_DIRECTORY "${bridge_dir}")
file(WRITE "${bridge_dir}/issue_commands.snapshot.tsv"
  "command\tstorage_kind\tencoded_bytes\n"
  "pauseGame\tunit-test-runtime-command-queue-v1\t10\n")
if(WIN32)
  set(bridge_process_id "123")
else()
  execute_process(
    COMMAND /bin/sh -c "sleep 120 >/dev/null 2>&1 & echo $!"
    OUTPUT_VARIABLE bridge_process_id
    OUTPUT_STRIP_TRAILING_WHITESPACE)
endif()
file(WRITE "${bridge_dir}/ready"
  "protocol=starcraft-api-file-bridge-v1\n"
  "product=starcraft-remastered\n"
  "version=test-build\n"
  "process_id=${bridge_process_id}\n"
  "executable=${STARCRAFT_API_TEST_FIXTURE_DIR}/remastered-complete.manifest\n"
  "mode=validated-runtime-adapter\n"
  "resident.adapter=active\n"
  "resident.adapter.abi=starcraft-api-resident-adapter-v1\n"
  "resident.adapter.process_id=${bridge_process_id}\n"
  "resident.adapter.heartbeat=20\n"
  "resident.proof.read_game_state.source=resident\n"
  "resident.proof.read_game_state.process_id=${bridge_process_id}\n"
  "resident.proof.read_game_state.heartbeat=20\n"
  "resident.proof.read_game_state.sample_count=3\n"
  "resident.proof.read_game_state.frame_samples=10,11,12\n"
  "resident.proof.read_game_state.tick_samples=100,116,132\n"
  "resident.proof.active_match.source=resident\n"
  "resident.proof.active_match.process_id=${bridge_process_id}\n"
  "resident.proof.active_match.heartbeat=20\n"
  "resident.proof.active_match.mode=match\n"
  "resident.proof.active_match.unit_activity_count=3\n"
  "resident.proof.active_match.evidence=resident-frame-unit-activity\n"
  "command.receiver=active\n"
  "command.sink=runtime-command-queue-v1\n"
  "contract.binding.shared-memory-client-transport=transport|proof.attach=passed\n"
  "contract.binding.BW::BWDATA::sgdwBytesInCmdQueue=command-queue|proof.issue_commands=passed:bytes-in-command-queue\n"
  "contract.binding.BW::BWDATA::TurnBuffer=command-queue|proof.issue_commands=passed:turn-buffer\n"
  "proof.issue_commands.command=pauseGame\n"
  "proof.issue_commands.source=live-sc-r-command-path\n"
  "proof.issue_commands.delivery_checked=true\n"
  "proof.issue_commands.behavior_checked=true\n"
  "proof.issue_commands.self_fixture=false\n"
  "proof.issue_commands.pause_frame_counter_matched=true\n"
  "proof.issue_commands.vector_address=0x1000\n"
  "proof.issue_commands.storage_kind=live-sc-r-command-queue-v1\n"
  "proof.issue_commands.bytes_in_queue_address=0x1100\n"
  "proof.issue_commands.frame_counter_address=0x1200\n"
  "proof.issue_commands.encoded_bytes=10\n"
  "proof.issue_commands.stale_proof_bytes_cleared=true\n"
  "proof.issue_commands.snapshot=issue_commands.snapshot.tsv\n"
  "proof.attach=passed\n"
  "proof.read_game_state=passed\n"
  "proof.active_match_state=passed\n"
  "proof.read_units=passed\n"
  "proof.read_region_data=passed\n"
  "proof.issue_commands=passed\n"
  "proof.draw_overlays=passed\n"
  "proof.dispatch_events=passed\n"
  "proof.replay_analysis=passed\n"
  "proof.battle_net_policy=passed\n"
  "proof.load_ai_modules=passed\n")

execute_process(
  COMMAND
    "${STARCRAFT_RUNTIME_GAP_REPORT}"
    --product starcraft-remastered
    --version test-build
    --process-id "${bridge_process_id}"
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
    "executor.behavior_proof.missing_count=11"
    "executor.behavior_proof.missing=proof.attach=passed"
    "executor.behavior_proof.missing=proof.read_game_state=passed"
    "executor.behavior_proof.missing=proof.active_match_state=passed"
    "executor.behavior_proof.missing=proof.read_units=passed"
    "executor.behavior_proof.missing=proof.multiplayer_sync=passed"
    "readiness.blocking_gap=executor-behavior-proof-complete"
    "implementation_gap.category.transport.count=2"
    "implementation_gap.category.executor-behavior-proof.count=11")
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
