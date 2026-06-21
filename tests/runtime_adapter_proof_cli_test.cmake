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
string(FIND "${ready}" "proof.active_match_state=passed" active_match_state_index)
if(NOT active_match_state_index EQUAL -1)
  message(FATAL_ERROR "attach proof must not claim active-match-state behavior\n${ready}")
endif()

file(REMOVE_RECURSE "${bridge_dir}")

set(units_bridge_dir "${STARCRAFT_API_CLI_TEST_DIR}/runtime-adapter-proof-units-bridge")
set(units_best_dump "${STARCRAFT_API_CLI_TEST_DIR}/runtime-adapter-proof-units-best.bin")
file(REMOVE_RECURSE "${units_bridge_dir}")
file(REMOVE "${units_best_dump}")

execute_process(
  COMMAND "${STARCRAFT_RUNTIME_ADAPTER_PROOF}"
    --self
    --product starcraft-remastered
    --version test-build
    --bridge "${units_bridge_dir}"
    --prove-read-units
    --state-max-scan-mb 1
    --unit-max-scan-mb 128
    --unit-scan-diagnostics
    --unit-best-dump-out "${units_best_dump}"
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
    "read_units.layout=bwapi-classic-cunit"
    "read_units.active_records="
    "read_units.scan.readable_writable_regions="
    "read_units.scan.readable_only_regions="
    "read_units.scan.scanned_readable_only_regions="
    "read_units.scan.executable_readable_regions="
    "read_units.scan.window_candidate_arrays_scored="
    "read_units.scan.field_plausible_records="
    "read_units.scan.sprite_rejected_records="
    "read_units.scan.best_dump.success=true"
    "read_units.scan.best_dump.path=${units_best_dump}"
    "proof.read_units=passed")
  string(FIND "${units_output}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "read-units proof output missing '${needle}'\n${units_output}")
  endif()
endforeach()
if(NOT EXISTS "${units_best_dump}")
  message(FATAL_ERROR "read-units best candidate dump was not written")
endif()

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
    "proof.read_units.layout=bwapi-classic-cunit"
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

string(FIND "${units_ready}" "proof.active_match_state=passed" fixture_active_match_index)
if(NOT fixture_active_match_index EQUAL -1)
  message(FATAL_ERROR "self fixture read-units proof must not claim active-match-state behavior\n${units_ready}")
endif()

file(REMOVE_RECURSE "${units_bridge_dir}")

set(active_match_bridge_dir "${STARCRAFT_API_CLI_TEST_DIR}/runtime-adapter-proof-active-match-bridge")
file(REMOVE_RECURSE "${active_match_bridge_dir}")

execute_process(
  COMMAND "${STARCRAFT_RUNTIME_ADAPTER_PROOF}"
    --self
    --product starcraft-remastered
    --version test-build
    --bridge "${active_match_bridge_dir}"
    --prove-active-match-state
    --self-unit-fixture
  RESULT_VARIABLE active_match_result
  OUTPUT_VARIABLE active_match_output
  ERROR_VARIABLE active_match_error
)
if(active_match_result EQUAL 0)
  message(FATAL_ERROR "expected active-match-state proof to reject self fixture\nstdout:\n${active_match_output}\nstderr:\n${active_match_error}")
endif()
foreach(needle
    "active_match_state.in_game=false"
    "self process and self fixtures cannot prove StarCraft active match state")
  string(FIND "${active_match_output}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "active-match-state rejection output missing '${needle}'\n${active_match_output}")
  endif()
endforeach()

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
