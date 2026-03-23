// Copyright (c) 2020-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/peertablesortproxy.h>

#include <qt/peertablemodel.h>
#include <util/check.h>

#include <QModelIndex>
#include <QString>
#include <QVariant>

#include <cstdint>

namespace {
double GetTxBpsPctSortValue(const CNodeStats& stats)
{
    if (stats.nBTxBpsPct > 0) {
        return stats.nBTxBpsPct;
    }

    const uint64_t recv_base{stats.nRecvBytesSnapOld};
    const uint64_t send_base{stats.nSendBytesSnapOld};
    const uint64_t mempool_base{stats.nMempoolBytesSnapOld};
    const uint64_t recv{stats.nRecvBytes > recv_base ? stats.nRecvBytes - recv_base : stats.nRecvBytes};
    const uint64_t send{stats.nSendBytes > send_base ? stats.nSendBytes - send_base : stats.nSendBytes};
    const uint64_t mempool{stats.nMempoolBytes > mempool_base ? stats.nMempoolBytes - mempool_base : stats.nMempoolBytes};
    return (recv + send) > 0 ? 100.0 * mempool / (recv + send) : 0.0;
}

double GetMPpmSortValue(const CNodeStats& stats, const int64_t now)
{
    if (stats.nBTXpm > 0) {
        return stats.nBTXpm;
    }

    const int64_t time_base{stats.nTimeSnapOld > 0 ? stats.nTimeSnapOld : count_seconds(stats.m_connected)};
    const unsigned int txs_base{stats.nMempoolTXsSnapOld};
    const unsigned int txs{stats.nMempoolTXs > txs_base ? stats.nMempoolTXs - txs_base : stats.nMempoolTXs};
    return now > time_base ? 60.0 * txs / (now - time_base) : 0.0;
}
} // namespace

PeerTableSortProxy::PeerTableSortProxy(QObject* parent)
    : QSortFilterProxyModel(parent)
{
}

bool PeerTableSortProxy::lessThan(const QModelIndex& left_index, const QModelIndex& right_index) const
{
    auto left_data = sourceModel()->data(left_index, PeerTableModel::StatsRole);
    auto right_data = sourceModel()->data(right_index, PeerTableModel::StatsRole);

    if (!left_data.isValid() || !right_data.isValid()) return false;

    auto left_stats_ptr = left_data.value<CNodeCombinedStats*>();
    auto right_stats_ptr = right_data.value<CNodeCombinedStats*>();

    if (!left_stats_ptr || !right_stats_ptr) return false;

    const CNodeStats& left_stats = left_stats_ptr->nodeStats;
    const CNodeStats& right_stats = right_stats_ptr->nodeStats;

    switch (static_cast<PeerTableModel::ColumnIndex>(left_index.column())) {
    case PeerTableModel::NetNodeId:
        return left_stats.nodeid < right_stats.nodeid;
    case PeerTableModel::Address:
        return left_stats.m_addr_name.compare(right_stats.m_addr_name) < 0;
    case PeerTableModel::Direction:
        return left_stats.fInbound > right_stats.fInbound; // default sort Inbound, then Outbound
    //case PeerTableModel::ConnectionType:
    //    return left_stats.m_conn_type < right_stats.m_conn_type;
    //case PeerTableModel::Network:
    //    return left_stats.m_network < right_stats.m_network;
    case PeerTableModel::Ping:
        return left_stats.m_min_ping_time < right_stats.m_min_ping_time;
    case PeerTableModel::Sent: {
        int64_t now = GetTimeSeconds();
        int Right = right_stats.nSendBytes * 8 / (now + 1 - count_seconds(right_stats.m_connected));
        int Left = left_stats.nSendBytes * 8 / (now + 1 - count_seconds(left_stats.m_connected));
        return Left < Right;
    }
    case PeerTableModel::Recv: {
        int64_t now = GetTimeSeconds();
        int Right; int Left;
        if (right_stats.nRecvBytesSnapOld && now > right_stats.nTimeSnapOld)
            Right = ((right_stats.nRecvBytes - right_stats.nRecvBytesSnapOld) * 8 / (now + 1 - right_stats.nTimeSnapOld));
        else
            Right = right_stats.nRecvBytes * 8 / (now + 1 - count_seconds(right_stats.m_connected));
        if (left_stats.nRecvBytesSnapOld && now > left_stats.nTimeSnapOld)
            Left = ((left_stats.nRecvBytes - left_stats.nRecvBytesSnapOld) * 8 / (now + 1 - left_stats.nTimeSnapOld));
        else
            Left = left_stats.nRecvBytes * 8 / (now + 1 - count_seconds(left_stats.m_connected));
        return Left < Right;
    }
    case PeerTableModel::TxBpsPct: {
        const double Right = GetTxBpsPctSortValue(right_stats);
        const double Left = GetTxBpsPctSortValue(left_stats);
        return Left < Right;
    }
    case PeerTableModel::MPpm: {
        const int64_t now = GetTimeSeconds();
        const double Right = GetMPpmSortValue(right_stats, now);
        const double Left = GetMPpmSortValue(left_stats, now);
        return Left < Right;
    }
    case PeerTableModel::Subversion:
        return left_stats.cleanSubVer.compare(right_stats.cleanSubVer) < 0;
    } // no default case, so the compiler can warn about missing cases
    assert(false);
}
