# StarCraft API

StarCraft API is a production-oriented C++ runtime adaptation project for StarCraft: Remastered and Battle.net installations.

The goal is to preserve the BWAPI programming surface while moving the runtime integration away from the original Windows-only StarCraft: Brood War 1.16.1 memory-offset model. The current codebase provides a portable runtime contract, manifest validation, process/memory primitives, command queue plumbing, launch/attach bootstrapping, and cross-platform build/test gates.

## Current Status

- macOS, Linux, and Windows CMake targets are supported.
- The public BWAPI-compatible API surface is audited at 385 abstract methods.
- The command surface is audited at 44 unit commands and 28 game actions.
- StarCraft: Remastered runtime contracts fail closed until version-specific game-state bindings, command execution, events, overlays, replay behavior, and multiplayer synchronization are validated. The command/action names are known to the portable queue and encoder, while live command delivery remains separately gated by command receiver and `proof.issue_commands=passed` evidence. Overlay rendering remains a separate production proof.
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

On StarCraft: Remastered, launch retries default to a partial-screen direct game executable launch before Battle.net fallback. The executable target is started with `-launch -uid s1 -displayMode 0 -windowwidth 1024 -windowheight 768 -windowx 100 -windowy 100`, then the launcher falls back to `open StarCraft Launcher.app` and the launcher binary when needed. Set `STARCRAFT_API_WINDOWED=0` to disable the windowed executable-first path, or set `STARCRAFT_API_WINDOW_WIDTH`, `STARCRAFT_API_WINDOW_HEIGHT`, `STARCRAFT_API_WINDOW_X`, and `STARCRAFT_API_WINDOW_Y` to change the test window geometry. Use `--play-replay <path>` for controlled active replay proof runs; it requires an existing Brood War `.rep` file, appends `playReplay <path>` after the Remastered launch flags, and records `runtime.warning=runtime.launch_replay=<path>`. If a StarCraft process is already running, pair it with `--replace-running`; otherwise the replay request is rejected instead of silently attaching to the old menu process. After each target, the launcher waits for a stable StarCraft game process before moving to the next target. The evidence report records each attempted path as `runtime.warning=runtime.launch_target=*` and records targets that produced no stable game as `runtime.warning=runtime.launch_target_no_game=*`.

If Battle.net is already handling StarCraft startup, the launcher does not spawn another Battle.net instance. It only exports `STARCRAFT_API_PROCESS_ID` when the actual StarCraft game executable is visible and stable.

POSIX launch targets are detached from the short-lived CLI process, so a successfully launched StarCraft process remains available after `starcraft-runtime-launch` exits.

Use `--replace-running` with `--launch` only for controlled direct-launch tests where the current StarCraft game process must be replaced with a new single instance. This is required when changing the `--play-replay` target and useful when changing `STARCRAFT_API_EXTRA_ARGS`, which appends quoted arguments after the default Remastered launch flags.

If a Battle.net `--game=s1` handoff is stale and no StarCraft game process appears, rerun with `--replace-stale-handoff`. The launcher terminates the visible handoff before retrying and also terminates per-target handoffs before trying the next launch target, so it does not intentionally leave multiple Battle.net StarCraft handoffs running. This option is explicit because it terminates Battle.net processes.

Use `--evidence-out` to record the local launch/attach evidence without claiming production parity. The evidence report includes installation identity, executable size/hash, observed StarCraft/Battle.net processes, launch result, recent Battle.net/StarCraft log tails, parsed StarCraft launch PID/start/stop events, session transition duration summaries, Battle.net support error rows, and `diagnosis.*` fields. The diagnosis classifies blockers such as `blocked-no-game-process`, `blocked-battlenet-handoff-without-game`, `blocked-battlenet-handoff-short-lived-session`, `blocked-multiple-battlenet-handoffs-without-game`, and `blocked-multiple-battlenet-main-processes-no-game`. When Battle.net logs expose a support URL or `/client/error/BLZ...` code, the status is refined with support-error variants such as `blocked-battlenet-handoff-support-error` and `blocked-battlenet-handoff-short-lived-session-support-error`; the raw evidence is emitted as `support.error.*` plus `diagnosis.battle_net_support_*`. `diagnosis.short_lived_session_age_ms` shows how recently a short StarCraft run ended relative to the newest observed handoff event. `diagnosis.ready_for_attach=true` is required before runtime attach work can proceed. `STARCRAFT_API_LOG_DIR` can override the log directory for controlled test runs.

## Validation

```sh
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
tools/linux-smoke-build.sh
```

Use `starcraft-runtime-gap-report --require-production` as the release gate. It remains non-zero until all BWAPI parity requirements are backed by validated runtime evidence. When `--evidence-out` is provided, the gap report also prints the launch diagnosis to stdout as `diagnosis.status`, `diagnosis.ready_for_attach`, `diagnosis.battle_net_support_*`, and `diagnosis.blocker.*` rows so automated audits can see Battle.net blockers without opening the evidence file.

For iterative gap closure, use `--summary-only` to print category totals without per-gap detail, or `--category <name>` to focus on one category such as `memory-access`, `executor-preflight`, `executor-behavior-proof`, or `data-address`.

Gap reports now print `executor.bridge_mode`, `executor.behavior_proof.missing_count`, and `executor-behavior-proof` categories when a bridge is absent or has not proven every required in-game behavior. `proof.active_match_state=passed` is required separately from `proof.read_game_state=passed`, so menu/login memory activity cannot be treated as in-game BWAPI parity evidence. If `starcraft-runtime-adapter-proof` fails after some checks pass, it writes only the passing proof lines to the validated bridge ready file; failed behaviors remain missing. This keeps missing, bootstrap, fixture, menu-only, or partial adapter artifacts from being mistaken for production adapter evidence.

Executor bridge ready files are also bound to the selected runtime identity. If `STARCRAFT_API_PROCESS_ID` or `--process-id` is set, the ready file must contain the same `process_id`. If `STARCRAFT_API_EXECUTABLE` or `--executable` is set, the ready file must contain the same normalized `executable` path. Stale ready files from another StarCraft process are rejected before preflight or command submission can pass. Auto-discovery of `/tmp/starcraft-api-live-bridge` now requires a currently resolved StarCraft game PID; if the game process has exited or only a Blizzard crash reporter remains, the stale bridge is ignored instead of being treated as partial production evidence.

Use `starcraft-runtime-memory-probe --require-open` after a successful launch to verify that the selected runtime process identity is visible. Use `--require-access` to require actual process memory access rights. Add `--process-state --region-summary` when debugging macOS Battle.net launches so the report includes OS process status, thread count, memory-region counters, mapped-file region counts, and target-executable mapped region counts. Use `--region-list --region-around <addr>` before promoting a candidate address so helper-framework mappings are not mistaken for StarCraft game state. Use `--scan-u32-counters --find-target-executable-only --find-non-executable-only` during an active match to list target-image counter candidates with three samples and deltas before passing one to `--state-counter-address`. On macOS, `memory.opened=true` can still be paired with `memory.accessible=false` when `task_for_pid` is denied, and an attachable menu/login process is still not in-game BWAPI evidence. The production readiness report also emits `runtime-memory-accessible` and a `memory-access` implementation gap when the BWAPI parity contract requires a shared-memory client but the selected runtime process cannot be accessed. Pass `--address <addr> --size <bytes> --require-read` only for an address that has been separately authorized and validated; the default probe does not read arbitrary game memory.

`starcraft-runtime-binary-anchors` is a code-level SC:R porting tool. It scans a target executable for stable ASCII anchors such as `CUnit::sgUnitsMem`, SC:R EUD unit/bullet adapter RTTI strings, and `CBullet: Damage`, maps Mach-O file offsets to VM addresses on macOS, prints all duplicate anchor occurrences plus enclosing C-string start addresses, and prints RIP-relative/pointer xref candidates that can be turned into validated runtime binding proofs. This is static analysis evidence only; a binding still must be verified against the live process before it can remove a production gap.

`starcraft-runtime-adapter-proof --unit-candidate-address <addr>` validates explicit CUnit array candidates before broad memory scans. Use it with addresses derived from binary or live memory analysis to avoid treating menu-state heuristic scans as production evidence; the proof still emits `proof.read_units=passed` only when the candidate or fallback scan contains enough active BWAPI-compatible unit records. Broad unit scans skip regions mapped from the selected StarCraft executable by default because static image/cstring mappings produce high false-positive rates; pass `--unit-scan-include-image-regions` only when deliberately auditing executable-image data. Add `--state-scan-diagnostics` and `--unit-scan-diagnostics` to print scan coverage, timeout, byte-limit, image-skip, candidate, and best-candidate counters. State-counter scans now chunk through readable non-executable StarCraft target and anonymous runtime regions so a counter deeper than the first 4 MiB of a large SC:R mapping is not silently skipped. Explicit frame-counter addresses in non-StarCraft file-backed mappings are rejected because framework/runtime helper fields can move without representing BW game state.

`starcraft-runtime-adapter-proof --prove-read-map-data` scans live memory for installed map names/paths and falls back to fresh replay artifacts when the live string proof is unavailable. Set `STARCRAFT_API_REPLAY_DIR` to override replay discovery. A replay fallback ready file records `proof.read_map_data.source=latest-replay-artifact`, `proof.read_map_data.replay_path`, and `proof.read_map_data.replay_file_size`; this is map/replay metadata evidence only and does not satisfy active-match, command, overlay, or multiplayer proof requirements by itself. Replay-analysis proof can use this metadata only when the selected StarCraft process is either running that replay or has already proven active-match player metadata from live unit snapshots.

`starcraft-runtime-adapter-proof --prove-read-player-data` can project BWAPI-facing `BW::PlayerInfo` and `BW::BWGame.alliance` fields from a passing live SC:R unit-node snapshot. The ready file marks this as `compat-player-projection-v1:unit-snapshot-derived`, not as a discovered native SC:R `PlayerInfo` array. Resource and supply slots are present in the projection so the BWAPI facade has a stable layout, but they remain weaker evidence than a future versioned native resource/supply binding. Unit/player read proofs are recorded when their own live snapshots pass; `proof.active_match_state=passed` remains a separate stricter behavior proof and is not inferred from those read-side bindings.

`starcraft-runtime-adapter-proof --discover-command-queue` scans readable+writable live memory for command-queue-like vector candidates and writes `command_queue.candidates.tsv` plus `proof.command_queue_discovery=*` metadata into the bridge. Sliding-window vector false positives over an accepted `{begin,end,capacity}` triple are suppressed, and live dynamic-vector candidates do not need to be exactly 512 bytes as long as they have enough bounded capacity for a safe tail append attempt. Raw turn-buffer discovery now also samples anonymous/private writable regions when the candidate has non-zero used bytes, so heap-backed SC:R command buffers are not hidden behind executable-image false positives. This is discovery-only evidence: it does not write `proof.issue_commands=passed`, does not resolve `BW::BWDATA::sgdwBytesInCmdQueue`/`TurnBuffer`, and does not make `starcraft-runtime-submit-command` accept commands. If an explicit command candidate fails behavior proof, the bridge records `diagnostic.issue_commands.snapshot=issue_commands.snapshot.tsv` and the failure reason without promoting the candidate. Add `--issue-command-candidate-scan-limit <count>` only during authorized live debugging to try the top discovered candidates sequentially; the snapshot records each attempt, but failed attempts remain negative evidence. Promote a candidate only after a resident adapter proves that encoded BWAPI payloads are consumed by the live SC:R command path and produce in-game behavior.

During live command debugging, `--prove-issue-commands` now samples discovered candidates for natural queue activity before ranking them. Use `--command-queue-activity-ms <ms>` to lengthen that observation window when a human or test harness is producing in-game input, and `--command-queue-max-scan-mb <mb>` to tune command-queue discovery independently from unit scans. Use `--command-queue-candidate-limit <count>` with `--discover-command-queue` for read-only triage when you need to retain more than the default top 32 candidates without writing command bytes into the target process. Empty fixed-size raw buffers and overlapping vector triples are deprioritized because they created high-scoring false positives in SC:R memory. Without an explicit `--command-queue-vector-address`, the live proof refuses to write to candidates that showed no natural activity; raw turn-buffer-shaped memory is no longer enough for implicit writes. High-entropy raw buffers that behave like large state storage are excluded from implicit writes even if their count-like field changes during input. `command_queue.candidates.tsv` records `counter_offset`, prefix byte/entropy/opcode/pointer-table diagnostics, `region_class`, selector/count before/after bytes, buffer prefix before/after bytes, changed byte totals/ranges, `implicit_write_eligible`, `live_write_safe`, and the reason for each retained candidate. Activity columns are still diagnostic only; a candidate still needs delivery plus behavior proof before production command gaps can close.

Command discovery also prints vector/raw candidate totals plus retained vector/raw/active counts. These numbers make it clear when raw turn-buffer-like memory is being found but not validated as the live command sink. `issue_commands.snapshot.tsv` records whether a pause-frame-counter sample was actually read and whether it matched the strict pause condition; a zero delta is not promoted unless the matching resume behavior is also proven.

After any live command delivery attempt, `starcraft-runtime-adapter-proof` waits 10 seconds and rechecks that the target StarCraft process is still visible before writing the ready file. A candidate that accepts bytes but destabilizes the runtime remains diagnostic failure evidence and must not be promoted to `proof.issue_commands=passed`. Live command proof also refuses to write to std::vector selectors stored in the target executable image; those image-mapped vectors can expose active engine state but are not safe BW turn-buffer sinks. Failed pause/resume proofs restore both resume and original pause appends when those proof bytes are still present, and manual `--append-game-action` diagnostics with `--state-counter-address` restore the append if the tracked frame counter does not resume progressing.

`--prove-multiplayer-sync` inspects the target process command line on macOS/Linux and marks replay launches such as `playReplay` as replay-only diagnostics. Replay-analysis evidence derived from active-match live metadata is reported separately as `active-match-live-metadata`, not as `replay_only`. A replay can prove read/event/replay metadata, but it cannot satisfy Battle.net multiplayer synchronization proof.

`starcraft-runtime-input` is a local macOS test helper for sending bounded keyboard input to one selected StarCraft process. Use `--post-timeout-ms <ms>` during live debugging so a blocked macOS event post fails cleanly instead of leaving a stuck helper process. Codex, Accessibility, and keyboard automation are not runtime dependencies for the StarCraft API adapter; they are only optional local test-control plumbing and do not satisfy BWAPI parity proof by themselves.

On macOS local development builds, run `cmake --build build --target starcraft-runtime-sign-debug-tools` to ad-hoc sign the runtime diagnostic tools with debugger entitlements from `tools/macos-debugger.entitlements`. This enables authorized local attach tests on systems that allow debugger-signed tools. Then use `starcraft-runtime-memory-probe --read-first-readable --require-open --require-access --require-read` to prove real target-process VM reads without hard-coding an address.

When using a bootstrap manifest, pass the runtime identity explicitly so the report attributes gaps to StarCraft Remastered instead of an unknown runtime:

```sh
build/starcraft-runtime-gap-report \
  --manifest /tmp/starcraft-api-local-bootstrap.manifest \
  --product starcraft-remastered \
  --version <version> \
  --executable "$STARCRAFT_API_EXECUTABLE" \
  --bridge /tmp/starcraft-api-local-bridge \
  --evidence-out /tmp/starcraft-api-local-gap.evidence
```

Command submission is manifest-backed for StarCraft: Remastered and requires a validated runtime adapter bridge. The CLI rejects command submission when the manifest is missing, incomplete, does not declare the BWAPI command/action being submitted, or the bridge is only a launch/attach bootstrap bridge:

```sh
build/starcraft-runtime-submit-command \
  --product starcraft-remastered \
  --version <version> \
  --process-id <pid> \
  --executable "$STARCRAFT_API_EXECUTABLE" \
  --manifest /path/to/validated-remastered.manifest \
  --bridge /path/to/authorized-runtime-bridge \
  --game-action pauseGame
```

When a live command receiver is serving a validated adapter bridge, turn-buffer-backed commands are encoded and appended to the proven runtime command queue. Adapter-local BWAPI actions such as `drawBox`, `vDrawText`, `setGUI`, `setFrameSkip`, and `setCommandOptimizationLevel` are not written into the StarCraft turn buffer; the receiver records them in `adapter.local-actions.tsv` and audits them as `adapter-local` in `commands.applied.tsv`. This prevents command queue corruption while keeping the BWAPI facade command stream complete. It is still not overlay-rendering evidence by itself; `proof.draw_overlays=passed` remains required before production readiness can pass.

The bridge written by `starcraft-runtime-launch --bridge` is `mode=launch-attach-bootstrap` and includes the selected `process_id` and `executable`. It is only bootstrap/plumbing evidence and cannot satisfy production readiness. `starcraft-runtime-adapter-proof` writes `proof.attach=passed` only after both process identity and actual process-memory access are available. It may preserve earlier passing `proof.*` and `contract.*` lines only when protocol, product, version, process id, executable identity, validated mode, and process visibility still match; proof keys requested by the current run are invalidated before rewrite, so a failed recheck cannot keep stale success. A production executor must publish matching runtime identity, `mode=validated-runtime-adapter`, and behavior proof lines for attach, active match/replay state, game-state reads, unit reads, command issue, overlay drawing, event dispatch, replay analysis, multiplayer sync, Battle.net policy validation, and AI module loading.

## BWAPI Reference

BWAPI is used as a compatibility reference for the public API shape, command vocabulary, and legacy StarCraft: Brood War 1.16.1 behavior.

This project is not presenting upstream BWAPI authors as contributors to this StarCraft: Remastered adaptation. The BWAPI code and concepts are acknowledged as reference material and compatibility baseline only.

Reference: https://github.com/bwapi/bwapi

## Contributor

- minsing-jin

## Legal

StarCraft, StarCraft: Brood War, StarCraft: Remastered, Battle.net, and Blizzard Entertainment names are trademarks of Blizzard Entertainment.
