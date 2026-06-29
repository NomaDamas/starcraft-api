# Legacy Brood War 1.16.1 Runtime Plan

## Boundary

BWAPI was built for StarCraft: Brood War 1.16.1 as a Windows 32-bit process. This project now treats that target as a first-class runtime product:

- Windows: launch/attach the Win32 `StarCraft.exe` directly.
- macOS/Linux: launch/attach the same Win32 runtime through Wine or CrossOver.
- StarCraft: Remastered: remains a separate product and does not share the 1.16.1 address/layout assumptions.

The legacy path is closer to upstream BWAPI because the game version, process bitness, address model, command queue model, and DLL injection assumptions match the original design.

## Local Runtime Inputs

Supported selectors:

- `STARCRAFT_API_BW1161_DIR`: directory containing `StarCraft.exe` or `Brood War.exe`.
- `STARCRAFT_API_BW1161_EXE`: explicit Win32 executable path.
- `STARCRAFT_API_WINE`: Wine executable for macOS/Linux.
- `STARCRAFT_API_WINEPREFIX`: optional Wine prefix.

Validation performed by `starcraft-runtime-legacy1161-setup`:

- Confirms the selected executable exists.
- Confirms the executable is PE32/i386 (`MZ`, `PE\0\0`, machine `0x014c`, optional header `0x010b`).
- Confirms the base install includes Brood War data files (`StarDat.mpq` and `BrooDat.mpq`) before patching.
- Verifies Blizzard-hosted `SC-1161.exe` and `BW-1161.exe` by fixed size, SHA256, and PE32/i386 type before running them.
- Rejects patch executables and `Battle.net-Setup.exe` when they are accidentally supplied as a base game installer.
- Records version as `1.16.1` for the selected legacy product.
- Records Wine/CrossOver compatibility runtime metadata on macOS/Linux.
- Writes a bootstrap manifest that intentionally does not claim production BWAPI parity.

## Runtime Strategy

The production route is to preserve upstream BWAPI's original in-process model instead of recreating the SC:R adapter:

1. Launch or attach the selected 1.16.1 process.
2. Load the BWAPI-compatible in-game bridge in the same Win32 address space.
3. Publish a validated executor bridge with the selected process id and executable identity.
4. Prove each required BWAPI behavior from inside the active game process.
5. Only then promote production proof lines consumed by `starcraft-runtime-gap-report`.

Required proof lines remain the same parity gates:

- `proof.attach=passed`
- `proof.read_game_state=passed`
- `proof.read_units=passed`
- `proof.read_bullet_data=passed`
- `proof.read_map_data=passed`
- `proof.read_player_data=passed`
- `proof.read_region_data=passed`
- `proof.issue_commands=passed`
- `proof.draw_overlays=passed`
- `proof.dispatch_events=passed`
- `proof.replay_analysis=passed`
- `proof.multiplayer_sync=passed`
- `proof.load_ai_modules=passed`

## Current Implementation

Implemented:

- Product selection for `starcraft-brood-war-1.16.1` / `1161`.
- Local 1.16.1 install detection through `STARCRAFT_API_BW1161_DIR` and `STARCRAFT_API_BW1161_EXE`.
- PE32/i386 executable gate.
- Wine/CrossOver launch target metadata on macOS/Linux.
- Wine command-line process detection.
- Legacy setup CLI with env and bootstrap manifest output.
- Backend probe exposes the full BWAPI API and command surface as the required parity contract while failing closed for production.
- Tests for legacy install detection, Wine process detection, bootstrap manifest output, and setup CLI behavior.

Not yet production proof:

- In-process BWAPI bridge loading under Wine/macOS.
- Live CUnit/BWGame/Bullet/Player/Region structure proof from the target process.
- Live command delivery that changes in-game behavior.
- Draw overlay proof on visible game frames.
- Event dispatch and multiplayer synchronization proof.
- AI module loading proof through the bridge.

## Stop Condition

Do not call the legacy runtime production ready until:

```sh
build/starcraft-runtime-gap-report --product 1161 --require-production
```

returns success and the summary includes:

```text
readiness.production_ready=true
```
