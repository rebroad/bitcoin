# Data Format

All candidate payloads use the Tor v3 address payload format:

- `address35[0..31]`: Ed25519 public key (32 bytes)
- `address35[32..33]`: Onion checksum first 2 bytes
- `address35[34]`: Version byte (`0x03`)

The full `.onion` service ID is:

- `base32(address35)` -> 56 lowercase characters

Prefix rules:

- Valid chars: `a-z`, `2-7`, `.`
- `.` wildcard means "match any non-letter base32 char", i.e. `2-7`

GPU kernel inputs are flattened arrays:

- `addresses`: `candidate_count * 35` bytes
- `prefixes`: `prefix_count * max_prefix_len` bytes (NUL padded)
- `prefix_lens`: `prefix_count` bytes
- `hit_flags`: `candidate_count` bytes (`0` or `1`)
