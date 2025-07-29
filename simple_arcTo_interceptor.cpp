// Copyright (c) 2024 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#define _GNU_SOURCE
#include <dlfcn.h>
#include <iostream>
#include <execinfo.h>
#include <cmath>
#include <cstring>

// Forward declarations
struct QPainterPath;
struct QRectF;

// Function pointers to original functions
static void (*original_arcTo_6params)(QPainterPath*, double, double, double, double, double, double) = nullptr;
static void (*original_arcTo_rect)(QPainterPath*, const QRectF&, double, double) = nullptr;

// Flag to prevent infinite recursion
static bool in_interceptor = false;

// Initialize function pointers
static void init_original_functions() {
    if (original_arcTo_6params && original_arcTo_rect) {
        return; // Already initialized
    }
    
    // Get the original function addresses
    original_arcTo_6params = (void(*)(QPainterPath*, double, double, double, double, double, double))dlsym(RTLD_NEXT, "_ZN11QPainterPath5arcToEddddd");
    original_arcTo_rect = (void(*)(QPainterPath*, const QRectF&, double, double))dlsym(RTLD_NEXT, "_ZN11QPainterPath5arcToERK6QRectFdd");
    
    if (!original_arcTo_6params) {
        std::cerr << "WARNING: Could not find original arcTo(6params) function" << std::endl;
    }
    if (!original_arcTo_rect) {
        std::cerr << "WARNING: Could not find original arcTo(rect) function" << std::endl;
    }
    
    std::cerr << "arcTo interceptor initialized" << std::endl;
}

// Interceptor for 6-parameter arcTo
extern "C" void _ZN11QPainterPath5arcToEddddd(QPainterPath* path, double x, double y, double w, double h, double startAngle, double arcLength) {
    if (in_interceptor) {
        // Fallback to prevent infinite recursion
        if (original_arcTo_6params) {
            original_arcTo_6params(path, x, y, w, h, startAngle, arcLength);
        }
        return;
    }
    
    init_original_functions();
    
    // Check for NaN parameters
    if (std::isnan(x) || std::isnan(y) || std::isnan(w) || std::isnan(h) || 
        std::isnan(startAngle) || std::isnan(arcLength)) {
        
        std::cerr << "🚨 LD_PRELOAD INTERCEPT: QPainterPath::arcTo called with NaN parameters!" << std::endl;
        std::cerr << "  x: " << x << ", y: " << y << ", w: " << w << ", h: " << h << std::endl;
        std::cerr << "  startAngle: " << startAngle << ", arcLength: " << arcLength << std::endl;
        
        // Print backtrace
        void* callstack[128];
        int frames = backtrace(callstack, 128);
        char** strs = backtrace_symbols(callstack, frames);
        std::cerr << "Backtrace:" << std::endl;
        for (int i = 0; i < frames && i < 20; i++) {
            std::cerr << "  " << strs[i] << std::endl;
        }
        free(strs);
        
        std::cerr << "Aborting due to NaN parameters in LD_PRELOAD arcTo..." << std::endl;
        abort();
    }
    
    // Call the original function
    in_interceptor = true;
    if (original_arcTo_6params) {
        original_arcTo_6params(path, x, y, w, h, startAngle, arcLength);
    }
    in_interceptor = false;
}

// Interceptor for QRectF arcTo
extern "C" void _ZN11QPainterPath5arcToERK6QRectFdd(QPainterPath* path, const QRectF& rect, double startAngle, double arcLength) {
    if (in_interceptor) {
        if (original_arcTo_rect) {
            original_arcTo_rect(path, rect, startAngle, arcLength);
        }
        return;
    }
    
    init_original_functions();
    
    // Check for NaN parameters - we can't easily check rect members without Qt headers
    // so we'll just check the angles
    if (std::isnan(startAngle) || std::isnan(arcLength)) {
        
        std::cerr << "🚨 LD_PRELOAD INTERCEPT: QPainterPath::arcTo(QRectF) called with NaN parameters!" << std::endl;
        std::cerr << "  startAngle: " << startAngle << ", arcLength: " << arcLength << std::endl;
        
        // Print backtrace
        void* callstack[128];
        int frames = backtrace(callstack, 128);
        char** strs = backtrace_symbols(callstack, frames);
        std::cerr << "Backtrace:" << std::endl;
        for (int i = 0; i < frames && i < 20; i++) {
            std::cerr << "  " << strs[i] << std::endl;
        }
        free(strs);
        
        std::cerr << "Aborting due to NaN parameters in LD_PRELOAD arcTo..." << std::endl;
        abort();
    }
    
    // Call the original function
    in_interceptor = true;
    if (original_arcTo_rect) {
        original_arcTo_rect(path, rect, startAngle, arcLength);
    }
    in_interceptor = false;
} 