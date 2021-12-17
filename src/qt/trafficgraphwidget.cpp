// Copyright (c) 2011-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <interfaces/node.h>
#include <qt/trafficgraphwidget.h>
#include <qt/clientmodel.h>
#include <qt/guiutil.h>

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
    disp_timer(nullptr),
    fMax(0.0f),
    new_fMax(0.0f),
    nMins(0),
    vSamplesIn(),
    vSamplesOut(),
    vTimeStamp(),
    nLastBytesIn(0),
    nLastBytesOut(0),
    nLastTime(0),
    clientModel(nullptr)
{
    timer = new QTimer(this);
    disp_timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &TrafficGraphWidget::updateRates);
    connect(disp_timer, &QTimer::timeout, this, &TrafficGraphWidget::updateDisplay);
    disp_timer->setInterval(100);
    disp_timer->start();
    setMouseTracking(true);
}

void TrafficGraphWidget::setClientModel(ClientModel *model)
{
    clientModel = model;
    if(model) {
        nLastBytesIn = model->node().getTotalBytesRecv();
        nLastBytesOut = model->node().getTotalBytesSent();
        nLastTime = GetTimeMillis();
    }
}

int TrafficGraphWidget::getGraphRangeMins() const
{
    return nMins;
}

int TrafficGraphWidget::y_value(float value)
{
    int h = height() - YMARGIN * 2;
    return YMARGIN + h - (h * 1.0 * (fToggle ? (pow(value, 0.30102) / pow(fMax, 0.30102)) : (value / fMax)));
}

void TrafficGraphWidget::paintPath(QPainterPath &path, QQueue<float> &samples)
{
    int sampleCount = samples.size();
    if(sampleCount > 0) {
        int h = height() - YMARGIN * 2, w = width() - XMARGIN * 2;
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

float floatmax(float a, float b)
{
    if (a > b) return a;
    else return b;
}

void TrafficGraphWidget::mouseMoveEvent(QMouseEvent *event)
{
    QWidget::mouseMoveEvent(event);
    static int last_x = -1;
    static int last_y = -1;
    int x = event->x();
    int y = event->y();
    x_offset = event->globalX() - x;
    y_offset = event->globalY() - y;
    if (last_x == x && last_y == y) return; // Do nothing if mouse hasn't moved
    int h = height() - YMARGIN * 2, w = width() - XMARGIN * 2;
    int i = (w + XMARGIN - x) * DESIRED_SAMPLES / w;
    unsigned int smallest_distance = 50; int closest_i = -1;
    int sampleSize = vTimeStamp.size();
    if (sampleSize && i >= -10 && i < sampleSize + 2 && y <= h + YMARGIN + 3) {
        for (int test_i = std::max(i - 2, 0); test_i < std::min(i + 10, sampleSize); test_i++) {
            float val = floatmax(vSamplesIn.at(test_i), vSamplesOut.at(test_i));
            int y_data = y_value(val);
            unsigned int distance = abs(y - y_data);
            if (distance < smallest_distance) {
                smallest_distance = distance;
                closest_i = test_i;
            }
        }
    }
    if (ttpoint != closest_i || closest_i != -1)
        LogPrintf("i=%d h=%d x=%d y=%d smdist=%d cl_i=%d\n", i, h, x-XMARGIN, y-YMARGIN, smallest_distance, closest_i);
    if (ttpoint != closest_i) {
        ttpoint = closest_i;
        update(); // Calls paintEvent() to draw or delete the highlighted point
    }
    last_x = x; last_y = y;
}

void TrafficGraphWidget::mousePressEvent(QMouseEvent *event)
{
    QWidget::mousePressEvent(event);
    fToggle = !fToggle;
    LogPrintf("%: here\n", __func__);
    update();
}

void TrafficGraphWidget::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), Qt::black);

    if(fMax <= 0.0f) return;

    QColor axisCol(Qt::gray);
    int h = height() - YMARGIN * 2;
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
        int count = 1;
        for(float y = val; y < (!fToggle || fMax / val < 20 ? fMax : oldval); y += val, count++) {
            if(count % 10 == 0)
                continue;
            int yy = y_value(y);
            painter.drawLine(XMARGIN, yy, width() - XMARGIN, yy);
        }
        if (fToggle) {
            int yy = y_value(val*0.1);
            painter.setPen(axisCol.darker().darker());
            painter.drawText(XMARGIN, yy-yMarginText, QString("%1 %2").arg(val*0.1).arg(units));
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
        painter.drawEllipse(QPointF(x, y), 3, 3);
        QString strTime;
        int64_t sampleTime = vTimeStamp.at(ttpoint);
        int age = GetTime() - sampleTime/1000;
        if (age < 60*60*23)
            strTime = QString::fromStdString(FormatISO8601Time(sampleTime/1000));
        else
            strTime = QString::fromStdString(FormatISO8601DateTime(sampleTime/1000));
        int milliseconds_between_samples = 1000;
        if (ttpoint > 0)
            milliseconds_between_samples = std::min(milliseconds_between_samples, int(vTimeStamp.at(ttpoint-1) - sampleTime));
        if (ttpoint + 1 < sampleCount)
            milliseconds_between_samples = std::min(milliseconds_between_samples, int(sampleTime - vTimeStamp.at(ttpoint+1)));
        if (milliseconds_between_samples < 1000)
            strTime += QString::fromStdString(strprintf(".%03d", (sampleTime%1000)));
        QString strData = tr("In") + " " + GUIUtil::formatBytesps(vSamplesIn.at(ttpoint)*1000) + "\n" + tr("Out") + " " + GUIUtil::formatBytesps(vSamplesOut.at(ttpoint)*1000);
        // Line below allows ToolTip to move faster than once every 10 seconds.
        QToolTip::showText(QPoint(x + x_offset, y + y_offset), strTime + "\n. " + strData);
        QToolTip::showText(QPoint(x + x_offset, y + y_offset), strTime + "\n  " + strData);
        tt_time = GetTime();
    } else
        QToolTip::hideText();
}

void TrafficGraphWidget::updateToolTip()
{
    static bool last_fToggle = fToggle;
    if (!QToolTip::isVisible()) {
        if (ttpoint >= 0) { // Remove the yellow circle if the ToolTip has gone due to mouse moving elsewhere.
            if (last_fToggle == fToggle) { // Not lost due to a toggle
                ttpoint = -1;
                LogPrintf("%s: InVisible. Setting ttpoint = -1. Call update()\n", __func__);
            } else
                LogPrintf("%s: InVisible but toggled. Call update()\n", __func__);
            update();
            last_fToggle = fToggle;
        }
    } else if (GetTime() >= tt_time + 9) { // ToolTip is about to expire so refresh it.
        LogPrintf("%s: Visible. Time>=tt_time+9. Call update()\n", __func__);
        update();
        last_fToggle = fToggle;
    }
}

void TrafficGraphWidget::updateRates()
{
    if(!clientModel) return;

    int64_t nTime = GetTimeMillis();
    int nRealInterval = nTime - nLastTime;
    quint64 bytesIn = clientModel->node().getTotalBytesRecv(),
            bytesOut = clientModel->node().getTotalBytesSent();
    //static int64_t nLastReport = 0;
    //int nInterval = timer->interval();
    //if ((nTime >= nLastReport + 10000) && ((nRealInterval <= nInterval * 0.9) || (nRealInterval >= nInterval * 1.1))) {
    //    LogPrintf("%s: nInterval=%d nRealInterval=%d\n", __func__, nInterval, nRealInterval);
    //    nLastReport = nTime;
    //}
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
        nLastTime = GetTimeMillis();
    }
    timer->start();
}
