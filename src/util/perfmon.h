#ifndef BITCOIN_UTIL_PERFMON_H
#define BITCOIN_UTIL_PERFMON_H

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <sys/time.h>
#include <sys/resource.h>
#include <unistd.h> // for sysconf
#include <sched.h>  // for sched_getcpu
#include <algorithm> // for std::find and std::sort
#include <logging.h>

// Forward declare logging function
void LogPerfStats();

class PerfMonitor {
public:
    struct ThreadStats {
        std::chrono::microseconds total_time{0};
        std::chrono::microseconds max_time{0};
        std::chrono::microseconds min_time{std::chrono::microseconds::max()};
        uint64_t call_count{0};
        double cpu_usage{0.0};  // CPU usage percentage
        std::vector<int> core_affinity; // Track which cores this thread has run on
    };

    struct SectionTimer {
        PerfMonitor& monitor;
        std::string section_name;
        std::thread::id thread_id;
        std::chrono::steady_clock::time_point start_time;
        struct rusage start_usage;
        bool enabled;

        SectionTimer(PerfMonitor& m, const std::string& name, bool enabled_in = true)
            : monitor(m), section_name(name), thread_id(std::this_thread::get_id()),
              enabled(enabled_in)
        {
            if (!enabled) return;
            start_time = std::chrono::steady_clock::now();
            getrusage(RUSAGE_THREAD, &start_usage);
        }

        ~SectionTimer() {
            if (!enabled) return;
            auto end_time = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
                end_time - start_time);

            struct rusage end_usage;
            getrusage(RUSAGE_THREAD, &end_usage);

            // Calculate CPU time (user + system) in microseconds
            const int64_t cpu_user = (end_usage.ru_utime.tv_sec - start_usage.ru_utime.tv_sec) * 1000000LL +
                                     (end_usage.ru_utime.tv_usec - start_usage.ru_utime.tv_usec);
            const int64_t cpu_sys = (end_usage.ru_stime.tv_sec - start_usage.ru_stime.tv_sec) * 1000000LL +
                                    (end_usage.ru_stime.tv_usec - start_usage.ru_stime.tv_usec);
            const int64_t cpu_total = std::max<int64_t>(0, cpu_user + cpu_sys);

            double cpu_percentage{0.0};
            if (duration.count() > 0) {
                cpu_percentage = 100.0 * static_cast<double>(cpu_total) / static_cast<double>(duration.count());
            }

            // Get current CPU core (approximate since thread might move between cores)
            int current_core = sched_getcpu();
            if (current_core >= 0) {
                monitor.AddMeasurement(section_name, thread_id, duration, cpu_percentage, current_core);
            } else {
                monitor.AddMeasurement(section_name, thread_id, duration, cpu_percentage);
            }
        }
    };

    void AddMeasurement(const std::string& section, const std::thread::id& thread_id,
                       std::chrono::microseconds duration, double cpu_percentage, int core = -1) {
        if (!std::isfinite(cpu_percentage)) cpu_percentage = 0.0;
        std::lock_guard<std::mutex> lock(mutex);
        auto& stats = measurements[section][thread_id];
        stats.total_time += duration;
        stats.max_time = std::max(stats.max_time, duration);
        stats.min_time = std::min(stats.min_time, duration);
        stats.call_count++;
        // Running average of CPU usage
        stats.cpu_usage = (stats.cpu_usage * (stats.call_count - 1) + cpu_percentage) / stats.call_count;

        // Add core to affinity list if not already present
        if (core >= 0) {
            auto it = std::find(stats.core_affinity.begin(), stats.core_affinity.end(), core);
            if (it == stats.core_affinity.end()) {
                stats.core_affinity.push_back(core);
                std::sort(stats.core_affinity.begin(), stats.core_affinity.end());
            }
        }
    }

    std::string GetStats() const {
        std::lock_guard<std::mutex> lock(mutex);
        struct Row {
            std::string section_name;
            std::thread::id thread_id;
            ThreadStats stats;
        };
        std::vector<Row> rows;
        int64_t total_wall_time_us{0};

        for (const auto& section : measurements) {
            for (const auto& thread_stat : section.second) {
                rows.push_back(Row{section.first, thread_stat.first, thread_stat.second});
                total_wall_time_us += thread_stat.second.total_time.count();
            }
        }

        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
            if (a.stats.total_time != b.stats.total_time) return a.stats.total_time > b.stats.total_time;
            return a.section_name < b.section_name;
        });

        std::string result;
        for (const Row& row : rows) {
            const auto& stats = row.stats;
            const int64_t count = stats.call_count ? static_cast<int64_t>(stats.call_count) : 1;
            const int64_t avg_us = stats.total_time.count() / count;
            const int64_t min_us = stats.call_count ? stats.min_time.count() : 0;
            const double wall_share = total_wall_time_us > 0 ?
                (100.0 * static_cast<double>(stats.total_time.count()) / static_cast<double>(total_wall_time_us)) : 0.0;

            result += row.section_name + " (thread " + std::to_string(std::hash<std::thread::id>{}(row.thread_id)) + "): ";
            result += "total=" + std::to_string(stats.total_time.count()) + "us " +
                      "share=" + std::to_string(wall_share) + "% " +
                      "avg/min/max=" + std::to_string(avg_us) + "us" +
                      "/" + std::to_string(min_us) + "us" +
                      "/" + std::to_string(stats.max_time.count()) + "us " +
                      "count=" + std::to_string(stats.call_count) + " " +
                      "CPU%=" + std::to_string(stats.cpu_usage);

            if (!stats.core_affinity.empty()) {
                result += " (cores: ";
                for (size_t i = 0; i < stats.core_affinity.size(); ++i) {
                    if (i > 0) result += ",";
                    result += std::to_string(stats.core_affinity[i]);
                }
                result += ")";
            }
            result += "\n";
        }
        return result;
    }

    void Reset() { std::lock_guard<std::mutex> lock(mutex); measurements.clear(); }

    static PerfMonitor& Instance() { static PerfMonitor instance; return instance; }

private:
    mutable std::mutex mutex;
    std::map<std::string, std::map<std::thread::id, ThreadStats>> measurements;
};

// Macro for easy performance monitoring
#define PERF_MONITOR(name) \
    PerfMonitor::SectionTimer perf_timer##__LINE__(PerfMonitor::Instance(), name, LogAcceptCategory(BCLog::PERFMON))

#endif // BITCOIN_UTIL_PERFMON_H
