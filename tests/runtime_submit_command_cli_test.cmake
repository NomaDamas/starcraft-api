if(NOT DEFINED STARCRAFT_RUNTIME_SUBMIT_COMMAND)
  message(FATAL_ERROR "STARCRAFT_RUNTIME_SUBMIT_COMMAND is required")
endif()
if(NOT DEFINED STARCRAFT_API_TEST_FIXTURE_DIR)
  message(FATAL_ERROR "STARCRAFT_API_TEST_FIXTURE_DIR is required")
endif()
if(NOT DEFINED STARCRAFT_API_CLI_TEST_DIR)
  message(FATAL_ERROR "STARCRAFT_API_CLI_TEST_DIR is required")
endif()

if(NOT DEFINED STARCRAFT_API_CMAKE_COMMAND)
  set(STARCRAFT_API_CMAKE_COMMAND "${CMAKE_COMMAND}")
endif()

function(start_runtime_identity_process output_process_id)
  if(WIN32)
    string(REPLACE "'" "''" cmake_command_ps "${STARCRAFT_API_CMAKE_COMMAND}")
    execute_process(
      COMMAND powershell -NoProfile -ExecutionPolicy Bypass -Command
        "$p = Start-Process -FilePath '${cmake_command_ps}' -ArgumentList '-E','sleep','120' -WindowStyle Hidden -PassThru; Write-Output $p.Id"
      RESULT_VARIABLE start_result
      OUTPUT_VARIABLE start_output
      ERROR_VARIABLE start_error
      OUTPUT_STRIP_TRAILING_WHITESPACE)
  else()
    execute_process(
      COMMAND /bin/sh -c "\"${STARCRAFT_API_CMAKE_COMMAND}\" -E sleep 120 >/dev/null 2>&1 & echo $!"
      RESULT_VARIABLE start_result
      OUTPUT_VARIABLE start_output
      ERROR_VARIABLE start_error
      OUTPUT_STRIP_TRAILING_WHITESPACE)
  endif()

  string(STRIP "${start_output}" start_output)
  if(NOT start_result EQUAL 0 OR NOT start_output MATCHES "^[0-9]+$")
    message(FATAL_ERROR "unable to start live runtime identity fixture: ${start_error}\n${start_output}")
  endif()
  set(${output_process_id} "${start_output}" PARENT_SCOPE)
endfunction()

function(stop_runtime_identity_process process_id)
  if(NOT process_id MATCHES "^[0-9]+$")
    return()
  endif()
  if(WIN32)
    execute_process(
      COMMAND powershell -NoProfile -ExecutionPolicy Bypass -Command
        "Stop-Process -Id ${process_id} -Force -ErrorAction SilentlyContinue")
  else()
    execute_process(COMMAND kill "${process_id}")
  endif()
endfunction()

start_runtime_identity_process(bridge_process_id)

set(bridge_dir "${STARCRAFT_API_CLI_TEST_DIR}/runtime-submit-command-bridge")
file(REMOVE_RECURSE "${bridge_dir}")
file(MAKE_DIRECTORY "${bridge_dir}")
file(WRITE "${bridge_dir}/issue_commands.snapshot.tsv"
  "command\tstorage_kind\tencoded_bytes\n"
  "pauseGame\tunit-test-runtime-command-queue-v1\t10\n")
file(WRITE "${bridge_dir}/ready"
  "protocol=starcraft-api-file-bridge-v1\n"
  "product=starcraft-remastered\n"
  "version=test-build\n"
  "process_id=${bridge_process_id}\n"
  "executable=${STARCRAFT_RUNTIME_SUBMIT_COMMAND}\n"
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
  "proof.command_surface=runtime-command-surface-v1\n"
  "command.receiver=active\n"
  "command.sink=runtime-command-queue-v1\n"
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
  "proof.issue_commands=passed\n"
  "proof.draw_overlays=passed\n"
  "proof.dispatch_events=passed\n"
  "proof.replay_analysis=passed\n"
  "proof.multiplayer_sync=passed\n"
  "proof.battle_net_policy=passed\n"
  "proof.load_ai_modules=passed\n")

set(manifest "${STARCRAFT_API_TEST_FIXTURE_DIR}/remastered-complete.manifest")
execute_process(
  COMMAND "${STARCRAFT_RUNTIME_SUBMIT_COMMAND}"
    --manifest "${manifest}"
    --process-id "${bridge_process_id}"
    --executable "${STARCRAFT_RUNTIME_SUBMIT_COMMAND}"
    --bridge "${bridge_dir}"
    --game-action pauseGame
  RESULT_VARIABLE submit_result
  OUTPUT_VARIABLE submit_output
  ERROR_VARIABLE submit_error
)
if(NOT submit_result EQUAL 0)
  stop_runtime_identity_process("${bridge_process_id}")
  message(FATAL_ERROR "expected manifest-backed command submission to pass\nstdout:\n${submit_output}\nstderr:\n${submit_error}")
endif()
if(NOT submit_output MATCHES "submitted=true")
  stop_runtime_identity_process("${bridge_process_id}")
  message(FATAL_ERROR "expected submitted=true in CLI output\nstdout:\n${submit_output}")
endif()

file(READ "${bridge_dir}/commands.log" command_log)
if(NOT command_log MATCHES "game-action\\|pauseGame\\|0\\|")
  stop_runtime_identity_process("${bridge_process_id}")
  message(FATAL_ERROR "expected pauseGame command in bridge log\nlog:\n${command_log}")
endif()

execute_process(
  COMMAND "${STARCRAFT_RUNTIME_SUBMIT_COMMAND}"
    --dry-run-encode
    --unit-command Move
    --arg 100
    --arg 200
    --arg 0
  RESULT_VARIABLE dry_run_result
  OUTPUT_VARIABLE dry_run_output
  ERROR_VARIABLE dry_run_error
)
if(NOT dry_run_result EQUAL 0)
  stop_runtime_identity_process("${bridge_process_id}")
  message(FATAL_ERROR "expected dry-run command encoding to pass\nstdout:\n${dry_run_output}\nstderr:\n${dry_run_error}")
endif()
if(NOT dry_run_output MATCHES "encoded=true")
  stop_runtime_identity_process("${bridge_process_id}")
  message(FATAL_ERROR "expected encoded=true in dry-run output\nstdout:\n${dry_run_output}")
endif()
if(NOT dry_run_output MATCHES "encoded.bytes=15 64 00 c8 00 00 00 e4 00 06 00")
  stop_runtime_identity_process("${bridge_process_id}")
  message(FATAL_ERROR "expected Move command bytes in dry-run output\nstdout:\n${dry_run_output}")
endif()

execute_process(
  COMMAND "${STARCRAFT_RUNTIME_SUBMIT_COMMAND}"
    --dry-run-encode
    --unit-command Load
  RESULT_VARIABLE unsupported_dry_run_result
  OUTPUT_VARIABLE unsupported_dry_run_output
  ERROR_VARIABLE unsupported_dry_run_error
)
if(unsupported_dry_run_result EQUAL 0)
  stop_runtime_identity_process("${bridge_process_id}")
  message(FATAL_ERROR "expected unsupported dry-run command encoding to fail\nstdout:\n${unsupported_dry_run_output}\nstderr:\n${unsupported_dry_run_error}")
endif()
if(NOT unsupported_dry_run_output MATCHES "encoded=false")
  stop_runtime_identity_process("${bridge_process_id}")
  message(FATAL_ERROR "expected encoded=false in unsupported dry-run output\nstdout:\n${unsupported_dry_run_output}")
endif()

set(bootstrap_bridge_dir "${STARCRAFT_API_CLI_TEST_DIR}/runtime-submit-command-bootstrap-bridge")
file(REMOVE_RECURSE "${bootstrap_bridge_dir}")
file(MAKE_DIRECTORY "${bootstrap_bridge_dir}")
file(WRITE "${bootstrap_bridge_dir}/ready"
  "protocol=starcraft-api-file-bridge-v1\n"
  "product=starcraft-remastered\n"
  "version=test-build\n"
  "process_id=${bridge_process_id}\n"
  "executable=${STARCRAFT_RUNTIME_SUBMIT_COMMAND}\n"
  "mode=launch-attach-bootstrap\n")

execute_process(
  COMMAND "${STARCRAFT_RUNTIME_SUBMIT_COMMAND}"
    --manifest "${manifest}"
    --process-id "${bridge_process_id}"
    --executable "${STARCRAFT_RUNTIME_SUBMIT_COMMAND}"
    --bridge "${bootstrap_bridge_dir}"
    --game-action pauseGame
  RESULT_VARIABLE bootstrap_submit_result
  OUTPUT_VARIABLE bootstrap_submit_output
  ERROR_VARIABLE bootstrap_submit_error
)
if(bootstrap_submit_result EQUAL 0)
  stop_runtime_identity_process("${bridge_process_id}")
  message(FATAL_ERROR "expected bootstrap bridge command submission to fail\nstdout:\n${bootstrap_submit_output}\nstderr:\n${bootstrap_submit_error}")
endif()
if(NOT bootstrap_submit_output MATCHES "runtime executor bridge is not a validated runtime adapter")
  stop_runtime_identity_process("${bridge_process_id}")
  message(FATAL_ERROR "expected bootstrap bridge proof failure\nstdout:\n${bootstrap_submit_output}\nstderr:\n${bootstrap_submit_error}")
endif()

execute_process(
  COMMAND "${STARCRAFT_RUNTIME_SUBMIT_COMMAND}"
    --product starcraft-remastered
    --version test-build
    --process-id "${bridge_process_id}"
    --executable "${STARCRAFT_RUNTIME_SUBMIT_COMMAND}"
    --bridge "${bridge_dir}"
    --game-action pauseGame
  RESULT_VARIABLE bridge_surface_result
  OUTPUT_VARIABLE bridge_surface_output
  ERROR_VARIABLE bridge_surface_error
)
if(NOT bridge_surface_result EQUAL 0)
  stop_runtime_identity_process("${bridge_process_id}")
  message(FATAL_ERROR "expected bridge-surface command submission to pass without manifest\nstdout:\n${bridge_surface_output}\nstderr:\n${bridge_surface_error}")
endif()
if(NOT bridge_surface_output MATCHES "submitted=true")
  stop_runtime_identity_process("${bridge_process_id}")
  message(FATAL_ERROR "expected submitted=true for bridge-surface submission\nstdout:\n${bridge_surface_output}")
endif()
if(NOT bridge_surface_output MATCHES "bridge-proven BWAPI command surface")
  stop_runtime_identity_process("${bridge_process_id}")
  message(FATAL_ERROR "expected bridge-surface validation warning\nstdout:\n${bridge_surface_output}")
endif()

file(REMOVE_RECURSE "${bridge_dir}")
file(REMOVE_RECURSE "${bootstrap_bridge_dir}")
stop_runtime_identity_process("${bridge_process_id}")
