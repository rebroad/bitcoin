// Copyright (c) 2011-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_RPCCONSOLE_H
#define BITCOIN_QT_RPCCONSOLE_H

#include <qt/guiutil.h>
#include <qt/peertablemodel.h>
#include <qt/mempoolstats.h>
#include <qt/blockvisualizationwidget.h>

#include <net.h>

#include <QByteArray>
#include <QCompleter>
#include <QThread>
#include <QWidget>

class ClientModel;
class PlatformStyle;
class RPCTimerInterface;
class WalletModel;

namespace interfaces {
    class Node;
    class Chain;
}

namespace Ui {
    class RPCConsole;
}

QT_BEGIN_NAMESPACE
class QDateTime;
class QMenu;
class QItemSelection;
class QTimer;
QT_END_NAMESPACE

/** Local Bitcoin RPC console. */
class RPCConsole: public QWidget
{
    Q_OBJECT

public:
    explicit RPCConsole(interfaces::Node& node, interfaces::Chain& chain, const PlatformStyle *platformStyle, QWidget *parent);
    ~RPCConsole();

    static bool RPCParseCommandLine(interfaces::Node* node, std::string &strResult, const std::string &strCommand, bool fExecute, std::string * const pstrFilteredOut = nullptr, const WalletModel* wallet_model = nullptr);
    static bool RPCExecuteCommandLine(interfaces::Node& node, std::string &strResult, const std::string &strCommand, std::string * const pstrFilteredOut = nullptr, const WalletModel* wallet_model = nullptr) {
        return RPCParseCommandLine(&node, strResult, strCommand, true, pstrFilteredOut, wallet_model);
    }

    void setClientModel(ClientModel *model = nullptr, int bestblock_height = 0, int64_t bestblock_date = 0, double verification_progress = 0.0);
    void addWallet(WalletModel * const walletModel);
    void removeWallet(WalletModel* const walletModel);

    enum MessageClass {
        MC_ERROR,
        MC_DEBUG,
        CMD_REQUEST,
        CMD_REPLY,
        CMD_ERROR
    };

    enum class TabTypes {
        INFO,
        CONSOLE,
        MEMPOOL,
        GRAPH,
        PEERS,
        BLOCKS
    };

    std::vector<TabTypes> tabs() const { return {TabTypes::INFO, TabTypes::CONSOLE, TabTypes::MEMPOOL, TabTypes::GRAPH, TabTypes::PEERS, TabTypes::BLOCKS}; }

    QString tabTitle(TabTypes tab_type) const;
    QKeySequence tabShortcut(TabTypes tab_type) const;

protected:
    virtual bool eventFilter(QObject* obj, QEvent *event) override;
    void keyPressEvent(QKeyEvent *) override;
    void changeEvent(QEvent* e) override;

private Q_SLOTS:
    void on_lineEdit_returnPressed();
    void on_tabWidget_currentChanged(int index);
    /** open the debug.log from the current datadir */
    void on_openDebugLogfileButton_clicked();
    /** change the time range of the network traffic graph */
    void on_sldGraphRange_valueChanged(int value);
    void on_sldGraphRange_sliderReleased();
    void on_sldGraphRange_sliderPressed();
    /** update traffic statistics */
    void updateTrafficStats(quint64 totalBytesIn, quint64 totalBytesOut);
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    /** Show custom context menu on Peers tab */
    void showPeersTableContextMenu(const QPoint& point);
    /** Show custom context menu on Bans tab */
    void showBanTableContextMenu(const QPoint& point);
    /** Hides ban table if no bans are present */
    void showOrHideBanTableIfRequired();
    /** clear the selected node */
    void clearSelectedNode();
    /** show detailed information on ui about selected node */
    void updateDetailWidget();
    /** update blocks information display */
    void updateBlocksDisplay();
    /** schedule a coalesced blocks tab refresh */
    void scheduleBlocksDisplayUpdate(int delay_ms = 75);
    /** schedule throttled legend refresh */
    void scheduleLegendUpdate(int delay_ms = 350);
    /** create and setup the block visualization widget */
    void setupBlockVisualizationWidget();
    /** update the legend display */
    void updateLegend();

public Q_SLOTS:
    void clear(bool keep_prompt = false);
    void fontBigger();
    void fontSmaller();
    void setFontSize(int newSize);
    /** Append the message to the message widget */
    void message(int category, const QString &msg) { message(category, msg, false); }
    void message(int category, const QString &message, bool html);
    /** Set number of connections shown in the UI */
    void setNumConnections(int count);
    /** Set network state shown in the UI */
    void setNetworkActive(bool networkActive);
    /** Set number of blocks and last block date shown in the UI */
    void setNumBlocks(int count, const QDateTime& blockDate, double nVerificationProgress, bool headers);
    /** Set size (number of transactions and memory usage) of the mempool in the UI */
    void setMempoolSize(long numberOfTxs, size_t dynUsage);
    /** Go forward or back in history */
    void browseHistory(int offset);
    /** Scroll console view to end */
    void scrollToEnd();
    /** Disconnect a selected node on the Peers tab */
    void disconnectSelectedNode();
    /** Remove selected peers from addnode list on the Peers tab */
    void removeSelectedNodeFromAddnode();
    /** Ban a selected node on the Peers tab */
    void banSelectedNode(int bantime);
    /** Unban a selected node on the Bans tab */
    void unbanSelectedNode();
    /** Ban ASN subnets for selected banned entries */
    void banSelectedAsn(int bantime);
    /** set which tab has the focus (is visible) */
    void setTabFocus(enum TabTypes tabType);
Q_SIGNALS:
    // For RPC command executor
    void cmdRequest(const QString &command, const WalletModel* wallet_model);

private:
    struct TranslatedStrings {
        const QString yes{tr("Yes")}, no{tr("No")}, to{tr("To")}, from{tr("From")},
            ban_for{tr("Ban for")}, na{tr("N/A")}, unknown{tr("Unknown")};
    } const ts;

    void startExecutor();
    void setTrafficGraphRange(int value);

    enum ColumnWidths {
        ADDRESS_COLUMN_WIDTH = 200,
        DIRECTION_COLUMN_WIDTH = 60,
        SUBVERSION_COLUMN_WIDTH = 150,
        PING_COLUMN_WIDTH = 80,
        BANSUBNET_COLUMN_WIDTH = 200,
        BANTIME_COLUMN_WIDTH = 250,
        STATUS_COLUMN_WIDTH = 100,
        BANCOUNT_COLUMN_WIDTH = 80,
        ASNCOLUMN_WIDTH = 280
    };

    interfaces::Node& m_node;
    interfaces::Chain& m_chain;
    Ui::RPCConsole* const ui;
    ClientModel *clientModel = nullptr;
    QStringList history;
    int historyPtr = 0;
    QString cmdBeforeBrowsing;
    QList<NodeId> cachedNodeids;
    const PlatformStyle* const platformStyle;
    RPCTimerInterface *rpcTimerInterface = nullptr;
    QMenu *peersTableContextMenu = nullptr;
    QMenu *banTableContextMenu = nullptr;
    QAction* m_remove_from_addnode_action = nullptr;
    QAction* m_unban_action = nullptr;
    QAction* m_ban_asn_1h_action = nullptr;
    QAction* m_ban_asn_1d_action = nullptr;
    QAction* m_ban_asn_1w_action = nullptr;
    QAction* m_ban_asn_1y_action = nullptr;
    int consoleFontSize = 0;
    QCompleter *autoCompleter = nullptr;
    QThread thread;
    WalletModel* m_last_wallet_model{nullptr};
    bool m_is_executing{false};
    QByteArray m_peer_widget_header_state;
    QByteArray m_banlist_widget_header_state;
    bool m_slider_in_use{false};
    int m_set_slider_value{0};
    bool m_blocks_display_dirty{false};
    int m_last_blocks_update_height{-1};
    bool m_seen_block_download_activity{false};
    QTimer* m_blocks_display_timer{nullptr};
    QTimer* m_legend_update_timer{nullptr};
    bool m_legend_dirty{false};

    /** Update UI with latest network info from model. */
    void updateNetworkState();
    /** Update local addresses shown in the Information tab. */
    void updateLocalAddresses();
    /** True when blocks tab work should run on this widget right now. */
    bool shouldRefreshBlockVisualization() const;

    /** Helper for the output of a time duration field. Inputs are UNIX epoch times. */
    QString TimeDurationField(std::chrono::seconds time_now, std::chrono::seconds time_at_event) const
    {
        return time_at_event.count() ? GUIUtil::formatDurationStr(time_now - time_at_event) : tr("Never");
    }

    BlockVisualizationWidget* m_blockVisualizationWidget = nullptr;

private Q_SLOTS:
    void updateAlerts(const QString& warnings);
};

#endif // BITCOIN_QT_RPCCONSOLE_H
