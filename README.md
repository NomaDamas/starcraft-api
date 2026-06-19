# StarCraft API

StarCraft API is a production-oriented C++ runtime adaptation project for StarCraft: Remastered and Battle.net installations.

The goal is to preserve the BWAPI programming surface while moving the runtime integration away from the original Windows-only StarCraft: Brood War 1.16.1 memory-offset model. The current codebase provides a portable runtime contract, manifest validation, process/memory primitives, command queue plumbing, launch/attach bootstrapping, and cross-platform build/test gates.

## Current Status

- macOS, Linux, and Windows CMake targets are supported.
- The public BWAPI-compatible API surface is audited at 385 abstract methods.
- The command surface is audited at 44 unit commands and 28 game actions.
- StarCraft: Remastered runtime contracts fail closed until version-specific game-state bindings, command execution, events, overlays, replay behavior, and multiplayer synchronization are validated.
- macOS Battle.net installations can be discovered and launch/attach bootstrap files can be generated with `starcraft-runtime-launch`.

## macOS Battle.net Bootstrap

```sh
cmake --build build --config Release --parallel

build/starcraft-runtime-launch \
  --launch \
  --require-running \
  --manifest-out /tmp/starcraft-api-local-bootstrap.manifest \
  --bridge /tmp/starcraft-api-local-bridge \
  --print-env
```

The launcher searches `STARCRAFT_API_EXECUTABLE`, `STARCRAFT_API_INSTALL_DIR`, `STARCRAFT_API_STARCRAFT_DIR`, common macOS Desktop/Application paths, and common Windows install roots.

If Battle.net is already handling StarCraft startup, the launcher does not spawn another Battle.net instance. It only exports `STARCRAFT_API_PROCESS_ID` when the actual StarCraft game executable is visible and stable.

## Validation

```sh
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
tools/linux-smoke-build.sh
```

Use `starcraft-runtime-gap-report --require-production` as the release gate. It remains non-zero until all BWAPI parity requirements are backed by validated runtime evidence.

## BWAPI Reference

BWAPI is used as a compatibility reference for the public API shape, command vocabulary, and legacy StarCraft: Brood War 1.16.1 behavior.

This project is not presenting upstream BWAPI authors as contributors to this StarCraft: Remastered adaptation. The BWAPI code and concepts are acknowledged as reference material and compatibility baseline only.

Reference: https://github.com/bwapi/bwapi

## Contributor

- minsing-jin

## Legal

StarCraft, StarCraft: Brood War, StarCraft: Remastered, Battle.net, and Blizzard Entertainment names are trademarks of Blizzard Entertainment. This repository does not include or redistribute StarCraft game binaries.
