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
    "command_surface.ready=true"
    "command_surface.entries=72"
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
    "proof.command_surface=runtime-command-surface-v1"
    "command_surface.entries=72"
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
    "command_queue_discovery.vector_candidate_count="
    "command_queue_discovery.raw_turn_buffer_candidate_count="
    "command_queue_discovery.retained_vector_candidate_count="
    "command_queue_discovery.retained_raw_turn_buffer_candidate_count="
    "command_queue_discovery.best.vector_address=0x"
    "command_queue_discovery.best.kind="
    "command_queue_discovery.best.bytes_in_queue_address=0x"
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
    "proof.command_queue_discovery.vector_candidate_count="
    "proof.command_queue_discovery.raw_turn_buffer_candidate_count="
    "proof.command_queue_discovery.retained_vector_candidate_count="
    "proof.command_queue_discovery.retained_raw_turn_buffer_candidate_count="
    "proof.command_queue_discovery.best.kind="
    "proof.command_queue_discovery.best.bytes_in_queue_address=0x"
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
    "kind"
    "bytes_in_queue_address"
    "buffer_begin"
    "capacity_bytes"
    "counter_offset"
    "prefix_entropy_milli"
    "prefix_hex"
    "activity_changed_byte_total"
    "activity_min_used_bytes"
    "activity_max_used_bytes"
    "activity_selector_first_hex"
    "activity_selector_last_hex"
    "activity_buffer_first_hex"
    "activity_buffer_last_hex"
    "live_write_safe"
    "live_write_reason"
    "region_class"
    "buffer_region_class"
    "buffer_region_path")
  string(FIND "${command_queue_snapshot_content}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "command queue discovery snapshot missing '${needle}'\n${command_queue_snapshot_content}")
  endif()
endforeach()

file(REMOVE_RECURSE "${command_queue_bridge_dir}")

set(issue_commands_bridge_dir "${STARCRAFT_API_CLI_TEST_DIR}/runtime-adapter-proof-issue-commands-bridge")
file(REMOVE_RECURSE "${issue_commands_bridge_dir}")

execute_process(
  COMMAND "${STARCRAFT_RUNTIME_ADAPTER_PROOF}"
    --self
    --product starcraft-remastered
    --version test-build
    --bridge "${issue_commands_bridge_dir}"
    --prove-issue-commands
    --self-command-queue-fixture
    --unit-scan-timeout-ms 5000
  RESULT_VARIABLE issue_commands_result
  OUTPUT_VARIABLE issue_commands_output
  ERROR_VARIABLE issue_commands_error
)
if(NOT issue_commands_result EQUAL 12)
  message(FATAL_ERROR "expected self issue-commands proof to fail only behavior proof with code 12\nstdout:\n${issue_commands_output}\nstderr:\n${issue_commands_error}")
endif()
foreach(needle
    "command_queue_discovery.ready=true"
    "issue_commands.ready=false"
    "issue_commands.delivery_checked=true"
    "issue_commands.behavior_checked=false"
    "issue_commands.pause_frame_counter_sampled=false"
    "issue_commands.pause_frame_counter_matched=false"
    "issue_commands.attempt_count=1"
    "issue_commands.self_fixture=true"
    "issue_commands.reason=self command queue fixture append/readback passed; active StarCraft behavior proof is required"
    "issue_commands.snapshot.success=true")
  string(FIND "${issue_commands_output}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "self issue-commands output missing '${needle}'\n${issue_commands_output}")
  endif()
endforeach()

set(issue_commands_ready_file "${issue_commands_bridge_dir}/ready")
if(NOT EXISTS "${issue_commands_ready_file}")
  message(FATAL_ERROR "self issue-commands proof did not write partial ready file")
endif()
file(READ "${issue_commands_ready_file}" issue_commands_ready)
foreach(forbidden
    "proof.issue_commands=passed"
    "command.receiver=active"
    "command.sink=runtime-command-queue-v1"
    "contract.binding.BW::BWDATA::sgdwBytesInCmdQueue=command-queue"
    "contract.binding.BW::BWDATA::TurnBuffer=command-queue")
  string(FIND "${issue_commands_ready}" "${forbidden}" forbidden_index)
  if(NOT forbidden_index EQUAL -1)
    message(FATAL_ERROR "self issue-commands proof must not claim production command path '${forbidden}'\n${issue_commands_ready}")
  endif()
endforeach()

set(issue_commands_snapshot "${issue_commands_bridge_dir}/issue_commands.snapshot.tsv")
if(NOT EXISTS "${issue_commands_snapshot}")
  message(FATAL_ERROR "self issue-commands snapshot was not written")
endif()
file(READ "${issue_commands_snapshot}" issue_commands_snapshot_content)
foreach(needle
    "delivery_checked\ttrue"
    "behavior_checked\tfalse"
    "pause_frame_counter_sampled\tfalse"
    "pause_frame_counter_matched\tfalse"
    "attempt_count\t1"
    "storage_kind\traw-turn-buffer"
    "attempt_rank"
    "self_fixture\ttrue")
  string(FIND "${issue_commands_snapshot_content}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "self issue-commands snapshot missing '${needle}'\n${issue_commands_snapshot_content}")
  endif()
endforeach()

file(REMOVE_RECURSE "${issue_commands_bridge_dir}")

set(issue_commands_explicit_bridge_dir "${STARCRAFT_API_CLI_TEST_DIR}/runtime-adapter-proof-issue-commands-explicit-bridge")
file(REMOVE_RECURSE "${issue_commands_explicit_bridge_dir}")

execute_process(
  COMMAND "${STARCRAFT_RUNTIME_ADAPTER_PROOF}"
    --self
    --product starcraft-remastered
    --version test-build
    --bridge "${issue_commands_explicit_bridge_dir}"
    --prove-issue-commands
    --self-command-queue-fixture
    --command-queue-vector-address 0x1
    --unit-scan-timeout-ms 5000
  RESULT_VARIABLE issue_commands_explicit_result
  OUTPUT_VARIABLE issue_commands_explicit_output
  ERROR_VARIABLE issue_commands_explicit_error
)
if(NOT issue_commands_explicit_result EQUAL 12)
  message(FATAL_ERROR "expected invalid explicit issue-commands proof to fail closed with code 12\nstdout:\n${issue_commands_explicit_output}\nstderr:\n${issue_commands_explicit_error}")
endif()
foreach(needle
    "command_queue_discovery.ready=true"
    "issue_commands.ready=false"
    "issue_commands.delivery_checked=false"
    "issue_commands.behavior_checked=false"
    "issue_commands.self_fixture=true"
    "issue_commands.reason=issue-commands proof requires the explicit command queue vector to be readable; refusing to fall back to discovery-only candidates"
    "issue_commands.snapshot.success=true")
  string(FIND "${issue_commands_explicit_output}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "invalid explicit issue-commands output missing '${needle}'\n${issue_commands_explicit_output}")
  endif()
endforeach()

set(issue_commands_explicit_snapshot "${issue_commands_explicit_bridge_dir}/issue_commands.snapshot.tsv")
if(NOT EXISTS "${issue_commands_explicit_snapshot}")
  message(FATAL_ERROR "invalid explicit issue-commands snapshot was not written")
endif()
file(READ "${issue_commands_explicit_snapshot}" issue_commands_explicit_snapshot_content)
foreach(needle
    "delivery_checked\tfalse"
    "behavior_checked\tfalse"
    "self_fixture\ttrue"
    "reason\tissue-commands proof requires the explicit command queue vector to be readable; refusing to fall back to discovery-only candidates")
  string(FIND "${issue_commands_explicit_snapshot_content}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "invalid explicit issue-commands snapshot missing '${needle}'\n${issue_commands_explicit_snapshot_content}")
  endif()
endforeach()

set(issue_commands_explicit_ready_file "${issue_commands_explicit_bridge_dir}/ready")
if(NOT EXISTS "${issue_commands_explicit_ready_file}")
  message(FATAL_ERROR "invalid explicit issue-commands proof did not write partial ready file")
endif()
file(READ "${issue_commands_explicit_ready_file}" issue_commands_explicit_ready)
foreach(forbidden
    "proof.issue_commands=passed"
    "command.receiver=active"
    "command.sink=runtime-command-queue-v1"
    "contract.binding.BW::BWDATA::sgdwBytesInCmdQueue=command-queue"
    "contract.binding.BW::BWDATA::TurnBuffer=command-queue")
  string(FIND "${issue_commands_explicit_ready}" "${forbidden}" forbidden_index)
  if(NOT forbidden_index EQUAL -1)
    message(FATAL_ERROR "invalid explicit issue-commands proof must not claim production command path '${forbidden}'\n${issue_commands_explicit_ready}")
  endif()
endforeach()

file(REMOVE_RECURSE "${issue_commands_explicit_bridge_dir}")

set(overlay_sync_bridge_dir "${STARCRAFT_API_CLI_TEST_DIR}/runtime-adapter-proof-overlay-sync-bridge")
file(REMOVE_RECURSE "${overlay_sync_bridge_dir}")

execute_process(
  COMMAND "${STARCRAFT_RUNTIME_ADAPTER_PROOF}"
    --self
    --product starcraft-remastered
    --version test-build
    --bridge "${overlay_sync_bridge_dir}"
    --prove-draw-overlays
    --prove-multiplayer-sync
  RESULT_VARIABLE overlay_sync_result
  OUTPUT_VARIABLE overlay_sync_output
  ERROR_VARIABLE overlay_sync_error
)
if(NOT overlay_sync_result EQUAL 17)
  message(FATAL_ERROR "expected overlay/sync proof to fail closed with code 17\nstdout:\n${overlay_sync_output}\nstderr:\n${overlay_sync_error}")
endif()
foreach(needle
    "draw_overlays.ready=false"
    "draw_overlays.render_hook_resolved=false"
    "draw_overlays.render_behavior_checked=false"
    "draw_overlays.snapshot.success=true"
    "multiplayer_sync.ready=false"
    "multiplayer_sync.snet_receive_resolved=false"
    "multiplayer_sync.snet_send_turn_resolved=false"
    "multiplayer_sync.sync_behavior_checked=false"
    "multiplayer_sync.snapshot.success=true")
  string(FIND "${overlay_sync_output}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "overlay/sync fail-closed output missing '${needle}'\n${overlay_sync_output}")
  endif()
endforeach()

set(overlay_sync_ready_file "${overlay_sync_bridge_dir}/ready")
if(NOT EXISTS "${overlay_sync_ready_file}")
  message(FATAL_ERROR "overlay/sync proof did not write partial ready file")
endif()
file(READ "${overlay_sync_ready_file}" overlay_sync_ready)
foreach(needle
    "diagnostic.draw_overlays.snapshot=draw_overlays.snapshot.tsv"
    "diagnostic.multiplayer_sync.snapshot=multiplayer_sync.snapshot.tsv")
  string(FIND "${overlay_sync_ready}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "overlay/sync ready file missing '${needle}'\n${overlay_sync_ready}")
  endif()
endforeach()
foreach(forbidden
    "proof.draw_overlays=passed"
    "proof.multiplayer_sync=passed"
    "contract.binding.draw-game-layer-hook=hook-point|proof.draw_overlays=passed"
    "contract.binding.Storm::SNetReceiveMessage=imported-function|proof.multiplayer_sync=passed"
    "contract.binding.Storm::SNetSendTurn=imported-function|proof.multiplayer_sync=passed")
  string(FIND "${overlay_sync_ready}" "${forbidden}" forbidden_index)
  if(NOT forbidden_index EQUAL -1)
    message(FATAL_ERROR "overlay/sync diagnostics must not claim production behavior '${forbidden}'\n${overlay_sync_ready}")
  endif()
endforeach()

file(READ "${overlay_sync_bridge_dir}/draw_overlays.snapshot.tsv" overlay_snapshot)
foreach(needle
    "passed\tfalse"
    "render_hook_resolved\tfalse"
    "adapter-local draw command logging alone is not production overlay rendering")
  string(FIND "${overlay_snapshot}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "overlay diagnostic snapshot missing '${needle}'\n${overlay_snapshot}")
  endif()
endforeach()

file(READ "${overlay_sync_bridge_dir}/multiplayer_sync.snapshot.tsv" sync_snapshot)
foreach(needle
    "passed\tfalse"
    "snet_receive_resolved\tfalse"
    "snet_send_turn_resolved\tfalse"
    "active replay or local command queue delivery is not multiplayer synchronization proof")
  string(FIND "${sync_snapshot}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "multiplayer diagnostic snapshot missing '${needle}'\n${sync_snapshot}")
  endif()
endforeach()

file(REMOVE_RECURSE "${overlay_sync_bridge_dir}")

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
string(REPLACE "\\" "/" normalized_map_output "${map_output}")
foreach(needle
    "read_map_data.ready=true"
    "read_map_data.map_name=(2)Astral Balance"
    "read_map_data.source=latest-replay-artifact"
    "read_map_data.replay_path=${fake_autosave_path}"
    "proof.read_map_data=passed")
  string(FIND "${normalized_map_output}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "map proof output missing '${needle}'\n${normalized_map_output}")
  endif()
endforeach()

set(map_ready_file "${map_bridge_dir}/ready")
if(NOT EXISTS "${map_ready_file}")
  message(FATAL_ERROR "map proof did not write ready file")
endif()
file(READ "${map_ready_file}" map_ready)
string(REPLACE "\\" "/" normalized_map_ready "${map_ready}")
foreach(needle
    "proof.attach=passed"
    "proof.read_map_data.map_name=(2)Astral Balance"
    "proof.read_map_data.map_path=${fake_install_root}/Maps/BroodWar/(2)Astral Balance.scm"
    "proof.read_map_data.source=latest-replay-artifact"
    "proof.read_map_data.replay_path=${fake_autosave_path}"
    "proof.read_map_data.snapshot=map.snapshot.tsv"
    "proof.read_map_data=passed")
  string(FIND "${normalized_map_ready}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "map proof ready file missing '${needle}'\n${normalized_map_ready}")
  endif()
endforeach()

file(READ "${map_bridge_dir}/map.snapshot.tsv" map_snapshot)
string(REPLACE "\\" "/" normalized_map_snapshot "${map_snapshot}")
foreach(needle
    "source"
    "latest-replay-artifact"
    "${fake_autosave_path}")
  string(FIND "${normalized_map_snapshot}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "map snapshot missing '${needle}'\n${normalized_map_snapshot}")
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
string(REPLACE "\\" "/" normalized_units_output "${units_output}")
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
    "read_units.scan.pointer_dense_rejected_records="
    "read_units.scan.sprite_rejected_records="
    "read_units.scan.top_candidate_count="
    "read_units.unit_node_scan.regions="
    "read_units.unit_node_scan.bytes="
    "read_units.unit_node_scan.field_sample_count="
    "read_units.scan.snapshot.success=true"
    "read_units.scan.snapshot.path=${units_bridge_dir}/unit_diagnostics.snapshot.tsv"
    "read_units.scan.best_dump.success=true"
    "read_units.scan.best_dump.path=${units_best_dump}"
    "proof.read_units=passed")
  string(FIND "${normalized_units_output}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "read-units proof output missing '${needle}'\n${normalized_units_output}")
  endif()
endforeach()
if(NOT EXISTS "${units_best_dump}")
  message(FATAL_ERROR "read-units best candidate dump was not written")
endif()
if(NOT EXISTS "${units_bridge_dir}/unit_diagnostics.snapshot.tsv")
  message(FATAL_ERROR "read-units diagnostics snapshot was not written")
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
    "diagnostic.read_units.scan_snapshot=unit_diagnostics.snapshot.tsv"
    "proof.read_units=passed")
  string(FIND "${units_ready}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "read-units ready file missing '${needle}'\n${units_ready}")
  endif()
endforeach()

file(READ "${units_bridge_dir}/unit_diagnostics.snapshot.tsv" unit_diagnostics_snapshot)
foreach(needle
    "field\tvalue"
    "read_units_passed\ttrue"
    "scan_pointer_dense_rejected_records\t"
    "scan_best_active_records\t"
    "scan_top_candidate_count\t"
    "unit_node_scan_regions\t"
    "unit_node_scan_bytes\t"
    "unit_node_field_sample_count\t")
  string(FIND "${unit_diagnostics_snapshot}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "read-units diagnostics snapshot missing '${needle}'\n${unit_diagnostics_snapshot}")
  endif()
endforeach()

string(FIND "${units_ready}" "proof.active_match_state=passed" fixture_active_match_index)
if(NOT fixture_active_match_index EQUAL -1)
  message(FATAL_ERROR "self fixture read-units proof must not claim active-match-state behavior\n${units_ready}")
endif()

file(REMOVE_RECURSE "${units_bridge_dir}")

set(unit_node_bridge_dir "${STARCRAFT_API_CLI_TEST_DIR}/runtime-adapter-proof-unit-node-bridge")
file(REMOVE_RECURSE "${unit_node_bridge_dir}")

execute_process(
  COMMAND "${STARCRAFT_RUNTIME_ADAPTER_PROOF}"
    --self
    --product starcraft-remastered
    --version test-build
    --bridge "${unit_node_bridge_dir}"
    --prove-read-units
    --unit-scan-diagnostics
    --self-unit-node-fixture
  RESULT_VARIABLE unit_node_result
  OUTPUT_VARIABLE unit_node_output
  ERROR_VARIABLE unit_node_error
)
if(NOT unit_node_result EQUAL 0)
  message(FATAL_ERROR "expected SC:R unit-node read-units proof to pass with self fixture\nstdout:\n${unit_node_output}\nstderr:\n${unit_node_error}")
endif()
foreach(needle
    "read_units.unit_node_candidate_address.count=1"
    "read_units.unit_array=true"
    "read_units.layout=scr-unit-node-object-graph"
    "read_units.derived_snapshot=true"
    "read_units.hit_points_resolved=true"
    "read_units.snapshot.success=true"
    "read_units.scan.snapshot.success=true"
    "proof.read_units=passed")
  string(FIND "${unit_node_output}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "SC:R unit-node proof output missing '${needle}'\n${unit_node_output}")
  endif()
endforeach()

set(unit_node_ready_file "${unit_node_bridge_dir}/ready")
if(NOT EXISTS "${unit_node_ready_file}")
  message(FATAL_ERROR "SC:R unit-node proof did not write ready file")
endif()
file(READ "${unit_node_ready_file}" unit_node_ready)
foreach(needle
    "proof.attach=passed"
    "proof.read_units.layout=scr-unit-node-object-graph"
    "proof.read_units.derived_snapshot=true"
    "proof.read_units.snapshot=units.snapshot.tsv"
    "proof.read_units.id_source=stable-node-handle"
    "proof.read_units.position_source=unit-node+36|4"
    "proof.read_units.hit_points_source=secondary+0x1a compact-hp-byte -> bwapi-hp-raw"
    "proof.read_units.order_source=unit-node+48|2"
    "proof.read_units.player_source=unit-node+0x50 secondary+0x14|1"
    "contract.field.BW::CUnit.hitPoints=12|4|proof.read_units=passed:scr-compact-hp-byte"
    "diagnostic.read_units.scan_snapshot=unit_diagnostics.snapshot.tsv"
    "proof.read_units=passed")
  string(FIND "${unit_node_ready}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "SC:R unit-node ready file missing '${needle}'\n${unit_node_ready}")
  endif()
endforeach()

string(FIND "${unit_node_ready}" "proof.active_match_state=passed" unit_node_active_match_index)
if(NOT unit_node_active_match_index EQUAL -1)
  message(FATAL_ERROR "self unit-node fixture read-units proof must not claim active-match-state behavior\n${unit_node_ready}")
endif()

foreach(path
    "${unit_node_bridge_dir}/units.snapshot.tsv"
    "${unit_node_bridge_dir}/unit_diagnostics.snapshot.tsv")
  if(NOT EXISTS "${path}")
    message(FATAL_ERROR "SC:R unit-node proof expected snapshot is missing: ${path}")
  endif()
endforeach()
set(unit_node_snapshot_failure_bridge_dir "${STARCRAFT_API_CLI_TEST_DIR}/runtime-adapter-proof-unit-node-snapshot-failure-bridge")
file(REMOVE_RECURSE "${unit_node_snapshot_failure_bridge_dir}")
file(MAKE_DIRECTORY "${unit_node_snapshot_failure_bridge_dir}/units.snapshot.tsv")

execute_process(
  COMMAND "${STARCRAFT_RUNTIME_ADAPTER_PROOF}"
    --self
    --product starcraft-remastered
    --version test-build
    --bridge "${unit_node_snapshot_failure_bridge_dir}"
    --prove-read-units
    --self-unit-node-fixture
  RESULT_VARIABLE unit_node_snapshot_failure_result
  OUTPUT_VARIABLE unit_node_snapshot_failure_output
  ERROR_VARIABLE unit_node_snapshot_failure_error
)
if(unit_node_snapshot_failure_result EQUAL 0)
  message(FATAL_ERROR "expected derived unit-node read-units proof to fail when units snapshot cannot be written\nstdout:\n${unit_node_snapshot_failure_output}\nstderr:\n${unit_node_snapshot_failure_error}")
endif()
foreach(needle
    "read_units.derived_snapshot=true"
    "read_units.snapshot.success=false")
  string(FIND "${unit_node_snapshot_failure_output}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "unit-node snapshot failure output missing '${needle}'\n${unit_node_snapshot_failure_output}")
  endif()
endforeach()
set(unit_node_snapshot_failure_ready_file "${unit_node_snapshot_failure_bridge_dir}/ready")
if(NOT EXISTS "${unit_node_snapshot_failure_ready_file}")
  message(FATAL_ERROR "failed unit-node snapshot proof must still write partial ready file")
endif()
file(READ "${unit_node_snapshot_failure_ready_file}" unit_node_snapshot_failure_ready)
foreach(forbidden
    "proof.read_units=passed"
    "proof.active_match_state=passed")
  string(FIND "${unit_node_snapshot_failure_ready}" "${forbidden}" forbidden_index)
  if(NOT forbidden_index EQUAL -1)
    message(FATAL_ERROR "failed unit-node snapshot proof must not claim '${forbidden}'\n${unit_node_snapshot_failure_ready}")
  endif()
endforeach()

file(READ "${unit_node_bridge_dir}/unit_diagnostics.snapshot.tsv" unit_node_diagnostics_snapshot)
foreach(needle
    "read_units_passed\ttrue"
    "unit_node_passed\ttrue"
    "scan_pointer_dense_rejected_records\t"
    "scan_top_candidate_count\t"
    "unit_node_scan_regions\t"
    "unit_node_scan_bytes\t"
    "unit_node_field_sample_count\t"
    "unit_node_active_records\t")
  string(FIND "${unit_node_diagnostics_snapshot}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "SC:R unit-node diagnostics snapshot missing '${needle}'\n${unit_node_diagnostics_snapshot}")
  endif()
endforeach()

file(REMOVE_RECURSE "${unit_node_bridge_dir}")

set(compact_unit_node_bridge_dir "${STARCRAFT_API_CLI_TEST_DIR}/runtime-adapter-proof-compact-unit-node-bridge")
file(REMOVE_RECURSE "${compact_unit_node_bridge_dir}")

execute_process(
  COMMAND "${STARCRAFT_RUNTIME_ADAPTER_PROOF}"
    --self
    --product starcraft-remastered
    --version test-build
    --bridge "${compact_unit_node_bridge_dir}"
    --prove-read-units
    --unit-scan-diagnostics
    --self-compact-unit-node-fixture
  RESULT_VARIABLE compact_unit_node_result
  OUTPUT_VARIABLE compact_unit_node_output
  ERROR_VARIABLE compact_unit_node_error
)
if(NOT compact_unit_node_result EQUAL 0)
  message(FATAL_ERROR "expected compact SC:R unit-node read-units proof to pass with self fixture\nstdout:\n${compact_unit_node_output}\nstderr:\n${compact_unit_node_error}")
endif()
foreach(needle
    "read_units.unit_node_candidate_address.count=1"
    "read_units.unit_array=true"
    "read_units.record_size=40"
    "read_units.layout=scr-compact-unit-node-object-graph"
    "read_units.derived_snapshot=true"
    "read_units.hit_points_resolved=true"
    "read_units.scan.snapshot.success=true"
    "proof.read_units=passed")
  string(FIND "${compact_unit_node_output}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "compact SC:R unit-node proof output missing '${needle}'\n${compact_unit_node_output}")
  endif()
endforeach()

set(compact_unit_node_ready_file "${compact_unit_node_bridge_dir}/ready")
if(NOT EXISTS "${compact_unit_node_ready_file}")
  message(FATAL_ERROR "compact SC:R unit-node proof did not write ready file")
endif()
file(READ "${compact_unit_node_ready_file}" compact_unit_node_ready)
foreach(needle
    "proof.attach=passed"
    "proof.read_units.record_size=40"
    "proof.read_units.layout=scr-compact-unit-node-object-graph"
    "proof.read_units.derived_snapshot=true"
    "proof.read_units.id_source=stable-node-handle|compact-node sprite metadata"
    "proof.read_units.position_source=unit-node+0x10|8 compact-xy"
    "proof.read_units.hit_points_source=sprite+0x80 hp-raw"
    "diagnostic.read_units.scan_snapshot=unit_diagnostics.snapshot.tsv"
    "proof.read_units=passed")
  string(FIND "${compact_unit_node_ready}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "compact SC:R unit-node ready file missing '${needle}'\n${compact_unit_node_ready}")
  endif()
endforeach()

file(READ "${compact_unit_node_bridge_dir}/unit_diagnostics.snapshot.tsv" compact_unit_node_diagnostics_snapshot)
foreach(needle
    "read_units_passed\ttrue"
    "unit_node_passed\ttrue"
    "unit_node_record_size\t40"
    "unit_node_active_records\t")
  string(FIND "${compact_unit_node_diagnostics_snapshot}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "compact SC:R unit-node diagnostics snapshot missing '${needle}'\n${compact_unit_node_diagnostics_snapshot}")
  endif()
endforeach()

file(REMOVE_RECURSE "${compact_unit_node_bridge_dir}")

set(player_projection_bridge_dir "${STARCRAFT_API_CLI_TEST_DIR}/runtime-adapter-proof-player-projection-bridge")
file(REMOVE_RECURSE "${player_projection_bridge_dir}")

execute_process(
  COMMAND "${STARCRAFT_RUNTIME_ADAPTER_PROOF}"
    --self
    --product starcraft-remastered
    --version test-build
    --bridge "${player_projection_bridge_dir}"
    --prove-read-units
    --prove-read-player-data
    --self-unit-node-fixture
  RESULT_VARIABLE player_projection_result
  OUTPUT_VARIABLE player_projection_output
  ERROR_VARIABLE player_projection_error
)
if(NOT player_projection_result EQUAL 7)
  message(FATAL_ERROR "expected self player projection to fail only active-match proof\nstdout:\n${player_projection_output}\nstderr:\n${player_projection_error}")
endif()
foreach(needle
    "read_units.unit_array=true"
    "read_player_data.ready=true"
    "read_player_data.player_info_projection=true"
    "read_player_data.alliance_projection=true"
    "read_player_data.snapshot.success=true")
  string(FIND "${player_projection_output}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "self player projection output missing '${needle}'\n${player_projection_output}")
  endif()
endforeach()

set(player_projection_ready_file "${player_projection_bridge_dir}/ready")
if(NOT EXISTS "${player_projection_ready_file}")
  message(FATAL_ERROR "self player projection proof did not write ready file")
endif()
file(READ "${player_projection_ready_file}" player_projection_ready)
foreach(needle
    "proof.attach=passed")
  string(FIND "${player_projection_ready}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "self player projection ready file missing '${needle}'\n${player_projection_ready}")
  endif()
endforeach()
foreach(forbidden
    "proof.active_match_state=passed"
    "proof.read_units=passed"
    "proof.read_player_data=passed"
    "contract.structure.BW::PlayerInfo"
    "contract.field.BW::BWGame.alliance")
  string(FIND "${player_projection_ready}" "${forbidden}" forbidden_index)
  if(NOT forbidden_index EQUAL -1)
    message(FATAL_ERROR "self player projection must not claim '${forbidden}' without active-match proof\n${player_projection_ready}")
  endif()
endforeach()
file(READ "${player_projection_bridge_dir}/players.snapshot.tsv" player_projection_snapshot)
foreach(needle
    "player\tstorm_id\trace\trace_inferred\tobserved_unit_count\tminerals\tgas\tsupply_used\tsupply_total\talliance_mask"
    "0x")
  string(FIND "${player_projection_snapshot}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "self player projection snapshot missing '${needle}'\n${player_projection_snapshot}")
  endif()
endforeach()
file(REMOVE_RECURSE "${player_projection_bridge_dir}")

set(bullet_bridge_dir "${STARCRAFT_API_CLI_TEST_DIR}/runtime-adapter-proof-bullet-bridge")
file(REMOVE_RECURSE "${bullet_bridge_dir}")

execute_process(
  COMMAND "${STARCRAFT_RUNTIME_ADAPTER_PROOF}"
    --self
    --product starcraft-remastered
    --version test-build
    --bridge "${bullet_bridge_dir}"
    --prove-read-bullet-data
    --self-bullet-fixture
  RESULT_VARIABLE bullet_result
  OUTPUT_VARIABLE bullet_output
  ERROR_VARIABLE bullet_error
)
if(NOT bullet_result EQUAL 0)
  message(FATAL_ERROR "expected read-bullet-data proof to pass with self fixture\nstdout:\n${bullet_output}\nstderr:\n${bullet_error}")
endif()
foreach(needle
    "read_bullet_data.ready=true"
    "read_bullet_data.candidate_address.count=1"
    "read_bullet_data.record_size=136"
    "read_bullet_data.layout=scr-x64-packed-cbullet"
    "read_bullet_data.active_records="
    "read_bullet_data.snapshot.success=true"
    "proof.read_bullet_data=passed")
  string(FIND "${bullet_output}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "read-bullet-data proof output missing '${needle}'\n${bullet_output}")
  endif()
endforeach()

set(bullet_ready_file "${bullet_bridge_dir}/ready")
if(NOT EXISTS "${bullet_ready_file}")
  message(FATAL_ERROR "read-bullet-data proof did not write ready file")
endif()
file(READ "${bullet_ready_file}" bullet_ready)
foreach(needle
    "proof.attach=passed"
    "proof.read_bullet_data.layout=scr-x64-packed-cbullet"
    "proof.read_bullet_data.snapshot=bullets.snapshot.tsv"
    "contract.binding.BW::BWDATA::BulletNodeTable=data-address|proof.read_bullet_data=passed"
    "contract.structure.BW::CBullet=136|proof.read_bullet_data=passed"
    "contract.field.BW::CBullet.position=64|4|proof.read_bullet_data=passed"
    "contract.field.BW::CBullet.velocity=88|8|proof.read_bullet_data=passed"
    "contract.field.BW::CBullet.sourceUnit=120|8|proof.read_bullet_data=passed"
    "contract.field.BW::CBullet.target=112|8|proof.read_bullet_data=passed"
    "proof.read_bullet_data=passed")
  string(FIND "${bullet_ready}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "read-bullet-data ready file missing '${needle}'\n${bullet_ready}")
  endif()
endforeach()

if(NOT EXISTS "${bullet_bridge_dir}/bullets.snapshot.tsv")
  message(FATAL_ERROR "read-bullet-data snapshot was not written")
endif()
file(READ "${bullet_bridge_dir}/bullets.snapshot.tsv" bullet_snapshot)
foreach(needle
    "address"
    "sprite"
    "source_unit"
    "target_unit")
  string(FIND "${bullet_snapshot}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "read-bullet-data snapshot missing '${needle}'\n${bullet_snapshot}")
  endif()
endforeach()
string(FIND "${bullet_ready}" "proof.active_match_state=passed" bullet_active_match_index)
if(NOT bullet_active_match_index EQUAL -1)
  message(FATAL_ERROR "self bullet proof must not claim active-match-state behavior\n${bullet_ready}")
endif()

file(REMOVE_RECURSE "${bullet_bridge_dir}")

set(region_bridge_dir "${STARCRAFT_API_CLI_TEST_DIR}/runtime-adapter-proof-region-bridge")
file(REMOVE_RECURSE "${region_bridge_dir}")

execute_process(
  COMMAND "${STARCRAFT_RUNTIME_ADAPTER_PROOF}"
    --self
    --product starcraft-remastered
    --version test-build
    --bridge "${region_bridge_dir}"
    --prove-read-region-data
    --self-region-fixture
  RESULT_VARIABLE region_result
  OUTPUT_VARIABLE region_output
  ERROR_VARIABLE region_error
)
if(NOT region_result EQUAL 0)
  message(FATAL_ERROR "expected read-region-data proof to pass with self fixture\nstdout:\n${region_output}\nstderr:\n${region_error}")
endif()
foreach(needle
    "read_region_data.ready=true"
    "read_region_data.source=self-region-fixture"
    "read_region_data.region_count=2"
    "read_region_data.observed_units=8"
    "read_region_data.snapshot.success=true"
    "proof.read_region_data=passed")
  string(FIND "${region_output}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "read-region-data proof output missing '${needle}'\n${region_output}")
  endif()
endforeach()

set(region_ready_file "${region_bridge_dir}/ready")
if(NOT EXISTS "${region_ready_file}")
  message(FATAL_ERROR "read-region-data proof did not write ready file")
endif()
file(READ "${region_ready_file}" region_ready)
foreach(needle
    "proof.attach=passed"
    "proof.read_region_data.source=self-region-fixture"
    "proof.read_region_data.region_count=2"
    "proof.read_region_data.snapshot=regions.snapshot.tsv"
    "proof.read_region_data=passed")
  string(FIND "${region_ready}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "read-region-data ready file missing '${needle}'\n${region_ready}")
  endif()
endforeach()
if(NOT EXISTS "${region_bridge_dir}/regions.snapshot.tsv")
  message(FATAL_ERROR "read-region-data snapshot was not written")
endif()
file(READ "${region_bridge_dir}/regions.snapshot.tsv" region_snapshot)
foreach(needle
    "center_x"
    "observed_units"
    "accessible")
  string(FIND "${region_snapshot}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "read-region-data snapshot missing '${needle}'\n${region_snapshot}")
  endif()
endforeach()
string(FIND "${region_ready}" "proof.active_match_state=passed" region_active_match_index)
if(NOT region_active_match_index EQUAL -1)
  message(FATAL_ERROR "self region proof must not claim active-match-state behavior\n${region_ready}")
endif()

file(REMOVE_RECURSE "${region_bridge_dir}")

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
    "read_game_state.scan.closest_counter.available="
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
string(FIND "${combined_ready}" "proof.read_game_state=passed" combined_read_state_index)
if(NOT combined_read_state_index EQUAL -1)
  foreach(needle
      "resident.adapter=active"
      "resident.proof.read_game_state.source=resident"
      "resident.proof.read_game_state.frame_samples="
      "resident.proof.read_game_state.tick_samples=")
    string(FIND "${combined_ready}" "${needle}" resident_read_state_index)
    if(resident_read_state_index EQUAL -1)
      message(FATAL_ERROR "combined read-game-state proof missing resident payload '${needle}'\n${combined_ready}")
    endif()
  endforeach()
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
set(policy_snapshot_executable "${policy_executable}")
string(REPLACE "\\" "/" policy_snapshot_executable "${policy_snapshot_executable}")
set(policy_process_snapshot "${STARCRAFT_API_CLI_TEST_DIR}/runtime-adapter-proof-policy-processes.snapshot")
file(REMOVE_RECURSE "${policy_bridge_dir}" "${policy_root}")
file(WRITE "${policy_process_snapshot}" "123 1 ${policy_snapshot_executable} -launch -uid s1\n")

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

set(policy_multi_main_bridge_dir "${STARCRAFT_API_CLI_TEST_DIR}/runtime-adapter-proof-policy-multi-main-bridge")
file(REMOVE_RECURSE "${policy_multi_main_bridge_dir}")
file(WRITE "${policy_process_snapshot}"
  "123 1 ${policy_executable} -launch -uid s1\n"
  "5501 1 /Applications/Battle.net.app/Contents/MacOS/Battle.net\n"
  "5502 1 /Applications/Battle.net.app/Contents/MacOS/Battle.net\n")

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env
    "STARCRAFT_API_PROCESS_SNAPSHOT=${policy_process_snapshot}"
    "${STARCRAFT_RUNTIME_ADAPTER_PROOF}"
      --self
      --product starcraft-remastered
      --version test-build
      --executable "${policy_executable}"
      --bridge "${policy_multi_main_bridge_dir}"
      --prove-battle-net-policy
  RESULT_VARIABLE policy_multi_main_result
  OUTPUT_VARIABLE policy_multi_main_output
  ERROR_VARIABLE policy_multi_main_error
)
if(NOT policy_multi_main_result EQUAL 0)
  message(FATAL_ERROR "expected Battle.net policy proof to pass with one game and multiple Battle.net main processes\nstdout:\n${policy_multi_main_output}\nstderr:\n${policy_multi_main_error}")
endif()
foreach(needle
    "battle_net_policy.ready_for_attach=true"
    "battle_net_policy.game_process_count=1"
    "battle_net_policy.battle_net_main_count=2"
    "battle_net_policy.blocker_count=0"
    "proof.battle_net_policy=passed")
  string(FIND "${policy_multi_main_output}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "Battle.net policy multi-main output missing '${needle}'\n${policy_multi_main_output}")
  endif()
endforeach()
file(REMOVE_RECURSE "${policy_multi_main_bridge_dir}")

set(ai_module_bridge_dir "${STARCRAFT_API_CLI_TEST_DIR}/runtime-adapter-proof-ai-module-bridge")
file(REMOVE_RECURSE "${ai_module_bridge_dir}")

execute_process(
  COMMAND "${STARCRAFT_RUNTIME_ADAPTER_PROOF}"
    --self
    --product starcraft-remastered
    --version test-build
    --bridge "${ai_module_bridge_dir}"
    --prove-load-ai-modules
  RESULT_VARIABLE ai_module_result
  OUTPUT_VARIABLE ai_module_output
  ERROR_VARIABLE ai_module_error
)
if(NOT ai_module_result EQUAL 0)
  message(FATAL_ERROR "expected self AI module loader proof to pass\nstdout:\n${ai_module_output}\nstderr:\n${ai_module_error}")
endif()
foreach(needle
    "load_ai_modules.ready=true"
    "load_ai_modules.loader="
    "load_ai_modules.module_extension="
    "load_ai_modules.self_process_smoke=true"
    "load_ai_modules.snapshot.success=true"
    "proof.load_ai_modules=passed")
  string(FIND "${ai_module_output}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "AI module proof output missing '${needle}'\n${ai_module_output}")
  endif()
endforeach()

set(ai_module_ready_file "${ai_module_bridge_dir}/ready")
if(NOT EXISTS "${ai_module_ready_file}")
  message(FATAL_ERROR "AI module proof did not write ready file")
endif()
file(READ "${ai_module_ready_file}" ai_module_ready)
foreach(needle
    "proof.attach=passed"
    "proof.load_ai_modules.snapshot=ai_module_load.snapshot.tsv"
    "contract.binding.ai-module-loader=transport|proof.load_ai_modules=passed"
    "proof.load_ai_modules=passed")
  string(FIND "${ai_module_ready}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "AI module ready file missing '${needle}'\n${ai_module_ready}")
  endif()
endforeach()
foreach(forbidden
    "proof.active_match_state=passed"
    "proof.issue_commands=passed"
    "proof.draw_overlays=passed")
  string(FIND "${ai_module_ready}" "${forbidden}" forbidden_index)
  if(NOT forbidden_index EQUAL -1)
    message(FATAL_ERROR "AI module proof must not claim unrelated in-game behavior '${forbidden}'\n${ai_module_ready}")
  endif()
endforeach()

file(READ "${ai_module_bridge_dir}/ai_module_load.snapshot.tsv" ai_module_snapshot)
foreach(needle
    "passed\ttrue"
    "self_process_smoke\ttrue")
  string(FIND "${ai_module_snapshot}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "AI module snapshot missing '${needle}'\n${ai_module_snapshot}")
  endif()
endforeach()

file(REMOVE_RECURSE "${ai_module_bridge_dir}")

if(NOT DEFINED STARCRAFT_API_AI_MODULE_SMOKE)
  message(FATAL_ERROR "STARCRAFT_API_AI_MODULE_SMOKE is required")
endif()

set(ai_module_file_bridge_dir "${STARCRAFT_API_CLI_TEST_DIR}/runtime-adapter-proof-ai-module-file-bridge")
file(REMOVE_RECURSE "${ai_module_file_bridge_dir}")

execute_process(
  COMMAND "${STARCRAFT_RUNTIME_ADAPTER_PROOF}"
    --self
    --product starcraft-remastered
    --version test-build
    --bridge "${ai_module_file_bridge_dir}"
    --prove-load-ai-modules
    --ai-module-path "${STARCRAFT_API_AI_MODULE_SMOKE}"
  RESULT_VARIABLE ai_module_file_result
  OUTPUT_VARIABLE ai_module_file_output
  ERROR_VARIABLE ai_module_file_error
)
if(NOT ai_module_file_result EQUAL 0)
  message(FATAL_ERROR "expected AI module file proof to pass\nstdout:\n${ai_module_file_output}\nstderr:\n${ai_module_file_error}")
endif()
foreach(needle
    "load_ai_modules.ready=true"
    "load_ai_modules.loader="
    "load_ai_modules.module_path=${STARCRAFT_API_AI_MODULE_SMOKE}"
    "load_ai_modules.self_process_smoke=false"
    "proof.load_ai_modules=passed")
  string(FIND "${ai_module_file_output}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "AI module file proof output missing '${needle}'\n${ai_module_file_output}")
  endif()
endforeach()

set(ai_module_file_ready_file "${ai_module_file_bridge_dir}/ready")
if(NOT EXISTS "${ai_module_file_ready_file}")
  message(FATAL_ERROR "AI module file proof did not write ready file")
endif()
file(READ "${ai_module_file_ready_file}" ai_module_file_ready)
foreach(needle
    "proof.load_ai_modules.module_path=${STARCRAFT_API_AI_MODULE_SMOKE}"
    "proof.load_ai_modules.self_process_smoke=false"
    "contract.binding.ai-module-loader=transport|proof.load_ai_modules=passed"
    "proof.load_ai_modules=passed")
  string(FIND "${ai_module_file_ready}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "AI module file ready missing '${needle}'\n${ai_module_file_ready}")
  endif()
endforeach()

file(READ "${ai_module_file_bridge_dir}/ai_module_load.snapshot.tsv" ai_module_file_snapshot)
foreach(needle
    "passed\ttrue"
    "self_process_smoke\tfalse"
    "module_path\t${STARCRAFT_API_AI_MODULE_SMOKE}")
  string(FIND "${ai_module_file_snapshot}" "${needle}" needle_index)
  if(needle_index EQUAL -1)
    message(FATAL_ERROR "AI module file snapshot missing '${needle}'\n${ai_module_file_snapshot}")
  endif()
endforeach()

file(REMOVE_RECURSE "${ai_module_file_bridge_dir}")
