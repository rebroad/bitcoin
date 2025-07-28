// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_UTIL_TIME_H
#define BITCOIN_UTIL_TIME_H

#include <compat.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <stdint.h>
#include <string>

using namespace std::chrono_literals;

void UninterruptibleSleep(const std::chrono::microseconds& n);

/**
 * Helper to count the seconds of a duration.
 *
 * All durations should be using std::chrono and calling this should generally
 * be avoided in code. Though, it is still preferred to an inline t.count() to
 * protect against a reliance on the exact type of t.
 *
 * This helper is used to convert durations before passing them over an
 * interface that doesn't support std::chrono (e.g. RPC, debug log, or the GUI)
 */
constexpr int64_t count_seconds(std::chrono::seconds t) { return t.count(); }
constexpr int64_t count_milliseconds(std::chrono::milliseconds t) { return t.count(); }
constexpr int64_t count_microseconds(std::chrono::microseconds t) { return t.count(); }

using SecondsDouble = std::chrono::duration<double, std::chrono::seconds::period>;

/**
 * Helper to count the seconds in any std::chrono::duration type
 */
inline double CountSecondsDouble(SecondsDouble t) { return t.count(); }

/**
 * DEPRECATED
 * Use either GetTimeSeconds (not mockable) or GetTime<T> (mockable)
 */
int64_t GetTime();

/** Returns the system time (not mockable) */
int64_t GetTimeMillis();
/** Returns the system time (not mockable) */
int64_t GetTimeMicros();
/** Returns the system time (not mockable) */
int64_t GetTimeSeconds(); // Like GetTime(), but not mockable

/**
 * DEPRECATED
 * Use SetMockTime with chrono type
 *
 * @param[in] nMockTimeIn Time in seconds.
 */
void SetMockTime(int64_t nMockTimeIn);

/** For testing. Set e.g. with the setmocktime rpc, or -mocktime argument */
void SetMockTime(std::chrono::seconds mock_time_in);

/** For testing */
std::chrono::seconds GetMockTime();

/** Return system time (or mocked time, if set) */
template <typename T>
T GetTime();

/**
 * ISO 8601 formatting is preferred. Use the FormatISO8601{DateTime,Date}
 * helper functions if possible.
 */
std::string FormatISO8601DateTime(int64_t nTime);
std::string FormatISO8601Date(int64_t nTime);
std::string FormatISO8601Time(int64_t nTime);
int64_t ParseISO8601DateTime(const std::string& str);

/**
 * Convert milliseconds to a struct timeval for e.g. select.
 */
struct timeval MillisToTimeval(int64_t nTimeout);

/**
 * Convert milliseconds to a struct timeval for e.g. select.
 */
struct timeval MillisToTimeval(std::chrono::milliseconds ms);

/** Sanity check epoch match normal Unix epoch */
bool ChronoSanityCheck();

/**
 * Get the CPU time spent by the current thread.
 */
std::chrono::nanoseconds ThreadCpuTime();

/**
 * Measure CPU time spent by the current thread.
 * A clock is started when a CpuTimer is created. When the object is destroyed
 * the elapsed CPU time is valculated and a callback function is invoked,
 * providing it the elapsed CPU time.
 */
class CpuTimer {
public:
    using FinishedCB = std::function<void(std::chrono::nanoseconds)>;

    CpuTimer(const FinishedCB& finished_cb) : m_start{ThreadCpuTime()}, m_finished_cb{finished_cb} {}

    ~CpuTimer()
    {
        if (m_finished_cb) {
            m_finished_cb(ThreadCpuTime() - m_start);
        }
    }

private:
    const std::chrono::nanoseconds m_start;
    const FinishedCB m_finished_cb;
};

inline std::atomic<std::chrono::nanoseconds>& operator+=(std::atomic<std::chrono::nanoseconds>& a, std::chrono::nanoseconds b)
{
    std::chrono::nanoseconds expected;
    std::chrono::nanoseconds desired;
    do {
        expected = a.load();
        desired = expected + b;
    } while (!a.compare_exchange_weak(expected, desired));
    return a;
}

#endif // BITCOIN_UTIL_TIME_H
