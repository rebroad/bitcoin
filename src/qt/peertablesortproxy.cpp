// Copyright (c) 2020 The Bitcoin Core developers
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

    int RightSendBps = right_stats.nSendBytes * 8 / (right_stats.nLastSend + 1 - right_stats.nTimeConnected);
    int LeftSendBps = left_stats.nSendBytes * 8 / (left_stats.nLastSend + 1 - left_stats.nTimeConnected);
    int RightRecvBps = right_stats.nRecvBytes * 8 / (right_stats.nLastRecv + 1 - right_stats.nTimeConnected);
    int LeftRecvBps = left_stats.nRecvBytes * 8 / (left_stats.nLastRecv + 1 - left_stats.nTimeConnected);
    int RightMempoolPct = 100 * right_stats.nMempoolBytes / (right_stats.nRecvBytes - right_stats.nRecvBytes1stTx + 1);
    int LeftMempoolPct = 100 * left_stats.nMempoolBytes / (left_stats.nRecvBytes - left_stats.nRecvBytes1stTx + 1);

    switch (static_cast<PeerTableModel::ColumnIndex>(left_index.column())) {
    case PeerTableModel::NetNodeId:
        return left_stats.nodeid < right_stats.nodeid;
    case PeerTableModel::Address:
        return left_stats.addrName.compare(right_stats.addrName) < 0;
    case PeerTableModel::ConnectionType:
        return left_stats.m_conn_type < right_stats.m_conn_type;
    case PeerTableModel::Network:
        return left_stats.m_network < right_stats.m_network;
    case PeerTableModel::Ping:
        return left_stats.m_min_ping_time < right_stats.m_min_ping_time;
    case PeerTableModel::Sent:
        return LeftSendBps < RightSendBps;
    case PeerTableModel::Recv:
        return LeftRecvBps < RightRecvBps;
    case PeerTableModel::TxRecv:
        return LeftMempoolPct < RightMempoolPct;
    case PeerTableModel::Subversion:
        return left_stats.cleanSubVer.compare(right_stats.cleanSubVer) < 0;
    } // no default case, so the compiler can warn about missing cases
    assert(false);
}
