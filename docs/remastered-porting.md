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

The Remastered backend advertises the built-in BWAPI facade surface when no manifest is supplied: 385 abstract API methods plus the 44 unit commands and 28 game actions known to the portable command queue/encoder. This reports deterministic command serialization coverage, not in-game behavior. Actual state extraction, command delivery, overlay rendering, event dispatch, replay analysis, multiplayer synchronization, and AI module loading remain gated by versioned runtime bindings plus live adapter proof lines such as `proof.issue_commands=passed`.

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

By default, StarCraft: Remastered launches use a partial-screen direct executable path before Battle.net fallback: `-launch -uid s1 -displayMode 0 -windowwidth 1024 -windowheight 768 -windowx 100 -windowy 100`. Set `STARCRAFT_API_WINDOWED=0` to use the older launcher-first fallback order. Set `STARCRAFT_API_WINDOW_WIDTH`, `STARCRAFT_API_WINDOW_HEIGHT`, `STARCRAFT_API_WINDOW_X`, and `STARCRAFT_API_WINDOW_Y` to change the window geometry for local tests. Use `--play-replay <path>` to append `playReplay <path>` for controlled active replay proof runs without relying on `STARCRAFT_API_EXTRA_ARGS`; the path must be an existing Brood War `.rep` file.

`--evidence-out` writes a launch/attach diagnostic report that captures the installation identity, executable size/hash, observed StarCraft/Battle.net process snapshot, launch result, recent Battle.net/StarCraft log tails, and parsed StarCraft session start/stop events. This report is for debugging and auditability only; it is not accepted as BWAPI parity evidence unless later in-game bindings, command execution, state extraction, event dispatch, overlay rendering, and synchronization tests pass. Set `STARCRAFT_API_LOG_DIR` to force a specific log directory during local tests.

## Runtime Manifest Format

StarCraft Remastered support must be described by a version-specific runtime manifest before code can claim parity. The manifest is line based, has no external parser dependency, supports `#` comments, and uses these directives:

```text
product starcraft-remastered
version <exact-client-build>
api-surface-methods 385
command-surface-entries 72
unit-command Attack_Move <live-proven|mock-tested|documented-scenario|fail-closed|adapter-local> [detail]
game-action issueCommand <live-proven|mock-tested|documented-scenario|fail-closed|adapter-local> [detail]
capability <capability-name>
binding <name> <kind> <required|optional> <evidence-id>
structure <name> <size> <required|optional>
field <structure>.<field> <offset> <size>
```

`binding`, `structure`, `field`, `unit-command`, and `game-action` entries are matched against the BWAPI parity contract. Unknown entries produce warnings or errors depending on whether they could falsely satisfy a release gate; missing required entries keep the manifest or contract invalid. Every command/action directive must record an evidence status. Only `live-proven` can satisfy production command parity; `mock-tested`, `documented-scenario`, `fail-closed`, and `adapter-local` remain useful tracking states but keep `command-evidence` implementation gaps. A complete fixture exists at `tests/fixtures/remastered-complete.manifest`, and `runtime_manifest_test` proves that a full manifest validates while an incomplete manifest fails. Fixture evidence such as `fixture:*` is parser/contract test data only; production support claims reject it even when the fixture is otherwise complete. `runtime_command_surface_test` locks 44 executable `UnitCommandTypes` plus 28 game/action methods and their non-production default evidence states.

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
  --version <version> \
  --executable "$STARCRAFT_API_EXECUTABLE" \
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

On StarCraft: Remastered, launch retries prefer the partial-screen direct executable path before launcher fallback. The default executable arguments are `-launch -uid s1 -displayMode 0 -windowwidth 1024 -windowheight 768 -windowx 100 -windowy 100`; if `--play-replay <path>` is provided, `playReplay <path>` is appended after those flags. The replay path is validated before launch: non-`.rep` files, missing files, and requests that would attach to an already-running StarCraft process without `--replace-running` fail instead of producing menu-only proof. If this does not produce a stable game process, macOS falls back to `open StarCraft Launcher.app` and then the launcher binary. Each target is followed by a stable game-process check. Attempted paths are emitted as `runtime.warning=runtime.launch_target=*`; replay intent is emitted as `runtime.warning=runtime.launch_replay=<path>`; targets that did not produce a stable game process are emitted as `runtime.warning=runtime.launch_target_no_game=*`.

For controlled command-line launch experiments, prefer `--play-replay <path>` for replay playback and use `STARCRAFT_API_EXTRA_ARGS` only for additional quoted arguments after the default Remastered flags. Pair `--launch --replace-running` when an existing StarCraft game process must be terminated before testing the new arguments or replay path, so only one StarCraft instance remains visible and the replay request is actually applied.

POSIX launch children are detached from the short-lived CLI process before `exec`, so a stable StarCraft process remains attachable after `starcraft-runtime-launch` exits.

For repeated gap audits, use `starcraft-runtime-gap-report --summary-only` to emit only readiness and implementation category totals. Use `starcraft-runtime-gap-report --category <name>` to print only one implementation gap category while preserving the global category counts. When `--evidence-out` is enabled, the gap report also mirrors launch diagnosis rows to stdout, including `diagnosis.status`, `diagnosis.ready_for_attach`, `diagnosis.battle_net_support_*`, and `diagnosis.blocker.*`.

## Runtime Executor Preflight

Use `starcraft-runtime-memory-probe --require-open` to verify process identity visibility, and add `--require-access` when the check must prove actual process-memory access rights. The probe resolves the same runtime environment as the launcher and prints both `memory.opened` and `memory.accessible`; on macOS, `memory.opened=true` can still be paired with `memory.accessible=false` when `task_for_pid` is denied. It only attempts `readProcessMemory` when `--address` is explicitly supplied. This is diagnostic evidence for the attach layer, not BWAPI parity evidence.

`starcraft-runtime-memory-probe --find-ascii` and `--find-u64` now separate scan execution from match proof: `memory.find.scan_success=true` means the readable memory scan ran, while `memory.find.success=true` requires at least one actual match. Use `--require-find` when a missing map/replay marker must fail the command.

When the selected parity contract requires `SharedMemoryClient`, executor preflight also emits `executor.memory_accessible` and fails readiness with `runtime-memory-accessible` plus a `memory-access` implementation gap if the target process cannot be opened for real VM access. This keeps a visible StarCraft process from being mistaken for an attachable BWAPI-compatible runtime.

For macOS local attach tests, `starcraft-runtime-sign-debug-tools` ad-hoc signs the runtime diagnostic tools with `tools/macos-debugger.entitlements`. After signing, `starcraft-runtime-memory-probe --read-first-readable --require-open --require-access --require-read` discovers a readable target VM region and reads bytes from the selected process. This proves the attach/read primitive without depending on a hard-coded StarCraft address.

`RuntimeExecutor` separates three release-gate signals:

- `executor.contract_valid`: the selected contract or manifest has no unresolved required BWAPI parity entries.
- `executor.process_identified`: `STARCRAFT_API_PROCESS_ID` identifies a visible target process.
- `executor.memory_accessible`: the selected target process grants the memory access required by the shared-memory client capability.
- `executor.target_located`: `STARCRAFT_API_EXECUTABLE` points to an existing target process executable or app bundle path.
- `executor.available`: the authorized attach/read/write/command executor is implemented for the selected product and platform.

The current macOS/Linux executor preflight can validate contracts, locate target paths, verify a supplied target process id, and verify a local filesystem bridge readiness file. This keeps release automation honest: a complete manifest, visible process, and bridge are necessary, but production support stays blocked until the runtime executor actually attaches to StarCraft Remastered and passes behavioral tests.

Automatic bridge discovery is stricter than an explicit `--bridge`: it only adopts one unambiguous ready directory whose product, version, selected PID, executable identity, resident ABI, active state, and heartbeat all validate against exactly one currently visible StarCraft process. If no PID is selected, multiple game PIDs are visible, the resident metadata is absent or stale, or more than one candidate bridge matches, discovery returns no bridge. Proof-backed contract lines are ignored until a concrete runtime PID is selected and the selected process opens with the expected executable identity, which prevents old `/tmp` bridge artifacts from reducing gap counts. Duplicate proof metadata and contract ready keys fail closed instead of using first-value-wins parsing.

The local filesystem bridge can now create a readiness file for launch/attach bootstrapping. `starcraft-runtime-launch --bridge` writes `mode=launch-attach-bootstrap`, which is sufficient for command-submission plumbing tests, but it is not in-game command execution evidence and is rejected by production preflight. `starcraft-runtime-adapter-proof` writes `proof.attach=passed` only after the selected process identity is visible and the required process-memory access succeeds. If later requested checks fail, the tool still writes a validated ready file containing only proof lines that actually passed, so gap reports can distinguish proven attach/read/Battle.net policy work from still-missing in-game behavior.

`starcraft-runtime-gap-report` and `starcraft-runtime-probe` expose bridge proof status directly through `executor.bridge_mode`, `executor.behavior_proof.missing_count`, and `executor.behavior_proof.missing=*` rows. Missing behavior proofs are also counted under the `executor-behavior-proof` implementation gap category, including when no validated executor bridge is configured.

`starcraft-runtime-memory-probe --process-state --region-summary` prints OS process status, thread count, and readable/writable/executable memory-region counters for launch debugging. Use `--region-list --region-around <addr>` to print the exact mapping that owns a candidate address before treating it as StarCraft evidence. During an active match, use `--scan-u32-counters --find-target-executable-only --find-non-executable-only` to list target-image counter candidates with three samples and deltas before passing a candidate to `starcraft-runtime-adapter-proof --state-counter-address`. These diagnostics help distinguish "process exists and can be attached" from "adapter proved an active match." They are not parity proof lines.

`starcraft-runtime-binary-anchors --default-remastered-anchors --executable <path>` scans the SC:R binary directly for known state/unit/bullet diagnostic anchors, including `CUnit::sgUnitsMem`, SC:R EUD unit/bullet adapter RTTI strings, and `CBullet: Damage`. It maps Mach-O file offsets to VM addresses, emits duplicate anchor occurrences, reports enclosing C-string start addresses, and emits candidate code/data references. Use this before broad live-memory scans so Remastered binding work is driven by binary evidence instead of UI automation or menu screenshots. These anchors become production evidence only after the live adapter proves the matching in-process structure or command path.

`starcraft-runtime-adapter-proof --unit-candidate-address <addr>` validates explicit CUnit array candidates before broad memory scans. Pair it with binary-anchor or live-memory analysis when a likely unit array address is available; the tool still refuses `proof.read_units=passed` unless the candidate or fallback scan proves active BWAPI-compatible unit records.

`starcraft-runtime-input --post-timeout-ms <ms>` bounds macOS keyboard event posting during manual windowed tests. If Accessibility trust is unavailable or the target process blocks event delivery, the helper exits with `input.success=false` instead of hanging and leaving a background process. This helper is not part of the production adapter contract; it is only local test-control plumbing when no replay or validated in-process command path is available.

`starcraft-runtime-adapter-proof --prove-active-match-state` requires resident read-game-state frame/tick progression plus live active-unit evidence from the selected StarCraft process, and rejects self fixtures. This is the gate that prevents menu/login state from being mistaken for in-game BWAPI parity evidence. `starcraft-runtime-adapter-proof --prove-read-units --unit-best-dump-out <path>` dumps the strongest CUnit candidate snapshot when the live scanner cannot prove a full active array. Use `--state-scan-timeout-ms <ms>` and `--unit-scan-timeout-ms <ms>` to keep live debugging loops bounded. Add `--state-scan-diagnostics` and `--unit-scan-diagnostics` to print scanned region/byte counts, timeout and byte-limit status, candidate counts, best-candidate counters, and executable-image region skips. State-counter scans chunk through readable non-executable StarCraft target and anonymous runtime regions instead of sampling only the first chunk of each region. Explicit frame-counter addresses in non-StarCraft file-backed mappings are rejected, because writable helper-framework state can look dynamic without being BW game state. Broad unit scans skip target-executable mapped regions by default because SC:R image/cstring mappings create many false positives; pass `--unit-scan-include-image-regions` only for deliberate image-data audits. This is failure-analysis evidence only; it does not emit `proof.read_units=passed` unless the active unit array threshold is met.

`starcraft-runtime-adapter-proof --prove-read-map-data` first scans live process memory for installed map path/name strings, including ASCII and UTF-16 ASCII-compatible strings. If the live string scan times out or finds no installed map, it falls back to a fresh replay artifact proof: `STARCRAFT_API_REPLAY_DIR`, `<install>/Maps/Replays`, and platform user replay directories are searched for a `LastReplay.rep`-matched autosave replay, and the autosave stem is matched to an installed map file. This fallback emits `proof.read_map_data.source=latest-replay-artifact` and records the replay path/size, but it is still map/replay metadata evidence only; it does not prove active-match state, command queue, overlay hook, or multiplayer sync behavior by itself. Replay analysis can use the fallback only when current replay playback is proven by the selected process command line or live active-match player metadata has already been proven from unit snapshots.

`--prove-read-player-data` projects BWAPI-facing player metadata from a passing live SC:R unit-node snapshot. The adapter ready file may emit `contract.structure.BW::PlayerInfo`, `contract.field.BW::PlayerInfo.*`, and `contract.field.BW::BWGame.alliance` with the evidence id `compat-player-projection-v1:unit-snapshot-derived`. This closes the BWAPI facade layout for player ids, inferred race, and alliance masks without claiming that a native SC:R `PlayerInfo` array or native resource/supply address has been found. Unit/player read proofs are recorded when their own live snapshots pass; `proof.active_match_state=passed` remains a separate stricter behavior proof. Native resource/supply bindings should replace the projection evidence when they are proven.

`starcraft-runtime-adapter-proof --discover-command-queue` is the next read-only step toward live command delivery. It scans prioritized writable SC:R runtime sections for vector-like `{begin,end,capacity}` triples whose buffers are also writable, scans non-empty anonymous/private raw turn-buffer-shaped windows so heap-backed command buffers can surface, suppresses sliding-window `{end,capacity,next}` false positives from accepted triples, writes ranked candidates to `command_queue.candidates.tsv`, and records `proof.command_queue_discovery=candidate-found` when candidates exist. This line is intentionally not a behavior proof: preflight still requires `proof.issue_commands=passed`, `command.receiver=active`, `command.sink=runtime-command-queue-v1`, and proof-backed `sgdwBytesInCmdQueue`/`TurnBuffer` bindings before any BWAPI command submission is accepted. When an explicit candidate is tested and fails behavior proof, the ready file may include `diagnostic.issue_commands.snapshot=issue_commands.snapshot.tsv` and `diagnostic.issue_commands.reason=*`; those diagnostic lines are negative evidence only and do not resolve command gaps. During authorized live debugging, `--issue-command-candidate-scan-limit <count>` tries the top discovered candidates one at a time and appends an attempt table to `issue_commands.snapshot.tsv`; candidates still require actual pause/resume or equivalent in-game behavior before any command proof can be emitted.

When `--prove-issue-commands` is enabled, discovery also samples the top candidates for natural live activity and records `activity_samples`, `activity_transitions`, and `activity_byte_changes` in `command_queue.candidates.tsv`. `--command-queue-activity-ms <ms>` controls that observation window; use a longer window while a human or harness is creating real in-game input. `--command-queue-max-scan-mb <mb>` controls the command-queue discovery budget independently from `--unit-max-scan-mb`, so command triage can run without broad unit scans. `--command-queue-candidate-limit <count>` retains more candidates for read-only discovery and activity sampling without enabling command writes; `--issue-command-candidate-scan-limit <count>` is still required before the adapter will attempt live command delivery against discovered candidates. Empty fixed raw-buffer candidates and overlapping vector triples are deprioritized because they are common SC:R memory false positives. A dynamic-vector or raw turn-buffer-shaped candidate may be tried implicitly only when it has observed natural activity, bounded used bytes, and enough remaining capacity for a tail append; raw shape alone is insufficient after live false positives accepted delivery/readback but produced no game behavior. Unless an explicit `--command-queue-vector-address` is supplied, the live issue-command proof refuses to write to discovered candidates that had no observed natural activity. High-entropy raw buffers that behave like large state storage are excluded from implicit writes even if a count-like field changes during input. `command_queue.candidates.tsv` records `counter_offset`, prefix byte/entropy/opcode/pointer-table diagnostics, selector/count before/after bytes, buffer prefix before/after bytes, changed byte totals/ranges, `region_class`, `implicit_write_eligible`, `live_write_safe`, and the reason for each retained candidate. Activity changes only reprioritize candidates and expose false positives. They do not emit `proof.issue_commands=passed` and do not resolve `TurnBuffer` or `sgdwBytesInCmdQueue` without subsequent delivery and behavior proof.

Command discovery emits both total and retained vector/raw candidate counts, plus a retained active-candidate count, so live audits can distinguish "scanner found many plausible raw buffers" from "scanner found a behavior-backed command sink." `issue_commands.snapshot.tsv` records `pause_frame_counter_sampled` and `pause_frame_counter_matched` at proof and attempt level. Failed candidates now preserve the sampled pause delta in the reason string when a frame counter was readable but did not stop.

`--prove-multiplayer-sync` now inspects the target process command line on macOS/Linux. If the process was launched with replay playback arguments such as `playReplay`, the sync snapshot records `replay_launch_detected=true` and keeps `multiplayer_sync.ready=false`; active replay proof is not Battle.net multiplayer synchronization behavior. Replay-analysis evidence derived from active-match live metadata is reported as `active-match-live-metadata`, not as `replay_only`.

Validated adapter ready files may also publish contract proof lines for evidence that was directly proven by that adapter run:

```text
contract.binding.<binding-name>=<binding-kind>|<evidence-id>
contract.structure.<structure-name>=<byte-size>|<evidence-id>
contract.field.<structure>.<field>=<offset>|<byte-size>|<evidence-id>
```

The runtime only applies these lines when the ready file protocol, product, version, process id, executable identity, and `mode=validated-runtime-adapter` all match the selected runtime. Live bridge auto-discovery additionally requires a currently resolved StarCraft game PID, so a stale `/tmp/starcraft-api-live-bridge` is ignored when the game has exited or only a crash reporter process remains. `starcraft-runtime-adapter-proof` may preserve earlier passing proof/contract lines across bounded rechecks only under that same identity, and it removes any proof key requested by the current run before writing the new ready file. Current built-in proofs emit `contract.binding.shared-memory-client-transport=transport|proof.attach=passed` after authorized attach/memory access succeeds. A passing `--prove-read-units` run additionally emits the CUnit array binding, CUnit record size, and the BWAPI-facing CUnit fields that the scanner validated. Missing or failed proof lines are not inferred, even when the same adapter run wrote other passing proof lines.

`starcraft-runtime-submit-command` also requires validated adapter mode, behavior proof lines, an active command receiver, a runtime command queue sink, and proof-backed command queue bindings before it appends commands to the bridge. A one-shot `starcraft-runtime-adapter-proof` ready file is not enough: it proves attach/read behavior and then exits, so there is no resident command receiver that can consume `commands.log` and deliver it into SC:R. Bootstrap bridges cannot receive BWAPI commands because they have not proven in-game command execution.

`RuntimeCommandEncoder` is the offline prerequisite for that live command path. It converts supported BWAPI unit commands and turn-buffer game actions into the packed little-endian byte payloads used by the original 1.16.1 `QueueGameCommand` path, and `starcraft-runtime-submit-command --dry-run-encode` exposes this without requiring a process, manifest, or bridge. The encoder now covers additional state-independent or explicitly-disambiguated forms such as `Unload_All`, `Cancel_Morph`, `Use_Tech` for Stim Packs, and `Load` when the caller supplies the already-resolved load order. The encoder is intentionally fail-closed: commands whose original BWAPI mapping depends on live selected-unit type, target-unit type, or discovered SC:R command queue state return `encoded=false` instead of inventing behavior. Passing dry-run encoding is not `proof.issue_commands=passed`; it only proves that the command payload contract is deterministic before a resident adapter writes to the live `TurnBuffer`/`sgdwBytesInCmdQueue` equivalent.

Live command proof now performs a post-delivery survival check before finalizing the ready file. If a candidate accepts encoded bytes but the StarCraft process exits shortly afterward, the attempt remains `diagnostic.issue_commands.*` failure evidence; it is not accepted as command queue behavior proof and does not resolve `sgdwBytesInCmdQueue` or `TurnBuffer`. Failed pause/resume proofs restore both the resume append and original pause append when proof bytes are still present. Manual `--append-game-action` diagnostics paired with `--state-counter-address` also restore the append when the tracked frame counter does not progress.

Adapter implementations should use `requiredRuntimeExecutorBehaviorProofs()` as the canonical behavior-proof inventory. The ready file strings below are generated from that same runtime contract in tests.

A production executor bridge must publish identity for the selected runtime, `mode=validated-runtime-adapter`, and behavior proof lines. When `STARCRAFT_API_PROCESS_ID` or `--process-id` is set, `process_id` must match. When `STARCRAFT_API_EXECUTABLE` or `--executable` is set, `executable` must match after path normalization. This prevents a stale ready file from another StarCraft process from satisfying preflight or accepting commands.

```text
process_id=<selected-runtime-pid>
executable=<selected-runtime-executable>
mode=validated-runtime-adapter
```

The Mac SC:R resident adapter foundation adds identity-bound resident metadata without making the bridge production-ready by itself. These lines only prove that a versioned resident bridge is present and current enough to be inspected:

```text
resident.adapter=active
resident.adapter.abi=starcraft-api-resident-adapter-v1
resident.adapter.process_id=<selected-runtime-pid>
resident.adapter.heartbeat=<monotonic-counter>
```

The resident bridge validator rejects duplicate resident and runtime identity keys, unsupported ABI major versions, stale or malformed heartbeats, stale ready-file timestamps, wrong process ids, wrong product/version, and executable mismatches. The resident queue ABI uses a fixed header with `magic=SCAQ`, `abiMajor=1`, `headerBytes`, `recordBytes`, `kind`, `capacityRecords`, `writeSequence`, `readSequence`, and `heartbeat`. Queue headers are rejected when the kind does not match the expected command/state/event/overlay/proof queue, the record or capacity size is zero, the read sequence is ahead of the write sequence, the queued distance exceeds capacity, or the heartbeat is stale. Queue records also carry a fixed record header with `headerBytes`, `kind`, `payloadBytes`, and `sequence`; malformed record kind, size, payload bounds, and sequence windows are rejected before any payload-specific parser can run.

`starcraft-api-resident.dylib` is the macOS resident module target for this ABI. At this stage it only exports the ABI entry points and loader planning validates the selected platform/product/process/executable/bridge/adapter path. Actual in-process loading and live behavior proof are later issues; the target existing is not itself attach, command, overlay, or sync proof.

Ready-file contract evidence also remains fail-closed. `fixture:`, `unit-test:`, `mock:`, `self-fixture:`, `diagnostic.*`, `static-anchor:`, and `scr-platform-anchor:` evidence is not accepted as production binding evidence. `proof.*` evidence must also have the matching proof line in the same ready file. This keeps mock harnesses and diagnostic anchors from resolving production contract bindings.

Resident metadata is not behavior evidence. It must not emit or imply these production proof lines:

```text
proof.issue_commands=passed
proof.draw_overlays=passed
proof.multiplayer_sync=passed
command.sink=runtime-command-queue-v1
```

Those lines remain gated by later live in-game behavior proofs.

For `proof.issue_commands=passed` to count, the ready file must also prove that a live executor is receiving commands and that the selected runtime command queue was resolved for this session:

```text
command.receiver=active
command.sink=runtime-command-queue-v1
contract.binding.BW::BWDATA::sgdwBytesInCmdQueue=command-queue|<non-fixture-evidence-id>
contract.binding.BW::BWDATA::TurnBuffer=command-queue|<non-fixture-evidence-id>
```

Without these lines, `proof.issue_commands=passed` is treated as missing by preflight. This prevents a diagnostic bridge log from being mistaken for a working in-game Move/Attack/Build command path.

Per-command production evidence is separate from the static command-surface listing.
`command_surface.unit_command.<n>` and `command_surface.game_action.<n>` describe
the current surface/evidence status, but they are not trusted as live proof
sources. A resident ready file may promote individual entries only through
proof-backed live rows:

```text
command_surface.live_unit_command.0=Attack_Move|live-proven|proof.issue_commands=passed:Attack_Move
command_surface.live_game_action.0=pauseGame|live-proven|proof.issue_commands=passed:pauseGame
```

Those rows are accepted only when the referenced proof line is already validated
for the selected runtime process, resident adapter ABI, heartbeat, active-match
state, and snapshot metadata. Hand-written manifest `live-proven` entries and
ready rows that reference missing, mock, stale, or fixture proof remain
non-production evidence and keep the command-evidence gap open.

The required behavior proof lines are:

```text
proof.attach=passed
proof.read_game_state=passed
proof.active_match_state=passed
proof.read_units=passed
proof.issue_commands=passed
proof.draw_overlays=passed
proof.dispatch_events=passed
proof.replay_analysis=passed
proof.multiplayer_sync=passed
proof.battle_net_policy=passed
proof.load_ai_modules=passed
```

These proof lines represent validated adapter evidence, not manifest declarations. Production readiness still requires a validated in-process or otherwise authorized SC:R adapter that can prove active match/replay state, read state, issue commands, draw overlays, dispatch events, replay analysis, multiplayer synchronization behavior, Battle.net policy compliance, and BWAPI-compatible AI module loading.
`proof.read_game_state=passed` is accepted only with resident adapter metadata, matching heartbeat/process id, strictly increasing frame/tick samples, and a readable live counter address in the selected process. `proof.active_match_state=passed` additionally requires valid resident read-game-state proof plus matching active unit-array or unit-node evidence; bare or hand-written behavior lines are treated as missing.

## Process Memory Primitive

`RuntimeProcessMemory` provides the first OS-specific read/write primitive:

- Linux uses `process_vm_readv` and `process_vm_writev`.
- macOS uses `mach_vm_read_overwrite` and `mach_vm_write`.
- Windows uses `ReadProcessMemory` and `WriteProcessMemory` for compatibility with the legacy target.

`runtime_process_memory_test` reads and writes a marker in the current process, so CI validates the real platform memory path without attaching to StarCraft or Battle.net. Production support still requires authorized target attach, validated address maps, command submission, event dispatch, overlay rendering, and multiplayer synchronization tests.
