#!/usr/bin/env python3
"""Run independent commands with dynamic memory-based concurrency limits."""

from __future__ import annotations

import argparse
import collections
import ctypes
import json
import math
import os
import platform
import shlex
import subprocess
import sys
import time
from dataclasses import dataclass
from typing import Deque, Iterable, List, Optional, Sequence, Tuple


@dataclass
class MemorySnapshot:
    total_mb: int
    available_mb: int
    source: str


@dataclass
class RunningJob:
    index: int
    command: str
    process: subprocess.Popen


def _run_text(argv: Sequence[str]) -> str:
    try:
        completed = subprocess.run(
            argv,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
        )
    except OSError:
        return ""
    if completed.returncode != 0:
        return ""
    return completed.stdout


def _linux_memory() -> Optional[MemorySnapshot]:
    try:
        values = {}
        with open("/proc/meminfo", "r", encoding="utf-8") as handle:
            for line in handle:
                parts = line.split()
                if len(parts) >= 2:
                    values[parts[0].rstrip(":")] = int(parts[1])
        total_kb = values.get("MemTotal")
        available_kb = values.get("MemAvailable")
        if total_kb and available_kb:
            return MemorySnapshot(total_kb // 1024, available_kb // 1024, "/proc/meminfo")
    except OSError:
        return None
    return None


def _macos_memory() -> Optional[MemorySnapshot]:
    total_text = _run_text(["sysctl", "-n", "hw.memsize"]).strip()
    vm_stat = _run_text(["vm_stat"])
    if not vm_stat:
        return None

    try:
        total_mb = int(total_text) // (1024 * 1024) if total_text else 0
    except ValueError:
        total_mb = 0

    page_size = 4096
    available_pages = 0
    for raw_line in vm_stat.splitlines():
        line = raw_line.strip().rstrip(".")
        if line.startswith("Mach Virtual Memory Statistics:"):
            marker = "page size of "
            if marker in line:
                try:
                    page_size = int(line.split(marker, 1)[1].split()[0])
                except (ValueError, IndexError):
                    page_size = 4096
            continue

        name, separator, value_text = line.partition(":")
        if not separator:
            continue
        normalized = name.lower()
        if normalized not in {
            "pages free",
            "pages inactive",
            "pages speculative",
        }:
            continue
        try:
            available_pages += int(value_text.strip().replace(".", ""))
        except ValueError:
            continue

    if available_pages <= 0:
        return None
    return MemorySnapshot(
        total_mb,
        (available_pages * page_size) // (1024 * 1024),
        "vm_stat",
    )


class _MemoryStatusEx(ctypes.Structure):
    _fields_ = [
        ("dwLength", ctypes.c_ulong),
        ("dwMemoryLoad", ctypes.c_ulong),
        ("ullTotalPhys", ctypes.c_ulonglong),
        ("ullAvailPhys", ctypes.c_ulonglong),
        ("ullTotalPageFile", ctypes.c_ulonglong),
        ("ullAvailPageFile", ctypes.c_ulonglong),
        ("ullTotalVirtual", ctypes.c_ulonglong),
        ("ullAvailVirtual", ctypes.c_ulonglong),
        ("sullAvailExtendedVirtual", ctypes.c_ulonglong),
    ]


def _windows_memory() -> Optional[MemorySnapshot]:
    try:
        status = _MemoryStatusEx()
        status.dwLength = ctypes.sizeof(status)
        if not ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status)):
            return None
        return MemorySnapshot(
            int(status.ullTotalPhys // (1024 * 1024)),
            int(status.ullAvailPhys // (1024 * 1024)),
            "GlobalMemoryStatusEx",
        )
    except Exception:
        return None


def memory_snapshot() -> MemorySnapshot:
    system = platform.system().lower()
    snapshot: Optional[MemorySnapshot]
    if system == "linux":
        snapshot = _linux_memory()
    elif system == "darwin":
        snapshot = _macos_memory()
    elif system == "windows":
        snapshot = _windows_memory()
    else:
        snapshot = None

    if snapshot is not None:
        return snapshot

    page_size = _sysconf_int("SC_PAGE_SIZE", 4096)
    total_pages = _sysconf_int("SC_PHYS_PAGES", 0)
    available_pages = _sysconf_int("SC_AVPHYS_PAGES", 0)
    if total_pages > 0 and available_pages > 0:
        return MemorySnapshot(
            (total_pages * page_size) // (1024 * 1024),
            (available_pages * page_size) // (1024 * 1024),
            "sysconf",
        )

    return MemorySnapshot(0, 0, "unknown")


def _sysconf_int(name: str, default: int) -> int:
    if not hasattr(os, "sysconf"):
        return default
    try:
        value = os.sysconf(name)
    except (OSError, ValueError):
        return default
    return int(value) if value > 0 else default


def recommended_jobs(snapshot: MemorySnapshot, max_jobs: int, per_job_mb: int, min_free_mb: int) -> int:
    if max_jobs <= 0:
        return 0
    if per_job_mb <= 0:
        return max_jobs
    usable_mb = snapshot.available_mb - min_free_mb
    if usable_mb <= 0:
        return 0
    return max(0, min(max_jobs, int(math.floor(usable_mb / per_job_mb))))


def load_jobs(job_args: Iterable[str], job_file: Optional[str]) -> List[str]:
    jobs = [job.strip() for job in job_args if job.strip()]
    if job_file:
        with open(job_file, "r", encoding="utf-8") as handle:
            for raw_line in handle:
                line = raw_line.strip()
                if line and not line.startswith("#"):
                    jobs.append(line)
    return jobs


def spawn_job(index: int, command: str, use_shell: bool) -> RunningJob:
    argv_or_command: Sequence[str] | str
    if use_shell:
        argv_or_command = command
    else:
        argv_or_command = shlex.split(command)
        if not argv_or_command:
            raise ValueError("empty command")

    print(f"[guarded-parallel] start job={index} command={command}", flush=True)
    process = subprocess.Popen(argv_or_command, shell=use_shell)
    return RunningJob(index=index, command=command, process=process)


def terminate_newest(running: List[RunningJob]) -> None:
    if not running:
        return
    newest = running[-1]
    if newest.process.poll() is None:
        print(
            f"[guarded-parallel] terminate job={newest.index} reason=critical-memory",
            flush=True,
        )
        newest.process.terminate()


def run_jobs(args: argparse.Namespace) -> int:
    jobs = load_jobs(args.job, args.job_file)
    if not jobs:
        print("[guarded-parallel] no jobs supplied", file=sys.stderr)
        return 2

    pending: Deque[Tuple[int, str]] = collections.deque(enumerate(jobs, start=1))
    running: List[RunningJob] = []
    failures: List[Tuple[int, int, str]] = []
    zero_budget_since: Optional[float] = None

    while pending or running:
        still_running: List[RunningJob] = []
        for job in running:
            code = job.process.poll()
            if code is None:
                still_running.append(job)
                continue
            print(f"[guarded-parallel] finish job={job.index} exit={code}", flush=True)
            if code != 0:
                failures.append((job.index, code, job.command))
        running = still_running

        if failures and args.stop_on_failure:
            pending.clear()

        snapshot = memory_snapshot()
        budget = recommended_jobs(snapshot, args.max_jobs, args.per_job_mb, args.min_free_mb)
        now = time.monotonic()
        if pending and not running and budget <= 0:
            if not args.wait_for_memory:
                print(
                    "[guarded-parallel] insufficient_memory "
                    f"available_mb={snapshot.available_mb} min_free_mb={args.min_free_mb} "
                    f"per_job_mb={args.per_job_mb}",
                    file=sys.stderr,
                )
                return 3
            if zero_budget_since is None:
                zero_budget_since = now
            elif args.max_wait_seconds >= 0 and now - zero_budget_since >= args.max_wait_seconds:
                print(
                    "[guarded-parallel] memory_wait_timeout "
                    f"available_mb={snapshot.available_mb} min_free_mb={args.min_free_mb} "
                    f"per_job_mb={args.per_job_mb} max_wait_seconds={args.max_wait_seconds}",
                    file=sys.stderr,
                )
                return 3
        else:
            zero_budget_since = None

        if (
            args.terminate_on_critical
            and running
            and snapshot.available_mb > 0
            and snapshot.available_mb < args.critical_free_mb
        ):
            terminate_newest(running)

        while pending and len(running) < budget:
            index, command = pending.popleft()
            try:
                running.append(spawn_job(index, command, args.shell))
            except (OSError, ValueError) as error:
                print(f"[guarded-parallel] failed_to_start job={index} reason={error}", file=sys.stderr)
                failures.append((index, 127, command))
                if args.stop_on_failure:
                    pending.clear()
                    break

        if pending and not running and budget <= 0:
            print(
                "[guarded-parallel] waiting_for_memory "
                f"available_mb={snapshot.available_mb} min_free_mb={args.min_free_mb} "
                f"per_job_mb={args.per_job_mb}",
                flush=True,
            )

        if pending or running:
            time.sleep(args.poll_seconds)

    if failures:
        for index, code, command in failures:
            print(
                f"[guarded-parallel] failed job={index} exit={code} command={command}",
                file=sys.stderr,
            )
        return 1
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--job", action="append", default=[], help="command to run; repeatable")
    parser.add_argument("--job-file", help="file containing one command per non-comment line")
    parser.add_argument("--max-jobs", type=int, default=min(os.cpu_count() or 1, 4))
    parser.add_argument("--per-job-mb", type=int, default=2048)
    parser.add_argument("--min-free-mb", type=int, default=4096)
    parser.add_argument("--critical-free-mb", type=int, default=1024)
    parser.add_argument("--poll-seconds", type=float, default=2.0)
    parser.add_argument("--stop-on-failure", action="store_true")
    parser.add_argument("--shell", action="store_true", help="run jobs through the platform shell")
    parser.add_argument("--terminate-on-critical", action="store_true")
    parser.add_argument("--wait-for-memory", action="store_true", help="wait for memory instead of failing immediately")
    parser.add_argument("--max-wait-seconds", type=float, default=60.0)
    parser.add_argument("--print-budget", action="store_true")
    parser.add_argument("--json", action="store_true", help="print budget as JSON")
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)

    snapshot = memory_snapshot()
    budget = recommended_jobs(snapshot, args.max_jobs, args.per_job_mb, args.min_free_mb)
    if args.print_budget:
        payload = {
            "memory_source": snapshot.source,
            "total_mb": snapshot.total_mb,
            "available_mb": snapshot.available_mb,
            "max_jobs": args.max_jobs,
            "per_job_mb": args.per_job_mb,
            "min_free_mb": args.min_free_mb,
            "recommended_jobs": budget,
        }
        if args.json:
            print(json.dumps(payload, sort_keys=True))
        else:
            for key, value in payload.items():
                print(f"{key}={value}")
        return 0

    return run_jobs(args)


if __name__ == "__main__":
    raise SystemExit(main())
