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
struct QPointF;

// Function pointers to original functions
static void (*original_arcTo_rect)(QPainterPath*, const QRectF&, double, double) = nullptr;
static void (*original_drawEllipse)(void*, const QPointF&, double, double) = nullptr;
static void (*original_drawEllipse_rect)(void*, const QRectF&) = nullptr;

// Flag to prevent infinite recursion
static bool in_interceptor = false;

// Initialize function pointers
static void init_original_functions() {
    if (original_arcTo_rect) {
        return; // Already initialized
    }
    
    // Get the original function addresses
    original_arcTo_rect = (void(*)(QPainterPath*, const QRectF&, double, double))dlsym(RTLD_NEXT, "_ZN12QPainterPath5arcToERK6QRectFdd");
    original_drawEllipse = (void(*)(void*, const QPointF&, double, double))dlsym(RTLD_NEXT, "_ZN8QPainter10drawEllipseERK7QPointFdd");
    original_drawEllipse_rect = (void(*)(void*, const QRectF&))dlsym(RTLD_NEXT, "_ZN8QPainter10drawEllipseERK6QRectF");
    
    if (!original_arcTo_rect) {
        std::cerr << "WARNING: Could not find original arcTo(rect) function" << std::endl;
    }
    if (!original_drawEllipse) {
        std::cerr << "WARNING: Could not find original drawEllipse(point) function" << std::endl;
    }
    if (!original_drawEllipse_rect) {
        std::cerr << "WARNING: Could not find original drawEllipse(rect) function" << std::endl;
    }
    
    std::cerr << "Qt painter interceptor initialized" << std::endl;
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
extern "C" void _ZN12QPainterPath5arcToERK6QRectFdd(QPainterPath* path, const QRectF& rect, double startAngle, double arcLength) {
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
        print_backtrace("🚨 INTERCEPT: QPainterPath::arcTo(QRectF) called with NaN parameters!");
    }
    
    // Call the original function
    in_interceptor = true;
    if (original_arcTo_rect) {
        original_arcTo_rect(path, rect, startAngle, arcLength);
    }
    in_interceptor = false;
}

// Interceptor for drawEllipse with QPointF
extern "C" void _ZN8QPainter10drawEllipseERK7QPointFdd(void* painter, const QPointF& center, double rx, double ry) {
    if (in_interceptor) {
        if (original_drawEllipse) {
            original_drawEllipse(painter, center, rx, ry);
        }
        return;
    }
    
    init_original_functions();
    
    // Check for NaN parameters
    if (std::isnan(rx) || std::isnan(ry)) {
        print_backtrace("🚨 INTERCEPT: QPainter::drawEllipse(QPointF) called with NaN radius!");
    }
    
    // Call the original function
    in_interceptor = true;
    if (original_drawEllipse) {
        original_drawEllipse(painter, center, rx, ry);
    }
    in_interceptor = false;
}

// Interceptor for drawEllipse with QRectF
extern "C" void _ZN8QPainter10drawEllipseERK6QRectF(void* painter, const QRectF& rect) {
    if (in_interceptor) {
        if (original_drawEllipse_rect) {
            original_drawEllipse_rect(painter, rect);
        }
        return;
    }
    
    init_original_functions();
    
    // We can't easily check rect members without Qt headers, but we can still intercept
    // to see if this is where the NaN is coming from
    
    // Call the original function
    in_interceptor = true;
    if (original_drawEllipse_rect) {
        original_drawEllipse_rect(painter, rect);
    }
    in_interceptor = false;
}

// Constructor to initialize when library is loaded
__attribute__((constructor))
static void init() {
    std::cerr << "Qt painter interceptor library loaded" << std::endl;
} 