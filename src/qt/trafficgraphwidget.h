// Copyright (c) 2011-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_TRAFFICGRAPHWIDGET_H
#define BITCOIN_QT_TRAFFICGRAPHWIDGET_H

#include <QWidget>
#include <QQueue>

class ClientModel;

QT_BEGIN_NAMESPACE
class QPaintEvent;
class QTimer;
QT_END_NAMESPACE

#define VALUES_SIZE 14

class TrafficGraphWidget : public QWidget
{
    Q_OBJECT

public:
    explicit TrafficGraphWidget(QWidget *parent = nullptr);
    void setClientModel(ClientModel *model);
    int getGraphRangeMins() const;

protected:
    void paintEvent(QPaintEvent *) override;
    int y_value(float value);
    void mouseMoveEvent(QMouseEvent *event) override;
    int ttpoint = -1;
    int x_offset = 0;
    int y_offset = 0;
    int64_t tt_time = 0;
    void mousePressEvent(QMouseEvent *event) override;
    bool fToggle = true;
    void focusInEvent(QFocusEvent *evt) override;
    void focusOutEvent(QFocusEvent *evt) override;

public Q_SLOTS:
    void updateStuff();
    int setGraphRange(float nMins);
    int getGraphRange() const;
    void clear();

private:
    void updatefMax();
    void paintPath(QPainterPath &path, QQueue<float> &samples);
    void updateRates(int value);

    QTimer *timer;
    float fMax;
    float new_fMax;
    float fMins;
    float new_fMins;
    int nValue;
    QQueue<float> vSamplesIn[VALUES_SIZE];
    QQueue<float> vSamplesOut[VALUES_SIZE];
    QQueue<float> vTimeStamp[VALUES_SIZE];
    quint64 nLastBytesIn[VALUES_SIZE];
    quint64 nLastBytesOut[VALUES_SIZE];
    int64_t nLastTime[VALUES_SIZE];
    ClientModel *clientModel;
};

#endif // BITCOIN_QT_TRAFFICGRAPHWIDGET_H
