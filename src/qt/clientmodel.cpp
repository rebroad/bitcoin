// Copyright (c) 2011-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/clientmodel.h>

#include <qt/bantablemodel.h>
#include <qt/guiconstants.h>
#include <qt/guiutil.h>
#include <qt/peertablemodel.h>
#include <qt/peertablesortproxy.h>

#include <clientversion.h>
#include <interfaces/handler.h>
#include <interfaces/node.h>
#include <memusage.h>
#include <net.h>
#include <netbase.h>
#include <util/system.h>
#include <util/threadnames.h>
#include <util/time.h>
#include <validation.h>

#include <stdint.h>
#include <functional>

#include <QDebug>
#include <QMutexLocker>
#include <QThread>
#include <QTimer>

static int64_t nLastHeaderTipUpdateNotification = 0;
static int64_t nLastBlockTipUpdateNotification = 0;

ClientModel::ClientModel(interfaces::Node& node, OptionsModel *_optionsModel, QObject *parent) :
    QObject(parent),
    m_node(node),
    optionsModel(_optionsModel),
    peerTableModel(nullptr),
    banTableModel(nullptr)
{
    peerTableModel = new PeerTableModel(m_node, this);
    m_peer_table_sort_proxy = new PeerTableSortProxy(this);
    m_peer_table_sort_proxy->setSourceModel(peerTableModel);

    banTableModel = new BanTableModel(m_node, this);

    // Initialize data immediately
    initializeData();

    subscribeToCoreSignals();
}

void ClientModel::initializeData()
{
    // Initialize data with current values (only called once at startup)
    // Most data will be updated via signals, this is just for initial state
    m_gui_data.numBlocks = m_node.getNumBlocks();
    m_gui_data.bestBlockHash = m_node.getBestBlockHash();
    m_gui_data.initialSyncFinished = m_node.isInitialSyncFinished();
    m_gui_data.numConnectionsIn = m_node.getNodeCount(ConnectionDirection::In);
    m_gui_data.numConnectionsOut = m_node.getNodeCount(ConnectionDirection::Out);
    m_gui_data.numConnectionsTotal = m_node.getNodeCount(ConnectionDirection::Both);
    m_gui_data.mempoolSize = m_node.getMempoolSize();
    m_gui_data.mempoolDynamicUsage = m_node.getMempoolDynamicUsage();
    m_gui_data.bytesRecv = m_node.getTotalBytesRecv();
    m_gui_data.bytesSent = m_node.getTotalBytesSent();

    // Initialize header data
    int height;
    int64_t blockTime;
    if (m_node.getHeaderTip(height, blockTime)) {
        m_gui_data.headerHeight = height;
        m_gui_data.headerTime = blockTime;
    } else {
        m_gui_data.headerHeight = -1;
        m_gui_data.headerTime = -1;
    }

    qDebug() << "ClientModel: Data initialized with" << m_gui_data.numBlocks << "blocks";
}

ClientModel::~ClientModel()
{
    unsubscribeFromCoreSignals();
}

int ClientModel::getNumConnections(unsigned int flags) const
{
    // Use single buffer - no lock needed (signal-based updates)
    if (flags == CONNECTIONS_IN) {
        return m_gui_data.numConnectionsIn;
    } else if (flags == CONNECTIONS_OUT) {
        return m_gui_data.numConnectionsOut;
    } else {
        // For CONNECTIONS_ALL or any other combination, return total (most common case)
        return m_gui_data.numConnectionsTotal;
    }
}

int ClientModel::getHeaderTipHeight() const
{
    // Use single buffer - no lock needed (signal-based updates)
    return m_gui_data.headerHeight;
}

int64_t ClientModel::getHeaderTipTime() const
{
    // Use single buffer - no lock needed (signal-based updates)
    return m_gui_data.headerTime;
}

int ClientModel::getNumBlocks() const
{
    // Use single buffer - no lock needed (signal-based updates)
    return m_gui_data.numBlocks;
}

uint256 ClientModel::getBestBlockHash()
{
    // Use single buffer - no lock needed (signal-based updates)
    return m_gui_data.bestBlockHash;
}

void ClientModel::updateNumConnections(int numConnections)
{
    Q_EMIT numConnectionsChanged(numConnections);
}

void ClientModel::updateNetworkActive(bool networkActive)
{
    Q_EMIT networkActiveChanged(networkActive);
}

void ClientModel::updateAlert()
{
    Q_EMIT alertsChanged(getStatusBarWarnings());
}

enum BlockSource ClientModel::getBlockSource() const
{
    if (m_node.getReindex()) return BlockSource::REINDEX;
    if (m_node.getImporting()) return BlockSource::DISK;
    if (getNumConnections() > 0) return BlockSource::NETWORK;
    return BlockSource::NONE;
}

QString ClientModel::getStatusBarWarnings() const
{
    return QString::fromStdString(m_node.getWarnings().translated);
}

OptionsModel *ClientModel::getOptionsModel()
{
    return optionsModel;
}

PeerTableModel *ClientModel::getPeerTableModel()
{
    return peerTableModel;
}

PeerTableSortProxy* ClientModel::peerTableSortProxy()
{
    return m_peer_table_sort_proxy;
}

BanTableModel *ClientModel::getBanTableModel()
{
    return banTableModel;
}

QString ClientModel::formatFullVersion() const
{
    return QString::fromStdString(FormatFullVersion());
}

QString ClientModel::formatSubVersion() const
{
    return QString::fromStdString(strSubVersion);
}

bool ClientModel::isReleaseVersion() const
{
    return CLIENT_VERSION_IS_RELEASE;
}

QString ClientModel::formatClientStartupTime() const
{
    return QDateTime::fromSecsSinceEpoch(GetStartupTime()).toString();
}

QString ClientModel::dataDir() const
{
    return GUIUtil::PathToQString(gArgs.GetDataDirNet());
}

QString ClientModel::blocksDir() const
{
    return GUIUtil::PathToQString(gArgs.GetBlocksDirPath());
}

void ClientModel::updateBanlist()
{
    banTableModel->refresh();
}

// Handlers for core signals
static void ShowProgress(ClientModel *clientmodel, const std::string &title, int nProgress)
{
    // emits signal "showProgress"
    bool invoked = QMetaObject::invokeMethod(clientmodel, "showProgress", Qt::QueuedConnection,
                              Q_ARG(QString, QString::fromStdString(title)),
                              Q_ARG(int, nProgress));
    assert(invoked);
}

static void NotifyNumConnectionsChanged(ClientModel *clientmodel, int newNumConnections)
{
    // Too noisy: qDebug() << "NotifyNumConnectionsChanged: " + QString::number(newNumConnections);
    bool invoked = QMetaObject::invokeMethod(clientmodel, "updateNumConnections", Qt::QueuedConnection,
                              Q_ARG(int, newNumConnections));
    assert(invoked);
}

static void NotifyNetworkActiveChanged(ClientModel *clientmodel, bool networkActive)
{
    bool invoked = QMetaObject::invokeMethod(clientmodel, "updateNetworkActive", Qt::QueuedConnection,
                              Q_ARG(bool, networkActive));
    assert(invoked);
}

static void NotifyAlertChanged(ClientModel *clientmodel)
{
    qDebug() << "NotifyAlertChanged";
    bool invoked = QMetaObject::invokeMethod(clientmodel, "updateAlert", Qt::QueuedConnection);
    assert(invoked);
}

static void BannedListChanged(ClientModel *clientmodel)
{
    qDebug() << QString("%1: Requesting update for peer banlist").arg(__func__);
    bool invoked = QMetaObject::invokeMethod(clientmodel, "updateBanlist", Qt::QueuedConnection);
    assert(invoked);
}

static void BlockTipChanged(ClientModel* clientmodel, SynchronizationState sync_state, interfaces::BlockTip tip, double verificationProgress, bool fHeader)
{
    if (fHeader) {
        // Queue header tip update to GUI thread for thread safety
        // This ensures cache updates happen in the correct thread context
        bool invoked = QMetaObject::invokeMethod(clientmodel, "updateHeaderTip", Qt::QueuedConnection,
            Q_ARG(int, tip.block_height),
            Q_ARG(int64_t, tip.block_time));
        if (!invoked) {
            qWarning() << "ClientModel: Failed to queue header tip update";
        }
    } else {
        // Update block data via signal
        bool invoked = QMetaObject::invokeMethod(clientmodel, "updateBlockData", Qt::QueuedConnection,
            Q_ARG(int, tip.block_height),
            Q_ARG(uint256, tip.block_hash),
            Q_ARG(bool, false)); // Will be updated via signal later
        if (!invoked) {
            qWarning() << "ClientModel: Failed to queue block data update";
        }
    }

    // Throttle GUI notifications about (a) blocks during initial sync, and (b) both blocks and headers during reindex.
    const bool throttle = (sync_state != SynchronizationState::POST_INIT && !fHeader) || sync_state == SynchronizationState::INIT_REINDEX;
    const int64_t now = throttle ? GetTimeMillis() : 0;
    int64_t& nLastUpdateNotification = fHeader ? nLastHeaderTipUpdateNotification : nLastBlockTipUpdateNotification;
    if (throttle && now < nLastUpdateNotification + count_milliseconds(MODEL_UPDATE_DELAY)) {
        return;
    }

    bool invoked = QMetaObject::invokeMethod(clientmodel, "numBlocksChanged", Qt::QueuedConnection,
        Q_ARG(int, tip.block_height),
        Q_ARG(QDateTime, QDateTime::fromSecsSinceEpoch(tip.block_time)),
        Q_ARG(double, verificationProgress),
        Q_ARG(bool, fHeader),
        Q_ARG(SynchronizationState, sync_state));
    assert(invoked);
    nLastUpdateNotification = now;
}

static void MempoolStatsDidChange(ClientModel *clientmodel)
{
    QMetaObject::invokeMethod(clientmodel, "updateMempoolStats", Qt::QueuedConnection);
}

void ClientModel::subscribeToCoreSignals()
{
    // Connect signals to client
    m_handler_show_progress = m_node.handleShowProgress(std::bind(ShowProgress, this, std::placeholders::_1, std::placeholders::_2));
    m_handler_notify_num_connections_changed = m_node.handleNotifyNumConnectionsChanged(std::bind(NotifyNumConnectionsChanged, this, std::placeholders::_1));
    m_handler_notify_network_active_changed = m_node.handleNotifyNetworkActiveChanged(std::bind(NotifyNetworkActiveChanged, this, std::placeholders::_1));
    m_handler_notify_alert_changed = m_node.handleNotifyAlertChanged(std::bind(NotifyAlertChanged, this));
    m_handler_banned_list_changed = m_node.handleBannedListChanged(std::bind(BannedListChanged, this));
    m_handler_notify_block_tip = m_node.handleNotifyBlockTip(std::bind(BlockTipChanged, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, false));
    m_handler_notify_header_tip = m_node.handleNotifyHeaderTip(std::bind(BlockTipChanged, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, true));

    m_connection_mempool_stats_did_change = CStats::DefaultStats()->MempoolStatsDidChange.connect(std::bind(MempoolStatsDidChange, this));
}

void ClientModel::unsubscribeFromCoreSignals()
{
    // Disconnect signals from client
    m_handler_show_progress->disconnect();
    m_handler_notify_num_connections_changed->disconnect();
    m_handler_notify_network_active_changed->disconnect();
    m_handler_notify_alert_changed->disconnect();
    m_handler_banned_list_changed->disconnect();
    m_handler_notify_block_tip->disconnect();
    m_handler_notify_header_tip->disconnect();

    m_connection_mempool_stats_did_change.disconnect();
}

bool ClientModel::getProxyInfo(std::string& ip_port) const
{
    proxyType ipv4, ipv6;
    if (m_node.getProxy((Network) 1, ipv4) && m_node.getProxy((Network) 2, ipv6)) {
      ip_port = ipv4.proxy.ToStringIPPort();
      return true;
    }
    return false;
}

mempoolSamples_t ClientModel::getMempoolStatsInRange(QDateTime &from, QDateTime &to)
{
    // get stats from the core stats model
    uint64_t timeFrom = from.toTime_t();
    uint64_t timeTo = to.toTime_t();

    mempoolSamples_t samples = CStats::DefaultStats()->mempoolGetValuesInRange(timeFrom,timeTo);
    from.setTime_t(timeFrom);
    to.setTime_t(timeTo);
    return samples;
}

void ClientModel::updateMempoolStats()
{
    Q_EMIT mempoolStatsDidUpdate();
}

void ClientModel::updateHeaderTip(int height, int64_t blockTime)
{
    // Update header tip in the GUI thread context
    // This is called from the validation thread via QMetaObject::invokeMethod
    m_gui_data.headerHeight = height;
    m_gui_data.headerTime = blockTime;
}

void ClientModel::updateBlockData(int numBlocks, const uint256& bestBlockHash, bool initialSyncFinished)
{
    // Update block data in the GUI thread context
    m_gui_data.numBlocks = numBlocks;
    m_gui_data.bestBlockHash = bestBlockHash;
    m_gui_data.initialSyncFinished = initialSyncFinished;
}

void ClientModel::updateConnectionData(int connectionsIn, int connectionsOut, int connectionsTotal)
{
    // Update connection data in the GUI thread context
    m_gui_data.numConnectionsIn = connectionsIn;
    m_gui_data.numConnectionsOut = connectionsOut;
    m_gui_data.numConnectionsTotal = connectionsTotal;
}

void ClientModel::updateNetworkData(int64_t bytesRecv, int64_t bytesSent)
{
    // Update network data in the GUI thread context
    m_gui_data.bytesRecv = bytesRecv;
    m_gui_data.bytesSent = bytesSent;
}

void ClientModel::updatePeerStats(const interfaces::Node::NodesStats& stats)
{
    // Update peer stats in the GUI thread context
    m_gui_data.peerStats = stats;
    m_gui_data.lastPeerUpdateTime = GetTime();
}

void ClientModel::updateFeeHistogram(const interfaces::mempool_feehistogram& histogram)
{
    // Update fee histogram in the GUI thread context
    m_gui_data.feeHistogram = histogram;
    m_gui_data.lastFeeHistogramUpdateTime = GetTime();
}

QString ClientModel::getCacheStats() const
{
    // Use single buffer - no lock needed (signal-based updates)
    int64_t now = GetTime();
    int64_t age = now - m_gui_data.lastUpdateTime;

    QString stats = QString("Signal-Based Data Stats:\n"
                           "  Updates: %1\n"
                           "  Last Update: %2 seconds ago\n"
                           "  Blocks: %3\n"
                           "  Header Height: %4\n"
                           "  Connections: %5 in, %6 out, %7 total\n"
                           "  Mempool: %8 txs, %9 bytes\n"
                           "  Network: %10 bytes in, %11 bytes out\n"
                           "  Peers: %12 cached, last update: %13 seconds ago\n"
                           "  Fee Histogram: %14 ranges, last update: %15 seconds ago")
                           .arg(m_gui_data.updateCount)
                           .arg(age)
                           .arg(m_gui_data.numBlocks)
                           .arg(m_gui_data.headerHeight)
                           .arg(m_gui_data.numConnectionsIn)
                           .arg(m_gui_data.numConnectionsOut)
                           .arg(m_gui_data.numConnectionsTotal)
                           .arg(m_gui_data.mempoolSize)
                           .arg(m_gui_data.mempoolDynamicUsage)
                           .arg(m_gui_data.bytesRecv)
                           .arg(m_gui_data.bytesSent)
                           .arg(m_gui_data.peerStats.size())
                           .arg(now - m_gui_data.lastPeerUpdateTime)
                           .arg(m_gui_data.feeHistogram.size())
                           .arg(now - m_gui_data.lastFeeHistogramUpdateTime);

    return stats;
}

int64_t ClientModel::getCachedBytesRecv() const
{
    // Use single buffer - no lock needed (signal-based updates)
    return m_gui_data.bytesRecv;
}

int64_t ClientModel::getCachedBytesSent() const
{
    // Use single buffer - no lock needed (signal-based updates)
    return m_gui_data.bytesSent;
}

bool ClientModel::getCachedPeerStats(interfaces::Node::NodesStats& stats) const
{
    // Use single buffer - no lock needed (signal-based updates)
    stats = m_gui_data.peerStats;
    return true;
}

bool ClientModel::getCachedFeeHistogram(interfaces::mempool_feehistogram& histogram) const
{
    // Use single buffer - no lock needed (signal-based updates)
    histogram = m_gui_data.feeHistogram;
    return true;
}
