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
}

void TrafficGraphWidget::setClientModel(ClientModel *model) {
    if (model) {
        clientModel = model;
        m_dataDir = model->dataDir();
        if (vSamplesIn[0].empty() && vSamplesOut[0].empty()) {
            // Load saved traffic data if available and the arrays are empty
            LogPrintf("vSamplesIn[0].empty()=%d\n", vSamplesIn[0].empty());
            loadData();
            return;
        }

        uint64_t nTime = GetTimeMillis();
        for (int i = 0; i < VALUES_SIZE; i++) {
            nLastBytesIn[i] = model->node().getTotalBytesRecv();
            nLastBytesOut[i] = model->node().getTotalBytesSent();
            nLastTime[i] = std::chrono::milliseconds{nTime};
            vSamplesIn[i].push_front(nLastBytesIn[i]);
            vSamplesOut[i].push_front(nLastBytesOut[i]);
            vTimeStamp[i].push_front(nLastTime[i]);
        }
    } else {
        LogPrintf("%s: Saving data\n", __func__);
        saveData(); // TODO might not need to cache the data_dir now that clientmodel moved to below
        clientModel = model;
    }
}

bool TrafficGraphWidget::GraphRangeBump() const { return m_bump_value; }

unsigned int TrafficGraphWidget::getCurrentRangeIndex() const {
    return m_new_value;
}

int TrafficGraphWidget::y_value(float value) {
    int h = height() - YMARGIN * 2;

    if (fMax <= 0.0001f || value <= std::numeric_limits<float>::epsilon())
        return YMARGIN + h;

    float result = fToggle ? pow(value, 0.30102) / pow(fMax, 0.30102) : value / fMax;

    if (std::isnan(result) || std::isinf(result))
        return YMARGIN + h;

    return YMARGIN + h - (h * 1.0 * result);
}

void TrafficGraphWidget::paintPath(QPainterPath &path, QQueue<float> &samples) {
    int sampleCount = std::min(int(DESIRED_SAMPLES * m_range / values[m_value]), int(samples.size()));
    if (sampleCount <= 0) return;
    int h = height() - YMARGIN * 2, w = width() - XMARGIN * 2;
    int x = XMARGIN + w;
    path.moveTo(x, YMARGIN + h);
    for(int i = 0; i < sampleCount; ++i) {
        double ratio = static_cast<double>(i) * values[m_value] / m_range / DESIRED_SAMPLES;
        x = XMARGIN + w - static_cast<int>(w * ratio);
        int y = y_value(samples.at(i));
        path.lineTo(x, y);
    }
    path.lineTo(x, YMARGIN + h);
}

float floatmax(float a, float b) {
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

    if (fMax <= 0.0f) return;

    QColor axisCol(Qt::gray);
    painter.setPen(axisCol);
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
    int sampleCount = vTimeStamp[m_value].size();
    if (ttpoint >= 0 && ttpoint < sampleCount) {
        painter.setPen(Qt::yellow);
        int w = width() - XMARGIN * 2;
        double ratio = static_cast<double>(ttpoint) * values[m_value] / m_range / DESIRED_SAMPLES;
        int x = XMARGIN + w - static_cast<int>(w * ratio);
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

void TrafficGraphWidget::update_fMax() {
    float tmax = 0.0f;
    for (const float f : vSamplesIn[m_new_value]) if (f > tmax) tmax = f;
    for (const float f : vSamplesOut[m_new_value]) if (f > tmax) tmax = f;
    new_fMax = tmax;
    static float last_fMax = -1;
    if (new_fMax != last_fMax) {
        LogPrintf("%s: i=%d new_fMax = %d -> %d\n", __func__, m_new_value, last_fMax, new_fMax);
        last_fMax = new_fMax;
    }
}

bool update_num(float new_val, float &current, float &increment, int length) {
    if (new_val == 0 || current == new_val) return false;

    if (abs(increment) <= abs(0.8 * current) / length) { // allow equal to as current and increment could be zero
        int old_increment = increment;
        if (new_val > current)
            increment = 1.0 * (current+1) / length; // +1s are to get it started even if current is zero
        else
            increment = -1.0 * (current+1) / length;
        if (abs(increment) > abs(new_val - current)) { // Only check this when creating an increment
            increment = 0; // Nothing to do!
            current = new_val;
            return true;
        }
        LogPrintf("%s: new increment: %d+1 / %d = %d->%d\n", __func__, current, length, old_increment, increment);
    } else {
        if (((increment > 0) && (current + increment * 2 > new_val)) ||
                ((increment < 0) && (current + increment * 2 < new_val))) {
            increment = increment / 2; // Keep the momentum going even if new_val is elsewhere.
        } else
            if (((increment > 0) && (current + increment * 8 < new_val)) ||
                    ((increment < 0) && (current + increment * 8 > new_val)))
                increment = increment * 2;
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

void TrafficGraphWidget::updateStuff() {
    if(!clientModel) return;

    static int nInterval{timer->interval()};
    uint64_t nTime{GetTimeMillis()};

    bool fUpdate = false;
    for (int i = 0; i < VALUES_SIZE; i++) {
        uint64_t msecs_per_sample = static_cast<uint64_t>(values[i]) * static_cast<uint64_t>(60000) / DESIRED_SAMPLES;
        if (nTime > (nLastTime[i].count() + msecs_per_sample - nInterval/2)) {
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

    static float y_increment = 0, x_increment = 0;
    if (update_num(new_fMax, fMax, y_increment, height() - YMARGIN * 2)) fUpdate = true;
    if (update_num(values[m_new_value], m_range, x_increment, width() - XMARGIN * 2)) {
        if (values[m_new_value] > m_range && values[m_value] < m_range) {
            LogPrintf("%s: m_value %d->%d m_range %d->%d cur_range=%d\n", __func__, m_value, m_value+1,
                        values[m_value], values[m_value+1], m_range);
            m_value++; ttpoint = -1; // TODO - move the tooltip to where the corresponding data point would be
        } else if (m_value > 0 && values[m_new_value] <= m_range && values[m_value-1] > m_range * 0.99) {
            LogPrintf("%s: m_value %d->%d m_range %d->%d cur_range=%d\n", __func__, m_value, m_value-1,
                        values[m_value], values[m_value-1], m_range);
            m_value--; ttpoint = -1; // TODO - move the tooltip to where the corresponding data point would be
        }
        fUpdate = true;
        //LogPrintf("%s: new_range=%d range=%d new_val=%d val=%d increment=%d\n", __func__, values[m_new_value], m_range, m_new_value, m_value, x_increment);
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
        fUpdate = true; // TODO - make fUpdate non-boolean so we can have a partial-update flag just for refreshing the tooltip only.
    }

    if (fUpdate) update();
}

void TrafficGraphWidget::updateRates(int i) {
    std::chrono::milliseconds nTime{GetTimeMillis()};
    quint64 bytesIn = clientModel->node().getTotalBytesRecv(),
            bytesOut = clientModel->node().getTotalBytesSent();
    int64_t nRealInterval = (nTime - nLastTime[i]).count();
    static int nDebugI = 0;
    if (i > nDebugI) nDebugI = i;
    float in_rate_kilobytes_per_msec = 0, out_rate_kilobytes_per_msec = 0;
    if (nRealInterval >= 0) {
        if (nDebugI == i)
            LogPrintf("%s: i=%d mins=%d nRI=%d\n", __func__, i, values[i], nRealInterval);

        in_rate_kilobytes_per_msec = static_cast<float>(bytesIn - nLastBytesIn[i]) / nRealInterval;
        out_rate_kilobytes_per_msec = static_cast<float>(bytesOut - nLastBytesOut[i]) / nRealInterval;
    }
    vSamplesIn[i].push_front(in_rate_kilobytes_per_msec);
    vSamplesOut[i].push_front(out_rate_kilobytes_per_msec);
    vTimeStamp[i].push_front(nTime);
    nLastTime[i] = nTime;
    nLastBytesIn[i] = bytesIn;
    nLastBytesOut[i] = bytesOut;
    static int8_t fFull[VALUES_SIZE] = {};
    if (fFull[i]<=0 && vTimeStamp[i].size()+5 > DESIRED_SAMPLES) {
        if (fFull[i]<0)
            LogPrintf("%s: fFull[%d] %d steps from full\n", __func__, i, DESIRED_SAMPLES+1 - vTimeStamp[i].size());
        fFull[i] = vTimeStamp[i].size() - DESIRED_SAMPLES - 1;
    }
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

    return std::chrono::minutes{values[m_new_value]};
}

void TrafficGraphWidget::saveData() {
    LogPrintf("TrafficGraphWidget: saveData() called\n");

    try {
        fs::path pathTrafficGraph = fs::path((m_dataDir).toStdString().c_str()) / "trafficgraphdata";
        LogPrintf("TrafficGraphWidget: Trying to save data to %s\n", fs::PathToString(pathTrafficGraph));
        FILE* file = fsbridge::fopen(pathTrafficGraph, "wb");
        if (file) {
            CAutoFile fileout(file, SER_DISK, CLIENT_VERSION);
            if (!fileout.IsNull()) {
                // Version
                fileout << static_cast<int>(1);

                // Save vSamplesIn, vSamplesOut, and vTimeStamp arrays
                for (unsigned int i = 0; i < VALUES_SIZE; i++) {
                    // Save size of each queue
                    unsigned int samplesInSize = vSamplesIn[i].size();
                    fileout << VARINT(static_cast<uint32_t>(samplesInSize));

                    // Save queue contents - convert float to uint32_t for serialization
                    for (unsigned int j = 0; j < samplesInSize; j++) {
                        float value = vSamplesIn[i].at(j);
                        uint32_t uint_value;
                        // Use memcpy for bit-exact conversion (safe on any system with IEEE 754 floats)
                        memcpy(&uint_value, &value, sizeof(float));
                        ser_writedata32(fileout, uint_value);
                    }

                    unsigned int samplesOutSize = vSamplesOut[i].size();
                    fileout << VARINT(static_cast<uint32_t>(samplesOutSize));

                    for (unsigned int j = 0; j < samplesOutSize; j++) {
                        float value = vSamplesOut[i].at(j);
                        uint32_t uint_value;
                        // Use memcpy for bit-exact conversion (safe on any system with IEEE 754 floats)
                        memcpy(&uint_value, &value, sizeof(float));
                        ser_writedata32(fileout, uint_value);
                    }

                    unsigned int timeStampSize = vTimeStamp[i].size();
                    fileout << VARINT(static_cast<uint32_t>(timeStampSize));

                    for (unsigned int j = 0; j < timeStampSize; j++)
                        fileout << VARINT(static_cast<uint64_t>(vTimeStamp[i].at(j).count()));
                }

                fileout.fclose();
                LogPrintf("TrafficGraphWidget: Data saved to %s\n", fs::PathToString(pathTrafficGraph));
            }
        }
    } catch (const std::exception& e) {
        LogPrintf("TrafficGraphWidget: Error saving data: %s\n", e.what());
    }
}

bool TrafficGraphWidget::loadDataFromBinary() {
    LogPrintf("TrafficGraphWidget: Attempting to load binary data file\n");
    try {
        fs::path pathTrafficGraph = fs::path((m_dataDir).toStdString().c_str()) / "trafficgraphdata";
        FILE* file = fsbridge::fopen(pathTrafficGraph, "rb");

        if (!file) {
            LogPrintf("TrafficGraphWidget: Binary data file not found, attempting to load from CSV\n");
            return loadDataFromCSV();
        } else
            LogPrintf("TrafficGraphWidget: Binary data file found, attempting to load from it\n");

        CAutoFile filein(file, SER_DISK, CLIENT_VERSION);
        if (filein.IsNull()) return false;

        // Read version
        int version;
        filein >> version;
        if (version != 1) return false;

        // Load vSamplesIn, vSamplesOut, and vTimeStamp arrays
        for (unsigned int i = 0; i < VALUES_SIZE; i++) {
            // Load vSamplesIn
            unsigned int samplesInSize;
            filein >> VARINT(samplesInSize);

            for (unsigned int j = 0; j < samplesInSize; j++) {
                uint32_t uint_value = ser_readdata32(filein);
                float value;
                // Use memcpy for bit-exact conversion back to float
                memcpy(&value, &uint_value, sizeof(float));
                vSamplesIn[i].push_back(value);
            }

            // Load vSamplesOut
            unsigned int samplesOutSize;
            filein >> VARINT(samplesOutSize);
            for (unsigned int j = 0; j < samplesOutSize; j++) {
                uint32_t uint_value = ser_readdata32(filein);
                float value;
                // Use memcpy for bit-exact conversion back to float
                memcpy(&value, &uint_value, sizeof(float));
                vSamplesOut[i].push_back(value);
            }

            // Load vTimeStamp
            unsigned int timeStampSize;
            filein >> VARINT(timeStampSize);

            for (unsigned int j = 0; j < timeStampSize; j++) {
                uint64_t timeMs;
                filein >> VARINT(timeMs);
                vTimeStamp[i].push_back(std::chrono::milliseconds{static_cast<int64_t>(timeMs)});
            }
        }

        filein.fclose();
        LogPrintf("TrafficGraphWidget: Data loaded from %s\n", fs::PathToString(pathTrafficGraph));
        return true;
    } catch (const std::exception& e) {
        LogPrintf("TrafficGraphWidget: Error loading binary data: %s\n", e.what());
        LogPrintf("TrafficGraphWidget: Attempting to load from CSV after binary load error\n");
        return loadDataFromCSV();
    }
}

bool TrafficGraphWidget::loadDataFromCSV() {
    LogPrintf("TrafficGraphWidget: Attempting to load data from CSV in the data directory\n");
    try {
        // Path to the CSV file
        fs::path pathCSV = fs::path((m_dataDir).toStdString().c_str()) / "trafficgraphdata.csv";
        QFile file(QString::fromStdString(fs::PathToString(pathCSV)));

        // Check if file exists and can be opened
        if (!file.exists()) {
            LogPrintf("TrafficGraphWidget: CSV file not found\n");
            return false;
        }

        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            LogPrintf("TrafficGraphWidget: CSV file exists but cannot be opened\n");
            return false;
        }

        LogPrintf("TrafficGraphWidget: CSV file found and opened successfully\n");

        QTextStream in(&file);
        QString line;

        // Variables to track current time range
        int currentRange = -1;

        // Clear existing data - in case the binary load partially succeeded
        for (unsigned int i = 0; i < VALUES_SIZE; i++) {
            vSamplesIn[i].clear();
            vSamplesOut[i].clear();
            vTimeStamp[i].clear();
        }

        // Read the file line by line
        while (!in.atEnd()) {
            line = in.readLine().trimmed();

            if (line.isEmpty()) continue;

            // Check for time range headers
            if (line.startsWith("#")) {
                // Time Range header: "# Time Range X: Y minutes"
                QRegExp rangeRegex("# Time Range (\\d+): (\\d+) minutes");
                if (rangeRegex.indexIn(line) != -1) {
                    currentRange = rangeRegex.cap(1).toInt();
                    LogPrintf("TrafficGraphWidget: Found original format header for range %d\n", currentRange);

                    // Validate range
                    if (currentRange < 0 || currentRange >= VALUES_SIZE) {
                        LogPrintf("TrafficGraphWidget: Invalid range in CSV: %d\n", currentRange);
                        currentRange = -1; // Reset to invalid
                    }
                }
                continue;
            }

            // Check for CSV DATA START format
            QRegExp startRegex("CSV DATA START - RANGE (\\d+)");
            if (startRegex.indexIn(line) != -1) {
                currentRange = startRegex.cap(1).toInt();
                LogPrintf("TrafficGraphWidget: Found CSV DATA START marker for range %d\n", currentRange);

                // Validate range
                if (currentRange < 0 || currentRange >= VALUES_SIZE) {
                    LogPrintf("TrafficGraphWidget: Invalid range in CSV: %d\n", currentRange);
                    currentRange = -1; // Reset to invalid
                }
                continue;
            }

            // Check for CSV DATA END format - we'll skip this line
            if (line.startsWith("CSV DATA END")) {
                LogPrintf("TrafficGraphWidget: Found CSV DATA END marker for range %d\n", currentRange);
                continue;
            }

            // Process data rows only if we have a valid current range
            if (currentRange >= 0 && currentRange < VALUES_SIZE) {
                // Check for header row
                if (line.startsWith("index,")) continue;

                // Parse data row: "index,timestamp,in_rate,out_rate"
                QStringList parts = line.split(',');
                if (parts.size() >= 4) {
                    // Convert strings to appropriate types
                    bool ok1, ok2, ok3, ok4;
                    int index = parts[0].toInt(&ok1);
                    uint64_t timestamp = parts[1].toLongLong(&ok2);
                    float inRate = parts[2].toFloat(&ok3);
                    float outRate = parts[3].toFloat(&ok4);

                    // Check conversions were successful
                    if (!ok1 || !ok2 || !ok3 || !ok4) {
                        LogPrintf("TrafficGraphWidget: Failed to parse CSV data row: %s\n", line.toStdString().c_str());
                        continue;
                    }

                    // Add to corresponding queues (push_back because we're reading oldest to newest)
                    Q_UNUSED(index);
                    vTimeStamp[currentRange].push_back(std::chrono::milliseconds{timestamp});
                    vSamplesIn[currentRange].push_back(inRate);
                    vSamplesOut[currentRange].push_back(outRate);
                }
            }
        }

        file.close();

        // Log how many data points were loaded for each time range
        int totalDataPoints = 0;
        for (unsigned int i = 0; i < VALUES_SIZE; i++) {
            if (!vSamplesIn[i].empty()) {
            int count = vSamplesIn[i].size();
            totalDataPoints += count;
            LogPrintf("TrafficGraphWidget: Loaded %d data points for time range %d (%d minutes)\n",
                count, i, values[i]);
            }
        }

        if (totalDataPoints == 0) {
            LogPrintf("TrafficGraphWidget: No data points were loaded from the CSV file\n");
            return false;
        }

        LogPrintf("TrafficGraphWidget: Successfully loaded %d total data points from CSV file\n", totalDataPoints);
        return true;
    } catch (const std::exception& e) {
        LogPrintf("TrafficGraphWidget: Error loading CSV data: %s\n", e.what());
        return false;
    }
}

bool TrafficGraphWidget::loadData() {
    bool success = false;

    // Try to load from binary file first, then fall back to CSV if that fails
    if (!(success = loadDataFromBinary())) success = loadDataFromCSV();

    if (!success) return false;

    // If we successfully loaded data, determine the correct band to use
    int firstNonFullBand = VALUES_SIZE - 1;

    for (int i = 0; i < VALUES_SIZE; i++)
        if (vTimeStamp[i].size() < DESIRED_SAMPLES) {
            firstNonFullBand = i;
            break;
        }

    if (firstNonFullBand == VALUES_SIZE - 1)
        LogPrintf("TrafficGraphWidget: After loading, all bands full, setting to highest band %d\n", firstNonFullBand);
    else
        LogPrintf("TrafficGraphWidget: After loading, setting to first non-full band %d\n", firstNonFullBand);

    if (firstNonFullBand) { // not the first band
        m_value = firstNonFullBand - 1; // Minus one as we're bumping it
        m_bump_value = true;
    }

    return true;
}
