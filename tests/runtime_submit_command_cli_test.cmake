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
file(WRITE "${bridge_dir}/ready" "protocol=starcraft-api-file-bridge-v1\nproduct=starcraft-remastered\nversion=test-build\n")

set(manifest "${STARCRAFT_API_TEST_FIXTURE_DIR}/remastered-complete.manifest")
execute_process(
  COMMAND "${STARCRAFT_RUNTIME_SUBMIT_COMMAND}"
    --product starcraft-remastered
    --version test-build
    --manifest "${manifest}"
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
