# Production Execution Plan for Mac SC:R BWAPI Parity

This plan is the issue and PR execution contract for making the BWAPI-compatible
API surface work against StarCraft: Remastered on Battle.net. It does not mark
the project production-ready by itself; only the runtime gap report can do that.

## Non-Negotiable Release Rule

The project is production-ready only when a live Mac SC:R run produces:

```text
readiness.production_ready=true
executor.behavior_proof.missing_count=0
implementation_gap.count=0
```

The release gate is:

```sh
build/starcraft-runtime-gap-report \
  --manifest <live-generated-manifest-outside-tests-fixtures> \
  --product starcraft-remastered \
  --version <version> \
  --process-id <pid> \
  --executable <StarCraft executable> \
  --bridge <bridge> \
  --require-production
```

## Issue Sequence

1. Issue #2: bootstrap, versioned ABI, loader identity, and heartbeat.
   Status: closed. This foundation is plumbing only and must not imply gameplay
   parity.
2. Issue #3: resident `read_game_state` and `active_match_state` proof.
   Exit only after resident-source frame and active-match evidence rejects menu,
   stale bridge, wrong PID, and wrong executable cases.
3. Issue #4: resident units, players, map, regions, and replay metadata proof.
   Exit only after BWAPI projections are schema-versioned, identity-bound, and
   active-match-correlated.
4. Issue #5: bullet data live proof.
   Exit only after projectile records or a resident-proven empty table are
   bound to the selected process and current heartbeat.
5. Issue #6: first real command delivery.
   Exit only after one BWAPI command reaches the live SC:R command path, changes
   behavior, and survives post-delivery frame advancement.
6. Issue #7: full BWAPI command surface parity.
   Exit only after a generated BWAPI command/action surface manifest has every
   public command listed. Production parity counts only per-command
   `live-proven` evidence; mock, fixture, documented scenario, or fail-closed
   entries must keep an implementation gap.
7. Issue #8: Battle.net policy and AI module loading proof.
   Exit only after exactly one target process is selected and AI callback
   loading is proven from the resident path, not helper-process `dlopen`.
8. Issue #15: BWAPI event dispatch live proof.
   Exit only after resident-observed, frame-correlated BWAPI events prove match
   start/end, unit create/destroy/morph, order-relevant transitions, and
   replay-vs-live separation.
9. Issue #9: visible overlay rendering proof.
   Exit only after BWAPI draw primitives render on a visible SC:R game frame
   through a resident render hook or equivalent visible-frame renderer.
10. Issue #10: Battle.net multiplayer synchronization proof.
   Exit only after live send/receive turn synchronization evidence rejects
   replay-only, socket-only, and local-queue-only proofs.
11. Issue #11: production gate hardening and anti-fake-proof tests.
    Exit only after every false-positive class discovered during development is
    covered by a negative test. `--require-production` may pass only for a
    complete live-evidence proof bundle with no fixture, mock, or diagnostic
    markers in production-required fields; synthetic fixtures are parser tests
    only.
12. Issue #12: macOS packaging, signing, live runbook, and release PR.
    Exit only after a clean clone can build, test, run the live proof flow, and
    pass the production gate.

## PR Workflow

- One PR should target one issue unless a small prerequisite fix is required to
  keep validation honest.
- Every PR body must list the issue number, touched proof lines, forbidden proof
  lines, validation commands, and whether live SC:R evidence was collected.
- Every proof-promoting PR must include a linked issue AC checklist,
  allowed/forbidden ready-line diff, gap-report before/after, live evidence or
  explicit non-production status, and negative tests for fake proof.
- Each PR must get a devil's-advocate review before merge. The reviewer should
  look for fake proof, stale identity, fixture leakage, missing negative tests,
  and any reduction of the BWAPI API surface.
- A PR cannot merge if it lowers required capabilities, converts diagnostics
  into production proof, hard-codes a local PID/path, or makes
  `starcraft-runtime-gap-report --require-production` pass with fixture-only
  evidence.
- Final release commands must not use manifests under `tests/fixtures/**`;
  fixture manifests are parser/contract test inputs only.
- After merge, the next issue starts from updated `main`; do not stack hidden
  unreviewed changes across issues.

## Parallel-Agent Guardrail

Parallel work is allowed only when write ownership is disjoint. The main agent
owns live StarCraft/Battle.net process control. Before spawning agents or
parallel local jobs, run:

```sh
python3 tools/guarded_parallel.py --print-budget --per-job-mb 2048 --min-free-mb 4096 --max-jobs 4
```

Do not exceed `recommended_jobs`. If `recommended_jobs=0`, do not spawn new
agents or jobs; reduce memory pressure or continue serially.
