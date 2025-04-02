// Copyright (c) 2011-present The Bitcoin Core developers
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
#include <QToolTip>
#include <chrono>
#include <cmath>

#define DESIRED_SAMPLES         800

#define XMARGIN                 10
#define YMARGIN                 10

TrafficGraphWidget::TrafficGraphWidget(QWidget* parent)
    : QWidget(parent)
{
    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, &TrafficGraphWidget::updateStuff);
    m_timer->setInterval(75);
    m_timer->start();
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus); // Make widget focusable to respond to keyboard events
}

void TrafficGraphWidget::setClientModel(ClientModel *model)
{
    m_client_model = model;
    if(model) {
        m_data_dir = model->dataDir().toStdString();
        m_node = &model->node();  // Cache the node interface

        if (m_samples_in[0].empty() && m_samples_out[0].empty()) {
            loadData();
        }
    } else {
        // Save data when model is being disconnected during shutdown
        saveData();
    }
}

int TrafficGraphWidget::y_value(float value) const
{
    int h = height() - YMARGIN * 2;
    return YMARGIN + h - (h * 1.0 * (m_toggle ? (std::pow(value, 0.30102) / std::pow(m_fmax, 0.30102)) : (value / m_fmax)));
}

void TrafficGraphWidget::paintPath(QPainterPath& path, const QQueue<float>& samples)
{
    int sampleCount = std::min(int(DESIRED_SAMPLES * m_range / m_values[m_value]), int(samples.size()));
    if (sampleCount <= 0) return;
    int h = height() - YMARGIN * 2, w = width() - XMARGIN * 2;
    int x = XMARGIN + w;
    path.moveTo(x, YMARGIN + h);
    for (int i = 0; i < sampleCount; ++i) {
        double ratio = static_cast<double>(i) * m_values[m_value] / m_range / DESIRED_SAMPLES;
        x = XMARGIN + w - static_cast<int>(w * ratio);
        path.lineTo(x, y_value(samples.at(i)));
    }
    path.lineTo(x, YMARGIN + h);
}

void TrafficGraphWidget::focusSlider(Qt::FocusReason reason)
{
    QWidget* parent = parentWidget();
    if (parent) {
        QSlider* slider = parent->findChild<QSlider*>("sldGraphRange");
        if (slider) slider->setFocus(reason);
    }
}

/*void TrafficGraphWidget::focusSlider(Qt::FocusReason reason)
{
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
}*/

void TrafficGraphWidget::mousePressEvent(QMouseEvent* event)
{
    QWidget::mousePressEvent(event);
    m_toggle = !m_toggle;
    m_update = true;
    update();
}

void TrafficGraphWidget::mouseMoveEvent(QMouseEvent* event)
{
    QWidget::mouseMoveEvent(event);
    static int last_x = -1, last_y = -1;
    QPointF pos = event->position();
    QPointF globalPos = event->globalPosition();
    int x = qRound(pos.x()), y = qRound(pos.y());
    m_x_offset = qRound(globalPos.x()) - x;
    m_y_offset = qRound(globalPos.y()) - y;
    if (last_x == x && last_y == y) return; // Do nothing if mouse hasn't moved
    int h = height() - YMARGIN * 2, w = width() - XMARGIN * 2;
    int i = (w + XMARGIN - x) * DESIRED_SAMPLES / w, closest_i = -1;
    int sampleSize = m_time_stamp[m_value].size();
    unsigned int smallest_distance = 50;
    bool is_in_series = true;
    if (sampleSize && i >= -10 && i < sampleSize + 2 && y <= h + YMARGIN + 3) {
        for (int test_i = std::max(i - 2, 0); test_i < std::min(i + 10, sampleSize); test_i++) {
            float in_val = m_samples_in[m_value].at(test_i), out_val = m_samples_out[m_value].at(test_i);
            int y_in = y_value(in_val), y_out = y_value(out_val);
            unsigned int distance_in = abs(y - y_in), distance_out = abs(y - y_out);
            unsigned int min_distance = std::min(distance_in, distance_out);
            if (min_distance < smallest_distance) {
                smallest_distance = min_distance;
                closest_i = test_i;
                is_in_series = (distance_in <= distance_out);
            }
        }
    }
    if (m_tt_point != closest_i || m_tt_in_series != is_in_series) {
        m_tt_point = closest_i;
        m_tt_in_series = is_in_series;
        m_update = true;
        update(); // Calls paintEvent() to draw or delete the highlighted point
    }
    last_x = x;
    last_y = y;
}

void TrafficGraphWidget::drawTooltipPoint(QPainter& painter)
{
    int w = width() - XMARGIN * 2;
    double ratio = static_cast<double>(m_tt_point) * m_values[m_value] / m_range / DESIRED_SAMPLES;
    int x = XMARGIN + w - static_cast<int>(w * ratio);
    float inSample = m_samples_in[m_value].at(m_tt_point);
    float outSample = m_samples_out[m_value].at(m_tt_point);
    float selectedSample = m_tt_in_series ? inSample : outSample;
    int y = y_value(selectedSample);
    painter.setPen(Qt::yellow);
    painter.drawEllipse(QPointF(x, y), 3, 3);
    QString strTime;
    int64_t sampleTime;
    if (m_tt_point + 1 < m_time_stamp[m_value].size()) {
        sampleTime = m_time_stamp[m_value].at(m_tt_point + 1);
    } else {
        strTime = "to ";
        sampleTime = m_time_stamp[m_value].at(m_tt_point);
    }
    int age = GetTime() - sampleTime / 1000;
    if (age < 60 * 60 * 23)
        strTime += QString::fromStdString(FormatISO8601Time(sampleTime / 1000));
    else
        strTime += QString::fromStdString(FormatISO8601DateTime(sampleTime / 1000));
    int nDuration = (m_time_stamp[m_value].at(m_tt_point) - sampleTime);
    if (nDuration > 0) {
        if (nDuration > 9999)
            strTime += " +" + GUIUtil::formatDurationStr(std::chrono::seconds{(nDuration + 500) / 1000});
        else
            strTime += " +" + GUIUtil::formatPingTime(std::chrono::microseconds{nDuration * 1000});
    }
    QString strData = tr("In") + " " + GUIUtil::formatBytesps(m_samples_in[m_value].at(m_tt_point) * 1000) + " " + tr("Out") + " " + GUIUtil::formatBytesps(m_samples_out[m_value].at(m_tt_point) * 1000);
    // Line below allows ToolTip to move faster than the default ToolTip timeout (10 seconds).
    QToolTip::showText(QPoint(x + m_x_offset, y + m_y_offset), strTime + "\n. " + strData);
    QToolTip::showText(QPoint(x + m_x_offset, y + m_y_offset), strTime + "\n  " + strData);
    m_tt_time = GetTime();
}

void TrafficGraphWidget::paintEvent(QPaintEvent *)
{
    m_update = false;
    QPainter painter(this);
    painter.fillRect(rect(), Qt::black);

    if (m_fmax < 0.0001f) return;

    QColor axisCol(Qt::gray);
    int h = height() - YMARGIN * 2;
    painter.setPen(axisCol);
    painter.drawLine(XMARGIN, YMARGIN + h, width() - XMARGIN, YMARGIN + h);

    // decide what order of magnitude we are
    int base = std::floor(std::log10(m_fmax));
    float val = std::pow(10.0f, base); // kB/s

    const float yMarginText = 2.0;

    // draw lines
    painter.setPen(axisCol);
    for(float y = val; y < m_fmax; y += val) {
        int yy = y_value(y);
        painter.drawLine(XMARGIN, yy, width() - XMARGIN, yy);
    }
    painter.drawText(XMARGIN, y_value(val) - yMarginText, GUIUtil::formatBytesps(val * 1000));

    // if we drew 10 or 3 fewer lines, break them up at the next lower order of magnitude
    if (m_fmax / val <= (m_toggle ? 10.0f : 3.0f)) {
        val = std::pow(10.0f, base - 1);
        painter.setPen(axisCol.darker());
        painter.drawText(XMARGIN, y_value(val) - yMarginText, GUIUtil::formatBytesps(val * 1000));
        int count = 1;
        for (float y = val; y < (!m_toggle || m_fmax / val < 20 ? m_fmax : val*10); y += val, count++) {
            // don't overwrite lines drawn above
            if (count % 10 == 0) continue;
            int yy = y_value(y);
            painter.drawLine(XMARGIN, yy, width() - XMARGIN, yy);
        }
        if (m_toggle) {
            int yy = y_value(val * 0.1);
            painter.setPen(axisCol.darker().darker());
            painter.drawText(XMARGIN, yy - yMarginText, GUIUtil::formatBytesps(val*100));
            painter.drawLine(XMARGIN, yy, width() - XMARGIN, yy);
        }
    }

    painter.setRenderHint(QPainter::Antialiasing);
    if (!m_samples_in[m_value].empty()) {
        QPainterPath p;
        paintPath(p, m_samples_in[m_value]);
        painter.fillPath(p, QColor(0, 255, 0, 128));
        painter.setPen(Qt::green);
        painter.drawPath(p);
    }
    if (!m_samples_out[m_value].empty()) {
        QPainterPath p;
        paintPath(p, m_samples_out[m_value]);
        painter.fillPath(p, QColor(255, 0, 0, 128));
        painter.setPen(Qt::red);
        painter.drawPath(p);
    }
    if (m_tt_point >= 0 && m_tt_point < m_time_stamp[m_value].size() && isVisible() && !window()->isMinimized())
        drawTooltipPoint(painter);
    else QToolTip::hideText();
}

void TrafficGraphWidget::update_fmax()
{
    float tmax = 0.0f;
    for (const float f : m_samples_in[m_new_value])
        if (f > tmax) tmax = f;
    for (const float f : m_samples_out[m_new_value])
        if (f > tmax) tmax = f;
    m_new_fmax = tmax;
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
bool update_num(float new_val, float& current, float& increment, int length)
{
    if (new_val <= 0 || current == new_val) return false;

    if (abs(increment) <= abs(0.8 * current) / length) { // allow equal to as current and increment could be zero
        if (new_val > current)
            increment = 1.0 * (current + 1) / length; // +1s are to get it started even if current is zero
        else
            increment = -1.0 * (current + 1) / length;
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
        if ((increment >= 0 && new_val > current) || (increment <= 0 && new_val < current)) {
            current = new_val;
            increment = 0;
        }
    } else
        current += increment;
    if (current <= 0.0f) current = 0.0001f;

    return true;
}

void TrafficGraphWidget::updateStuff()
{
    if (!m_client_model) return;
    int64_t expected_gap = m_timer->interval();
    int64_t now = GetTime<std::chrono::milliseconds>().count();
    static int64_t last_jump_time = 0;
    int64_t time_offset = 0;

    if (!m_time_stamp[0].empty()) {
        int64_t last_time = m_time_stamp[0].front();
        int64_t actual_gap = now - last_time;
        if (actual_gap >= 1000 + expected_gap && last_time != last_jump_time) {
            time_offset = actual_gap - expected_gap;
            last_jump_time = last_time;
        }
    }

    // Check for new sample and update display if a new sample taken for current range
    for (int i = 0; i < VALUES_SIZE; i++) {
        int64_t msecs_per_sample = static_cast<int64_t>(m_values[i]) * 60000 / DESIRED_SAMPLES;
        if (time_offset) {
            m_offset[i] += time_offset;
            if (m_offset[i] > now - m_last_time[i]) m_offset[i] = now - m_last_time[i];
        }
        if (now > (m_last_time[i] + msecs_per_sample + m_offset[i] - expected_gap / 2)) {
            m_offset[i] = 0;
            updateRates(i);
            if (i == m_value) {
                if (m_tt_point >= 0 && m_tt_point < DESIRED_SAMPLES) {
                    m_tt_point++; // Move the selected point to the left
                    if (m_tt_point >= DESIRED_SAMPLES) m_tt_point = -1;
                }
                m_update = true;
            }
            if (i == m_new_value) update_fmax();
        }
    }
    time_offset = 0;

    // Update display due to transtion between ranges or new fmax
    static float y_increment = 0, x_increment = 0;
    if (update_num(m_new_fmax, m_fmax, y_increment, height() - YMARGIN * 2)) m_update = true;
    int next_m_value = m_value;
    if (update_num(m_values[m_new_value], m_range, x_increment, width() - XMARGIN * 2)) {
        if (m_values[m_new_value] > m_range && m_values[m_value] < m_range) {
            next_m_value = m_value + 1;
        } else if (m_value > 0 && m_values[m_new_value] <= m_range && m_values[m_value - 1] > m_range * 0.99)
            next_m_value = m_value - 1;
        m_update = true;
    } else if (m_value != m_new_value) {
        next_m_value = m_new_value;
        m_update = true;
    }

    if (next_m_value != m_value) {
        if (m_tt_point >= 0 && m_tt_point < m_time_stamp[m_value].size()) {
            m_tt_point = findClosestPointByTimestamp(m_value, m_tt_point, next_m_value);
        } else
            m_tt_point = -1;
        m_value = next_m_value;
    }

    static bool last_m_toggle = m_toggle;
    if (!QToolTip::isVisible()) {
        if (m_tt_point >= 0) { // Remove the yellow circle if the ToolTip has gone due to mouse moving elsewhere.
            if (last_m_toggle == m_toggle) {
                m_tt_point = -1;
            } else
                last_m_toggle = m_toggle;
            m_update = true;
        }
    } else if (m_tt_point >= 0 && GetTime() >= m_tt_time + 9) m_update = true;

    if (m_update) update();
    static bool graph_visible = false;
    if (isVisible() && !window()->isMinimized()) {
        if (!graph_visible) focusSlider(Qt::OtherFocusReason);
        graph_visible = true;
    } else graph_visible = false;
}

void TrafficGraphWidget::updateRates(int i)
{
    int64_t now = GetTime<std::chrono::milliseconds>().count();
    int64_t actual_gap = now - m_last_time[i];
    quint64 bytesIn = m_client_model->node().getTotalBytesRecv() + m_baseline_bytes_recv,
            bytesOut = m_client_model->node().getTotalBytesSent() + m_baseline_bytes_sent;
    float in_rate_kilobytes_per_msec = static_cast<float>(bytesIn - m_last_bytes_in[i]) / actual_gap;
    float out_rate_kilobytes_per_msec = static_cast<float>(bytesOut - m_last_bytes_out[i]) / actual_gap;
    m_samples_in[i].push_front(in_rate_kilobytes_per_msec);
    m_samples_out[i].push_front(out_rate_kilobytes_per_msec);
    m_time_stamp[i].push_front(now);
    m_last_time[i] = now;
    m_last_bytes_in[i] = bytesIn;
    m_last_bytes_out[i] = bytesOut;
    static int8_t fFull[VALUES_SIZE] = {};
    if (fFull[i] == 0 && m_time_stamp[i].size() <= DESIRED_SAMPLES)
        fFull[i] = -1;
    while (m_time_stamp[i].size() > DESIRED_SAMPLES) {
        if (m_tt_point < 0 && m_value == i && i < VALUES_SIZE - 1 && fFull[i] < 0)
            m_bump_value = true;
        fFull[i] = 1;
        m_samples_in[i].pop_back();
        m_samples_out[i].pop_back();
        m_time_stamp[i].pop_back();
    }
}

int TrafficGraphWidget::setGraphRange(int value)
{
    // value is the array marker plus 1 (as zero is reserved for bumping up)
    if (!value) { // bump
        m_bump_value = false;
        value = m_value + 1;
    } else
        value--; // get the array marker
    int old_value = m_new_value;
    m_new_value = std::min(value, VALUES_SIZE - 1);
    if (m_new_value != old_value) update_fmax();

    return m_values[m_new_value];
}

void TrafficGraphWidget::saveData()
{
    try {
        fs::path pathTrafficGraph = fs::path(m_data_dir.c_str()) / "trafficgraph.dat";
        FILE* file = fsbridge::fopen(pathTrafficGraph, "wb");
        if (!file) {
            LogPrintf("TrafficGraphWidget: Failed to open file for writing: %s\n", pathTrafficGraph.generic_string());
            throw std::runtime_error("Failed to open file");
        }
        AutoFile fileout(file);
        if (fileout.IsNull()) throw std::runtime_error("File stream is null");
        fileout << static_cast<uint32_t>(1); // Version 1

        // Get current node values and add them to our baseline
        if (m_node) {
            m_baseline_bytes_recv += m_node->getTotalBytesRecv();
            m_baseline_bytes_sent += m_node->getTotalBytesSent();
        }

        fileout << VARINT(m_baseline_bytes_recv) << VARINT(m_baseline_bytes_sent);

        for (unsigned int i = 0; i < VALUES_SIZE; i++) {
            // Save the size of these samples
            fileout << VARINT(static_cast<uint32_t>(m_time_stamp[i].size()));

            for (int j = 0; j < m_time_stamp[i].size(); j++) {
                int64_t timestamp = m_time_stamp[i].at(j);
                fileout << static_cast<uint64_t>(timestamp);
            }

            for (int j = 0; j < m_samples_in[i].size(); j++) {
                float value = m_samples_in[i].at(j);
                uint32_t uint_value;
                memcpy(&uint_value, &value, sizeof(float)); // IEEE 754
                fileout << uint_value;
            }

            for (int j = 0; j < m_samples_out[i].size(); j++) {
                float value = m_samples_out[i].at(j);
                uint32_t uint_value;
                memcpy(&uint_value, &value, sizeof(float)); // IEEE 754
                fileout << uint_value;
            }

            fileout << VARINT(static_cast<uint64_t>(m_offset[i]));
        }

        if (fileout.fclose() != 0) {
            throw std::runtime_error("Failed to close traffic graph data file");
        }
        LogPrintf("TrafficGraphWidget: Successfully saved traffic graph data to %s\n", pathTrafficGraph.generic_string());
    } catch (const std::exception& e) {
        LogPrintf("TrafficGraphWidget: Error saving data: %s (path: %s)\n",
                 e.what(), m_data_dir);
    }
}

bool TrafficGraphWidget::loadDataFromBinary()
{
    try {
        fs::path pathTrafficGraph = fs::path(m_data_dir.c_str()) / "trafficgraph.dat";
        LogPrintf("TrafficGraphWidget: Attempting to load data from %s\n", pathTrafficGraph.generic_string());

        FILE* file = fsbridge::fopen(pathTrafficGraph, "rb");
        if (!file) {
            LogPrintf("TrafficGraphWidget: File not found or could not be opened\n");
            return false;
        }
        AutoFile filein(file);
        if (filein.IsNull()) {
            return false;
        }

        int version;
        filein >> version;
        if (version < 1 || version > 1) {
            return false;
        }

        filein >> VARINT(m_baseline_bytes_recv) >> VARINT(m_baseline_bytes_sent);

        for (unsigned int i = 0; i < VALUES_SIZE; i++) {
            uint32_t samplesSize;
            filein >> VARINT(samplesSize);

            static uint64_t last_time_ms;

            for (unsigned int j = 0; j < samplesSize; j++) {
                uint64_t time_ms;
                filein >> time_ms;
                if (!j) last_time_ms = time_ms;
                if (time_ms > last_time_ms) {
                    return false;
                }
                m_time_stamp[i].push_back(static_cast<int64_t>(time_ms));
                last_time_ms = time_ms;
            }

            for (unsigned int j = 0; j < samplesSize; j++) {
                uint32_t uint_value;
                filein >> uint_value;
                float value;
                memcpy(&value, &uint_value, sizeof(float));
                m_samples_in[i].push_back(value);
            }

            for (unsigned int j = 0; j < samplesSize; j++) {
                uint32_t uint_value;
                filein >> uint_value;
                float value;
                memcpy(&value, &uint_value, sizeof(float));
                m_samples_out[i].push_back(value);
            }

            uint64_t offset;
            filein >> VARINT(offset);
            m_offset[i] = static_cast<int64_t>(offset);
        }

        if (filein.fclose() != 0) {
            return false;
        }

        return true;
    } catch (const std::exception& e) {
        LogPrintf("TrafficGraphWidget: Error loading data: %s\n", e.what());
        return false;
    }
}

bool TrafficGraphWidget::loadData()
{
    bool success = loadDataFromBinary();

    if (!success) { // Zero the values
        LogPrintf("TrafficGraphWidget: Saved traffic data was invalid.\n");
        m_baseline_bytes_recv = m_baseline_bytes_sent = 0;
        for (int i = 0; i < VALUES_SIZE; i++) {
            m_samples_in[i].clear();
            m_samples_out[i].clear();
            m_time_stamp[i].clear();
        }
        return false;
    }

    // If we successfully loaded data, determine the correct band to use
    int firstNonFullBand = VALUES_SIZE - 1;

    for (int i = 0; i < VALUES_SIZE; i++) {
        if (m_time_stamp[i].size() < DESIRED_SAMPLES) {
            firstNonFullBand = i;
            break;
        }
    }

    if (firstNonFullBand) { // not the first band
        m_value = firstNonFullBand - 1; // Minus one as we're bumping it
        m_bump_value = true;
    }

    return true;
}

int TrafficGraphWidget::findClosestPointByTimestamp(int src_range, int src_point, int dst_range) const
{
    if (src_point < 0 || src_point >= m_time_stamp[src_range].size() ||
        m_time_stamp[dst_range].empty()) {
        return -1;
    }

    bool is_peak = false, is_dip = false;
    float src_value = m_tt_in_series ? m_samples_in[src_range].at(src_point) :
                m_samples_out[src_range].at(src_point);
    int64_t src_timestamp = m_time_stamp[src_range].at(src_point);

    if (src_point > 0 && src_point < m_time_stamp[src_range].size() - 1) {
        float prev_value = m_tt_in_series ? m_samples_in[src_range].at(src_point - 1) :
                    m_samples_out[src_range].at(src_point - 1);
        float next_value = m_tt_in_series ? m_samples_in[src_range].at(src_point + 1) :
                    m_samples_out[src_range].at(src_point + 1);

        is_peak = src_value > prev_value && src_value > next_value;
        is_dip = src_value < prev_value && src_value < next_value;
    }

    int dst_point = -1;
    int64_t min_difference = std::numeric_limits<int64_t>::max();

    // Find the nearest point timestamp-wise
    for (int i = 0; i < m_time_stamp[dst_range].size(); ++i) {
        auto diff = std::abs(m_time_stamp[dst_range].at(i) - src_timestamp);
        if (diff < min_difference) {
            min_difference = diff;
            dst_point = i;
        }
    }

    // Exit early if no point found or not a peak nor a dip
    if (dst_point < 0 || (!is_peak && !is_dip)) return dst_point;

    // If a peak/dip, snap to the nearest peak/dip
    float dst_value = m_tt_in_series ? m_samples_in[dst_range].at(dst_point) :
                m_samples_out[dst_range].at(dst_point);
    float best_value = dst_value;
    int best_point = dst_point;
    uint64_t avg_sample_interval = (m_values[dst_range] * 60 * 1000) / DESIRED_SAMPLES;
    int64_t time_window = avg_sample_interval * 3; // Stay within sample interval * 3

    for (int i = best_point - 3; i <= best_point + 3; ++i) {
        if (i < 0 || i >= m_time_stamp[dst_range].size()) continue;
        if (std::abs(m_time_stamp[dst_range].at(i) - src_timestamp) > time_window) continue;
        float value = m_tt_in_series ? m_samples_in[dst_range].at(i) : m_samples_out[dst_range].at(i);
        if (is_peak && value > best_value) {
            dst_point = i;
            best_value = value;
        } else if (is_dip && value < best_value) {
            dst_point = i;
            best_value = value;
        }
    }

    return dst_point;
}
