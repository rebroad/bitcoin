// Copyright (c) 2024 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_LOCKTIMING_H
#define BITCOIN_QT_LOCKTIMING_H

#include <QElapsedTimer>
#include <QDebug>
#include <sync.h>

// Macro to time cs_main lock attempts with rate limiting
#define TIME_CS_MAIN_LOCK() \
    QElapsedTimer lockTimer; \
    lockTimer.start(); \
    std::unique_lock<RecursiveMutex> lock(cs_main, std::try_to_lock); \
    qint64 waited = lockTimer.nsecsElapsed() / 1000000; \
    if (waited > 0 || !lock.owns_lock()) { \
        static QElapsedTimer lastLogTimer; \
        static QElapsedTimer lastFailureTimer; \
        static int failedCount = 0; \
        static qint64 maxWaitTime = 0; \
        static qint64 maxTimeBetweenFailures = 0; \
        static bool timerInitialized = false; \
        static bool failureTimerInitialized = false; \
        if (!timerInitialized) { \
            lastLogTimer.start(); \
            timerInitialized = true; \
        } \
        if (!failureTimerInitialized) { \
            lastFailureTimer.start(); \
            failureTimerInitialized = true; \
        } \
        failedCount++; \
        if (waited > maxWaitTime) maxWaitTime = waited; \
        qint64 timeSinceLastFailure = lastFailureTimer.nsecsElapsed() / 1000000; \
        if (timeSinceLastFailure > maxTimeBetweenFailures) { \
            maxTimeBetweenFailures = timeSinceLastFailure; \
        } \
        lastFailureTimer.restart(); \
        qint64 timeSinceLastLog = lastLogTimer.nsecsElapsed() / 1000000; \
        if (timeSinceLastLog >= 1000) { \
            qDebug() << "[LOCK_TIMED] cs_main try_to_lock at" << __FILE__ << ":" << __LINE__ << __FUNCTION__ \
                     << (lock.owns_lock() ? "ACQUIRED" : "FAILED") << "after" << waited << "ms" \
                     << "| Total attempts:" << failedCount << "| Max wait:" << maxWaitTime << "ms" \
                     << "| Max time between failures:" << maxTimeBetweenFailures << "ms" \
                     << "| Time since last log:" << timeSinceLastLog << "ms"; \
            lastLogTimer.restart(); \
            failedCount = 0; \
            maxWaitTime = 0; \
            maxTimeBetweenFailures = 0; \
        } \
    }

#endif // BITCOIN_QT_LOCKTIMING_H 