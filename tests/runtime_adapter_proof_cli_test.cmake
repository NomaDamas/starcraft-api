if(NOT DEFINED STARCRAFT_RUNTIME_ADAPTER_PROOF)
  message(FATAL_ERROR "STARCRAFT_RUNTIME_ADAPTER_PROOF is required")
endif()
if(NOT DEFINED STARCRAFT_API_CLI_TEST_DIR)
  message(FATAL_ERROR "STARCRAFT_API_CLI_TEST_DIR is required")
endif()

set(bridge_dir "${STARCRAFT_API_CLI_TEST_DIR}/runtime-adapter-proof-bridge")
file(REMOVE_RECURSE "${bridge_dir}")

execute_process(
  COMMAND "${STARCRAFT_RUNTIME_ADAPTER_PROOF}"
    --self
    --product starcraft-remastered
    --version test-build
    --bridge "${bridge_dir}"
  RESULT_VARIABLE proof_result
  OUTPUT_VARIABLE proof_output
  ERROR_VARIABLE proof_error
)
if(NOT proof_result EQUAL 0)
  message(FATAL_ERROR "expected self attach proof to pass\nstdout:\n${proof_output}\nstderr:\n${proof_error}")
endif()
foreach(needle
    "attach.opened=true"
    "attach.memory_accessible=true"
    "proof.attach=passed")
  string(FIND "${proof_output}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "adapter proof output missing '${needle}'\n${proof_output}")
  endif()
endforeach()

set(ready_file "${bridge_dir}/ready")
if(NOT EXISTS "${ready_file}")
  message(FATAL_ERROR "adapter proof did not write ready file")
endif()
file(READ "${ready_file}" ready)
foreach(needle
    "protocol=starcraft-api-file-bridge-v1"
    "product=starcraft-remastered"
    "version=test-build"
    "executor=starcraft-api-attach-proof"
    "mode=validated-runtime-adapter"
    "proof.attach=passed")
  string(FIND "${ready}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "ready file missing '${needle}'\n${ready}")
  endif()
endforeach()

string(FIND "${ready}" "proof.read_game_state=passed" read_game_state_index)
if(NOT read_game_state_index EQUAL -1)
  message(FATAL_ERROR "attach proof must not claim read-game-state behavior\n${ready}")
endif()

file(REMOVE_RECURSE "${bridge_dir}")
