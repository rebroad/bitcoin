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
#include <QApplication>
#include <QPainter>
#include <QMouseEvent>
#include <QToolTip>
#include <QDateTime>
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

    // Initialize update timer for asynchronous block status updates
    m_updateTimer = new QTimer(this);
    m_updateTimer->setSingleShot(true);
    m_updateTimer->setInterval(250); // Responsive refresh for visible statuses
    connect(m_updateTimer, &QTimer::timeout, [this]() {
        updateBlockStatusesAsync();
        if (isVisible()) m_updateTimer->start();
    });

    // Don't update block data here - wait until the widget is shown
}

BlockVisualizationWidget::~BlockVisualizationWidget()
{
}

void BlockVisualizationWidget::updateBlockData()
{
    if (!m_chain.isUsable()) return;

    // Check if global cache is populated
    BlockStatusCache& globalCache = BlockStatusCache::getInstance();

    if (globalCache.isPopulated()) {
        // Use the global cache that was populated during startup
        m_totalBlocks = globalCache.getTotalBlocks();
        m_dataLoaded = true;
        m_initialized = true;
    } else {
        // Fallback to the old method if global cache isn't populated
        int numBlocks = 0;
        try {
            numBlocks = m_node.getNumBlocks();
        } catch (...) {
            // Chainman not available yet, just return
            return;
        }

        if (numBlocks <= 0) return;

        m_totalBlocks = numBlocks;
        populateBlockStatusCache();
        m_dataLoaded = true;
        m_initialized = true;
    }

    calculateLayout();
    update();
    if (isVisible()) {
        refreshVisibleStatuses();
        if (!m_updateTimer->isActive()) m_updateTimer->start();
    }
}

void BlockVisualizationWidget::populateBlockStatusCache()
{
    // This method bulk-populates the cache by accessing the block index directly
    // This is much faster than checking each block individually via the Chain interface

    // Process all blocks at once without chunking to avoid interruption
    for (int height = 0; height <= m_totalBlocks; ++height) {
        // Use the same logic as haveBlockOnDisk but cache the result
        try {
            uint256 blockHash = m_chain.getBlockHash(height);
            if (blockHash.IsNull()) {
                m_statusCache[height] = NO_HEADER;
                continue;
            }

            // We have at least the header
            m_statusCache[height] = HEADER_ONLY;

            // Check if we have the full block data on disk.
            if (m_chain.haveBlockOnDisk(height)) {
                m_statusCache[height] = HAVE_BLOCK;
            } else {
                // Missing active-chain data is not necessarily "pruned" (it may
                // have never been downloaded). Use HEADER_ONLY unless we later
                // observe a HAVE_BLOCK -> missing transition.
                m_statusCache[height] = HEADER_ONLY;
            }
        } catch (...) {
            m_statusCache[height] = NO_HEADER;
        }
    }

    // Update the display once at the end
    update();
}

void BlockVisualizationWidget::refreshBlockStatus(int height)
{
    if (height < 0 || height > m_totalBlocks) {
        return;
    }

    // Remove from cache to force re-check.
    m_statusCache.erase(height);
    // For targeted updates (requested/received), refresh synchronously so fast
    // transitions (in-flight -> have block) are visible immediately.
    updateBlockStatus(height);

    if (m_blocksPerRow > 0 && m_blockWidth > 0 && m_blockHeight > 0) {
        const int row = height / m_blocksPerRow;
        const int col = height % m_blocksPerRow;
        update(QRect(col * m_blockWidth, row * m_blockHeight, m_blockWidth, m_blockHeight));
    } else {
        update();
    }
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
        });
    }

    if (m_chain.isUsable() && !m_updateTimer->isActive()) m_updateTimer->start();
}

void BlockVisualizationWidget::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    if (m_updateTimer->isActive()) m_updateTimer->stop();
}

void BlockVisualizationWidget::updateBlockStatusesAsync()
{
    if (!m_chain.isUsable()) {
        if (m_updateTimer->isActive()) m_updateTimer->stop();
        return;
    }

    // Get visible block range to prioritize them
    QScrollArea* scrollArea = FindParentScrollArea(this);
    int visibleStart = 0, visibleEnd = m_totalBlocks;
    if (scrollArea) {
        QScrollBar* vbar = scrollArea->verticalScrollBar();
        if (vbar) {
            int scrollPos = vbar->value();
            int visibleHeight = scrollArea->viewport()->height();

            // Calculate visible block range
            int blockHeight = m_blockHeight;
            int blocksPerRow = m_blocksPerRow;
            if (blocksPerRow > 0) {
                int rowHeight = blockHeight;
                int startRow = scrollPos / rowHeight;
                int endRow = (scrollPos + visibleHeight) / rowHeight;

                visibleStart = startRow * blocksPerRow;
                visibleEnd = std::min((endRow + 1) * blocksPerRow, m_totalBlocks);
            }
        }
    }

    if (m_pendingBlocks.empty() && m_totalBlocks > 0 && visibleEnd >= visibleStart) {
        for (int height = std::max(0, visibleStart); height <= visibleEnd; ++height) {
            m_pendingBlocks.insert(height);
        }
    }

    if (m_pendingBlocks.empty()) return;

    // Process blocks in chunks to avoid blocking the GUI
    const int CHUNK_SIZE = 200;
    int processed = 0;

    // Process visible blocks first without scanning the whole pending set.
    for (auto it = m_pendingBlocks.lower_bound(visibleStart);
         it != m_pendingBlocks.end() && *it <= visibleEnd && processed < CHUNK_SIZE;) {
        const int height = *it;
        it = m_pendingBlocks.erase(it);
        updateBlockStatus(height);
        processed++;
    }

    // Then process remaining blocks
    if (processed < CHUNK_SIZE) {
        for (auto it = m_pendingBlocks.begin(); it != m_pendingBlocks.end();) {
            if (processed >= CHUNK_SIZE) break;

            const int height = *it;
            it = m_pendingBlocks.erase(it);
            updateBlockStatus(height);
            processed++;
        }
    }

    // Update the display
    update();

    // Schedule next chunk if there are more blocks to process
    if (!m_pendingBlocks.empty()) {
        m_updateTimer->start();
    }
}

void BlockVisualizationWidget::updateBlockStatus(int height)
{
    if (!m_chain.isUsable()) return;

    if (height < 0 || height > m_totalBlocks) {
        return;
    }

    // Try to get the block hash for this height
    uint256 blockHash;
    try {
        blockHash = m_chain.getBlockHash(height);
    } catch (...) {
        m_statusCache[height] = NO_HEADER;
        return;
    }

    if (blockHash.IsNull()) {
        m_statusCache[height] = NO_HEADER;
        return;
    }

    // Classify by known state without forcing block reads from disk.
    try {
        if (m_chain.haveBlockOnDisk(height)) {
            m_statusCache[height] = HAVE_BLOCK;
        } else if (m_chain.isBlockInFlight(height)) {
            m_statusCache[height] = IN_FLIGHT;
        } else {
            m_statusCache[height] = HEADER_ONLY;
        }
        // Any multi-block height should be surfaced as competing, regardless
        // of whether we currently have active-chain block data.
        if (m_chain.hasCompetingBlocks(height)) {
            m_statusCache[height] = COMPETING;
        }
    } catch (...) {
        // Keep deterministic fallback semantics if chain access fails.
        m_statusCache[height] = HEADER_ONLY;
    }
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
        if (globalCache.isPopulated()) {
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

    QString tooltip = tr("Block %1\nStatus: %2").arg(height).arg(statusText);

    // Try to get additional block information.
    // Avoid requesting block data for header-only/pruned states, which can
    // trigger noisy OpenBlockFile errors for blocks with no on-disk file.
    try {
        uint256 blockHash = m_chain.getBlockHash(height);
        if (!blockHash.IsNull()) {
            // Get block information using FoundBlock
            int64_t blockTime = 0;
            interfaces::FoundBlock foundBlock;
            foundBlock.time(blockTime);

            if (m_chain.findBlock(blockHash, foundBlock) && foundBlock.found) {
                // Add timestamp
                if (blockTime > 0) {
                    QDateTime blockDateTime = QDateTime::fromSecsSinceEpoch(blockTime);
                    QString timeStr = blockDateTime.toString("yyyy-MM-dd hh:mm:ss");
                    tooltip += tr("\nTime: %1").arg(timeStr);
                }

                // Add block size only when we know block data is available.
                if (status == HAVE_BLOCK) {
                    CBlock blockData;
                    interfaces::FoundBlock dataBlock;
                    dataBlock.data(blockData);
                    if (m_chain.findBlock(blockHash, dataBlock) && dataBlock.found && !blockData.IsNull()) {
                        size_t blockSize = ::GetSerializeSize(blockData, PROTOCOL_VERSION);
                        tooltip += tr("\nSize: %1 bytes").arg(blockSize);
                    }
                }
            }
        }
        const auto blocks_at_height = m_chain.getBlocksAtHeight(height);
        if (blocks_at_height.size() > 1) {
            tooltip += tr("\nCompeting blocks (%1):").arg(blocks_at_height.size());
            for (const auto& block : blocks_at_height) {
                QString flags;
                if (block.in_active_chain) flags += " active";
                if (block.have_data) flags += " data";
                if (flags.isEmpty()) flags = " header-only";
                tooltip += tr("\n- %1 (%2)")
                               .arg(QString::fromStdString(block.hash.ToString().substr(0, 16)))
                               .arg(flags.trimmed());
            }
        }
    } catch (...) {
        // Additional info not available
    }

    return tooltip;
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
    int blockIndex = getBlockIndexFromPosition(event->pos());

    if (blockIndex >= 0 && blockIndex <= m_totalBlocks) {
        QString tooltip = getTooltipForBlock(blockIndex);
        QToolTip::showText(event->globalPos(), tooltip, this);
    } else {
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
