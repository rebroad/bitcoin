// Copyright (c) 2011-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_TRAFFICGRAPHWIDGET_H
#define BITCOIN_QT_TRAFFICGRAPHWIDGET_H

#include <interfaces/node.h>
#include <QFile>
#include <QKeyEvent>
#include <QQueue>
#include <QWidget>

#include <chrono>
#include <map>
#include <string>
#include <vector>

class ClientModel;

QT_BEGIN_NAMESPACE
class QPaintEvent;
class QTimer;
QT_END_NAMESPACE

static constexpr int VALUES_SIZE = 13;

class TrafficGraphWidget : public QWidget
{
    Q_OBJECT

public:
    explicit TrafficGraphWidget(QWidget* parent = nullptr);
    void setClientModel(ClientModel* model);
    bool graphRangeBump() const { return m_bump; }
    unsigned int getCurrentRangeIndex() const { return m_new_value; }
    quint64 getBaselineBytesRecv() const { return m_baseline_bytes_recv; }
    quint64 getBaselineBytesSent() const { return m_baseline_bytes_sent; }

protected:
    void paintEvent(QPaintEvent*) override;
    int yValue(float) const;
    void mouseMoveEvent(QMouseEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void leaveEvent(QEvent*) override;
    int findClosestPointByTimestamp(int) const;

public Q_SLOTS:
    void updateStuff();
    int setGraphRange(int);

private:
    struct DirectionSampleStats {
        bool has_peer{false};
        bool has_msg_type{false};
        NodeId top_peer_id{-1};
        std::string top_peer_addr;
        std::string top_msg_type;
        uint64_t top_peer_rate_bps{0};
        uint64_t avg_peer_rate_bps{0};
        uint64_t top_msg_type_rate_bps{0};
        int peer_count{0};
    };

    void saveData();
    int paintPath(QPainterPath&, const QQueue<float>&);
    bool loadDataFromBinary();
    bool loadData();
    void updateFmax();
    void updateDirectionalSampleStats(int range_index, int64_t sample_duration_msecs, const std::vector<CNodeStats>& peer_stats, bool outgoing);
    void updateRates(int range_index, int64_t now, quint64 bytes_in, quint64 bytes_out, const std::vector<CNodeStats>* peer_stats);
    void focusSlider();
    void drawTooltipPoint(QPainter&);

    QTimer* m_timer{nullptr};
    float m_fmax{1.1f};
    float m_new_fmax{1.1f};
    float m_range{0};
    QQueue<float> m_samples_in[VALUES_SIZE] = {};
    QQueue<float> m_samples_out[VALUES_SIZE] = {};
    QQueue<DirectionSampleStats> m_incoming_sample_stats[VALUES_SIZE] = {};
    QQueue<DirectionSampleStats> m_outgoing_sample_stats[VALUES_SIZE] = {};
    QQueue<int64_t> m_time_stamp[VALUES_SIZE] = {};
    quint64 m_last_bytes_in[VALUES_SIZE] = {};
    quint64 m_last_bytes_out[VALUES_SIZE] = {};
    int64_t m_last_time[VALUES_SIZE] = {};
    std::map<NodeId, uint64_t> m_last_peer_recv_bytes[VALUES_SIZE] = {};
    std::map<NodeId, uint64_t> m_last_peer_send_bytes[VALUES_SIZE] = {};
    mapMsgCmdSize m_last_msg_recv_bytes[VALUES_SIZE] = {};
    mapMsgCmdSize m_last_msg_send_bytes[VALUES_SIZE] = {};
    std::map<NodeId, std::string> m_connected_peer_addr = {};
    ClientModel* m_client_model{nullptr};
    int m_value{0};
    int m_new_value{0};
    bool m_bump{false};
    bool m_toggle{true}; // Default to logarithmic
    bool m_update{false}; // whether to redraw graph
    int m_tt_point{0}; // 0 = no tooltip (array index + 1)
    bool m_tt_in_series{true}; // true = in, false = out
    int m_x_offset{0};
    int m_y_offset{0};
    int64_t m_tt_time{0};
    int m_values[VALUES_SIZE] = {5, 10, 20, 45, 90, 3*60, 6*60, 12*60, 24*60, 3*24*60, 7*24*60, 14*24*60, 28*24*60};
    std::string m_data_dir;
    interfaces::Node* m_node;
    quint64 m_baseline_bytes_recv{0};
    quint64 m_baseline_bytes_sent{0};
    int64_t m_last_save_ms{0};
};

#endif // BITCOIN_QT_TRAFFICGRAPHWIDGET_H
