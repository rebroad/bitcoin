// Copyright (c) 2011-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <interfaces/node.h>
#include <qt/trafficgraphwidget.h>
#include <qt/clientmodel.h>
#include <qt/guiutil.h>
#include <clientversion.h>

#include <QPainter>
#include <QPainterPath>
#include <QColor>
#include <QTimer>

#include <QHelpEvent>
#include <QToolTip>
#include <QTextStream>
#include <chrono>
#include <cmath>

#define DESIRED_SAMPLES         800

#define XMARGIN                 10
#define YMARGIN                 10

TrafficGraphWidget::TrafficGraphWidget(QWidget *parent) :
    QWidget(parent),
    timer(nullptr),
    clientModel(nullptr)
{
    timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &TrafficGraphWidget::updateStuff);
    timer->setInterval(75);
    timer->start();
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus); // Make widget focusable to respond to keyboard events
}

void TrafficGraphWidget::setClientModel(ClientModel *model) {
    if (model) {
        clientModel = model;
        if (vSamplesIn[0].empty() && vSamplesOut[0].empty()) {
            // Load saved traffic data if available and the arrays are empty
            loadData();

            // Restore the saved total bytes counts to the node if they exist
            if (m_totalBytesRecv > 0 || m_totalBytesSent > 0) {
                model->node().setTotalBytesRecv(m_totalBytesRecv);
                model->node().setTotalBytesSent(m_totalBytesSent);
            }

            return;
        }

        uint64_t nTime = GetTimeMillis();
        for (int i = 0; i < VALUES_SIZE; i++) {
            nLastBytesIn[i] = model->node().getTotalBytesRecv();
            nLastBytesOut[i] = model->node().getTotalBytesSent();
            nLastTime[i] = std::chrono::milliseconds{nTime};
        }
    } else {
        saveData();
        clientModel = model;
    }
}

bool TrafficGraphWidget::GraphRangeBump() const { return m_bump_value; }

unsigned int TrafficGraphWidget::getCurrentRangeIndex() const { return m_new_value; }

int TrafficGraphWidget::y_value(float value) const {
    int h = height() - YMARGIN * 2;
    if (fMax <= 0.0001f || value <= std::numeric_limits<float>::epsilon()) return YMARGIN + h;
    float result = fToggle ? pow(value, 0.30102) / pow(fMax, 0.30102) : value / fMax;
    if (std::isnan(result) || std::isinf(result)) return YMARGIN + h;
    return YMARGIN + h - (h * 1.0 * result);
}

void TrafficGraphWidget::paintPath(QPainterPath &path, QQueue<float> &samples) {
    int sampleCount = std::min(int(DESIRED_SAMPLES * m_range / values[m_value]), int(samples.size()));
    if (sampleCount <= 0 || m_range <= 0.0001f) return;

    int h = height() - YMARGIN * 2, w = width() - XMARGIN * 2, x = XMARGIN + w;
    path.moveTo(x, YMARGIN + h);
    for(int i = 0; i < sampleCount; ++i) {
        float sample = samples.at(i);
        if (std::isnan(sample) || std::isinf(sample)) continue;
        double ratio = static_cast<double>(i) * values[m_value] / m_range / DESIRED_SAMPLES;
        if (std::isnan(ratio) || std::isinf(ratio)) continue;
        x = XMARGIN + w - static_cast<int>(w * ratio);
        path.lineTo(x, y_value(sample));
    }
    path.lineTo(x, YMARGIN + h);
}

void TrafficGraphWidget::mouseMoveEvent(QMouseEvent *event) {
    QWidget::mouseMoveEvent(event);
    if (fMax <= 0.0f) return;
    static int last_x = -1, last_y = -1;
    int x = event->x(), y = event->y();
    x_offset = event->globalX() - x; y_offset = event->globalY() - y;
    if (last_x == x && last_y == y) return; // Do nothing if mouse hasn't moved
    int h = height() - YMARGIN * 2, w = width() - XMARGIN * 2;
    int i = (w + XMARGIN - x) * DESIRED_SAMPLES / w, closest_i = -1;
    int sampleSize = vTimeStamp[m_value].size();
    unsigned int smallest_distance = 50;
    bool is_in_series = true;
    if (sampleSize && i >= -10 && i < sampleSize + 2 && y <= h + YMARGIN + 3)
        for (int test_i = std::max(i - 2, 0); test_i < std::min(i + 10, sampleSize); test_i++) {
            float in_val = vSamplesIn[m_value].at(test_i), out_val = vSamplesOut[m_value].at(test_i);
            int y_in = y_value(in_val), y_out = y_value(out_val);
            unsigned int distance_in = abs(y - y_in), distance_out = abs(y - y_out);
            unsigned int min_distance = std::min(distance_in, distance_out);
            if (min_distance < smallest_distance) {
                smallest_distance = min_distance;
                closest_i = test_i;
                is_in_series = (distance_in <= distance_out);
            }
        }
    if (ttpoint != closest_i || (ttpoint >= 0 && closest_i >= 0 && tt_in_series != is_in_series)) {
        ttpoint = closest_i;
        tt_in_series = is_in_series;
        update(); // Calls paintEvent() to draw or delete the highlighted point
    }
    last_x = x; last_y = y;
}

void TrafficGraphWidget::focusSlider(Qt::FocusReason reason) {
    // Find the slider in the parent hierarchy and give it focus
    QWidget* parent = parentWidget();
    while (parent) {
        QSlider* slider = parent->findChild<QSlider*>("sldGraphRange");
        if (slider) {
            slider->setFocus(reason);
            break;
        }
        parent = parent->parentWidget();
    }
}

void TrafficGraphWidget::mousePressEvent(QMouseEvent *event) {
    focusSlider(Qt::MouseFocusReason);
    QWidget::mousePressEvent(event);
    fToggle = !fToggle;
    update();
}

void TrafficGraphWidget::mouseReleaseEvent(QMouseEvent *event) {
    QWidget::mouseReleaseEvent(event);
    focusSlider(Qt::MouseFocusReason);
}

void TrafficGraphWidget::focusInEvent(QFocusEvent *event) {
    QWidget::focusInEvent(event);
    focusSlider(Qt::OtherFocusReason);
}

void TrafficGraphWidget::drawTooltipPoint(QPainter& painter) {
    int w = width() - XMARGIN * 2;
    double ratio = static_cast<double>(ttpoint) * values[m_value] / m_range / DESIRED_SAMPLES;
    int x = XMARGIN + w - static_cast<int>(w * ratio);
    if (ttpoint >= vSamplesIn[m_value].size() || ttpoint >= vSamplesOut[m_value].size()) {
        QToolTip::hideText();
        return;
    }
    float inSample = vSamplesIn[m_value].at(ttpoint);
    float outSample = vSamplesOut[m_value].at(ttpoint);
    float selectedSample = tt_in_series ? inSample : outSample;
    int y = y_value(selectedSample);
    painter.setPen(Qt::yellow);
    painter.drawEllipse(QPointF(x, y), 3, 3);
    QString strTime;
    std::chrono::milliseconds sampleTime{0};
    if (ttpoint + 1 < vTimeStamp[m_value].size()) sampleTime = vTimeStamp[m_value].at(ttpoint + 1);
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
    }
    QString strData = tr("In") + " " + GUIUtil::formatBytesps(vSamplesIn[m_value].at(ttpoint)*1000) + " " + tr("Out") + " " + GUIUtil::formatBytesps(vSamplesOut[m_value].at(ttpoint)*1000);
    // Line below allows ToolTip to move faster than the default ToolTip timeout (10 seconds).
    QToolTip::showText(QPoint(x + x_offset, y + y_offset), strTime + "\n. " + strData);
    QToolTip::showText(QPoint(x + x_offset, y + y_offset), strTime + "\n  " + strData);
    tt_time = GetTime();
}

void TrafficGraphWidget::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.fillRect(rect(), Qt::black);

    if (fMax <= 0.0f) return;

    QColor axisCol(Qt::gray);
    painter.setPen(axisCol);
    int h = height() - YMARGIN * 2;
    painter.drawLine(XMARGIN, YMARGIN + h, width() - XMARGIN, YMARGIN + h);

    // decide what order of magnitude we are
    int base = floor(log10(fMax));
    float val = pow(10.0f, base);

    const float yMarginText = 2.0;

    // if we drew 10 or 3 fewer lines, break them up at the next lower order of magnitude
    if (fMax / val <= (fToggle ? 10.0f : 3.0f)) {
        float oldval = val;
        val = pow(10.0f, base - 1);
        painter.setPen(axisCol.darker());
        painter.drawText(XMARGIN, y_value(val)-yMarginText, GUIUtil::formatBytesps(val*1000));
        int count = 1;
        for(float y = val; y < (!fToggle || fMax / val < 20 ? fMax : oldval); y += val, count++) {
            if (count % 10 == 0) continue;
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
    for (float y = val; y < fMax; y += val) {
        int yy = y_value(y);
        painter.drawLine(XMARGIN, yy, width() - XMARGIN, yy);
    }
    painter.drawText(XMARGIN, y_value(val)-yMarginText, GUIUtil::formatBytesps(val*1000));

    painter.setRenderHint(QPainter::Antialiasing);
    if (!vSamplesIn[m_value].empty()) {
        QPainterPath p;
        paintPath(p, vSamplesIn[m_value]);
        painter.fillPath(p, QColor(0, 255, 0, 128));
        painter.setPen(Qt::green);
        painter.drawPath(p);
    }
    if (!vSamplesOut[m_value].empty()) {
        QPainterPath p;
        paintPath(p, vSamplesOut[m_value]);
        painter.fillPath(p, QColor(255, 0, 0, 128));
        painter.setPen(Qt::red);
        painter.drawPath(p);
    }
    if (ttpoint >= 0 && ttpoint < vTimeStamp[m_value].size() && isVisible() && !window()->isMinimized())
        drawTooltipPoint(painter);
    else
        QToolTip::hideText();
}

void TrafficGraphWidget::update_fMax() {
    float tmax = 0.0f;
    for (const float f : vSamplesIn[m_new_value]) if (f > tmax) tmax = f;
    for (const float f : vSamplesOut[m_new_value]) if (f > tmax) tmax = f;
    new_fMax = tmax;
    static float last_fMax = -1;
    if (new_fMax != last_fMax) last_fMax = new_fMax;
}

/**
 * Smoothly updates a value with acceleration/deceleration for animation.
 *
 * @param new_val The target value to approach
 * @param current The current value that will be updated
 * @param increment The current rate of change (velocity), updated by this function
 * @param length The scale factor for controlling animation speed
 * @return true if the value was updated, false otherwise
 *
 * This implements a simple physics-based approach to animation:
 * - If moving too slowly, accelerate
 * - If moving too quickly, decelerate
 * - If close enough to target, snap to it
 */
bool update_num(float new_val, float &current, float &increment, int length) {
    if (new_val <= 0 || current == new_val) return false;

    if (abs(increment) <= abs(0.8 * current) / length) { // allow equal to as current and increment could be zero
        if (new_val > current)
            increment = 1.0 * (current+1) / length; // +1s are to get it started even if current is zero
        else
            increment = -1.0 * (current+1) / length;
        if (abs(increment) > abs(new_val - current)) { // Only check this when creating an increment
            increment = 0; // Nothing to do!
            current = new_val;
            return true;
        }
    } else {
        if (((increment > 0) && (current + increment * 2 > new_val)) ||
                ((increment < 0) && (current + increment * 2 < new_val))) {
            increment = increment / 2; // Keep the momentum going even if new_val is elsewhere.
        } else {
            if (((increment > 0) && (current + increment * 8 < new_val)) ||
                    ((increment < 0) && (current + increment * 8 > new_val)))
                increment = increment * 2;
        }
    }
    if (abs(increment) < 0.8 * current / length) {
        if ((increment >= 0 && new_val > current) || (increment <= 0 && new_val < current))
            current = new_val;
        increment = 0;
    } else
        current += increment;
    if (current <= 0.0f) current = 0.0001f;

    return true;
}

void TrafficGraphWidget::updateStuff() {
    if (!clientModel) return;
    uint64_t expected_gap = timer->interval(), now = GetTimeMillis();
    static uint64_t last_jump_time = 0;
    uint64_t m_time_offset = 0;

    if (!vTimeStamp[0].empty()) {
        uint64_t last_time = vTimeStamp[0].front().count();
        uint64_t actual_gap = now - last_time;
        if (actual_gap >= 1000 + expected_gap && last_time != last_jump_time) {
            m_time_offset = actual_gap - expected_gap;
            last_jump_time = last_time;
        }
    }

    bool fUpdate = false;
    for (int i = 0; i < VALUES_SIZE; i++) {
        uint64_t msecs_per_sample = static_cast<uint64_t>(values[i]) * static_cast<uint64_t>(60000) / DESIRED_SAMPLES;
        if (m_time_offset) {
            m_offset[i] += m_time_offset;
            if (m_offset[i] > now - nLastTime[i].count()) m_offset[i] = now - nLastTime[i].count();
        }
        if (now > (nLastTime[i].count() + msecs_per_sample + m_offset[i] - expected_gap/2)) {
            m_offset[i] = 0;
            updateRates(i);
            if (i == m_value) {
                if (ttpoint >= 0 && ttpoint < DESIRED_SAMPLES) {
                    ttpoint++; // Move the selected point to the left
                    if (ttpoint >= DESIRED_SAMPLES) ttpoint = -1;
                }
                fUpdate = true;
            }
            if (i == m_new_value) update_fMax();
        }
    }
    m_time_offset = 0;

    static float y_increment = 0, x_increment = 0;
    if (update_num(new_fMax, fMax, y_increment, height() - YMARGIN * 2)) fUpdate = true;
    int next_m_value = m_value;
    if (update_num(values[m_new_value], m_range, x_increment, width() - XMARGIN * 2)) {
        if (values[m_new_value] > m_range && values[m_value] < m_range)
            next_m_value = m_value + 1;
        else if (m_value > 0 && values[m_new_value] <= m_range && values[m_value-1] > m_range * 0.99)
            next_m_value = m_value - 1;
        fUpdate = true;
    } else if (m_value != m_new_value) {
        next_m_value = m_new_value;
        fUpdate = true;
    }

    if (next_m_value != m_value) {
        if (ttpoint >= 0 && ttpoint < vTimeStamp[m_value].size())
            ttpoint = findClosestPointByTimestamp(m_value, ttpoint, next_m_value);
        else
            ttpoint = -1;
        m_value = next_m_value;
    }

    static bool last_fToggle = fToggle;
    if (!QToolTip::isVisible() || !isVisible() || window()->isMinimized()) {
        if (ttpoint >= 0) { // Remove the yellow circle if the ToolTip has gone due to mouse moving elsewhere.
            if (last_fToggle == fToggle) ttpoint = -1;
            else last_fToggle = fToggle;
            fUpdate = true;
        }
    } else if (ttpoint >= 0 && GetTime() >= tt_time + 9) fUpdate = true;

    if (fUpdate) update();
}

void TrafficGraphWidget::updateRates(int i) {
    std::chrono::milliseconds now{GetTimeMillis()};
    quint64 bytesIn = clientModel->node().getTotalBytesRecv(),
            bytesOut = clientModel->node().getTotalBytesSent();
    float in_rate_kilobytes_per_msec = 0, out_rate_kilobytes_per_msec = 0;
    int64_t actual_gap = (now - nLastTime[i]).count();
    if (actual_gap >= 0) {
        in_rate_kilobytes_per_msec = static_cast<float>(bytesIn - nLastBytesIn[i]) / actual_gap;
        out_rate_kilobytes_per_msec = static_cast<float>(bytesOut - nLastBytesOut[i]) / actual_gap;
    }
    vSamplesIn[i].push_front(in_rate_kilobytes_per_msec);
    vSamplesOut[i].push_front(out_rate_kilobytes_per_msec);
    vTimeStamp[i].push_front(now);
    nLastTime[i] = now;
    nLastBytesIn[i] = bytesIn;
    nLastBytesOut[i] = bytesOut;
    static int8_t fFull[VALUES_SIZE] = {};
    if (fFull[i]<=0 && vTimeStamp[i].size()+5 > DESIRED_SAMPLES)
        fFull[i] = vTimeStamp[i].size() - DESIRED_SAMPLES - 1;
    while (vTimeStamp[i].size() > DESIRED_SAMPLES) {
        if (ttpoint < 0 && m_value == i && i < VALUES_SIZE - 1 && fFull[i]<0)
            m_bump_value = true;
        fFull[i] = 1;
        vSamplesIn[i].pop_back();
        vSamplesOut[i].pop_back();
        vTimeStamp[i].pop_back();
    }
}

std::chrono::minutes TrafficGraphWidget::setGraphRange(unsigned int value) {
    // value is the array marker plus 1 (as zero is reserved for bumping up)
    if (!value) { // bump
        m_bump_value = false;
        value = m_value + 1;
    } else value--; // get the array marker
    int old_value = m_new_value;
    m_new_value = std::min((int)value, VALUES_SIZE - 1);
    if (m_new_value != old_value) {
        update_fMax();
        update();
    }
    setFocus(Qt::OtherFocusReason);
    focusSlider(Qt::OtherFocusReason);

    return std::chrono::minutes{values[m_new_value]};
}

void TrafficGraphWidget::saveData() {
    try {
        fs::path pathTrafficGraph = fs::path(clientModel->dataDir().toStdString().c_str()) / "trafficgraphdata.dat";
        LogPrintf("TrafficGraphWidget: Saving data to %s\n", pathTrafficGraph.u8string());

        FILE* file = fsbridge::fopen(pathTrafficGraph, "wb");
        if (!file) {
            LogPrintf("TrafficGraphWidget: Failed to open file for writing: %s\n", pathTrafficGraph.u8string());
            throw std::runtime_error("Failed to open file");
        }
        CAutoFile fileout(file, SER_DISK, CLIENT_VERSION);
        if (fileout.IsNull()) throw std::runtime_error("File stream is null");
        fileout << static_cast<int>(1); // Version 1

        uint64_t totalBytesRecv = clientModel->node().getTotalBytesRecv();
        uint64_t totalBytesSent = clientModel->node().getTotalBytesSent();
        fileout << VARINT(totalBytesRecv) << VARINT(totalBytesSent);

        for (unsigned int i = 0; i < VALUES_SIZE; i++) {
            // Save the size of these samples
            fileout << VARINT(static_cast<uint32_t>(vTimeStamp[i].size()));

            for (int j = 0; j < vSamplesIn[i].size(); j++) {
                float value = vSamplesIn[i].at(j);
                uint32_t uint_value;
                memcpy(&uint_value, &value, sizeof(float)); // IEEE 754
                ser_writedata32(fileout, uint_value);
            }

            for (int j = 0; j < vSamplesOut[i].size(); j++) {
                float value = vSamplesOut[i].at(j);
                uint32_t uint_value;
                memcpy(&uint_value, &value, sizeof(float)); // IEEE 754
                ser_writedata32(fileout, uint_value);
            }

            for (int j = 0; j < vTimeStamp[i].size(); j++)
                fileout << VARINT(static_cast<uint64_t>(vTimeStamp[i].at(j).count()));

            fileout << VARINT(m_offset[i]);
        }

        fileout.fclose();
    } catch (const std::exception& e) {
        LogPrintf("TrafficGraphWidget: Error saving data: %s (path: %s)\n",
                 e.what(), clientModel->dataDir().toStdString());
    }
}

bool TrafficGraphWidget::loadDataFromBinary() {
    try {
        fs::path pathTrafficGraph = fs::path(clientModel->dataDir().toStdString().c_str()) / "trafficgraphdata.dat";
        LogPrintf("TrafficGraphWidget: Attempting to load data from %s\n", pathTrafficGraph.u8string());

        FILE* file = fsbridge::fopen(pathTrafficGraph, "rb");
        if (!file) {
            LogPrintf("TrafficGraphWidget: File not found or could not be opened\n");
            return false;
        }
        CAutoFile filein(file, SER_DISK, CLIENT_VERSION);
        if (filein.IsNull()) return false;

        int version;
        filein >> version;
        if (version < 1 || version > 3) return false;

        filein >> VARINT(m_totalBytesRecv) >> VARINT(m_totalBytesSent);

        for (unsigned int i = 0; i < VALUES_SIZE; i++) {
            unsigned int samplesSize;
            filein >> VARINT(samplesSize);

            for (unsigned int j = 0; j < samplesSize; j++) {
                uint32_t uint_value = ser_readdata32(filein);
                float value;
                memcpy(&value, &uint_value, sizeof(float));
                vSamplesIn[i].push_back(value);
            }

            for (unsigned int j = 0; j < samplesSize; j++) {
                uint32_t uint_value = ser_readdata32(filein);
                float value;
                memcpy(&value, &uint_value, sizeof(float));
                vSamplesOut[i].push_back(value);
            }

            for (unsigned int j = 0; j < samplesSize; j++) {
                uint64_t timeMs;
                filein >> VARINT(timeMs);
                vTimeStamp[i].push_back(std::chrono::milliseconds{static_cast<int64_t>(timeMs)});
            }

            filein >> VARINT(m_offset[i]);
        }

        filein.fclose();

        return true;
    } catch (const std::exception& e) {
        LogPrintf("TrafficGraphWidget: Error loading data: %s\n", e.what());
        return false;
    }
}

bool TrafficGraphWidget::loadData() {
    bool success = loadDataFromBinary();

    if (!success) return false;

    // If we successfully loaded data, determine the correct band to use
    int firstNonFullBand = VALUES_SIZE - 1;

    for (int i = 0; i < VALUES_SIZE; i++)
        if (vTimeStamp[i].size() < DESIRED_SAMPLES) {
            firstNonFullBand = i;
            break;
        }

    if (firstNonFullBand) { // not the first band
        m_value = firstNonFullBand - 1; // Minus one as we're bumping it
        m_bump_value = true;
    }

    return true;
}

int TrafficGraphWidget::findClosestPointByTimestamp(int sourceRange, int sourcePoint, int targetRange) const {
    if (sourcePoint < 0 || sourcePoint >= vTimeStamp[sourceRange].size() ||
        vTimeStamp[targetRange].empty()) {
        return -1;
    }

    bool isPeak = false, isDip = false;
    float sourceValue = tt_in_series ? vSamplesIn[sourceRange].at(sourcePoint) : vSamplesOut[sourceRange].at(sourcePoint);

    if (sourcePoint > 0 && sourcePoint < vTimeStamp[sourceRange].size() - 1) {
        float prevValue = tt_in_series ? vSamplesIn[sourceRange].at(sourcePoint - 1) : vSamplesOut[sourceRange].at(sourcePoint - 1);
        float nextValue = tt_in_series ? vSamplesIn[sourceRange].at(sourcePoint + 1) : vSamplesOut[sourceRange].at(sourcePoint + 1);

        isPeak = sourceValue > prevValue && sourceValue > nextValue;
        isDip = sourceValue < prevValue && sourceValue < nextValue;
    }

    std::chrono::milliseconds sourceTimestamp = vTimeStamp[sourceRange].at(sourcePoint);
    int closestPoint = -1;
    std::chrono::milliseconds::rep minDifference = std::numeric_limits<std::chrono::milliseconds::rep>::max();

    for (int i = 0; i < vTimeStamp[targetRange].size(); ++i) {
        auto diff = std::abs(vTimeStamp[targetRange].at(i).count() - sourceTimestamp.count());
        if (diff < minDifference) {
            minDifference = diff;
            closestPoint = i;
        }
    }

    if (closestPoint >= 0 && (isPeak || isDip)) {
        float closestValue = tt_in_series ? vSamplesIn[targetRange].at(closestPoint) : vSamplesOut[targetRange].at(closestPoint);
        int bestPoint = closestPoint; float bestValue = closestValue;
        uint64_t avgSampleInterval = (values[targetRange] * 60 * 1000) / DESIRED_SAMPLES;
        uint64_t timeWindow = avgSampleInterval * 3;

        for (int i = 0; i < vTimeStamp[targetRange].size(); ++i) {
            uint64_t timeDiff = static_cast<uint64_t>(std::abs(vTimeStamp[targetRange].at(i).count() - sourceTimestamp.count()));
            if (timeDiff <= timeWindow) {
                float currentValue = tt_in_series ? vSamplesIn[targetRange].at(i) : vSamplesOut[targetRange].at(i);

                if (isPeak && currentValue > bestValue) {
                    bestPoint = i;
                    bestValue = currentValue;
                } else if (isDip && currentValue < bestValue) {
                    bestPoint = i;
                    bestValue = currentValue;
                }
            }
        }
        closestPoint = bestPoint;
    }

    return closestPoint;
}
