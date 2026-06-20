#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir="${TMPDIR:-/tmp}/starcraft-api-linux-smoke-$$"
generated_dir="$build_dir/generated"
bin_dir="$build_dir/bin"

mkdir -p "$generated_dir" "$bin_dir"

cat > "$generated_dir/svnrev.h" <<'EOF'
#pragma once

static const int SVN_REV = 0;

#include "starcraftver.h"
EOF

cxx=${CXX:-c++}
cxxflags=${CXXFLAGS:-}

"$cxx" -std=c++17 $cxxflags \
  -I "$repo_root/bwapi/include" \
  "$repo_root/bwapi/Runtime/Legacy1161RuntimeBackend.cpp" \
  "$repo_root/bwapi/Runtime/RemasteredRuntimeBackend.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeBackend.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeBackendFactory.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeCommandQueue.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeCommandSurface.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeContract.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeEnvironment.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeExecutor.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeImplementationGap.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeInstallation.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeManifest.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeProcess.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeProcessMemory.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeReadiness.cpp" \
  "$repo_root/bwapi/Runtime/UnsupportedRuntimeBackend.cpp" \
  "$repo_root/tests/runtime_backend_test.cpp" \
  -o "$bin_dir/runtime_backend_test"

"$bin_dir/runtime_backend_test"

"$cxx" -std=c++17 $cxxflags \
  -I "$repo_root/bwapi/include" \
  "$repo_root/bwapi/Runtime/RuntimeCommandSurface.cpp" \
  "$repo_root/tests/runtime_command_surface_test.cpp" \
  -o "$bin_dir/runtime_command_surface_test"

"$bin_dir/runtime_command_surface_test"

"$cxx" -std=c++17 $cxxflags \
  -I "$repo_root/bwapi/include" \
  "$repo_root/bwapi/Runtime/RuntimeCommandQueue.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeCommandSurface.cpp" \
  "$repo_root/tests/runtime_command_queue_test.cpp" \
  -o "$bin_dir/runtime_command_queue_test"

"$bin_dir/runtime_command_queue_test"

"$cxx" -std=c++17 $cxxflags \
  -I "$repo_root/bwapi/include" \
  "$repo_root/bwapi/Runtime/Legacy1161RuntimeBackend.cpp" \
  "$repo_root/bwapi/Runtime/RemasteredRuntimeBackend.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeBackend.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeBackendFactory.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeCommandQueue.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeCommandSurface.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeContract.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeEnvironment.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeExecutor.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeImplementationGap.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeInstallation.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeManifest.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeProcess.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeProcessMemory.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeReadiness.cpp" \
  "$repo_root/bwapi/Runtime/UnsupportedRuntimeBackend.cpp" \
  "$repo_root/tests/runtime_contract_test.cpp" \
  -o "$bin_dir/runtime_contract_test"

"$bin_dir/runtime_contract_test"

"$cxx" -std=c++17 $cxxflags \
  -I "$repo_root/bwapi/include" \
  "$repo_root/bwapi/Runtime/RuntimeBackend.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeCommandSurface.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeContract.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeImplementationGap.cpp" \
  "$repo_root/tests/runtime_implementation_gap_test.cpp" \
  -o "$bin_dir/runtime_implementation_gap_test"

"$bin_dir/runtime_implementation_gap_test"

"$cxx" -std=c++17 $cxxflags \
  -DSTARCRAFT_API_TEST_FIXTURE_DIR="\"$repo_root/tests/fixtures\"" \
  -I "$repo_root/bwapi/include" \
  "$repo_root/bwapi/Runtime/Legacy1161RuntimeBackend.cpp" \
  "$repo_root/bwapi/Runtime/RemasteredRuntimeBackend.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeBackend.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeBackendFactory.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeCommandQueue.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeCommandSurface.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeContract.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeEnvironment.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeExecutor.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeImplementationGap.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeManifest.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeProcess.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeProcessMemory.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeReadiness.cpp" \
  "$repo_root/bwapi/Runtime/UnsupportedRuntimeBackend.cpp" \
  "$repo_root/tests/runtime_manifest_test.cpp" \
  -o "$bin_dir/runtime_manifest_test"

"$bin_dir/runtime_manifest_test"

"$cxx" -std=c++17 $cxxflags \
  -DSTARCRAFT_API_TEST_FIXTURE_DIR="\"$repo_root/tests/fixtures\"" \
  -I "$repo_root/bwapi/include" \
  "$repo_root/bwapi/Runtime/Legacy1161RuntimeBackend.cpp" \
  "$repo_root/bwapi/Runtime/RemasteredRuntimeBackend.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeBackend.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeBackendFactory.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeCommandQueue.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeCommandSurface.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeContract.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeEnvironment.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeExecutor.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeImplementationGap.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeManifest.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeProcess.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeProcessMemory.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeReadiness.cpp" \
  "$repo_root/bwapi/Runtime/UnsupportedRuntimeBackend.cpp" \
  "$repo_root/tests/runtime_executor_test.cpp" \
  -o "$bin_dir/runtime_executor_test"

"$bin_dir/runtime_executor_test"

"$cxx" -std=c++17 $cxxflags \
  -DSTARCRAFT_API_TEST_FIXTURE_DIR="\"$repo_root/tests/fixtures\"" \
  -I "$repo_root/bwapi/include" \
  "$repo_root/bwapi/Runtime/Legacy1161RuntimeBackend.cpp" \
  "$repo_root/bwapi/Runtime/RemasteredRuntimeBackend.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeBackend.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeBackendFactory.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeCommandQueue.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeCommandSurface.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeContract.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeEnvironment.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeExecutor.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeImplementationGap.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeManifest.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeProcess.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeProcessMemory.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeReadiness.cpp" \
  "$repo_root/bwapi/Runtime/UnsupportedRuntimeBackend.cpp" \
  "$repo_root/tests/runtime_production_bridge_test.cpp" \
  -o "$bin_dir/runtime_production_bridge_test"

"$bin_dir/runtime_production_bridge_test"

"$cxx" -std=c++17 $cxxflags \
  -I "$repo_root/bwapi/include" \
  "$repo_root/bwapi/Runtime/RuntimeBackend.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeCommandQueue.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeCommandSurface.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeContract.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeImplementationGap.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeReadiness.cpp" \
  "$repo_root/tests/runtime_readiness_test.cpp" \
  -o "$bin_dir/runtime_readiness_test"

"$bin_dir/runtime_readiness_test"

"$cxx" -std=c++17 $cxxflags \
  -I "$repo_root/bwapi/include" \
  "$repo_root/bwapi/Runtime/RuntimeBackend.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeEnvironment.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeProcess.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeProcessMemory.cpp" \
  "$repo_root/tests/runtime_process_test.cpp" \
  -o "$bin_dir/runtime_process_test"

"$bin_dir/runtime_process_test"

"$cxx" -std=c++17 $cxxflags \
  -I "$repo_root/bwapi/include" \
  "$repo_root/bwapi/Runtime/RuntimeProcessMemory.cpp" \
  "$repo_root/tests/runtime_process_memory_test.cpp" \
  -o "$bin_dir/runtime_process_memory_test"

"$bin_dir/runtime_process_memory_test"

"$cxx" -std=c++17 $cxxflags \
  -I "$repo_root/bwapi/include" \
  "$repo_root/bwapi/Runtime/Legacy1161RuntimeBackend.cpp" \
  "$repo_root/bwapi/Runtime/RemasteredRuntimeBackend.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeBackend.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeBackendFactory.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeCommandQueue.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeCommandSurface.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeContract.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeEnvironment.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeExecutor.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeImplementationGap.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeInstallation.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeManifest.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeProcess.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeReadiness.cpp" \
  "$repo_root/bwapi/Runtime/UnsupportedRuntimeBackend.cpp" \
  "$repo_root/tools/runtime_probe.cpp" \
  -o "$bin_dir/starcraft-runtime-probe"

"$bin_dir/starcraft-runtime-probe" >/dev/null

"$cxx" -std=c++17 $cxxflags \
  -I "$repo_root/bwapi/include" \
  "$repo_root/bwapi/Runtime/Legacy1161RuntimeBackend.cpp" \
  "$repo_root/bwapi/Runtime/RemasteredRuntimeBackend.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeBackend.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeBackendFactory.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeCommandQueue.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeCommandSurface.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeContract.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeEnvironment.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeExecutor.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeImplementationGap.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeInstallation.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeManifest.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeProcess.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeReadiness.cpp" \
  "$repo_root/bwapi/Runtime/UnsupportedRuntimeBackend.cpp" \
  "$repo_root/tools/runtime_gap_report.cpp" \
  -o "$bin_dir/starcraft-runtime-gap-report"

"$bin_dir/starcraft-runtime-gap-report" >/dev/null

"$cxx" -std=c++17 $cxxflags \
  -I "$repo_root/bwapi/include" \
  "$repo_root/bwapi/Runtime/Legacy1161RuntimeBackend.cpp" \
  "$repo_root/bwapi/Runtime/RemasteredRuntimeBackend.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeBackend.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeBackendFactory.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeCommandQueue.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeCommandSurface.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeContract.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeEnvironment.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeExecutor.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeImplementationGap.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeInstallation.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeManifest.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeProcess.cpp" \
  "$repo_root/bwapi/Runtime/RuntimeReadiness.cpp" \
  "$repo_root/bwapi/Runtime/UnsupportedRuntimeBackend.cpp" \
  "$repo_root/tools/runtime_submit_command.cpp" \
  -o "$bin_dir/starcraft-runtime-submit-command"

"$bin_dir/starcraft-runtime-submit-command" --help >/dev/null

"$cxx" -std=c++17 $cxxflags \
  -DSTARCRAFT_API_SOURCE_DIR="\"$repo_root\"" \
  "$repo_root/tools/api_surface_audit.cpp" \
  -o "$bin_dir/bwapi-api-surface-audit"

"$bin_dir/bwapi-api-surface-audit" >/dev/null

"$cxx" -std=c++17 $cxxflags \
  -I "$repo_root/bwapi/include" \
  -I "$generated_dir" \
  "$repo_root/bwapi/BWAPILIB/Source/AIModule.cpp" \
  "$repo_root/bwapi/BWAPILIB/Source/BWAPI.cpp" \
  "$repo_root/bwapi/BWAPILIB/Source/BroodwarOutputDevice.cpp" \
  "$repo_root/bwapi/BWAPILIB/Source/BulletType.cpp" \
  "$repo_root/bwapi/BWAPILIB/Source/Color.cpp" \
  "$repo_root/bwapi/BWAPILIB/Source/DamageType.cpp" \
  "$repo_root/bwapi/BWAPILIB/Source/Error.cpp" \
  "$repo_root/bwapi/BWAPILIB/Source/Event.cpp" \
  "$repo_root/bwapi/BWAPILIB/Source/ExplosionType.cpp" \
  "$repo_root/bwapi/BWAPILIB/Source/Filters.cpp" \
  "$repo_root/bwapi/BWAPILIB/Source/Forceset.cpp" \
  "$repo_root/bwapi/BWAPILIB/Source/Game.cpp" \
  "$repo_root/bwapi/BWAPILIB/Source/GameType.cpp" \
  "$repo_root/bwapi/BWAPILIB/Source/Order.cpp" \
  "$repo_root/bwapi/BWAPILIB/Source/Player.cpp" \
  "$repo_root/bwapi/BWAPILIB/Source/PlayerType.cpp" \
  "$repo_root/bwapi/BWAPILIB/Source/Playerset.cpp" \
  "$repo_root/bwapi/BWAPILIB/Source/Position.cpp" \
  "$repo_root/bwapi/BWAPILIB/Source/Race.cpp" \
  "$repo_root/bwapi/BWAPILIB/Source/Region.cpp" \
  "$repo_root/bwapi/BWAPILIB/Source/Regionset.cpp" \
  "$repo_root/bwapi/BWAPILIB/Source/Streams.cpp" \
  "$repo_root/bwapi/BWAPILIB/Source/TechType.cpp" \
  "$repo_root/bwapi/BWAPILIB/Source/Unit.cpp" \
  "$repo_root/bwapi/BWAPILIB/Source/UnitCommandType.cpp" \
  "$repo_root/bwapi/BWAPILIB/Source/UnitSizeType.cpp" \
  "$repo_root/bwapi/BWAPILIB/Source/UnitType.cpp" \
  "$repo_root/bwapi/BWAPILIB/Source/Unitset.cpp" \
  "$repo_root/bwapi/BWAPILIB/Source/UpgradeType.cpp" \
  "$repo_root/bwapi/BWAPILIB/Source/WeaponType.cpp" \
  "$repo_root/bwapi/BWAPILIB/UnitCommand.cpp" \
  "$repo_root/tests/public_api_smoke_test.cpp" \
  -o "$bin_dir/public_api_smoke_test"

"$bin_dir/public_api_smoke_test"

echo "Linux smoke build passed: $build_dir"
