add_executable(runtime_backend_test
  runtime_backend_test.cpp
)

target_link_libraries(runtime_backend_test
  PRIVATE
    StarCraftAPI::runtime
)

add_test(NAME runtime_backend_test COMMAND runtime_backend_test)

add_executable(runtime_command_surface_test
  runtime_command_surface_test.cpp
)

target_link_libraries(runtime_command_surface_test
  PRIVATE
    StarCraftAPI::runtime
)

add_test(NAME runtime_command_surface_test COMMAND runtime_command_surface_test)

add_executable(runtime_command_queue_test
  runtime_command_queue_test.cpp
)

target_link_libraries(runtime_command_queue_test
  PRIVATE
    StarCraftAPI::runtime
)

add_test(NAME runtime_command_queue_test COMMAND runtime_command_queue_test)

add_executable(runtime_command_encoder_test
  runtime_command_encoder_test.cpp
)

target_link_libraries(runtime_command_encoder_test
  PRIVATE
    StarCraftAPI::runtime
)

add_test(NAME runtime_command_encoder_test COMMAND runtime_command_encoder_test)

add_executable(runtime_contract_test
  runtime_contract_test.cpp
)

target_link_libraries(runtime_contract_test
  PRIVATE
    StarCraftAPI::runtime
)

add_test(NAME runtime_contract_test COMMAND runtime_contract_test)

add_executable(runtime_implementation_gap_test
  runtime_implementation_gap_test.cpp
)

target_link_libraries(runtime_implementation_gap_test
  PRIVATE
    StarCraftAPI::runtime
)

add_test(NAME runtime_implementation_gap_test COMMAND runtime_implementation_gap_test)

add_executable(runtime_manifest_test
  runtime_manifest_test.cpp
)

target_compile_definitions(runtime_manifest_test
  PRIVATE
    STARCRAFT_API_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)

target_link_libraries(runtime_manifest_test
  PRIVATE
    StarCraftAPI::runtime
)

add_test(NAME runtime_manifest_test COMMAND runtime_manifest_test)

add_executable(runtime_executor_test
  runtime_executor_test.cpp
)

target_compile_definitions(runtime_executor_test
  PRIVATE
    STARCRAFT_API_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)

target_link_libraries(runtime_executor_test
  PRIVATE
    StarCraftAPI::runtime
)

add_test(NAME runtime_executor_test COMMAND runtime_executor_test)

add_executable(runtime_readiness_test
  runtime_readiness_test.cpp
)

target_link_libraries(runtime_readiness_test
  PRIVATE
    StarCraftAPI::runtime
)

add_test(NAME runtime_readiness_test COMMAND runtime_readiness_test)

add_executable(runtime_production_bridge_test
  runtime_production_bridge_test.cpp
)

target_compile_definitions(runtime_production_bridge_test
  PRIVATE
    STARCRAFT_API_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)

target_link_libraries(runtime_production_bridge_test
  PRIVATE
    StarCraftAPI::runtime
)

add_test(NAME runtime_production_bridge_test COMMAND runtime_production_bridge_test)

add_executable(runtime_process_memory_test
  runtime_process_memory_test.cpp
)

target_link_libraries(runtime_process_memory_test
  PRIVATE
    StarCraftAPI::runtime
)

add_test(NAME runtime_process_memory_test COMMAND runtime_process_memory_test)

add_executable(runtime_process_test
  runtime_process_test.cpp
)

target_link_libraries(runtime_process_test
  PRIVATE
    StarCraftAPI::runtime
)

add_test(NAME runtime_process_test COMMAND runtime_process_test)

add_executable(runtime_installation_test
  runtime_installation_test.cpp
)

target_link_libraries(runtime_installation_test
  PRIVATE
    StarCraftAPI::runtime
)

add_test(NAME runtime_installation_test COMMAND runtime_installation_test)

if(TARGET starcraft-runtime-probe)
  add_test(NAME runtime_probe_cli_test COMMAND starcraft-runtime-probe)
  add_test(NAME runtime_probe_require_production_fails COMMAND starcraft-runtime-probe --require-production)
  set_tests_properties(runtime_probe_require_production_fails PROPERTIES WILL_FAIL TRUE)
endif()

if(TARGET starcraft-runtime-gap-report)
  add_test(NAME runtime_gap_report_cli_test COMMAND starcraft-runtime-gap-report)
  add_test(NAME runtime_gap_report_require_production_fails COMMAND starcraft-runtime-gap-report --require-production)
  set_tests_properties(runtime_gap_report_require_production_fails PROPERTIES WILL_FAIL TRUE)
  add_test(
    NAME runtime_gap_report_evidence_cli_test
    COMMAND
      "${CMAKE_COMMAND}"
      -DSTARCRAFT_RUNTIME_GAP_REPORT=$<TARGET_FILE:starcraft-runtime-gap-report>
      -DSTARCRAFT_API_TEST_FIXTURE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/fixtures
      -DSTARCRAFT_API_CLI_TEST_DIR=${CMAKE_CURRENT_BINARY_DIR}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/runtime_gap_report_cli_test.cmake
  )
endif()

if(TARGET starcraft-runtime-submit-command)
  add_test(NAME runtime_submit_command_help COMMAND starcraft-runtime-submit-command --help)
  add_test(
    NAME runtime_submit_command_cli_manifest_gate
    COMMAND
      "${CMAKE_COMMAND}"
      -DSTARCRAFT_RUNTIME_SUBMIT_COMMAND=$<TARGET_FILE:starcraft-runtime-submit-command>
      -DSTARCRAFT_API_TEST_FIXTURE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/fixtures
      -DSTARCRAFT_API_CLI_TEST_DIR=${CMAKE_CURRENT_BINARY_DIR}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/runtime_submit_command_cli_test.cmake
  )
endif()

if(TARGET starcraft-runtime-launch)
  add_test(NAME runtime_launch_help COMMAND starcraft-runtime-launch --help)
endif()

if(TARGET starcraft-runtime-memory-probe)
  add_test(NAME runtime_memory_probe_help COMMAND starcraft-runtime-memory-probe --help)
  add_test(NAME runtime_memory_probe_self_open COMMAND starcraft-runtime-memory-probe --self --require-open --require-access)
  add_test(NAME runtime_memory_probe_self_product_version COMMAND starcraft-runtime-memory-probe --self --product starcraft-remastered --version test-build --require-open --require-access)
  add_test(NAME runtime_memory_probe_self_read_first_readable COMMAND starcraft-runtime-memory-probe --self --read-first-readable --require-open --require-access --require-read)
  add_test(
    NAME runtime_memory_probe_find_cli_test
    COMMAND
      "${CMAKE_COMMAND}"
      -DSTARCRAFT_RUNTIME_MEMORY_PROBE=$<TARGET_FILE:starcraft-runtime-memory-probe>
      -P ${CMAKE_CURRENT_SOURCE_DIR}/runtime_memory_probe_cli_test.cmake
  )
endif()

if(TARGET starcraft-runtime-adapter-proof)
  add_library(starcraft_api_ai_module_smoke SHARED
    fixtures/ai_module_smoke.cpp
  )
  set_target_properties(starcraft_api_ai_module_smoke PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
  )

  add_test(NAME runtime_adapter_proof_help COMMAND starcraft-runtime-adapter-proof --help)
  add_test(
    NAME runtime_adapter_proof_cli_test
    COMMAND
      "${CMAKE_COMMAND}"
      -DSTARCRAFT_RUNTIME_ADAPTER_PROOF=$<TARGET_FILE:starcraft-runtime-adapter-proof>
      -DSTARCRAFT_API_AI_MODULE_SMOKE=$<TARGET_FILE:starcraft_api_ai_module_smoke>
      -DSTARCRAFT_API_CLI_TEST_DIR=${CMAKE_CURRENT_BINARY_DIR}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/runtime_adapter_proof_cli_test.cmake
  )
endif()

if(TARGET starcraft-runtime-binary-anchors)
  add_test(NAME runtime_binary_anchors_help COMMAND starcraft-runtime-binary-anchors --help)
  add_test(
    NAME runtime_binary_anchors_self_fixture
    COMMAND
      starcraft-runtime-binary-anchors
      --executable $<TARGET_FILE:starcraft-runtime-binary-anchors>
      --anchor starcraft-api-binary-anchor-self-fixture
      --require-anchor
  )
endif()

if(TARGET starcraft-runtime-input)
  add_test(NAME runtime_input_help COMMAND starcraft-runtime-input --help)
  add_test(
    NAME runtime_input_dry_run
    COMMAND starcraft-runtime-input --process-id 1 --keys "s enter c" --post-timeout-ms 1 --dry-run
  )
endif()

if(TARGET bwapi-api-surface-audit)
  add_test(NAME bwapi_api_surface_audit COMMAND bwapi-api-surface-audit)
endif()

add_executable(public_api_smoke_test
  public_api_smoke_test.cpp
)

target_link_libraries(public_api_smoke_test
  PRIVATE
    StarCraftAPI::public
)

add_test(NAME public_api_smoke_test COMMAND public_api_smoke_test)
