// Copyright (c) 2016 The Bitcoin Unlimited developers
// Copyright (c) 2016 Tom Zander <tomz@freedommail.ch>
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "thinblock.h"
#include <sstream>
#include <iomanip>

#include "main.h"
#include "chainparams.h"
#include "txmempool.h"

#include <consensus/validation.h>
#include <boost/foreach.hpp>
#include <boost/thread.hpp>


std::map<uint256, uint64_t> mapThinBlockTimer;

CXThinBlock::CXThinBlock(const CBlock& block, CBloomFilter* filter)
{
    header = block.GetBlockHeader();
    this->collision = false;

    unsigned int nTx = block.vtx.size();
    vTxHashes.reserve(nTx);
    std::set<uint64_t> setPartialTxHash;
    for (unsigned int i = 0; i < nTx; i++)
    {
        const uint256 hash256 = block.vtx[i].GetHash();
        uint64_t cheapHash = hash256.GetCheapHash();
        vTxHashes.push_back(cheapHash);

        if (setPartialTxHash.count(cheapHash))
                this->collision = true;
        setPartialTxHash.insert(cheapHash);

        // Find the transactions that do not match the filter.
        // These are the ones we need to relay back to the requesting peer.
        // NOTE: We always add the first tx, the coinbase as it is the one
        //       most often missing.
        if ((filter && !filter->contains(hash256)) || i == 0)
            vMissingTx.push_back(block.vtx[i]);
    }
}

CXThinBlock::CXThinBlock(const CBlock& block)
{
    header = block.GetBlockHeader();
    this->collision = false;

    unsigned int nTx = block.vtx.size();
    vTxHashes.reserve(nTx);
    std::set<uint64_t> setPartialTxHash;

    for (unsigned int i = 1; i < nTx; i++)
    {
        const uint256 hash256 = block.vtx[i].GetHash();
        uint64_t cheapHash = hash256.GetCheapHash();
        vTxHashes.push_back(cheapHash);

        if (setPartialTxHash.count(cheapHash))
                this->collision = true;
        setPartialTxHash.insert(cheapHash);

        // We always add the first tx, the coinbase as it is the one
        // most often missing.
        if (i == 0) vMissingTx.push_back(block.vtx[i]);
    }
}

CXThinBlockTx::CXThinBlockTx(uint256 blockHash, std::vector<CTransaction>& vTx)
{
    blockhash = blockHash;
    vMissingTx = vTx;
}

CXRequestThinBlockTx::CXRequestThinBlockTx(uint256 blockHash, std::set<uint64_t>& setHashesToRequest)
{
    blockhash = blockHash;
    setCheapHashesToRequest = setHashesToRequest;
}

bool HaveThinblockNodes()
{
    LOCK(cs_vNodes);
    BOOST_FOREACH (CNode* pnode, vNodes) {
        if (pnode->ThinBlockCapable())
            return true;
    }
    return false;
}

bool CheckThinblockTimer(const uint256 &hash)
{
    if (!mapThinBlockTimer.count(hash)) {
        mapThinBlockTimer[hash] = GetTimeMillis();
        LogPrint("thin", "Starting Preferential Thinblock timer\n");
    }
    else {
        // Check that we have not exceeded the 10 second limit.
        // If we have then we want to return false so that we can
        // proceed to download a regular block instead.
        uint64_t elapsed = GetTimeMillis() - mapThinBlockTimer[hash];
        if (elapsed > 10000) {
            LogPrint("thin", "Preferential Thinblock timer exceeded - downloading regular block instead\n");
            return false;
        }
    }
    return true;
}

bool IsChainNearlySyncd()
{
    LOCK(cs_main);
    if(chainActive.Height() < pindexBestHeader->nHeight - 2)
        return false;
    return true;
}

CBloomFilter createSeededBloomFilter(const std::vector<uint256>& vOrphanHashes)
{
    LogPrint("thin", "Starting creation of bloom filter\n");
    seed_insecure_rand();
    double nBloomPoolSize = (double)mempool.mapTx.size();
    if (nBloomPoolSize > MAX_BLOOM_FILTER_SIZE / 1.8)
        nBloomPoolSize = MAX_BLOOM_FILTER_SIZE / 1.8;
    double nBloomDecay = 1.5 - (nBloomPoolSize * 1.8 / MAX_BLOOM_FILTER_SIZE);  // We should never go below 0.5 as we will start seeing re-requests for tx's
    int nElements = std::max((int)(((int)mempool.mapTx.size() + (int)vOrphanHashes.size()) * nBloomDecay), 1); // Must make sure nElements is greater than zero or will assert
    double nFPRate = .001 + (((double)nElements * 1.8 / MAX_BLOOM_FILTER_SIZE) * .004); // The false positive rate in percent decays as the mempool grows
    CBloomFilter filterMemPool(nElements, nFPRate, insecure_rand(), BLOOM_UPDATE_ALL);
    LogPrint("thin", "Bloom multiplier: %f FPrate: %f Num elements in bloom filter: %d num mempool entries: %d\n", nBloomDecay, nFPRate, nElements, (int)mempool.mapTx.size());

    // Seed the filter with the transactions in the memory pool
    LOCK(cs_main);
    std::vector<uint256> vMemPoolHashes;
    mempool.queryHashes(vMemPoolHashes);
    for (uint64_t i = 0; i < vMemPoolHashes.size(); i++)
         filterMemPool.insert(vMemPoolHashes[i]);
    for (uint64_t i = 0; i < vOrphanHashes.size(); i++)
         filterMemPool.insert(vOrphanHashes[i]);
    LogPrint("thin", "Created bloom filter: %d bytes\n",::GetSerializeSize(filterMemPool, SER_NETWORK, PROTOCOL_VERSION));

    return filterMemPool;
}

void LoadFilter(CNode *pfrom, CBloomFilter *filter)
{
    if (!filter->IsWithinSizeConstraints())
        // There is no excuse for sending a too-large filter
        Misbehaving(pfrom->GetId(), 100);
    else
    {
        LOCK(pfrom->cs_filter);
        delete pfrom->pThinBlockFilter;
        pfrom->pThinBlockFilter = new CBloomFilter(*filter);
        pfrom->pThinBlockFilter->UpdateEmptyFull();
    }
    uint64_t nSizeFilter = ::GetSerializeSize(*pfrom->pThinBlockFilter, SER_NETWORK, PROTOCOL_VERSION);
    LogPrint("thin", "Thinblock Bloom filter size: %d\n", nSizeFilter);
}

void HandleBlockMessage(CNode *pfrom, const std::string &strCommand, const CBlock &block, const CInv &inv)
{
    int64_t startTime = GetTimeMicros();
    CValidationState state;
    // Process all blocks from whitelisted peers, even if not requested,
    // unless we're still syncing with the network.
    // Such an unrequested block may still be processed, subject to the
    // conditions in AcceptBlock().
    bool forceProcessing = pfrom->fWhitelisted && !IsInitialBlockDownload();
    const CChainParams& chainparams = Params();
    ProcessNewBlock(state, chainparams, pfrom, &block, forceProcessing, NULL);
    int nDoS;
    if (state.IsInvalid(nDoS)) {
        LogPrintf("Invalid block due to %s\n", state.GetRejectReason().c_str());
        pfrom->PushMessage("reject", strCommand, (unsigned char)state.GetRejectCode(),
                           state.GetRejectReason().substr(0, MAX_REJECT_MESSAGE_LENGTH), inv.hash);
        if (nDoS > 0) {
            LOCK(cs_main);
            Misbehaving(pfrom->GetId(), nDoS);
        }
    }
    //LogPrint("thin", "Processed Block %s in %.2f seconds\n", inv.hash.ToString(), (double)(GetTimeMicros() - startTime) / 1000000.0);

    // When we request a thinblock we may get back a regular block if it is smaller than a thinblock
    // Therefore we have to remove the thinblock in flight if it exists and we also need to check that
    // the block didn't arrive from some other peer.  This code ALSO cleans up the thin block that
    // was passed to us (&block), so do not use it after this.
    {
        int nTotalThinBlocksInFlight = 0;
        LOCK(cs_vNodes);
        BOOST_FOREACH(CNode* pnode, vNodes) {
            if (pnode->mapThinBlocksInFlight.count(inv.hash)) {
                pnode->mapThinBlocksInFlight.erase(inv.hash);
                pnode->thinBlockWaitingForTxns = -1;
                pnode->thinBlock.SetNull();
            }
            if (pnode->mapThinBlocksInFlight.size() > 0)
                nTotalThinBlocksInFlight++;
        }

        // When we no longer have any thinblocks in flight then clear the set
        // just to make sure we don't somehow get growth over time.
        if (nTotalThinBlocksInFlight == 0) {
            setPreVerifiedTxHash.clear();
            setUnVerifiedOrphanTxHash.clear();
        }
    }

    // Clear the thinblock timer used for preferential download
    mapThinBlockTimer.erase(inv.hash);
}
