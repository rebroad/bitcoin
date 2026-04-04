// Copyright (c) 2011-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/peertablemodel.h>

#include <qt/geoipresolver.h>
#include <qt/guiconstants.h>
#include <qt/guiutil.h>

#include <interfaces/node.h>
#include <sync.h>
#include <util/strencodings.h>

#include <algorithm>
#include <memory>

#include <QtConcurrent/QtConcurrentRun>
#include <QFutureWatcher>
#include <QList>
#include <QTimer>

class CountryFlagResolver
{
public:
    QString TooltipForPeerAddress(const std::string& peer_addr)
    {
        return m_resolver.ResolveAddress(peer_addr, /*is_subnet_text=*/false).tooltip;
    }

    QString FlagForPeerAddress(const std::string& peer_addr)
    {
        return m_resolver.ResolveAddress(peer_addr, /*is_subnet_text=*/false).flag;
    }

    QString GeoIpStatusSummary()
    {
        return m_resolver.GeoIpStatusSummary();
    }

    bool GeoIpNeedsAttention()
    {
        return m_resolver.GeoIpNeedsAttention();
    }

private:
    GeoIpResolver m_resolver{};
};

static constexpr auto PEER_MODEL_UPDATE_DELAY{500ms};

PeerTableModel::PeerTableModel(interfaces::Node& node, QObject* parent) :
    QAbstractTableModel(parent),
    m_node(node),
    m_country_flag_resolver(std::make_unique<CountryFlagResolver>()),
    timer(nullptr)
{
    // set up timer for auto refresh
    timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &PeerTableModel::refresh);
    timer->setInterval(PEER_MODEL_UPDATE_DELAY);
    m_refresh_watcher = new QFutureWatcher<QList<CNodeCombinedStats>>(this);
    connect(m_refresh_watcher, &QFutureWatcher<QList<CNodeCombinedStats>>::finished,
            this, &PeerTableModel::onRefreshFinished);

    // load initial data
    refresh();
}

PeerTableModel::~PeerTableModel()
{
    // Intentionally left empty
}

void PeerTableModel::startAutoRefresh()
{
    timer->start();
}

void PeerTableModel::stopAutoRefresh()
{
    timer->stop();
}

QString PeerTableModel::geoIpStatusSummary() const
{
    return m_country_flag_resolver->GeoIpStatusSummary();
}

bool PeerTableModel::geoIpNeedsAttention() const
{
    return m_country_flag_resolver->GeoIpNeedsAttention();
}

int PeerTableModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return m_peers_data.size();
}

int PeerTableModel::columnCount(const QModelIndex& parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return columns.length();
}

QVariant PeerTableModel::data(const QModelIndex& index, int role) const
{
    if(!index.isValid())
        return QVariant();

    CNodeCombinedStats *rec = static_cast<CNodeCombinedStats*>(index.internalPointer());

    const auto column = static_cast<ColumnIndex>(index.column());
    if (role == Qt::DisplayRole) {
        switch (column) {
        case NetNodeId:
            return (qint64)rec->nodeStats.nodeid;
        case Address:
            return QString::fromStdString(rec->nodeStats.m_addr_name);
        case Direction:
            return rec->nodeStats.fInbound
                ? QString("%1 %2")
                      .arg(
                          //: An Inbound Connection from a Peer.
                          tr("↓"),
                          m_country_flag_resolver->FlagForPeerAddress(rec->nodeStats.m_addr_name))
                      .trimmed()
                : QString("%1 %2")
                      .arg(
                          //: An Outbound Connection to a Peer.
                          tr("↑"),
                          m_country_flag_resolver->FlagForPeerAddress(rec->nodeStats.m_addr_name))
                      .trimmed();
        //case ConnectionType:
        //    return GUIUtil::ConnectionTypeToQString(rec->nodeStats.m_conn_type, rec->nodeStats.fErlay, /* prepend_direction */ false);
        //case Network:
        //    return GUIUtil::NetworkToQString(rec->nodeStats.m_network);
        case Ping:
            return GUIUtil::formatPingTime(rec->nodeStats.m_min_ping_time);
        case Sent: {
            int64_t now = GetTimeSeconds();
            if (now != count_seconds(rec->nodeStats.m_connected)) // Avoid division by zero
                return GUIUtil::formatBps(rec->nodeStats.nSendBytes * 8.0 / (now - count_seconds(rec->nodeStats.m_connected)));
            else
                return QString::fromStdString("");
        }
        case Recv: {
            int64_t now = GetTimeSeconds();
            if (rec->nodeStats.nRecvBytesSnapOld && rec->nodeStats.nTimeSnapOld != now)
                return GUIUtil::formatBps((rec->nodeStats.nRecvBytes - rec->nodeStats.nRecvBytesSnapOld) * 8.0 / (now - rec->nodeStats.nTimeSnapOld));
            else if (now != count_seconds(rec->nodeStats.m_connected))
                return GUIUtil::formatBps(rec->nodeStats.nRecvBytes * 8.0 / (now - count_seconds(rec->nodeStats.m_connected)));
            else
                return QString::fromStdString("");
        }
        case TxBpsPct: {
            int64_t now = GetTimeSeconds();
            std::string dots;
            if (now - count_seconds(rec->nodeStats.m_connected) >= 120) dots="";
            else if (now - count_seconds(rec->nodeStats.m_connected) >= 60) dots=".";
            else dots="..";
            const uint64_t recv_bytes_base{rec->nodeStats.nRecvBytesSnapOld};
            const uint64_t send_bytes_base{rec->nodeStats.nSendBytesSnapOld};
            const uint64_t mempool_bytes_base{rec->nodeStats.nMempoolBytesSnapOld};
            const uint64_t recv_bytes{rec->nodeStats.nRecvBytes > recv_bytes_base ? rec->nodeStats.nRecvBytes - recv_bytes_base : rec->nodeStats.nRecvBytes};
            const uint64_t send_bytes{rec->nodeStats.nSendBytes > send_bytes_base ? rec->nodeStats.nSendBytes - send_bytes_base : rec->nodeStats.nSendBytes};
            const uint64_t mempool_bytes{rec->nodeStats.nMempoolBytes > mempool_bytes_base ? rec->nodeStats.nMempoolBytes - mempool_bytes_base : rec->nodeStats.nMempoolBytes};
            if ((recv_bytes + send_bytes) > 0) {
                int nTxBpsPct = int((100.0 * mempool_bytes / (recv_bytes + send_bytes)) + 0.5);
                int nBTxBpsPct = int(rec->nodeStats.nBTxBpsPct + 0.5);
                return QString::fromStdString(strprintf("%s%d%s", dots, nTxBpsPct, nBTxBpsPct ? strprintf("+%d", nBTxBpsPct) : ""));
            } else
                return QString::fromStdString(dots);
        }
        case MPpm: {
            int64_t now = GetTimeSeconds();
            const unsigned int mempool_txs_base{rec->nodeStats.nMempoolTXsSnapOld};
            const int64_t time_base{rec->nodeStats.nTimeSnapOld > 0 ? rec->nodeStats.nTimeSnapOld : count_seconds(rec->nodeStats.m_connected)};
            if (now > time_base) {
                const unsigned int mempool_txs{rec->nodeStats.nMempoolTXs > mempool_txs_base ? rec->nodeStats.nMempoolTXs - mempool_txs_base : rec->nodeStats.nMempoolTXs};
                float nMPpm = 60.0 * mempool_txs / (now - time_base);
                float nBTxpm = rec->nodeStats.nBTXpm;
                std::string strMPpm; std::string strBTpm;
                if (nMPpm < 1) strMPpm = strprintf("%d", 0.1 * (int)(nMPpm * 10));
                else strMPpm = strprintf("%d", (int)nMPpm);
                if (nBTxpm < 1) strBTpm = strprintf("%d", 0.1 * (int)(nBTxpm * 10));
                else strBTpm = strprintf("%d", (int)nBTxpm);
                return QString::fromStdString(strprintf("%s%s", strMPpm, nBTxpm ? strprintf("+%s", strBTpm) : ""));
            } else
                return {};
        }
        case Subversion:
            return QString::fromStdString(rec->nodeStats.cleanSubVer);
        } // no default case, so the compiler can warn about missing cases
        assert(false);
    } else if (role == Qt::TextAlignmentRole) {
        switch (column) {
        case NetNodeId:
        case Address:
        case Direction:
        //case ConnectionType:
        //case Network:
        case Ping:
        case Sent:
        case Recv:
        case TxBpsPct:
        case MPpm:
            return QVariant(Qt::AlignCenter);
        case Subversion:
            return QVariant(Qt::AlignLeft | Qt::AlignVCenter);
        } // no default case, so the compiler can warn about missing cases
        assert(false);
    } else if (role == StatsRole) {
        return QVariant::fromValue(rec);
    } else if (role == Qt::ToolTipRole) {
        switch (column) {
        case Direction:
            return m_country_flag_resolver->TooltipForPeerAddress(rec->nodeStats.m_addr_name);
        default:
            return {};
        }
    }

    return QVariant();
}

QVariant PeerTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if(orientation == Qt::Horizontal)
    {
        if(role == Qt::DisplayRole && section < columns.size())
            return columns[section];
        if (static_cast<ColumnIndex>(section) == Subversion)
            return QVariant(Qt::AlignLeft | Qt::AlignVCenter);
    }
    return QVariant();
}

Qt::ItemFlags PeerTableModel::flags(const QModelIndex &index) const
{
    if (!index.isValid()) return Qt::NoItemFlags;

    Qt::ItemFlags retval = Qt::ItemIsSelectable | Qt::ItemIsEnabled;
    return retval;
}

QModelIndex PeerTableModel::index(int row, int column, const QModelIndex& parent) const
{
    Q_UNUSED(parent);

    if (0 <= row && row < rowCount() && 0 <= column && column < columnCount()) {
        return createIndex(row, column, const_cast<CNodeCombinedStats*>(&m_peers_data[row]));
    }

    return QModelIndex();
}

void PeerTableModel::refresh()
{
    if (m_refresh_in_flight) {
        m_refresh_pending = true;
        return;
    }
    m_refresh_in_flight = true;
    interfaces::Node* node = &m_node;
    m_refresh_watcher->setFuture(QtConcurrent::run([node]() {
        interfaces::Node::NodesStats nodes_stats;
        node->getNodesStats(nodes_stats);
        QList<CNodeCombinedStats> peers_data;
        peers_data.reserve(nodes_stats.size());
        for (const auto& node_stats : nodes_stats) {
            peers_data.append(CNodeCombinedStats{std::get<0>(node_stats), std::get<2>(node_stats), std::get<1>(node_stats)});
        }
        return peers_data;
    }));
}

void PeerTableModel::onRefreshFinished()
{
    applyPeerStats(m_refresh_watcher->result());
    m_refresh_in_flight = false;
    if (m_refresh_pending) {
        m_refresh_pending = false;
        QTimer::singleShot(0, this, &PeerTableModel::refresh);
    }
}

void PeerTableModel::applyPeerStats(const QList<CNodeCombinedStats>& peers_data)
{
    decltype(m_peers_data) new_peers_data;
    new_peers_data.reserve(peers_data.size());
    for (const auto& stats : peers_data) new_peers_data.append(stats);

    // Handle peer addition or removal as suggested in Qt Docs. See:
    // - https://doc.qt.io/qt-5/model-view-programming.html#inserting-and-removing-rows
    // - https://doc.qt.io/qt-5/model-view-programming.html#resizable-models
    // We take advantage of the fact that the std::vector returned
    // by interfaces::Node::getNodesStats is sorted by nodeid.
    for (int i = 0; i < m_peers_data.size();) {
        if (i < new_peers_data.size() && m_peers_data.at(i).nodeStats.nodeid == new_peers_data.at(i).nodeStats.nodeid) {
            ++i;
            continue;
        }
        // A peer has been removed from the table.
        beginRemoveRows(QModelIndex(), i, i);
        m_peers_data.erase(m_peers_data.begin() + i);
        endRemoveRows();
    }

    if (m_peers_data.size() < new_peers_data.size()) {
        // Some peers have been added to the end of the table.
        beginInsertRows(QModelIndex(), m_peers_data.size(), new_peers_data.size() - 1);
        m_peers_data.swap(new_peers_data);
        endInsertRows();
    } else {
        m_peers_data.swap(new_peers_data);
    }

    const auto top_left = index(0, 0);
    const auto bottom_right = index(rowCount() - 1, columnCount() - 1);
    // Only emit dataChanged if both indices are valid
    if (top_left.isValid() && bottom_right.isValid())
        Q_EMIT dataChanged(top_left, bottom_right);
}
