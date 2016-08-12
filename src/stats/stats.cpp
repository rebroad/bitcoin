// Copyright (c) 2016 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "stats/stats.h"

#include "memusage.h"
#include "utiltime.h"

#include "util.h"

const uint32_t CStats::SAMPLE_MIN_DELTA_IN_SEC = 2;
const int CStats::CLEANUP_SAMPLES_THRESHOLD = 100;
size_t CStats::maxStatsMemory = 0;
const size_t CStats::DEFAULT_MAX_STATS_MEMORY = 10 * 1024 * 1024; //10 MB
const bool CStats::DEFAULT_STATISTICS_ENABLED = false;
std::atomic<bool> CStats::statsEnabled(false); //disable stats by default

CStats* CStats::sharedInstance = NULL;

CStats* CStats::DefaultStats()
{
    if (!sharedInstance)
        sharedInstance = new CStats();

    return sharedInstance;
}

void CStats::addMempoolSample(int64_t txcount, int64_t dynUsage, int64_t currentMinRelayFee)
{
    if (!statsEnabled)
        return;

    uint64_t now = GetTime();
    {
        LOCK(cs_stats);

        // set the mempool stats start time if this is the first sample
        if (mempoolStats.startTime == 0)
            mempoolStats.startTime = now;

        // ensure the minimum time delta between samples
        if (mempoolStats.vSamples.size() && mempoolStats.vSamples.back().timeDelta + SAMPLE_MIN_DELTA_IN_SEC >= now - mempoolStats.startTime)
            return;

        // calculate the current time delta and add a sample
        uint32_t timeDelta = now - mempoolStats.startTime; //truncate to uint32_t should be sufficient
        mempoolStats.vSamples.push_back({timeDelta, txcount, dynUsage, currentMinRelayFee});
        mempoolStats.cleanupCounter++;

        // check if we should cleanup the container
        if (mempoolStats.cleanupCounter >= CLEANUP_SAMPLES_THRESHOLD) {
            //check memory usage
            int32_t memDelta = memusage::DynamicUsage(mempoolStats.vSamples) - maxStatsMemory;
            if (memDelta > 0 && mempoolStats.vSamples.size()) {
                // only shrink if the vector.capacity() is > the target for performance reasons
                mempoolStats.vSamples.shrink_to_fit();
                int32_t memUsage = memusage::DynamicUsage(mempoolStats.vSamples);
                // calculate the amount of samples we need to remove
                size_t itemsToRemove = ceil((memUsage - maxStatsMemory) / sizeof(mempoolStats.vSamples[0]));

                // make sure the vector contains more items then we'd like to remove
                if (mempoolStats.vSamples.size() > itemsToRemove)
                    mempoolStats.vSamples.erase(mempoolStats.vSamples.begin(), mempoolStats.vSamples.begin() + itemsToRemove);
            }
            // shrink vector
            mempoolStats.vSamples.shrink_to_fit();
            mempoolStats.cleanupCounter = 0;
        }

        // fire signal
        MempoolStatsDidChange();
    }
}

mempoolSamples_t CStats::mempoolGetValuesInRange(uint64_t& fromTime, uint64_t& toTime)
{
    if (!statsEnabled)
        return mempoolSamples_t();

    LOCK(cs_stats);

    // if empty, return directly
    if (!mempoolStats.vSamples.size())
        return mempoolStats.vSamples;


    if (!(fromTime == 0 && toTime == 0) && (fromTime > mempoolStats.startTime + mempoolStats.vSamples.front().timeDelta || toTime < mempoolStats.startTime + mempoolStats.vSamples.back().timeDelta)) {
        mempoolSamples_t::iterator fromSample = mempoolStats.vSamples.begin();
        mempoolSamples_t::iterator toSample = std::prev(mempoolStats.vSamples.end());

        // create subset of samples
        bool fromSet = false;
        for (mempoolSamples_t::iterator it = mempoolStats.vSamples.begin(); it != mempoolStats.vSamples.end(); ++it) {
            if (mempoolStats.startTime + (*it).timeDelta >= fromTime && !fromSet) {
                fromSample = it;
                fromSet = true;
            }
            else if (mempoolStats.startTime + (*it).timeDelta > toTime) {
                toSample = std::prev(it);
                break;
            }
        }

        mempoolSamples_t subset(fromSample, toSample + 1);

        // set the fromTime and toTime pass-by-ref parameters
        fromTime = mempoolStats.startTime + (*fromSample).timeDelta;
        toTime = mempoolStats.startTime + (*toSample).timeDelta;

        // return subset
        return subset;
    }

    // return all available samples
    fromTime = mempoolStats.startTime + mempoolStats.vSamples.front().timeDelta;
    toTime = mempoolStats.startTime + mempoolStats.vSamples.back().timeDelta;
    return mempoolStats.vSamples;
}

void CStats::setMaxMemoryUsageTarget(size_t maxMem)
{
    statsEnabled = (maxMem > 0);

    LOCK(cs_stats);
    maxStatsMemory = maxMem;
}

std::string CStats::getHelpString(bool showDebug)
{
    std::string strUsage = HelpMessageGroup(_("Statistic options:"));
    strUsage += HelpMessageOpt("-statsenable=", strprintf("Enable statistics (default: %u)", DEFAULT_STATISTICS_ENABLED));
    strUsage += HelpMessageOpt("-statsmaxmemorytarget=<n>", strprintf(_("Set the memory limit target for statictics in bytes (default: %u)"), DEFAULT_MAX_STATS_MEMORY));

    return strUsage;
}

bool CStats::parameterInteraction()
{
    if (gArgs.GetBoolArg("-statsenable", DEFAULT_STATISTICS_ENABLED))
        DefaultStats()->setMaxMemoryUsageTarget(gArgs.GetArg("-statsmaxmemorytarget", DEFAULT_MAX_STATS_MEMORY));

    return true;
}
