# Repository Agent Rules

This repository adapts the BWAPI-compatible public API surface to StarCraft:
Remastered. Do not claim production readiness unless the runtime gap report
prints `readiness.production_ready=true`.

## Parallel Agent Policy

- Parallel agents are allowed only for independent tasks with disjoint write
  ownership.
- Before spawning parallel work, compute a safe budget:

```sh
python3 tools/guarded_parallel.py --print-budget --per-job-mb 2048 --min-free-mb 4096 --max-jobs 4
```

- Do not exceed the printed `recommended_jobs`.
- Close completed agents when their result has been integrated.
- The main agent owns live StarCraft/Battle.net process control. Subagents must
  not launch, kill, attach to, or inject into StarCraft unless explicitly
  delegated that single responsibility.
- Keep write scopes explicit. Example: one worker owns runtime bridge files,
  another owns tests, another owns documentation.
- Do not run multiple live SC:R/Battle.net instances to create parallelism.
- If `recommended_jobs=0`, do not spawn new agents or parallel commands.

## Production Evidence Rules

- `proof.issue_commands=passed` requires observed live SC:R command-path
  behavior, not resident queue ingress only.
- `proof.draw_overlays=passed` requires visible-frame render-hook evidence, not
  adapter-local draw logging only.
- `proof.multiplayer_sync=passed` requires live send/receive synchronization
  evidence, not replay/local command delivery.
- `proof.dispatch_events=passed` requires resident-observed, frame-correlated
  BWAPI event dispatch behavior, not snapshot diffs or helper-process callbacks
  alone.
- Bootstrap bridges, fixture manifests, static anchors, dry-run encoders, and
  menu/login process activity are not production proof.

## Validation

Compute the memory-safe job budget before publishing code changes:

```sh
python3 tools/guarded_parallel.py --print-budget --per-job-mb 2048 --min-free-mb 4096 --max-jobs 4
```

If `recommended_jobs=0`, do not start build/test jobs. Otherwise run build and
test sequentially, using the recommended job count for the build:

```sh
cmake --build build --parallel <recommended_jobs>
ctest --test-dir build --output-on-failure
git diff --check
```

## Issue and PR Workflow

- Track Mac SC:R parity through Issues #2-#12 plus #15 and
  `docs/remastered-bwapi-production-execution.md`.
- One PR should close one issue unless the PR is a small prerequisite governance
  or validation fix.
- Every PR must state touched proof lines, forbidden proof lines, validation
  commands, live-evidence status, and the exact reason production readiness does
  or does not change.
- Any PR that promotes or preserves a production `proof.*=passed` line must
  include linked issue AC status, allowed/forbidden ready-line diff,
  gap-report before/after, live evidence or explicit non-production status, and
  negative tests for fake proof.
- Require a devil's-advocate review before merge. The review must look for fake
  proof, stale runtime identity, fixture leakage, missing negative tests, local
  PID/path hard-coding, and BWAPI API surface loss.
