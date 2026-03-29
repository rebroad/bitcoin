// Copyright (c) 2011-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/clientmodel.h>

// Static member definition
uint256 ClientModel::g_cached_best_block_hash;

#include <qt/bantablemodel.h>
#include <qt/guiconstants.h>
#include <qt/guiutil.h>
#include <qt/peertablemodel.h>
#include <sync.h>
#include <net.h>
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
#include <execinfo.h>
#include <qt/locktiming.h>

static int64_t nLastHeaderTipUpdateNotification = 0;
static int64_t nLastBlockTipUpdateNotification = 0;

ClientModel::ClientModel(interfaces::Node& node, OptionsModel *_optionsModel, QObject *parent) :
    QObject(parent),
    m_data_thread(nullptr),
    m_data_worker(nullptr),
    m_node(node),
    optionsModel(_optionsModel),
    peerTableModel(nullptr),
    banTableModel(nullptr),
    m_thread(new QThread(this))
{
    m_thread->setObjectName("qt-clientmodl");

    peerTableModel = new PeerTableModel(m_node, this);
    m_peer_table_sort_proxy = new PeerTableSortProxy(this);
    m_peer_table_sort_proxy->setSourceModel(peerTableModel);

    banTableModel = new BanTableModel(m_node, this);

    QTimer* timer = new QTimer;
    timer->setInterval(MODEL_UPDATE_DELAY);
    connect(timer, &QTimer::timeout, [this] {
        // Queue network and mempool stats update to data thread to avoid cs_main in GUI thread
        if (m_data_worker) {
            QMetaObject::invokeMethod(m_data_worker, "updateNetworkAndMempoolStatsAsync", Qt::QueuedConnection);
        }

        // Handle fee histogram collection (only when not in IBD)
        // This is safe to do in GUI thread since it's not critical during IBD
        int64_t now = GetTime();
        if (m_mempool_feehist_last_sample_timestamp == 0 ||
            static_cast<uint64_t>(m_mempool_feehist_last_sample_timestamp)+static_cast<uint64_t>(m_mempool_collect_intervall) <= static_cast<uint64_t>(now)) {

            // Only collect fee histogram if initial sync has finished (mempool likely empty during IBD anyway)
            if (m_cached_initial_sync_finished.load()) { // Non-blocking check for IBD completion
                // Try to get fresh data directly if cs_main is free
                TIME_CS_MAIN_LOCK(10);

                if (lock.owns_lock()) {
                    // We got the lock! Get fresh data and update cache
                    QMutexLocker locker(&m_mempool_locker);
                    interfaces::mempool_feehistogram fee_histogram = m_node.getMempoolFeeHistogram();
                    m_mempool_feehist.push_back({now, fee_histogram});
                    if (m_mempool_feehist.size() > m_mempool_max_samples) {
                        m_mempool_feehist.erase(m_mempool_feehist.begin(), m_mempool_feehist.begin()+1);
                    }
                    m_mempool_feehist_last_sample_timestamp = now;
                    Q_EMIT mempoolFeeHistChanged();
                }
                // If cs_main is busy, skip this update (non-blocking)
            }
        }
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

    // Setup data processing thread
    setupDataThread();

    // Initialize cache with try_lock to avoid blocking during startup
    // This prevents GUI from constantly trying to get fresh data during IBD
    std::unique_lock<RecursiveMutex> lock(cs_main, std::try_to_lock);
    if (lock.owns_lock()) {
        // We got the lock! Initialize all caches
        int height;
        int64_t block_time;
        if (m_node.getHeaderTip(height, block_time)) {
            m_cached_header_height.store(height);
            m_cached_header_time.store(block_time);
        }

        // Initialize other caches
        m_cached_num_blocks.store(m_node.getNumBlocks());
        g_cached_best_block_hash = m_node.getBestBlockHash();

        // Initialize block source cache
        BlockSource freshSource;
        if (m_node.getReindex()) freshSource = BlockSource::REINDEX;
        else if (m_node.getImporting()) freshSource = BlockSource::DISK;
        else if (m_node.getNodeCount(ConnectionDirection::Both) > 0) freshSource = BlockSource::NETWORK;
        else freshSource = BlockSource::NONE;
        m_cached_block_source.store(freshSource);

        // Initialize status bar warnings cache
        m_cached_status_bar_warnings = QString::fromStdString(m_node.getWarnings().translated);
    } else {
        // cs_main is locked, use default values
        // The cache will be updated when the first block tip change occurs
        qDebug() << "Cache initialization skipped - cs_main is locked during startup";
    }
}

ClientModel::~ClientModel()
{
    unsubscribeFromCoreSignals();

    m_thread->quit();
    m_thread->wait();

    // Teardown data processing thread
    teardownDataThread();
}

int ClientModel::getNumConnections(unsigned int flags) const
{
    // Try to get fresh data directly if cs_main is free
    TIME_CS_MAIN_LOCK(10);

    if (lock.owns_lock()) {
        // We got the lock! Get fresh data and update cache
        int freshCount = static_cast<int>(m_node.getNodeCount(static_cast<ConnectionDirection>(flags)));
        m_cached_num_connections.store(freshCount);
        return freshCount;
    } else {
        // cs_main is busy, use cached data
        return m_cached_num_connections.load();
    }
}

int ClientModel::getHeaderTipHeight() const
{
    // Try to get fresh data directly if cs_main is free
    TIME_CS_MAIN_LOCK(10);

    if (lock.owns_lock()) {
        // We got the lock! Get fresh data and update cache
        int height;
        int64_t block_time;
        if (m_node.getHeaderTip(height, block_time)) {
            m_cached_header_height.store(height);
            m_cached_header_time.store(block_time);
            return height;
        }
    }

    // cs_main is busy or getHeaderTip failed, use cached data
    return m_cached_header_height.load();
}

int64_t ClientModel::getHeaderTipTime() const
{
    // Try to get fresh data directly if cs_main is free
    TIME_CS_MAIN_LOCK(10);

    if (lock.owns_lock()) {
        // We got the lock! Get fresh data and update cache
        int height;
        int64_t block_time;
        if (m_node.getHeaderTip(height, block_time)) {
            m_cached_header_height.store(height);
            m_cached_header_time.store(block_time);
            return block_time;
        }
    }

    // cs_main is busy or getHeaderTip failed, use cached data
    return m_cached_header_time.load();
}

int ClientModel::getNumBlocks() const
{
    // Try to get fresh data directly if cs_main is free
    TIME_CS_MAIN_LOCK(10);

    if (lock.owns_lock()) {
        // We got the lock! Get fresh data and update cache
        int freshBlocks = m_node.getNumBlocks();
        m_cached_num_blocks.store(freshBlocks);
        return freshBlocks;
    } else {
        // cs_main is busy, use cached data
        return m_cached_num_blocks.load();
    }
}

uint256 ClientModel::getBestBlockHash()
{
    // Try to get fresh data directly if cs_main is free
    TIME_CS_MAIN_LOCK(10);

    if (lock.owns_lock()) {
        // We got the lock! Get fresh data and update cache
        uint256 freshHash = m_node.getBestBlockHash();
        g_cached_best_block_hash = freshHash; // Update static global (non-blocking)
        return freshHash;
    } else {
        // cs_main is busy, use cached data
        return g_cached_best_block_hash; // Read from static global (non-blocking)
    }
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
    // getStatusBarWarnings() already handles responsiveness internally
    Q_EMIT alertsChanged(getStatusBarWarnings());
}

enum BlockSource ClientModel::getBlockSource() const
{
    // Try to get fresh data directly if cs_main is free
    TIME_CS_MAIN_LOCK(10);

    if (lock.owns_lock()) {
        // We got the lock! Get fresh data and update cache
        BlockSource freshSource;
        if (m_node.getReindex()) freshSource = BlockSource::REINDEX;
        else if (m_node.getImporting()) freshSource = BlockSource::DISK;
        else if (m_node.getNodeCount(ConnectionDirection::Both) > 0) freshSource = BlockSource::NETWORK;
        else freshSource = BlockSource::NONE;

        m_cached_block_source.store(freshSource);
        return freshSource;
    } else {
        // cs_main is busy, use cached data
        return m_cached_block_source.load();
    }
}

QString ClientModel::getStatusBarWarnings() const
{
    // Try to get fresh data directly if cs_main is free
    TIME_CS_MAIN_LOCK(10);

    if (lock.owns_lock()) {
        // We got the lock! Get fresh data and update cache
        QString freshWarnings = QString::fromStdString(m_node.getWarnings().translated);
        m_cached_status_bar_warnings = freshWarnings;
        return freshWarnings;
    } else {
        // cs_main is busy, use cached data
        return m_cached_status_bar_warnings;
    }
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

void ClientModel::setupDataThread()
{
    LogPrint(BCLog::QT, "ClientModel: Setting up data processing thread\n");

    m_data_thread = new QThread(this);
    m_data_thread->setObjectName("qt-data");
    m_data_worker = new ClientModelDataWorker(m_node, this);
    m_data_worker->moveToThread(m_data_thread);

        // Connect signals from worker to GUI thread
    connect(static_cast<ClientModelDataWorker*>(m_data_worker), &ClientModelDataWorker::blockSourceResult,
            this, [this](BlockSource result) {
        // Handle result in GUI thread
        LogPrint(BCLog::QT, "ClientModel: Received block source result from data thread\n");
    });

    connect(static_cast<ClientModelDataWorker*>(m_data_worker), &ClientModelDataWorker::statusBarWarningsResult,
            this, [this](QString warnings) {
        // Handle result in GUI thread
        LogPrint(BCLog::QT, "ClientModel: Received status bar warnings from data thread\n");
    });

    // Connect additional result signals to update cached data
    connect(static_cast<ClientModelDataWorker*>(m_data_worker), &ClientModelDataWorker::numConnectionsResult,
            this, [this](int count) {
        m_cached_num_connections.store(count);
        LogPrint(BCLog::QT, "ClientModel: Updated cached num connections: %d\n", count);
    });

    connect(static_cast<ClientModelDataWorker*>(m_data_worker), &ClientModelDataWorker::numBlocksResult,
            this, [this](int count) {
        m_cached_num_blocks.store(count);
        LogPrint(BCLog::QT, "ClientModel: Updated cached num blocks: %d\n", count);
    });

    connect(static_cast<ClientModelDataWorker*>(m_data_worker), &ClientModelDataWorker::bestBlockHashResult,
            this, [this](uint256 hash) {
        g_cached_best_block_hash = hash; // Update static global (non-blocking)
        LogPrint(BCLog::QT, "ClientModel: Updated cached best block hash\n");
    });

    connect(static_cast<ClientModelDataWorker*>(m_data_worker), &ClientModelDataWorker::headerTipHeightResult,
            this, [this](int height) {
        m_cached_header_height.store(height);
        LogPrint(BCLog::QT, "ClientModel: Updated cached header height: %d\n", height);
    });

    connect(static_cast<ClientModelDataWorker*>(m_data_worker), &ClientModelDataWorker::headerTipTimeResult,
            this, [this](int64_t time) {
        m_cached_header_time.store(time);
        LogPrint(BCLog::QT, "ClientModel: Updated cached header time: %d\n", time);
    });

    connect(static_cast<ClientModelDataWorker*>(m_data_worker), &ClientModelDataWorker::proxyInfoResult,
            this, [this](bool hasProxy, QString ipPort) {
        m_cached_has_proxy.store(hasProxy);
        m_cached_proxy_ip_port = ipPort;
        LogPrint(BCLog::QT, "ClientModel: Updated cached proxy info\n");
    });

    // Connect mempool and network stats signals
    connect(static_cast<ClientModelDataWorker*>(m_data_worker), &ClientModelDataWorker::mempoolSizeChanged,
            this, &ClientModel::mempoolSizeChanged);
    connect(static_cast<ClientModelDataWorker*>(m_data_worker), &ClientModelDataWorker::bytesChanged,
            this, &ClientModel::bytesChanged);


    m_data_thread->start();
    LogPrint(BCLog::QT, "ClientModel: Data processing thread started\n");
}

void ClientModel::teardownDataThread()
{
    if (m_data_thread) {
        LogPrint(BCLog::QT, "ClientModel: Stopping data processing thread\n");
        m_data_thread->quit();
        m_data_thread->wait();
        delete m_data_worker;
        m_data_worker = nullptr;
        m_data_thread = nullptr;
    }
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

void ClientModel::updateInitialSyncFinished()
{
    m_cached_initial_sync_finished.store(true);
    LogPrint(BCLog::QT, "ClientModel: Initial sync finished - fee histogram collection enabled\n");
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
    bool invoked = QMetaObject::invokeMethod(clientmodel, "updateBanlist", Qt::QueuedConnection);
    assert(invoked);
}

static void NotifyInitialSyncFinished(ClientModel *clientmodel)
{
    qDebug() << QString("%1: Initial sync finished").arg(__func__);
    bool invoked = QMetaObject::invokeMethod(clientmodel, "updateInitialSyncFinished", Qt::QueuedConnection);
    assert(invoked);
}

static void NotifyBlockStatusChanged(ClientModel* clientmodel)
{
    Q_EMIT clientmodel->blockStatusesChanged();
}

static void BlockTipChanged(ClientModel* clientmodel, SynchronizationState sync_state, interfaces::BlockTip tip, double verificationProgress, bool fHeader)
{
    // Cache synchronization state for non-blocking access
    clientmodel->m_cached_sync_state.store(sync_state);

    if (fHeader) {
        // cache best headers time and height to reduce future cs_main locks
        clientmodel->m_cached_header_height.store(tip.block_height);
        clientmodel->m_cached_header_time.store(tip.block_time);
    } else {
        clientmodel->m_cached_num_blocks.store(tip.block_height);
        clientmodel->g_cached_best_block_hash = tip.block_hash; // Update static global (non-blocking)
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
    m_handler_notify_block_status_changed = m_node.handleNotifyBlockStatusChanged(std::bind(NotifyBlockStatusChanged, this));
    m_handler_notify_initial_sync_finished = m_node.handleNotifyInitialSyncFinished(std::bind(NotifyInitialSyncFinished, this));

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
    m_handler_notify_block_status_changed->disconnect();
    m_handler_notify_initial_sync_finished->disconnect();

    m_connection_mempool_stats_did_change.disconnect();
}

bool ClientModel::getProxyInfo(std::string& ip_port) const
{
    // Try to get fresh data directly if cs_main is free
    TIME_CS_MAIN_LOCK(10);

    if (lock.owns_lock()) {
        // We got the lock! Get fresh data and update cache
        proxyType ipv4, ipv6;
        bool freshHasProxy = m_node.getProxy((Network) 1, ipv4) && m_node.getProxy((Network) 2, ipv6);
        m_cached_has_proxy.store(freshHasProxy);

        if (freshHasProxy) {
            QString freshIpPort = QString::fromStdString(ipv4.proxy.ToStringIPPort());
            m_cached_proxy_ip_port = freshIpPort;
            ip_port = freshIpPort.toStdString();
        }

        return freshHasProxy;
    } else {
        // cs_main is busy, use cached data
        bool hasProxy = m_cached_has_proxy.load();
        if (hasProxy) {
            ip_port = m_cached_proxy_ip_port.toStdString();
        }
        return hasProxy;
    }
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

// ClientModelDataWorker constructor
ClientModelDataWorker::ClientModelDataWorker(interfaces::Node& node, QObject* parent)
    : QObject(parent), m_node(node)
{
}

void ClientModelDataWorker::getBlockSourceAsync()
{
    enum BlockSource result;
    if (m_node.getReindex()) result = BlockSource::REINDEX;
    else if (m_node.getImporting()) result = BlockSource::DISK;
    else if (m_node.getNodeCount(ConnectionDirection::Both) > 0) result = BlockSource::NETWORK;
    else result = BlockSource::NONE;

    Q_EMIT blockSourceResult(result);
}

void ClientModelDataWorker::getStatusBarWarningsAsync()
{
    QString result = QString::fromStdString(m_node.getWarnings().translated);
    Q_EMIT statusBarWarningsResult(result);
}

void ClientModelDataWorker::getMempoolStatsInRangeAsync(QDateTime from, QDateTime to)
{
    // Implementation for mempool stats
    mempoolSamples_t samples;
    Q_EMIT mempoolStatsResult(samples);
}

void ClientModelDataWorker::updateNetworkAndMempoolStatsAsync()
{
    // Get mempool size and usage (requires cs_main)
    size_t mempoolSize = m_node.getMempoolSize();
    size_t mempoolDynamicUsage = m_node.getMempoolDynamicUsage();

    // Get network stats (used by traffic graph widget)
    quint64 totalBytesRecv = m_node.getTotalBytesRecv();
    quint64 totalBytesSent = m_node.getTotalBytesSent();

    // Emit signals with the data
    Q_EMIT mempoolSizeChanged(mempoolSize, mempoolDynamicUsage);
    Q_EMIT bytesChanged(totalBytesRecv, totalBytesSent);
    Q_EMIT mempoolStatsUpdated();
}

void ClientModelDataWorker::getNumConnectionsAsync(unsigned int flags)
{
    ConnectionDirection connections = ConnectionDirection::None;
    if (flags == CONNECTIONS_IN)
        connections = ConnectionDirection::In;
    else if (flags == CONNECTIONS_OUT)
        connections = ConnectionDirection::Out;
    else if (flags == CONNECTIONS_ALL)
        connections = ConnectionDirection::Both;

    int count = m_node.getNodeCount(connections);
    Q_EMIT numConnectionsResult(count);
}

void ClientModelDataWorker::getNumBlocksAsync()
{
    int count = m_node.getNumBlocks();
    Q_EMIT numBlocksResult(count);
}

void ClientModelDataWorker::getBestBlockHashAsync()
{
    uint256 hash = m_node.getBestBlockHash();
    Q_EMIT bestBlockHashResult(hash);
}

void ClientModelDataWorker::getHeaderTipHeightAsync()
{
    int height;
    int64_t blockTime;
    if (m_node.getHeaderTip(height, blockTime)) {
        Q_EMIT headerTipHeightResult(height);
    } else {
        Q_EMIT headerTipHeightResult(-1);
    }
}

void ClientModelDataWorker::getHeaderTipTimeAsync()
{
    int height;
    int64_t blockTime;
    if (m_node.getHeaderTip(height, blockTime)) {
        Q_EMIT headerTipTimeResult(blockTime);
    } else {
        Q_EMIT headerTipTimeResult(-1);
    }
}

void ClientModelDataWorker::getProxyInfoAsync()
{
    proxyType ipv4, ipv6;
    bool hasProxy = false;
    QString ipPort;

    if (m_node.getProxy((Network) 1, ipv4) && m_node.getProxy((Network) 2, ipv6)) {
        ipPort = QString::fromStdString(ipv4.proxy.ToStringIPPort());
        hasProxy = true;
    }

    Q_EMIT proxyInfoResult(hasProxy, ipPort);
}

size_t ClientModel::getMempoolDynamicUsage() const
{
    // Try to get fresh data directly if cs_main is free
    TIME_CS_MAIN_LOCK(10);

    if (lock.owns_lock()) {
        // We got the lock! Get fresh data and update cache
        size_t freshUsage = m_node.getMempoolDynamicUsage();
        m_cached_mempool_dynamic_usage.store(freshUsage);
        return freshUsage;
    } else {
        // cs_main is busy, use cached data
        return m_cached_mempool_dynamic_usage.load();
    }
}
