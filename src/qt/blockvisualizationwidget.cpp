// Copyright (c) 2024 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <qt/blockvisualizationwidget.h>
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
#include <QDebug>
#include <map>

BlockVisualizationWidget::BlockVisualizationWidget(interfaces::Node& node, QWidget *parent)
    : QWidget(parent)
    , m_node(node)
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

    // Don't update block data here - wait until the widget is shown
}

BlockVisualizationWidget::~BlockVisualizationWidget()
{
}

void BlockVisualizationWidget::updateBlockData()
{
    m_blocks.clear();

    // Get current chain height - check if chainman is available first
    int numBlocks = 0;
    try {
        numBlocks = m_node.getNumBlocks();
    } catch (...) {
        // Chainman not available yet, just return
        return;
    }

    if (numBlocks <= 0) return;

    // Initialize blocks from 0 to current height
    for (int height = 0; height <= numBlocks; ++height) {
        BlockInfo block;
        block.height = height;
        block.status = NO_HEADER; // Default status
        block.hash.SetNull();
        block.time = 0;
        block.nTx = 0;
        m_blocks.push_back(block);
    }

    updateBlockStatuses();
    calculateLayout();
    update();
}

void BlockVisualizationWidget::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    // Update block data when the widget is first shown
    updateBlockData();

    // Ensure we get the proper size from the parent scroll area
    if (parentWidget()) {
        QTimer::singleShot(0, this, [this]() {
            calculateLayout();
        });
    }
}

void BlockVisualizationWidget::updateBlockStatuses()
{
    // This is a simplified implementation
    // In a full implementation, we would need to access the chainman to get detailed block information
    // For now, we'll use the available node interface methods

    int numBlocks = m_node.getNumBlocks();
    uint256 bestHash = m_node.getBestBlockHash();

    // Mark the tip block as having the block
    if (!bestHash.IsNull() && numBlocks >= 0 && numBlocks < (int)m_blocks.size()) {
        m_blocks[numBlocks].status = HAVE_BLOCK;
        m_blocks[numBlocks].hash = bestHash;
    }

    // For now, we'll make some assumptions about block availability
    // In a real implementation, we'd query the chainman for each block's status

    // Mark recent blocks as having data (simplified)
    for (int i = std::max(0, numBlocks - 100); i <= numBlocks; ++i) {
        if (i < (int)m_blocks.size()) {
            if (m_blocks[i].status == NO_HEADER) {
                m_blocks[i].status = HAVE_BLOCK;
            }
        }
    }

    // Mark older blocks as potentially pruned or having headers only
    for (int i = 0; i < std::max(0, numBlocks - 100); ++i) {
        if (i < (int)m_blocks.size()) {
            if (m_blocks[i].status == NO_HEADER) {
                // This is a simplified heuristic - in reality we'd check the actual block status
                if (i % 10 == 0) { // Every 10th block has header
                    m_blocks[i].status = HEADER_ONLY;
                } else if (i % 50 == 0) { // Every 50th block is pruned
                    m_blocks[i].status = PRUNED;
                } else {
                    m_blocks[i].status = HAVE_BLOCK;
                }
            }
        }
    }
}

void BlockVisualizationWidget::calculateLayout()
{
    if (m_blocks.empty()) return;

    // Get the available width from the parent scroll area
    int availableWidth = width();
    if (availableWidth <= 0) {
        // If width is not set yet, use a reasonable default
        availableWidth = 800;
    }

    // Calculate optimal block size to fit blocks in width
    int totalBlocks = m_blocks.size();

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
    int totalHeight = numRows * m_blockHeight + 120; // Add space for legend

    // Set the widget's size for scrolling
    setMinimumSize(availableWidth, totalHeight);
    resize(availableWidth, totalHeight);

    // Force a repaint
    update();
}

QColor BlockVisualizationWidget::getColorForStatus(BlockStatus status) const
{
    switch (status) {
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
    if (height < 0 || height >= (int)m_blocks.size()) {
        return tr("Invalid block");
    }

    const BlockInfo& block = m_blocks[height];
    QString statusText;

    switch (block.status) {
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

    if (!block.hash.IsNull()) {
        tooltip += tr("\nHash: %1").arg(QString::fromStdString(block.hash.ToString()));
    }

    if (block.time > 0) {
        tooltip += tr("\nTime: %1").arg(QDateTime::fromSecsSinceEpoch(block.time).toString());
    }

    if (block.nTx > 0) {
        tooltip += tr("\nTransactions: %1").arg(block.nTx);
    }

    return tooltip;
}

int BlockVisualizationWidget::getBlockIndexFromPosition(const QPoint& pos) const
{
    if (m_blocks.empty() || m_blocksPerRow <= 0) return -1;

    int row = pos.y() / m_blockHeight;
    int col = pos.x() / m_blockWidth;

    int blockIndex = row * m_blocksPerRow + col;

    if (blockIndex >= 0 && blockIndex < (int)m_blocks.size()) {
        return blockIndex;
    }

    return -1;
}

void BlockVisualizationWidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    if (m_blocks.empty()) {
        painter.drawText(rect(), Qt::AlignCenter, tr("No block data available"));
        return;
    }

    // Draw blocks
    for (size_t i = 0; i < m_blocks.size(); ++i) {
        int row = i / m_blocksPerRow;
        int col = i % m_blocksPerRow;

        int x = col * m_blockWidth;
        int y = row * m_blockHeight;

        QRect blockRect(x, y, m_blockWidth, m_blockHeight);

        // Draw block with appropriate color
        QColor color = getColorForStatus(m_blocks[i].status);
        painter.fillRect(blockRect, color);

        // Draw border only if blocks are large enough
        if (m_blockWidth > 3 && m_blockHeight > 3) {
            painter.setPen(QPen(Qt::black, 1));
            painter.drawRect(blockRect);
        }
    }

    // Draw legend
    drawLegend(painter);

    // Draw statistics
    drawStatistics(painter);
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

void BlockVisualizationWidget::drawStatistics(QPainter& painter)
{
    if (m_blocks.empty()) return;

    // Count blocks by status
    std::map<BlockStatus, int> statusCounts;
    for (const auto& block : m_blocks) {
        statusCounts[block.status]++;
    }

    // Draw statistics in top-right corner
    int statsX = width() - 200;
    int statsY = 10;
    int statsWidth = 190;
    int statsHeight = 80;

    // Draw background
    painter.fillRect(statsX, statsY, statsWidth, statsHeight, QColor(255, 255, 255, 200));
    painter.setPen(QPen(Qt::black, 1));
    painter.drawRect(statsX, statsY, statsWidth, statsHeight);

    // Draw title
    painter.setFont(QFont("Arial", 8, QFont::Bold));
    painter.drawText(statsX + 5, statsY + 15, tr("Block Statistics"));

    // Draw statistics
    painter.setFont(QFont("Arial", 7));
    int itemY = statsY + 25;
    int itemHeight = 12;

    painter.drawText(statsX + 5, itemY, tr("Total Blocks: %1").arg(m_blocks.size()));
    itemY += itemHeight;

    if (statusCounts[HAVE_BLOCK] > 0) {
        painter.drawText(statsX + 5, itemY, tr("Have Block: %1").arg(statusCounts[HAVE_BLOCK]));
        itemY += itemHeight;
    }

    if (statusCounts[HEADER_ONLY] > 0) {
        painter.drawText(statsX + 5, itemY, tr("Header Only: %1").arg(statusCounts[HEADER_ONLY]));
        itemY += itemHeight;
    }

    if (statusCounts[PRUNED] > 0) {
        painter.drawText(statsX + 5, itemY, tr("Pruned: %1").arg(statusCounts[PRUNED]));
        itemY += itemHeight;
    }

    if (statusCounts[NO_HEADER] > 0) {
        painter.drawText(statsX + 5, itemY, tr("No Header: %1").arg(statusCounts[NO_HEADER]));
    }
}

void BlockVisualizationWidget::mouseMoveEvent(QMouseEvent *event)
{
    int blockIndex = getBlockIndexFromPosition(event->pos());

    if (blockIndex >= 0 && blockIndex < (int)m_blocks.size()) {
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

    if (blockIndex >= 0 && blockIndex < (int)m_blocks.size()) {
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
