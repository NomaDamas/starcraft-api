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

add_executable(public_api_smoke_test
  public_api_smoke_test.cpp
)

target_link_libraries(public_api_smoke_test
  PRIVATE
    StarCraftAPI::public
)

add_test(NAME public_api_smoke_test COMMAND public_api_smoke_test)
