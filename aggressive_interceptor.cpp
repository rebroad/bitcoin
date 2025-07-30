// Copyright (c) 2024 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#define _GNU_SOURCE
#include <dlfcn.h>
#include <iostream>
#include <execinfo.h>
#include <cmath>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

// Function pointers to original functions
static void (*original_arcTo_rect)(void*, void*, double, double) = nullptr;

// Flag to prevent infinite recursion
static bool in_interceptor = false;

// Initialize function pointers
static void init_original_functions() {
    if (original_arcTo_rect) {
        return; // Already initialized
    }
    
    // Get the original function address
    original_arcTo_rect = (void(*)(void*, void*, double, double))dlsym(RTLD_NEXT, "_ZN12QPainterPath5arcToERK6QRectFdd");
    
    if (!original_arcTo_rect) {
        std::cerr << "WARNING: Could not find original arcTo(rect) function" << std::endl;
    }
    
    std::cerr << "Aggressive interceptor initialized" << std::endl;
}

// Helper function to print backtrace
static void print_backtrace(const char* message) {
    std::cerr << message << std::endl;
    
    void* callstack[128];
    int frames = backtrace(callstack, 128);
    char** strs = backtrace_symbols(callstack, frames);
    std::cerr << "Backtrace:" << std::endl;
    for (int i = 0; i < frames && i < 20; i++) {
        std::cerr << "  " << strs[i] << std::endl;
    }
    free(strs);
    
    std::cerr << "Aborting due to NaN parameters..." << std::endl;
    abort();
}

// Interceptor for QRectF arcTo
extern "C" void _ZN12QPainterPath5arcToERK6QRectFdd(void* path, void* rect, double startAngle, double arcLength) {
    if (in_interceptor) {
        if (original_arcTo_rect) {
            original_arcTo_rect(path, rect, startAngle, arcLength);
        }
        return;
    }
    
    init_original_functions();
    
    // Check for NaN parameters
    if (std::isnan(startAngle) || std::isnan(arcLength)) {
        print_backtrace("🚨 AGGRESSIVE INTERCEPT: QPainterPath::arcTo(QRectF) called with NaN parameters!");
    }
    
    // Call the original function
    in_interceptor = true;
    if (original_arcTo_rect) {
        original_arcTo_rect(path, rect, startAngle, arcLength);
    }
    in_interceptor = false;
}

// Now let's try to hook into the 6-parameter version by patching the binary
// This is more aggressive but should catch ALL arcTo calls

// Function to patch Qt's internal arcTo calls
static void patch_qt_arcTo() {
    // Get the Qt library handle
    void* handle = dlopen("libQt5Gui.so.5", RTLD_LAZY);
    if (!handle) {
        std::cerr << "Could not open Qt library for patching" << std::endl;
        return;
    }
    
    // Try to find the 6-parameter arcTo function in memory
    // This is tricky because it's not exported, but we can try to find it
    // by looking for the function signature in memory
    
    std::cerr << "Attempting to patch Qt's internal arcTo functions..." << std::endl;
    
    // For now, let's just log that we're trying
    dlclose(handle);
}

// Constructor to initialize when library is loaded
__attribute__((constructor))
static void init() {
    std::cerr << "Aggressive Qt interceptor library loaded" << std::endl;
    patch_qt_arcTo();
} 