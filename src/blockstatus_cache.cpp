#include <blockstatus_cache.h>
#include <validation.h>
#include <chain.h>
#include <node/blockstorage.h>
#include <node/context.h>

// Global cache instance
BlockStatusCache& BlockStatusCache::getInstance()
{
    static BlockStatusCache instance;
    return instance;
}

void BlockStatusCache::populateFromBlockIndex(ChainstateManager& chainman)
{
    // This method populates the cache from the block index during startup
    // It's called from the block loading process

    LOCK(cs_main);

    // Clear any existing data
    m_statusCache.clear();

    // Get the active chain
    const CChain& active = chainman.ActiveChain();

    // Get the total number of blocks
    int totalBlocks = active.Height();
    m_totalBlocks.store(totalBlocks);

    // Populate the cache for all blocks
    for (int height = 0; height <= totalBlocks; ++height) {
        CBlockIndex* pindex = active[height];
        if (!pindex) {
            m_statusCache[height] = NO_HEADER;
            continue;
        }

        // Check if we have block data
        if (pindex->nStatus & BLOCK_HAVE_DATA) {
            m_statusCache[height] = HAVE_BLOCK;
        } else {
            // We have the header but no block data
            m_statusCache[height] = HEADER_ONLY;
        }
    }

    // Mark as populated
    m_populated.store(true);
}

BlockStatusCache::BlockStatus BlockStatusCache::getStatus(int height) const
{
    auto it = m_statusCache.find(height);
    if (it != m_statusCache.end()) {
        return it->second;
    }
    return UNKNOWN;
}

void BlockStatusCache::clear()
{
    m_statusCache.clear();
    m_populated.store(false);
    m_totalBlocks.store(0);
}

void PopulateBlockStatusCacheFromBlockIndex(ChainstateManager& chainman)
{
    // This function is called from the block loading process
    BlockStatusCache& cache = BlockStatusCache::getInstance();
    cache.populateFromBlockIndex(chainman);
}
