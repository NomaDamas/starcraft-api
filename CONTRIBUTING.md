# Contributing

This repository is maintained as the StarCraft: Remastered adaptation of the BWAPI-compatible public API surface.

## Ownership

- Primary contributor: minsing-jin
- BWAPI is acknowledged as a compatibility reference only.
- Do not add upstream BWAPI maintainers as contributors for this adaptation unless they directly contribute to this repository.

## Development Rules

- Keep the public BWAPI-compatible API surface intact.
- Do not mark StarCraft: Remastered support production-ready without live adapter proof.
- Do not infer in-game behavior from menu/login process attachment.
- Keep macOS, Linux, and Windows build paths compiling when touching runtime code.

## Validation

Before publishing changes, run:

```sh
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
git diff --check
```

For macOS live attach diagnostics, also run:

```sh
cmake --build build --target starcraft-runtime-sign-debug-tools
```
