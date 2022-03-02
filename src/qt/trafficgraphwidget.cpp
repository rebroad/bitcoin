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

#include <chrono>
#include <cmath>

#define DESIRED_SAMPLES         800

#define XMARGIN                 10
#define YMARGIN                 10

TrafficGraphWidget::TrafficGraphWidget(QWidget *parent) :
    QWidget(parent),
    timer(nullptr),
    vSamplesIn(),
    vSamplesOut(),
    vTimeStamp(),
    nLastBytesIn(),
    nLastBytesOut(),
    nLastTime(),
    clientModel(nullptr)
{
    timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &TrafficGraphWidget::updateStuff);
    timer->setInterval(75);
    timer->start();
    setMouseTracking(true);
}

void TrafficGraphWidget::setClientModel(ClientModel *model)
{
    clientModel = model;
    int64_t nTime = GetTimeMillis();
    if(model) {
        for (int i = 0; i < VALUES_SIZE; i++) {
            nLastBytesIn[i] = model->node().getTotalBytesRecv();
            nLastBytesOut[i] = model->node().getTotalBytesSent();
            nLastTime[i] = std::chrono::milliseconds{nTime};
            vSamplesIn[i].push_front(nLastBytesIn[i]);
            vSamplesOut[i].push_front(nLastBytesOut[i]);
            vTimeStamp[i].push_front(nLastTime[i]);
        }
    }
}

bool TrafficGraphWidget::GraphRangeBump() const { return m_bump_value; }

int TrafficGraphWidget::y_value(float value)
{
    int h = height() - YMARGIN * 2;
    return YMARGIN + h - (h * 1.0 * (fToggle ? (pow(value, 0.30102) / pow(fMax, 0.30102)) : (value / fMax)));
}

void TrafficGraphWidget::paintPath(QPainterPath &path, QQueue<float> &samples)
{
    int sampleCount = std::min(int(DESIRED_SAMPLES * m_range / values[m_value]), int(samples.size()));
    if(sampleCount > 0) {
        int h = height() - YMARGIN * 2, w = width() - XMARGIN * 2;
        int x = XMARGIN + w;
        path.moveTo(x, YMARGIN + h);
        for(int i = 0; i < sampleCount; ++i) {
            x = XMARGIN + w - w * i * values[m_value] / m_range / DESIRED_SAMPLES;
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
    if (fMax <= 0.0f) return;
    static int last_x = -1;
    static int last_y = -1;
    int x = event->x();
    int y = event->y();
    x_offset = event->globalX() - x;
    y_offset = event->globalY() - y;
    if (last_x == x && last_y == y) return; // Do nothing if mouse hasn't moved
    int h = height() - YMARGIN * 2, w = width() - XMARGIN * 2;
    int i = (w + XMARGIN - x) * DESIRED_SAMPLES / w;
    int sampleSize = vTimeStamp[m_value].size();
    unsigned int smallest_distance = 50; int closest_i = (i >= 0 && i < sampleSize) ? i : -1;
    if (sampleSize && i >= -10 && i < sampleSize + 2 && y <= h + YMARGIN + 3) {
        for (int test_i = std::max(i - 2, 0); test_i < std::min(i + 10, sampleSize); test_i++) {
            float val = floatmax(vSamplesIn[m_value].at(test_i), vSamplesOut[m_value].at(test_i));
            int y_data = y_value(val);
            unsigned int distance = abs(y - y_data);
            if (distance < smallest_distance) {
                smallest_distance = distance;
                closest_i = test_i;
            }
        }
    }
    //if (ttpoint != closest_i || closest_i != -1)
    //    LogPrintf("i=%d h=%d x=%d y=%d smdist=%d cl_i=%d\n", i, h, x-XMARGIN, y-YMARGIN, smallest_distance, closest_i);
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
    if(!vSamplesIn[m_value].empty()) {
        QPainterPath p;
        paintPath(p, vSamplesIn[m_value]);
        painter.fillPath(p, QColor(0, 255, 0, 128));
        painter.setPen(Qt::green);
        painter.drawPath(p);
    }
    if(!vSamplesOut[m_value].empty()) {
        QPainterPath p;
        paintPath(p, vSamplesOut[m_value]);
        painter.fillPath(p, QColor(255, 0, 0, 128));
        painter.setPen(Qt::red);
        painter.drawPath(p);
    }
    int sampleCount = vTimeStamp[m_value].size();
    if (ttpoint >= 0 && ttpoint < sampleCount) {
        painter.setPen(Qt::yellow);
        int w = width() - XMARGIN * 2;
        int x = XMARGIN + w - w * ttpoint / DESIRED_SAMPLES;
        int y = y_value(floatmax(vSamplesIn[m_value].at(ttpoint), vSamplesOut[m_value].at(ttpoint)));
        painter.drawEllipse(QPointF(x, y), 3, 3);
        QString strTime;
        std::chrono::milliseconds sampleTime{0};
        if (ttpoint + 1 < sampleCount)
            sampleTime = vTimeStamp[m_value].at(ttpoint+1);
        else {
            strTime = "to ";
            sampleTime = vTimeStamp[m_value].at(ttpoint);
        }
        int age = GetTime() - sampleTime.count() / 1000;
        if (age < 60*60*23)
            strTime += QString::fromStdString(FormatISO8601Time(sampleTime.count() / 1000));
        else
            strTime += QString::fromStdString(FormatISO8601DateTime(sampleTime.count() / 1000));
        int nDuration = (vTimeStamp[m_value].at(ttpoint) - sampleTime).count();
        if (nDuration > 0) {
            if (nDuration > 9999)
                strTime += " +" + GUIUtil::formatDurationStr(std::chrono::seconds{(nDuration+500)/1000});
            else
                strTime += " +" + GUIUtil::formatPingTime(std::chrono::microseconds{nDuration*1000});
        } else // REBTEMP
            strTime += QString::fromStdString(strprintf(" i=%d ttp=%d nDur=%d", m_value, ttpoint, nDuration));
        QString strData = tr("In") + " " + GUIUtil::formatBytesps(vSamplesIn[m_value].at(ttpoint)*1000) + "\n" + tr("Out") + " " + GUIUtil::formatBytesps(vSamplesOut[m_value].at(ttpoint)*1000);
        // Line below allows ToolTip to move faster than the default ToolTip timeout (10 seconds).
        QToolTip::showText(QPoint(x + x_offset, y + y_offset), strTime + "\n. " + strData);
        QToolTip::showText(QPoint(x + x_offset, y + y_offset), strTime + "\n  " + strData);
        tt_time = GetTime();
    } else
        QToolTip::hideText();
}

void TrafficGraphWidget::update_fMax()
{
    float tmax = 0.0f;
    for (const float f : vSamplesIn[m_new_value]) {
        if(f > tmax) tmax = f;
    }
    for (const float f : vSamplesOut[m_new_value]) {
        if(f > tmax) tmax = f;
    }
    new_fMax = tmax;
    static float last_fMax = -1;
    if (new_fMax != last_fMax) {
        LogPrintf("%s: i=%d new_fMax = %d -> %d\n", __func__, m_new_value, last_fMax, new_fMax);
        last_fMax = new_fMax;
    }
}

bool update_num(float new_val, float &current, float &increment, int length)
{
    if (new_val == 0 || current == new_val)
        return false;

    if (abs(increment) <= abs(0.8 * current) / length) { // allow equal to as current and increment could be zero
        int old_increment = increment;
        if (new_val > current)
            increment = 1.0 * (current+1) / length; // +1s are to get it started even if current is zero
        else
            increment = -1.0 * (current+1) / length;
        if (abs(increment) > abs(new_val - current)) // Only check this when creating an increment
            increment = 0; // Nothing to do!
        LogPrintf("%s: new increment: %d+1 / %d = %d->%d\n", __func__, current, length, old_increment, increment);
    } else {
        if (((increment > 0) && (current + increment * 2 > new_val)) ||
                ((increment < 0) && (current + increment * 2 < new_val))) {
            increment = increment / 2; // Keep the momentum going even if new_val is elsewhere.
        } else {
            if (((increment > 0) && (current + increment * 8 < new_val)) ||
                    ((increment < 0) && (current + increment * 8 > new_val))) {
                increment = increment * 2;
            }
        }
    }
    if (abs(increment) < 0.8 * current / length) {
        if ((increment >= 0 && new_val > current) || (increment <= 0 && new_val < current)) {
            if (increment)
                LogPrintf("%s: final jump. inc=%d < 0.8 * %d / %d\n", __func__, abs(increment), current, length);
            current = new_val;
        }
        increment = 0;
    } else
        current += increment;

    return true;
}

void TrafficGraphWidget::updateStuff()
{
    if(!clientModel) return;

    static int nInterval{timer->interval()};
    int64_t nTime{GetTimeMillis()};

    bool fUpdate = false;
    for (int i = 0; i < VALUES_SIZE; i++) {
        int64_t msecs_per_sample = int64_t(values[i]) * int64_t(60000) / DESIRED_SAMPLES;
        if (nTime > (nLastTime[i].count() + msecs_per_sample - nInterval/2)) { // REBTODO - fix bad timing
            updateRates(i);
            if (i == m_value) {
                if (ttpoint >= 0 && ttpoint < DESIRED_SAMPLES) {
                    ttpoint++; // Move the selected point to the left
                    if (ttpoint >= DESIRED_SAMPLES) ttpoint = -1;
                }
                fUpdate = true;
            }
            if (i == m_new_value)
                update_fMax();
        }
    }

    static float y_increment = 0;
    static float x_increment = 0;
    if (update_num(new_fMax, fMax, y_increment, height() - YMARGIN * 2))
        fUpdate = true;
    if (update_num(values[m_new_value], m_range, x_increment, width() - XMARGIN * 2)) {
        if (values[m_new_value] > m_range && values[m_value] < m_range * 0.99) {
            LogPrintf("%s: m_value %d->%d m_range %d->%d cur_range=%d\n", __func__, m_value, m_value+1,
                values[m_value], values[m_value+1], m_range);
            m_value++;
        } else if (m_value > 0 && values[m_new_value] <= m_range && values[m_value-1] > m_range * 0.99) {
            LogPrintf("%s: m_value %d->%d m_range %d->%d cur_range=%d\n", __func__, m_value, m_value-1,
                values[m_value], values[m_value-1], m_range);
            m_value--;
        }
        fUpdate = true;
        LogPrintf("%s: new_range=%d range=%d new_val=%d val=%d increment=%d\n", __func__, values[m_new_value], m_range, m_new_value, m_value, x_increment);
    } else if (m_value != m_new_value) {
        LogPrintf("%s: CAUGHT! m_value %d->%d\n", __func__, m_value, m_new_value);
        fUpdate = true;
        m_value = m_new_value;
    }

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
        fUpdate = true; // TODO - technically it's only the ToolTip that needs to be refreshed
    }

    if (fUpdate)
        update();
}

void TrafficGraphWidget::updateRates(int i)
{
    std::chrono::milliseconds nTime{GetTimeMillis()};
    quint64 bytesIn = clientModel->node().getTotalBytesRecv(),
            bytesOut = clientModel->node().getTotalBytesSent();
    int nRealInterval = (nTime - nLastTime[i]).count();
    static int nDebugI = 0;
    if (i > nDebugI) nDebugI = i;
    if (nDebugI == i)
        LogPrintf("%s: i=%d mins=%d nRI=%d\n", __func__, i, values[i], nRealInterval);
    float in_rate_kilobytes_per_sec = static_cast<float>(bytesIn - nLastBytesIn[i]) / nRealInterval;
    float out_rate_kilobytes_per_sec = static_cast<float>(bytesOut - nLastBytesOut[i]) / nRealInterval;
    vSamplesIn[i].push_front(in_rate_kilobytes_per_sec);
    vSamplesOut[i].push_front(out_rate_kilobytes_per_sec);
    vTimeStamp[i].push_front(nTime);
    nLastTime[i] = nTime;
    nLastBytesIn[i] = bytesIn;
    nLastBytesOut[i] = bytesOut;
    static bool fFull[VALUES_SIZE];
    if (!fFull[i] && vTimeStamp[i].size()+5 > DESIRED_SAMPLES)
        LogPrintf("%s: fFull[%d] %d steps from full\n", __func__, i, DESIRED_SAMPLES - vTimeStamp[i].size());
    while(vTimeStamp[i].size() > DESIRED_SAMPLES) {
        if (ttpoint < 0 && m_value == i && i < VALUES_SIZE - 1 && !fFull[i])
            m_bump_value = true;

        fFull[i] = true;

        vSamplesIn[i].pop_back();
        vSamplesOut[i].pop_back();
        vTimeStamp[i].pop_back();
    }
}

std::chrono::minutes TrafficGraphWidget::setGraphRange(unsigned int value)
{
    // value is the array marker plus 1 (as zero is reserved for bumping up)
    if (!value) { // bump
        m_bump_value = false;
        value = m_value + 1;
    } else
        value--; // get the array marker
    int old_value = m_new_value;
    m_new_value = std::min((int)value, VALUES_SIZE - 1);
    if (m_new_value != old_value) {
        update_fMax();
        update();
    }

    return std::chrono::minutes{values[m_new_value]};
}
