// Copyright (c) 2024 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_LOCKTIMING_H
#define BITCOIN_QT_LOCKTIMING_H

#include <QElapsedTimer>
#include <QDebug>
#include <sync.h>

// Macro to time cs_main lock attempts
#define TIME_CS_MAIN_LOCK() \
    QElapsedTimer lockTimer; \
    lockTimer.start(); \
    std::unique_lock<RecursiveMutex> lock(cs_main, std::try_to_lock); \
    qint64 waited = lockTimer.nsecsElapsed() / 1000000; \
    if (waited > 0 || !lock.owns_lock()) { \
        qDebug() << "[LOCK_TIMED] cs_main try_to_lock at" << __FILE__ << ":" << __LINE__ << __FUNCTION__ \
                 << (lock.owns_lock() ? "ACQUIRED" : "FAILED") << "after" << waited << "ms"; \
    }

#endif // BITCOIN_QT_LOCKTIMING_H 