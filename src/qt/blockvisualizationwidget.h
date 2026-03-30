// Copyright (c) 2024 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_BLOCKVISUALIZATIONWIDGET_H
#define BITCOIN_QT_BLOCKVISUALIZATIONWIDGET_H

#include <QWidget>
#include <QTimer>
#include <QThread>
#include <QElapsedTimer>
#include <QPainter>
#include <QMouseEvent>
#include <QShowEvent>
#include <QHideEvent>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QToolTip>
#include <QDateTime>
#include <QCoreApplication>
#include <vector>
#include <optional>
#include <uint256.h>
#include <map>
#include <set>
#include <blockstatus_cache.h>

namespace interfaces {
    class Node;
    class Chain;
}

class BlockVisualizationWidget : public QWidget
{
    Q_OBJECT

public:
    enum BlockStatus {
        UNKNOWN,        // Status not yet determined
        NO_HEADER,      // Don't have the header
        HEADER_ONLY,    // Have header but no block data
        IN_FLIGHT,      // Requested and currently downloading
        COMPETING,      // Height has multiple known blocks (fork/side-chain competition)
        HAVE_BLOCK      // Have the full block
    };

    explicit BlockVisualizationWidget(interfaces::Node& node, interfaces::Chain& chain, QWidget *parent = nullptr);
    ~BlockVisualizationWidget();

    void updateBlockData();
    void refreshBlockStatus(int height);
    void refreshVisibleStatuses();
    void setDisplayTipHeight(int height);
    void centerBlockInView(int height);
    int countCachedBlocksByStatus(BlockStatus status) const;
    bool isDataLoaded() const { return m_dataLoaded; }
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    class BlockDataWorker;

    interfaces::Node& m_node;
    interfaces::Chain& m_chain;

    int m_blockWidth = 4;  // Width of each block in pixels
    int m_blockHeight = 4; // Height of each block in pixels
    int m_blocksPerRow = 100; // Number of blocks per row

    QTimer* m_resizeTimer = nullptr;
    QTimer* m_updateTimer = nullptr;
    QTimer* m_tooltip_timer = nullptr;
    QThread* m_worker_thread = nullptr;
    BlockDataWorker* m_worker = nullptr;
    bool m_initialized = false;
    bool m_dataLoaded = false;
    bool m_status_request_in_flight = false;
    bool m_auto_follow_tip = true;
    bool m_internal_scroll = false;
    bool m_ignore_scroll_tracking = false;
    bool m_shutting_down = false;

    // Block status caching
    std::map<int, BlockStatus> m_statusCache;
    std::map<int, QString> m_tooltipFullCache;
    std::set<int> m_pendingBlocks;
    std::set<int> m_pendingTooltipRequests;
    int m_totalBlocks = 0;
    int m_known_tip_height = 0;
    int m_backfill_cursor = 0;
    bool m_backfill_complete = false;
    int m_lowest_in_flight_height_hint = -1;
    int m_highest_pruned_height_hint = -1;
    bool m_is_initial_block_download = false;
    int m_last_hovered_block = -1;
    int m_latest_updated_height = -1;
    QPoint m_last_hover_global_pos;
    QElapsedTimer m_event_loop_timer;
    bool m_event_loop_timer_started = false;

    QColor getColorForStatus(BlockStatus status) const;
    BlockStatus getDisplayStatus(int height) const;
    QString getTooltipForBlock(int height) const;
    QString getTooltipForBlockLightweight(int height) const;
    void dispatchStatusBatch();
    void requestTooltipDetails(int height);
    void processTooltipHover();
    void onScrollValueChanged(int value);
    void attachScrollTracking();
    void scheduleStatusRefresh(bool urgent = false);
    bool isShuttingDownNow();
    void onStatusObserved(int height, BlockStatus status);
    void recomputeLowestInFlightHint();
    int getBlockIndexFromPosition(const QPoint& pos) const;
    void calculateLayout();
    void updateBlockStatusesAsync();
    void drawLegend(QPainter& painter);
};

#endif // BITCOIN_QT_BLOCKVISUALIZATIONWIDGET_H
