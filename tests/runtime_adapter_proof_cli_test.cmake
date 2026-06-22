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

set(command_queue_bridge_dir "${STARCRAFT_API_CLI_TEST_DIR}/runtime-adapter-proof-command-queue-bridge")
file(REMOVE_RECURSE "${command_queue_bridge_dir}")

execute_process(
  COMMAND "${STARCRAFT_RUNTIME_ADAPTER_PROOF}"
    --self
    --product starcraft-remastered
    --version test-build
    --bridge "${command_queue_bridge_dir}"
    --discover-command-queue
    --self-command-queue-fixture
    --unit-max-scan-mb 16
    --unit-scan-timeout-ms 5000
  RESULT_VARIABLE command_queue_result
  OUTPUT_VARIABLE command_queue_output
  ERROR_VARIABLE command_queue_error
)
if(NOT command_queue_result EQUAL 0)
  message(FATAL_ERROR "expected command queue discovery to pass with self fixture\nstdout:\n${command_queue_output}\nstderr:\n${command_queue_error}")
endif()
foreach(needle
    "command_queue_discovery.ready=true"
    "command_queue_discovery.candidate_count="
    "command_queue_discovery.best.vector_address=0x"
    "command_queue_discovery.proof_scope=discovery-only-not-command-behavior"
    "command_queue_discovery.snapshot.success=true"
    "proof.command_queue_discovery=candidate-found")
  string(FIND "${command_queue_output}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "command queue discovery output missing '${needle}'\n${command_queue_output}")
  endif()
endforeach()

set(command_queue_ready_file "${command_queue_bridge_dir}/ready")
if(NOT EXISTS "${command_queue_ready_file}")
  message(FATAL_ERROR "command queue discovery did not write ready file")
endif()
file(READ "${command_queue_ready_file}" command_queue_ready)
foreach(needle
    "proof.attach=passed"
    "proof.command_queue_discovery=candidate-found"
    "proof.command_queue_discovery.snapshot=command_queue.candidates.tsv"
    "proof.command_queue_discovery.proof_scope=discovery-only-not-command-behavior"
    "proof.command_queue_discovery.best.vector_address=0x")
  string(FIND "${command_queue_ready}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "command queue discovery ready file missing '${needle}'\n${command_queue_ready}")
  endif()
endforeach()
foreach(forbidden
    "proof.issue_commands=passed"
    "contract.binding.BW::BWDATA::sgdwBytesInCmdQueue=command-queue"
    "contract.binding.BW::BWDATA::TurnBuffer=command-queue")
  string(FIND "${command_queue_ready}" "${forbidden}" forbidden_index)
  if(NOT forbidden_index EQUAL -1)
    message(FATAL_ERROR "command queue discovery must not claim production command proof '${forbidden}'\n${command_queue_ready}")
  endif()
endforeach()

set(command_queue_snapshot "${command_queue_bridge_dir}/command_queue.candidates.tsv")
if(NOT EXISTS "${command_queue_snapshot}")
  message(FATAL_ERROR "command queue discovery snapshot was not written")
endif()
file(READ "${command_queue_snapshot}" command_queue_snapshot_content)
foreach(needle
    "vector_address"
    "buffer_begin"
    "capacity_bytes")
  string(FIND "${command_queue_snapshot_content}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "command queue discovery snapshot missing '${needle}'\n${command_queue_snapshot_content}")
  endif()
endforeach()

file(REMOVE_RECURSE "${command_queue_bridge_dir}")

set(map_bridge_dir "${STARCRAFT_API_CLI_TEST_DIR}/runtime-adapter-proof-map-bridge")
set(fake_install_root "${STARCRAFT_API_CLI_TEST_DIR}/fake-starcraft-install")
set(fake_replay_root "${STARCRAFT_API_CLI_TEST_DIR}/fake-starcraft-replays")
set(fake_autosave_dir "${fake_replay_root}/AutoSave/20260622")
file(REMOVE_RECURSE "${map_bridge_dir}" "${fake_install_root}" "${fake_replay_root}")
file(MAKE_DIRECTORY
  "${fake_install_root}/x86_64/StarCraft.app/Contents/MacOS"
  "${fake_install_root}/Maps/BroodWar"
  "${fake_autosave_dir}")
file(WRITE "${fake_install_root}/x86_64/StarCraft.app/Contents/MacOS/StarCraft" "fake executable\n")
file(WRITE "${fake_install_root}/Maps/BroodWar/(2)Astral Balance.scm" "fake map fixture\n")
set(fake_replay_payload "fake replay fixture payload\n")
set(fake_autosave_path "${fake_autosave_dir}/013435,(2)Astral Balance.rep")
set(fake_last_replay_path "${fake_replay_root}/LastReplay.rep")
file(WRITE "${fake_autosave_path}" "${fake_replay_payload}")
file(WRITE "${fake_last_replay_path}" "${fake_replay_payload}")

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env
    "STARCRAFT_API_INSTALL_DIR=${fake_install_root}"
    "STARCRAFT_API_REPLAY_DIR=${fake_replay_root}"
    "${STARCRAFT_RUNTIME_ADAPTER_PROOF}"
    --self
    --product starcraft-remastered
    --version test-build
    --bridge "${map_bridge_dir}"
    --prove-read-map-data
    --state-max-scan-mb 1
    --state-scan-timeout-ms 1
  RESULT_VARIABLE map_result
  OUTPUT_VARIABLE map_output
  ERROR_VARIABLE map_error
)
if(NOT map_result EQUAL 0)
  message(FATAL_ERROR "expected replay-artifact map proof to pass\nstdout:\n${map_output}\nstderr:\n${map_error}")
endif()
foreach(needle
    "read_map_data.ready=true"
    "read_map_data.map_name=(2)Astral Balance"
    "read_map_data.source=latest-replay-artifact"
    "read_map_data.replay_path=${fake_autosave_path}"
    "proof.read_map_data=passed")
  string(FIND "${map_output}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "map proof output missing '${needle}'\n${map_output}")
  endif()
endforeach()

set(map_ready_file "${map_bridge_dir}/ready")
if(NOT EXISTS "${map_ready_file}")
  message(FATAL_ERROR "map proof did not write ready file")
endif()
file(READ "${map_ready_file}" map_ready)
foreach(needle
    "proof.attach=passed"
    "proof.read_map_data.map_name=(2)Astral Balance"
    "proof.read_map_data.map_path=${fake_install_root}/Maps/BroodWar/(2)Astral Balance.scm"
    "proof.read_map_data.source=latest-replay-artifact"
    "proof.read_map_data.replay_path=${fake_autosave_path}"
    "proof.read_map_data.snapshot=map.snapshot.tsv"
    "proof.read_map_data=passed")
  string(FIND "${map_ready}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "map proof ready file missing '${needle}'\n${map_ready}")
  endif()
endforeach()

file(READ "${map_bridge_dir}/map.snapshot.tsv" map_snapshot)
foreach(needle
    "source"
    "latest-replay-artifact"
    "${fake_autosave_path}")
  string(FIND "${map_snapshot}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "map snapshot missing '${needle}'\n${map_snapshot}")
  endif()
endforeach()

file(REMOVE_RECURSE "${map_bridge_dir}" "${fake_install_root}" "${fake_replay_root}")

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
    --unit-candidate-address 0x1
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
    "read_units.candidate_address.count=1"
    "read_units.unit_array=true"
    "read_units.record_size=336"
    "read_units.layout=bwapi-classic-cunit"
    "read_units.active_records="
    "read_units.scan.readable_writable_regions="
    "read_units.scan.readable_only_regions="
    "read_units.scan.scanned_readable_only_regions="
    "read_units.scan.executable_readable_regions="
    "read_units.scan.image_mapped_regions="
    "read_units.scan.skipped_image_mapped_regions="
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

set(combined_bridge_dir "${STARCRAFT_API_CLI_TEST_DIR}/runtime-adapter-proof-combined-bridge")
file(REMOVE_RECURSE "${combined_bridge_dir}")

execute_process(
  COMMAND "${STARCRAFT_RUNTIME_ADAPTER_PROOF}"
    --self
    --product starcraft-remastered
    --version test-build
    --bridge "${combined_bridge_dir}"
    --prove-read-game-state
    --state-max-scan-mb 1
    --state-scan-timeout-ms 1
    --state-scan-diagnostics
    --prove-read-units
    --unit-max-scan-mb 128
    --self-unit-fixture
  RESULT_VARIABLE combined_result
  OUTPUT_VARIABLE combined_output
  ERROR_VARIABLE combined_error
)
if(NOT combined_result EQUAL 0 AND NOT combined_result EQUAL 4)
  message(FATAL_ERROR "expected combined proof to pass or fail only read-game-state\nstdout:\n${combined_output}\nstderr:\n${combined_error}")
endif()
foreach(needle
    "read_game_state.live_counter="
    "read_game_state.scan.scanned_regions="
    "read_game_state.scan.candidate_counters="
    "read_units.unit_array=true"
    "proof.read_units=passed")
  string(FIND "${combined_output}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "combined proof output missing '${needle}'\n${combined_output}")
  endif()
endforeach()

set(combined_ready_file "${combined_bridge_dir}/ready")
if(NOT EXISTS "${combined_ready_file}")
  message(FATAL_ERROR "combined proof did not write ready file")
endif()
file(READ "${combined_ready_file}" combined_ready)
string(FIND "${combined_ready}" "proof.read_units=passed" combined_units_index)
if(combined_units_index EQUAL -1)
  message(FATAL_ERROR "combined proof must preserve passing read-units proof\n${combined_ready}")
endif()

set(dispatch_bridge_dir "${STARCRAFT_API_CLI_TEST_DIR}/runtime-adapter-proof-dispatch-bridge")
file(REMOVE_RECURSE "${dispatch_bridge_dir}")

execute_process(
  COMMAND "${STARCRAFT_RUNTIME_ADAPTER_PROOF}"
    --self
    --product starcraft-remastered
    --version test-build
    --bridge "${dispatch_bridge_dir}"
    --prove-dispatch-events
    --state-max-scan-mb 1
    --state-scan-timeout-ms 1
    --unit-max-scan-mb 128
    --self-unit-fixture
  RESULT_VARIABLE dispatch_result
  OUTPUT_VARIABLE dispatch_output
  ERROR_VARIABLE dispatch_error
)
if(dispatch_result EQUAL 0)
  message(FATAL_ERROR "expected dispatch-events proof to reject self fixture\nstdout:\n${dispatch_output}\nstderr:\n${dispatch_error}")
endif()
foreach(needle
    "active_match_state.in_game=false"
    "dispatch_events.ready=false")
  string(FIND "${dispatch_output}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "dispatch-events rejection output missing '${needle}'\n${dispatch_output}")
  endif()
endforeach()

set(dispatch_ready_file "${dispatch_bridge_dir}/ready")
if(NOT EXISTS "${dispatch_ready_file}")
  message(FATAL_ERROR "failed dispatch-events proof must still write partial attach ready file")
endif()
file(READ "${dispatch_ready_file}" dispatch_ready)
foreach(needle
    "mode=validated-runtime-adapter"
    "proof.attach=passed")
  string(FIND "${dispatch_ready}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "partial dispatch ready file missing '${needle}'\n${dispatch_ready}")
  endif()
endforeach()
string(FIND "${dispatch_ready}" "proof.dispatch_events=passed" failed_dispatch_ready_index)
if(NOT failed_dispatch_ready_index EQUAL -1)
  message(FATAL_ERROR "failed dispatch-events proof must not claim passed behavior\n${dispatch_ready}")
endif()

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
set(active_match_ready_file "${active_match_bridge_dir}/ready")
if(NOT EXISTS "${active_match_ready_file}")
  message(FATAL_ERROR "failed active-match-state proof must still write partial attach ready file")
endif()
file(READ "${active_match_ready_file}" active_match_ready)
foreach(needle
    "mode=validated-runtime-adapter"
    "proof.attach=passed")
  string(FIND "${active_match_ready}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "partial active-match ready file missing '${needle}'\n${active_match_ready}")
  endif()
endforeach()
string(FIND "${active_match_ready}" "proof.active_match_state=passed" failed_active_match_ready_index)
if(NOT failed_active_match_ready_index EQUAL -1)
  message(FATAL_ERROR "failed active-match-state proof must not claim passed behavior\n${active_match_ready}")
endif()

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
