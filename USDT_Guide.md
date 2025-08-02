# User-space, Statically Defined Tracing (USDT) in Bitcoin Core
## A Comprehensive Guide

*This document provides an overview of USDT implementation and usage in Bitcoin Core, with references to detailed documentation and examples.*

---

## Table of Contents
1. [What is USDT?](#what-is-usdt)
2. [USDT in Bitcoin Core](#usdt-in-bitcoin-core)
3. [Available Tracepoints](#available-tracepoints)
4. [Who Uses USDT?](#who-uses-usdt)
5. [When to Use USDT](#when-to-use-usdt)
6. [Getting Started](#getting-started)
7. [Examples and Scripts](#examples-and-scripts)
8. [Advanced Usage](#advanced-usage)
9. [References and Further Reading](#references-and-further-reading)

---

## What is USDT?

**User-space, Statically Defined Tracing (USDT)** is a tracing framework that allows you to add static tracepoints to user-space applications. Think of it as putting invisible "breadcrumbs" in your code that you can follow later to understand what's happening.

### Key Characteristics:
- **User-space**: Runs in user applications (not kernel space)
- **Statically Defined**: Tracepoints are defined at compile time
- **Tracing**: Allows monitoring and debugging application behavior
- **Zero Overhead**: No performance impact when not being used
- **Non-invasive**: No code changes needed for production debugging

### How It Works:
1. **Tracepoints**: Defined at specific points in code where you want to collect data
2. **Probes**: External tools can attach to these tracepoints
3. **Data Collection**: When a tracepoint is hit, it emits data like function parameters, timestamps, etc.

---

## USDT in Bitcoin Core

Bitcoin Core includes statically defined tracepoints to allow for more observability during development, debugging, code review, and production usage. These tracepoints make it possible to keep track of custom statistics and enable detailed monitoring of otherwise hidden internals.

### Architecture Overview:
```
┌──────────────────┐            ┌──────────────┐
│ tracing script   │            │ bitcoind     │
│==================│      2.    │==============│
│  eBPF  │ tracing │      hooks │              │
│  code  │ logic   │      into┌─┤►tracepoint 1─┼───┐ 3.
└────┬───┴──▲──────┘          ├─┤►tracepoint 2 │   │ pass args
     │      │ 4.              │ │ ...          │   │ to eBPF
     │      │ pass data to    │ └──────────────┘   │ program
     │      │ tracing script  │                    │
     └──────┼─────────────────┼────────────────────┼───
            │                 │                    │
       ┌──┬─▼─────────────────┴────────────┐       │
       │  │  eBPF program                 │◄──────┘
       │  └───────────────────────────────┤
       │ eBPF kernel Virtual Machine      │
       └──────────────────────────────────┘
```

### Implementation Details:
- **Header File**: `src/util/trace.h` defines the TRACE macros
- **Conditional Compilation**: USDT is enabled/disabled via `ENABLE_TRACING`
- **SystemTap Integration**: Uses `DTRACE_PROBE` macros for tracepoint definition
- **Argument Support**: Up to 12 arguments per tracepoint

---

## Available Tracepoints

Bitcoin Core implements tracepoints in **4 main categories**:

### 1. Network Traffic Monitoring (`net` context)

#### `net:inbound_message`
Called when a message is received from a peer over the P2P network.

**Arguments:**
1. Peer ID as `int64`
2. Peer Address and Port as `pointer to C-style String` (max 68 chars)
3. Connection Type as `pointer to C-style String` (max 20 chars)
4. Message Type as `pointer to C-style String` (max 20 chars)
5. Message Size in bytes as `uint64`
6. Message Bytes as `pointer to unsigned chars`

#### `net:outbound_message`
Called when a message is sent to a peer over the P2P network.

**Arguments:** Same as `net:inbound_message`

### 2. UTXO Cache Operations (`utxocache` context)

#### `utxocache:add`
Called when a coin is added to a UTXO cache.

**Arguments:**
1. Transaction ID (hash) as `pointer to unsigned chars` (32 bytes)
2. Output index as `uint32`
3. Block height as `uint32`
4. Value of the coin as `int64`
5. If the coin is a coinbase as `bool`

#### `utxocache:spent`
Called when a coin is spent from a UTXO cache.

**Arguments:** Same as `utxocache:add`

#### `utxocache:uncache`
Called when a coin is purposefully unloaded from a UTXO cache.

**Arguments:** Same as `utxocache:add`

#### `utxocache:flush`
Called after the in-memory UTXO cache is flushed.

**Arguments:**
1. Time to flush in microseconds as `int64`
2. Flush state mode as `uint32`
3. Cache size (number of coins) as `uint64`
4. Cache memory usage in bytes as `uint64`
5. If pruning caused the flush as `bool`

### 3. Block Validation (`validation` context)

#### `validation:block_connected`
Called after a block is connected to the chain.

**Arguments:**
1. Block Header Hash as `pointer to unsigned chars` (32 bytes)
2. Block Height as `int32`
3. Transactions in the Block as `uint64`
4. Inputs spent in the Block as `int32`
5. SigOps in the Block as `uint64`
6. Time to connect the Block in microseconds as `uint64`

---

## Who Uses USDT?

### Primary Users:

#### 1. Bitcoin Core Developers
- **Performance debugging** during development
- **Code review** to understand complex flows
- **Testing** new features and optimizations
- **Benchmarking** different implementations

#### 2. Bitcoin Node Operators
- **Production monitoring** of node performance
- **Network analysis** to understand peer behavior
- **Troubleshooting** performance issues
- **Capacity planning** based on real usage patterns

#### 3. Researchers & Analysts
- **Network topology studies** using P2P traffic data
- **UTXO set analysis** for economic research
- **Block propagation studies** for network efficiency
- **Academic research** on Bitcoin's internals

---

## When to Use USDT

### 🚨 CRITICAL TIMES (When you NEED it):

1. **Performance Issues**
   - Node running slowly? Use `connectblock_benchmark.bt` to find slow blocks
   - Memory usage spiking? Use `log_utxocache_flush.py` to monitor cache behavior
   - Network problems? Use `p2p_monitor.py` to see peer communication

2. **Production Debugging**
   - Node crashes or hangs? USDT gives visibility without invasive logging
   - Network connectivity issues? Real-time P2P traffic monitoring
   - Memory leaks? UTXO cache monitoring shows memory patterns

3. **Development & Testing**
   - Testing new features? Benchmark before/after performance
   - Code review? Understand complex flows with real data
   - Regression testing? Compare performance across versions

### 🔍 INVESTIGATION TIMES (When you WANT it):

1. **Network Analysis** - Understanding P2P communication patterns
2. **Performance Profiling** - Identifying bottlenecks in block processing
3. **Memory Usage Monitoring** - Tracking UTXO cache behavior

### 📊 MONITORING TIMES (When you SHOULD have it):

1. **Production Monitoring**
   - High-value nodes (exchanges, mining pools)
   - Critical infrastructure nodes
   - Nodes with performance SLAs

2. **Research & Analysis**
   - Academic Bitcoin research
   - Network topology studies
   - Economic analysis of UTXO patterns

---

## Getting Started

### Prerequisites:
- Linux system (USDT support is Linux-specific)
- Root privileges (required for eBPF programs)
- Bitcoin Core compiled with USDT support

### Installation:

#### For bpftrace:
```bash
# Ubuntu/Debian
sudo apt-get install bpftrace

# Or build from source
git clone https://github.com/iovisor/bpftrace.git
cd bpftrace
mkdir build && cd build
cmake ..
make
sudo make install
```

#### For BCC (BPF Compiler Collection):
```bash
# Ubuntu/Debian
sudo apt-get install python3-bpfcc

# Or install from source
git clone https://github.com/iovisor/bcc.git
cd bcc
mkdir build && cd build
cmake ..
make
sudo make install
```

### Verify USDT Support:
```bash
# Check if your bitcoind binary has USDT tracepoints
readelf -n ./src/bitcoind | grep -A 20 "stapsdt"
```

---

## Examples and Scripts

### 1. P2P Network Monitoring

#### Interactive P2P Monitor:
```bash
python3 contrib/tracing/p2p_monitor.py ./src/bitcoind
```

**What it shows:**
- Real-time peer connections
- Message types and sizes
- Traffic patterns per peer
- Connection types (inbound, outbound, block-relay-only)

#### Simple P2P Traffic Logging:
```bash
bpftrace contrib/tracing/log_p2p_traffic.bt
```

**Output example:**
```
outbound 'ping' msg to peer 11 (outbound-full-relay, [2a02:b10c:f747:1:ef:fake:ipv6:addr]:8333) with 8 bytes
inbound 'pong' msg from peer 11 (outbound-full-relay, [2a02:b10c:f747:1:ef:fake:ipv6:addr]:8333) with 8 bytes
inbound 'inv' msg from peer 16 (outbound-full-relay, XX.XX.XXX.121:8333) with 37 bytes
```

### 2. Block Processing Benchmarking

#### Find Slow Blocks During Reindex:
```bash
bpftrace contrib/tracing/connectblock_benchmark.bt 20000 38000 25
```

**What it shows:**
- Blocks taking >25ms to process
- Performance statistics (tx/s, inputs/s, sigops/s)
- Histogram of processing times

**Output example:**
```
Attaching 5 probes...
ConnectBlock Benchmark between height 20000 and 38000 inclusive
Logging blocks taking longer than 25 ms to connect.
Starting Connect Block Benchmark between height 20000 and 38000.
BENCH   39 blk/s     59 tx/s      59 inputs/s       20 sigops/s (height 20038)
Block 20492 (000000f555653bb05e2f3c6e79925e01a20dd57033f4dc7c354b46e34735d32b)    20 tx   2319 ins   2318 sigops  took   38 ms
```

### 3. UTXO Cache Analysis

#### Monitor Memory Usage Patterns:
```bash
python3 contrib/tracing/log_utxocache_flush.py ./src/bitcoind
```

**What it shows:**
- Cache flush timing and frequency
- Memory usage before/after flushes
- Pruning impact on cache behavior

**Output example:**
```
Duration (µs)   Mode       Coins Count     Memory Usage    Prune
730451          IF_NEEDED  22990           3323.54 kB      True
637657          ALWAYS     122320          17124.80 kB     False
81349           ALWAYS     0               1383.49 kB      False
```

#### Track UTXO Operations:
```bash
bpftrace contrib/tracing/log_utxos.bt
```

**What it shows:**
- Coins being added to cache
- Coins being spent from cache
- Coins being uncached
- Transaction details and values

---

## Advanced Usage

### Custom Scripts

You can create custom tracing scripts using the available tracepoints. Here's a basic template:

#### bpftrace Script Template:
```bash
#!/usr/bin/env bpftrace

usdt:./src/bitcoind:net:inbound_message
{
    printf("Inbound message from peer %d: %s\n", arg0, str(arg3));
}

usdt:./src/bitcoind:net:outbound_message
{
    printf("Outbound message to peer %d: %s\n", arg0, str(arg3));
}
```

#### BCC Python Script Template:
```python
#!/usr/bin/env python3

from bcc import BPF, USDT

# Define BPF program
program = """
// BPF program code here
"""

# Attach to bitcoind
bitcoind_with_usdts = USDT(path="./src/bitcoind")
bitcoind_with_usdts.enable_probe(probe="inbound_message", fn_name="trace_inbound")
bpf = BPF(text=program, usdt_contexts=[bitcoind_with_usdts])

# Handle events
def handle_event(_, data, size):
    event = bpf["events"].event(data)
    print(f"Event: {event}")

bpf["events"].open_perf_buffer(handle_event)

# Main loop
while True:
    bpf.perf_buffer_poll()
```

### Performance Considerations

#### Ring Buffer Limitations:
- eBPF VM stack is limited to 512 bytes
- Messages larger than ~32KB may be truncated
- Ring buffer throughput is limited - rapid events may be lost

#### Environment Variables:
```bash
# Increase string length limit
export BPFTRACE_STRLEN=70

# Increase ring buffer size (default 64 pages)
export BPFTRACE_PERF_RB_PAGES=128
```

### Troubleshooting

#### Common Issues:
1. **Permission Denied**: USDT requires root privileges
2. **No Tracepoints Found**: Ensure Bitcoin Core was compiled with USDT support
3. **Lost Events**: Increase ring buffer size or reduce event frequency
4. **Truncated Messages**: Large P2P messages (>32KB) will be cut off

#### Debug Commands:
```bash
# List available tracepoints
readelf -n ./src/bitcoind | grep -A 20 "stapsdt"

# Check USDT compilation
grep -r "ENABLE_TRACING" src/

# Verify SystemTap support
stap -e 'probe process("bitcoind").mark("net__inbound_message") { printf("Found tracepoint\n"); exit(); }'
```

---

## References and Further Reading

### Official Documentation:
- **[Bitcoin Core USDT Documentation](doc/tracing.md)** - Complete technical reference
- **[Tracing Examples](contrib/tracing/README.md)** - Working examples and scripts
- **[Dependencies Documentation](doc/dependencies.md)** - Build requirements

### External Resources:
- **[eBPF.io](https://ebpf.io/)** - eBPF overview and resources
- **[bpftrace Documentation](https://github.com/iovisor/bpftrace/blob/master/docs/reference_guide.md)** - bpftrace reference guide
- **[BCC Documentation](https://github.com/iovisor/bcc/blob/master/docs/reference_guide.md)** - BCC reference guide
- **[BCC Python Tutorial](https://github.com/iovisor/bcc/blob/master/docs/tutorial_bcc_python_developer.md)** - Python development guide

### Installation Guides:
- **[bpftrace Installation](https://github.com/iovisor/bpftrace/blob/master/INSTALL.md)**
- **[BCC Installation](https://github.com/iovisor/bcc/blob/master/INSTALL.md)**

### Related Bitcoin Core Documentation:
- **[Build Documentation](doc/build-unix.md)** - Build instructions including USDT dependencies
- **[Development Documentation](doc/developer-notes.md)** - Development guidelines

---

## Conclusion

USDT in Bitcoin Core provides a powerful, non-invasive way to monitor and debug Bitcoin nodes in production. With zero performance impact when not used, it's the perfect tool for understanding Bitcoin's internal workings without modifying code or adding logging overhead.

The combination of network monitoring, UTXO cache analysis, and block processing benchmarking makes USDT an invaluable tool for Bitcoin Core developers, operators, and researchers.

**Remember**: Always run USDT scripts with root privileges and carefully review any scripts before execution in production environments.

---

*This document was generated based on Bitcoin Core source code analysis and existing documentation. For the most up-to-date information, always refer to the official Bitcoin Core documentation and source code.* 