# StarCraft Remastered Porting Plan

## Compatibility Boundary

The upstream BWAPI runtime is tied to StarCraft: Brood War 1.16.1 on Windows. It reads and patches process memory through fixed offsets, Storm.dll imports, and Chaoslauncher-based injection. macOS, Linux, and StarCraft Remastered need a separate versioned runtime backend rather than edits to the 1.16.1 offset table.

## Production Requirements

Before a StarCraft Remastered backend can be marked supported, it must provide:

- A versioned runtime identity for the exact client build.
- A validated symbol or offset map for game state, command queue, player data, unit data, sprite/bullet data, replay state, and rendering hooks.
- Structure layout tests that prove BWAPI-facing fields match the target runtime.
- Command issue tests for movement, attack, build, train, research, upgrade, chat, pause/resume, leave, and observer/replay behavior.
- Multiplayer synchronization tests covering latency, turn submission, disconnect, replay determinism, and Battle.net policy constraints.
- macOS and Linux launch/attach tests for the authorized integration mechanism.

## Current Refactor Step

This repository now has a portable CMake entry point, a `BWAPI::Runtime` abstraction, and a runtime contract validator. Unsupported runtimes fail explicitly with a reason instead of silently reusing unsafe 1.16.1 memory bindings.

The production gate is `canClaimProductionSupport(probe, contract)`. A backend can only claim support when:

- The backend probe reports supported.
- The versioned runtime contract validates without unresolved required bindings.
- The backend exposes every BWAPI parity capability required by the contract.
- The backend reports at least 385 implemented BWAPI abstract API methods.
- The backend reports at least 72 implemented BWAPI command/action surface entries.

Use `starcraft-runtime-probe` to print the selected runtime, backend probe result, open result, contract validation errors, and final production-support decision. The tool reads `STARCRAFT_API_PRODUCT`, `STARCRAFT_API_VERSION`, `STARCRAFT_API_PROCESS_ID`, `STARCRAFT_API_EXECUTABLE`, and `STARCRAFT_API_MANIFEST` for non-interactive runtime selection. Use `starcraft-runtime-probe --require-production` in release gates; it exits non-zero until full parity support is validated.

Use `bwapi-api-surface-audit` to lock the public abstract API surface. The current parity baseline is 385 pure virtual methods across `Game`, `UnitInterface`, `PlayerInterface`, `BulletInterface`, `RegionInterface`, and `ForceInterface`.

## Runtime Manifest Format

StarCraft Remastered support must be described by a version-specific runtime manifest before code can claim parity. The manifest is line based, has no external parser dependency, supports `#` comments, and uses these directives:

```text
product starcraft-remastered
version <exact-client-build>
api-surface-methods 385
command-surface-entries 72
capability <capability-name>
binding <name> <kind> <required|optional> <evidence-id>
structure <name> <size> <required|optional>
field <structure>.<field> <offset> <size>
```

`binding`, `structure`, and `field` entries are matched against the BWAPI parity contract. Unknown entries produce warnings; missing required entries keep the contract invalid. A complete fixture exists at `tests/fixtures/remastered-complete.manifest`, and `runtime_manifest_test` proves that a full manifest validates while an incomplete manifest fails. `runtime_command_surface_test` locks 44 executable `UnitCommandTypes` plus 28 game/action methods.

Run a manifest through the probe with:

```sh
STARCRAFT_API_PRODUCT=starcraft-remastered \
  starcraft-runtime-probe --manifest tests/fixtures/remastered-complete.manifest
```

A valid manifest only proves that versioned offsets, symbols, structure fields, capabilities, and API surface declarations are complete. It does not by itself prove that the macOS/Linux runtime executor can attach, read state, issue commands, render overlays, or satisfy Battle.net synchronization rules; `production.supported` remains false until the backend probe proves those runtime behaviors.

## Runtime Executor Preflight

`RuntimeExecutor` separates three release-gate signals:

- `executor.contract_valid`: the selected contract or manifest has no unresolved required BWAPI parity entries.
- `executor.process_identified`: `STARCRAFT_API_PROCESS_ID` identifies a visible target process.
- `executor.target_located`: `STARCRAFT_API_EXECUTABLE` points to an existing target process executable or app bundle path.
- `executor.available`: the authorized attach/read/write/command executor is implemented for the selected product and platform.

The current macOS/Linux executor preflight can validate contracts, locate target paths, and verify a supplied target process id, but it intentionally reports `executor.available=false`. This keeps release automation honest: a complete manifest and visible process are necessary, but production support stays blocked until the runtime executor actually attaches to StarCraft Remastered and passes behavioral tests.

## Process Memory Primitive

`RuntimeProcessMemory` provides the first OS-specific read/write primitive:

- Linux uses `process_vm_readv` and `process_vm_writev`.
- macOS uses `mach_vm_read_overwrite` and `mach_vm_write`.
- Windows uses `ReadProcessMemory` and `WriteProcessMemory` for compatibility with the legacy target.

`runtime_process_memory_test` reads and writes a marker in the current process, so CI validates the real platform memory path without attaching to StarCraft or Battle.net. Production support still requires authorized target attach, validated address maps, command submission, event dispatch, overlay rendering, and multiplayer synchronization tests.
