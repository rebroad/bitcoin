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
#include <QApplication>
#include <QPainter>
#include <QMouseEvent>
#include <QToolTip>
#include <QDateTime>
#include <QScrollBar>
#include <QScrollArea>
#include <QDebug>
#include <map>

BlockVisualizationWidget::BlockVisualizationWidget(interfaces::Node& node, interfaces::Chain& chain, QWidget *parent)
    : QWidget(parent)
    , m_node(node)
    , m_chain(chain)
{
    setMouseTracking(true);
    setMinimumSize(400, 200);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    // Set background color
    setAutoFillBackground(true);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, Qt::white);
    setPalette(pal);

    // Ensure the widget can expand to fill the scroll area
    setMinimumHeight(200);

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
    m_updateTimer->setInterval(100); // 100ms delay
    connect(m_updateTimer, &QTimer::timeout, [this]() {
        updateBlockStatusesAsync();
    });

    // Don't update block data here - wait until the widget is shown
}

BlockVisualizationWidget::~BlockVisualizationWidget()
{
}

void BlockVisualizationWidget::updateBlockData()
{
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

            // Check if we have the full block data on disk
            if (m_chain.haveBlockOnDisk(height)) {
                m_statusCache[height] = HAVE_BLOCK;
            } else {
                m_statusCache[height] = PRUNED;
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

    // Remove from cache to force re-check
    m_statusCache.erase(height);

    // Add to pending blocks for immediate update
    m_pendingBlocks.insert(height);

    // Trigger immediate update
    if (!m_updateTimer->isActive()) {
        m_updateTimer->start();
    }
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
}

void BlockVisualizationWidget::updateBlockStatusesAsync()
{
    if (m_pendingBlocks.empty()) {
        return; // Nothing to do
    }

    // Process blocks in chunks to avoid blocking the GUI
    const int CHUNK_SIZE = 50; // Process 50 blocks at a time
    int processed = 0;

    // Get visible block range to prioritize them
    QScrollArea* scrollArea = qobject_cast<QScrollArea*>(parentWidget());
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

    // First, process visible blocks
    std::set<int> visiblePending;
    for (int height : m_pendingBlocks) {
        if (height >= visibleStart && height <= visibleEnd) {
            visiblePending.insert(height);
        }
    }

    // Process visible blocks first
    for (int height : visiblePending) {
        if (processed >= CHUNK_SIZE) break;

        updateBlockStatus(height);
        m_pendingBlocks.erase(height);
        processed++;
    }

    // Then process remaining blocks
    if (processed < CHUNK_SIZE) {
        for (auto it = m_pendingBlocks.begin(); it != m_pendingBlocks.end();) {
            if (processed >= CHUNK_SIZE) break;

            updateBlockStatus(*it);
            it = m_pendingBlocks.erase(it);
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

    // We have at least the header
    m_statusCache[height] = HEADER_ONLY;

    // Check if we have the full block data on disk
    try {
        if (m_chain.haveBlockOnDisk(height)) {
            m_statusCache[height] = HAVE_BLOCK;
        } else {
            m_statusCache[height] = PRUNED;
        }
    } catch (...) {
        m_statusCache[height] = HAVE_BLOCK;
    }
}

void BlockVisualizationWidget::calculateLayout()
{
    if (m_totalBlocks <= 0) return;

    // Get the available width from the parent scroll area
    int availableWidth = width();
    if (availableWidth <= 0) {
        // If width is not set yet, use a reasonable default
        availableWidth = 800;
    }

    // Calculate optimal block size to fit blocks in width
    int totalBlocks = m_totalBlocks + 1; // +1 because we include block 0

    // Use a reasonable block size that fits well in the width
    int minBlockSize = 3;
    int maxBlockSize = std::max(10, availableWidth / 50); // At least 50 blocks per row

    m_blockWidth = std::min(maxBlockSize, std::max(minBlockSize, availableWidth / 100));
    m_blockHeight = m_blockWidth; // Keep blocks square

    // Calculate how many blocks fit per row
    m_blocksPerRow = availableWidth / m_blockWidth;
    if (m_blocksPerRow <= 0) m_blocksPerRow = 1;

    // Calculate total height needed for all blocks
    int numRows = (totalBlocks + m_blocksPerRow - 1) / m_blocksPerRow; // Ceiling division
    int totalHeight = numRows * m_blockHeight; // No need for legend space

    // Set the widget's size for scrolling
    setMinimumSize(availableWidth, totalHeight);
    resize(availableWidth, totalHeight);

    // Force a repaint
    update();
}

QColor BlockVisualizationWidget::getColorForStatus(BlockStatus status) const
{
    switch (status) {
        case UNKNOWN:
            return QColor(128, 128, 128); // Gray
        case NO_HEADER:
            return QColor(200, 200, 200); // Light gray
        case HEADER_ONLY:
            return QColor(255, 255, 0);   // Yellow
        case HAVE_BLOCK:
            return QColor(0, 255, 0);     // Green
        case PRUNED:
            return QColor(255, 165, 0);   // Orange
        case HAVE_UTXOS:
            return QColor(0, 0, 255);     // Blue
        case WALLET_UTXOS:
            return QColor(255, 0, 255);   // Magenta
        default:
            return QColor(128, 128, 128); // Gray
    }
}

QString BlockVisualizationWidget::getTooltipForBlock(int height) const
{
    if (height < 0 || height > m_totalBlocks) {
        return tr("Invalid block");
    }

    // Get status from cache
    BlockStatus status = UNKNOWN;
    auto it = m_statusCache.find(height);
    if (it != m_statusCache.end()) {
        status = it->second;
    }

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
    case HAVE_BLOCK:
        statusText = tr("Have block");
        break;
    case PRUNED:
        statusText = tr("Pruned");
        break;
    case HAVE_UTXOS:
        statusText = tr("Have UTXOs");
        break;
    case WALLET_UTXOS:
        statusText = tr("Wallet UTXOs");
        break;
    }

    QString tooltip = tr("Block %1\nStatus: %2").arg(height).arg(statusText);

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
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, false); // Disable antialiasing for better performance

    if (m_totalBlocks <= 0) {
        painter.drawText(rect(), Qt::AlignCenter, tr("No block data available"));
        return;
    }

    // Pre-calculate colors for better performance
    QColor colors[7];
    colors[0] = getColorForStatus(UNKNOWN);
    colors[1] = getColorForStatus(NO_HEADER);
    colors[2] = getColorForStatus(HEADER_ONLY);
    colors[3] = getColorForStatus(HAVE_BLOCK);
    colors[4] = getColorForStatus(PRUNED);
    colors[5] = getColorForStatus(HAVE_UTXOS);
    colors[6] = getColorForStatus(WALLET_UTXOS);

    // Draw blocks more efficiently
    for (int height = 0; height <= m_totalBlocks; ++height) {
        int row = height / m_blocksPerRow;
        int col = height % m_blocksPerRow;

        int x = col * m_blockWidth;
        int y = row * m_blockHeight;

        QRect blockRect(x, y, m_blockWidth, m_blockHeight);

        // Get status from global cache, fallback to local cache
        BlockStatus status = UNKNOWN;
        BlockStatusCache& globalCache = BlockStatusCache::getInstance();
        if (globalCache.isPopulated()) {
            status = static_cast<BlockStatus>(globalCache.getStatus(height));
        } else {
            auto it = m_statusCache.find(height);
            if (it != m_statusCache.end()) {
                status = it->second;
            }
        }

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
        {HEADER_ONLY, tr("Header Only")},
        {PRUNED, tr("Pruned")},
        {NO_HEADER, tr("No Header")},
        {HAVE_UTXOS, tr("Have UTXOs")},
        {WALLET_UTXOS, tr("Wallet UTXOs")}
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
        // Emit signal or handle click
        qDebug() << "Clicked on block" << blockIndex;
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
