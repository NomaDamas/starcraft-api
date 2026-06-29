if(NOT DEFINED STARCRAFT_RUNTIME_LEGACY1161_SETUP)
  message(FATAL_ERROR "STARCRAFT_RUNTIME_LEGACY1161_SETUP is required")
endif()
if(NOT DEFINED STARCRAFT_API_CLI_TEST_DIR)
  message(FATAL_ERROR "STARCRAFT_API_CLI_TEST_DIR is required")
endif()
if(NOT DEFINED Python3_EXECUTABLE)
  message(FATAL_ERROR "Python3_EXECUTABLE is required")
endif()

set(test_root "${STARCRAFT_API_CLI_TEST_DIR}/runtime-legacy1161-setup-cli")
file(REMOVE_RECURSE "${test_root}")
file(MAKE_DIRECTORY "${test_root}/bw1161")

set(create_pe "${test_root}/create_pe32.py")
file(WRITE "${create_pe}" [=[
from pathlib import Path
path = Path(__import__("sys").argv[1])
path.parent.mkdir(parents=True, exist_ok=True)
data = bytearray(512)
data[0:2] = b"MZ"
pe = 0x80
data[0x3c:0x40] = pe.to_bytes(4, "little")
data[pe:pe + 4] = b"PE\0\0"
data[pe + 4:pe + 6] = (0x014c).to_bytes(2, "little")
data[pe + 24:pe + 26] = (0x010b).to_bytes(2, "little")
path.write_bytes(data)
]=])

execute_process(
  COMMAND "${Python3_EXECUTABLE}" "${create_pe}" "${test_root}/bw1161/StarCraft.exe"
  RESULT_VARIABLE create_pe_result
)
if(NOT create_pe_result EQUAL 0)
  message(FATAL_ERROR "failed to create PE32 fixture")
endif()

set(fake_wine "${test_root}/wine")
file(WRITE "${fake_wine}" "#!/bin/sh\nexit 0\n")
file(CHMOD "${fake_wine}"
  PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
)

set(env_out "${test_root}/legacy1161.env")
set(manifest_out "${test_root}/legacy1161.manifest")
execute_process(
  COMMAND
    "${CMAKE_COMMAND}" -E env
    "STARCRAFT_API_BW1161_DIR=${test_root}/bw1161"
    "STARCRAFT_API_WINE=${fake_wine}"
    "${STARCRAFT_RUNTIME_LEGACY1161_SETUP}"
    --require-launchable
    --write-env "${env_out}"
    --write-manifest "${manifest_out}"
  RESULT_VARIABLE setup_result
  OUTPUT_VARIABLE setup_output
  ERROR_VARIABLE setup_error
)
if(NOT setup_result EQUAL 0)
  message(FATAL_ERROR "legacy 1.16.1 setup failed: ${setup_error}\n${setup_output}")
endif()

foreach(expected
    "install.found=true"
    "product=starcraft-brood-war-1.16.1"
    "version=1.16.1"
    "legacy1161.pe32_i386=true"
    "legacy1161.launchable=true")
  if(NOT setup_output MATCHES "${expected}")
    message(FATAL_ERROR "missing setup output '${expected}':\n${setup_output}")
  endif()
endforeach()

if(NOT EXISTS "${env_out}")
  message(FATAL_ERROR "setup did not write env file")
endif()
if(NOT EXISTS "${manifest_out}")
  message(FATAL_ERROR "setup did not write manifest")
endif()

file(READ "${manifest_out}" manifest_content)
if(NOT manifest_content MATCHES "product starcraft-brood-war-1.16.1")
  message(FATAL_ERROR "manifest did not select legacy product")
endif()
