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
  --evidence-out /tmp/starcraft-api-local-runtime.evidence \
  --bridge /tmp/starcraft-api-local-bridge \
  --print-env
```

The launcher searches `STARCRAFT_API_EXECUTABLE`, `STARCRAFT_API_INSTALL_DIR`, `STARCRAFT_API_STARCRAFT_DIR`, common macOS Desktop/Application paths, and common Windows install roots.

If Battle.net is already handling StarCraft startup, the launcher does not spawn another Battle.net instance. It only exports `STARCRAFT_API_PROCESS_ID` when the actual StarCraft game executable is visible and stable.

Use `--evidence-out` to record the local launch/attach evidence without claiming production parity. The evidence report includes installation identity, executable size/hash, observed StarCraft/Battle.net processes, launch result, recent Battle.net/StarCraft log tails, parsed StarCraft launch PID/start/stop events, session transition duration summaries, and `diagnosis.*` fields. The diagnosis classifies blockers such as `blocked-no-game-process`, `blocked-battlenet-handoff-without-game`, and `blocked-battlenet-handoff-short-lived-session`; `diagnosis.ready_for_attach=true` is required before runtime attach work can proceed. `STARCRAFT_API_LOG_DIR` can override the log directory for controlled test runs.

## Validation

```sh
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
tools/linux-smoke-build.sh
```

Use `starcraft-runtime-gap-report --require-production` as the release gate. It remains non-zero until all BWAPI parity requirements are backed by validated runtime evidence.

When using a bootstrap manifest, pass the runtime identity explicitly so the report attributes gaps to StarCraft Remastered instead of an unknown runtime:

```sh
build/starcraft-runtime-gap-report \
  --manifest /tmp/starcraft-api-local-bootstrap.manifest \
  --product starcraft-remastered \
  --version 1.23.10.13515 \
  --executable /Users/jinminseong/Desktop/Starcraft1/StarCraft/x86_64/StarCraft.app/Contents/MacOS/StarCraft \
  --bridge /tmp/starcraft-api-local-bridge \
  --evidence-out /tmp/starcraft-api-local-gap.evidence
```

Command submission is manifest-backed for StarCraft: Remastered. The CLI rejects command submission when the manifest is missing, incomplete, or does not declare the BWAPI command/action being submitted:

```sh
build/starcraft-runtime-submit-command \
  --product starcraft-remastered \
  --version 1.23.10.13515 \
  --manifest /path/to/validated-remastered.manifest \
  --bridge /path/to/authorized-runtime-bridge \
  --game-action pauseGame
```

## BWAPI Reference

BWAPI is used as a compatibility reference for the public API shape, command vocabulary, and legacy StarCraft: Brood War 1.16.1 behavior.

This project is not presenting upstream BWAPI authors as contributors to this StarCraft: Remastered adaptation. The BWAPI code and concepts are acknowledged as reference material and compatibility baseline only.

Reference: https://github.com/bwapi/bwapi

## Contributor

- minsing-jin

## Legal

StarCraft, StarCraft: Brood War, StarCraft: Remastered, Battle.net, and Blizzard Entertainment names are trademarks of Blizzard Entertainment. This repository does not include or redistribute StarCraft game binaries.
