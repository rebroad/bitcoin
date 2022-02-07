// Copyright (c) 2011-2021 The Bitcoin Core developers
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

#define DESIRED_SAMPLES         600

#define XMARGIN                 10
#define YMARGIN                 10

TrafficGraphWidget::TrafficGraphWidget(QWidget *parent) :
    QWidget(parent),
    timer(nullptr),
    disp_timer(nullptr),
    fMax(0.0f),
    new_fMax(0.0f),
    nValue(0),
    vSamplesIn(),
    vSamplesOut(),
    vTimeStamp(),
    nLastBytesIn(),
    nLastBytesOut(),
    nLastTime(),
    nBlanks(),
    clientModel(nullptr)
{
    timer = new QTimer(this);
    disp_timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &TrafficGraphWidget::updateRates);
    connect(disp_timer, &QTimer::timeout, this, &TrafficGraphWidget::updateDisplay);
    timer->setInterval(100);
    disp_timer->setInterval(100); // REBTODO combine these two timers
    timer->start();
    disp_timer->start();
    setMouseTracking(true);
}

void TrafficGraphWidget::setClientModel(ClientModel *model)
{
    clientModel = model;
    if(model) {
        for (int i = 0; i < VALUES_SIZE; i++) {
            nLastBytesIn[i] = model->node().getTotalBytesRecv();
            nLastBytesOut[i] = model->node().getTotalBytesSent();
            nLastTime[i] = GetTimeMillis();
        }
    }
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

void TrafficGraphWidget::focusInEvent(QFocusEvent *evt)
{
    LogPrintf("%s\n", __func__);
    QWidget::focusInEvent(evt);
}

void TrafficGraphWidget::focusOutEvent(QFocusEvent *evt)
{
    LogPrintf("%s\n", __func__);
    QWidget::focusOutEvent(evt);
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
    int sampleSize = vTimeStamp[nValue].size();
    unsigned int smallest_distance = 50; int closest_i = (i >= 0 && i < sampleSize) ? i : -1;
    if (sampleSize && i >= -10 && i < sampleSize + 2 && y <= h + YMARGIN + 3) {
        for (int test_i = std::max(i - 2, 0); test_i < std::min(i + 10, sampleSize); test_i++) {
            float val = floatmax(vSamplesIn[nValue].at(test_i), vSamplesOut[nValue].at(test_i));
            int y_data = y_value(val);
            unsigned int distance = abs(y - y_data);
            if (distance < smallest_distance) {
                smallest_distance = distance;
                closest_i = test_i;
            }
        }
    }
    if (ttpoint != closest_i || closest_i != -1)
        LogPrintf("i=%d x=%d y=%d smdist=%d cl_i=%d\n", i, x-XMARGIN, y-YMARGIN, smallest_distance, closest_i);
    if (ttpoint != closest_i) {
        ttpoint = closest_i;
        update(); // Calls paintEvent() to draw or delete the highlighted point
    }
    last_x = x; last_y = y;
}

void TrafficGraphWidget::mousePressEvent(QMouseEvent *event)
{
    QWidget::mousePressEvent(event);
    int x = event->x();
    int y = event->y();
    fToggle = !fToggle;
    LogPrintf("%: x=%d y=%d\n", __func__, x-XMARGIN, y-YMARGIN);
    update();
}

void TrafficGraphWidget::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), Qt::black);

    int h = height() - YMARGIN * 2; int w = width() - XMARGIN * 2;
    static int last_h = 0; static int last_w = 0;
    if (last_h != h || last_w != w) {
        LogPrintf("%s: w=%d h=%d\n", __func__, w, h);
        last_w = w; last_h = h;
    }

    if(fMax <= 0.0f) return;

    QColor axisCol(Qt::gray);
    painter.setPen(axisCol);
    painter.drawLine(XMARGIN, YMARGIN + h, width() - XMARGIN, YMARGIN + h);

    // decide what order of magnitude we are
    int base = floor(log10(fMax));
    float val = pow(10.0f, base);

    const float yMarginText = 2.0;

    // if we drew 10 or 3 fewer lines, break them up at the next lower order of magnitude
    if(fMax / val <= (fToggle ? 10.0f : 3.0f)) {
        float oldval = val;
        val = pow(10.0f, base - 1);
        painter.setPen(axisCol.darker());
        painter.drawText(XMARGIN, y_value(val)-yMarginText, GUIUtil::formatBytesps(val*1000));
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
            painter.drawText(XMARGIN, yy-yMarginText, GUIUtil::formatBytesps(val*100));
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
    painter.drawText(XMARGIN, y_value(val)-yMarginText, GUIUtil::formatBytesps(val*1000));

    painter.setRenderHint(QPainter::Antialiasing);
    if(!vSamplesIn[nValue].empty()) {
        QPainterPath p;
        paintPath(p, vSamplesIn[nValue]);
        painter.fillPath(p, QColor(0, 255, 0, 128));
        painter.setPen(Qt::green);
        painter.drawPath(p);
    }
    if(!vSamplesOut[nValue].empty()) {
        QPainterPath p;
        paintPath(p, vSamplesOut[nValue]);
        painter.fillPath(p, QColor(255, 0, 0, 128));
        painter.setPen(Qt::red);
        painter.drawPath(p);
    }
    int sampleCount = vTimeStamp[nValue].size();
    if (ttpoint >= 0 && ttpoint < sampleCount) {
        painter.setPen(Qt::yellow);
        int w = width() - XMARGIN * 2;
        int x = XMARGIN + w - w * ttpoint / DESIRED_SAMPLES;
        int y = y_value(floatmax(vSamplesIn[nValue].at(ttpoint), vSamplesOut[nValue].at(ttpoint)));
        painter.drawEllipse(QPointF(x, y), 3, 3);
        QString strTime;
        int64_t sampleTime = vTimeStamp[nValue].at(ttpoint);
        int age = GetTime() - sampleTime/1000;
        if (age < 60*60*23)
            strTime = QString::fromStdString(FormatISO8601Time(sampleTime/1000));
        else
            strTime = QString::fromStdString(FormatISO8601DateTime(sampleTime/1000));
        int milliseconds_between_samples = 1000;
        if (ttpoint > 0)
            milliseconds_between_samples = std::min(milliseconds_between_samples, int(vTimeStamp[nValue].at(ttpoint-1) - sampleTime));
        if (ttpoint + 1 < sampleCount)
            milliseconds_between_samples = std::min(milliseconds_between_samples, int(sampleTime - vTimeStamp[nValue].at(ttpoint+1)));
        if (milliseconds_between_samples < 750)
            strTime += QString::fromStdString(strprintf(".%03d", (sampleTime%1000)));
        QString strData = tr("In") + " " + GUIUtil::formatBytesps(vSamplesIn[nValue].at(ttpoint)*1000) + "\n" + tr("Out") + " " + GUIUtil::formatBytesps(vSamplesOut[nValue].at(ttpoint)*1000);
        // Line below allows ToolTip to move faster than the default ToolTip timeout (10 seconds).
        QToolTip::showText(QPoint(x + x_offset, y + y_offset), strTime + "\n. " + strData);
        QToolTip::showText(QPoint(x + x_offset, y + y_offset), strTime + "\n  " + strData);
        tt_time = GetTime();
    } else
        QToolTip::hideText();
}

void TrafficGraphWidget::updateDisplay()
{
    // This function refreshes or deletes the ToolTip. Also used for smooth Y scaling changes.

    bool fUpdate = false;
    static float increment = 0;
    static float old_fMax = 0;
    if (new_fMax && fMax != new_fMax) {
        fUpdate = true;
        int h = height() - YMARGIN * 2;
        if (!increment) {
            old_fMax = fMax;
            if (fMax) increment = fMax / h;
            else increment = new_fMax / 2;
        } else if (abs(old_fMax - fMax) + increment * 2 < abs(new_fMax - old_fMax) / 2) {
            increment = increment * 2;
        } else {
            increment = abs(new_fMax - fMax) / 2;
        }
        if (abs((h * fMax / new_fMax) - h) > 1) {
            if (new_fMax > fMax)
                fMax += increment;
            else
                fMax -= increment;
        } else {
            fMax = new_fMax;
        }
    } else increment = 0;
    static bool last_fToggle = fToggle;
    if (!QToolTip::isVisible()) {
        if (ttpoint >= 0) { // Remove the yellow circle if the ToolTip has gone due to mouse moving elsewhere.
            if (last_fToggle == fToggle) { // Not lost due to a toggle
                ttpoint = -1;
                LogPrintf("%s: InVisible. Setting ttpoint = -1. age=%d Call update()\n", __func__, GetTime() - tt_time);
            } else {
                last_fToggle = fToggle;
                LogPrintf("%s: InVisible but toggled. Call update()\n", __func__);
            }
            fUpdate = true;
        }
    } else if (ttpoint >= 0 && GetTime() >= tt_time + 9) { // ToolTip is about to expire so refresh it.
        LogPrintf("%s: Visible. Time>=tt_time+9. Call update()\n", __func__);
        fUpdate = true;
    }
    if (fUpdate)
        update();
}

static const std::vector<int> values{1, 2, 5, 10, 20, 30, 60, 2*60, 3*60, 6*60, 12*60, 24*60, 7*24*60, 28*24*60};

void TrafficGraphWidget::updatefMax()
{
    float tmax = 0.0f;
    for (const float f : vSamplesIn[nValue]) {
        if(f > tmax) tmax = f;
    }
    for (const float f : vSamplesOut[nValue]) {
        if(f > tmax) tmax = f;
    }
    static float last_fMax = -1;
    new_fMax = tmax;
    if (new_fMax != last_fMax) {
        LogPrintf("%s: new_fMax = %d -> %d\n", __func__, last_fMax, new_fMax);
        last_fMax = new_fMax;
    }
}

void TrafficGraphWidget::updateRates()
{
    if(!clientModel) return;

    static int nInterval = timer->interval();
    int64_t nTime = GetTimeMillis();

    bool fUpdate = false;
    for (int i = 0; i < VALUES_SIZE-1; i++) {
        int msecsPerSample = values[i] * 60 * 1000 / DESIRED_SAMPLES;
        if (nTime > (nLastTime[i] + msecsPerSample - nInterval/2)) {
            updateRateStep(i);
            if (i == nValue) {
                fUpdate = true;
            }
        }
    }

    if (fUpdate) {
        if (ttpoint >= 0 && ttpoint < DESIRED_SAMPLES) {
            ttpoint++; // Move the selected point to the left
            if (ttpoint >= DESIRED_SAMPLES) ttpoint = -1;
        }
        updatefMax();
        update();
    }
}

void TrafficGraphWidget::updateRateStep(int i)
{
    int64_t nTime = GetTimeMillis();
    quint64 bytesIn = clientModel->node().getTotalBytesRecv(),
            bytesOut = clientModel->node().getTotalBytesSent();
    int nRealInterval = nTime - nLastTime[i];
    float in_rate_kilobytes_per_sec = static_cast<float>(bytesIn - nLastBytesIn[i]) / nRealInterval;
    float out_rate_kilobytes_per_sec = static_cast<float>(bytesOut - nLastBytesOut[i]) / nRealInterval;
    if (!in_rate_kilobytes_per_sec && !out_rate_kilobytes_per_sec) {
        nBlanks[i]++;
        if (nBlanks[i] >= 5) {
            nLastTime[i] = nTime;
            return;
        }
    } else
        nBlanks[i] = 0;
    vSamplesIn[i].push_front(in_rate_kilobytes_per_sec);
    vSamplesOut[i].push_front(out_rate_kilobytes_per_sec);
    vTimeStamp[i].push_front(nLastTime[i]);
    nLastTime[i] = nTime;
    nLastBytesIn[i] = bytesIn;
    nLastBytesOut[i] = bytesOut;

    while(vTimeStamp[i].size() > DESIRED_SAMPLES) {
        vSamplesIn[i].pop_back();
        vSamplesOut[i].pop_back();
        vTimeStamp[i].pop_back();
    }
}

int TrafficGraphWidget::setGraphRangeMins(int value)
{
    nValue = std::min(value, VALUES_SIZE) - 1;
    updatefMax();
    update();

    return values[nValue];
}

void TrafficGraphWidget::clear()
{
    timer->stop();

    vSamplesOut[nValue].clear();
    vSamplesIn[nValue].clear();
    vTimeStamp[nValue].clear();
    new_fMax = 0.0f; fMax = 0.0f;

    if(clientModel) {
        nLastBytesIn[nValue] = clientModel->node().getTotalBytesRecv();
        nLastBytesOut[nValue] = clientModel->node().getTotalBytesSent();
        nLastTime[nValue] = GetTimeMillis();
    }
    update();
    timer->start();
}
