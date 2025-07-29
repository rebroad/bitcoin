#!/bin/bash

echo "🔧 Building arcTo interceptor..."
echo "================================"

# Build the interceptor
make -f Makefile.interceptor

if [ $? -ne 0 ]; then
    echo "❌ Failed to build interceptor!"
    echo "Trying alternative Qt paths..."
    
    # Try different Qt include paths
    export CXXFLAGS="-fPIC -shared -std=c++11 -Wall -Wextra"
    export INCLUDES="-I/usr/include/qt -I/usr/include/qt/QtCore -I/usr/include/qt/QtGui -I/usr/include/qt/QtWidgets"
    
    g++ $CXXFLAGS $INCLUDES -o libarcTo_interceptor.so arcTo_interceptor.cpp -ldl
    
    if [ $? -ne 0 ]; then
        echo "❌ Still failed. Please check your Qt installation."
        exit 1
    fi
fi

echo "✅ Interceptor built successfully!"
echo ""
echo "🎯 To test with Bitcoin Core:"
echo ""
echo "1. Build Bitcoin Core normally (if not already built):"
echo "   ./autogen.sh"
echo "   ./configure --enable-debug --with-gui=qt5"
echo "   make"
echo ""
echo "2. Run Bitcoin Core with the interceptor:"
echo "   LD_PRELOAD=./libarcTo_interceptor.so ./src/qt/bitcoin-qt"
echo ""
echo "3. Or run with gdb for debugging:"
echo "   LD_PRELOAD=./libarcTo_interceptor.so gdb --args ./src/qt/bitcoin-qt"
echo "   (gdb) run"
echo "   (gdb) bt  # when it crashes"
echo ""
echo "4. Watch for these messages in the console:"
echo "   🚨 LD_PRELOAD INTERCEPT: QPainterPath::arcTo called with NaN parameters!"
echo ""
echo "🔍 The interceptor will:"
echo "   - Catch ALL arcTo calls (even from Qt's internal drawEllipse)"
echo "   - Print the exact NaN parameters"
echo "   - Show a backtrace of where the call originated"
echo "   - Abort the program so you can debug with gdb"
echo ""
echo "💡 If you want to see the function names in the backtrace, build Bitcoin Core with debug symbols:"
echo "   ./configure --enable-debug --with-gui=qt5" 