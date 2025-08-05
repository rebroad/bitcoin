# Force Abandon Transaction Feature

## Overview

This feature enhances Bitcoin Core's transaction management capabilities by adding a "Force Abandon" option that can handle transactions stuck in the mempool. It provides users with more control over their unconfirmed transactions.

## What's New

### 1. Enhanced GUI Context Menu

The transaction list now has **three transaction management options**:

- **"Abandon transaction"** - For transactions already out of mempool
- **"Force abandon transaction"** - For transactions stuck in mempool (NEW!)
- **"Increase transaction fee"** - For RBF-enabled transactions

### 2. New RPC Method: `evicttransaction`

A new RPC method that removes transactions from the local mempool:

```bash
bitcoin-cli evicttransaction "txid"
```

## How It Works

### Force Abandon Process

1. **Right-click** on a transaction in the transaction list
2. **Select "Force abandon transaction"** (only enabled for mempool transactions)
3. **Confirm the action** in the warning dialog
4. **Automatic two-step process**:
   - Step 1: Evict transaction from local mempool
   - Step 2: Abandon the transaction in the wallet
5. **Success confirmation** appears when complete

### Smart Enable/Disable Logic

The GUI automatically shows the appropriate options based on transaction state:

- **"Abandon transaction"**: Only enabled when `!inMempool && !confirmed && !alreadyAbandoned`
- **"Force abandon transaction"**: Only enabled when `inMempool && !confirmed`
- **"Increase transaction fee"**: Only enabled when `rbfEnabled && inMempool`

## Technical Implementation

### RPC Layer

```cpp
RPCHelpMan evicttransaction()
{
    // Validates transaction exists in wallet and mempool
    // Removes from local mempool using MemPoolRemovalReason::ABANDONED
    // Returns success/failure
}
```

### Wallet Interface

```cpp
// New methods added to interfaces::Wallet
virtual bool inMempool(const uint256& txid) = 0;
virtual bool evictTransaction(const uint256& txid) = 0;
```

### GUI Integration

```cpp
void TransactionView::forceAbandonTx()
{
    // Shows confirmation dialog with warnings
    // Calls evictTransaction() then abandonTransaction()
    // Updates UI and shows success message
}
```

## Use Cases

### When to Use Force Abandon

- **Low-fee transactions** stuck in mempool for weeks/months
- **Transactions with dust outputs** that are unlikely to be mined
- **Test transactions** that you want to clean up
- **Transactions sent to wrong addresses** (before they're mined)

### When NOT to Use Force Abandon

- **High-fee transactions** that are likely to be mined soon
- **Important transactions** where you want to preserve the original fee
- **Transactions that are already confirmed** (use regular abandon instead)

## Limitations and Warnings

### Important Limitations

1. **Local Only**: Force abandonment only affects your local node
2. **Network Persistence**: Other nodes may still relay the transaction
3. **Mining Risk**: The transaction could still be mined by other miners
4. **No Guarantee**: There's no guarantee the transaction won't be mined

### User Warnings

The GUI shows clear warnings before force abandonment:

```
This will evict the transaction from the mempool and then abandon it.

Warning: This only affects your local node. The transaction may still be 
relayed by other nodes and could potentially be mined.

Are you sure you want to force abandon this transaction?
```

## Testing

### Manual Testing

1. **Create a low-fee transaction** that gets stuck in mempool
2. **Right-click** on the transaction in the GUI
3. **Verify** "Force abandon transaction" is enabled
4. **Click** the option and confirm
5. **Verify** the transaction is abandoned

### RPC Testing

```bash
# Test the new RPC method
bitcoin-cli help evicttransaction

# Test with invalid txid (should return error)
bitcoin-cli evicttransaction "invalid_txid"

# Test with valid txid in mempool
bitcoin-cli evicttransaction "your_txid_here"
```

## Comparison with Existing Methods

| Method | When Available | Effect | Network Impact |
|--------|---------------|---------|----------------|
| **Regular Abandon** | Transaction out of mempool | Marks as abandoned | None |
| **Force Abandon** | Transaction in mempool | Evicts + abandons | Local only |
| **Fee Bump (RBF)** | RBF-enabled transaction | Replaces with higher fee | Network-wide |

## Future Enhancements

Potential improvements for future versions:

1. **Network-wide eviction** - Coordinate with other nodes
2. **Batch operations** - Abandon multiple transactions at once
3. **Automatic detection** - Suggest abandonment for old low-fee transactions
4. **Fee estimation** - Show what fee would be needed for confirmation

## Troubleshooting

### Common Issues

1. **"Force abandon transaction" not enabled**
   - Transaction must be in mempool
   - Transaction must not be confirmed

2. **"Failed to evict transaction from mempool"**
   - Transaction may have already been removed
   - Check if transaction still exists in wallet

3. **"Failed to abandon transaction"**
   - Transaction may have been confirmed
   - Check transaction status

### Debug Commands

```bash
# Check if transaction is in mempool
bitcoin-cli getmempoolentry "txid"

# Check transaction status
bitcoin-cli gettransaction "txid"

# List all mempool transactions
bitcoin-cli getrawmempool
```

## Security Considerations

- **No network-wide effect** - Only affects local node
- **No double-spending** - Original transaction could still be mined
- **User confirmation required** - Prevents accidental abandonment
- **Clear warnings** - Users understand the limitations

## Conclusion

The Force Abandon Transaction feature provides users with an additional tool for managing stuck transactions. While it has limitations (local-only effect), it offers a practical solution for cleaning up old, low-fee transactions that are unlikely to be mined.

This feature complements the existing fee bumping and regular abandonment options, giving users maximum control over their transaction management workflow. 