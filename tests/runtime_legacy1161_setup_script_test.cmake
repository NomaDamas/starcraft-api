if(NOT DEFINED STARCRAFT_API_LEGACY1161_SETUP_SCRIPT)
  message(FATAL_ERROR "STARCRAFT_API_LEGACY1161_SETUP_SCRIPT is required")
endif()
if(NOT DEFINED STARCRAFT_API_CLI_TEST_DIR)
  message(FATAL_ERROR "STARCRAFT_API_CLI_TEST_DIR is required")
endif()
if(NOT DEFINED Python3_EXECUTABLE)
  message(FATAL_ERROR "Python3_EXECUTABLE is required")
endif()

set(test_root "${STARCRAFT_API_CLI_TEST_DIR}/runtime-legacy1161-setup-script")
file(REMOVE_RECURSE "${test_root}")
file(MAKE_DIRECTORY "${test_root}/patches" "${test_root}/bw1161")

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

foreach(patcher IN ITEMS SC-1161.exe BW-1161.exe)
  execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${create_pe}" "${test_root}/patches/${patcher}"
    RESULT_VARIABLE create_pe_result
  )
  if(NOT create_pe_result EQUAL 0)
    message(FATAL_ERROR "failed to create fake ${patcher}")
  endif()
endforeach()

execute_process(
  COMMAND
    "${CMAKE_COMMAND}" -E env
    "STARCRAFT_API_BW1161_PATCH_DIR=${test_root}/patches"
    "STARCRAFT_API_BW1161_DIR=${test_root}/bw1161"
    "STARCRAFT_API_BW1161_MIN_FREE_MB=1"
    "${STARCRAFT_API_LEGACY1161_SETUP_SCRIPT}"
  RESULT_VARIABLE setup_result
  OUTPUT_VARIABLE setup_output
  ERROR_VARIABLE setup_error
)
if(setup_result EQUAL 0)
  message(FATAL_ERROR "setup script accepted forged patchers:\n${setup_output}\n${setup_error}")
endif()

set(combined_output "${setup_output}\n${setup_error}")
if(NOT combined_output MATCHES "patcher size mismatch")
  message(FATAL_ERROR "setup script did not fail closed on forged patchers:\n${combined_output}")
endif()
