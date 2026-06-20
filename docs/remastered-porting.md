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
- The backend reports every BWAPI command/action surface entry by name: 44 executable unit commands and 28 game actions.
- Executor preflight proves actual process-memory access when the parity contract requires the shared-memory client capability.

Use `starcraft-runtime-probe` to print the selected runtime, backend probe result, open result, contract validation errors, and final production-support decision. The tool reads `STARCRAFT_API_PRODUCT`, `STARCRAFT_API_VERSION`, `STARCRAFT_API_PROCESS_ID`, `STARCRAFT_API_EXECUTABLE`, and `STARCRAFT_API_MANIFEST` for non-interactive runtime selection. Use `starcraft-runtime-probe --require-production` in release gates; it exits non-zero until full parity support is validated.

Use `bwapi-api-surface-audit` to lock the public abstract API surface. The current parity baseline is 385 pure virtual methods across `Game`, `UnitInterface`, `PlayerInterface`, `BulletInterface`, `RegionInterface`, and `ForceInterface`.

Use `starcraft-runtime-launch` to discover and launch a local StarCraft Remastered installation before probing:

```sh
starcraft-runtime-launch \
  --launch \
  --require-running \
  --manifest-out /tmp/starcraft-api-local-bootstrap.manifest \
  --evidence-out /tmp/starcraft-api-local-runtime.evidence \
  --bridge /tmp/starcraft-api-local-bridge \
  --print-env
```

The launcher searches `STARCRAFT_API_EXECUTABLE`, `STARCRAFT_API_INSTALL_DIR`, `STARCRAFT_API_STARCRAFT_DIR`, common macOS Desktop/Application paths, and common Windows install roots. On macOS it recognizes the Battle.net layout:

```text
StarCraft/x86_64/StarCraft.app/Contents/MacOS/StarCraft
StarCraft/StarCraft Launcher.app/Contents/MacOS/StarCraft Launcher
```

`--require-running` only succeeds when the actual game executable is visible and stable for the runtime process check. A transient launcher, Battle.net handoff, or short-lived splash process is reported as `runtime.running=false` and is not exported as `STARCRAFT_API_PROCESS_ID`.

By default, StarCraft: Remastered launches use a partial-screen direct executable path before Battle.net fallback: `-launch -uid s1 -displayMode 0 -windowwidth 1024 -windowheight 768 -windowx 100 -windowy 100`. Set `STARCRAFT_API_WINDOWED=0` to use the older launcher-first fallback order. Set `STARCRAFT_API_WINDOW_WIDTH`, `STARCRAFT_API_WINDOW_HEIGHT`, `STARCRAFT_API_WINDOW_X`, and `STARCRAFT_API_WINDOW_Y` to change the window geometry for local tests.

`--evidence-out` writes a launch/attach diagnostic report that captures the installation identity, executable size/hash, observed StarCraft/Battle.net process snapshot, launch result, recent Battle.net/StarCraft log tails, and parsed StarCraft session start/stop events. This report is for debugging and auditability only; it is not accepted as BWAPI parity evidence unless later in-game bindings, command execution, state extraction, event dispatch, overlay rendering, and synchronization tests pass. Set `STARCRAFT_API_LOG_DIR` to force a specific log directory during local tests.

## Runtime Manifest Format

StarCraft Remastered support must be described by a version-specific runtime manifest before code can claim parity. The manifest is line based, has no external parser dependency, supports `#` comments, and uses these directives:

```text
product starcraft-remastered
version <exact-client-build>
api-surface-methods 385
command-surface-entries 72
unit-command Attack_Move
game-action issueCommand
capability <capability-name>
binding <name> <kind> <required|optional> <evidence-id>
structure <name> <size> <required|optional>
field <structure>.<field> <offset> <size>
```

`binding`, `structure`, `field`, `unit-command`, and `game-action` entries are matched against the BWAPI parity contract. Unknown entries produce warnings or errors depending on whether they could falsely satisfy a release gate; missing required entries keep the manifest or contract invalid. A complete fixture exists at `tests/fixtures/remastered-complete.manifest`, and `runtime_manifest_test` proves that a full manifest validates while an incomplete manifest fails. Fixture evidence such as `fixture:*` is parser/contract test data only; production support claims reject it even when the fixture is otherwise complete. `runtime_command_surface_test` locks 44 executable `UnitCommandTypes` plus 28 game/action methods.

Run a manifest through the probe with:

```sh
STARCRAFT_API_PRODUCT=starcraft-remastered \
  starcraft-runtime-probe --manifest tests/fixtures/remastered-complete.manifest
```

A valid manifest only proves that versioned offsets, symbols, structure fields, capabilities, and API surface declarations are complete. It does not by itself prove that the macOS/Linux runtime executor can attach, read state, issue commands, render overlays, or satisfy Battle.net synchronization rules; `production.supported` remains false until the backend probe proves those runtime behaviors.

For launch/attach gap analysis, pass the runtime identity from `starcraft-runtime-launch` directly into the gap report. Add `--evidence-out` when you want the readiness gaps and the launch/attach diagnosis from the same run:

```sh
starcraft-runtime-gap-report \
  --manifest /tmp/starcraft-api-local-bootstrap.manifest \
  --product starcraft-remastered \
  --version 1.23.10.13515 \
  --executable /Users/jinminseong/Desktop/Starcraft1/StarCraft/x86_64/StarCraft.app/Contents/MacOS/StarCraft \
  --bridge /tmp/starcraft-api-local-bridge \
  --evidence-out /tmp/starcraft-api-local-gap.evidence
```

Incomplete bootstrap manifests remain non-production, but the report preserves product/version identity so the remaining gaps are attributed to the StarCraft Remastered backend instead of collapsing to `unknown`.

`starcraft-runtime-launch --evidence-out` also summarizes parsed Battle.net `s1` session transitions:

- `session.latest_state` records whether the latest collected StarCraft session event is running, stopped, pre-existing, or unknown.
- `session.latest_observed_timestamp` records the newest parsed Battle.net timestamp used for the session summary.
- `session.latest_transition_start_timestamp` and `session.latest_transition_end_timestamp` preserve the latest complete paired start/stop window even if newer Battle.net handoff lines follow it.
- `session.launch_process_event_count` and `session.latest_launch_process_id` record Battle.net launch lines with transient StarCraft process ids.
- `session.shortest_transition_duration_ms`, `session.longest_transition_duration_ms`, and `session.latest_transition_duration_ms` quantify how long observed StarCraft sessions stayed running before Battle.net reported stop events.
- `session.transition.*` keeps the paired start/stop timestamps and source log lines for attach debugging.
- `diagnosis.status` gives the machine-readable launch/attach blocker, including `blocked-no-game-process`, `blocked-battlenet-handoff-without-game`, `blocked-battlenet-handoff-short-lived-session`, `blocked-battlenet-handoff-support-error`, and `blocked-battlenet-handoff-short-lived-session-support-error`.
- `diagnosis.short_lived_session_age_ms` records how far the latest observed handoff event is from the latest short complete StarCraft transition; this keeps a run that starts and stops in a few seconds from being mislabeled as a generic stale handoff.
- `diagnosis.game_process_count`, `diagnosis.battle_net_main_count`, `diagnosis.battle_net_handoff_count`, `diagnosis.multiple_battle_net_main_visible`, and `diagnosis.multiple_battle_net_handoffs_visible` distinguish the real StarCraft executable from one or more Battle.net main/handoff processes.
- `support.error.*` records recent Battle.net support error lines when logs include `/client/error/BLZ...`, `battle.net/support`, or `support.blizzard.com`; `diagnosis.battle_net_support_code`, `diagnosis.battle_net_support_url`, and `diagnosis.battle_net_support_line` mirror the latest parsed support error.
- `diagnosis.ready_for_attach` is only true when a stable StarCraft game executable process is visible, selected, and safe for the next authorized adapter step.
- `diagnosis.blocker.*` records the concrete reasons the current run cannot submit commands or claim production parity.

These fields are diagnostic evidence, not readiness evidence. If sessions stop after only a few seconds and no stable StarCraft process id is visible, the production gate must continue to fail with `runtime-process-identified` and executor preflight gaps.

`starcraft-runtime-launch --replace-stale-handoff` is the explicit recovery path for a stuck Battle.net `--game=s1` handoff. The default remains conservative and does not spawn a duplicate Battle.net instance while a handoff is visible. The recovery flag terminates a visible handoff before retrying and also terminates per-target handoffs before trying the next launch target, so retries do not intentionally leave multiple Battle.net StarCraft handoffs running.

On StarCraft: Remastered, launch retries prefer the partial-screen direct executable path before launcher fallback. The default executable arguments are `-launch -uid s1 -displayMode 0 -windowwidth 1024 -windowheight 768 -windowx 100 -windowy 100`; if this does not produce a stable game process, macOS falls back to `open StarCraft Launcher.app` and then the launcher binary. Each target is followed by a stable game-process check. Attempted paths are emitted as `runtime.warning=runtime.launch_target=*`; targets that did not produce a stable game process are emitted as `runtime.warning=runtime.launch_target_no_game=*`.

POSIX launch children are detached from the short-lived CLI process before `exec`, so a stable StarCraft process remains attachable after `starcraft-runtime-launch` exits.

For repeated gap audits, use `starcraft-runtime-gap-report --summary-only` to emit only readiness and implementation category totals. Use `starcraft-runtime-gap-report --category <name>` to print only one implementation gap category while preserving the global category counts. When `--evidence-out` is enabled, the gap report also mirrors launch diagnosis rows to stdout, including `diagnosis.status`, `diagnosis.ready_for_attach`, `diagnosis.battle_net_support_*`, and `diagnosis.blocker.*`.

## Runtime Executor Preflight

Use `starcraft-runtime-memory-probe --require-open` to verify process identity visibility, and add `--require-access` when the check must prove actual process-memory access rights. The probe resolves the same runtime environment as the launcher and prints both `memory.opened` and `memory.accessible`; on macOS, `memory.opened=true` can still be paired with `memory.accessible=false` when `task_for_pid` is denied. It only attempts `readProcessMemory` when `--address` is explicitly supplied. This is diagnostic evidence for the attach layer, not BWAPI parity evidence.

When the selected parity contract requires `SharedMemoryClient`, executor preflight also emits `executor.memory_accessible` and fails readiness with `runtime-memory-accessible` plus a `memory-access` implementation gap if the target process cannot be opened for real VM access. This keeps a visible StarCraft process from being mistaken for an attachable BWAPI-compatible runtime.

`RuntimeExecutor` separates three release-gate signals:

- `executor.contract_valid`: the selected contract or manifest has no unresolved required BWAPI parity entries.
- `executor.process_identified`: `STARCRAFT_API_PROCESS_ID` identifies a visible target process.
- `executor.memory_accessible`: the selected target process grants the memory access required by the shared-memory client capability.
- `executor.target_located`: `STARCRAFT_API_EXECUTABLE` points to an existing target process executable or app bundle path.
- `executor.available`: the authorized attach/read/write/command executor is implemented for the selected product and platform.

The current macOS/Linux executor preflight can validate contracts, locate target paths, verify a supplied target process id, and verify a local filesystem bridge readiness file. This keeps release automation honest: a complete manifest, visible process, and bridge are necessary, but production support stays blocked until the runtime executor actually attaches to StarCraft Remastered and passes behavioral tests.

The local filesystem bridge can now create a readiness file for launch/attach bootstrapping. `starcraft-runtime-launch --bridge` writes `mode=launch-attach-bootstrap`, which is sufficient for command-submission plumbing tests, but it is not in-game command execution evidence and is rejected by production preflight. `starcraft-runtime-adapter-proof` writes `proof.attach=passed` only after the selected process identity is visible and the required process-memory access succeeds.

`starcraft-runtime-gap-report` and `starcraft-runtime-probe` expose bridge proof status directly through `executor.bridge_mode`, `executor.behavior_proof.missing_count`, and `executor.behavior_proof.missing=*` rows. Missing behavior proofs are also counted under the `executor-behavior-proof` implementation gap category, including when no validated executor bridge is configured.

`starcraft-runtime-submit-command` also requires the validated adapter mode and behavior proof lines before it appends commands to the bridge. Bootstrap bridges cannot receive BWAPI commands because they have not proven in-game command execution.

Adapter implementations should use `requiredRuntimeExecutorBehaviorProofs()` as the canonical behavior-proof inventory. The ready file strings below are generated from that same runtime contract in tests.

A production executor bridge must publish identity for the selected runtime, `mode=validated-runtime-adapter`, and behavior proof lines. When `STARCRAFT_API_PROCESS_ID` or `--process-id` is set, `process_id` must match. When `STARCRAFT_API_EXECUTABLE` or `--executable` is set, `executable` must match after path normalization. This prevents a stale ready file from another StarCraft process from satisfying preflight or accepting commands.

```text
process_id=<selected-runtime-pid>
executable=<selected-runtime-executable>
mode=validated-runtime-adapter
```

The required behavior proof lines are:

```text
proof.attach=passed
proof.read_game_state=passed
proof.read_units=passed
proof.issue_commands=passed
proof.draw_overlays=passed
proof.dispatch_events=passed
proof.replay_analysis=passed
proof.multiplayer_sync=passed
proof.battle_net_policy=passed
```

These proof lines represent validated adapter evidence, not manifest declarations. Production readiness still requires a validated in-process or otherwise authorized SC:R adapter that can read state, issue commands, draw overlays, dispatch events, and prove multiplayer synchronization behavior.

## Process Memory Primitive

`RuntimeProcessMemory` provides the first OS-specific read/write primitive:

- Linux uses `process_vm_readv` and `process_vm_writev`.
- macOS uses `mach_vm_read_overwrite` and `mach_vm_write`.
- Windows uses `ReadProcessMemory` and `WriteProcessMemory` for compatibility with the legacy target.

`runtime_process_memory_test` reads and writes a marker in the current process, so CI validates the real platform memory path without attaching to StarCraft or Battle.net. Production support still requires authorized target attach, validated address maps, command submission, event dispatch, overlay rendering, and multiplayer synchronization tests.
