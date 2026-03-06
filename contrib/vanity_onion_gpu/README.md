# Vanity Onion GPU Prototype (AMD/HIP)

This directory contains a standalone prototype for a GPU-based Tor v3 vanity search pipeline with AMD ROCm/HIP as the primary target.

## Scope (Phase 1)

- Define data layout and HIP kernel interface for high-throughput prefix filtering on GPU.
- Provide CPU reference matching logic to validate behavior.
- Keep all code isolated from the main Bitcoin Core build until GPU path is mature.

## Why this exists

`src/vanity_onion.cpp` is CPU-only and already optimized with early rejection.
To get materially faster searches for longer prefixes, we need a GPU-native candidate loop.

## Current files

- `vanity_onion_gpu.hip.cpp`:
  HIP kernel interface and prefix matcher over precomputed 35-byte onion address payloads.
- `hip_smoke.cpp`:
  Minimal HIP host harness that launches the kernel and prints hit flags.
- `reference_prefix_match.cpp`:
  CPU reference implementation for correctness checks.
- `FORMAT.md`:
  Fixed binary layout used between host and kernels.

## Next milestones

1. Add device-side Ed25519 basepoint multiplication from clamped scalar.
2. Add device-side SHA3-256 checksum for `.onion checksum || pubkey || version`.
3. Keep full candidate generation and filtering on GPU; return only hits to host.
4. Verify hit candidates with existing CPU path before writing key files.
5. Add benchmark harness comparing CPU and GPU `tries/sec` and energy use.

## Build (on ROCm system)

```bash
cd contrib/vanity_onion_gpu
hipcc -O3 vanity_onion_gpu.hip.cpp hip_smoke.cpp -o hip_smoke
./hip_smoke
```

CPU-only reference:

```bash
g++ -std=c++20 -O2 reference_prefix_match.cpp -o reference_prefix_match
./reference_prefix_match
```

## Notes

- This prototype is not wired into `src/Makefile.am`.
- It is intentionally separate while architecture and validation are in progress.
