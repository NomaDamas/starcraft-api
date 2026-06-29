# Legacy 1.16.1 Official Download Research

Checked on 2026-06-29.

## Target

BWAPI-compatible legacy mode needs a Windows 32-bit StarCraft/Brood War runtime
that can be patched to 1.16.1. The required runtime artifact is an installed
game directory containing files such as:

```text
StarCraft.exe
Brood War.exe
StarDat.mpq
BrooDat.mpq
```

## Verified Blizzard/Battle.net Endpoints

### Battle.net download page

```sh
curl -I -L 'https://download.battle.net/?product=s1&os=win'
curl -I -L 'https://download.battle.net/?product=s1&os=mac'
```

Both requests redirect to the Battle.net Desktop App page, not a StarCraft
1.16.1 installer.

### Battle.net installer endpoint

```sh
curl -I -L 'https://downloader.battle.net/download/getInstallerForGame?os=win&gameProgram=S1&version=Live'
curl -I -L 'https://downloader.battle.net/download/getInstallerForGame?os=win&gameProgram=STARCRAFT&version=Live'
curl -I -L 'https://downloader.battle.net/download/getInstallerForGame?os=win&gameProgram=STARCRAFT_REMASTERED&version=Live'
```

These return the Battle.net App installer:

```text
https://downloader.battle.net/download/installer/win/1.0.66/Battle.net-Setup.exe
```

### NGDP product metadata

```sh
curl -sS http://us.patch.battle.net:1119/s1/versions
curl -sS http://us.patch.battle.net:1119/s1/cdns
```

The public `s1` product currently points at StarCraft: Remastered/current builds,
for example `1.23.10.13515`, not Brood War `1.16.1`.

### Blizzard-hosted 1.16.1 patchers

```sh
curl -I -L 'http://ftp.blizzard.com/pub/starcraft/patches/PC/SC-1161.exe'
curl -I -L 'http://ftp.blizzard.com/pub/broodwar/patches/PC/BW-1161.exe'
```

Both endpoints return `200 OK` PE32 patch executables. These are patchers, not a
full base game install.

Downloaded copies are cached locally by `scripts/setup_legacy1161_runtime.sh`
under:

```text
runtime/downloads/blizzard-official/
```

The cache is intentionally ignored by git.
The setup script verifies the cached patchers by fixed SHA256, exact byte size,
and PE32/i386 type before running them.

## Current Blocker

No public Blizzard/Battle.net endpoint found in this pass provides a full
Windows StarCraft/Brood War base installer for the 1.16.1 line.

To finish local setup, provide one of these inputs:

```sh
STARCRAFT_API_BW_BASE_DIR=/path/to/existing/windows/starcraft-broodwar-install
STARCRAFT_API_BW_INSTALLER=/path/to/windows/starcraft-installer.exe
```

If Blizzard supplied a separate installer URL, download it to
`runtime/downloads/blizzard-official/` and pass that local file as
`STARCRAFT_API_BW_INSTALLER`.
