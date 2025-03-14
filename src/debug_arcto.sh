#!/bin/bash

# debug_arcto.sh
# Script to debug QPainterPath::arcTo NaN warnings in Bitcoin-Qt
# -------------------------------------------------------------

# Set the path to Bitcoin source directory (adjust if necessary)
BITCOIN_SRC_DIR="$(pwd)"

# GDB script path
GDB_SCRIPT="debug_arcto.gdb"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[0;33m'
NC='\033[0m' # No Color

# Check if GDB is installed
if ! command -v gdb &> /dev/null; then
    echo -e "${RED}Error: GDB is not installed. Please install GDB first.${NC}"
    exit 1
fi

# Check if GDB script exists
if [ ! -f "$GDB_SCRIPT" ]; then
    echo -e "${RED}Error: GDB script '$GDB_SCRIPT' not found.${NC}"
    echo -e "Please make sure it exists in the current directory.${NC}"
    exit 1
fi

# Function to build Bitcoin-Qt with debug symbols
build_bitcoin_qt() {
    echo -e "${BLUE}Building Bitcoin-Qt with debug symbols...${NC}"

    # Navigate to the source directory
    cd "$BITCOIN_SRC_DIR" || { echo -e "${RED}Error: Could not navigate to Bitcoin source directory.${NC}"; exit 1; }

    # Configure with debug symbols if needed
    if [ ! -f "Makefile" ] || ! grep -q "CXXFLAGS = -g" Makefile; then
        echo -e "${YELLOW}Configuring Bitcoin with debug symbols...${NC}"
        if ! ./configure --enable-debug; then
            echo -e "${RED}Configure failed!${NC}"
            echo -e "${YELLOW}Possible solutions:${NC}"
            echo -e "1. Make sure all dependencies are installed"
            echo -e "2. Check configure.log for specific errors"
            echo -e "3. Run './autogen.sh' before configuring"
            echo -e "Press enter to return to the main menu..."
            read
            return 1
        fi
    fi

    # Build Bitcoin-Qt
    echo -e "${YELLOW}Compiling Bitcoin-Qt...${NC}"
    if ! make -j$(nproc) bitcoin-qt; then
        echo -e "${RED}Build failed!${NC}"
        echo -e "${YELLOW}Possible solutions:${NC}"
        echo -e "1. Check for compilation errors above"
        echo -e "2. Try running 'make clean' first"
        echo -e "3. Ensure you have enough disk space and memory"
        echo -e "Press enter to return to the main menu..."
        read
        return 1
    fi

    echo -e "${GREEN}Bitcoin-Qt built successfully with debug symbols.${NC}"
    return 0
}

# Function to launch GDB with Bitcoin-Qt
launch_gdb_new() {
    echo -e "${BLUE}Launching GDB with Bitcoin-Qt...${NC}"

    # Determine the Bitcoin-Qt binary path
    BITCOIN_QT_BIN="$BITCOIN_SRC_DIR/src/qt/bitcoin-qt"

    if [ ! -f "$BITCOIN_QT_BIN" ]; then
        echo -e "${RED}Error: Bitcoin-Qt binary not found at $BITCOIN_QT_BIN${NC}"
        echo -e "${YELLOW}Try building Bitcoin-Qt first using option 1 from the menu.${NC}"
        echo -e "Press enter to return to the main menu..."
        read
        show_menu
        return
    fi

    echo -e "${GREEN}Starting GDB with Bitcoin-Qt...${NC}"
    echo -e "${BLUE}=== GDB USAGE GUIDE ===${NC}"
    echo -e "${YELLOW}1. When GDB starts, type 'run' to launch Bitcoin-Qt${NC}"
    echo -e "${YELLOW}2. Interact with the application to trigger the arcTo warnings${NC}"
    echo -e "${YELLOW}3. When a breakpoint is hit, GDB will pause execution${NC}"
    echo -e "${YELLOW}4. Use these GDB commands to debug:${NC}"
    echo -e "   ${GREEN}bt${NC} - Show the call stack (backtrace)"
    echo -e "   ${GREEN}info locals${NC} - Show local variables"
    echo -e "   ${GREEN}print variable_name${NC} - Print a specific variable"
    echo -e "   ${GREEN}continue${NC} - Resume program execution"
    echo -e "   ${GREEN}quit${NC} - Exit GDB"
    echo -e "${YELLOW}5. Press Ctrl+C in GDB to interrupt if program is running${NC}"
    echo

    gdb -x "$GDB_SCRIPT" --args "$BITCOIN_QT_BIN"
}

# Function to attach GDB to a running Bitcoin-Qt process
attach_gdb() {
    echo -e "${BLUE}Attaching GDB to a running Bitcoin-Qt process...${NC}"

    # Check if we have sudo access for attaching to processes
    NEED_SUDO=0
    if [ "$(id -u)" -ne 0 ]; then
        # Check if ptrace_scope is restricting process attachment
        if [ -f /proc/sys/kernel/yama/ptrace_scope ] && [ "$(cat /proc/sys/kernel/yama/ptrace_scope)" -ne 0 ]; then
            echo -e "${YELLOW}System requires elevated privileges to attach to processes.${NC}"
            echo -e "${YELLOW}Will use sudo for the GDB attach operation.${NC}"
            NEED_SUDO=1
        fi
    fi

    # Find running Bitcoin-Qt processes
    BITCOIN_PIDS=$(pgrep -f "bitcoin-qt")

    if [ -z "$BITCOIN_PIDS" ]; then
        echo -e "${RED}Error: No running Bitcoin-Qt processes found.${NC}"
        echo -e "${YELLOW}Would you like to launch a new Bitcoin-Qt instance instead? (y/n)${NC}"
        read -r answer
        if [[ "$answer" =~ ^[Yy]$ ]]; then
            launch_gdb_new
        else
            echo -e "Press enter to return to the main menu..."
            read
            show_menu
        fi
        return
    fi

    # If multiple processes, let user select
    if [ $(echo "$BITCOIN_PIDS" | wc -l) -gt 1 ]; then
        echo -e "${YELLOW}Multiple Bitcoin-Qt processes found:${NC}"
        PS_OUTPUT=$(ps -p $BITCOIN_PIDS -o pid,cmd)
        echo "$PS_OUTPUT"
        echo -e "${YELLOW}Enter the PID of the process to attach to:${NC}"
        read -r selected_pid

        if ! echo "$BITCOIN_PIDS" | grep -q "$selected_pid"; then
            echo -e "${RED}Invalid PID selected.${NC}"
            echo -e "Press enter to return to the main menu..."
            read
            show_menu
            return
        fi

        BITCOIN_PID=$selected_pid
    else
        BITCOIN_PID=$BITCOIN_PIDS
    fi

    echo -e "${GREEN}Attaching GDB to Bitcoin-Qt process $BITCOIN_PID...${NC}"
    echo -e "${BLUE}=== GDB USAGE GUIDE ===${NC}"
    echo -e "${YELLOW}1. When GDB is attached, the program will be paused${NC}"
    echo -e "${YELLOW}2. Type 'continue' to resume program execution${NC}"
    echo -e "${YELLOW}3. Interact with the application to trigger the arcTo warnings${NC}"
    echo -e "${YELLOW}4. When a breakpoint is hit, GDB will pause execution again${NC}"
    echo -e "${YELLOW}5. Use these GDB commands to debug:${NC}"
    echo -e "   ${GREEN}bt${NC} - Show the call stack (backtrace)"
    echo -e "   ${GREEN}info locals${NC} - Show local variables"
    echo -e "   ${GREEN}print variable_name${NC} - Print a specific variable"
    echo -e "   ${GREEN}frame N${NC} - Move to frame N in the backtrace"
    echo -e "   ${GREEN}continue${NC} - Resume program execution"
    echo -e "   ${GREEN}quit${NC} - Exit GDB"
    echo -e "${YELLOW}6. Press Ctrl+C in GDB to interrupt if program is running${NC}"
    echo

    # Use sudo if needed
    if [ "$NEED_SUDO" -eq 1 ]; then
        echo -e "${YELLOW}Using sudo to attach to process...${NC}"
        sudo gdb -x "$GDB_SCRIPT" -p "$BITCOIN_PID"
    else
        gdb -x "$GDB_SCRIPT" -p "$BITCOIN_PID"
    fi
}

# Main menu
show_menu() {
    echo -e "${BLUE}===== Bitcoin-Qt arcTo Debugger =====${NC}"
    echo -e "${YELLOW}This script helps debug QPainterPath::arcTo NaN warnings in Bitcoin-Qt.${NC}"
    echo
    echo -e "${GREEN}1. Build Bitcoin-Qt with debug symbols${NC}"
    echo -e "${GREEN}2. Launch Bitcoin-Qt with GDB (new instance)${NC}"
    echo -e "${GREEN}3. Attach GDB to a running Bitcoin-Qt process${NC}"
    echo -e "${GREEN}4. Build and launch with GDB${NC}"
    echo -e "${GREEN}5. Exit${NC}"
    echo
    echo -e "${YELLOW}Enter your choice [1-5]:${NC}"
    read -r choice

    case $choice in
        1) build_bitcoin_qt && echo -e "${YELLOW}Press enter to continue...${NC}" && read; show_menu ;;
        2) launch_gdb_new; echo -e "${YELLOW}Returned from GDB. Press enter to continue...${NC}"; read; show_menu ;;
        3) attach_gdb; echo -e "${YELLOW}Returned from GDB. Press enter to continue...${NC}"; read; show_menu ;;
        4) build_bitcoin_qt && launch_gdb_new; echo -e "${YELLOW}Returned from GDB. Press enter to continue...${NC}"; read; show_menu ;;
# Make the script executable
chmod +x "$0"

# Display usage information
echo -e "${BLUE}===== Bitcoin-Qt arcTo Debugger =====${NC}"
echo -e "${YELLOW}This script will help you debug QPainterPath::arcTo NaN warnings in Bitcoin-Qt.${NC}"
echo -e "${YELLOW}It works in conjunction with the GDB script '${GDB_SCRIPT}'.${NC}"
echo
echo -e "${BLUE}What this debugger will do:${NC}"
echo -e "1. Set breakpoints on Qt warning functions and QPainterPath functions"
echo -e "2. Set conditional breakpoints to catch when NaN values are passed"
echo -e "3. Print the backtrace, variables, and arguments when breakpoints are hit"
echo -e "4. Help identify the exact source of arcTo NaN warnings"
echo
# Display the menu
show_menu

