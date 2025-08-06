#!/bin/bash

# GUI Thread Debug Script for Bitcoin Core
# This script automatically finds bitcoin-qt and monitors the GUI thread

set -e

echo "🔍 Bitcoin Core GUI Thread Debugger"
echo "=================================="

# Find bitcoin-qt process
echo "📋 Looking for bitcoin-qt process..."
BITCOIN_PID=$(pgrep bitcoin-qt)

if [ -z "$BITCOIN_PID" ]; then
    echo "❌ bitcoin-qt process not found. Is it running?"
    exit 1
fi

echo "✅ Found bitcoin-qt process: PID $BITCOIN_PID"

# Get all threads
echo "🧵 Getting thread information..."
ps -T -p $BITCOIN_PID

# Find the main GUI thread (usually the one with the highest CPU usage or main thread)
echo ""
echo "🎯 Identifying GUI thread..."

# Get thread info and find the main thread (usually thread 1 or the one with most CPU)
THREAD_INFO=$(ps -T -p $BITCOIN_PID --no-headers | sort -k3 -nr | head -1)
GUI_TID=$(echo $THREAD_INFO | awk '{print $2}')

echo "✅ Identified GUI thread: TID $GUI_TID"
echo "   Thread info: $THREAD_INFO"

# Create a temporary script for strace
TRACE_SCRIPT="/tmp/bitcoin_gui_trace_$$.sh"

cat > $TRACE_SCRIPT << 'EOF'
#!/bin/bash
# Temporary strace script for GUI thread monitoring

PID=$1
TID=$2

echo "🔍 Starting strace monitoring on GUI thread $TID..."
echo "📊 This will show system calls during GUI freezes"
echo "⏹️  Press Ctrl+C to stop monitoring"
echo ""

# Try strace without sudo first
echo "🔄 Attempting strace without sudo..."
if strace -p $TID -f \
    -e trace=network,desc,file,process,signal,futex \
    -e signal=!all \
    -e status=successful \
    -o /tmp/bitcoin_gui_strace_$$.log \
    -s 200 \
    -ttt 2>/dev/null; then
    echo "✅ Strace started successfully without sudo"
else
    echo "⚠️  Strace failed without sudo, trying with sudo..."
    sudo strace -p $TID -f \
        -e trace=network,desc,file,process,signal,futex \
        -e signal=!all \
        -e status=successful \
        -o /tmp/bitcoin_gui_strace_$$.log \
        -s 200 \
        -ttt
fi

echo ""
echo "📄 Strace log saved to: /tmp/bitcoin_gui_strace_$$.log"
echo "🔍 To view the log: tail -f /tmp/bitcoin_gui_strace_$$.log"
EOF

chmod +x $TRACE_SCRIPT

echo ""
echo "🚀 Starting GUI thread monitoring..."
echo "   This will show system calls that might be causing freezes"
echo "   Look for long-running calls during the 5-10 second delays"
echo ""
echo "📝 Monitoring started at: $(date)"
echo "⏹️  Press Ctrl+C to stop monitoring"
echo ""

# Run the strace script
$TRACE_SCRIPT $BITCOIN_PID $GUI_TID

# Cleanup
rm -f $TRACE_SCRIPT

echo ""
echo "✅ Monitoring complete!"
echo "📄 Log file: /tmp/bitcoin_gui_strace_$$.log"
echo "🔍 To analyze the log:"
echo "   grep 'blocked\|futex\|poll\|read\|write' /tmp/bitcoin_gui_strace_$$.log" 