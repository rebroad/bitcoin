# Anyone Can Spend Handler - Complete Guide

This comprehensive guide covers the detection, handling, and usage of "anyone can spend" outputs in Bitcoin Core.

## Table of Contents

1. [Overview](#overview)
2. [What are "Anyone Can Spend" Outputs?](#what-are-anyone-can-spend-outputs)
3. [Detection System](#detection-system)
4. [Handler Architecture](#handler-architecture)
5. [Installation](#installation)
6. [Usage](#usage)
7. [Configuration](#configuration)
8. [Security Considerations](#security-considerations)
9. [Troubleshooting](#troubleshooting)
10. [API Reference](#api-reference)
11. [Examples](#examples)

## Overview

The Anyone Can Spend Handler is a comprehensive system that:

- **Detects** transactions with "anyone can spend" outputs in the mempool and blocks
- **Automatically spends** these outputs to a specified address
- **Provides RPC interface** for manual control and monitoring
- **Tracks statistics** about detected and spent outputs
- **Uses a dedicated wallet** for persistent transaction management

## What are "Anyone Can Spend" Outputs?

"Anyone can spend" outputs are Bitcoin transaction outputs that can be spent by anyone without requiring cryptographic proof. These are scripts that **always succeed when executed**.

### True "Anyone Can Spend" Scripts:
- **OP_TRUE**: Always evaluates to true
- **OP_1**: Pushes the value 1 (which is true)
- **OP_DROP OP_TRUE**: Drops the top stack item, then evaluates to true
- **OP_DROP OP_1**: Drops the top stack item, then pushes 1
- **OP_NOP**: No operation (always succeeds)
- **OP_NOP1 through OP_NOP10**: No operations (always succeed)

### Important Note:
**SegWit addresses (P2WPKH, P2WSH, P2TR) are NOT "anyone can spend"** - they still require proper witness data including signatures, public keys, and script execution.

### What We Do NOT Detect:
- **OP_RETURN outputs**: These are provably unspendable (no one can spend them)
- **Invalid/malformed scripts**: These will fail execution
- **Oversized scripts**: These will fail validation
- **SegWit addresses**: These require proper cryptographic proof

## Detection System

### Detection Logic

The detection is implemented in `src/validation.cpp` and `src/rpc/blockchain.cpp` with two main functions:

1. `IsAnyoneCanSpendAddress(const CScript& scriptPubKey)`: Checks if a script represents a truly "anyone can spend" address
2. `HasAnyoneCanSpendOutputs(const CTransaction& tx)`: Checks if a transaction has any outputs to such addresses

### Notification System

When a transaction with "anyone can spend" outputs is added to the mempool, the system:

1. Logs a warning message to the console/log file
2. Emits a validation interface signal `AnyoneCanSpendTransactionAddedToMempool`
3. Allows external applications to subscribe to these notifications

### Integration Points

The detection is integrated into the mempool transaction processing pipeline:

- `MemPoolAccept::AcceptSingleTransaction()`: For individual transactions
- `MemPoolAccept::SubmitPackage()`: For package transactions
- `CChainState::ConnectBlock()`: For transactions in blocks

## Handler Architecture

### Core Components

1. **AnyoneCanSpendHandler**: Main handler class that inherits from `CValidationInterface`
2. **"Anyone" Wallet**: Dedicated wallet for managing detected outputs
3. **RPC Interface**: Commands for initialization, control, and monitoring
4. **Validation Integration**: Automatic detection and processing

### Key Features

- **Automatic Detection**: Monitors mempool and blocks for "anyone can spend" outputs
- **Automatic Spending**: Can automatically create spending transactions
- **Manual Control**: RPC interface for manual control and monitoring
- **Statistics**: Track detected and spent outputs
- **Safe Architecture**: Runs in separate thread, no interference with core validation
- **Persistent Storage**: Uses Bitcoin Core's wallet system for transaction history
- **Automatic Re-broadcast**: Leverages wallet's built-in re-broadcast functionality

## Installation

1. Build Bitcoin Core with the anyone can spend handler:
   ```bash
   make -j$(nproc)
   ```

2. The handler is automatically available when you run `bitcoind` or `bitcoin-qt`.

## Usage

### RPC Commands

#### Initialize the Handler

```bash
# Initialize with your destination address
bitcoin-cli anyonecanspendhandler init "bc1qxy2kgdygjrsqtzq2n0yrf2493p83kkfjhx0wlh"
```

Response:
```json
{
  "status": "initialized",
  "destination": "bc1qxy2kgdygjrsqtzq2n0yrf2493p83kkfjhx0wlh",
  "auto_spend": false
}
```

#### Enable Auto-Spending

```bash
# Enable automatic spending of detected outputs
bitcoin-cli anyonecanspendhandler enable
```

Response:
```json
{
  "status": "auto-spend enabled",
  "auto_spend": true,
  "destination": "bc1qxy2kgdygjrsqtzq2n0yrf2493p83kkfjhx0wlh"
}
```

#### Check Status

```bash
# Check current handler status
bitcoin-cli anyonecanspendhandler status
```

Response:
```json
{
  "status": "initialized",
  "auto_spend": true,
  "destination": "bc1qxy2kgdygjrsqtzq2n0yrf2493p83kkfjhx0wlh"
}
```

#### Get Statistics

```bash
# Get statistics about detected and spent outputs
bitcoin-cli anyonecanspendhandler stats
```

Response:
```json
{
  "status": "stats retrieved",
  "stats": {
    "outputs_detected": 5,
    "outputs_spent": 3,
    "total_amount_detected": "0.00150000",
    "total_amount_spent": "0.00100000"
  }
}
```

#### Disable Auto-Spending

```bash
# Disable automatic spending
bitcoin-cli anyonecanspendhandler disable
```

Response:
```json
{
  "status": "auto-spend disabled",
  "auto_spend": false,
  "destination": "bc1qxy2kgdygjrsqtzq2n0yrf2493p83kkfjhx0wlh"
}
```

### Programmatic Usage

You can also use the handler programmatically:

```cpp
#include <anyone_can_spend_handler.h>
#include <wallet/wallet.h>

// Create the handler
auto handler = std::make_unique<AnyoneCanSpendHandler>();

// Initialize with destination address and wallet context
std::string destination = "bc1qxy2kgdygjrsqtzq2n0yrf2493p83kkfjhx0wlh";
WalletContext* wallet_context = /* get wallet context from node */;
handler->Initialize(destination, wallet_context, false);

// Enable auto-spending
handler->SetAutoSpend(true);

// Get statistics
auto stats = handler->GetStats();
std::cout << "Detected: " << stats.outputs_detected << std::endl;
std::cout << "Spent: " << stats.outputs_spent << std::endl;
```

## Configuration

### Bitcoin Core Configuration

Add these options to your `bitcoin.conf`:

```ini
# Enable anyone can spend detection (already enabled by default)
# No additional configuration needed

# Optional: Set log level for handler messages
debug=anyonecanspend

# Optional: Set default destination address
anyonecanspenddestination=bc1qxy2kgdygjrsqtzq2n0yrf2493p83kkfjhx0wlh
```

### Configuration Options

- **`anyonecanspenddestination`**: Default destination address for spending outputs
- **`debug=anyonecanspend`**: Enable detailed logging for the handler

## Security Considerations

⚠️ **WARNING**: This handler can automatically spend outputs to your address. Consider:

1. **Test First**: Always test on testnet or regtest first
2. **Monitor Logs**: Watch the logs for handler activity
3. **Start Disabled**: Initialize with `auto_spend=false` and enable manually
4. **Use Dedicated Address**: Use a separate address for testing
5. **Network Effects**: Be aware that multiple nodes running this could create competing transactions
6. **Wallet Security**: The "Anyone" wallet will contain private keys for spending transactions

### Best Practices

- **For Testing/Development**: Use regtest mode for safe testing
- **For Production**: Always specify a dedicated address and monitor activity
- **For Encrypted Wallets**: The handler works with any wallet state

## Logging

The handler logs its activities. Look for messages like:

```
AnyoneCanSpendHandler: Initialized with destination bc1q..., auto_spend=false
AnyoneCanSpendHandler: Detected transaction abc123... with anyone can spend outputs
AnyoneCanSpendHandler: Found anyone can spend output abc123...:0, amount 0.00100000
AnyoneCanSpendHandler: Created and committed spending transaction def456... for 2 outputs (total: 0.00100000, fee: 0.00010000)
```

## Troubleshooting

### Handler Not Initialized

If you get "Handler not initialized" errors:

1. Make sure you called `init` first
2. Check that you provided a valid Bitcoin address
3. Ensure you have a wallet loaded

### No Outputs Detected

If no outputs are being detected:

1. Check that "anyone can spend" outputs actually exist in the network
2. Verify the detection logic matches the outputs you're looking for
3. Check the logs for detection messages

### Spending Transactions Not Created

If spending transactions aren't being created:

1. Check that auto-spend is enabled
2. Verify the destination address is valid
3. Check for fee calculation issues (outputs too small)
4. Look for errors in the logs

### Compilation Issues

If you encounter compilation errors:

1. Ensure all required headers are included
2. Check that wallet support is enabled
3. Verify Bitcoin Core version compatibility

## API Reference

### AnyoneCanSpendHandler Class

#### Methods

- `Initialize(destination, wallet_context, auto_spend)` - Initialize the handler
- `SetAutoSpend(enabled)` - Enable/disable auto-spending
- `GetAutoSpend()` - Get current auto-spend setting
- `GetDestinationAddress()` - Get destination address
- `GetStats()` - Get statistics

#### Events

- `AnyoneCanSpendTransactionAddedToMempool(tx, sequence)` - Called when anyone can spend outputs are detected

### Statistics Structure

```cpp
struct Stats {
    uint64_t outputs_detected;    // Number of outputs detected
    uint64_t outputs_spent;       // Number of outputs spent
    CAmount total_amount_detected; // Total amount detected
    CAmount total_amount_spent;   // Total amount spent
};
```

### RPC Commands

- `anyonecanspendhandler init [address]` - Initialize the handler
- `anyonecanspendhandler enable` - Enable auto-spending
- `anyonecanspendhandler disable` - Disable auto-spending
- `anyonecanspendhandler status` - Get current status
- `anyonecanspendhandler stats` - Get statistics

## Examples

### Complete Workflow

```bash
# 1. Start Bitcoin Core
bitcoind

# 2. Initialize the handler
bitcoin-cli anyonecanspendhandler init "bc1qxy2kgdygjrsqtzq2n0yrf2493p83kkfjhx0wlh"

# 3. Check status
bitcoin-cli anyonecanspendhandler status

# 4. Enable auto-spending
bitcoin-cli anyonecanspendhandler enable

# 5. Monitor statistics
bitcoin-cli anyonecanspendhandler stats

# 6. Check logs for activity
tail -f ~/.bitcoin/debug.log | grep AnyoneCanSpendHandler
```

### Testing on Regtest

```bash
# Start regtest mode
bitcoind -regtest

# Generate some blocks
bitcoin-cli -regtest generatetoaddress 101 "bcrt1qxy2kgdygjrsqtzq2n0yrf2493p83kkfjhx0wlh"

# Initialize handler
bitcoin-cli -regtest anyonecanspendhandler init "bcrt1qxy2kgdygjrsqtzq2n0yrf2493p83kkfjhx0wlh"

# Enable auto-spending
bitcoin-cli -regtest anyonecanspendhandler enable
```

### Programmatic Example

```cpp
#include <anyone_can_spend_handler.h>
#include <node/context.h>

class MyValidationInterface : public CValidationInterface {
    void AnyoneCanSpendTransactionAddedToMempool(const CTransactionRef& tx, uint64_t mempool_sequence) override {
        // Handle the notification
        LogPrintf("Detected anyone can spend transaction: %s\n", tx->GetHash().ToString());
    }
};

// In your main function
auto handler = std::make_unique<AnyoneCanSpendHandler>();
handler->Initialize("bc1qxy2kgdygjrsqtzq2n0yrf2493p83kkfjhx0wlh", wallet_context, true);
```

## Files Modified

- `src/validation.cpp`: Added detection functions and integration
- `src/validationinterface.h`: Added new validation interface method
- `src/validationinterface.cpp`: Implemented the new signal
- `src/anyone_can_spend_handler.h`: Handler class definition
- `src/anyone_can_spend_handler.cpp`: Handler implementation
- `src/rpc/anyone_can_spend_handler.cpp`: RPC command definitions
- `src/rpc/blockchain.cpp`: Added UTXO scanning command
- `src/Makefile.am`: Build system configuration
- `test/functional/feature_anyone_can_spend_detection.py`: Test file

## Support

For issues or questions:

1. Check the logs for error messages
2. Verify your Bitcoin Core version supports this feature
3. Test on regtest/testnet first
4. Review the security considerations

## License

This code is released under the MIT license, same as Bitcoin Core.
