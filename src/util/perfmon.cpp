#include <util/perfmon.h>
#include <logging.h>

// Implementation is mostly in the header, but we'll add some logging functionality here
void LogPerfStats() {
    LogPrintf("Performance Monitor Stats:\n%s", PerfMonitor::Instance().GetStats());
}
