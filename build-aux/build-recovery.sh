#!/bin/bash

# Build recovery script for Bitcoin Core
# Detects common build errors and suggests automatic fixes

set -e

# Function to detect and suggest fixes for common build errors
detect_and_suggest_fixes() {
    local error_output="$1"
    
    # Check for undefined reference errors (most common)
    if echo "$error_output" | grep -q "undefined reference"; then
        echo ""
        echo "🔧 DETECTED: Undefined reference errors"
        echo "   This usually indicates corrupted libraries, especially libbitcoin_util.a"
        echo ""
        echo "💡 SUGGESTED FIX:"
        echo "   make fix-undefined-refs"
        echo ""
        echo "   Or for comprehensive fix:"
        echo "   make fix-build"
        echo ""
        return 1
    fi
    
    # Check for malformed archive errors
    if echo "$error_output" | grep -q "malformed archive"; then
        echo ""
        echo "🔧 DETECTED: Malformed archive errors"
        echo "   This indicates corrupted .a library files"
        echo ""
        echo "💡 SUGGESTED FIX:"
        echo "   make fix-build"
        echo ""
        return 1
    fi
    
    # Check for missing object files
    if echo "$error_output" | grep -q "No rule to make target.*\.o"; then
        echo ""
        echo "🔧 DETECTED: Missing object files"
        echo "   This indicates incomplete or corrupted build state"
        echo ""
        echo "💡 SUGGESTED FIX:"
        echo "   make fix-build"
        echo ""
        return 1
    fi
    
    return 0
}

# Main build function with error detection
build_with_recovery() {
    echo "🚀 Starting build with automatic error detection..."
    
    # Capture build output
    local build_output
    if build_output=$(make "$@" 2>&1); then
        echo "✅ Build completed successfully!"
        return 0
    else
        echo "❌ Build failed. Analyzing error..."
        detect_and_suggest_fixes "$build_output"
        return 1
    fi
}

# Show available recovery targets
show_help() {
    echo "🔧 Bitcoin Core Build Recovery System"
    echo ""
    echo "Available recovery targets:"
    echo "  make fix-undefined-refs  - Fix undefined reference errors"
    echo "  make fix-build          - Comprehensive build fix"
    echo "  make smart-build        - Build with automatic error recovery"
    echo "  make robust-build       - Build with corrupted library detection"
    echo ""
    echo "Usage:"
    echo "  $0 [make_targets...]    - Build with automatic error detection"
    echo "  $0 --help              - Show this help"
    echo ""
}

# Main script logic
if [ "$1" = "--help" ] || [ "$1" = "-h" ]; then
    show_help
    exit 0
fi

# If no arguments, show help
if [ $# -eq 0 ]; then
    show_help
    exit 1
fi

# Run build with recovery
build_with_recovery "$@"
