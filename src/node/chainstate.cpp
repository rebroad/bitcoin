// Copyright (c) 2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/chainstate.h>

#include <chainparams.h>
#include <consensus/params.h>
#include <node/blockstorage.h>
#include <util/system.h>
#include <validation.h>

namespace node {
std::optional<ChainstateLoadingError> LoadChainstate(bool fReset,
                                                     ChainstateManager& chainman,
                                                     CTxMemPool* mempool,
                                                     bool fPruneMode,
                                                     const Consensus::Params& consensus_params,
                                                     bool fReindexChainState,
                                                     int64_t nBlockTreeDBCache,
                                                     int64_t nCoinDBCache,
                                                     int64_t nCoinCacheUsage,
                                                     bool block_tree_db_in_memory,
                                                     bool coins_db_in_memory,
                                                     std::function<bool()> shutdown_requested,
                                                     std::function<void()> coins_error_cb)
{
    auto is_coinsview_empty = [&](CChainState* chainstate) EXCLUSIVE_LOCKS_REQUIRED(::cs_main) {
        return fReset || fReindexChainState || chainstate->CoinsTip().GetBestBlock().IsNull();
    };

    LOCK(cs_main);
    chainman.InitializeChainstate(mempool);
    chainman.m_total_coinstip_cache = nCoinCacheUsage;
    chainman.m_total_coinsdb_cache = nCoinDBCache;

    UnloadBlockIndex(mempool, chainman);

    auto& pblocktree{chainman.m_blockman.m_block_tree_db};
    // new CBlockTreeDB tries to delete the existing file, which
    // fails if it's still open from the previous loop. Close it first:
    pblocktree.reset();
    pblocktree.reset(new CBlockTreeDB(nBlockTreeDBCache, block_tree_db_in_memory, fReset));

    if (fReset) {
        pblocktree->WriteReindexing(true);
        //If we're reindexing in prune mode, wipe away unusable block files and all undo data files
        if (fPruneMode)
            CleanupBlockRevFiles();
    }

    if (shutdown_requested && shutdown_requested()) return ChainstateLoadingError::SHUTDOWN_PROBED;

    // LoadBlockIndex will load fHavePruned if we've ever removed a
    // block file from disk.
    // Note that it also sets fReindex based on the disk flag!
    // From here on out fReindex and fReset mean something different!
    if (!chainman.LoadBlockIndex()) {
        if (shutdown_requested && shutdown_requested()) return ChainstateLoadingError::SHUTDOWN_PROBED;
        LogPrintf("%s: chainman.LoadBlockIndex() failed\n", __func__);
        return ChainstateLoadingError::ERROR_LOADING_BLOCK_DB;
    }

    if (!chainman.BlockIndex().empty() &&
            !chainman.m_blockman.LookupBlockIndex(consensus_params.hashGenesisBlock)) {
        LogPrintf("%s: missing genesis hash %s from non-empty block index (size=%u)\n",
                  __func__,
                  consensus_params.hashGenesisBlock.ToString(),
                  static_cast<unsigned int>(chainman.BlockIndex().size()));
        return ChainstateLoadingError::ERROR_BAD_GENESIS_BLOCK;
    }

    // Historically we required a reindex when switching from pruned to unpruned.
    // Missing historical blocks can now be fetched after startup, so proceed.

    // At this point blocktree args are consistent with what's on disk.
    // If we're not mid-reindex (based on disk + args), add a genesis block on disk
    // (otherwise we use the one already on disk).
    // This is called again in ThreadImport after the reindex completes.
    if (!fReindex && !chainman.ActiveChainstate().LoadGenesisBlock()) {
        LogPrintf("%s: LoadGenesisBlock() failed while not reindexing\n", __func__);
        return ChainstateLoadingError::ERROR_LOAD_GENESIS_BLOCK_FAILED;
    }

    // At this point we're either in reindex or we've loaded a useful
    // block tree into BlockIndex()!

    for (CChainState* chainstate : chainman.GetAll()) {
        chainstate->InitCoinsDB(
            /* cache_size_bytes */ nCoinDBCache,
            /* in_memory */ coins_db_in_memory,
            /* should_wipe */ fReset || fReindexChainState);

        if (coins_error_cb) {
            chainstate->CoinsErrorCatcher().AddReadErrCallback(coins_error_cb);
        }

        // If necessary, upgrade from older database format.
        // This is a no-op if we cleared the coinsviewdb with -reindex or -reindex-chainstate
        if (!chainstate->CoinsDB().Upgrade()) {
            LogPrintf("%s: CoinsDB().Upgrade() failed\n", __func__);
            return ChainstateLoadingError::ERROR_CHAINSTATE_UPGRADE_FAILED;
        }

        // ReplayBlocks is a no-op if we cleared the coinsviewdb with -reindex or -reindex-chainstate
        if (!chainstate->ReplayBlocks()) {
            LogPrintf("%s: ReplayBlocks() failed\n", __func__);
            return ChainstateLoadingError::ERROR_REPLAYBLOCKS_FAILED;
        }

        // The on-disk coinsdb is now in a good state, create the cache
        chainstate->InitCoinsCache(nCoinCacheUsage);
        assert(chainstate->CanFlushToDisk());

        if (!is_coinsview_empty(chainstate)) {
            // LoadChainTip initializes the chain based on CoinsTip()'s best block
            if (!chainstate->LoadChainTip()) {
                const uint256 coins_tip_hash = chainstate->CoinsTip().GetBestBlock();
                if (gArgs.IsArgSet("-bootstrap")) {
                    chainman.SetBootstrapChainstateTipHash(coins_tip_hash);
                    CBlockIndex* genesis_index = chainman.m_blockman.LookupBlockIndex(consensus_params.hashGenesisBlock);
                    if (genesis_index) {
                        chainstate->m_chain.SetTip(genesis_index);
                    }
                    LogPrintf("%s: deferring LoadChainTip() for coins tip hash %s while bootstrapping headers via -bootstrap\n",
                              __func__, coins_tip_hash.ToString());
                } else {
                    LogPrintf("%s: LoadChainTip() failed for coins tip hash %s\n",
                              __func__, coins_tip_hash.ToString());
                    return ChainstateLoadingError::ERROR_LOADCHAINTIP_FAILED;
                }
            }
            if (!chainman.IsBootstrapChainstatePending()) {
                assert(chainstate->m_chain.Tip() != nullptr);
            }
        } else
            LogPrintf("%s: Skipped LoadChainTip()\n", __func__);
    }

    if (!fReset) {
        auto chainstates{chainman.GetAll()};
        if (std::any_of(chainstates.begin(), chainstates.end(),
                        [](const CChainState* cs) EXCLUSIVE_LOCKS_REQUIRED(cs_main) { return cs->NeedsRedownload(); })) {
            LogPrintf("%s: chainstate requires redownload due to insufficient witness validation\n", __func__);
            return ChainstateLoadingError::ERROR_BLOCKS_WITNESS_INSUFFICIENTLY_VALIDATED;
        }
    }

    return std::nullopt;
}

std::optional<ChainstateLoadVerifyError> VerifyLoadedChainstate(ChainstateManager& chainman,
                                                                bool fReset,
                                                                bool fReindexChainState,
                                                                const Consensus::Params& consensus_params,
                                                                unsigned int check_blocks,
                                                                unsigned int check_level,
                                                                std::function<int64_t()> get_unix_time_seconds)
{
    auto is_coinsview_empty = [&](CChainState* chainstate) EXCLUSIVE_LOCKS_REQUIRED(::cs_main) {
        return fReset || fReindexChainState || chainstate->CoinsTip().GetBestBlock().IsNull();
    };

    LOCK(cs_main);

    for (CChainState* chainstate : chainman.GetAll()) {
        if (!is_coinsview_empty(chainstate)) {
            const CBlockIndex* tip = chainstate->m_chain.Tip();
            if (tip && tip->nTime > get_unix_time_seconds() + MAX_FUTURE_BLOCK_TIME) {
                return ChainstateLoadVerifyError::ERROR_BLOCK_FROM_FUTURE;
            }

            if (!CVerifyDB().VerifyDB(
                    *chainstate, consensus_params, chainstate->CoinsDB(),
                    check_level,
                    check_blocks)) {
                return ChainstateLoadVerifyError::ERROR_CORRUPTED_BLOCK_DB;
            }
        }
    }

    return std::nullopt;
}
} // namespace node
