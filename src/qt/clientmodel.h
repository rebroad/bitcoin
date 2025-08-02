// Copyright (c) 2011-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_CLIENTMODEL_H
#define BITCOIN_QT_CLIENTMODEL_H

#include <QMutex>
#include <QObject>
#include <QDateTime>

// Forward declarations
class ClientModelDataWorker;

// Macro to automatically capture caller for responsiveness requests
#define REQUEST_RESPONSIVENESS(reason) requestResponsiveness(reason)
#define REQUEST_RESPONSIVENESS_AUTO() requestResponsiveness(__PRETTY_FUNCTION__)

#include <atomic>
#include <memory>
#include <stats/stats.h>
#include <sync.h>
#include <uint256.h>
#include <validation.h>

#include <interfaces/node.h>

class BanTableModel;
class CBlockIndex;
class OptionsModel;
class PeerTableModel;
class PeerTableSortProxy;
enum class SynchronizationState;

QT_BEGIN_NAMESPACE
class QTimer;
QT_END_NAMESPACE

enum class BlockSource {
    NONE,
    REINDEX,
    DISK,
    NETWORK
};

enum NumConnections {
    CONNECTIONS_NONE = 0,
    CONNECTIONS_IN   = (1U << 0),
    CONNECTIONS_OUT  = (1U << 1),
    CONNECTIONS_ALL  = (CONNECTIONS_IN | CONNECTIONS_OUT),
};

// Data Worker Class for handling cs_main operations in separate thread
class ClientModelDataWorker : public QObject
{
    Q_OBJECT

public:
    explicit ClientModelDataWorker(interfaces::Node& node, QObject* parent = nullptr);

public Q_SLOTS:
    // Data operations that require cs_main
    void getBlockSourceAsync();
    void getStatusBarWarningsAsync();
    void getMempoolStatsInRangeAsync(QDateTime from, QDateTime to);
    void updateNetworkAndMempoolStatsAsync();
    void getNumConnectionsAsync(unsigned int flags);
    void getNumBlocksAsync();
    void getBestBlockHashAsync();
    void getHeaderTipHeightAsync();
    void getHeaderTipTimeAsync();
    void getProxyInfoAsync();

Q_SIGNALS:
    // Results sent back to GUI thread
    void blockSourceResult(BlockSource result);
    void statusBarWarningsResult(QString warnings);
    void mempoolStatsResult(mempoolSamples_t samples);
    void mempoolStatsUpdated();
    void numConnectionsResult(int count);
    void numBlocksResult(int count);
    void bestBlockHashResult(uint256 hash);
    void headerTipHeightResult(int height);
    void headerTipTimeResult(int64_t time);
    void proxyInfoResult(bool hasProxy, QString ipPort);
    void mempoolSizeChanged(size_t count, size_t mempoolSizeInBytes);
    void bytesChanged(quint64 totalBytesIn, quint64 totalBytesOut);


private:
    interfaces::Node& m_node;
};

/** Model for Bitcoin network client. */
class ClientModel : public QObject
{
    Q_OBJECT

public:
    explicit ClientModel(interfaces::Node& node, OptionsModel *optionsModel, QObject *parent = nullptr);
    ~ClientModel();

    interfaces::Node& node() const { return m_node; }
    OptionsModel *getOptionsModel();
    PeerTableModel *getPeerTableModel();
    PeerTableSortProxy* peerTableSortProxy();
    BanTableModel *getBanTableModel();

    //! Return number of connections, default is in- and outbound (total)
    int getNumConnections(unsigned int flags = CONNECTIONS_ALL) const;
    int getNumBlocks() const;
    uint256 getBestBlockHash();
    int getHeaderTipHeight() const;
    size_t getMempoolDynamicUsage() const;
    int64_t getHeaderTipTime() const;

    //! Returns enum BlockSource of the current importing/syncing state
    enum BlockSource getBlockSource() const;
    //! Return warnings to be displayed in status bar
    QString getStatusBarWarnings() const;

    QString formatFullVersion() const;
    QString formatSubVersion() const;
    bool isReleaseVersion() const;
    QString formatClientStartupTime() const;
    QString dataDir() const;
    QString blocksDir() const;

    bool getProxyInfo(std::string& ip_port) const;

    // Cached data for GUI thread (updated via signals from data thread)
    mutable std::atomic<int> m_cached_num_connections{0};
    mutable std::atomic<int> m_cached_num_blocks{-1};
    mutable std::atomic<int> m_cached_header_height{-1};
    mutable std::atomic<int64_t> m_cached_header_time{-1};
    // Static global for atomic uint256 access
    static uint256 g_cached_best_block_hash;
    mutable std::atomic<uint256*> m_cached_best_block_hash_ptr{&g_cached_best_block_hash};
    mutable std::atomic<bool> m_cached_has_proxy{false};
    mutable QString m_cached_proxy_ip_port;
    mutable std::atomic<size_t> m_cached_mempool_dynamic_usage{0};
    mutable std::atomic<SynchronizationState> m_cached_sync_state{SynchronizationState::INIT_DOWNLOAD};
    mutable std::atomic<bool> m_cached_initial_sync_finished{false};
    mutable std::atomic<BlockSource> m_cached_block_source{BlockSource::NONE};
    mutable QString m_cached_status_bar_warnings;

    mempoolSamples_t getMempoolStatsInRange(QDateTime &from, QDateTime &to);

    typedef std::pair<int64_t, std::vector<interfaces::mempool_feeinfo>> mempool_feehist_sample; //!< sample plus timestamp
    mutable QMutex m_mempool_locker;
    const static size_t m_mempool_max_samples{540};
    const static size_t m_mempool_collect_intervall{20}; // 540*20 = 3h of sample window
    std::vector<mempool_feehist_sample> m_mempool_feehist;
    std::atomic<int64_t> m_mempool_feehist_last_sample_timestamp{0};

    // GUI responsiveness functions
    void requestResponsiveness(const char* reason = nullptr);
    void releaseResponsiveness();

    // Performance debugging
    QString getPerformanceStats() const;
    static int getSignalProcessingCount();
    static void resetSignalProcessingCount();

private:
    // Data processing thread for cs_main operations
    QThread* m_data_thread;
    QObject* m_data_worker;

    // Move data operations to separate thread
    void setupDataThread();
    void teardownDataThread();

    interfaces::Node& m_node;
    std::unique_ptr<interfaces::Handler> m_handler_show_progress;
    std::unique_ptr<interfaces::Handler> m_handler_notify_num_connections_changed;
    std::unique_ptr<interfaces::Handler> m_handler_notify_network_active_changed;
    std::unique_ptr<interfaces::Handler> m_handler_notify_alert_changed;
    std::unique_ptr<interfaces::Handler> m_handler_banned_list_changed;
    std::unique_ptr<interfaces::Handler> m_handler_notify_block_tip;
    std::unique_ptr<interfaces::Handler> m_handler_notify_header_tip;
    std::unique_ptr<interfaces::Handler> m_handler_notify_initial_sync_finished;
    boost::signals2::scoped_connection m_connection_mempool_stats_did_change;
    OptionsModel *optionsModel;
    PeerTableModel *peerTableModel;
    PeerTableSortProxy* m_peer_table_sort_proxy{nullptr};
    BanTableModel *banTableModel;

    //! A thread to interact with m_node asynchronously
    QThread* const m_thread;

    void subscribeToCoreSignals();
    void unsubscribeFromCoreSignals();

Q_SIGNALS:
    void numConnectionsChanged(int count);
    void numBlocksChanged(int count, const QDateTime& blockDate, double nVerificationProgress, bool header, SynchronizationState sync_state);
    void mempoolSizeChanged(long count, size_t mempoolSizeInBytes);
    void mempoolFeeHistChanged();
    void networkActiveChanged(bool networkActive);
    void alertsChanged(const QString &warnings);
    void bytesChanged(quint64 totalBytesIn, quint64 totalBytesOut);

    //! Fired when a message should be reported to the user
    void message(const QString &title, const QString &message, unsigned int style);

    // Show progress dialog e.g. for verifychain
    void showProgress(const QString &title, int nProgress);

    void mempoolStatsDidUpdate();

public Q_SLOTS:
    void updateNumConnections(int numConnections);
    void updateNetworkActive(bool networkActive);
    void updateAlert();
    void updateBanlist();
    void updateInitialSyncFinished();

    /* stats stack */
    void updateMempoolStats();
};

#endif // BITCOIN_QT_CLIENTMODEL_H
