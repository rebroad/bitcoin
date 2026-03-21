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
        const uint64_t right_recv_base{right_stats.nRecvBytesSnapOld};
        const uint64_t left_recv_base{left_stats.nRecvBytesSnapOld};
        const uint64_t right_mempool_base{right_stats.nMempoolBytesSnapOld};
        const uint64_t left_mempool_base{left_stats.nMempoolBytesSnapOld};
        const uint64_t right_recv{right_stats.nRecvBytes > right_recv_base ? right_stats.nRecvBytes - right_recv_base : right_stats.nRecvBytes};
        const uint64_t left_recv{left_stats.nRecvBytes > left_recv_base ? left_stats.nRecvBytes - left_recv_base : left_stats.nRecvBytes};
        const uint64_t right_mempool{right_stats.nMempoolBytes > right_mempool_base ? right_stats.nMempoolBytes - right_mempool_base : right_stats.nMempoolBytes};
        const uint64_t left_mempool{left_stats.nMempoolBytes > left_mempool_base ? left_stats.nMempoolBytes - left_mempool_base : left_stats.nMempoolBytes};
        const double Right = right_recv > 0 ? 1.0 * right_mempool / right_recv : 0.0;
        const double Left = left_recv > 0 ? 1.0 * left_mempool / left_recv : 0.0;
        return Left < Right;
    }
    case PeerTableModel::MPpm: {
        int64_t now = GetTimeSeconds();
        const int64_t right_time_base{right_stats.nTimeSnapOld > 0 ? right_stats.nTimeSnapOld : count_seconds(right_stats.m_connected)};
        const int64_t left_time_base{left_stats.nTimeSnapOld > 0 ? left_stats.nTimeSnapOld : count_seconds(left_stats.m_connected)};
        const unsigned int right_txs_base{right_stats.nMempoolTXsSnapOld};
        const unsigned int left_txs_base{left_stats.nMempoolTXsSnapOld};
        const unsigned int right_txs{right_stats.nMempoolTXs > right_txs_base ? right_stats.nMempoolTXs - right_txs_base : right_stats.nMempoolTXs};
        const unsigned int left_txs{left_stats.nMempoolTXs > left_txs_base ? left_stats.nMempoolTXs - left_txs_base : left_stats.nMempoolTXs};
        double Right;
        double Left;
        if (right_stats.nBTXpm && left_stats.nBTXpm) {
            Right = right_stats.nBTXpm;
            Left = left_stats.nBTXpm;
        } else {
            Right = now > right_time_base ? 1.0 * right_txs / (now - right_time_base) : 0.0;
            Left = now > left_time_base ? 1.0 * left_txs / (now - left_time_base) : 0.0;
        }
        return Left < Right;
    }
    case PeerTableModel::Subversion:
        return left_stats.cleanSubVer.compare(right_stats.cleanSubVer) < 0;
    } // no default case, so the compiler can warn about missing cases
    assert(false);
}
