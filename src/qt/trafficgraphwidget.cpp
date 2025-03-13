// Copyright (c) 2011-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <interfaces/node.h>
#include <qt/trafficgraphwidget.h>
#include <qt/clientmodel.h>
#include <qt/guiutil.h>
#include <clientversion.h>
#include <util/system.h>

#include <QPainter>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFileDialog>
#include <QMessageBox>
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
	setFocusPolicy(Qt::StrongFocus); // To accept keyboard events
}

TrafficGraphWidget::~TrafficGraphWidget()
{
    saveData();
}

void TrafficGraphWidget::setClientModel(ClientModel *model) {
	clientModel = model;
	int64_t nTime = GetTimeMillis();
	if (model) {
		if (vSamplesIn[0].empty() && vSamplesOut[0].empty()) {
		// Load saved traffic data if available and the arrays are empty
			LogPrintf("vSamplesIn[0].empty()=%d\n", vSamplesIn[0].empty());
			loadData();
			return;
		}

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

int TrafficGraphWidget::y_value(float value) {
	int h = height() - YMARGIN * 2;

	// Check for potential division by zero or very small values
	if (fMax <= 0.0001f) {
		LogPrintf("TrafficGraphWidget::y_value: fMax is too small or zero: %f\n", fMax);
		return YMARGIN + h; // Return bottom of the graph
	}

	// Check for NaN input value
	if (std::isnan(value) || std::isinf(value)) {
		LogPrintf("TrafficGraphWidget::y_value: input value is NaN or infinity: %f\n", value);
		return YMARGIN + h; // Return bottom of the graph
	}

	float result;
	if (fToggle) {
		result = pow(value, 0.30102) / pow(fMax, 0.30102);
	} else {
		result = value / fMax;
	}

	// Check final calculation result
	if (std::isnan(result) || std::isinf(result)) {
		LogPrintf("TrafficGraphWidget::y_value: calculation resulted in NaN or infinity. value: %f, fMax: %f, result: %f\n",
				 value, fMax, result);
		return YMARGIN + h; // Return bottom of the graph
	}

	return YMARGIN + h - (h * 1.0 * result);
}

void TrafficGraphWidget::paintPath(QPainterPath &path, QQueue<float> &samples)
{
	int sampleCount = std::min(int(DESIRED_SAMPLES * m_range / values[m_value]), int(samples.size()));
	if(sampleCount > 0) {
		int h = height() - YMARGIN * 2, w = width() - XMARGIN * 2;
		int x = XMARGIN + w;
		path.moveTo(x, YMARGIN + h);
		for(int i = 0; i < sampleCount; ++i) {
			float sample = samples.at(i);
			x = XMARGIN + w - w * i * values[m_value] / m_range / DESIRED_SAMPLES;

			// Check for NaN or infinity in calculations
			if (std::isnan(x) || std::isinf(x)) {
				LogPrintf("TrafficGraphWidget::paintPath: x coordinate is NaN or infinity at index %d\n", i);
				continue; // Skip this point
			}

			int y = y_value(sample);

			// Check for NaN or infinity in y value
			if (std::isnan(y) || std::isinf(y)) {
				LogPrintf("TrafficGraphWidget::paintPath: y coordinate is NaN or infinity at index %d (sample: %f, fMax: %f)\n",
						  i, sample, fMax);
				continue; // Skip this point
			}

			// Only add valid points to the path
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
		int x = XMARGIN + w - w * ttpoint * values[m_value] / m_range / DESIRED_SAMPLES;
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
		if (values[m_new_value] > m_range && values[m_value] < m_range) {
			LogPrintf("%s: m_value %d->%d m_range %d->%d cur_range=%d\n", __func__, m_value, m_value+1,
				values[m_value], values[m_value+1], m_range);
			m_value++; // TODO - re-assess the tooltip
		} else if (m_value > 0 && values[m_new_value] <= m_range && values[m_value-1] > m_range * 0.99) {
			LogPrintf("%s: m_value %d->%d m_range %d->%d cur_range=%d\n", __func__, m_value, m_value-1,
				values[m_value], values[m_value-1], m_range);
			m_value--; // TODO - re-assess the tooltip
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
	if (!fFull[i] && vTimeStamp[i].size()+4 > DESIRED_SAMPLES)
		LogPrintf("%s: fFull[%d] %d steps from full\n", __func__, i, DESIRED_SAMPLES+1 - vTimeStamp[i].size());
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

void TrafficGraphWidget::keyPressEvent(QKeyEvent *event)
{
	if (event->modifiers() & Qt::ControlModifier) {
		if (event->key() == Qt::Key_E) {
			exportData();
			return;
		}
	}
	QWidget::keyPressEvent(event);
}

void TrafficGraphWidget::exportData()
{
	if (!clientModel) return;

	// Create a JSON object to store the data
	QJsonObject jsonObj;

	// Add values array
	QJsonArray valuesArray;
	for (int i = 0; i < VALUES_SIZE; i++) {
		valuesArray.append(QJsonValue(static_cast<int>(values[i])));
	}
	jsonObj["values"] = valuesArray;

	// Add nLastBytesIn array
	QJsonArray lastBytesInArray;
	for (int i = 0; i < VALUES_SIZE; i++) {
		lastBytesInArray.append(QJsonValue(QString::number(nLastBytesIn[i])));
	}
	jsonObj["nLastBytesIn"] = lastBytesInArray;

	// Add nLastBytesOut array
	QJsonArray lastBytesOutArray;
	for (int i = 0; i < VALUES_SIZE; i++) {
		lastBytesOutArray.append(QJsonValue(QString::number(nLastBytesOut[i])));
	}
	jsonObj["nLastBytesOut"] = lastBytesOutArray;

	// Add nLastTime array
	QJsonArray lastTimeArray;
	for (int i = 0; i < VALUES_SIZE; i++) {
		lastTimeArray.append(QJsonValue(QString::number(nLastTime[i].count())));
	}
	jsonObj["nLastTime"] = lastTimeArray;

	// Add vSamplesIn, vSamplesOut, and vTimeStamp arrays
	QJsonArray samplesInArray;
	QJsonArray samplesOutArray;
	QJsonArray timeStampArray;

	for (int i = 0; i < VALUES_SIZE; i++) {
		QJsonArray samplesInSubArray;
		for (int j = 0; j < vSamplesIn[i].size(); j++) {
			samplesInSubArray.append(QJsonValue(vSamplesIn[i].at(j)));
		}
		samplesInArray.append(samplesInSubArray);

		QJsonArray samplesOutSubArray;
		for (int j = 0; j < vSamplesOut[i].size(); j++) {
			samplesOutSubArray.append(QJsonValue(vSamplesOut[i].at(j)));
		}
		samplesOutArray.append(samplesOutSubArray);

		QJsonArray timeStampSubArray;
		for (int j = 0; j < vTimeStamp[i].size(); j++) {
			timeStampSubArray.append(QJsonValue(QString::number(vTimeStamp[i].at(j).count())));
		}
		timeStampArray.append(timeStampSubArray);
	}

	jsonObj["vSamplesIn"] = samplesInArray;
	jsonObj["vSamplesOut"] = samplesOutArray;
	jsonObj["vTimeStamp"] = timeStampArray;

	// Convert to JSON document
	QJsonDocument doc(jsonObj);

	// Get a filename from the user or use the default
	QString fileName = QFileDialog::getSaveFileName(this, tr("Save Traffic Graph Data"),
												  "traffic_data.json",
												  tr("JSON Files (*.json)"));

	if (fileName.isEmpty()) {
		return; // User canceled the dialog
	}

	// Save to file
	QFile file(fileName);
	if (!file.open(QIODevice::WriteOnly)) {
		QMessageBox::critical(this, tr("Error"), tr("Could not open file for writing"));
		return;
	}

	file.write(doc.toJson());
	file.close();

	QMessageBox::information(this, tr("Export Successful"),
						   tr("Traffic data has been exported to %1").arg(fileName));
}

void TrafficGraphWidget::saveData()
{
    if (!clientModel) return;

    try {
	fs::path pathTrafficGraph = fs::path("/tmp/trafficgraphdata");
	FILE* file = fsbridge::fopen(pathTrafficGraph, "wb");
	if (file) {
	    CAutoFile fileout(file, SER_DISK, CLIENT_VERSION);
	    if (!fileout.IsNull()) {
		// Version
		fileout << static_cast<int>(1);

		// Save values array
		for (unsigned int i = 0; i < VALUES_SIZE; i++) {
		    fileout << VARINT(static_cast<uint32_t>(values[i]));
		}

		// Save nLastBytesIn array
		for (unsigned int i = 0; i < VALUES_SIZE; i++) {
		    fileout << VARINT(nLastBytesIn[i]);
		}

		// Save nLastBytesOut array
		for (unsigned int i = 0; i < VALUES_SIZE; i++) {
		    fileout << VARINT(nLastBytesOut[i]);
		}

		// Save nLastTime array
		for (unsigned int i = 0; i < VALUES_SIZE; i++) {
		    fileout << VARINT(static_cast<uint64_t>(nLastTime[i].count()));
		}

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

		    for (unsigned int j = 0; j < timeStampSize; j++) {
			fileout << VARINT(static_cast<uint64_t>(vTimeStamp[i].at(j).count()));
		    }
		}

		fileout.fclose();
		LogPrintf("TrafficGraphWidget: Data saved to %s\n", fs::PathToString(pathTrafficGraph));
	    }
	}
    } catch (const std::exception& e) {
	LogPrintf("TrafficGraphWidget: Error saving data: %s\n", e.what());
    }
}

bool TrafficGraphWidget::loadData() {
    LogPrintf("TrafficGraphWidget: Attempting to load binary data file\n");
    try {
		fs::path pathTrafficGraph = fs::path("/tmp/trafficgraphdata");
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

		// We don't load the values array as it's initialized in the header
		// Skip values
		for (unsigned int i = 0; i < VALUES_SIZE; i++) {
		    uint32_t dummy;
		    filein >> VARINT(dummy);
		}

		// Load nLastBytesIn array
		for (unsigned int i = 0; i < VALUES_SIZE; i++)
		    filein >> VARINT(nLastBytesIn[i]);

		// Load nLastBytesOut array
		for (unsigned int i = 0; i < VALUES_SIZE; i++)
		    filein >> VARINT(nLastBytesOut[i]);

		// Load nLastTime array
		for (unsigned int i = 0; i < VALUES_SIZE; i++) {
		    uint64_t timeMs;
		    filein >> VARINT(timeMs);
		    nLastTime[i] = std::chrono::milliseconds{static_cast<int64_t>(timeMs)};
		}

		// Load vSamplesIn, vSamplesOut, and vTimeStamp arrays
		for (unsigned int i = 0; i < VALUES_SIZE; i++) {
		    // Clear existing data
		    vSamplesIn[i].clear();
		    vSamplesOut[i].clear();
		    vTimeStamp[i].clear();

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

bool TrafficGraphWidget::loadDataFromCSV()
{
    LogPrintf("TrafficGraphWidget: Attempting to load data from CSV at /tmp/trafficgraphdata.csv\n");
    try {
	// Path to the CSV file
	fs::path pathCSV = fs::path("/tmp/trafficgraphdata.csv");
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

	// Clear existing data
	for (unsigned int i = 0; i < VALUES_SIZE; i++) {
	    vSamplesIn[i].clear();
	    vSamplesOut[i].clear();
	    vTimeStamp[i].clear();
	}

	// First, let's examine the CSV file to check if timestamps are valid
	QTextStream preReadStream(&file);
	QString line;
	int currentRange = -1;
	int largestRangeIndex = VALUES_SIZE - 1; // Index for 28-day range
	int sampleCount = 0;
	bool timestampsValid = true;
	int64_t currentTime = GetTime();
	int64_t oldestAllowedTime = currentTime - (365 * 24 * 60 * 60); // 1 year ago
	int64_t lastValidTimestamp = 0;

	// First pass: check validity of timestamps and count samples in largest range
	while (!preReadStream.atEnd()) {
	    line = preReadStream.readLine().trimmed();

	    // Skip empty lines
	    if (line.isEmpty()) continue;

	    // Process range headers
	    if (line.startsWith("#") || line.startsWith("CSV DATA START")) {
			QRegExp rangeRegex;
			if (line.startsWith("#")) {
			    // Time Range header: "# Time Range X: Y minutes"
			    rangeRegex = QRegExp("# Time Range (\\d+): (\\d+) minutes");
			} else {
			    // CSV DATA START format
			    rangeRegex = QRegExp("CSV DATA START - RANGE (\\d+)");
			}

			if (rangeRegex.indexIn(line) != -1) {
			    currentRange = rangeRegex.cap(1).toInt();
			    // Validate range
			    if (currentRange < 0 || currentRange >= VALUES_SIZE) {
					LogPrintf("TrafficGraphWidget: Invalid range in CSV: %d\n", currentRange);
					currentRange = -1; // Reset to invalid
			    }
			}
			continue;
	    }

	    // Skip CSV DATA END lines
	    if (line.startsWith("CSV DATA END")) continue;

	    // Skip header rows
	    if (line.startsWith("index,")) continue;

	    // Process data rows if we have a valid current range
	    if (currentRange >= 0 && currentRange < VALUES_SIZE) {
			// Parse data row: "index,timestamp,in_rate,out_rate"
			QStringList parts = line.split(',');
			if (parts.size() >= 4) {
			    bool ok;
			    int64_t timestamp = parts[1].toLongLong(&ok);

			    if (!ok) {
					LogPrintf("TrafficGraphWidget: Failed to parse timestamp: %s\n", parts[1].toStdString().c_str());
					continue;
			    }

			    // Count samples in the largest range (28 days)
				// TODO better to use the first range it finds that is not full. If all full, then it's 28 days.
			    if (currentRange == largestRangeIndex) sampleCount++;

			    // Validate timestamps
			    // 1. Check for future timestamps
			    if (timestamp > currentTime * 1000) {
					LogPrintf("TrafficGraphWidget: Found future timestamp %lld (current time: %lld)\n", timestamp/1000, currentTime);
					timestampsValid = false;
			    }

			    // 2. Check for extremely old timestamps (more than a year old)
			    if (timestamp/1000 < oldestAllowedTime) {
					LogPrintf("TrafficGraphWidget: Found too old timestamp %lld (oldest allowed: %lld)\n", timestamp/1000, oldestAllowedTime);
					timestampsValid = false;
			    }

			    // 3. Check for non-linear sequence (timestamps should be in descending order as we read the file)
			    if (lastValidTimestamp > 0 && lastValidTimestamp <= timestamp) {
					LogPrintf("TrafficGraphWidget: Found non-linear timestamp sequence: %lld after %lld\n", timestamp/1000, lastValidTimestamp/1000);
					timestampsValid = false;
			    }

			    lastValidTimestamp = timestamp;
			}
	    }
	}

	// If timestamps are invalid, we need to synthesize new ones
	if (!timestampsValid) {
	    LogPrintf("TrafficGraphWidget: Invalid timestamps detected, will synthesize new timestamps\n");

	    // Reset file position to beginning
	    file.seek(0);

	    // Calculate sample density based on largest range (28-day range)
	    int samplesNeeded = sampleCount > 0 ? sampleCount : DESIRED_SAMPLES;
	    int daysToScan = 0;

	    if (samplesNeeded > 0) {
		// Calculate days to scan based on sample density
		// For the 28-day range with samplesNeeded samples, calculate proportional days
		daysToScan = 28 * samplesNeeded / DESIRED_SAMPLES;
		LogPrintf("TrafficGraphWidget: Largest range has %d samples, will scan debug.log for %d days\n",
			  samplesNeeded, daysToScan);
	    } else {
		// Default to 14 days if we couldn't determine sample count
		daysToScan = 14;
		LogPrintf("TrafficGraphWidget: Using default of 14 days to scan debug.log\n");
	    }

	    // Open and read debug.log to identify running periods
	    QFile debugLog(QString::fromStdString(fs::PathToString(gArgs.GetDataDirNet() / "debug.log")));
	    QVector<QPair<int64_t, int64_t>> runningPeriods;

	    if (debugLog.open(QIODevice::ReadOnly | QIODevice::Text)) {
		QTextStream debugStream(&debugLog);
		int64_t lastLogTime = 0;
		int64_t currentStartTime = 0;
		const int64_t gapThreshold = 30 * 60; // 30 minutes in seconds
		int64_t cutoffTime = currentTime - (daysToScan * 24 * 60 * 60); // daysToScan days ago

		LogPrintf("TrafficGraphWidget: Analyzing debug.log for Bitcoin running periods\n");

		// Process debug.log to find running periods
		while (!debugStream.atEnd()) {
		    QString logLine = debugStream.readLine();

		    // Parse timestamp from debug.log line
		    // Format is typically: YYYY-MM-DD HH:MM:SS message
		    QRegExp timeRegex("(\\d{4})-(\\d{2})-(\\d{2}) (\\d{2}):(\\d{2}):(\\d{2})");
		    if (timeRegex.indexIn(logLine) != -1) {
			QDateTime logDateTime = QDateTime::fromString(
			    timeRegex.cap(0), "yyyy-MM-dd HH:mm:ss");
			int64_t logTimestamp = logDateTime.toSecsSinceEpoch();

			// Only consider log entries within our time window
			if (logTimestamp >= cutoffTime) {
			    if (lastLogTime == 0) {
				// First valid log entry
				currentStartTime = logTimestamp;
			    } else if (logTimestamp - lastLogTime > gapThreshold) {
				// Found a gap, end previous period and start new one
				if (currentStartTime > 0 && lastLogTime > currentStartTime) {
				    runningPeriods.append(qMakePair(currentStartTime, lastLogTime));
				    LogPrintf("TrafficGraphWidget: Found running period from %s to %s\n",
					      FormatISO8601DateTime(currentStartTime).c_str(),
					      FormatISO8601DateTime(lastLogTime).c_str());
				}
				currentStartTime = logTimestamp;
			    }
			    lastLogTime = logTimestamp;
			}
		    }
		}

		// Add the final period if there is one
		if (currentStartTime > 0 && lastLogTime > currentStartTime) {
		    runningPeriods.append(qMakePair(currentStartTime, lastLogTime));
		    LogPrintf("TrafficGraphWidget: Found final running period from %s to %s\n",
			      FormatISO8601DateTime(currentStartTime).c_str(),
			      FormatISO8601DateTime(lastLogTime).c_str());
		}

		debugLog.close();
		LogPrintf("TrafficGraphWidget: Found %d running periods in debug.log\n", runningPeriods.size());
	    } else {
		LogPrintf("TrafficGraphWidget: Could not open debug.log, using current time as reference\n");
		// If we can't access debug.log, create a single running period for the last daysToScan days
		runningPeriods.append(qMakePair(currentTime - (daysToScan * 24 * 60 * 60), currentTime));
	    }

	    // If no running periods found, create a fallback period
	    if (runningPeriods.isEmpty()) {
		LogPrintf("TrafficGraphWidget: No running periods found, using fallback period\n");
		runningPeriods.append(qMakePair(currentTime - (daysToScan * 24 * 60 * 60), currentTime));
	    }

	    // Now synthesize timestamps for each range based on running periods
	    for (unsigned int i = 0; i < VALUES_SIZE; i++) {
		// Clear any partial data
		vSamplesIn[i].clear();
		vSamplesOut[i].clear();
		vTimeStamp[i].clear();

		// Calculate samples per range
		int rangeMinutes = values[i];
		int targetSamples = std::min(DESIRED_SAMPLES, static_cast<int>(DESIRED_SAMPLES * rangeMinutes / values[largestRangeIndex]));

		// Calculate total running time across all periods
		int64_t totalRunningTime = 0;
		for (const auto& period : runningPeriods) {
		    totalRunningTime += (period.second - period.first);
		}

		// Skip if no running time
		if (totalRunningTime <= 0) continue;

		// Calculate interval between samples based on total running time
		int64_t sampleIntervalSeconds = totalRunningTime / std::max(1, targetSamples);

		LogPrintf("TrafficGraphWidget: Synthesizing %d samples for range %d (%d minutes) with interval %d seconds\n",
			  targetSamples, i, rangeMinutes, sampleIntervalSeconds);

		// Generate synthetic samples distributed across running periods
		int samplesGenerated = 0;
		for (int periodIdx = runningPeriods.size() - 1; periodIdx >= 0 && samplesGenerated < targetSamples; periodIdx--) {
		    int64_t periodStart = runningPeriods[periodIdx].first;
		    int64_t periodEnd = runningPeriods[periodIdx].second;
		    int64_t periodDuration = periodEnd - periodStart;

		    // Calculate how many samples to generate in this period
		    int periodSamples = std::min(targetSamples - samplesGenerated,
					      static_cast<int>(periodDuration * targetSamples / totalRunningTime));

		    // Ensure we generate at least one sample if this is the only period
		    if (periodSamples == 0 && runningPeriods.size() == 1) {
			periodSamples = 1;
		    }

		    // Calculate interval for this period
		    int64_t periodInterval = periodSamples > 1 ? periodDuration / (periodSamples - 1) : periodDuration;

		    // Generate evenly spaced samples for this period
		    for (int s = 0; s < periodSamples && samplesGenerated < targetSamples; s++) {
			int64_t sampleTime = periodEnd - s * periodInterval;

			// Generate random traffic values (these will be replaced by actual values later)
			float inRate = 0.01f + (static_cast<float>(rand()) / RAND_MAX) * 1.0f;  // Random value between 0.01 and 1.01
			float outRate = 0.01f + (static_cast<float>(rand()) / RAND_MAX) * 0.5f; // Random value between 0.01 and 0.51

			// Add to corresponding queues (push_front because we're generating newest to oldest)
			vSamplesIn[i].push_front(inRate);
			vSamplesOut[i].push_front(outRate);
			vTimeStamp[i].push_front(std::chrono::milliseconds{sampleTime * 1000});

			samplesGenerated++;
		    }
		}

		LogPrintf("TrafficGraphWidget: Generated %d synthetic samples for range %d\n",
			  samplesGenerated, i);

		// Set last values for this range
		if (!vSamplesIn[i].empty()) {
		    nLastBytesIn[i] = vSamplesIn[i].front() * 1000; // Approximate byte counts
		    nLastBytesOut[i] = vSamplesOut[i].front() * 1000;
		    nLastTime[i] = vTimeStamp[i].front();
		}
	    }

	    LogPrintf("TrafficGraphWidget: Finished synthesizing timestamps\n");
	    return true;
	}

	// If timestamps are valid, continue with normal CSV loading
	file.seek(0);

		QTextStream in(&file);

		// Read the file line by line
		while (!in.atEnd()) {
		    line = in.readLine().trimmed();

		    // Skip empty lines
		    // Skip empty lines
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
				    bool ok1, ok2, ok3;
				    int index = parts[0].toInt(&ok1);
				    Q_UNUSED(index);
				    int64_t timestamp = parts[1].toLongLong(&ok2);
				    float inRate = parts[2].toFloat(&ok3);

				    // Check conversions were successful
				    if (!ok1 || !ok2 || !ok3) {
						LogPrintf("TrafficGraphWidget: Failed to parse CSV data row: %s\n", line.toStdString().c_str());
						continue;
				    }

				    float outRate = parts[3].toFloat();

				    // Add to corresponding queues (push_back because we're reading oldest to newest)
				    vSamplesIn[currentRange].push_back(inRate);
				    vSamplesOut[currentRange].push_back(outRate);
				    vTimeStamp[currentRange].push_back(std::chrono::milliseconds{timestamp});
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
		// Set last values based on the first (most recent) entries in the queues
		for (unsigned int i = 0; i < VALUES_SIZE; i++) {
		    if (!vSamplesIn[i].empty() && !vSamplesOut[i].empty() && !vTimeStamp[i].empty()) {
				nLastBytesIn[i] = vSamplesIn[i].front() * 1000; // Approximate byte counts
				nLastBytesOut[i] = vSamplesOut[i].front() * 1000;
				nLastTime[i] = vTimeStamp[i].front();
		    }
		}

		LogPrintf("TrafficGraphWidget: Successfully loaded data from CSV file %s\n", fs::PathToString(pathCSV));
		return true;
    } catch (const std::exception& e) {
		LogPrintf("TrafficGraphWidget: Error loading CSV data: %s\n", e.what());
		return false;
    }
}
