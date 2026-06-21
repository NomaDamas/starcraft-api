if(NOT DEFINED STARCRAFT_RUNTIME_SUBMIT_COMMAND)
  message(FATAL_ERROR "STARCRAFT_RUNTIME_SUBMIT_COMMAND is required")
endif()
if(NOT DEFINED STARCRAFT_API_TEST_FIXTURE_DIR)
  message(FATAL_ERROR "STARCRAFT_API_TEST_FIXTURE_DIR is required")
endif()
if(NOT DEFINED STARCRAFT_API_CLI_TEST_DIR)
  message(FATAL_ERROR "STARCRAFT_API_CLI_TEST_DIR is required")
endif()

set(bridge_dir "${STARCRAFT_API_CLI_TEST_DIR}/runtime-submit-command-bridge")
file(REMOVE_RECURSE "${bridge_dir}")
file(MAKE_DIRECTORY "${bridge_dir}")
file(WRITE "${bridge_dir}/ready"
  "protocol=starcraft-api-file-bridge-v1\n"
  "product=starcraft-remastered\n"
  "version=test-build\n"
  "process_id=1\n"
  "executable=${STARCRAFT_RUNTIME_SUBMIT_COMMAND}\n"
  "mode=validated-runtime-adapter\n"
  "command.receiver=active\n"
  "command.sink=runtime-command-queue-v1\n"
  "contract.binding.BW::BWDATA::sgdwBytesInCmdQueue=command-queue|unit-test:bytes-in-command-queue\n"
  "contract.binding.BW::BWDATA::TurnBuffer=command-queue|unit-test:turn-buffer\n"
  "proof.attach=passed\n"
  "proof.read_game_state=passed\n"
  "proof.active_match_state=passed\n"
  "proof.read_units=passed\n"
  "proof.issue_commands=passed\n"
  "proof.draw_overlays=passed\n"
  "proof.dispatch_events=passed\n"
  "proof.replay_analysis=passed\n"
  "proof.multiplayer_sync=passed\n"
  "proof.battle_net_policy=passed\n")

set(manifest "${STARCRAFT_API_TEST_FIXTURE_DIR}/remastered-complete.manifest")
execute_process(
  COMMAND "${STARCRAFT_RUNTIME_SUBMIT_COMMAND}"
    --manifest "${manifest}"
    --process-id 1
    --executable "${STARCRAFT_RUNTIME_SUBMIT_COMMAND}"
    --bridge "${bridge_dir}"
    --game-action pauseGame
  RESULT_VARIABLE submit_result
  OUTPUT_VARIABLE submit_output
  ERROR_VARIABLE submit_error
)
if(NOT submit_result EQUAL 0)
  message(FATAL_ERROR "expected manifest-backed command submission to pass\nstdout:\n${submit_output}\nstderr:\n${submit_error}")
endif()
if(NOT submit_output MATCHES "submitted=true")
  message(FATAL_ERROR "expected submitted=true in CLI output\nstdout:\n${submit_output}")
endif()

file(READ "${bridge_dir}/commands.log" command_log)
if(NOT command_log MATCHES "game-action\\|pauseGame\\|0\\|")
  message(FATAL_ERROR "expected pauseGame command in bridge log\nlog:\n${command_log}")
endif()

set(bootstrap_bridge_dir "${STARCRAFT_API_CLI_TEST_DIR}/runtime-submit-command-bootstrap-bridge")
file(REMOVE_RECURSE "${bootstrap_bridge_dir}")
file(MAKE_DIRECTORY "${bootstrap_bridge_dir}")
file(WRITE "${bootstrap_bridge_dir}/ready"
  "protocol=starcraft-api-file-bridge-v1\n"
  "product=starcraft-remastered\n"
  "version=test-build\n"
  "process_id=1\n"
  "executable=${STARCRAFT_RUNTIME_SUBMIT_COMMAND}\n"
  "mode=launch-attach-bootstrap\n")

execute_process(
  COMMAND "${STARCRAFT_RUNTIME_SUBMIT_COMMAND}"
    --manifest "${manifest}"
    --process-id 1
    --executable "${STARCRAFT_RUNTIME_SUBMIT_COMMAND}"
    --bridge "${bootstrap_bridge_dir}"
    --game-action pauseGame
  RESULT_VARIABLE bootstrap_submit_result
  OUTPUT_VARIABLE bootstrap_submit_output
  ERROR_VARIABLE bootstrap_submit_error
)
if(bootstrap_submit_result EQUAL 0)
  message(FATAL_ERROR "expected bootstrap bridge command submission to fail\nstdout:\n${bootstrap_submit_output}\nstderr:\n${bootstrap_submit_error}")
endif()
if(NOT bootstrap_submit_output MATCHES "validated runtime adapter proof is required")
  message(FATAL_ERROR "expected bootstrap bridge proof failure\nstdout:\n${bootstrap_submit_output}\nstderr:\n${bootstrap_submit_error}")
endif()

execute_process(
  COMMAND "${STARCRAFT_RUNTIME_SUBMIT_COMMAND}"
    --product starcraft-remastered
    --version test-build
    --bridge "${bridge_dir}"
    --game-action pauseGame
  RESULT_VARIABLE missing_manifest_result
  OUTPUT_VARIABLE missing_manifest_output
  ERROR_VARIABLE missing_manifest_error
)
if(missing_manifest_result EQUAL 0)
  message(FATAL_ERROR "expected missing manifest command submission to fail\nstdout:\n${missing_manifest_output}\nstderr:\n${missing_manifest_error}")
endif()
if(NOT missing_manifest_output MATCHES "runtime manifest is required")
  message(FATAL_ERROR "expected missing manifest failure reason\nstdout:\n${missing_manifest_output}\nstderr:\n${missing_manifest_error}")
endif()

file(REMOVE_RECURSE "${bridge_dir}")
file(REMOVE_RECURSE "${bootstrap_bridge_dir}")
