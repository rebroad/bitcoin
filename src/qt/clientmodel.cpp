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
    banTableModel(nullptr),
    m_thread(new QThread(this))
{
    cachedBestHeaderHeight = -1;
    cachedBestHeaderTime = -1;

    peerTableModel = new PeerTableModel(m_node, this);
    m_peer_table_sort_proxy = new PeerTableSortProxy(this);
    m_peer_table_sort_proxy->setSourceModel(peerTableModel);

    banTableModel = new BanTableModel(m_node, this);

    QTimer* timer = new QTimer;
    timer->setInterval(MODEL_UPDATE_DELAY);
    connect(timer, &QTimer::timeout, [this, timer] {
        // no locking required at this point
        // the following calls will acquire the required lock

        // Update GUI data asynchronously to avoid cs_main contention
        // Note: These calls still need cs_main, but they're in a background thread
        // and the GUI methods will use cached data instead
        interfaces::mempool_feehistogram current_fee_histogram;
        {
            LOCK(m_gui_data_mutex);

            int64_t updateStartTime = GetTime();

            // Direct assignment - these methods are reliable and always return valid values
            m_gui_data.numBlocks = m_node.getNumBlocks();

            // Fix: Use local variables for getHeaderTip to avoid passing stale cached values
            int height;
            int64_t blockTime;
            if (m_node.getHeaderTip(height, blockTime)) {
                m_gui_data.headerHeight = height;
                m_gui_data.headerTime = blockTime;
            } else {
                // Keep previous values if getHeaderTip fails
                // This prevents cache from being corrupted with invalid data
                qDebug() << "ClientModel: getHeaderTip failed, keeping previous cached values";
            }

            m_gui_data.bestBlockHash = m_node.getBestBlockHash();
            m_gui_data.initialSyncFinished = m_node.isInitialSyncFinished();
            m_gui_data.numConnectionsIn = m_node.getNodeCount(ConnectionDirection::In);
            m_gui_data.numConnectionsOut = m_node.getNodeCount(ConnectionDirection::Out);
            m_gui_data.numConnectionsTotal = m_node.getNodeCount(ConnectionDirection::Both);
            m_gui_data.mempoolSize = m_node.getMempoolSize();
            m_gui_data.mempoolDynamicUsage = m_node.getMempoolDynamicUsage();
            m_gui_data.bytesRecv = m_node.getTotalBytesRecv();
            m_gui_data.bytesSent = m_node.getTotalBytesSent();

            // Update peer stats cache (less frequently to avoid cs_main contention)
            int64_t now = GetTime();
            if (now - m_gui_data.lastPeerUpdateTime >= 5) { // Update every 5 seconds
                m_gui_data.peerStats.clear();
                m_node.getNodesStats(m_gui_data.peerStats);
                m_gui_data.lastPeerUpdateTime = now;
            }

            // Update mempool fee histogram cache (less frequently to avoid cs_main contention)
            if (now - m_gui_data.lastFeeHistogramUpdateTime >= 10) { // Update every 10 seconds
                m_gui_data.feeHistogram.clear();
                m_gui_data.feeHistogram = m_node.getMempoolFeeHistogram();
                m_gui_data.lastFeeHistogramUpdateTime = now;
            }

            // Update performance metrics
            m_gui_data.lastUpdateTime = updateStartTime;
            m_gui_data.updateCount++;

            // Store fee histogram for use outside the mutex block
            current_fee_histogram = m_gui_data.feeHistogram;
        }

        // Check if we're in IBD and adjust timer interval for better responsiveness
        bool inIBD = !m_gui_data.initialSyncFinished; // Use cached data
        if (inIBD && timer->interval() != count_milliseconds(MODEL_UPDATE_DELAY_IBD)) {
            timer->setInterval(MODEL_UPDATE_DELAY_IBD);
            // Force immediate GUI update when entering IBD
            QApplication::processEvents();
        } else if (!inIBD && timer->interval() != count_milliseconds(MODEL_UPDATE_DELAY)) {
            timer->setInterval(MODEL_UPDATE_DELAY);
        }

        int64_t now = GetTime();
        if (m_mempool_feehist_last_sample_timestamp == 0 || static_cast<uint64_t>(m_mempool_feehist_last_sample_timestamp)+static_cast<uint64_t>(m_mempool_collect_intervall) <= static_cast<uint64_t>(now)) {
            QMutexLocker locker(&m_mempool_locker);
            // Use the fee histogram we captured inside the mutex block
            m_mempool_feehist.push_back({now, current_fee_histogram});
            if (m_mempool_feehist.size() > m_mempool_max_samples) {
                m_mempool_feehist.erase(m_mempool_feehist.begin(), m_mempool_feehist.begin()+1);
            }
            m_mempool_feehist_last_sample_timestamp = now;
            Q_EMIT mempoolFeeHistChanged();
        }

        Q_EMIT mempoolSizeChanged(m_gui_data.mempoolSize, m_gui_data.mempoolDynamicUsage);
        Q_EMIT bytesChanged(m_gui_data.bytesRecv, m_gui_data.bytesSent);
    });
    connect(m_thread, &QThread::finished, timer, &QObject::deleteLater);
    connect(m_thread, &QThread::started, [timer] { timer->start(); });
    // move timer to thread so that polling doesn't disturb main event loop
    timer->moveToThread(m_thread);
    m_thread->start();
    QTimer::singleShot(0, timer, []() {
        util::ThreadRename("qt-clientmodl");
    });

    subscribeToCoreSignals();

    // Log the IBD freeze-fix implementation
    qDebug() << "ClientModel: IBD freeze-fix enabled - using async cache updates with"
             << MODEL_UPDATE_DELAY.count() << "ms normal /"
             << MODEL_UPDATE_DELAY_IBD.count() << "ms IBD intervals";

    // Log initial cache stats after 10 seconds
    QTimer::singleShot(10000, [this]() {
        qDebug() << "ClientModel: Initial cache stats:" << getCacheStats();
    });
}

ClientModel::~ClientModel()
{
    unsubscribeFromCoreSignals();

    m_thread->quit();
    m_thread->wait();
}

int ClientModel::getNumConnections(unsigned int flags) const
{
    // Use cached data to avoid cs_main contention
    LOCK(m_gui_data_mutex);

    // Check if cache is stale (older than 60 seconds) and log warning
    int64_t now = GetTime();
    if (m_gui_data.lastUpdateTime > 0 && (now - m_gui_data.lastUpdateTime) > 60) {
        qDebug() << "ClientModel: Warning - cache is stale, last update was" << (now - m_gui_data.lastUpdateTime) << "seconds ago";
    }

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
    // Use cached data to avoid cs_main contention
    LOCK(m_gui_data_mutex);

    // If cache is invalid, fall back to direct call
    if (m_gui_data.headerHeight < 0) {
        qDebug() << "ClientModel: Cache miss for headerHeight, falling back to direct call";
        int height;
        int64_t blockTime;
        if (m_node.getHeaderTip(height, blockTime)) {
            return height;
        }
        return -1; // Return invalid if getHeaderTip fails
    }

    return m_gui_data.headerHeight;
}

int64_t ClientModel::getHeaderTipTime() const
{
    // Use cached data to avoid cs_main contention
    LOCK(m_gui_data_mutex);

    // If cache is invalid, fall back to direct call
    if (m_gui_data.headerTime < 0) {
        qDebug() << "ClientModel: Cache miss for headerTime, falling back to direct call";
        int height;
        int64_t blockTime;
        if (m_node.getHeaderTip(height, blockTime)) {
            return blockTime;
        }
        return -1; // Return invalid if getHeaderTip fails
    }

    return m_gui_data.headerTime;
}

int ClientModel::getNumBlocks() const
{
    // Use cached data to avoid cs_main contention
    LOCK(m_gui_data_mutex);

    // If cache is invalid, fall back to direct call (with warning)
    if (m_gui_data.numBlocks < 0) {
        qDebug() << "ClientModel: Cache miss for numBlocks, falling back to direct call";
        return m_node.getNumBlocks();
    }

    return m_gui_data.numBlocks;
}

uint256 ClientModel::getBestBlockHash()
{
    // Use cached data to avoid cs_main contention
    LOCK(m_gui_data_mutex);

    // If cache is invalid, fall back to direct call
    if (m_gui_data.bestBlockHash.IsNull()) {
        qDebug() << "ClientModel: Cache miss for bestBlockHash, falling back to direct call";
        return m_node.getBestBlockHash();
    }

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
        // cache best headers time and height to reduce future cs_main locks
        clientmodel->cachedBestHeaderHeight = tip.block_height;
        clientmodel->cachedBestHeaderTime = tip.block_time;
    } else {
        clientmodel->m_cached_num_blocks = tip.block_height;
        WITH_LOCK(clientmodel->m_cached_tip_mutex, clientmodel->m_cached_tip_blocks = tip.block_hash;);
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

QString ClientModel::getCacheStats() const
{
    LOCK(m_gui_data_mutex);

    int64_t now = GetTime();
    int64_t age = now - m_gui_data.lastUpdateTime;

    QString stats = QString("Cache Stats:\n"
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

bool ClientModel::isCacheValid() const
{
    LOCK(m_gui_data_mutex);

    // Cache is valid if we've had at least one successful update
    // and the last update was within the last 30 seconds
    int64_t now = GetTime();
    int64_t age = now - m_gui_data.lastUpdateTime;

    return m_gui_data.updateCount > 0 && age < 30;
}

void ClientModel::forceCacheRefresh()
{
    // This method can be called from the GUI thread to force an immediate cache update
    // It will trigger the timer to fire immediately
    qDebug() << "ClientModel: Forcing cache refresh";

    // Reset cache age to force immediate update
    LOCK(m_gui_data_mutex);
    m_gui_data.lastUpdateTime = 0;
}

int64_t ClientModel::getCachedBytesRecv() const
{
    // Use cached data to avoid cs_main contention
    LOCK(m_gui_data_mutex);

    // If cache hasn't been updated yet, fall back to direct call
    if (m_gui_data.updateCount == 0) {
        qDebug() << "ClientModel: Cache miss for bytesRecv, falling back to direct call";
        return m_node.getTotalBytesRecv();
    }

    return m_gui_data.bytesRecv;
}

int64_t ClientModel::getCachedBytesSent() const
{
    // Use cached data to avoid cs_main contention
    LOCK(m_gui_data_mutex);

    // If cache hasn't been updated yet, fall back to direct call
    if (m_gui_data.updateCount == 0) {
        qDebug() << "ClientModel: Cache miss for bytesSent, falling back to direct call";
        return m_node.getTotalBytesSent();
    }

    return m_gui_data.bytesSent;
}

bool ClientModel::getCachedPeerStats(interfaces::Node::NodesStats& stats) const
{
    // Use cached data to avoid cs_main contention
    LOCK(m_gui_data_mutex);

    // If cache is empty, fall back to direct call
    if (m_gui_data.peerStats.empty()) {
        qDebug() << "ClientModel: Cache miss for peerStats, falling back to direct call";
        return m_node.getNodesStats(stats);
    }

    stats = m_gui_data.peerStats;
    return true;
}

bool ClientModel::getCachedFeeHistogram(interfaces::mempool_feehistogram& histogram) const
{
    // Use cached data to avoid cs_main contention
    LOCK(m_gui_data_mutex);

    // If cache is empty, fall back to direct call
    if (m_gui_data.feeHistogram.empty()) {
        qDebug() << "ClientModel: Cache miss for feeHistogram, falling back to direct call";
        histogram = m_node.getMempoolFeeHistogram();
        return true;
    }

    histogram = m_gui_data.feeHistogram;
    return true;
}
