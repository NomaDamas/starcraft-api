# Local Runtime Files

Place a local StarCraft: Brood War Win32 base install here when using legacy
mode:

```text
runtime/bw1161/StarCraft.exe
runtime/bw1161/StarDat.mpq
runtime/bw1161/BrooDat.mpq
runtime/bw1161/*.dll
```

Then validate it:

```sh
build/starcraft-runtime-legacy1161-setup \
  --dir "$PWD/runtime/bw1161" \
  --write-env runtime/bw1161.env \
  --write-manifest runtime/bw1161.manifest \
  --require-launchable
```

`runtime/bw1161/`, generated env files, and generated manifests are ignored by git.

The setup script can fetch Blizzard-hosted `SC-1161.exe` and `BW-1161.exe`
patchers into `runtime/downloads/blizzard-official/` automatically. It verifies
the fixed SHA256, size, and PE32/i386 type of both patchers before running
anything. Those patchers are not a full game install; the script still needs
one of:

```sh
STARCRAFT_API_BW_BASE_DIR=/path/to/existing/windows/starcraft-broodwar-install
STARCRAFT_API_BW_INSTALLER=/path/to/windows/starcraft-installer.exe
```

`STARCRAFT_API_BW_BASE_DIR` must contain a Brood War install with
`StarCraft.exe` or `Brood War.exe`, plus `StarDat.mpq` and `BrooDat.mpq`.
The script rejects patch executables and `Battle.net-Setup.exe` when they are
passed as a base installer.
