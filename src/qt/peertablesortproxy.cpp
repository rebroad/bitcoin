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
    const CNodeStats left_stats = Assert(sourceModel()->data(left_index, PeerTableModel::StatsRole).value<CNodeCombinedStats*>())->nodeStats;
    const CNodeStats right_stats = Assert(sourceModel()->data(right_index, PeerTableModel::StatsRole).value<CNodeCombinedStats*>())->nodeStats;

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
        if (right_stats.nRecvBytes1stTx)
            Right = ((right_stats.nRecvBytes - right_stats.nRecvBytes1stTx) * 8 / (now + 1 - right_stats.nTime1stTx));
        else
            Right = right_stats.nRecvBytes * 8 / (now + 1 - count_seconds(right_stats.m_connected));
        if (left_stats.nRecvBytes1stTx)
            Left = ((left_stats.nRecvBytes - left_stats.nRecvBytes1stTx) * 8 / (now + 1 - left_stats.nTime1stTx));
        else
            Left = left_stats.nRecvBytes * 8 / (now + 1 - count_seconds(left_stats.m_connected));
        return Left < Right;
    }
    /*case PeerTableModel::TxBps: {
        int64_t now = GetTimeSeconds();
        double Right = 1.0 * right_stats.nMempoolBytes / (now + 1 - right_stats.nTime1stTx);
        double Left = 1.0 * left_stats.nMempoolBytes / (now + 1 - left_stats.nTime1stTx);
        return Left < Right;
    } */
    case PeerTableModel::TxBpsPct: {
        double Right; double Left;
        if (right_stats.nBlockTXs && left_stats.nBlockTXs) {
            Right = 1.0 * right_stats.nBlockBytes / (right_stats.nRecvBytes + 1 - right_stats.nRecvBytes1stTx);
            Left = 1.0 * left_stats.nBlockBytes / (left_stats.nRecvBytes + 1 - left_stats.nRecvBytes1stTx);
        } else {
            Right = 1.0 * right_stats.nMempoolBytes / (right_stats.nRecvBytes + 1 - right_stats.nRecvBytes1stTx);
            Left = 1.0 * left_stats.nMempoolBytes / (left_stats.nRecvBytes + 1 - left_stats.nRecvBytes1stTx);
        }
        return Left < Right;
    }
    case PeerTableModel::MPpm: {
        int64_t now = GetTimeSeconds();
        double Right; double Left;
        if (right_stats.nBlockTXs && left_stats.nBlockTXs) {
            Right = 1.0 * right_stats.nBlockTXs / (now + 1 - right_stats.nTime1stTx);
            Left = 1.0 * left_stats.nBlockTXs / (now + 1 - left_stats.nTime1stTx);
        } else {
            Right = 1.0 * right_stats.nMempoolTXs / (now + 1 - right_stats.nTime1stTx);
            Left = 1.0 * left_stats.nMempoolTXs / (now + 1 - left_stats.nTime1stTx);
        }
        return Left < Right;
    }
    case PeerTableModel::Subversion:
        return left_stats.cleanSubVer.compare(right_stats.cleanSubVer) < 0;
    } // no default case, so the compiler can warn about missing cases
    assert(false);
}
