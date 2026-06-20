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

On StarCraft: Remastered, launch retries default to a partial-screen direct game executable launch before Battle.net fallback. The executable target is started with `-launch -uid s1 -displayMode 0 -windowwidth 1024 -windowheight 768 -windowx 100 -windowy 100`, then the launcher falls back to `open StarCraft Launcher.app` and the launcher binary when needed. Set `STARCRAFT_API_WINDOWED=0` to disable the windowed executable-first path, or set `STARCRAFT_API_WINDOW_WIDTH`, `STARCRAFT_API_WINDOW_HEIGHT`, `STARCRAFT_API_WINDOW_X`, and `STARCRAFT_API_WINDOW_Y` to change the test window geometry. After each target, the launcher waits for a stable StarCraft game process before moving to the next target. The evidence report records each attempted path as `runtime.warning=runtime.launch_target=*` and records targets that produced no stable game as `runtime.warning=runtime.launch_target_no_game=*`.

If Battle.net is already handling StarCraft startup, the launcher does not spawn another Battle.net instance. It only exports `STARCRAFT_API_PROCESS_ID` when the actual StarCraft game executable is visible and stable.

POSIX launch targets are detached from the short-lived CLI process, so a successfully launched StarCraft process remains available after `starcraft-runtime-launch` exits.

If a Battle.net `--game=s1` handoff is stale and no StarCraft game process appears, rerun with `--replace-stale-handoff`. The launcher terminates the visible handoff before retrying and also terminates per-target handoffs before trying the next launch target, so it does not intentionally leave multiple Battle.net StarCraft handoffs running. This option is explicit because it terminates Battle.net processes.

Use `--evidence-out` to record the local launch/attach evidence without claiming production parity. The evidence report includes installation identity, executable size/hash, observed StarCraft/Battle.net processes, launch result, recent Battle.net/StarCraft log tails, parsed StarCraft launch PID/start/stop events, session transition duration summaries, Battle.net support error rows, and `diagnosis.*` fields. The diagnosis classifies blockers such as `blocked-no-game-process`, `blocked-battlenet-handoff-without-game`, `blocked-battlenet-handoff-short-lived-session`, `blocked-multiple-battlenet-handoffs-without-game`, and `blocked-multiple-battlenet-main-processes-no-game`. When Battle.net logs expose a support URL or `/client/error/BLZ...` code, the status is refined with support-error variants such as `blocked-battlenet-handoff-support-error` and `blocked-battlenet-handoff-short-lived-session-support-error`; the raw evidence is emitted as `support.error.*` plus `diagnosis.battle_net_support_*`. `diagnosis.short_lived_session_age_ms` shows how recently a short StarCraft run ended relative to the newest observed handoff event. `diagnosis.ready_for_attach=true` is required before runtime attach work can proceed. `STARCRAFT_API_LOG_DIR` can override the log directory for controlled test runs.

## Validation

```sh
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
tools/linux-smoke-build.sh
```

Use `starcraft-runtime-gap-report --require-production` as the release gate. It remains non-zero until all BWAPI parity requirements are backed by validated runtime evidence. When `--evidence-out` is provided, the gap report also prints the launch diagnosis to stdout as `diagnosis.status`, `diagnosis.ready_for_attach`, `diagnosis.battle_net_support_*`, and `diagnosis.blocker.*` rows so automated audits can see Battle.net blockers without opening the evidence file.

For iterative gap closure, use `--summary-only` to print category totals without per-gap detail, or `--category <name>` to focus on one category such as `executor-preflight`, `unit-command`, or `data-address`.

Gap reports now print `executor.bridge_mode`, `executor.behavior_proof.missing_count`, and `executor-behavior-proof` categories when a bridge is present but has not proven every required in-game behavior. This keeps bootstrap artifacts from being mistaken for production adapter evidence.

Executor bridge ready files are also bound to the selected runtime identity. If `STARCRAFT_API_PROCESS_ID` or `--process-id` is set, the ready file must contain the same `process_id`. If `STARCRAFT_API_EXECUTABLE` or `--executable` is set, the ready file must contain the same normalized `executable` path. Stale ready files from another StarCraft process are rejected before preflight or command submission can pass.

Use `starcraft-runtime-memory-probe --require-open` after a successful launch to verify that the selected runtime process identity is visible. Use `--require-access` to require actual process memory access rights. On macOS, `memory.opened=true` can still be paired with `memory.accessible=false` when `task_for_pid` is denied. Pass `--address <addr> --size <bytes> --require-read` only for an address that has been separately authorized and validated; the default probe does not read arbitrary game memory.

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

Command submission is manifest-backed for StarCraft: Remastered and requires a validated runtime adapter bridge. The CLI rejects command submission when the manifest is missing, incomplete, does not declare the BWAPI command/action being submitted, or the bridge is only a launch/attach bootstrap bridge:

```sh
build/starcraft-runtime-submit-command \
  --product starcraft-remastered \
  --version 1.23.10.13515 \
  --process-id 94840 \
  --executable /Users/jinminseong/Desktop/Starcraft1/StarCraft/x86_64/StarCraft.app/Contents/MacOS/StarCraft \
  --manifest /path/to/validated-remastered.manifest \
  --bridge /path/to/authorized-runtime-bridge \
  --game-action pauseGame
```

The bridge written by `starcraft-runtime-launch --bridge` is `mode=launch-attach-bootstrap` and includes the selected `process_id` and `executable`. It is only bootstrap/plumbing evidence and cannot satisfy production readiness. A production executor must publish matching runtime identity, `mode=validated-runtime-adapter`, and behavior proof lines for attach, game-state reads, unit reads, command issue, overlay drawing, event dispatch, replay analysis, multiplayer sync, and Battle.net policy validation.

## BWAPI Reference

BWAPI is used as a compatibility reference for the public API shape, command vocabulary, and legacy StarCraft: Brood War 1.16.1 behavior.

This project is not presenting upstream BWAPI authors as contributors to this StarCraft: Remastered adaptation. The BWAPI code and concepts are acknowledged as reference material and compatibility baseline only.

Reference: https://github.com/bwapi/bwapi

## Contributor

- minsing-jin

## Legal

StarCraft, StarCraft: Brood War, StarCraft: Remastered, Battle.net, and Blizzard Entertainment names are trademarks of Blizzard Entertainment. This repository does not include or redistribute StarCraft game binaries.
