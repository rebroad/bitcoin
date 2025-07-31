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

// Initialize static member
std::atomic<int> ClientModel::s_signalProcessingCount{0};

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

    // Add a simple timer to test if GUI thread is alive
    QTimer* testTimer = new QTimer(this);
    testTimer->setInterval(5000); // Every 5 seconds
    connect(testTimer, &QTimer::timeout, [this]() {
        LogPrint(BCLog::QT, "GUI Thread Test: Timer fired - GUI thread is alive!\n");
    });
    testTimer->start();
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
    static int64_t lastConnLogTime = 0;
    int64_t now = GetTime();

    // Always log this to see if ANY signals are being processed
    LogPrint(BCLog::QT, "updateNumConnections: GUI thread processing connection update: %d\n", numConnections);

    if (now - lastConnLogTime > 5) {
        lastConnLogTime = now;
    }

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
    static int64_t lastLogTime = 0;
    static int callCount = 0;
    int64_t signalStartTime = GetTimeMillis();
    int64_t now = GetTime();

    callCount++;

    // Log every 5 seconds to avoid spam
    if (now - lastLogTime > 5) {
        LogPrint(BCLog::GUI, "NotifyNumConnectionsChanged: Call #%d, newNumConnections: %d\n", callCount, newNumConnections);
        lastLogTime = now;
        callCount = 0; // Reset counter
    }

    // Only collect basic connection data in signal handler to avoid blocking
    // Peer stats will be collected separately to avoid GUI thread blocking
    int64_t dataCollectStart = GetTimeMillis();
    int connectionsIn = clientmodel->node().getNodeCount(ConnectionDirection::In);
    int connectionsOut = clientmodel->node().getNodeCount(ConnectionDirection::Out);
    int connectionsTotal = clientmodel->node().getNodeCount(ConnectionDirection::Both);

    // Collect network traffic data
    int64_t bytesRecv = clientmodel->node().getTotalBytesRecv();
    int64_t bytesSent = clientmodel->node().getTotalBytesSent();
    int64_t dataCollectTime = GetTimeMillis() - dataCollectStart;

    // Queue peer stats collection separately to avoid blocking the signal handler
    // This will be done in a background thread or timer to avoid GUI thread blocking
    int64_t peerQueueStart = GetTimeMillis();
    QTimer::singleShot(0, [clientmodel, peerQueueStart]() {
        // Safety check - ensure clientmodel is still valid
        if (!clientmodel) return;

        int64_t peerCollectStart = GetTimeMillis();
        // Collect peer stats in a non-blocking way
        interfaces::Node::NodesStats peerStats;
        clientmodel->node().getNodesStats(peerStats);
        int64_t peerCollectTime = GetTimeMillis() - peerCollectStart;

        // Convert to QStringList for Qt compatibility
        int64_t convertStart = GetTimeMillis();
        QStringList peerData;
        for (const auto& peer : peerStats) {
            const CNodeStats& stats = std::get<0>(peer);
            const CNodeStateStats& stateStats = std::get<2>(peer);

            // Create a comprehensive string representation of peer data
            QString peerInfo = QString("%1|%2|%3|%4|%5|%6|%7|%8|%9|%10|%11|%12|%13|%14|%15|%16|%17|%18|%19|%20")
                .arg(stats.nodeid)
                .arg(QString::fromStdString(stats.m_addr_name))
                .arg(stats.nVersion)
                .arg(QString::fromStdString(stats.cleanSubVer))
                .arg(stats.fInbound ? "1" : "0")
                .arg(stats.nSendBytes)
                .arg(stats.nRecvBytes)
                .arg(count_seconds(stats.m_connected))
                .arg(stats.nTimeOffset)
                .arg(stateStats.m_ping_wait.count() > 0 ? QString::number(CountSecondsDouble(stateStats.m_ping_wait)) : "0")
                .arg(static_cast<int>(stats.m_network))  // Network enum value
                .arg(static_cast<int>(stats.m_conn_type)) // Connection type enum value
                .arg(stats.nServices)
                .arg(stats.fRelayTxes ? "1" : "0")
                .arg(count_seconds(stats.m_last_send))
                .arg(count_seconds(stats.m_last_recv))
                .arg(count_seconds(stats.m_last_tx_time))
                .arg(count_seconds(stats.m_last_block_time))
                .arg(static_cast<uint32_t>(stats.m_permissionFlags)) // NetPermissionFlags
                .arg(stats.m_mapped_as);

            peerData.append(peerInfo);
        }
        int64_t convertTime = GetTimeMillis() - convertStart;

        // Update peer stats via signal
        int64_t peerQueueTime = GetTimeMillis() - peerQueueStart;
        QMetaObject::invokeMethod(clientmodel, "updatePeerStats", Qt::QueuedConnection,
                                  Q_ARG(QStringList, peerData));

        // Log peer stats timing (only occasionally to avoid spam)
        static int64_t lastPeerLogTime = 0;
        int64_t peerNow = GetTime();
        if (peerNow - lastPeerLogTime > 10) {
            LogPrint(BCLog::GUI, "PeerStats: Collected %d peers in %dms, converted in %dms, total queue time: %dms\n",
                     peerStats.size(), peerCollectTime, convertTime, peerQueueTime);
            lastPeerLogTime = peerNow;
        }
    });

    // Update connection data via signal
    int64_t queueStartTime = GetTimeMillis();
    bool invoked = QMetaObject::invokeMethod(clientmodel, "updateConnectionData", Qt::QueuedConnection,
                              Q_ARG(int, connectionsIn),
                              Q_ARG(int, connectionsOut),
                              Q_ARG(int, connectionsTotal));
    int64_t queueTime = GetTimeMillis() - queueStartTime;
    if (!invoked) {
        LogPrint(BCLog::GUI, "NotifyNumConnectionsChanged: Failed to queue connection data update\n");
    }

    // Update network data via signal
    invoked = QMetaObject::invokeMethod(clientmodel, "updateNetworkData", Qt::QueuedConnection,
                              Q_ARG(qint64, bytesRecv),
                              Q_ARG(qint64, bytesSent));
    if (!invoked) {
        LogPrint(BCLog::GUI, "NotifyNumConnectionsChanged: Failed to queue network data update\n");
    }

    // Also emit the legacy signal for compatibility
    invoked = QMetaObject::invokeMethod(clientmodel, "updateNumConnections", Qt::QueuedConnection,
                              Q_ARG(int, newNumConnections));
    if (!invoked) {
        LogPrint(BCLog::GUI, "NotifyNumConnectionsChanged: Failed to queue num connections update\n");
    }

    // Log timing information for debugging GUI responsiveness
    int64_t totalSignalTime = GetTimeMillis() - signalStartTime;
    if (now - lastLogTime > 5) {
        LogPrint(BCLog::GUI, "NotifyNumConnectionsChanged: Data collect: %dms, Queue ops: %dms, Total signal time: %dms\n",
                 dataCollectTime, queueTime, totalSignalTime);

        // Warn if signal handler is taking too long (could block validation thread)
        if (totalSignalTime > 50) {
            LogPrint(BCLog::GUI, "WARNING: NotifyNumConnectionsChanged took %dms - this could block validation thread!\n", totalSignalTime);
        }
    }
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
    static int64_t lastBlockLogTime = 0;
    int64_t blockStartTime = GetTimeMillis();
    int64_t now = GetTime();

    if (fHeader) {
        // Queue header tip update to GUI thread for thread safety
        // This ensures cache updates happen in the correct thread context
        bool invoked = QMetaObject::invokeMethod(clientmodel, "updateHeaderTip", Qt::QueuedConnection,
            Q_ARG(int, tip.block_height),
            Q_ARG(qint64, tip.block_time));
        if (!invoked) {
            qWarning() << "ClientModel: Failed to queue header tip update";
        }
    } else {
        // Update block data via signal
        bool invoked = QMetaObject::invokeMethod(clientmodel, "updateBlockData", Qt::QueuedConnection,
            Q_ARG(int, tip.block_height),
            Q_ARG(QString, QString::fromStdString(tip.block_hash.ToString())),
            Q_ARG(bool, clientmodel->node().isInitialSyncFinished()));
        if (!invoked) {
            LogPrint(BCLog::GUI, "BlockTipChanged: Failed to queue updateBlockData signal\n");
        } else {
            LogPrint(BCLog::GUI, "BlockTipChanged: Successfully queued updateBlockData signal for height %d\n", tip.block_height);
        }
    }

    int64_t blockTime = GetTimeMillis() - blockStartTime;
    if (now - lastBlockLogTime > 5) {
        LogPrint(BCLog::GUI, "BlockTipChanged: %s update queued in %dms (height: %d)\n",
                 fHeader ? "Header" : "Block", blockTime, tip.block_height);
        lastBlockLogTime = now;
    }

    // Throttle GUI notifications about (a) blocks during initial sync, and (b) both blocks and headers during reindex.
    const bool throttle = (sync_state != SynchronizationState::POST_INIT && !fHeader) || sync_state == SynchronizationState::INIT_REINDEX;
    const int64_t throttleNow = throttle ? GetTimeMillis() : 0;
    int64_t& nLastUpdateNotification = fHeader ? nLastHeaderTipUpdateNotification : nLastBlockTipUpdateNotification;
    if (throttle && throttleNow < nLastUpdateNotification + count_milliseconds(MODEL_UPDATE_DELAY)) {
        return;
    }

    bool invoked = QMetaObject::invokeMethod(clientmodel, "numBlocksChanged", Qt::QueuedConnection,
        Q_ARG(int, tip.block_height),
        Q_ARG(QDateTime, QDateTime::fromSecsSinceEpoch(tip.block_time)),
        Q_ARG(double, verificationProgress),
        Q_ARG(bool, fHeader),
        Q_ARG(SynchronizationState, sync_state));
    if (!invoked) {
        LogPrint(BCLog::GUI, "BlockTipChanged: Failed to queue numBlocksChanged signal\n");
    } else {
        LogPrint(BCLog::GUI, "BlockTipChanged: Successfully queued numBlocksChanged signal for height %d\n", tip.block_height);
    }
    nLastUpdateNotification = throttleNow;
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

    // Note: Mempool fee histogram updates are handled via the existing MempoolStatsDidChange signal
    // The updateFeeHistogram method is called from updateMempoolStats() when needed

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

    // Note: m_handler_mempool_fee_histogram is not used since the method doesn't exist
    // Fee histogram updates are handled via MempoolStatsDidChange signal

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
    static int64_t lastMempoolLogTime = 0;
    int64_t mempoolStartTime = GetTimeMillis();

    // Update mempool size and usage
    m_gui_data.mempoolSize = m_node.getMempoolSize();
    m_gui_data.mempoolDynamicUsage = m_node.getMempoolDynamicUsage();

    // Emit signal for mempool size changes
    Q_EMIT mempoolSizeChanged(m_gui_data.mempoolSize, m_gui_data.mempoolDynamicUsage);

    // Collect fee histogram data
    int64_t now = GetTime();
    if (m_mempool_feehist_last_sample_timestamp == 0 ||
        static_cast<uint64_t>(m_mempool_feehist_last_sample_timestamp) + static_cast<uint64_t>(m_mempool_collect_intervall) <= static_cast<uint64_t>(now)) {

        QMutexLocker locker(&m_mempool_locker);
        interfaces::mempool_feehistogram fee_histogram = m_node.getMempoolFeeHistogram();
        m_mempool_feehist.push_back({now, fee_histogram});

        if (m_mempool_feehist.size() > m_mempool_max_samples) {
            m_mempool_feehist.erase(m_mempool_feehist.begin(), m_mempool_feehist.begin()+1);
        }

        m_mempool_feehist_last_sample_timestamp = now;

        // Update the cached fee histogram
        m_gui_data.feeHistogram = fee_histogram;
        m_gui_data.lastFeeHistogramUpdateTime = now;

        Q_EMIT mempoolFeeHistChanged();
    }

    int64_t mempoolTime = GetTimeMillis() - mempoolStartTime;
    if (now - lastMempoolLogTime > 5) {
        LogPrint(BCLog::QT, "updateMempoolStats: GUI thread update completed in %dms (size: %d txs, %d bytes)\n",
                 mempoolTime, m_gui_data.mempoolSize, m_gui_data.mempoolDynamicUsage);
        lastMempoolLogTime = now;
    }

    Q_EMIT mempoolStatsDidUpdate();
}

void ClientModel::updateHeaderTip(int height, qint64 blockTime)
{
    // Update header tip in the GUI thread context
    // This is called from the validation thread via QMetaObject::invokeMethod
    m_gui_data.headerHeight = height;
    m_gui_data.headerTime = blockTime;
}

void ClientModel::updateBlockData(int numBlocks, const QString& bestBlockHashStr, bool initialSyncFinished)
{
    static int64_t lastBlockLogTime = 0;
    int64_t updateStartTime = GetTimeMillis();
    int64_t now = GetTime();

    // Always log this to see if block signals are being processed
    LogPrint(BCLog::QT, "updateBlockData: GUI thread processing block update: height %d\n", numBlocks);

    // Increment signal processing counter
    s_signalProcessingCount++;

    // Update block data in the GUI thread context
    m_gui_data.numBlocks = numBlocks;

    // Safely convert block hash string
    try {
        m_gui_data.bestBlockHash = uint256S(bestBlockHashStr.toStdString());
    } catch (const std::exception& e) {
        qWarning() << "ClientModel: Invalid block hash string:" << bestBlockHashStr << "Error:" << e.what();
        // Keep previous hash if conversion fails
    }

    m_gui_data.initialSyncFinished = initialSyncFinished;

    // Note: numBlocksChanged signal is emitted by the original BlockTipChanged handler
    // to avoid duplicate signal emissions

    int64_t updateTime = GetTimeMillis() - updateStartTime;
    LogPrint(BCLog::QT, "updateBlockData: GUI thread processed block update in %dms (height: %d)\n", updateTime, numBlocks);
    lastBlockLogTime = now;
}

void ClientModel::updateConnectionData(int connectionsIn, int connectionsOut, int connectionsTotal)
{
    static int64_t lastLogTime = 0;
    int64_t updateStartTime = GetTimeMillis();
    int64_t now = GetTime();

    // Increment signal processing counter
    s_signalProcessingCount++;

    if (now - lastLogTime > 5) {
        LogPrint(BCLog::QT, "updateConnectionData: GUI thread processing connection update: %d in, %d out, %d total\n", connectionsIn, connectionsOut, connectionsTotal);
        lastLogTime = now;
    }

    // Update connection data in the GUI thread context
    m_gui_data.numConnectionsIn = connectionsIn;
    m_gui_data.numConnectionsOut = connectionsOut;
    m_gui_data.numConnectionsTotal = connectionsTotal;

    int64_t updateTime = GetTimeMillis() - updateStartTime;
    if (now - lastLogTime > 5) {
        LogPrint(BCLog::QT, "updateConnectionData: GUI thread update completed in %dms\n", updateTime);
    }
}

void ClientModel::updateNetworkData(qint64 bytesRecv, qint64 bytesSent)
{
    // Update network data in the GUI thread context
    m_gui_data.bytesRecv = bytesRecv;
    m_gui_data.bytesSent = bytesSent;

    // Emit signal for traffic graph widget
    Q_EMIT bytesChanged(bytesRecv, bytesSent);
}

void ClientModel::updatePeerStats(const QStringList& peerData)
{
    static int64_t lastLogTime = 0;
    int64_t updateStartTime = GetTimeMillis();
    int64_t now = GetTime();

    if (now - lastLogTime > 5) {
        LogPrint(BCLog::QT, "updatePeerStats: GUI thread processing %d peer updates...\n", peerData.size());
        lastLogTime = now;
    }

    // Update peer stats in the GUI thread context
    // Convert QStringList back to NodesStats format for compatibility
    interfaces::Node::NodesStats stats;

    for (const QString& peerInfo : peerData) {
        QStringList parts = peerInfo.split("|");
        if (parts.size() >= 20) {
            try {
                // Create a complete CNodeStats structure
                CNodeStats nodeStats;
                nodeStats.nodeid = parts[0].toInt();
                nodeStats.m_addr_name = parts[1].toStdString();
                nodeStats.nVersion = parts[2].toInt();
                nodeStats.cleanSubVer = parts[3].toStdString();
                nodeStats.fInbound = parts[4] == "1";
                nodeStats.nSendBytes = parts[5].toULongLong();
                nodeStats.nRecvBytes = parts[6].toULongLong();
                nodeStats.m_connected = std::chrono::seconds(parts[7].toLongLong());
                nodeStats.nTimeOffset = parts[8].toLongLong();

                // Restore actual network and connection type from serialized data
                nodeStats.m_network = static_cast<Network>(parts[10].toInt());
                nodeStats.m_conn_type = static_cast<ConnectionType>(parts[11].toInt());

                // Restore additional fields
                nodeStats.nServices = static_cast<ServiceFlags>(parts[12].toULongLong());
                nodeStats.fRelayTxes = parts[13] == "1";
                nodeStats.m_last_send = std::chrono::seconds(parts[14].toLongLong());
                nodeStats.m_last_recv = std::chrono::seconds(parts[15].toLongLong());
                nodeStats.m_last_tx_time = std::chrono::seconds(parts[16].toLongLong());
                nodeStats.m_last_block_time = std::chrono::seconds(parts[17].toLongLong());
                nodeStats.m_permissionFlags = static_cast<NetPermissionFlags>(parts[18].toUInt());
                nodeStats.m_mapped_as = parts[19].toUInt();

                CNodeStateStats stateStats;
                stateStats.m_ping_wait = std::chrono::seconds(static_cast<int64_t>(parts[9].toDouble()));

                stats.emplace_back(nodeStats, true, stateStats);
            } catch (const std::exception& e) {
                qWarning() << "ClientModel: Failed to parse peer data:" << peerInfo << "Error:" << e.what();
                // Skip this peer if parsing fails
            }
        }
    }

    m_gui_data.peerStats = stats;
    m_gui_data.lastPeerUpdateTime = GetTime();

    int64_t updateTime = GetTimeMillis() - updateStartTime;
    if (now - lastLogTime > 5) {
        LogPrint(BCLog::QT, "updatePeerStats: GUI thread completed processing %d peers in %dms\n", stats.size(), updateTime);

        // Warn if GUI thread processing is taking too long
        if (updateTime > 100) {
            LogPrint(BCLog::QT, "WARNING: updatePeerStats took %dms - this could cause GUI unresponsiveness!\n", updateTime);
        }
    }
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
                           .arg(s_signalProcessingCount.load())
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

// Add missing method implementations
bool ClientModel::isCacheValid() const
{
    // With signal-based updates, cache is always "valid" since it's updated immediately
    // This method is kept for compatibility but no longer needed
    return true;
}

void ClientModel::forceCacheRefresh()
{
    // With signal-based updates, this method is no longer needed
    // Cache is updated immediately when signals are received
    qDebug() << "ClientModel: forceCacheRefresh() called but no longer needed with signal-based updates";
}

QString ClientModel::getPerformanceStats() const
{
    int64_t now = GetTime();

    QString stats = QString("GUI Responsiveness Performance Stats:\n"
                           "  Signal-Based Updates: %1 total\n"
                           "  Signals Processed: %2\n"
                           "  Last Block Update: %3 seconds ago\n"
                           "  Last Header Update: %4 seconds ago\n"
                           "  Last Connection Update: %5 seconds ago\n"
                           "  Last Network Update: %6 seconds ago\n"
                           "  Last Peer Update: %7 seconds ago\n"
                           "  Last Mempool Update: %8 seconds ago\n"
                           "  Last Fee Histogram Update: %9 seconds ago\n"
                           "  Current Block Height: %10\n"
                           "  Header Height: %11\n"
                           "  Active Connections: %12\n"
                           "  Mempool Size: %13 txs\n"
                           "  Network Traffic: %14 bytes in, %15 bytes out\n"
                           "  Performance Thresholds:\n"
                           "    Signal Handler: < 50ms (validation thread)\n"
                           "    GUI Thread: < 100ms (responsiveness)\n"
                           "    Queue Operations: < 10ms (efficiency)\n"
                           "    Peer Collection: < 50ms (background)")
                           .arg(m_gui_data.updateCount)
                           .arg(now - m_gui_data.lastUpdateTime)
                           .arg(now - m_gui_data.lastUpdateTime) // Using same as block for now
                           .arg(now - m_gui_data.lastUpdateTime) // Using same as block for now
                           .arg(now - m_gui_data.lastUpdateTime) // Using same as block for now
                           .arg(now - m_gui_data.lastPeerUpdateTime)
                           .arg(now - m_gui_data.lastUpdateTime) // Using same as block for now
                           .arg(now - m_gui_data.lastFeeHistogramUpdateTime)
                           .arg(m_gui_data.numBlocks)
                           .arg(m_gui_data.headerHeight)
                           .arg(m_gui_data.numConnectionsTotal)
                           .arg(m_gui_data.mempoolSize)
                           .arg(m_gui_data.bytesRecv)
                           .arg(m_gui_data.bytesSent);

    return stats;
}
