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
    "contract.binding.shared-memory-client-transport=transport|proof.attach=passed"
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
string(FIND "${ready}" "proof.read_units=passed" read_units_index)
if(NOT read_units_index EQUAL -1)
  message(FATAL_ERROR "attach proof must not claim read-units behavior\n${ready}")
endif()

file(REMOVE_RECURSE "${bridge_dir}")

set(units_bridge_dir "${STARCRAFT_API_CLI_TEST_DIR}/runtime-adapter-proof-units-bridge")
file(REMOVE_RECURSE "${units_bridge_dir}")

execute_process(
  COMMAND "${STARCRAFT_RUNTIME_ADAPTER_PROOF}"
    --self
    --product starcraft-remastered
    --version test-build
    --bridge "${units_bridge_dir}"
    --prove-read-units
    --self-unit-fixture
  RESULT_VARIABLE units_result
  OUTPUT_VARIABLE units_output
  ERROR_VARIABLE units_error
)
if(NOT units_result EQUAL 0)
  message(FATAL_ERROR "expected read-units proof to pass with self fixture\nstdout:\n${units_output}\nstderr:\n${units_error}")
endif()
foreach(needle
    "read_units.unit_array=true"
    "read_units.record_size=336"
    "read_units.active_records="
    "proof.read_units=passed")
  string(FIND "${units_output}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "read-units proof output missing '${needle}'\n${units_output}")
  endif()
endforeach()

set(units_ready_file "${units_bridge_dir}/ready")
if(NOT EXISTS "${units_ready_file}")
  message(FATAL_ERROR "read-units proof did not write ready file")
endif()
file(READ "${units_ready_file}" units_ready)
foreach(needle
    "proof.attach=passed"
    "contract.binding.shared-memory-client-transport=transport|proof.attach=passed"
    "proof.read_units.address=0x"
    "proof.read_units.record_size=336"
    "proof.read_units.active_records="
    "contract.binding.BW::BWDATA::UnitNodeTable=data-address|proof.read_units=passed"
    "contract.structure.BW::CUnit=336|proof.read_units=passed"
    "contract.field.BW::CUnit.position=40|4|proof.read_units=passed"
    "contract.field.BW::CUnit.hitPoints=8|4|proof.read_units=passed"
    "contract.field.BW::CUnit.player=76|1|proof.read_units=passed"
    "proof.read_units=passed")
  string(FIND "${units_ready}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "read-units ready file missing '${needle}'\n${units_ready}")
  endif()
endforeach()

file(REMOVE_RECURSE "${units_bridge_dir}")

set(policy_bridge_dir "${STARCRAFT_API_CLI_TEST_DIR}/runtime-adapter-proof-policy-bridge")
set(policy_root "${STARCRAFT_API_CLI_TEST_DIR}/runtime-adapter-proof-policy-root")
set(policy_executable "${STARCRAFT_RUNTIME_ADAPTER_PROOF}")
set(policy_process_snapshot "${STARCRAFT_API_CLI_TEST_DIR}/runtime-adapter-proof-policy-processes.snapshot")
file(REMOVE_RECURSE "${policy_bridge_dir}" "${policy_root}")
file(WRITE "${policy_process_snapshot}" "123 1 ${policy_executable} -launch -uid s1\n")

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env
    "STARCRAFT_API_PROCESS_SNAPSHOT=${policy_process_snapshot}"
    "${STARCRAFT_RUNTIME_ADAPTER_PROOF}"
      --self
      --product starcraft-remastered
      --version test-build
      --executable "${policy_executable}"
      --bridge "${policy_bridge_dir}"
      --prove-battle-net-policy
  RESULT_VARIABLE policy_result
  OUTPUT_VARIABLE policy_output
  ERROR_VARIABLE policy_error
)
if(NOT policy_result EQUAL 0)
  message(FATAL_ERROR "expected Battle.net policy proof to pass\nstdout:\n${policy_output}\nstderr:\n${policy_error}")
endif()
foreach(needle
    "battle_net_policy.ready_for_attach=true"
    "battle_net_policy.game_process_count=1"
    "battle_net_policy.blocker_count=0"
    "proof.battle_net_policy=passed")
  string(FIND "${policy_output}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "Battle.net policy proof output missing '${needle}'\n${policy_output}")
  endif()
endforeach()

set(policy_ready_file "${policy_bridge_dir}/ready")
if(NOT EXISTS "${policy_ready_file}")
  message(FATAL_ERROR "Battle.net policy proof did not write ready file")
endif()
file(READ "${policy_ready_file}" policy_ready)
foreach(needle
    "proof.attach=passed"
    "proof.battle_net_policy.status=runtime-process-visible"
    "proof.battle_net_policy.game_process_count=1"
    "proof.battle_net_policy.blocker_count=0"
    "proof.battle_net_policy=passed")
  string(FIND "${policy_ready}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "Battle.net policy ready file missing '${needle}'\n${policy_ready}")
  endif()
endforeach()

file(REMOVE_RECURSE "${policy_bridge_dir}" "${policy_root}")
