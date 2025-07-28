#!/bin/bash

# Bitcoin Core Dependency Installer for Ubuntu/Debian
# This script intelligently installs missing dependencies based on configure output
# and provides a comprehensive fallback installation

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Function to print colored output
print_status() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

print_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Function to check if a package is installed
is_package_installed() {
    dpkg -l "$1" >/dev/null 2>&1
}

# Function to install packages if not already installed
install_packages() {
    local packages=("$@")
    local to_install=()

    for package in "${packages[@]}"; do
        if ! is_package_installed "$package"; then
            to_install+=("$package")
        else
            print_status "$package is already installed"
        fi
    done

    if [ ${#to_install[@]} -gt 0 ]; then
        print_status "Installing: ${to_install[*]}"
        sudo apt-get update
        sudo apt-get install -y "${to_install[@]}"
        print_success "Installed: ${to_install[*]}"
    fi
}

# Function to parse configure output and extract missing dependencies
parse_configure_output() {
    local configure_output="$1"
    local missing_packages=()

    # Parse common error patterns from configure output
    if echo "$configure_output" | grep -q "Boost is not available"; then
        missing_packages+=("libboost-dev" "libboost-system-dev" "libboost-filesystem-dev" "libboost-thread-dev" "libboost-chrono-dev" "libboost-program-options-dev" "libboost-test-dev")
    fi

    if echo "$configure_output" | grep -q "Qt5Core.*not found"; then
        missing_packages+=("qttools5-dev" "qttools5-dev-tools" "qtbase5-dev" "qtbase5-dev-tools" "libqt5gui5" "libqt5core5a" "libqt5dbus5")
    fi

    if echo "$configure_output" | grep -q "libdb_cxx headers missing"; then
        missing_packages+=("libdb++-dev")
    fi

    if echo "$configure_output" | grep -q "sqlite3.*not found"; then
        missing_packages+=("libsqlite3-dev")
    fi

    if echo "$configure_output" | grep -q "miniupnpc.*not found"; then
        missing_packages+=("libminiupnpc-dev")
    fi

    if echo "$configure_output" | grep -q "natpmp.*not found"; then
        missing_packages+=("libnatpmp-dev")
    fi

    if echo "$configure_output" | grep -q "qrencode.*not found"; then
        missing_packages+=("libqrencode-dev")
    fi

    if echo "$configure_output" | grep -q "zmq.*not found"; then
        missing_packages+=("libzmq3-dev")
    fi

    echo "${missing_packages[@]}"
}

    # Function to run configure and capture output
    run_configure() {
        print_status "Running ./configure to check for missing dependencies..."

        # Capture both stdout and stderr
        local output
        if output=$(./configure 2>&1); then
            print_success "Configure completed successfully!"

            # Check if GUI was actually enabled
            if grep -q "bitcoin_enable_qt.*=.*yes" config.log 2>/dev/null || grep -q "with gui.*=.*yes" config.log 2>/dev/null || grep -q "whether to build Bitcoin Core GUI.*yes" config.log 2>/dev/null; then
                print_success "GUI support enabled! You can build bitcoin-qt"
                return 0
            else
                print_warning "GUI support not enabled - Qt development packages may be missing"
                return 1
            fi
        else
            print_warning "Configure failed, analyzing missing dependencies..."
            echo "$output"
            return 1
        fi
    }

# Main installation function
install_dependencies() {
    print_status "Starting Bitcoin Core dependency installation..."

    # Essential build tools (always install these)
    print_status "Installing essential build tools..."
    install_packages \
        "build-essential" \
        "libtool" \
        "autotools-dev" \
        "automake" \
        "pkg-config" \
        "bsdmainutils" \
        "python3" \
        "git"

    # Core dependencies (always install these)
    print_status "Installing core dependencies..."
    install_packages \
        "libevent-dev" \
        "libboost-dev" \
        "libboost-system-dev" \
        "libboost-filesystem-dev" \
        "libboost-thread-dev" \
        "libboost-chrono-dev" \
        "libboost-program-options-dev" \
        "libboost-test-dev"

    # Try to run configure first
    if run_configure; then
        print_success "All dependencies are already installed! 🎉"
        return 0
    fi

    # If configure failed or GUI is disabled, check for Qt development packages
    print_status "Checking for Qt development packages..."
    local qt_dev_packages=("qttools5-dev" "qttools5-dev-tools" "qtbase5-dev" "qtbase5-dev-tools" "libqt5gui5" "libqt5core5a" "libqt5dbus5")
    local missing_qt_packages=()

    for package in "${qt_dev_packages[@]}"; do
        if ! is_package_installed "$package"; then
            missing_qt_packages+=("$package")
        fi
    done

    if [ ${#missing_qt_packages[@]} -gt 0 ]; then
        print_status "Missing Qt development packages: ${missing_qt_packages[*]}"
        install_packages "${missing_qt_packages[@]}"
    fi

    # If configure failed, try to parse the output and install missing packages
    print_status "Attempting to install missing dependencies based on configure output..."

    # Run configure again to capture the output
    local configure_output
    configure_output=$(./configure 2>&1 || true)

    # Parse the output for missing dependencies
    local missing_packages
    missing_packages=($(parse_configure_output "$configure_output"))

    if [ ${#missing_packages[@]} -gt 0 ]; then
        print_status "Found missing packages: ${missing_packages[*]}"
        install_packages "${missing_packages[@]}"
    fi

    # Install comprehensive fallback packages
    print_status "Installing comprehensive fallback packages..."
    install_packages \
        "libsqlite3-dev" \
        "libdb++-dev" \
        "libminiupnpc-dev" \
        "libnatpmp-dev" \
        "libzmq3-dev" \
        "libqrencode-dev" \
        "qttools5-dev" \
        "qttools5-dev-tools" \
        "qtbase5-dev" \
        "qtbase5-dev-tools" \
        "libqt5gui5" \
        "libqt5core5a" \
        "libqt5dbus5" \
        "qtwayland5" \
        "systemtap-sdt-dev"

    # Try configure again
    print_status "Running ./configure again after installing dependencies..."
    if run_configure; then
        print_success "Dependencies installed successfully! 🚀"

        # Check if GUI was enabled
        if grep -q "with gui.*=.*yes" config.log 2>/dev/null || grep -q "bitcoin_enable_qt.*=.*yes" config.log 2>/dev/null; then
            print_success "GUI support enabled! You can build bitcoin-qt"
        else
            print_warning "GUI support not enabled. To enable it, run: ./configure --with-gui=qt5"
        fi

        print_status "You can now run 'make' to build Bitcoin Core"
        return 0
    else
        print_error "Configure still failing after installing dependencies"
        print_status "This might be due to version incompatibilities or missing system libraries"
        print_status "Consider using the depends system: make -C depends"
        return 1
    fi
}

# Function to show usage
show_usage() {
    echo "Bitcoin Core Dependency Installer"
    echo ""
    echo "Usage: $0 [OPTIONS]"
    echo ""
    echo "Options:"
    echo "  -h, --help     Show this help message"
    echo "  -f, --force    Force reinstall all dependencies"
    echo "  -q, --quiet    Suppress verbose output"
    echo ""
    echo "This script will:"
    echo "  1. Install essential build tools"
    echo "  2. Install core dependencies (Boost, libevent)"
    echo "  3. Run ./configure to detect missing dependencies"
    echo "  4. Install any missing packages found"
    echo "  5. Install comprehensive fallback packages"
    echo "  6. Verify the installation with ./configure"
}

# Parse command line arguments
FORCE_INSTALL=false
QUIET=false

while [[ $# -gt 0 ]]; do
    case $1 in
        -h|--help)
            show_usage
            exit 0
            ;;
        -f|--force)
            FORCE_INSTALL=true
            shift
            ;;
        -q|--quiet)
            QUIET=true
            shift
            ;;
        *)
            print_error "Unknown option: $1"
            show_usage
            exit 1
            ;;
    esac
done

# Check if we're in a Bitcoin Core source directory
if [ ! -f "./configure.ac" ] && [ ! -f "./configure" ]; then
    print_error "This script must be run from the Bitcoin Core source directory"
    print_error "Please cd to your Bitcoin Core source directory and try again"
    exit 1
fi

# Check if autogen.sh needs to be run
if [ ! -f "./configure" ]; then
    print_status "Running ./autogen.sh to generate configure script..."
    ./autogen.sh
fi

# Main execution
if install_dependencies; then
    print_success "🎉 Bitcoin Core dependencies installed successfully!"
    print_status "Next steps:"
    print_status "  1. Run 'make' to build Bitcoin Core"
    print_status "  2. Optionally run 'make check' to run tests"
    print_status "  3. Optionally run 'sudo make install' to install system-wide"
else
    print_error "❌ Failed to install all dependencies"
    print_status "You may need to:"
    print_status "  1. Check your system's package repositories"
    print_status "  2. Use the depends system: make -C depends"
    print_status "  3. Build dependencies from source"
    exit 1
fi
