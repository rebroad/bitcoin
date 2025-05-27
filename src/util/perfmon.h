#ifndef BITCOIN_UTIL_PERFMON_H
#define BITCOIN_UTIL_PERFMON_H

#include <atomic>
#include <chrono>
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

        SectionTimer(PerfMonitor& m, const std::string& name)
            : monitor(m), section_name(name), thread_id(std::this_thread::get_id()),
              start_time(std::chrono::steady_clock::now())
        {
            getrusage(RUSAGE_THREAD, &start_usage);
        }

        ~SectionTimer() {
            auto end_time = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
                end_time - start_time);

            struct rusage end_usage;
            getrusage(RUSAGE_THREAD, &end_usage);

            // Calculate CPU time (user + system) in microseconds
            auto cpu_user = (end_usage.ru_utime.tv_sec - start_usage.ru_utime.tv_sec) * 1000000 +
                          (end_usage.ru_utime.tv_usec - start_usage.ru_utime.tv_usec);
            auto cpu_sys = (end_usage.ru_stime.tv_sec - start_usage.ru_stime.tv_sec) * 1000000 +
                         (end_usage.ru_stime.tv_usec - start_usage.ru_stime.tv_usec);

            double cpu_percentage = 100.0 * (cpu_user + cpu_sys) / duration.count();

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
        std::string result = "";

        for (const auto& section : measurements) {
            for (const auto& thread_stat : section.second) {
                result += section.first + " (thread " + std::to_string(std::hash<std::thread::id>{}(thread_stat.first)) + "): ";
                const auto& stats = thread_stat.second;
                result += "avg/min/max=" +
                        std::to_string(stats.total_time.count() / (stats.call_count ? stats.call_count : 1)) + "us" +
                        "/" + std::to_string(stats.min_time.count()) + "us" +
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
    PerfMonitor::SectionTimer perf_timer##__LINE__(PerfMonitor::Instance(), name)

#endif // BITCOIN_UTIL_PERFMON_H
