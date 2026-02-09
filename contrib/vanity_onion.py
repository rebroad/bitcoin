#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# Generate Tor v3 onion service keys that match vanity prefixes and write
# one key per file into a directory (e.g. onion_v3_private_keys).

import argparse
import base64
import hashlib
import os
import math
import queue
import signal
import sys
import time
from pathlib import Path
from typing import Iterable, List, Sequence, Set, Tuple

try:
    from cryptography.hazmat.primitives.asymmetric import ed25519
    from cryptography.hazmat.primitives.serialization import Encoding, PrivateFormat, PublicFormat, NoEncryption
except Exception as e:
    print(f"error: cryptography is required ({e})", file=sys.stderr)
    sys.exit(2)


ONION_VERSION = b"\x03"
ONION_CHECKSUM_PREFIX = b".onion checksum"
VALID_BASE32_CHARS = set("abcdefghijklmnopqrstuvwxyz234567")


def onion_service_id_from_pubkey(pubkey: bytes) -> str:
    checksum = hashlib.sha3_256(ONION_CHECKSUM_PREFIX + pubkey + ONION_VERSION).digest()[:2]
    address_bytes = pubkey + checksum + ONION_VERSION
    # Standard base32 (RFC 4648) without padding, lowercase.
    return base64.b32encode(address_bytes).decode("ascii").lower().rstrip("=")


def generate_keypair() -> Tuple[bytes, str]:
    sk = ed25519.Ed25519PrivateKey.generate()
    seed = sk.private_bytes(Encoding.Raw, PrivateFormat.Raw, NoEncryption())
    pub = sk.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw)
    key_blob = seed + pub  # 32-byte seed + 32-byte pubkey
    service_id = onion_service_id_from_pubkey(pub)
    return key_blob, service_id


def normalize_prefix(prefix: str) -> str:
    p = prefix.strip().lower()
    if p.endswith(".onion"):
        p = p[:-6]
    if not p:
        raise ValueError("empty prefix")
    if len(p) > 56:
        raise ValueError("prefix too long (max 56 chars)")
    if any(ch not in VALID_BASE32_CHARS for ch in p):
        raise ValueError("invalid characters (allowed: a-z2-7)")
    return p


def load_prefixes(prefixes_arg: str, prefix_file: str) -> List[str]:
    prefixes: Set[str] = set()
    if prefixes_arg:
        for raw in prefixes_arg.split(","):
            raw = raw.strip()
            if not raw:
                continue
            prefixes.add(normalize_prefix(raw))
    if prefix_file:
        with open(prefix_file, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                prefixes.add(normalize_prefix(line))
    if not prefixes:
        raise ValueError("no prefixes provided")
    return sorted(prefixes)


def effective_prefixes(prefixes: Sequence[str]) -> List[str]:
    # Remove any prefix that is covered by a shorter prefix.
    sorted_p = sorted(prefixes, key=len)
    effective: List[str] = []
    for p in sorted_p:
        if any(p.startswith(e) for e in effective):
            continue
        effective.append(p)
    return effective


def hit_probability(prefixes: Sequence[str]) -> float:
    # Approximate hit probability per attempt as sum(32^-len),
    # assuming no overlapping prefixes (handled via effective_prefixes).
    eff = effective_prefixes(prefixes)
    return sum(32.0 ** (-len(p)) for p in eff)


def attempts_for_probability(p_hit: float, target: float) -> float:
    if p_hit <= 0.0:
        return float("inf")
    if target <= 0.0:
        return 0.0
    if target >= 1.0:
        return float("inf")
    return math.log(1.0 - target) / math.log(1.0 - p_hit)


def format_duration(seconds: float) -> str:
    if seconds >= 24 * 3600:
        return f"{seconds / 86400.0:,.1f} days"
    if seconds >= 3600:
        return f"{seconds / 3600.0:,.1f} hours"
    return f"{seconds / 60.0:,.1f} min"


def worker_loop(prefixes: Sequence[str], stop_event, out_queue, status_interval: float) -> None:
    attempts = 0
    last_report = time.time()
    while not stop_event.is_set():
        key_blob, service_id = generate_keypair()
        attempts += 1
        if any(service_id.startswith(p) for p in prefixes):
            key_b64 = base64.b64encode(key_blob).decode("ascii")
            out_queue.put(("match", service_id, key_b64))
        if status_interval > 0 and (time.time() - last_report) >= status_interval:
            out_queue.put(("stat", attempts))
            attempts = 0
            last_report = time.time()


def write_key_file(outdir: Path, service_id: str, key_b64: str) -> bool:
    outdir.mkdir(parents=True, exist_ok=True)
    path = outdir / service_id
    try:
        with open(path, "x", encoding="utf-8") as f:
            f.write("ED25519-V3:" + key_b64 + "\n")
        return True
    except FileExistsError:
        return False


def parse_args(argv: List[str]) -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="Generate vanity Tor v3 onion keys and write one key per file.",
    )
    p.add_argument("--prefixes", help="Comma-separated list of prefixes to match (a-z2-7).")
    p.add_argument("--prefix-file", help="File with one prefix per line (a-z2-7).")
    p.add_argument("--outdir", default="onion_v3_private_keys", help="Output directory for key files.")
    p.add_argument("--count", type=int, default=1, help="Number of matches to generate.")
    p.add_argument("--workers", type=int, default=0, help="Number of worker processes (default: CPU count).")
    p.add_argument("--status-interval", type=float, default=5.0, help="Seconds between status updates (0 to disable).")
    return p.parse_args(argv)


def main(argv: List[str]) -> int:
    args = parse_args(argv)
    try:
        prefixes = load_prefixes(args.prefixes, args.prefix_file)
    except ValueError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2

    if args.count < 1:
        print("error: --count must be >= 1", file=sys.stderr)
        return 2

    outdir = Path(args.outdir)
    workers = args.workers if args.workers > 0 else (os.cpu_count() or 1)
    workers = max(1, workers)

    print(f"prefixes: {', '.join(prefixes)}")
    print(f"output: {outdir}")
    print(f"workers: {workers}")

    import multiprocessing as mp

    stop_event = mp.Event()
    out_queue: "mp.Queue[Tuple[str, object]]" = mp.Queue()

    def handle_sigint(_signum, _frame):
        stop_event.set()

    signal.signal(signal.SIGINT, handle_sigint)
    signal.signal(signal.SIGTERM, handle_sigint)

    procs = []
    for _ in range(workers):
        p = mp.Process(target=worker_loop, args=(prefixes, stop_event, out_queue, args.status_interval))
        p.daemon = True
        p.start()
        procs.append(p)

    matches = 0
    attempts = 0
    last_print = time.time()
    start_time = last_print
    p_hit = hit_probability(prefixes)
    n50 = attempts_for_probability(p_hit, 0.5)
    if math.isfinite(n50):
        print(f"per-attempt hit probability ~ {p_hit:.6e}, 50% chance in ~ {n50:,.0f} attempts")

    try:
        while matches < args.count and not stop_event.is_set():
            try:
                msg = out_queue.get(timeout=0.2)
            except queue.Empty:
                continue

            if msg[0] == "match":
                service_id, key_b64 = msg[1], msg[2]
                if write_key_file(outdir, service_id, key_b64):
                    matches += 1
                    print(f"match {matches}/{args.count}: {service_id}.onion")
                else:
                    print(f"warning: file exists for {service_id}, skipping")
            elif msg[0] == "stat":
                attempts += int(msg[1])
                now = time.time()
                if args.status_interval > 0 and (now - last_print) >= args.status_interval:
                    elapsed = now - start_time
                    rate = attempts / elapsed if elapsed > 0 else 0.0
                    eta = ""
                    if math.isfinite(n50) and rate > 0:
                        eta_seconds = max(0.0, n50 / rate)
                        eta = f", est 50% time ~ {format_duration(eta_seconds)}"
                    print(f"attempts: {attempts} ({rate:,.0f} per sec){eta}")
                    last_print = now
    finally:
        stop_event.set()
        for p in procs:
            p.join(timeout=1.0)

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

