#!/usr/bin/env python3
"""
Parse a LevelDB manifest and output the set of live .ldb file numbers.
A file is "live" if it is in the final version after replaying all VersionEdit records.

Usage:
  ./contrib/leveldb-manifest-live-files.py ~/.bitcoin/chainstate
  ./contrib/leveldb-manifest-live-files.py ~/.bitcoin/chainstate 3670338 3669612

With file numbers as args, checks whether each is live (in manifest) or obsolete.
For live files, shows Bitcoin-specific info:
- Level: LevelDB compaction level (0 = newest SSTs from memtable, hot; 1-6 = compacted,
  older; higher level = older/bulk data). Level 0 files can overlap; 1+ do not.
- Block height range: approximate block heights of UTXOs in the file (from Coin.nHeight).
  Requires the chainstate obfuscation key (read from debug.log). Works while Bitcoin is
  running. Optional deps: python3-snappy (apt install python3-snappy).
"""

import struct
import sys
from pathlib import Path
from typing import Any, List, Optional, Tuple

# VersionEdit tags (from leveldb/db/version_edit.cc)
K_COMPARATOR = 1
K_LOG_NUMBER = 2
K_NEXT_FILE_NUMBER = 3
K_LAST_SEQUENCE = 4
K_COMPACT_POINTER = 5
K_DELETED_FILE = 6
K_NEW_FILE = 7
K_PREV_LOG_NUMBER = 9

# Log format
HEADER_SIZE = 4 + 2 + 1  # checksum + length + type
BLOCK_SIZE = 32768

# Bitcoin chainstate: coin keys are b'C' + txid(32) + varint(vout) (see txdb.cpp CoinEntry)
DB_COIN = ord("C")
INTERNAL_KEY_SUFFIX = 8  # LevelDB appends 8 bytes (sequence|type) to user key
TXID_LEN = 32
OBFUSCATE_KEY_KEY = b"\x00obfuscate_key"  # 14 bytes (dbwrapper.cpp)
OBFUSCATE_KEY_NUM_BYTES = 8

# Level interpretation for user-facing output
LEVEL_HELP = (
    "level 0 = newest/hot (from memtable flush); 1-6 = compacted (higher = older bulk)"
)


def decode_varint(data: bytes, offset: int) -> tuple[int, int]:
    """Decode varint; return (value, new_offset) or raise."""
    result = 0
    shift = 0
    while offset < len(data) and shift <= 63:
        byte = data[offset]
        offset += 1
        result |= ((byte & 127) << shift)
        if (byte & 128) == 0:
            return result, offset
        shift += 7
    raise ValueError("varint overflow")


def decode_length_prefixed_slice(data: bytes, offset: int) -> tuple[bytes, int]:
    """Decode length-prefixed slice; return (slice_bytes, new_offset)."""
    n, offset = decode_varint(data, offset)
    if offset + n > len(data):
        raise ValueError("length-prefixed slice overflow")
    return data[offset : offset + n], offset + n


def coin_key_txid_preview(internal_key: bytes) -> Optional[str]:
    """
    If internal_key is a Bitcoin chainstate coin key, return first 8 hex chars
    of txid (RPC/reverse byte order). Otherwise None.
    """
    if len(internal_key) < INTERNAL_KEY_SUFFIX + 1 + TXID_LEN:
        return None
    user_key = internal_key[: -INTERNAL_KEY_SUFFIX]
    if user_key[0] != DB_COIN:
        return None
    txid_internal = user_key[1 : 1 + TXID_LEN]
    txid_rpc = txid_internal[::-1]
    return txid_rpc.hex()[:8]


def parse_version_edit(data: bytes) -> tuple[dict[int, dict[str, Any]], set[int]]:
    """Parse VersionEdit; return (new_files: {fnum: {level, size}}, deleted_files)."""
    new_files: dict[int, dict[str, Any]] = {}
    deleted_files: set[int] = set()
    offset = 0
    while offset < len(data):
        try:
            tag, offset = decode_varint(data, offset)
        except (ValueError, IndexError):
            break
        if tag == K_NEW_FILE:
            try:
                level, offset = decode_varint(data, offset)
                fnum, offset = decode_varint(data, offset)
                fsize, offset = decode_varint(data, offset)
                smallest, offset = decode_length_prefixed_slice(data, offset)
                largest, offset = decode_length_prefixed_slice(data, offset)
                small_preview = coin_key_txid_preview(smallest)
                large_preview = coin_key_txid_preview(largest)
                txid_range = (small_preview, large_preview) if (small_preview and large_preview) else None
                new_files[fnum] = {"level": level, "size": fsize, "txid_range": txid_range}
            except (ValueError, IndexError):
                break
        elif tag == K_DELETED_FILE:
            try:
                level, offset = decode_varint(data, offset)
                fnum, offset = decode_varint(data, offset)
                deleted_files.add(fnum)
            except (ValueError, IndexError):
                break
        elif tag == K_COMPARATOR:
            try:
                _, offset = decode_length_prefixed_slice(data, offset)
            except (ValueError, IndexError):
                break
        elif tag in (K_LOG_NUMBER, K_PREV_LOG_NUMBER, K_NEXT_FILE_NUMBER, K_LAST_SEQUENCE):
            try:
                _, offset = decode_varint(data, offset)
            except (ValueError, IndexError):
                break
        elif tag == K_COMPACT_POINTER:
            try:
                _, offset = decode_varint(data, offset)
                slen, offset = decode_varint(data, offset)
                offset += slen
            except (ValueError, IndexError):
                break
    return new_files, deleted_files


def decode_varint64(data: bytes, offset: int) -> tuple[int, int]:
    """Decode varint64; return (value, new_offset)."""
    result = 0
    shift = 0
    while offset < len(data) and shift <= 63:
        byte = data[offset]
        offset += 1
        result |= ((byte & 127) << shift)
        if (byte & 128) == 0:
            return result, offset
        shift += 7
    raise ValueError("varint64 overflow")


# LevelDB block trailer: 1 byte type (0=none, 1=snappy), 4 byte crc
BLOCK_TRAILER_SIZE = 5
SST_FOOTER_SIZE = 48
SST_MAGIC = 0xDB4775248B80FB57.to_bytes(8, "little")


def read_block_handle(data: bytes, offset: int) -> tuple[int, int, int]:
    """Parse BlockHandle (varint64 offset, varint64 size); return (offset, size, new_offset)."""
    off, offset = decode_varint64(data, offset)
    size, offset = decode_varint64(data, offset)
    return off, size, offset


def decompress_block(raw: bytes, block_type: int) -> bytes:
    """Decompress block if type==1 (snappy); otherwise return raw."""
    if block_type == 0:
        return raw
    if block_type == 1:
        try:
            import snappy
            return snappy.decompress(raw)
        except Exception:
            raise ValueError("snappy decompression failed")
    raise ValueError(f"unknown block type {block_type}")


def parse_block_entries(data: bytes) -> List[Tuple[bytes, bytes]]:
    """
    Parse LevelDB block format; return list of (key, value).
    Block ends with 4-byte num_restarts and 4*num_restarts bytes of restart offsets.
    """
    if len(data) < 4:
        return []
    num_restarts = struct.unpack_from("<I", data, len(data) - 4)[0]
    restarts_start = len(data) - 4 - num_restarts * 4
    entries = []
    pos = 0
    last_key = bytearray()
    while pos < restarts_start:
        if pos + 3 > restarts_start:
            break
        # Decode shared, non_shared, value_len (each 1 byte or varint32)
        shared, pos = decode_varint(data, pos)
        non_shared, pos = decode_varint(data, pos)
        value_len, pos = decode_varint(data, pos)
        if pos + non_shared + value_len > restarts_start:
            break
        key_delta = data[pos : pos + non_shared]
        pos += non_shared
        value = data[pos : pos + value_len]
        pos += value_len
        # Reconstruct key (shared prefix from last key + delta)
        key = bytearray(last_key[:shared])
        key.extend(key_delta)
        last_key = key
        entries.append((bytes(key), value))
    return entries


def coin_value_height(value_xor: bytes, obfuscate_key: bytes) -> Optional[int]:
    """De-obfuscate chainstate value and parse Coin: return nHeight (code >> 1)."""
    if len(obfuscate_key) != OBFUSCATE_KEY_NUM_BYTES:
        return None
    value = bytearray(len(value_xor))
    for i in range(len(value_xor)):
        value[i] = value_xor[i] ^ obfuscate_key[i % OBFUSCATE_KEY_NUM_BYTES]
    try:
        code, _ = decode_varint(bytes(value), 0)
        return code >> 1
    except Exception:
        return None


def read_height_range_from_ldb(
    chainstate_dir: Path, fnum: int, obfuscate_key: Optional[bytes]
) -> Optional[tuple[int, int]]:
    """
    Read first and last Coin.nHeight from an SST file. Returns (min_height, max_height) or None.
    """
    if obfuscate_key is None:
        return None
    ldb_path = chainstate_dir / f"{fnum}.ldb"
    if not ldb_path.is_file():
        return None
    try:
        with open(ldb_path, "rb") as f:
            data = f.read()
    except OSError:
        return None
    if len(data) < SST_FOOTER_SIZE:
        return None
    footer = data[-SST_FOOTER_SIZE:]
    if footer[-8:] != SST_MAGIC:
        return None
    # Parse footer: metaindex handle, index handle (each varint64 offset + varint64 size)
    pos = 0
    _, _, pos = read_block_handle(footer, pos)
    index_off, index_size, _ = read_block_handle(footer, pos)
    # Read index block (raw + trailer)
    block_start = index_off
    block_raw_len = index_size
    if block_start + block_raw_len + BLOCK_TRAILER_SIZE > len(data):
        return None
    index_raw = data[block_start : block_start + block_raw_len]
    block_type = data[block_start + block_raw_len]
    try:
        index_data = decompress_block(index_raw, block_type)
    except Exception:
        return None
    index_entries = parse_block_entries(index_data)
    if len(index_entries) < 1:
        return None
    # First and last index entry values are BlockHandles for first and last data blocks
    _, first_handle_val = index_entries[0]
    _, last_handle_val = index_entries[-1]
    pos = 0
    if len(first_handle_val) < 2:
        return None
    off1, size1, _ = read_block_handle(first_handle_val, 0)
    off2, size2, _ = read_block_handle(last_handle_val, 0)
    def read_data_block(offset: int, size: int) -> Optional[List[Tuple[bytes, bytes]]]:
        if offset + size + BLOCK_TRAILER_SIZE > len(data):
            return None
        raw = data[offset : offset + size]
        typ = data[offset + size]
        try:
            block_data = decompress_block(raw, typ)
        except Exception:
            return None
        return parse_block_entries(block_data)
    first_block = read_data_block(off1, size1)
    last_block = read_data_block(off2, size2)
    if not first_block or not last_block:
        return None
    _, first_value = first_block[0]
    _, last_value = last_block[-1]
    h1 = coin_value_height(first_value, obfuscate_key)
    h2 = coin_value_height(last_value, obfuscate_key)
    if h1 is None or h2 is None:
        return None
    return (min(h1, h2), max(h1, h2))


def get_obfuscate_key(chainstate_dir: Path) -> Optional[bytes]:
    """Read obfuscation key from Bitcoin debug.log (logged at startup)."""
    debug_log = chainstate_dir.parent / "debug.log"
    if not debug_log.is_file():
        return None
    try:
        with open(debug_log, "r", errors="ignore") as f:
            for line in f:
                if "Using obfuscation key for" in line and str(chainstate_dir) in line:
                    parts = line.split(":")
                    if len(parts) >= 2:
                        hex_key = parts[-1].strip()
                        if len(hex_key) == 16:  # 8 bytes = 16 hex chars
                            return bytes.fromhex(hex_key)
    except OSError:
        pass
    return None


def human_size(n: int) -> str:
    """Format byte count as human-readable (e.g. 1.2M)."""
    for u in ("B", "K", "M", "G"):
        if n < 1024:
            return f"{n}{u}" if u == "B" else f"{n:.1f}{u}"
        n /= 1024
    return f"{n:.1f}T"


def read_log_records(path: Path) -> list[bytes]:
    """Read manifest log file; return list of logical record payloads."""
    with open(path, "rb") as f:
        data = f.read()
    records = []
    offset = 0
    scratch = bytearray()
    while offset + HEADER_SIZE <= len(data):
        # header: checksum(4) + length(2 LE) + type(1)
        length = struct.unpack_from("<H", data, offset + 4)[0]
        rec_type = data[offset + 6]
        payload_start = offset + HEADER_SIZE
        payload_end = payload_start + length
        if payload_end > len(data):
            break
        payload = data[payload_start:payload_end]
        if rec_type == 1:  # kFullType - complete record
            records.append(bytes(payload))
            scratch.clear()
        elif rec_type == 2:  # kFirstType
            scratch = bytearray(payload)
        elif rec_type == 3:  # kMiddleType
            scratch.extend(payload)
        elif rec_type == 4:  # kLastType
            scratch.extend(payload)
            records.append(bytes(scratch))
            scratch.clear()
        offset = payload_end
    return records


def get_live_files(chainstate_dir: Path) -> dict[int, dict[str, Any]]:
    """Replay manifest; return {file_num: {level, size}} for live .ldb files."""
    current_file = chainstate_dir / "CURRENT"
    manifest_name = current_file.read_text().strip()
    manifest_path = chainstate_dir / manifest_name
    if not manifest_path.exists():
        manifest_path = chainstate_dir / manifest_name.split("/")[-1]
    records = read_log_records(manifest_path)
    live: dict[int, dict[str, Any]] = {}
    for rec in records:
        try:
            new_f, del_f = parse_version_edit(rec)
            live.update(new_f)
            for fnum in del_f:
                live.pop(fnum, None)
        except Exception:
            continue
    return live


def main() -> None:
    if len(sys.argv) < 2:
        print("Usage: leveldb-manifest-live-files.py <chainstate_dir> [file_num ...]")
        print("")
        print("Level: " + LEVEL_HELP)
        sys.exit(1)
    chainstate_dir = Path(sys.argv[1]).resolve()
    if not chainstate_dir.is_dir():
        sys.exit(f"Not a directory: {chainstate_dir}")
    check_nums = [int(x) for x in sys.argv[2:]] if len(sys.argv) > 2 else []
    live = get_live_files(chainstate_dir)
    obfuscate_key = get_obfuscate_key(chainstate_dir)
    for fnum in live:
        height_range = read_height_range_from_ldb(chainstate_dir, fnum, obfuscate_key)
        if height_range is not None:
            live[fnum]["height_range"] = height_range
    if check_nums:
        for n in check_nums:
            if n in live:
                meta = live[n]
                level_hint = "0 (newest/hot)" if meta["level"] == 0 else str(meta["level"])
                msg = f"LIVE  level={level_hint}"
                if meta.get("height_range"):
                    lo, hi = meta["height_range"]
                    msg += f"  block_height={lo}..{hi}"
                if meta.get("txid_range"):
                    small, large = meta["txid_range"]
                    msg += f"  txid_range={small}..{large}"
                print(f"{n}.ldb: {msg}")
            else:
                print(f"{n}.ldb: OBSOLETE")
    else:
        print("# " + LEVEL_HELP)
        print("# level  height_range   file")
        for n in sorted(live):
            meta = live[n]
            level_hint = "0" if meta["level"] == 0 else str(meta["level"])
            hr = f"{meta['height_range'][0]}..{meta['height_range'][1]}" if meta.get("height_range") else "-"
            print(f"  {level_hint}    {hr:12}  {n}.ldb")


if __name__ == "__main__":
    try:
        main()
    except BrokenPipeError:
        sys.exit(0)  # e.g. script | head
