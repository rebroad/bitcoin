#!/usr/bin/env python3
"""
Explore mempool fee distribution: compare total tx count / memory with
how many transactions fall into the GUI fee histogram buckets (sat/vB).

Useful to verify that the Debug window Mempool tab (fee-range chart)
accounts for all mempool entries. Transactions with fee rate < 1 sat/vB
were previously omitted because the histogram buckets started at 1; they
are now included in a [0,1) bucket.

Usage:
  ./contrib/mempool-fee-ranges.py
  BITCOIN_CLI='ssh bigquiet ~/bin/bitcoin-cli' ./contrib/mempool-fee-ranges.py

On remote hosts, use the full path to bitcoin-cli (e.g. ~/bin/bitcoin-cli) because
SSH runs a non-interactive shell that does not source .profile/.bashrc, so PATH
often does not include $HOME/bin.

Requires: bitcoin-cli (or set BITCOIN_CLI), Python 3.
"""

import json
import os
import subprocess
import sys

# Fee bucket upper bounds (sat/vB) matching src/node/interfaces.cpp feelimits
FEELIMITS = [
    0, 1, 2, 3, 4, 5, 6, 7, 8, 10,
    12, 14, 17, 20, 25, 30, 40, 50, 60, 70, 80, 100,
    120, 140, 170, 200, 250, 300, 400, 500, 600, 700, 800, 1000,
    1200, 1400, 1700, 2000, 2500, 3000, 4000, 5000, 6000, 7000, 8000, 10000,
]


def btc_to_sats(btc_str: str) -> int:
    """Convert RPC amount string (e.g. '0.00001') to satoshis."""
    try:
        val = float(btc_str)
    except (TypeError, ValueError):
        return 0
    return int(round(val * 100_000_000))


def run_cli(*args: str) -> str:
    cli = os.environ.get("BITCOIN_CLI", "bitcoin-cli")
    cmd = cli.split() + list(args)
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"Error: {result.stderr.strip() or result.stdout}", file=sys.stderr)
        sys.exit(1)
    return result.stdout.strip()


def main() -> None:
    info = json.loads(run_cli("getmempoolinfo"))
    total_tx = info["size"]
    usage = info["usage"]
    print(f"getmempoolinfo: size={total_tx} tx, usage={usage} bytes")

    raw = json.loads(run_cli("getrawmempool", "true"))
    # RPC has "fees": {"base": "0.00001", ...} and "vsize"
    # One bucket per feelimit; last bucket is [10000, +inf)
    bucket_counts = [0] * len(FEELIMITS)
    below_one = 0
    for txid, entry in raw.items():
        vsize = entry.get("vsize") or entry.get("size", 0)
        if vsize <= 0:
            continue
        fees = entry.get("fees") or {}
        base_btc = fees.get("base")
        if base_btc is None:
            # Deprecated top-level "fee" if -deprecatedrpc=fees
            base_btc = entry.get("fee", 0)
        fee_sats = btc_to_sats(str(base_btc))
        sat_per_vb = fee_sats / vsize if vsize else 0

        if sat_per_vb < 1:
            below_one += 1
        for i in range(len(FEELIMITS)):
            if i == len(FEELIMITS) - 1:
                if sat_per_vb >= FEELIMITS[i]:
                    bucket_counts[i] += 1
                break
            if FEELIMITS[i] <= sat_per_vb < FEELIMITS[i + 1]:
                bucket_counts[i] += 1
                break

    in_buckets = sum(bucket_counts)
    print(f"getrawmempool true: {len(raw)} tx with vsize/fee")
    print(f"Tx with fee rate < 1 sat/vB: {below_one}")
    print(f"Tx counted in histogram buckets: {in_buckets}")
    if total_tx != in_buckets:
        print(f"  -> Mismatch: mempool size {total_tx} vs bucket sum {in_buckets} (diff {total_tx - in_buckets})")
    else:
        print("  -> All mempool entries fall in histogram buckets.")
    print("\nFirst few buckets (sat/vB):")
    for i in range(min(6, len(bucket_counts))):
        low = FEELIMITS[i]
        label = f"{low}+" if i == len(FEELIMITS) - 1 else f"{low}-{FEELIMITS[i + 1]}"
        print(f"  {label}: {bucket_counts[i]} tx")


if __name__ == "__main__":
    main()
