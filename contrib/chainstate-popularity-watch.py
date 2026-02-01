#!/usr/bin/env python3
"""
Watch chainstate with inotify; track last access time per file.
Print the 6 most recently accessed and 6 least recently accessed files whenever
UpdateTip appears in debug.log (or on a fixed interval if --no-tail).
Also report how many files have never been accessed.

Requires: inotify-tools (inotifywait), Python 3.

Usage:
  ./contrib/chainstate-popularity-watch.py ~/.bitcoin/chainstate
  ./contrib/chainstate-popularity-watch.py ~/.bitcoin/chainstate --debug-log /path/to/debug.log
  ./contrib/chainstate-popularity-watch.py ~/.bitcoin/chainstate --no-tail --interval 120
"""

import argparse
import os
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import Dict, Optional

INOTIFYWAIT = shutil.which("inotifywait") or "/usr/bin/inotifywait"

def _format_age(ts: Optional[float]) -> str:
    """Format last access as 'X ago' or 'never'."""
    if ts is None:
        return "never"
    age = time.time() - ts
    if age < 60:
        return f"{int(age)}s ago"
    if age < 3600:
        return f"{int(age / 60)}m ago"
    if age < 86400:
        return f"{age / 3600:.1f}h ago"
    return f"{age / 86400:.1f}d ago"

def seed_file_last_access(chainstate_dir: Path) -> Dict[str, Optional[float]]:
    """Seed with all existing files (relative path -> None = never accessed)."""
    file_last_access = {}
    for root, _dirs, files in os.walk(chainstate_dir):
        for f in files:
            full = Path(root) / f
            try:
                rel = full.relative_to(chainstate_dir)
            except ValueError:
                continue
            file_last_access[str(rel)] = None
    return file_last_access

def report(
    file_last_access: Dict[str, Optional[float]], lock: threading.Lock
) -> None:
    with lock:
        items = list(file_last_access.items())
    if not items:
        print("(no files)")
        return
    # Most recent first: sort by last_access descending (None = -inf)
    by_recent = sorted(
        items, key=lambda x: (x[1] if x[1] is not None else -1.0)
    )
    top6 = by_recent[-6:][::-1]  # 6 with largest timestamp
    accessed = [(r, t) for r, t in by_recent if t is not None]
    bottom6 = accessed[:6]  # 6 least recently accessed (exclude never-accessed)
    never_count = sum(1 for _, v in items if v is None)

    ts = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime())
    print(f"\n[{ts}] --- 6 most recently accessed ---")
    for rel, last_ts in top6:
        print(f"  {_format_age(last_ts):>12}  {rel}")
    print("--- 6 least recently accessed ---")
    for rel, last_ts in bottom6:
        print(f"  {_format_age(last_ts):>12}  {rel}")
    print(f"Files never accessed: {never_count}")
    sys.stdout.flush()

def run_inotify(
    chainstate_dir: Path,
    file_last_access: Dict[str, Optional[float]],
    lock: threading.Lock,
) -> None:
    proc = subprocess.Popen(
        [
            INOTIFYWAIT,
            "-m",
            "-r",
            "-e",
            "access,open,create",
            "--format",
            "%w%f",
            str(chainstate_dir),
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
    )
    for line in proc.stdout:
        line = line.strip()
        if not line:
            continue
        path = Path(line)
        if not path.is_file():
            continue
        try:
            rel = path.relative_to(chainstate_dir)
        except ValueError:
            continue
        if rel.parts and rel.parts[0] == "..":
            continue
        rel_str = str(rel)
        now = time.time()
        with lock:
            file_last_access[rel_str] = now

def tail_debug_log(
    debug_log: Path,
    file_last_access: Dict[str, Optional[float]],
    lock: threading.Lock,
) -> None:
    if not debug_log.exists():
        print(f"debug.log not found: {debug_log}", file=sys.stderr)
        return
    with open(debug_log, "r", encoding="utf-8", errors="replace") as f:
        f.seek(0, 2)
        while True:
            line = f.readline()
            if not line:
                time.sleep(0.2)
                continue
            if "UpdateTip" in line:
                report(file_last_access, lock)


def interval_report(
    interval: float,
    file_last_access: Dict[str, Optional[float]],
    lock: threading.Lock,
) -> None:
    while True:
        time.sleep(interval)
        report(file_last_access, lock)

def main() -> None:
    # Require inotifywait (inotify-tools)
    if not os.path.isfile(INOTIFYWAIT) or not os.access(INOTIFYWAIT, os.X_OK):
        sys.exit(
            "inotifywait not found. Install inotify-tools (e.g. apt install inotify-tools)."
        )

    parser = argparse.ArgumentParser(
        description="Chainstate file last-access: inotify, report on UpdateTip or interval."
    )
    parser.add_argument(
        "chainstate_dir",
        type=Path,
        help="Path to chainstate directory",
    )
    parser.add_argument(
        "--debug-log",
        type=Path,
        default=None,
        help="Path to debug.log (default: chainstate_dir/../debug.log)",
    )
    parser.add_argument(
        "--no-tail",
        action="store_true",
        help="Do not tail debug.log; report on --interval instead",
    )
    parser.add_argument(
        "--interval",
        type=float,
        default=60.0,
        help="If --no-tail, report every N seconds (default: 60)",
    )
    args = parser.parse_args()

    chainstate_dir = args.chainstate_dir.resolve()
    if not chainstate_dir.is_dir():
        sys.exit(f"Not a directory: {chainstate_dir}")

    debug_log = args.debug_log
    if debug_log is None:
        debug_log = chainstate_dir.parent / "debug.log"
    else:
        debug_log = debug_log.resolve()

    file_last_access = seed_file_last_access(chainstate_dir)
    lock = threading.Lock()

    t_inotify = threading.Thread(
        target=run_inotify,
        args=(chainstate_dir, file_last_access, lock),
        daemon=True,
    )
    t_inotify.start()

    if args.no_tail:
        t_interval = threading.Thread(
            target=interval_report,
            args=(args.interval, file_last_access, lock),
            daemon=True,
        )
        t_interval.start()
    else:
        t_tail = threading.Thread(
            target=tail_debug_log,
            args=(debug_log, file_last_access, lock),
            daemon=True,
        )
        t_tail.start()

    t_inotify.join()


if __name__ == "__main__":
    main()
