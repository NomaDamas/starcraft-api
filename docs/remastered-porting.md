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

Use `starcraft-runtime-probe` to print the selected runtime, backend probe result, open result, contract validation errors, and final production-support decision. The tool reads `STARCRAFT_API_PRODUCT`, `STARCRAFT_API_VERSION`, and `STARCRAFT_API_EXECUTABLE` for non-interactive runtime selection. Use `starcraft-runtime-probe --require-production` in release gates; it exits non-zero until full parity support is validated.

Use `bwapi-api-surface-audit` to lock the public abstract API surface. The current parity baseline is 385 pure virtual methods across `Game`, `UnitInterface`, `PlayerInterface`, `BulletInterface`, `RegionInterface`, and `ForceInterface`.
