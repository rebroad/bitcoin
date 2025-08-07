// Copyright (c) 2024 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_LOCKTIMING_H
#define BITCOIN_QT_LOCKTIMING_H

#include <QElapsedTimer>
#include <QDebug>
#include <QDateTime>
#include <QThread>
#include <sync.h>
#include <limits>
#include <chrono>

// Forward declaration for GUI state management
void UpdateGuiLastUsed();

// Global heartbeat timer (shared across all macro instances)
extern QElapsedTimer g_gui_heartbeat_timer;
extern bool g_gui_heartbeat_initialized;

// Macro to time cs_main lock attempts with rate limiting
#define TIME_CS_MAIN_LOCK(maxWaitMs) \
    UpdateGuiLastUsed(); \
    if (!g_gui_heartbeat_initialized) { \
        g_gui_heartbeat_timer.start(); \
        g_gui_heartbeat_initialized = true; \
    } \
    qint64 heartbeatDelay = g_gui_heartbeat_timer.nsecsElapsed() / 1000000; \
    if (heartbeatDelay >= 1000) { /* Every second */ \
        QString threadName = QThread::currentThread()->objectName().isEmpty() ? "unnamed" : QThread::currentThread()->objectName(); \
        qDebug() << "[GUI_HEARTBEAT] GUI thread alive at" << QDateTime::currentDateTime().toString("hh:mm:ss.zzz") \
                << "from" << __FILE__ << ":" << __LINE__ << __FUNCTION__ << "[" << threadName << "]" \
                << "(delay:" << heartbeatDelay << "ms)"; \
        g_gui_heartbeat_timer.restart(); \
    } \
    QElapsedTimer lockTimer; \
    lockTimer.start(); \
    std::unique_lock<RecursiveMutex> lock(cs_main, std::try_to_lock); \
    qint64 waited = lockTimer.nsecsElapsed() / 1000000; \
    /* Try again with small delays until maxWaitMs or lock acquired */ \
    while (!lock.owns_lock() && waited < maxWaitMs) { \
        std::this_thread::sleep_for(std::chrono::milliseconds(1)); \
        lockTimer.restart(); \
        lock = std::unique_lock<RecursiveMutex>(cs_main, std::try_to_lock); \
        waited += lockTimer.nsecsElapsed() / 1000000; \
    } \
    { \
        static QElapsedTimer lastLogTimer; \
        static QElapsedTimer lastSuccessTimer; \
        static QElapsedTimer lastFailureTimer; \
        static int successCount = 0; \
        static int failureCount = 0; \
        static qint64 maxWaitTime = 0; \
        static qint64 minSuccessInterval = -1; \
        static qint64 maxSuccessInterval = 0; \
        static qint64 minFailureInterval = -1; \
        static qint64 maxFailureInterval = 0; \
        static bool timerInitialized = false; \
        static bool successTimerInitialized = false; \
        static bool failureTimerInitialized = false; \
        if (!timerInitialized) { \
            lastLogTimer.start(); \
            timerInitialized = true; \
        } \
        if (!successTimerInitialized) { \
            lastSuccessTimer.start(); \
            successTimerInitialized = true; \
        } \
        if (!failureTimerInitialized) { \
            lastFailureTimer.start(); \
            failureTimerInitialized = true; \
        } \
        if (waited > maxWaitTime) maxWaitTime = waited; \
        if (lock.owns_lock()) { \
            successCount++; \
            qint64 timeSinceLastSuccess = lastSuccessTimer.nsecsElapsed() / 1000000; \
            if (minSuccessInterval == -1 || timeSinceLastSuccess < minSuccessInterval) minSuccessInterval = timeSinceLastSuccess; \
            if (timeSinceLastSuccess > maxSuccessInterval) maxSuccessInterval = timeSinceLastSuccess; \
            lastSuccessTimer.restart(); \
        } else { \
            failureCount++; \
            qint64 timeSinceLastFailure = lastFailureTimer.nsecsElapsed() / 1000000; \
            if (minFailureInterval == -1 || timeSinceLastFailure < minFailureInterval) minFailureInterval = timeSinceLastFailure; \
            if (timeSinceLastFailure > maxFailureInterval) maxFailureInterval = timeSinceLastFailure; \
            lastFailureTimer.restart(); \
        } \
        qint64 timeSinceLastLog = lastLogTimer.nsecsElapsed() / 1000000; \
        if (timeSinceLastLog >= 1000) { \
            QString successIntervals = (minSuccessInterval == -1) ? "none" : QString("%1-%2").arg(minSuccessInterval).arg(maxSuccessInterval); \
            QString failureIntervals = (minFailureInterval == -1) ? "none" : QString("%1-%2").arg(minFailureInterval).arg(maxFailureInterval); \
            QString threadName = QThread::currentThread()->objectName().isEmpty() ? "unnamed" : QThread::currentThread()->objectName(); \
            qDebug() << "[LOCK_TIMED] cs_main try_to_lock at" << __FILE__ << ":" << __LINE__ << __FUNCTION__ \
                     << "[" << threadName << "]" \
                     << (lock.owns_lock() ? "ACQUIRED" : "FAILED") << "after" << waited << "ms" \
                     << "| Successes:" << successCount << "| Failures:" << failureCount \
                     << "| Max wait:" << maxWaitTime << "ms" \
                     << "| Success intervals:" << successIntervals << "ms" \
                     << "| Failure intervals:" << failureIntervals << "ms" \
                     << "| Heartbeat delay:" << heartbeatDelay << "ms"; \
            lastLogTimer.restart(); \
            successCount = 0; \
            failureCount = 0; \
            maxWaitTime = 0; \
            minSuccessInterval = -1; \
            maxSuccessInterval = 0; \
            minFailureInterval = -1; \
            maxFailureInterval = 0; \
        } else { \
            /* Log every attempt for debugging */ \
            QString threadName = QThread::currentThread()->objectName().isEmpty() ? "unnamed" : QThread::currentThread()->objectName(); \
            qDebug() << "[LOCK_TIMED] cs_main try_to_lock at" << __FILE__ << ":" << __LINE__ << __FUNCTION__ \
                     << "[" << threadName << "]" \
                     << (lock.owns_lock() ? "ACQUIRED" : "FAILED") << "after" << waited << "ms" \
                     << "| Heartbeat delay:" << heartbeatDelay << "ms"; \
        } \
    } \
    UpdateGuiLastUsed();

#endif // BITCOIN_QT_LOCKTIMING_H 
