if(NOT DEFINED STARCRAFT_API_LEGACY1161_ARTIFACT_SCAN)
  message(FATAL_ERROR "STARCRAFT_API_LEGACY1161_ARTIFACT_SCAN is required")
endif()
if(NOT DEFINED STARCRAFT_API_CLI_TEST_DIR)
  message(FATAL_ERROR "STARCRAFT_API_CLI_TEST_DIR is required")
endif()
if(NOT DEFINED Python3_EXECUTABLE)
  message(FATAL_ERROR "Python3_EXECUTABLE is required")
endif()

set(test_root "${STARCRAFT_API_CLI_TEST_DIR}/runtime-legacy1161-artifact-scan")
file(REMOVE_RECURSE "${test_root}")
file(MAKE_DIRECTORY "${test_root}/patches" "${test_root}/base-install" "${test_root}/patch-only")

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
  COMMAND "${Python3_EXECUTABLE}" "${create_pe}" "${test_root}/patches/SC-1161.exe"
  RESULT_VARIABLE create_fake_patch_result
)
if(NOT create_fake_patch_result EQUAL 0)
  message(FATAL_ERROR "failed to create fake patcher")
endif()

execute_process(
  COMMAND "${Python3_EXECUTABLE}" "${STARCRAFT_API_LEGACY1161_ARTIFACT_SCAN}" "${test_root}/patches/SC-1161.exe"
  RESULT_VARIABLE fake_patch_result
  OUTPUT_VARIABLE fake_patch_output
  ERROR_VARIABLE fake_patch_error
)
if(fake_patch_result EQUAL 0)
  message(FATAL_ERROR "forged patcher was accepted:\n${fake_patch_output}\n${fake_patch_error}")
endif()
if(NOT fake_patch_output MATCHES "official-patcher-size-mismatch")
  message(FATAL_ERROR "forged patcher failure did not mention size mismatch:\n${fake_patch_output}")
endif()
if(NOT fake_patch_output MATCHES "official-patcher-sha256-mismatch")
  message(FATAL_ERROR "forged patcher failure did not mention sha256 mismatch:\n${fake_patch_output}")
endif()

execute_process(
  COMMAND "${Python3_EXECUTABLE}" "${create_pe}" "${test_root}/base-install/StarCraft.exe"
  RESULT_VARIABLE create_base_exe_result
)
if(NOT create_base_exe_result EQUAL 0)
  message(FATAL_ERROR "failed to create base executable")
endif()
file(WRITE "${test_root}/base-install/StarDat.mpq" "fixture")
file(WRITE "${test_root}/base-install/BrooDat.mpq" "fixture")

execute_process(
  COMMAND
    "${Python3_EXECUTABLE}" "${STARCRAFT_API_LEGACY1161_ARTIFACT_SCAN}"
    --require-base-install
    "${test_root}/base-install"
  RESULT_VARIABLE base_result
  OUTPUT_VARIABLE base_output
  ERROR_VARIABLE base_error
)
if(NOT base_result EQUAL 0)
  message(FATAL_ERROR "base install fixture was rejected:\n${base_output}\n${base_error}")
endif()
if(NOT base_output MATCHES "base_install.ready=true")
  message(FATAL_ERROR "base install fixture was not marked ready:\n${base_output}")
endif()

execute_process(
  COMMAND "${Python3_EXECUTABLE}" "${create_pe}" "${test_root}/patch-only/BW-1161.exe"
  RESULT_VARIABLE create_patch_only_result
)
if(NOT create_patch_only_result EQUAL 0)
  message(FATAL_ERROR "failed to create patch-only fixture")
endif()

execute_process(
  COMMAND
    "${Python3_EXECUTABLE}" "${STARCRAFT_API_LEGACY1161_ARTIFACT_SCAN}"
    --require-base-install
    "${test_root}/patch-only"
  RESULT_VARIABLE patch_only_result
  OUTPUT_VARIABLE patch_only_output
  ERROR_VARIABLE patch_only_error
)
if(patch_only_result EQUAL 0)
  message(FATAL_ERROR "patch-only directory was accepted:\n${patch_only_output}\n${patch_only_error}")
endif()
if(NOT patch_only_output MATCHES "base-install-missing-StarCraft.exe-or-Brood-War.exe")
  message(FATAL_ERROR "patch-only failure did not mention missing executable:\n${patch_only_output}")
endif()
if(NOT patch_only_output MATCHES "base-install-missing-StarDat.mpq")
  message(FATAL_ERROR "patch-only failure did not mention missing StarDat.mpq:\n${patch_only_output}")
endif()
if(NOT patch_only_output MATCHES "base-install-missing-BrooDat.mpq")
  message(FATAL_ERROR "patch-only failure did not mention missing BrooDat.mpq:\n${patch_only_output}")
endif()
