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
        HAVE_BLOCK = 3      // Have the full block
    };

    static BlockStatusCache& getInstance();

    void addBlock(int height, class CBlockIndex* pindex);
    void setPopulated() { m_populated.store(true); }
    BlockStatus getStatus(int height) const;
    bool isPopulated() const { return m_populated.load(); }
    int getTotalBlocks() const { return m_totalBlocks.load(); }
    size_t getCacheSize() const { return m_statusCache.size(); }
    void clear();

private:
    BlockStatusCache() = default;
    std::map<int, BlockStatus> m_statusCache;
    std::atomic<bool> m_populated{false};
    std::atomic<int> m_totalBlocks{0};
};

#endif // BITCOIN_BLOCKSTATUS_CACHE_H
