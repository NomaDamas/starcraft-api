cmake_minimum_required(VERSION 3.20)

project(StarCraftAPI
  VERSION 0.1.0
  LANGUAGES CXX
)

option(STARCRAFT_API_BUILD_TESTS "Build StarCraft API portable tests" ON)
option(STARCRAFT_API_BUILD_TOOLS "Build StarCraft API portable tools" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(BWAPI_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/bwapi")
set(BWAPI_INCLUDE_DIR "${BWAPI_ROOT}/include")
set(GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/CMake/svnrev.h.in"
  "${GENERATED_INCLUDE_DIR}/svnrev.h"
  @ONLY
)

add_library(bwapi_public STATIC
  "${BWAPI_ROOT}/BWAPILIB/Source/AIModule.cpp"
  "${BWAPI_ROOT}/BWAPILIB/Source/BWAPI.cpp"
  "${BWAPI_ROOT}/BWAPILIB/Source/BroodwarOutputDevice.cpp"
  "${BWAPI_ROOT}/BWAPILIB/Source/BulletType.cpp"
  "${BWAPI_ROOT}/BWAPILIB/Source/Color.cpp"
  "${BWAPI_ROOT}/BWAPILIB/Source/DamageType.cpp"
  "${BWAPI_ROOT}/BWAPILIB/Source/Error.cpp"
  "${BWAPI_ROOT}/BWAPILIB/Source/Event.cpp"
  "${BWAPI_ROOT}/BWAPILIB/Source/ExplosionType.cpp"
  "${BWAPI_ROOT}/BWAPILIB/Source/Filters.cpp"
  "${BWAPI_ROOT}/BWAPILIB/Source/Forceset.cpp"
  "${BWAPI_ROOT}/BWAPILIB/Source/Game.cpp"
  "${BWAPI_ROOT}/BWAPILIB/Source/GameType.cpp"
  "${BWAPI_ROOT}/BWAPILIB/Source/Order.cpp"
  "${BWAPI_ROOT}/BWAPILIB/Source/Player.cpp"
  "${BWAPI_ROOT}/BWAPILIB/Source/PlayerType.cpp"
  "${BWAPI_ROOT}/BWAPILIB/Source/Playerset.cpp"
  "${BWAPI_ROOT}/BWAPILIB/Source/Position.cpp"
  "${BWAPI_ROOT}/BWAPILIB/Source/Race.cpp"
  "${BWAPI_ROOT}/BWAPILIB/Source/Region.cpp"
  "${BWAPI_ROOT}/BWAPILIB/Source/Regionset.cpp"
  "${BWAPI_ROOT}/BWAPILIB/Source/Streams.cpp"
  "${BWAPI_ROOT}/BWAPILIB/Source/TechType.cpp"
  "${BWAPI_ROOT}/BWAPILIB/Source/Unit.cpp"
  "${BWAPI_ROOT}/BWAPILIB/Source/UnitCommandType.cpp"
  "${BWAPI_ROOT}/BWAPILIB/Source/UnitSizeType.cpp"
  "${BWAPI_ROOT}/BWAPILIB/Source/UnitType.cpp"
  "${BWAPI_ROOT}/BWAPILIB/Source/Unitset.cpp"
  "${BWAPI_ROOT}/BWAPILIB/Source/UpgradeType.cpp"
  "${BWAPI_ROOT}/BWAPILIB/Source/WeaponType.cpp"
  "${BWAPI_ROOT}/BWAPILIB/UnitCommand.cpp"
)

target_include_directories(bwapi_public
  PUBLIC
    "${BWAPI_INCLUDE_DIR}"
  PRIVATE
    "${GENERATED_INCLUDE_DIR}"
)

add_library(starcraft_runtime STATIC
  "${BWAPI_ROOT}/Runtime/Legacy1161RuntimeBackend.cpp"
  "${BWAPI_ROOT}/Runtime/RemasteredRuntimeBackend.cpp"
  "${BWAPI_ROOT}/Runtime/RuntimeBackend.cpp"
  "${BWAPI_ROOT}/Runtime/RuntimeBackendFactory.cpp"
  "${BWAPI_ROOT}/Runtime/RuntimeCommandEncoder.cpp"
  "${BWAPI_ROOT}/Runtime/RuntimeCommandQueue.cpp"
  "${BWAPI_ROOT}/Runtime/RuntimeCommandSurface.cpp"
  "${BWAPI_ROOT}/Runtime/RuntimeContract.cpp"
  "${BWAPI_ROOT}/Runtime/RuntimeEnvironment.cpp"
  "${BWAPI_ROOT}/Runtime/RuntimeExecutor.cpp"
  "${BWAPI_ROOT}/Runtime/RuntimeImplementationGap.cpp"
  "${BWAPI_ROOT}/Runtime/RuntimeManifest.cpp"
  "${BWAPI_ROOT}/Runtime/RuntimeInstallation.cpp"
  "${BWAPI_ROOT}/Runtime/RuntimeProcess.cpp"
  "${BWAPI_ROOT}/Runtime/RuntimeProcessMemory.cpp"
  "${BWAPI_ROOT}/Runtime/RuntimeReadiness.cpp"
  "${BWAPI_ROOT}/Runtime/UnsupportedRuntimeBackend.cpp"
)

target_include_directories(starcraft_runtime
  PUBLIC
    "${BWAPI_INCLUDE_DIR}"
)

add_library(starcraft_api INTERFACE)
target_link_libraries(starcraft_api
  INTERFACE
    bwapi_public
    starcraft_runtime
)

add_library(StarCraftAPI::public ALIAS bwapi_public)
add_library(StarCraftAPI::runtime ALIAS starcraft_runtime)
add_library(StarCraftAPI::api ALIAS starcraft_api)

if(STARCRAFT_API_BUILD_TOOLS)
  add_executable(starcraft-runtime-probe
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/runtime_probe.cpp"
  )
  target_link_libraries(starcraft-runtime-probe
    PRIVATE
      StarCraftAPI::runtime
  )

  add_executable(starcraft-runtime-gap-report
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/runtime_gap_report.cpp"
  )
  target_link_libraries(starcraft-runtime-gap-report
    PRIVATE
      StarCraftAPI::runtime
  )

  add_executable(starcraft-runtime-submit-command
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/runtime_submit_command.cpp"
  )
  target_link_libraries(starcraft-runtime-submit-command
    PRIVATE
      StarCraftAPI::runtime
  )

  add_executable(starcraft-runtime-launch
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/runtime_launch.cpp"
  )
  target_link_libraries(starcraft-runtime-launch
    PRIVATE
      StarCraftAPI::runtime
  )

  add_executable(starcraft-runtime-memory-probe
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/runtime_memory_probe.cpp"
  )
  target_link_libraries(starcraft-runtime-memory-probe
    PRIVATE
      StarCraftAPI::runtime
  )

  add_executable(starcraft-runtime-adapter-proof
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/runtime_adapter_proof.cpp"
  )
  target_link_libraries(starcraft-runtime-adapter-proof
    PRIVATE
      StarCraftAPI::runtime
  )

  add_executable(starcraft-runtime-binary-anchors
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/runtime_binary_anchors.cpp"
  )

  add_executable(starcraft-runtime-input
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/runtime_input.cpp"
  )
  target_link_libraries(starcraft-runtime-input
    PRIVATE
      StarCraftAPI::runtime
  )
  if(APPLE)
    find_library(APPLICATION_SERVICES_FRAMEWORK ApplicationServices REQUIRED)
    target_link_libraries(starcraft-runtime-input
      PRIVATE
        "${APPLICATION_SERVICES_FRAMEWORK}"
    )
  endif()

  if(APPLE)
    add_custom_target(starcraft-runtime-sign-debug-tools
      COMMAND
        /usr/bin/codesign --force --sign - --entitlements
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/macos-debugger.entitlements"
        $<TARGET_FILE:starcraft-runtime-memory-probe>
      COMMAND
        /usr/bin/codesign --force --sign - --entitlements
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/macos-debugger.entitlements"
        $<TARGET_FILE:starcraft-runtime-adapter-proof>
      COMMAND
        /usr/bin/codesign --force --sign - --entitlements
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/macos-debugger.entitlements"
        $<TARGET_FILE:starcraft-runtime-probe>
      COMMAND
        /usr/bin/codesign --force --sign - --entitlements
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/macos-debugger.entitlements"
        $<TARGET_FILE:starcraft-runtime-gap-report>
      COMMAND
        /usr/bin/codesign --force --sign - --entitlements
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/macos-debugger.entitlements"
        $<TARGET_FILE:starcraft-runtime-launch>
      COMMAND
        /usr/bin/codesign --force --sign - --entitlements
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/macos-debugger.entitlements"
        $<TARGET_FILE:starcraft-runtime-binary-anchors>
      COMMAND
        /usr/bin/codesign --force --sign - --entitlements
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/macos-debugger.entitlements"
        $<TARGET_FILE:starcraft-runtime-input>
      DEPENDS
        starcraft-runtime-memory-probe
        starcraft-runtime-adapter-proof
        starcraft-runtime-probe
        starcraft-runtime-gap-report
        starcraft-runtime-launch
        starcraft-runtime-binary-anchors
        starcraft-runtime-input
      COMMENT "Ad-hoc signing runtime debug tools with macOS debugger entitlements"
      VERBATIM
    )
  endif()

  add_executable(bwapi-api-surface-audit
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/api_surface_audit.cpp"
  )
  target_compile_definitions(bwapi-api-surface-audit
    PRIVATE
      STARCRAFT_API_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
  )
endif()

if(STARCRAFT_API_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
