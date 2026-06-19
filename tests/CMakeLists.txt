add_executable(runtime_backend_test
  runtime_backend_test.cpp
)

target_link_libraries(runtime_backend_test
  PRIVATE
    StarCraftAPI::runtime
)

add_test(NAME runtime_backend_test COMMAND runtime_backend_test)

add_executable(runtime_contract_test
  runtime_contract_test.cpp
)

target_link_libraries(runtime_contract_test
  PRIVATE
    StarCraftAPI::runtime
)

add_test(NAME runtime_contract_test COMMAND runtime_contract_test)

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

add_executable(runtime_process_memory_test
  runtime_process_memory_test.cpp
)

target_link_libraries(runtime_process_memory_test
  PRIVATE
    StarCraftAPI::runtime
)

add_test(NAME runtime_process_memory_test COMMAND runtime_process_memory_test)

if(TARGET starcraft-runtime-probe)
  add_test(NAME runtime_probe_cli_test COMMAND starcraft-runtime-probe)
  add_test(NAME runtime_probe_require_production_fails COMMAND starcraft-runtime-probe --require-production)
  set_tests_properties(runtime_probe_require_production_fails PROPERTIES WILL_FAIL TRUE)
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
