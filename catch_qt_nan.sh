#!/bin/bash

# Script to catch Qt's NaN error message and get a backtrace
# Usage: ./catch_qt_nan.sh

echo "🚨 Starting Bitcoin Core with Qt NaN monitoring..."
echo "This script will catch the exact moment Qt detects NaN in arcTo parameters"
echo ""

# Get the debug.log path
DEBUG_LOG="$HOME/.bitcoin/debug.log"
echo "Monitoring debug.log: $DEBUG_LOG"

# Function to handle the error when detected
handle_nan_error() {
    echo ""
    echo "🚨 Qt NaN ERROR DETECTED!"
    echo "Error message: $1"
    echo ""
    echo "Getting backtrace with GDB..."
    
    # Find the bitcoin-qt process
    PID=$(pgrep bitcoin-qt)
    if [ -n "$PID" ]; then
        echo "Found bitcoin-qt process: $PID"
        echo "Attaching GDB to get backtrace..."
        
        # Create a temporary GDB script
        GDB_SCRIPT=$(mktemp)
        cat > "$GDB_SCRIPT" << 'EOF'
set pagination off
echo 🚨 Qt NaN Error Backtrace:\n
bt
echo \n
echo Current registers:\n
info registers
echo \n
echo Disconnecting from process...\n
detach
quit
EOF
        
        # Run GDB to get backtrace
        gdb -x "$GDB_SCRIPT" -p "$PID" 2>/dev/null
        
        # Clean up
        rm "$GDB_SCRIPT"
    else
        echo "Could not find bitcoin-qt process"
    fi
    
    echo ""
    echo "Continuing to monitor for more errors..."
}

# Start Bitcoin Core in background
echo "Starting Bitcoin Core..."
./src/qt/bitcoin-qt -nowallet &
BITCOIN_PID=$!

echo "Bitcoin Core started with PID: $BITCOIN_PID"
echo "Monitoring debug.log for Qt NaN errors..."

# Monitor debug.log for the error message
tail -f "$DEBUG_LOG" | while IFS= read -r line; do
    echo "$line"
    if [[ "$line" == *"QPainterPath::arcTo: Adding arc where a parameter is NaN"* ]]; then
        handle_nan_error "$line"
    fi
done 