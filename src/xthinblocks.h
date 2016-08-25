// Copyright (c) 2015 G. Andrew Stone
// Copyright (c) 2016 The Bitcoin Unlimited developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef BITCOIN_UNLIMITED_H
#define BITCOIN_UNLIMITED_H

#include "net.h"
#include "stat.h"
#include <univalue.h>
#include "util.h"
#include <vector>

class CBlock;
class CBlockIndex;
class CValidationState;
class CDiskBlockPos;
class CNode;

extern bool HaveThinblockNodes();
extern bool CheckThinblockTimer(const uint256 &hash);
inline bool IsThinBlocksEnabled()
{
    return GetBoolArg("-use-thinblocks", true);
}
extern bool IsChainNearlySyncd();
CBloomFilter createSeededBloomFilter(const std::vector<uint256>& vOrphanHashes);
extern void LoadFilter(CNode *pfrom, CBloomFilter *filter);
extern void HandleBlockMessage(CNode *pfrom, const std::string &strCommand, const CBlock &block, const CInv &inv);

// Handle receiving and sending messages from thin block capable nodes only (so that thin block nodes capable nodes are preferred)
extern bool ThinBlockMessageHandler(const std::vector<CNode*>& vNodesCopy);

// txn mempool statistics
extern CStatHistory<unsigned int, MinValMax<unsigned int> > txAdded;
extern CStatHistory<uint64_t, MinValMax<uint64_t> > poolSize;

#endif
