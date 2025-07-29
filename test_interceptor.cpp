// Copyright (c) 2024 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <QApplication>
#include <QPainter>
#include <QPainterPath>
#include <QWidget>
#include <QTimer>
#include <iostream>
#include <cmath>

class TestWidget : public QWidget {
public:
    TestWidget() {
        setFixedSize(400, 300);
        
        // Create a timer to trigger painting
        QTimer* timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, [this]() {
            update(); // Trigger repaint
        });
        timer->start(1000); // Repaint every second
    }
    
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        
        // Test 1: Normal drawing (should work)
        painter.setPen(Qt::green);
        painter.drawEllipse(QPointF(100, 100), 50, 50);
        
        // Test 2: NaN radius (should trigger interceptor)
        painter.setPen(Qt::red);
        double nan_radius = std::numeric_limits<double>::quiet_NaN();
        painter.drawEllipse(QPointF(200, 100), nan_radius, 50);
        
        // Test 3: NaN angle in arcTo (should trigger interceptor)
        QPainterPath path;
        path.moveTo(50, 200);
        double nan_angle = std::numeric_limits<double>::quiet_NaN();
        path.arcTo(QRectF(50, 200, 100, 50), 0, nan_angle);
        
        painter.setPen(Qt::blue);
        painter.drawPath(path);
        
        std::cout << "Test widget painted" << std::endl;
    }
};

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    
    TestWidget widget;
    widget.show();
    
    std::cout << "Test application started. Look for interceptor messages." << std::endl;
    
    return app.exec();
} 