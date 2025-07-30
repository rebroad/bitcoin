// Copyright (c) 2024 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <QApplication>
#include <QPainter>
#include <QPainterPath>
#include <QWidget>
#include <QDebug>
#include <iostream>
#include <execinfo.h>
#include <cmath>

// Global flag to track if we're in our interceptor
static bool in_interceptor = false;

// Custom QPainterPath that checks for NaN values
class SafeQPainterPath : public QPainterPath {
public:
    void arcTo(qreal x, qreal y, qreal w, qreal h, qreal startAngle, qreal arcLength) override {
        if (in_interceptor) {
            QPainterPath::arcTo(x, y, w, h, startAngle, arcLength);
            return;
        }
        
        // Check for NaN parameters
        if (std::isnan(x) || std::isnan(y) || std::isnan(w) || std::isnan(h) || 
            std::isnan(startAngle) || std::isnan(arcLength)) {
            
            std::cerr << "🚨 SAFE INTERCEPT: QPainterPath::arcTo called with NaN parameters!" << std::endl;
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
            
            std::cerr << "Aborting due to NaN parameters in arcTo..." << std::endl;
            abort();
        }
        
        in_interceptor = true;
        QPainterPath::arcTo(x, y, w, h, startAngle, arcLength);
        in_interceptor = false;
    }
    
    void arcTo(const QRectF& rect, qreal startAngle, qreal arcLength) override {
        if (in_interceptor) {
            QPainterPath::arcTo(rect, startAngle, arcLength);
            return;
        }
        
        // Check for NaN parameters
        if (std::isnan(rect.x()) || std::isnan(rect.y()) || std::isnan(rect.width()) || std::isnan(rect.height()) ||
            std::isnan(startAngle) || std::isnan(arcLength)) {
            
            std::cerr << "🚨 SAFE INTERCEPT: QPainterPath::arcTo(QRectF) called with NaN parameters!" << std::endl;
            std::cerr << "  rect: x=" << rect.x() << ", y=" << rect.y() 
                     << ", w=" << rect.width() << ", h=" << rect.height() << std::endl;
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
            
            std::cerr << "Aborting due to NaN parameters in arcTo..." << std::endl;
            abort();
        }
        
        in_interceptor = true;
        QPainterPath::arcTo(rect, startAngle, arcLength);
        in_interceptor = false;
    }
};

// Custom QPainter that uses SafeQPainterPath
class SafeQPainter : public QPainter {
public:
    void drawEllipse(const QPointF& center, qreal rx, qreal ry) {
        // Check for NaN parameters
        if (std::isnan(center.x()) || std::isnan(center.y()) || std::isnan(rx) || std::isnan(ry)) {
            std::cerr << "🚨 SAFE INTERCEPT: QPainter::drawEllipse called with NaN parameters!" << std::endl;
            std::cerr << "  center: (" << center.x() << ", " << center.y() << ")" << std::endl;
            std::cerr << "  rx: " << rx << ", ry: " << ry << std::endl;
            
            // Print backtrace
            void* callstack[128];
            int frames = backtrace(callstack, 128);
            char** strs = backtrace_symbols(callstack, frames);
            std::cerr << "Backtrace:" << std::endl;
            for (int i = 0; i < frames && i < 20; i++) {
                std::cerr << "  " << strs[i] << std::endl;
            }
            free(strs);
            
            std::cerr << "Aborting due to NaN parameters in drawEllipse..." << std::endl;
            abort();
        }
        
        QPainter::drawEllipse(center, rx, ry);
    }
    
    void drawEllipse(const QRectF& rect) {
        // Check for NaN parameters
        if (std::isnan(rect.x()) || std::isnan(rect.y()) || std::isnan(rect.width()) || std::isnan(rect.height())) {
            std::cerr << "🚨 SAFE INTERCEPT: QPainter::drawEllipse(QRectF) called with NaN parameters!" << std::endl;
            std::cerr << "  rect: x=" << rect.x() << ", y=" << rect.y() 
                     << ", w=" << rect.width() << ", h=" << rect.height() << std::endl;
            
            // Print backtrace
            void* callstack[128];
            int frames = backtrace(callstack, 128);
            char** strs = backtrace_symbols(callstack, frames);
            std::cerr << "Backtrace:" << std::endl;
            for (int i = 0; i < frames && i < 20; i++) {
                std::cerr << "  " << strs[i] << std::endl;
            }
            free(strs);
            
            std::cerr << "Aborting due to NaN parameters in drawEllipse..." << std::endl;
            abort();
        }
        
        QPainter::drawEllipse(rect);
    }
};

// Constructor to initialize when library is loaded
__attribute__((constructor))
static void init() {
    std::cerr << "Simple Qt interceptor library loaded" << std::endl;
    std::cerr << "This library provides SafeQPainterPath and SafeQPainter classes" << std::endl;
    std::cerr << "To use them, replace QPainterPath with SafeQPainterPath in your code" << std::endl;
} 