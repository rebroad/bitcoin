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

void TrafficGraphWidget::paintPath(QPainterPath &path, QQueue<float> &samples)
{
    int sampleCount = samples.size();
    if(sampleCount > 0 && fMax > 0) {
        int h = height() - YMARGIN * 2, w = width() - XMARGIN * 2;
        int x = XMARGIN + w;
        path.moveTo(x, YMARGIN + h);
        for(int i = 0; i < sampleCount; ++i) {
            x = XMARGIN + w - w * i / DESIRED_SAMPLES;
            int y = YMARGIN + h - (int)(h * 1.0 * (fToggle ? (pow(samples.at(i), 0.30102) / pow(fMax, 0.30102)) : (samples.at(i) / fMax)));
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

void TrafficGraphWidget::UpdateToolTip(QMouseEvent *event, bool fShiftLeft/*=false*/)
{
    static int x = -1;
    static int global_x = 0;
    static int y = 0;
    static int global_y = 0;
    static int last_x = -1;
    static int last_y = -1;
    if (event) {
        x = event->x();
        global_x = event->globalX();
        y = event->y();
        global_y = event->globalY();
    }
    if (x == -1) return;

    int w = width() - XMARGIN * 2;
    int sampleCount = vTimeStamp.size();
    int real_i = (w + XMARGIN - x) * DESIRED_SAMPLES / w;
    static int i = real_i;
    int new_x = x;
    if (x == last_x && y == last_y) { // Follow ToolTip value if mouse has not moved
        if (fShiftLeft && i>=0 && i < sampleCount) i++;
        new_x = XMARGIN + w - w * i / DESIRED_SAMPLES;
        static int old_i = -1; static int old_new_x = -1;
        bool new_event = event ? true : false;
        if (i != old_i || new_x != old_new_x || new_event) {
            LogPrintf("No movement. x=%d y=%d new_x=%d i=%d real_i=%d w=%d event=%d left=%d\n", x, y, new_x, i, real_i, w, event ? 1:0, fShiftLeft ? 1:0);
            old_i = i; old_new_x = new_x;
        }
    } else {
        i = real_i;
        LogPrintf("Movement! x=%d->%d y=%d->%d i=%d event=%d\n", last_x, x, last_y, y, i, event ? 1:0);
        last_x = x; last_y = y;
    }
    if (i >= 0 && i < sampleCount) {
        std::string strTime = FormatISO8601Time(vTimeStamp.at(i)/1000);
        int milliseconds_between_samples = 1000;
        if (i > 0)
            milliseconds_between_samples = std::min(milliseconds_between_samples, int(vTimeStamp.at(i-1) - vTimeStamp.at(i)));
        if (i + 1 < sampleCount)
            milliseconds_between_samples = std::min(milliseconds_between_samples, int(vTimeStamp.at(i) - vTimeStamp.at(i+1)));
        if (milliseconds_between_samples < 1000)
            strTime += strprintf(".%03d", (vTimeStamp.at(i))%1000);
        QToolTip::showText(QPoint(global_x,global_y), QString::fromStdString(strTime));
    } else
        QToolTip::hideText();
}

void TrafficGraphWidget::mouseMoveEvent(QMouseEvent *event)
{
    UpdateToolTip(event);
    QWidget::mouseMoveEvent(event);
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
        painter.drawText(XMARGIN, YMARGIN + h - (h * 1.0 * (fToggle ? (pow(val, 0.30102) / pow(fMax, 0.30102)) : (val / fMax)))-yMarginText, QString("%1 %2").arg(val).arg(units));
        if (fToggle) {
            painter.drawText(XMARGIN, YMARGIN + h - (h * 1.0 * pow(val*0.1, 0.30102) / pow(fMax, 0.30102))-yMarginText, QString("%1 %2").arg(val*0.1).arg(units));
            int yy = YMARGIN + h - (h * 1.0 * pow(val*0.1, 0.30102) / pow(fMax, 0.30102));
            painter.drawLine(XMARGIN, yy, width() - XMARGIN, yy);
        }
        int count = 1;
        for(float y = val; y < (!fToggle || fMax / val < 20 ? fMax : oldval); y += val, count++) {
            if(count % 10 == 0)
                continue;
            int yy = YMARGIN + h - (h * 1.0 * (fToggle ? (pow(y, 0.30102) / pow(fMax, 0.30102)) : (y / fMax)));
            painter.drawLine(XMARGIN, yy, width() - XMARGIN, yy);
        }
        val = oldval;
    }
    // draw lines
    painter.setPen(axisCol);
    for(float y = val; y < fMax; y += val) {
        int yy = YMARGIN + h - (h * 1.0 * (fToggle ? (pow(y, 0.30102) / pow(fMax, 0.30102)) : (y / fMax)));
        painter.drawLine(XMARGIN, yy, width() - XMARGIN, yy);
    }
    painter.drawText(XMARGIN, YMARGIN + h - (h * 1.0 * (fToggle ? (pow(val, 0.30102) / pow(fMax, 0.30102)) : (val / fMax)))-yMarginText, QString("%1 %2").arg(val).arg(units));

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
    QMouseEvent *mouseevent = nullptr;
    UpdateToolTip(mouseevent); // Update the ToolTip
}

void TrafficGraphWidget::updateRates()
{
    if(!clientModel) return;

    static int64_t nTime = GetTimeMillis();
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
    update();
    QMouseEvent *mouseevent = nullptr;
    UpdateToolTip(mouseevent, true); // Update the ToolTip
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
