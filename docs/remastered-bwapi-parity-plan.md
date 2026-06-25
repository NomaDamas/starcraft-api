# BWAPI Parity Path for StarCraft: Remastered Battle.net

The target is full BWAPI-compatible behavior on StarCraft: Remastered without
reusing the original Brood War 1.16.1 fixed-offset injection model.

## Required Architecture

1. Runtime identity and launch

- Discover one selected StarCraft game executable process.
- Bind every bridge ready file to product, version, process id, and executable.
- Reject stale bridges, menu-only activity, fixture manifests, and bootstrap
  launch artifacts as production evidence.

2. In-process resident adapter

- Load a resident adapter into the StarCraft process through an authorized path.
- Publish resident command, overlay, state, event, and proof queues.
- Emit `proof.*=passed` only from resident-observed behavior, not from static
  anchors or external guesses.

3. State model

- Prove active match or replay state separately from generic frame progression.
- Resolve BWAPI-facing projections for game state, units, players, map, bullets,
  regions, events, replay metadata, and AI module loading.
- Replace compatibility projections with native SC:R bindings when proven.

4. Event path

- Dispatch BWAPI events from resident-observed, frame-correlated game-state
  transitions.
- Prove match start/end, unit create/destroy/morph, order-relevant, and
  replay-vs-live event separation before emitting `proof.dispatch_events=passed`.
- Reject helper-process callbacks, stale snapshot diffs, and replay-only event
  evidence as production proof.

5. Command path

- Encode BWAPI unit commands and game actions deterministically.
- Route commands from the API facade to the resident adapter.
- Prove the payload reaches the live SC:R command path and changes frame
  behavior before emitting `proof.issue_commands=passed`.

6. Overlay path

- Queue BWAPI draw primitives in the resident adapter.
- Bind a real render callback or equivalent visible-frame renderer.
- Emit `proof.draw_overlays=passed` only after primitives are observed on a
  visible game frame.

7. Multiplayer sync

- Resolve live send/receive turn synchronization hooks.
- Prove behavior in a real multiplayer/Battle.net path.
- Reject replay-only, local queue, and static network-anchor evidence.

8. Cross-platform support

- macOS: resident dylib path, hardened runtime/debug entitlement diagnostics,
  Battle.net launch handling, and windowed test control.
- Linux: native process/ptrace or supported compatibility-layer adapter path,
  with the same proof contract.
- Windows: DLL/in-process adapter path with the same proof contract.

## Release Gate

Production readiness requires:

```sh
build/starcraft-runtime-gap-report \
  --manifest tests/fixtures/remastered-complete.manifest \
  --product starcraft-remastered \
  --version <version> \
  --process-id <pid> \
  --executable <StarCraft executable> \
  --bridge <bridge> \
  --require-production
```

The only acceptable final state is:

```text
readiness.production_ready=true
executor.behavior_proof.missing_count=0
implementation_gap.count=0
```

## Parallel Work Guardrails

Parallel agents should split by independent proof lanes:

- Resident bridge and queues.
- State/unit/player/map/bullet readers.
- Event dispatch proof.
- Command behavior proof.
- Overlay render proof.
- Multiplayer sync proof.
- Tests and readiness gates.

Before spawning parallel work, compute a memory-safe budget:

```sh
python3 tools/guarded_parallel.py --print-budget --per-job-mb 2048 --min-free-mb 4096 --max-jobs 4
```

Do not exceed `recommended_jobs`. Do not run build and test against the same
build tree concurrently. Build first, then test:

```sh
cmake --build build --parallel <recommended_jobs>
ctest --test-dir build --output-on-failure
```

Only run commands together when they are independent and do not mutate the same
build tree or live StarCraft process. The main agent should own live SC:R
process control.

## Issue and PR Execution

The detailed issue sequence and merge protocol live in
`docs/remastered-bwapi-production-execution.md`. Each parity issue must land via
its own PR, receive devil's-advocate review, and preserve the release rule that
only `readiness.production_ready=true` with zero implementation gaps can be
called production-ready.
