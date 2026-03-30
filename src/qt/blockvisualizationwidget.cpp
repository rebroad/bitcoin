// Copyright (c) 2024 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <qt/blockvisualizationwidget.h>
#include <interfaces/chain.h>
#include <interfaces/node.h>
#include <chain.h>
#include <validation.h>
#include <node/blockstorage.h>
#include <util/time.h>
#include <primitives/block.h>
#include <serialize.h>
#include <logging.h>
#include <QApplication>
#include <QPainter>
#include <QMouseEvent>
#include <QToolTip>
#include <QDateTime>
#include <QMetaObject>
#include <QScreen>
#include <QScrollBar>
#include <QScrollArea>
#include <algorithm>
#include <cmath>
#include <map>

static QScrollArea* FindParentScrollArea(QWidget* widget)
{
    QWidget* current = widget ? widget->parentWidget() : nullptr;
    while (current) {
        if (auto* scroll_area = qobject_cast<QScrollArea*>(current)) return scroll_area;
        current = current->parentWidget();
    }
    return nullptr;
}

static BlockVisualizationWidget::BlockStatus ToWidgetStatus(const BlockStatusCache::BlockStatus status)
{
    switch (status) {
    case BlockStatusCache::UNKNOWN: return BlockVisualizationWidget::UNKNOWN;
    case BlockStatusCache::NO_HEADER: return BlockVisualizationWidget::NO_HEADER;
    case BlockStatusCache::HEADER_ONLY: return BlockVisualizationWidget::HEADER_ONLY;
    case BlockStatusCache::HAVE_BLOCK: return BlockVisualizationWidget::HAVE_BLOCK;
    }
    return BlockVisualizationWidget::UNKNOWN;
}

class BlockVisualizationWidget::BlockDataWorker : public QObject
{
public:
    explicit BlockDataWorker(interfaces::Chain& chain) : m_chain(chain) {}
    void requestStatusesBatch(const QVector<int>& heights, QVector<int>& out_statuses, qint64& elapsed_ms)
    {
        QElapsedTimer timer;
        timer.start();
        out_statuses.clear();
        out_statuses.reserve(heights.size());
        for (const int height : heights) {
            out_statuses.push_back(static_cast<int>(fetchStatus(height)));
        }
        elapsed_ms = timer.elapsed();
    }

    QString requestTooltipData(int height, qint64& elapsed_ms)
    {
        QElapsedTimer timer;
        timer.start();
        const QString tooltip = buildFullTooltip(height);
        elapsed_ms = timer.elapsed();
        return tooltip;
    }

private:
    interfaces::Chain& m_chain;

    BlockVisualizationWidget::BlockStatus fetchStatus(int height) const
    {
        try {
            const uint256 blockHash = m_chain.getBlockHash(height);
            if (blockHash.IsNull()) return BlockVisualizationWidget::NO_HEADER;
            BlockVisualizationWidget::BlockStatus status = BlockVisualizationWidget::HEADER_ONLY;
            if (m_chain.haveBlockOnDisk(height)) {
                status = BlockVisualizationWidget::HAVE_BLOCK;
            } else if (m_chain.isBlockInFlight(height)) {
                status = BlockVisualizationWidget::IN_FLIGHT;
            }
            if (m_chain.hasCompetingBlocks(height)) {
                status = BlockVisualizationWidget::COMPETING;
            }
            return status;
        } catch (...) {
            return BlockVisualizationWidget::UNKNOWN;
        }
    }

    QString buildFullTooltip(int height) const
    {
        if (height < 0) return QStringLiteral("Invalid block");

        const auto status_to_text = [](BlockVisualizationWidget::BlockStatus status) {
            switch (status) {
            case BlockVisualizationWidget::NO_HEADER: return QStringLiteral("No header");
            case BlockVisualizationWidget::HEADER_ONLY: return QStringLiteral("Header only");
            case BlockVisualizationWidget::IN_FLIGHT: return QStringLiteral("In flight");
            case BlockVisualizationWidget::COMPETING: return QStringLiteral("Competing blocks");
            case BlockVisualizationWidget::HAVE_BLOCK: return QStringLiteral("Have block");
            case BlockVisualizationWidget::UNKNOWN:
            default: return QStringLiteral("Unknown");
            }
        };

        QString tooltip = QStringLiteral("Block %1\nStatus: %2").arg(height).arg(status_to_text(fetchStatus(height)));
        try {
            const uint256 blockHash = m_chain.getBlockHash(height);
            if (!blockHash.IsNull()) {
                int64_t blockTime = 0;
                interfaces::FoundBlock foundBlock;
                foundBlock.time(blockTime);
                if (m_chain.findBlock(blockHash, foundBlock) && foundBlock.found && blockTime > 0) {
                    tooltip += QStringLiteral("\nTime: %1")
                                   .arg(QDateTime::fromSecsSinceEpoch(blockTime).toString(QStringLiteral("yyyy-MM-dd hh:mm:ss")));
                }

                if (fetchStatus(height) == BlockVisualizationWidget::HAVE_BLOCK) {
                    CBlock blockData;
                    interfaces::FoundBlock dataBlock;
                    dataBlock.data(blockData);
                    if (m_chain.findBlock(blockHash, dataBlock) && dataBlock.found && !blockData.IsNull()) {
                        const size_t blockSize = ::GetSerializeSize(blockData, PROTOCOL_VERSION);
                        tooltip += QStringLiteral("\nSize: %1 bytes").arg(blockSize);
                    }
                }
            }

            const auto blocks_at_height = m_chain.getBlocksAtHeight(height);
            if (blocks_at_height.size() > 1) {
                tooltip += QStringLiteral("\nCompeting blocks (%1):").arg(blocks_at_height.size());
                for (const auto& block : blocks_at_height) {
                    QString flags;
                    if (block.in_active_chain) flags += QStringLiteral(" active");
                    if (block.have_data) flags += QStringLiteral(" data");
                    if (flags.isEmpty()) flags = QStringLiteral(" header-only");
                    tooltip += QStringLiteral("\n- %1 (%2)")
                                   .arg(QString::fromStdString(block.hash.ToString().substr(0, 16)))
                                   .arg(flags.trimmed());
                }
            }
        } catch (...) {
        }
        return tooltip;
    }
};

BlockVisualizationWidget::BlockVisualizationWidget(interfaces::Node& node, interfaces::Chain& chain, QWidget *parent)
    : QWidget(parent)
    , m_node(node)
    , m_chain(chain)
{
    setMouseTracking(true);
    setMinimumWidth(0);
    setMinimumHeight(200);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);

    // Set background color to match system theme
    setAutoFillBackground(true);
    QPalette pal = palette();
    // Use the system background color instead of white
    pal.setColor(QPalette::Window, pal.color(QPalette::Base));
    setPalette(pal);

    // Initialize resize timer for efficient layout updates
    m_resizeTimer = new QTimer(this);
    m_resizeTimer->setSingleShot(true);
    m_resizeTimer->setInterval(100); // 100ms delay
    connect(m_resizeTimer, &QTimer::timeout, [this]() {
        calculateLayout();
        update();
    });

    // Initialize update timer for asynchronous block status updates.
    m_updateTimer = new QTimer(this);
    m_updateTimer->setSingleShot(true);
    m_updateTimer->setInterval(75); // Coalesced updates: keep UI responsive during IBD bursts.
    connect(m_updateTimer, &QTimer::timeout, [this]() {
        updateBlockStatusesAsync();
    });

    // Debounce hover processing so rapid mouse movement does not trigger costly fetches.
    m_tooltip_timer = new QTimer(this);
    m_tooltip_timer->setSingleShot(true);
    m_tooltip_timer->setInterval(90);
    connect(m_tooltip_timer, &QTimer::timeout, this, &BlockVisualizationWidget::processTooltipHover);

    // Worker thread for chain/status/tooltip lookups.
    m_worker_thread = new QThread(this);
    m_worker = new BlockDataWorker(m_chain);
    m_worker->moveToThread(m_worker_thread);
    connect(m_worker_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    m_worker_thread->start();

    // Don't update block data here - wait until the widget is shown.
}

BlockVisualizationWidget::~BlockVisualizationWidget()
{
    if (m_worker_thread) {
        m_worker_thread->quit();
        m_worker_thread->wait();
    }
}

void BlockVisualizationWidget::updateBlockData()
{
    if (!m_chain.isUsable()) return;

    int numBlocks = 0;
    try {
        numBlocks = m_node.getNumBlocks();
    } catch (...) {
        // Chainman not available yet, just return
        return;
    }
    if (numBlocks <= 0) return;

    // Check if global cache is populated
    BlockStatusCache& globalCache = BlockStatusCache::getInstance();

    const int old_total_blocks = m_totalBlocks;
    if (globalCache.isPopulated()) {
        // Use startup cache as a baseline, but prefer current runtime height.
        m_totalBlocks = std::max(globalCache.getTotalBlocks(), numBlocks);
        m_dataLoaded = true;
        m_initialized = true;
    } else {
        m_totalBlocks = numBlocks;
        // Avoid bulk synchronous scans on GUI thread; statuses are filled asynchronously.
        m_dataLoaded = true;
        m_initialized = true;
    }

    if (m_totalBlocks != old_total_blocks) {
        calculateLayout();
        update();
        for (int height = std::max(0, old_total_blocks + 1); height <= m_totalBlocks; ++height) {
            m_pendingBlocks.insert(height);
        }
    }
    if (isVisible()) {
        refreshVisibleStatuses();
        m_updateTimer->start();
    }
}

void BlockVisualizationWidget::refreshBlockStatus(int height)
{
    if (height < 0 || height > m_totalBlocks) {
        return;
    }

    m_latest_updated_height = height;
    m_pendingBlocks.insert(height);
    m_tooltipFullCache.erase(height);
    m_updateTimer->start();
}

void BlockVisualizationWidget::refreshVisibleStatuses()
{
    if (!m_chain.isUsable() || m_totalBlocks <= 0) return;

    QScrollArea* scrollArea = FindParentScrollArea(this);
    int visibleStart = 0;
    int visibleEnd = m_totalBlocks;
    if (scrollArea) {
        if (QScrollBar* vbar = scrollArea->verticalScrollBar()) {
            const int scrollPos = vbar->value();
            const int visibleHeight = scrollArea->viewport()->height();
            if (m_blocksPerRow > 0 && m_blockHeight > 0) {
                const int startRow = scrollPos / m_blockHeight;
                const int endRow = (scrollPos + visibleHeight) / m_blockHeight;
                visibleStart = std::max(0, startRow * m_blocksPerRow);
                visibleEnd = std::min((endRow + 1) * m_blocksPerRow, m_totalBlocks);
            }
        }
    }

    for (int height = visibleStart; height <= visibleEnd; ++height) {
        m_pendingBlocks.insert(height);
    }
    if (!m_updateTimer->isActive()) m_updateTimer->start();
}

int BlockVisualizationWidget::countCachedBlocksByStatus(BlockStatus status) const
{
    int count = 0;
    for (const auto& [_, cached_status] : m_statusCache) {
        if (cached_status == status) ++count;
    }
    return count;
}

void BlockVisualizationWidget::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);

    // Ensure we get the proper size from the parent scroll area
    if (parentWidget()) {
        QTimer::singleShot(0, this, [this]() {
            calculateLayout();
            attachScrollTracking();
        });
    }

    if (m_chain.isUsable()) m_updateTimer->start();
}

void BlockVisualizationWidget::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    if (m_updateTimer->isActive()) m_updateTimer->stop();
    if (m_tooltip_timer->isActive()) m_tooltip_timer->stop();
}

void BlockVisualizationWidget::updateBlockStatusesAsync()
{
    if (!m_chain.isUsable()) {
        if (m_updateTimer->isActive()) m_updateTimer->stop();
        return;
    }

    if (m_event_loop_timer_started) {
        const qint64 lag_ms = m_event_loop_timer.elapsed();
        if (lag_ms > 250) {
            LogPrint(BCLog::QT, "BlockVisualizationWidget GUI event-loop lag=%d ms\n", static_cast<int>(lag_ms));
        }
    }
    m_event_loop_timer.restart();
    m_event_loop_timer_started = true;

    // Get visible block range to prioritize them.
    QScrollArea* scrollArea = FindParentScrollArea(this);
    int visibleStart = 0;
    int visibleEnd = m_totalBlocks;
    if (scrollArea && m_blocksPerRow > 0 && m_blockHeight > 0) {
        if (QScrollBar* vbar = scrollArea->verticalScrollBar()) {
            const int scrollPos = vbar->value();
            const int visibleHeight = scrollArea->viewport()->height();
            const int startRow = scrollPos / m_blockHeight;
            const int endRow = (scrollPos + visibleHeight) / m_blockHeight;
            visibleStart = std::max(0, startRow * m_blocksPerRow);
            visibleEnd = std::min((endRow + 1) * m_blocksPerRow, m_totalBlocks);
        }
    }

    if (m_pendingBlocks.empty() && m_totalBlocks > 0 && visibleEnd >= visibleStart) {
        for (int height = visibleStart; height <= visibleEnd; ++height) {
            m_pendingBlocks.insert(height);
        }
    }

    if (m_pendingBlocks.empty()) return;
    if (m_status_request_in_flight) {
        // Keep timer ticking while a worker request is in-flight.
        m_updateTimer->start();
        return;
    }

    dispatchStatusBatch();
}

void BlockVisualizationWidget::centerBlockInView(int height)
{
    if (height < 0 || m_totalBlocks <= 0 || m_blocksPerRow <= 0 || m_blockHeight <= 0) {
        return;
    }
    m_latest_updated_height = std::min(height, m_totalBlocks);
    if (!m_auto_follow_tip) return;

    QScrollArea* scrollArea = FindParentScrollArea(this);
    if (!scrollArea) return;
    QScrollBar* vbar = scrollArea->verticalScrollBar();
    if (!vbar) return;

    const int clamped_height = m_latest_updated_height;
    const int row = clamped_height / m_blocksPerRow;
    const int block_center_y = row * m_blockHeight + (m_blockHeight / 2);
    const int viewport_half = scrollArea->viewport()->height() / 2;
    const int target = std::clamp(block_center_y - viewport_half, 0, vbar->maximum());
    m_internal_scroll = true;
    vbar->setValue(target);
    m_internal_scroll = false;
}

void BlockVisualizationWidget::dispatchStatusBatch()
{
    if (m_pendingBlocks.empty()) return;
    QVector<int> batch;
    batch.reserve(256);

    // Prefer visible range entries first.
    QScrollArea* scrollArea = FindParentScrollArea(this);
    int visibleStart = 0;
    int visibleEnd = m_totalBlocks;
    if (scrollArea && m_blocksPerRow > 0 && m_blockHeight > 0) {
        if (QScrollBar* vbar = scrollArea->verticalScrollBar()) {
            const int scrollPos = vbar->value();
            const int visibleHeight = scrollArea->viewport()->height();
            visibleStart = std::max(0, (scrollPos / m_blockHeight) * m_blocksPerRow);
            visibleEnd = std::min(((scrollPos + visibleHeight) / m_blockHeight + 1) * m_blocksPerRow, m_totalBlocks);
        }
    }

    for (auto it = m_pendingBlocks.lower_bound(visibleStart);
         it != m_pendingBlocks.end() && *it <= visibleEnd && batch.size() < 256;) {
        batch.push_back(*it);
        it = m_pendingBlocks.erase(it);
    }
    for (auto it = m_pendingBlocks.begin(); it != m_pendingBlocks.end() && batch.size() < 256;) {
        batch.push_back(*it);
        it = m_pendingBlocks.erase(it);
    }

    if (batch.empty()) return;
    m_status_request_in_flight = true;
    BlockDataWorker* const worker = m_worker;
    QMetaObject::invokeMethod(worker, [this, worker, batch]() {
        QVector<int> statuses;
        qint64 elapsed_ms{0};
        worker->requestStatusesBatch(batch, statuses, elapsed_ms);
        QMetaObject::invokeMethod(this, [this, batch, statuses, elapsed_ms]() {
            if (batch.size() != statuses.size()) {
                m_status_request_in_flight = false;
                return;
            }

            for (int i = 0; i < batch.size(); ++i) {
                const int height = batch[i];
                if (height < 0 || height > m_totalBlocks) continue;
                const BlockStatus new_status = static_cast<BlockStatus>(statuses[i]);
                const auto it = m_statusCache.find(height);
                if (it == m_statusCache.end() || it->second != new_status) {
                    m_statusCache[height] = new_status;
                    m_tooltipFullCache.erase(height);
                    if (m_blocksPerRow > 0 && m_blockWidth > 0 && m_blockHeight > 0) {
                        const int row = height / m_blocksPerRow;
                        const int col = height % m_blocksPerRow;
                        update(QRect(col * m_blockWidth, row * m_blockHeight, m_blockWidth, m_blockHeight));
                    } else {
                        update();
                    }
                }
            }

            m_status_request_in_flight = false;
            if (!m_pendingBlocks.empty()) m_updateTimer->start();
            if (elapsed_ms > 20) {
                LogPrint(BCLog::QT, "BlockVisualizationWidget::requestStatusesBatch took %d ms for %d heights\n",
                         static_cast<int>(elapsed_ms), batch.size());
            }
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
}

void BlockVisualizationWidget::calculateLayout()
{
    if (m_totalBlocks <= 0) return;

    // Get the available width from the parent scroll area
    int availableWidth = width();
    if (QScrollArea* scrollArea = FindParentScrollArea(this)) {
        availableWidth = scrollArea->viewport()->width();
    }
    if (availableWidth <= 0) {
        // If width is not set yet, use a reasonable default
        availableWidth = 800;
    }

    // Keep tile size fixed (DPI-scaled); resizing should reveal more columns, not enlarge tiles.
    int totalBlocks = m_totalBlocks + 1; // +1 because we include block 0
    const double dpi_scale = (screen() ? screen()->logicalDotsPerInch() : 96.0) / 96.0;
    const int base_size = std::clamp(static_cast<int>(std::lround(5.0 * dpi_scale)), 5, 12);
    m_blockWidth = base_size;
    m_blockHeight = base_size;

    // Calculate how many blocks fit per row
    m_blocksPerRow = availableWidth / m_blockWidth;
    if (m_blocksPerRow <= 0) m_blocksPerRow = 1;

    // Calculate total height needed for all blocks
    int numRows = (totalBlocks + m_blocksPerRow - 1) / m_blocksPerRow; // Ceiling division
    int totalHeight = numRows * m_blockHeight; // No need for legend space

    // Set the widget's size for scrolling
    setMinimumWidth(0);
    setMinimumHeight(totalHeight);
    resize(availableWidth, totalHeight);

    // Force a repaint
    update();
}

QColor BlockVisualizationWidget::getColorForStatus(BlockStatus status) const
{
    switch (status) {
        case UNKNOWN:
            return QColor("#6B7280");
        case NO_HEADER:
            return QColor("#9AA0A6");
        case HEADER_ONLY:
            return QColor("#E69F00");
        case IN_FLIGHT:
            return QColor("#06B6D4");
        case COMPETING:
            return QColor("#7C3AED");
        case HAVE_BLOCK:
            return QColor("#009E73");
        default:
            return QColor("#6B7280");
    }
}

BlockVisualizationWidget::BlockStatus BlockVisualizationWidget::getDisplayStatus(int height) const
{
    BlockStatus status = UNKNOWN;
    auto it = m_statusCache.find(height);
    if (it != m_statusCache.end()) {
        status = it->second;
    } else {
        BlockStatusCache& globalCache = BlockStatusCache::getInstance();
        if (globalCache.isPopulated() && height <= globalCache.getTotalBlocks()) {
            status = ToWidgetStatus(globalCache.getStatus(height));
        }
    }
    return status;
}

QString BlockVisualizationWidget::getTooltipForBlock(int height) const
{
    if (height < 0 || height > m_totalBlocks) {
        return tr("Invalid block");
    }

    if (const auto it = m_tooltipFullCache.find(height); it != m_tooltipFullCache.end()) {
        return it->second;
    }
    return getTooltipForBlockLightweight(height);
}

QString BlockVisualizationWidget::getTooltipForBlockLightweight(int height) const
{
    if (height < 0 || height > m_totalBlocks) {
        return tr("Invalid block");
    }

    const BlockStatus status = getDisplayStatus(height);

    QString statusText;
    switch (status) {
    case UNKNOWN:
        statusText = tr("Unknown");
        break;
    case NO_HEADER:
        statusText = tr("No header");
        break;
    case HEADER_ONLY:
        statusText = tr("Header only");
        break;
    case IN_FLIGHT:
        statusText = tr("In flight");
        break;
    case COMPETING:
        statusText = tr("Competing blocks");
        break;
    case HAVE_BLOCK:
        statusText = tr("Have block");
        break;
    }

    return tr("Block %1\nStatus: %2\nLoading details…").arg(height).arg(statusText);
}

void BlockVisualizationWidget::requestTooltipDetails(int height)
{
    if (height < 0 || height > m_totalBlocks) return;
    if (m_tooltipFullCache.find(height) != m_tooltipFullCache.end()) return;
    if (m_pendingTooltipRequests.count(height) > 0) return;
    m_pendingTooltipRequests.insert(height);
    BlockDataWorker* const worker = m_worker;
    QMetaObject::invokeMethod(worker, [this, worker, height]() {
        qint64 elapsed_ms{0};
        const QString tooltip = worker->requestTooltipData(height, elapsed_ms);
        QMetaObject::invokeMethod(this, [this, height, tooltip, elapsed_ms]() {
            m_pendingTooltipRequests.erase(height);
            m_tooltipFullCache[height] = tooltip;
            if (height == m_last_hovered_block) {
                QToolTip::showText(m_last_hover_global_pos, tooltip, this);
            }
            if (elapsed_ms > 25) {
                LogPrint(BCLog::QT, "BlockVisualizationWidget::requestTooltipData took %d ms at height %d\n",
                         static_cast<int>(elapsed_ms), height);
            }
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
}

void BlockVisualizationWidget::processTooltipHover()
{
    if (m_last_hovered_block < 0 || m_last_hovered_block > m_totalBlocks) return;
    QToolTip::showText(m_last_hover_global_pos, getTooltipForBlock(m_last_hovered_block), this);
    requestTooltipDetails(m_last_hovered_block);
}

void BlockVisualizationWidget::onScrollValueChanged(int value)
{
    if (m_internal_scroll || m_latest_updated_height < 0 || m_blocksPerRow <= 0 || m_blockHeight <= 0) {
        return;
    }

    QScrollArea* scrollArea = FindParentScrollArea(this);
    if (!scrollArea) return;
    QScrollBar* vbar = scrollArea->verticalScrollBar();
    if (!vbar) return;

    const int row = m_latest_updated_height / m_blocksPerRow;
    const int block_center_y = row * m_blockHeight + (m_blockHeight / 2);
    const int viewport_half = scrollArea->viewport()->height() / 2;
    const int target = std::clamp(block_center_y - viewport_half, 0, vbar->maximum());
    const int follow_window = std::max(2 * m_blockHeight, 20);
    m_auto_follow_tip = std::abs(value - target) <= follow_window;
}

void BlockVisualizationWidget::attachScrollTracking()
{
    QScrollArea* scrollArea = FindParentScrollArea(this);
    if (!scrollArea) return;
    if (QScrollBar* vbar = scrollArea->verticalScrollBar()) {
        connect(vbar, &QScrollBar::valueChanged, this, &BlockVisualizationWidget::onScrollValueChanged, Qt::UniqueConnection);
    }
}

int BlockVisualizationWidget::getBlockIndexFromPosition(const QPoint& pos) const
{
    if (m_totalBlocks <= 0 || m_blocksPerRow <= 0) return -1;

    int row = pos.y() / m_blockHeight;
    int col = pos.x() / m_blockWidth;

    int blockIndex = row * m_blocksPerRow + col;

    if (blockIndex >= 0 && blockIndex <= m_totalBlocks) {
        return blockIndex;
    }

    return -1;
}

void BlockVisualizationWidget::paintEvent(QPaintEvent *event)
{
    QElapsedTimer paint_timer;
    paint_timer.start();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, false); // Disable antialiasing for better performance

    if (m_totalBlocks <= 0) {
        painter.drawText(rect(), Qt::AlignCenter, tr("No block data available"));
        return;
    }

    // Pre-calculate colors for better performance
    QColor colors[HAVE_BLOCK + 1];
    colors[0] = getColorForStatus(UNKNOWN);
    colors[1] = getColorForStatus(NO_HEADER);
    colors[2] = getColorForStatus(HEADER_ONLY);
    colors[3] = getColorForStatus(IN_FLIGHT);
    colors[4] = getColorForStatus(COMPETING);
    colors[5] = getColorForStatus(HAVE_BLOCK);

    const QRect dirty = event ? event->rect() : rect();
    const int start_row = std::max(0, dirty.top() / m_blockHeight);
    const int end_row = std::max(start_row, std::min((m_totalBlocks / m_blocksPerRow), dirty.bottom() / m_blockHeight + 1));
    const int start_height = start_row * m_blocksPerRow;
    const int end_height = std::min(m_totalBlocks, ((end_row + 1) * m_blocksPerRow) - 1);

    for (int height = start_height; height <= end_height; ++height) {
        int row = height / m_blocksPerRow;
        int col = height % m_blocksPerRow;

        int x = col * m_blockWidth;
        int y = row * m_blockHeight;

        QRect blockRect(x, y, m_blockWidth, m_blockHeight);

        // Prefer locally refreshed status, fallback to startup cache.
        const BlockStatus status = getDisplayStatus(height);

        // Draw block with appropriate color
        painter.fillRect(blockRect, colors[static_cast<int>(status)]);

        // Draw border only if blocks are large enough
        if (m_blockWidth > 3 && m_blockHeight > 3) {
            painter.setPen(QPen(Qt::black, 1));
            painter.drawRect(blockRect);
        }
    }

    const qint64 elapsed = paint_timer.elapsed();
    if (elapsed > 20) {
        LogPrint(BCLog::QT, "BlockVisualizationWidget::paintEvent took %d ms (rows %d-%d)\n",
                 static_cast<int>(elapsed), start_row, end_row);
    }
}

void BlockVisualizationWidget::drawLegend(QPainter& painter)
{
    int legendX = 10;
    int legendY = height() - 120;
    int legendWidth = 200;
    int legendHeight = 100;

    // Draw legend background
    painter.fillRect(legendX, legendY, legendWidth, legendHeight, QColor(255, 255, 255, 200));
    painter.setPen(QPen(Qt::black, 1));
    painter.drawRect(legendX, legendY, legendWidth, legendHeight);

    // Draw legend title
    painter.setFont(QFont("Arial", 8, QFont::Bold));
    painter.drawText(legendX + 5, legendY + 15, tr("Block Status Legend"));

    // Draw legend items
    painter.setFont(QFont("Arial", 7));
    int itemY = legendY + 25;
    int itemHeight = 12;

    struct LegendItem {
        BlockStatus status;
        QString text;
    };

    std::vector<LegendItem> legendItems = {
        {HAVE_BLOCK, tr("Have Block")},
        {IN_FLIGHT, tr("In Flight")},
        {HEADER_ONLY, tr("Header Only")},
        {COMPETING, tr("Competing")},
        {NO_HEADER, tr("No Header")}
    };

    for (const auto& item : legendItems) {
        // Draw color box
        QRect colorRect(legendX + 5, itemY, 10, 8);
        painter.fillRect(colorRect, getColorForStatus(item.status));
        painter.drawRect(colorRect);

        // Draw text
        painter.drawText(legendX + 20, itemY + 7, item.text);

        itemY += itemHeight;
    }
}

void BlockVisualizationWidget::mouseMoveEvent(QMouseEvent *event)
{
    const int blockIndex = getBlockIndexFromPosition(event->pos());
    if (blockIndex >= 0 && blockIndex <= m_totalBlocks) {
        m_last_hovered_block = blockIndex;
        m_last_hover_global_pos = event->globalPos();
        m_tooltip_timer->start();
    } else {
        m_last_hovered_block = -1;
        if (m_tooltip_timer->isActive()) m_tooltip_timer->stop();
        QToolTip::hideText();
    }

    QWidget::mouseMoveEvent(event);
}

void BlockVisualizationWidget::mousePressEvent(QMouseEvent *event)
{
    int blockIndex = getBlockIndexFromPosition(event->pos());

    if (blockIndex >= 0 && blockIndex <= m_totalBlocks) {
        // Reserved for future click handling.
    }

    QWidget::mousePressEvent(event);
}

void BlockVisualizationWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    // Use timer to avoid too frequent recalculations during resize
    if (m_resizeTimer) {
        m_resizeTimer->start();
    }
}
