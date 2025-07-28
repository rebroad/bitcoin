// Copyright (c) 2020-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/peertablesortproxy.h>

#include <qt/peertablemodel.h>
#include <util/check.h>

#include <QModelIndex>
#include <QString>
#include <QVariant>

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
        if (right_stats.nRecvBytesSnapOld)
            Right = ((right_stats.nRecvBytes - right_stats.nRecvBytesSnapOld) * 8 / (now + 1 - right_stats.nTimeSnapOld));
        else
            Right = right_stats.nRecvBytes * 8 / (now + 1 - count_seconds(right_stats.m_connected));
        if (left_stats.nRecvBytesSnapOld)
            Left = ((left_stats.nRecvBytes - left_stats.nRecvBytesSnapOld) * 8 / (now + 1 - left_stats.nTimeSnapOld));
        else
            Left = left_stats.nRecvBytes * 8 / (now + 1 - count_seconds(left_stats.m_connected));
        return Left < Right;
    }
    case PeerTableModel::TxBpsPct: {
        double Right; double Left;
        if (right_stats.nBTxBpsPct && left_stats.nBTxBpsPct) {
            Right = 1.0 * right_stats.nBTxBpsPct;
            Left = 1.0 * left_stats.nBTxBpsPct;
        } else {
            Right = 1.0 * (right_stats.nMempoolBytes - right_stats.nMempoolBytesSnapOld) / (right_stats.nRecvBytes - right_stats.nRecvBytesSnapOld);
            Left = 1.0 * (left_stats.nMempoolBytes - left_stats.nMempoolBytesSnapOld) / (left_stats.nRecvBytes - left_stats.nRecvBytesSnapOld);
        }
        return Left < Right;
    }
    case PeerTableModel::MPpm: {
        int64_t now = GetTimeSeconds();
        double Right; double Left;
        if (right_stats.nBTXpm && left_stats.nBTXpm) {
            Right = right_stats.nBTXpm;
            Left = left_stats.nBTXpm;
        } else {
            Right = 1.0 * (right_stats.nMempoolTXs - right_stats.nMempoolTXsSnapOld) / (now - right_stats.nTimeSnapOld);
            Left = 1.0 * (left_stats.nMempoolTXs - right_stats.nMempoolTXsSnapOld) / (now - left_stats.nTimeSnapOld);
        }
        return Left < Right;
    }
    case PeerTableModel::CpuTime: {
        int64_t now = GetTimeSeconds();
        if (left_stats.m_cpu_time_snap_old.count() > 0 && right_stats.m_cpu_time_snap_old.count() > 0 &&
            left_stats.nTimeSnapOld != now && right_stats.nTimeSnapOld != now) {
            // Compare CPU time per second using snapshots
            double left_cpu_diff = (left_stats.m_cpu_time_snap - left_stats.m_cpu_time_snap_old).count() / 1e9;
            double right_cpu_diff = (right_stats.m_cpu_time_snap - right_stats.m_cpu_time_snap_old).count() / 1e9;
            double left_time_diff = now - left_stats.nTimeSnapOld;
            double right_time_diff = now - right_stats.nTimeSnapOld;
            if (left_time_diff > 0 && right_time_diff > 0) {
                double left_cpu_per_sec = left_cpu_diff / left_time_diff;
                double right_cpu_per_sec = right_cpu_diff / right_time_diff;
                return left_cpu_per_sec < right_cpu_per_sec;
            }
        }
        // Fallback to total CPU time comparison
        return left_stats.m_cpu_time < right_stats.m_cpu_time;
    }
    case PeerTableModel::Subversion:
        return left_stats.cleanSubVer.compare(right_stats.cleanSubVer) < 0;
    } // no default case, so the compiler can warn about missing cases
    assert(false);
}
