#ifndef BITCOIN_BLOCKSTATUS_CACHE_H
#define BITCOIN_BLOCKSTATUS_CACHE_H

#include <map>
#include <atomic>

// Global block status cache that can be populated during block loading
// This avoids Qt dependencies in the core Bitcoin code
class BlockStatusCache
{
public:
    enum BlockStatus {
        UNKNOWN = 0,        // Status not yet determined
        NO_HEADER = 1,      // Don't have the header
        HEADER_ONLY = 2,    // Have header but no block data
        HAVE_BLOCK = 3,     // Have the full block
        PRUNED = 4,         // Had block but it's been pruned
        HAVE_UTXOS = 5,     // Have unspent UTXOs from this block
        WALLET_UTXOS = 6    // Have unspent UTXOs from this block that we own
    };

    static BlockStatusCache& getInstance();

    void populateFromBlockIndex(class ChainstateManager& chainman);
    BlockStatus getStatus(int height) const;
    bool isPopulated() const { return m_populated.load(); }
    int getTotalBlocks() const { return m_totalBlocks.load(); }
    void clear();

private:
    BlockStatusCache() = default;
    std::map<int, BlockStatus> m_statusCache;
    std::atomic<bool> m_populated{false};
    std::atomic<int> m_totalBlocks{0};
};

// Function to be called from block loading process
void PopulateBlockStatusCacheFromBlockIndex(class ChainstateManager& chainman);

#endif // BITCOIN_BLOCKSTATUS_CACHE_H
