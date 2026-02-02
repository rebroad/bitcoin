#!/usr/bin/env python3
"""
Parse a LevelDB manifest and output the set of live .ldb file numbers.
A file is "live" if it is in the final version after replaying all VersionEdit records.

Usage:
  ./contrib/leveldb-manifest-live-files.py ~/.bitcoin/chainstate
  ./contrib/leveldb-manifest-live-files.py ~/.bitcoin/chainstate 3670338 3669612

With file numbers as args, checks whether each is live (in manifest) or obsolete.
"""

import struct
import sys
from pathlib import Path

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


def parse_version_edit(data: bytes) -> tuple[set[int], set[int]]:
    """Parse VersionEdit; return (new_files, deleted_files) as sets of file numbers."""
    new_files = set()
    deleted_files = set()
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
                _, offset = decode_length_prefixed_slice(data, offset)  # smallest
                _, offset = decode_length_prefixed_slice(data, offset)  # largest
                new_files.add(fnum)
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


def get_live_files(chainstate_dir: Path) -> set[int]:
    """Replay manifest and return set of live .ldb file numbers."""
    current_file = chainstate_dir / "CURRENT"
    manifest_name = current_file.read_text().strip()
    manifest_path = chainstate_dir / manifest_name
    if not manifest_path.exists():
        manifest_path = chainstate_dir / manifest_name.split("/")[-1]
    records = read_log_records(manifest_path)
    live = set()
    for rec in records:
        try:
            new_f, del_f = parse_version_edit(rec)
            live |= new_f
            live -= del_f
        except Exception:
            continue
    return live


def main() -> None:
    if len(sys.argv) < 2:
        print("Usage: leveldb-manifest-live-files.py <chainstate_dir> [file_num ...]")
        sys.exit(1)
    chainstate_dir = Path(sys.argv[1]).resolve()
    if not chainstate_dir.is_dir():
        sys.exit(f"Not a directory: {chainstate_dir}")
    check_nums = [int(x) for x in sys.argv[2:]] if len(sys.argv) > 2 else []
    live = get_live_files(chainstate_dir)
    if check_nums:
        for n in check_nums:
            status = "LIVE" if n in live else "OBSOLETE"
            print(f"{n}.ldb: {status}")
    else:
        for n in sorted(live):
            print(f"{n}.ldb")


if __name__ == "__main__":
    main()
