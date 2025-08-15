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

void BlockStatusCache::addBlock(int height, CBlockIndex* pindex)
{
    // Add a single block to the cache
    if (!pindex) {
        m_statusCache[height] = NO_HEADER;
        return;
    }

    // Check if we have block data
    if (pindex->nStatus & BLOCK_HAVE_DATA) {
        m_statusCache[height] = HAVE_BLOCK;
    } else {
        // We have the header but no block data
        m_statusCache[height] = HEADER_ONLY;
    }

    // Update the total blocks count if this is higher
    int currentMax = m_totalBlocks.load();
    if (height > currentMax) {
        m_totalBlocks.store(height);
    }
}

BlockStatusCache::BlockStatus BlockStatusCache::getStatus(int height) const
{
    auto it = m_statusCache.find(height);
    if (it != m_statusCache.end()) {
        return it->second;
    }
    printf("BlockStatusCache::getStatus: no status found for height %d\n", height);
    return UNKNOWN;
}

void BlockStatusCache::clear()
{
    m_statusCache.clear();
    m_populated.store(false);
    m_totalBlocks.store(0);
}

