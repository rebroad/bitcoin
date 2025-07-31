// Copyright (c) 2011-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_CLIENTMODEL_H
#define BITCOIN_QT_CLIENTMODEL_H

#include <QMutex>
#include <QObject>
#include <QDateTime>

#include <atomic>
#include <memory>
#include <stats/stats.h>
#include <sync.h>
#include <uint256.h>

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

    // Performance monitoring methods (for debugging)
    QString getCacheStats() const;
    bool isCacheValid() const;
    void forceCacheRefresh();

    // Cached traffic data for debug components
    int64_t getCachedBytesRecv() const;
    int64_t getCachedBytesSent() const;

    // Cached peer data for debug components
    bool getCachedPeerStats(interfaces::Node::NodesStats& stats) const;

    // Cached mempool fee histogram for debug components
    bool getCachedFeeHistogram(interfaces::mempool_feehistogram& histogram) const;

    // Legacy caching - now replaced by m_gui_data (kept for compatibility)
    mutable std::atomic<int> m_cached_num_blocks{-1};

    Mutex m_cached_tip_mutex;
    uint256 m_cached_tip_blocks GUARDED_BY(m_cached_tip_mutex){};

    mempoolSamples_t getMempoolStatsInRange(QDateTime &from, QDateTime &to);

    typedef std::pair<int64_t, std::vector<interfaces::mempool_feeinfo>> mempool_feehist_sample; //!< sample plus timestamp
    mutable QMutex m_mempool_locker;
    const static size_t m_mempool_max_samples{540};
    const static size_t m_mempool_collect_intervall{20}; // 540*20 = 3h of sample window
    std::vector<mempool_feehist_sample> m_mempool_feehist;
    std::atomic<int64_t> m_mempool_feehist_last_sample_timestamp{0};

    // GUI-specific data layer - single buffer for signal-based updates
    struct GuiData {
        int numBlocks{-1};
        int headerHeight{-1};
        int64_t headerTime{-1};
        uint256 bestBlockHash;
        bool initialSyncFinished{false};
        int numConnectionsIn{0};
        int numConnectionsOut{0};
        int numConnectionsTotal{0};
        size_t mempoolSize{0};
        size_t mempoolDynamicUsage{0};
        int64_t bytesRecv{0};
        int64_t bytesSent{0};

        // Traffic graph data
        int64_t trafficBytesRecv{0};
        int64_t trafficBytesSent{0};

        // Peer data cache
        interfaces::Node::NodesStats peerStats;
        int64_t lastPeerUpdateTime{0};

        // Mempool fee histogram cache
        interfaces::mempool_feehistogram feeHistogram;
        int64_t lastFeeHistogramUpdateTime{0};

        // Performance monitoring metrics
        int64_t lastUpdateTime{0};
        int64_t updateCount{0};
    };

    // Single buffer for signal-based updates
    GuiData m_gui_data;  // Updated directly by signals in GUI thread

private:
    interfaces::Node& m_node;
    std::unique_ptr<interfaces::Handler> m_handler_show_progress;
    std::unique_ptr<interfaces::Handler> m_handler_notify_num_connections_changed;
    std::unique_ptr<interfaces::Handler> m_handler_notify_network_active_changed;
    std::unique_ptr<interfaces::Handler> m_handler_notify_alert_changed;
    std::unique_ptr<interfaces::Handler> m_handler_banned_list_changed;
    std::unique_ptr<interfaces::Handler> m_handler_notify_block_tip;
    std::unique_ptr<interfaces::Handler> m_handler_notify_header_tip;
    boost::signals2::scoped_connection m_connection_mempool_stats_did_change;
    OptionsModel *optionsModel;
    PeerTableModel *peerTableModel;
    PeerTableSortProxy* m_peer_table_sort_proxy{nullptr};
    BanTableModel *banTableModel;

    void subscribeToCoreSignals();
    void unsubscribeFromCoreSignals();
    void initializeData();

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

    /* stats stack */
    void updateMempoolStats();
    void updateHeaderTip(int height, int64_t blockTime);
    void updateBlockData(int numBlocks, const uint256& bestBlockHash, bool initialSyncFinished);
    void updateConnectionData(int connectionsIn, int connectionsOut, int connectionsTotal);
    void updateNetworkData(int64_t bytesRecv, int64_t bytesSent);
    void updatePeerStats(const interfaces::Node::NodesStats& stats);
    void updateFeeHistogram(const interfaces::mempool_feehistogram& histogram);
};

#endif // BITCOIN_QT_CLIENTMODEL_H
