#!/usr/bin/env python3
"""
Track chainstate file last-access time; print the 6 most recently and 6 least
recently accessed files whenever UpdateTip appears in debug.log (or on a fixed
interval if --no-tail). Also report how many files have never been accessed.

By default uses filesystem atime (stat) - no extra deps. Use --inotify to use
inotify-tools instead (requires: apt install inotify-tools).

Usage:
  ./contrib/chainstate-popularity-watch.py ~/.bitcoin/chainstate
  ./contrib/chainstate-popularity-watch.py ~/.bitcoin/chainstate --inotify
  ./contrib/chainstate-popularity-watch.py ~/.bitcoin/chainstate --no-tail --interval 120
"""

import argparse
import os
import select
import shutil
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import Callable, Dict, Optional

shutdown = threading.Event()

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

def scan_atime(chainstate_dir: Path) -> list:
    """
    Scan chainstate dir; return [(rel_path, last_activity, is_never_accessed), ...].
    last_activity = max(atime, mtime, ctime) for all files.
    is_never_accessed = atime <= mtime+1 (heuristic: never read after creation).
    """
    items = []
    for root, _dirs, files in os.walk(chainstate_dir):
        for f in files:
            full = Path(root) / f
            try:
                rel = full.relative_to(chainstate_dir)
            except ValueError:
                continue
            try:
                st = full.stat()
                atime, mtime, ctime = st.st_atime, st.st_mtime, st.st_ctime
                last_activity = max(atime, mtime, ctime)
                is_never_accessed = atime <= mtime + 1
                items.append((str(rel), last_activity, is_never_accessed))
            except OSError:
                continue
    return items


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

def report_from_items(items: list) -> None:
    """items: from scan_atime [(rel, last_activity, is_never_accessed), ...]
    or from inotify [(rel, last_ts), ...] with last_ts None if never accessed.
    """
    if not items:
        print("(no files)")
        return
    # Handle atime format (3-tuple) vs inotify format (2-tuple)
    if len(items[0]) == 3:
        never_count = sum(1 for _x in items if _x[2])
        by_recent = sorted(items, key=lambda x: x[1])
        top6 = [(r, t) for r, t, _ in by_recent[-6:][::-1]]
        accessed = [(r, t) for r, t, never in by_recent if not never]
        bottom6 = accessed[:6]
    else:
        never_count = sum(1 for _, v in items if v is None)
        by_recent = sorted(
            items, key=lambda x: (x[1] if x[1] is not None else -1.0)
        )
        top6 = by_recent[-6:][::-1]
        accessed = [(r, t) for r, t in by_recent if t is not None]
        bottom6 = accessed[:6]
    ts = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime())
    print(f"\n[{ts}] --- 6 most recently accessed ---")
    for rel, last_ts in top6:
        print(f"  {_format_age(last_ts):>12}  {rel}")
    print("--- 6 least recently accessed ---")
    for rel, last_ts in bottom6:
        print(f"  {_format_age(last_ts):>12}  {rel}")
    print(f"Files never accessed: {never_count}")
    sys.stdout.flush()


def report_atime(chainstate_dir: Path) -> None:
    """Report using filesystem atime (no inotify)."""
    report_from_items(scan_atime(chainstate_dir))


def report_inotify(
    file_last_access: Dict[str, Optional[float]], lock: threading.Lock
) -> None:
    """Report using inotify-maintained dict."""
    with lock:
        items = list(file_last_access.items())
    report_from_items(items)

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
    while not shutdown.is_set():
        r, _, _ = select.select([proc.stdout], [], [], 0.5)
        if not r:
            continue
        line = proc.stdout.readline()
        if not line:
            break
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
    try:
        proc.terminate()
    except OSError:
        pass

def tail_debug_log(debug_log: Path, on_report: "Callable[[], None]") -> None:
    if not debug_log.exists():
        print(f"debug.log not found: {debug_log}", file=sys.stderr)
        return
    with open(debug_log, "r", encoding="utf-8", errors="replace") as f:
        f.seek(0, 2)
        while not shutdown.is_set():
            line = f.readline()
            if not line:
                shutdown.wait(timeout=0.2)
                continue
            if "UpdateTip" in line:
                on_report()


def interval_report(interval: float, on_report: "Callable[[], None]") -> None:
    while not shutdown.wait(timeout=interval):
        on_report()

def main() -> None:
    parser = argparse.ArgumentParser(
        description="Chainstate file last-access (atime or inotify), report on UpdateTip or interval."
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
    parser.add_argument(
        "--inotify",
        action="store_true",
        help="Use inotify-tools (inotifywait) instead of filesystem atime",
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

    if args.inotify:
        if not os.path.isfile(INOTIFYWAIT) or not os.access(INOTIFYWAIT, os.X_OK):
            sys.exit(
                "inotifywait not found. Install inotify-tools (e.g. apt install inotify-tools)."
            )
        file_last_access = seed_file_last_access(chainstate_dir)
        lock = threading.Lock()

        def on_report() -> None:
            report_inotify(file_last_access, lock)

        t_inotify = threading.Thread(
            target=run_inotify,
            args=(chainstate_dir, file_last_access, lock),
            daemon=True,
        )
        t_inotify.start()
    else:
        def on_report() -> None:
            report_atime(chainstate_dir)

    if args.no_tail:
        t_interval = threading.Thread(
            target=interval_report,
            args=(args.interval, on_report),
            daemon=True,
        )
        t_interval.start()
    else:
        t_tail = threading.Thread(
            target=tail_debug_log,
            args=(debug_log, on_report),
            daemon=True,
        )
        t_tail.start()

    def sigint_handler(_signum: int, _frame: object) -> None:
        shutdown.set()

    signal.signal(signal.SIGINT, sigint_handler)
    signal.signal(signal.SIGTERM, sigint_handler)

    try:
        if args.inotify:
            t_inotify.join()
        elif args.no_tail:
            t_interval.join()
        else:
            t_tail.join()
    except KeyboardInterrupt:
        shutdown.set()
    sys.exit(0)


if __name__ == "__main__":
    main()
