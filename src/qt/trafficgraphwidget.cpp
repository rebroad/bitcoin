// Copyright (c) 2011-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <interfaces/node.h>
#include <qt/trafficgraphwidget.h>
#include <qt/clientmodel.h>

#include <QPainter>
#include <QPainterPath>
#include <QColor>
#include <QTimer>
#include <QHelpEvent>
#include <QToolTip>

#include <cmath>

#define DESIRED_SAMPLES         800

#define XMARGIN                 10
#define YMARGIN                 10

TrafficGraphWidget::TrafficGraphWidget(QWidget *parent) :
    QWidget(parent),
    timer(nullptr),
    fMax(0.0f),
    nMins(0),
    vSamplesIn(),
    vSamplesOut(),
    vTimeStamp(),
    nLastBytesIn(0),
    nLastBytesOut(0),
    clientModel(nullptr)
{
    timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &TrafficGraphWidget::updateRates);
    setMouseTracking(true);
}

void TrafficGraphWidget::setClientModel(ClientModel *model)
{
    clientModel = model;
    if(model) {
        nLastBytesIn = model->node().getTotalBytesRecv();
        nLastBytesOut = model->node().getTotalBytesSent();
    }
}

int TrafficGraphWidget::getGraphRangeMins() const
{
    return nMins;
}

int TrafficGraphWidget::y_value(float value)
{
    return YMARGIN + h - (h * 1.0 * (fToggle ? (pow(value, 0.30102) / pow(fMax, 0.30102)) : (value / fMax)));
}

void TrafficGraphWidget::paintPath(QPainterPath &path, QQueue<float> &samples)
{
    int sampleCount = samples.size();
    if(sampleCount > 0 && fMax > 0) {
        int w = width() - XMARGIN * 2;
        int x = XMARGIN + w;
        path.moveTo(x, YMARGIN + h);
        for(int i = 0; i < sampleCount; ++i) {
            x = XMARGIN + w - w * i / DESIRED_SAMPLES;
            int y = y_value(samples.at(i));
            path.lineTo(x, y);
        }
        path.lineTo(x, YMARGIN + h);
    }
}

void TrafficGraphWidget::mousePressEvent(QMouseEvent *event)
{
    QWidget::mousePressEvent(event);
    fToggle = !fToggle;
    update();
}

float floatmax(float a, float b)
{
    if (a > b) return a;
    else return b;
}

void TrafficGraphWidget::mouseMoveEvent(QMouseEvent *event)
{
    static int x = -1;
    static int y = 0;
    static int last_x = -1;
    static int last_y = -1;
    QWidget::mouseMoveEvent(event);
    x = event->x();
    y = event->y();
    x_offset = event->globalX() - x;
    y_offset = event->globalY() - y;
    if (x == last_x && y == last_y) return;

    last_x = x; last_y = y;
    int w = width() - XMARGIN * 2;
    int i = (w + XMARGIN - x) * DESIRED_SAMPLES / w;
    int last_ttpoint = DESIRED_SAMPLES; // a value that the new one cannot equal
    unsigned int smallest_distance = h; int closest_i = -1;
    int sampleSize = vTimeStamp.size();
    if (i >= -8 && i < sampleSize + 2 && y <= h + YMARGIN + 3) {
        for (int test_i = i - 2; test_i <= i + 8; test_i++) {
            if (test_i < 0 || test_i >= sampleSize) continue;
            float val = floatmax(vSamplesIn.at(test_i), vSamplesOut.at(test_i));
            int y_data = y_value(val);
            unsigned int distance = abs(y - y_data);
            if (distance < smallest_distance) {
                smallest_distance = distance;
                closest_i = test_i;
            }
        }
    }
    LogPrintf("i=%d h=%d y-margin=%d smdist=%d cl_i=%d\n", i, h, y-YMARGIN, smallest_distance, closest_i);
    ttpoint = closest_i;
    if (ttpoint != last_ttpoint) update(); // Calls paintEvent() to draw or delete the highlighted point
}

void TrafficGraphWidget::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), Qt::black);

    if(fMax <= 0.0f) return;

    QColor axisCol(Qt::gray);
    h = height() - YMARGIN * 2;
    painter.setPen(axisCol);
    painter.drawLine(XMARGIN, YMARGIN + h, width() - XMARGIN, YMARGIN + h);

    // decide what order of magnitude we are
    int base = floor(log10(fMax));
    float val = pow(10.0f, base);

    const QString units = tr("kB/s");
    const float yMarginText = 2.0;

    // if we drew 10 or 3 fewer lines, break them up at the next lower order of magnitude
    if(fMax / val <= (fToggle ? 10.0f : 3.0f)) {
        float oldval = val;
        val = pow(10.0f, base - 1);
        painter.setPen(axisCol.darker());
        painter.drawText(XMARGIN, y_value(val)-yMarginText, QString("%1 %2").arg(val).arg(units));
        if (fToggle) {
            int yy = y_value(val*0.1);
            painter.drawText(XMARGIN, yy-yMarginText, QString("%1 %2").arg(val*0.1).arg(units));
            painter.drawLine(XMARGIN, yy, width() - XMARGIN, yy);
        }
        int count = 1;
        for(float y = val; y < (!fToggle || fMax / val < 20 ? fMax : oldval); y += val, count++) {
            if(count % 10 == 0)
                continue;
            int yy = y_value(y);
            painter.drawLine(XMARGIN, yy, width() - XMARGIN, yy);
        }
        val = oldval;
    }
    // draw lines
    painter.setPen(axisCol);
    for(float y = val; y < fMax; y += val) {
        int yy = y_value(y);
        painter.drawLine(XMARGIN, yy, width() - XMARGIN, yy);
    }
    painter.drawText(XMARGIN, y_value(val)-yMarginText, QString("%1 %2").arg(val).arg(units));

    painter.setRenderHint(QPainter::Antialiasing);
    if(!vSamplesIn.empty()) {
        QPainterPath p;
        paintPath(p, vSamplesIn);
        painter.fillPath(p, QColor(0, 255, 0, 128));
        painter.setPen(Qt::green);
        painter.drawPath(p);
    }
    if(!vSamplesOut.empty()) {
        QPainterPath p;
        paintPath(p, vSamplesOut);
        painter.fillPath(p, QColor(255, 0, 0, 128));
        painter.setPen(Qt::red);
        painter.drawPath(p);
    }
    int sampleCount = vTimeStamp.size();
    if (ttpoint >= 0 && ttpoint < sampleCount) {
        painter.setPen(Qt::yellow);
        int w = width() - XMARGIN * 2;
        int x = XMARGIN + w - w * ttpoint / DESIRED_SAMPLES;
        int y = y_value(floatmax(vSamplesIn.at(ttpoint), vSamplesOut.at(ttpoint)));
        painter.drawEllipse(QPointF(x,y), 3, 3);

        std::string strTime;
        int64_t sampleTime = vTimeStamp.at(ttpoint);
        int age = GetTime() - sampleTime/1000;
        if (age < 60*60*23)
            strTime = FormatISO8601Time(sampleTime/1000);
        else
            strTime = FormatISO8601DateTime(sampleTime/1000);
        int milliseconds_between_samples = 1000;
        if (ttpoint > 0)
            milliseconds_between_samples = std::min(milliseconds_between_samples, int(vTimeStamp.at(ttpoint-1) - sampleTime));
        if (ttpoint + 1 < sampleCount)
            milliseconds_between_samples = std::min(milliseconds_between_samples, int(sampleTime - vTimeStamp.at(ttpoint+1)));
        if (milliseconds_between_samples < 1000)
            strTime += strprintf(".%03d", (sampleTime%1000));
        strTime += strprintf("\nIn %s\nOut %s", strBytesps(vSamplesIn.at(ttpoint)*1000), strBytesps(vSamplesOut.at(ttpoint)*1000));
        QToolTip::showText(QPoint(x + x_offset, y + y_offset), QString::fromStdString(strTime));
    } else
        QToolTip::hideText();
}

void TrafficGraphWidget::updateRates()
{
    if(!clientModel) return;

    int64_t nTime = GetTimeMillis();
    static int64_t nLastTime = nTime - timer->interval();
    int nRealInterval = nTime - nLastTime;
    quint64 bytesIn = clientModel->node().getTotalBytesRecv(),
            bytesOut = clientModel->node().getTotalBytesSent();
    float in_rate_kilobytes_per_sec = static_cast<float>(bytesIn - nLastBytesIn) / nRealInterval;
    float out_rate_kilobytes_per_sec = static_cast<float>(bytesOut - nLastBytesOut) / nRealInterval;
    vSamplesIn.push_front(in_rate_kilobytes_per_sec);
    vSamplesOut.push_front(out_rate_kilobytes_per_sec);
    vTimeStamp.push_front(nLastTime);
    nLastTime = nTime;
    nLastBytesIn = bytesIn;
    nLastBytesOut = bytesOut;

    while(vSamplesIn.size() > DESIRED_SAMPLES) {
        vSamplesIn.pop_back();
    }
    while(vSamplesOut.size() > DESIRED_SAMPLES) {
        vSamplesOut.pop_back();
    }
    while(vTimeStamp.size() > DESIRED_SAMPLES) {
        vTimeStamp.pop_back();
    }

    float tmax = 0.0f;
    for (const float f : vSamplesIn) {
        if(f > tmax) tmax = f;
    }
    for (const float f : vSamplesOut) {
        if(f > tmax) tmax = f;
    }
    fMax = tmax;
    if (ttpoint >=0 && ttpoint < vTimeStamp.size()) ttpoint++; // Move the selected point to the left
    update();
}

void TrafficGraphWidget::setGraphRangeMins(int mins)
{
    nMins = mins;
    int msecsPerSample = nMins * 60 * 1000 / DESIRED_SAMPLES;
    timer->stop();
    timer->setInterval(msecsPerSample);
    timer->start();
}

void TrafficGraphWidget::clear()
{
    timer->stop();

    vSamplesOut.clear();
    vSamplesIn.clear();
    vTimeStamp.clear();
    fMax = 0.0f;

    if(clientModel) {
        nLastBytesIn = clientModel->node().getTotalBytesRecv();
        nLastBytesOut = clientModel->node().getTotalBytesSent();
    }
    timer->start();
}
