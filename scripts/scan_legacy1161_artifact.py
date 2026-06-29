#!/usr/bin/env python3
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import urllib.error
import urllib.request


OFFICIAL_PATCHERS = {
    "sc-1161.exe": {
        "label": "official-sc-1161-patcher",
        "sha256": "755b4dbe3f8a928831b19bfa975445885b8c1760ffa4e5a795d37e7f02e6c31e",
        "size": 10696135,
    },
    "bw-1161.exe": {
        "label": "official-bw-1161-patcher",
        "sha256": "96890f59b664eb54dbb3be634f2045e70a4a757e87b405ec4aeeb69d50fb7bb1",
        "size": 26497843,
    },
}

EXECUTABLE_SUFFIXES = {
    ".exe",
    ".dll",
    ".scr",
    ".com",
    ".pif",
    ".msi",
    ".bat",
    ".cmd",
    ".ps1",
    ".vbs",
    ".js",
    ".jar",
}


def emit(key, value):
    print(f"{key}={value}")


def read_u16(data, offset):
    if offset + 2 > len(data):
        return None
    return int.from_bytes(data[offset : offset + 2], "little")


def read_u32(data, offset):
    if offset + 4 > len(data):
        return None
    return int.from_bytes(data[offset : offset + 4], "little")


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def parse_pe(path):
    result = {
        "present": False,
        "machine": "unknown",
        "optional_magic": "unknown",
        "pe32_i386": False,
        "reason": "",
    }
    try:
        with path.open("rb") as handle:
            data = handle.read(4096)
    except OSError as exc:
        result["reason"] = f"read-error:{exc}"
        return result

    if len(data) < 0x40 or data[:2] != b"MZ":
        result["reason"] = "missing-mz"
        return result
    pe_offset = read_u32(data, 0x3C)
    if pe_offset is None or pe_offset + 0x1A > len(data):
        result["reason"] = "invalid-pe-offset"
        return result
    if data[pe_offset : pe_offset + 4] != b"PE\0\0":
        result["reason"] = "missing-pe-signature"
        return result

    machine = read_u16(data, pe_offset + 4)
    optional_magic = read_u16(data, pe_offset + 24)
    result["present"] = True
    result["machine"] = f"0x{machine:04x}" if machine is not None else "unknown"
    result["optional_magic"] = (
        f"0x{optional_magic:04x}" if optional_magic is not None else "unknown"
    )
    result["pe32_i386"] = machine == 0x014C and optional_magic == 0x010B
    return result


def case_insensitive_child(root, wanted):
    wanted_lower = wanted.lower()
    try:
        for child in root.iterdir():
            if child.is_file() and child.name.lower() == wanted_lower:
                return child
    except OSError:
        return None
    return None


def collect_executables(root, max_files):
    found = []
    try:
        iterator = root.rglob("*")
        for path in iterator:
            if len(found) >= max_files:
                break
            if path.is_file() and path.suffix.lower() in EXECUTABLE_SUFFIXES:
                found.append(path)
    except OSError:
        pass
    return found


def classify_file(path, size, digest):
    name = path.name.lower()
    if name in OFFICIAL_PATCHERS:
        expected = OFFICIAL_PATCHERS[name]
        if size == expected["size"] and digest == expected["sha256"]:
            return expected["label"], []
        blockers = []
        if size != expected["size"]:
            blockers.append(
                f"official-patcher-size-mismatch:{path.name}:got-{size}:expected-{expected['size']}"
            )
        if digest != expected["sha256"]:
            blockers.append(f"official-patcher-sha256-mismatch:{path.name}")
        return "forged-or-corrupt-official-patcher-name", blockers
    if name == "battle.net-setup.exe":
        return "battle-net-setup-not-legacy-base-installer", [
            "battle-net-setup-is-not-legacy-1161-base-installer"
        ]
    if path.suffix.lower() in EXECUTABLE_SUFFIXES:
        return "unknown-executable", []
    return "non-executable-file", []


def virus_total_lookup(digest, api_key):
    request = urllib.request.Request(
        f"https://www.virustotal.com/api/v3/files/{digest}",
        headers={"x-apikey": api_key},
    )
    try:
        with urllib.request.urlopen(request, timeout=20) as response:
            payload = json.loads(response.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        if exc.code == 404:
            return {"status": "not-found"}
        return {"status": f"http-error-{exc.code}"}
    except Exception as exc:
        return {"status": f"error:{exc}"}

    stats = (
        payload.get("data", {})
        .get("attributes", {})
        .get("last_analysis_stats", {})
    )
    return {
        "status": "found",
        "malicious": int(stats.get("malicious", 0)),
        "suspicious": int(stats.get("suspicious", 0)),
        "harmless": int(stats.get("harmless", 0)),
        "undetected": int(stats.get("undetected", 0)),
    }


def run_clamscan(path):
    try:
        completed = subprocess.run(
            ["clamscan", "--no-summary", str(path)],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
    except FileNotFoundError:
        return {"status": "missing"}
    output = (completed.stdout + completed.stderr).strip().replace("\n", " | ")
    return {"status": "clean" if completed.returncode == 0 else "flagged", "output": output}


def scan_file(path, args, prefix):
    blockers = []
    warnings = []
    size = path.stat().st_size
    digest = sha256_file(path)
    classification, classification_blockers = classify_file(path, size, digest)
    blockers.extend(classification_blockers)

    pe = parse_pe(path)
    emit(f"{prefix}.path", path)
    emit(f"{prefix}.type", "file")
    emit(f"{prefix}.size", size)
    emit(f"{prefix}.sha256", digest)
    emit(f"{prefix}.classification", classification)
    emit(f"{prefix}.pe.present", str(pe["present"]).lower())
    emit(f"{prefix}.pe.machine", pe["machine"])
    emit(f"{prefix}.pe.optional_magic", pe["optional_magic"])
    emit(f"{prefix}.pe32_i386", str(pe["pe32_i386"]).lower())

    if path.suffix.lower() in EXECUTABLE_SUFFIXES and not pe["present"]:
        blockers.append(f"executable-extension-without-pe:{path.name}")
    if args.strict_unknown_executables and classification == "unknown-executable":
        blockers.append(f"unknown-executable:{path.name}")

    if args.virustotal:
        vt = virus_total_lookup(digest, os.environ.get("VT_API_KEY", ""))
        emit(f"{prefix}.virustotal.status", vt["status"])
        if vt["status"] == "found":
            emit(f"{prefix}.virustotal.malicious", vt["malicious"])
            emit(f"{prefix}.virustotal.suspicious", vt["suspicious"])
            if vt["malicious"] > 0 or vt["suspicious"] > 0:
                blockers.append(f"virustotal-flagged:{path.name}")
        elif not os.environ.get("VT_API_KEY"):
            blockers.append("virustotal-api-key-missing")

    if args.clamscan:
        av = run_clamscan(path)
        emit(f"{prefix}.clamscan.status", av["status"])
        if "output" in av:
            emit(f"{prefix}.clamscan.output", av["output"])
        if av["status"] == "missing":
            blockers.append("clamscan-missing")
        elif av["status"] == "flagged":
            blockers.append(f"clamscan-flagged:{path.name}")

    return blockers, warnings


def scan_directory(path, args, prefix):
    blockers = []
    warnings = []
    starcraft_exe = (
        case_insensitive_child(path, "StarCraft.exe")
        or case_insensitive_child(path, "Brood War.exe")
        or case_insensitive_child(path, "BroodWar.exe")
    )
    stardat = case_insensitive_child(path, "StarDat.mpq")
    broodat = case_insensitive_child(path, "BrooDat.mpq")
    executables = collect_executables(path, args.max_executables)

    emit(f"{prefix}.path", path)
    emit(f"{prefix}.type", "directory")
    emit(f"{prefix}.base_install.executable", starcraft_exe or "")
    emit(f"{prefix}.base_install.stardat_mpq", str(stardat is not None).lower())
    emit(f"{prefix}.base_install.broodat_mpq", str(broodat is not None).lower())
    ready = starcraft_exe is not None and stardat is not None and broodat is not None
    emit(f"{prefix}.base_install.ready", str(ready).lower())
    emit(f"{prefix}.executable.count", len(executables))

    if args.require_base_install and not ready:
        if starcraft_exe is None:
            blockers.append("base-install-missing-StarCraft.exe-or-Brood-War.exe")
        if stardat is None:
            blockers.append("base-install-missing-StarDat.mpq")
        if broodat is None:
            blockers.append("base-install-missing-BrooDat.mpq")

    for index, executable in enumerate(executables):
        child_prefix = f"{prefix}.executable.{index}"
        child_blockers, child_warnings = scan_file(executable, args, child_prefix)
        blockers.extend(child_blockers)
        warnings.extend(child_warnings)

    if len(executables) >= args.max_executables:
        warnings.append(f"executable-scan-truncated-at-{args.max_executables}")

    return blockers, warnings


def main():
    parser = argparse.ArgumentParser(
        description="Scan legacy StarCraft/Brood War 1.16.1 runtime artifacts before execution."
    )
    parser.add_argument("paths", nargs="+", help="file or directory to inspect")
    parser.add_argument(
        "--require-base-install",
        action="store_true",
        help="fail unless a directory has StarCraft.exe/Brood War.exe, StarDat.mpq, and BrooDat.mpq",
    )
    parser.add_argument(
        "--strict-unknown-executables",
        action="store_true",
        help="fail on executable files that are not known official patchers",
    )
    parser.add_argument(
        "--virustotal",
        action="store_true",
        help="query VirusTotal by SHA256 only; requires VT_API_KEY and never uploads files",
    )
    parser.add_argument(
        "--clamscan",
        action="store_true",
        help="run local clamscan if installed and fail if it is missing or flags a file",
    )
    parser.add_argument(
        "--max-executables",
        type=int,
        default=128,
        help="maximum executable-like files to hash in each directory",
    )
    args = parser.parse_args()

    all_blockers = []
    all_warnings = []
    for index, raw_path in enumerate(args.paths):
        path = Path(raw_path)
        prefix = f"artifact.{index}"
        if not path.exists():
            emit(f"{prefix}.path", path)
            emit(f"{prefix}.exists", "false")
            all_blockers.append(f"missing:{path}")
            continue

        if path.is_dir():
            blockers, warnings = scan_directory(path, args, prefix)
        else:
            blockers, warnings = scan_file(path, args, prefix)
        all_blockers.extend(blockers)
        all_warnings.extend(warnings)

    for index, warning in enumerate(all_warnings):
        emit(f"warning.{index}", warning)
    for index, blocker in enumerate(all_blockers):
        emit(f"blocker.{index}", blocker)
    emit("scan.blocking_issue_count", len(all_blockers))
    emit("scan.verdict", "blocked" if all_blockers else "review-passed")
    return 2 if all_blockers else 0


if __name__ == "__main__":
    sys.exit(main())
