// Copyright (c) 2024 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_BLOCKVISUALIZATIONWIDGET_H
#define BITCOIN_QT_BLOCKVISUALIZATIONWIDGET_H

#include <QWidget>
#include <QPainter>
#include <QMouseEvent>
#include <QToolTip>
#include <QColor>
#include <QTimer>
#include <vector>
#include <optional>
#include <uint256.h>

namespace interfaces {
    class Node;
}

class BlockVisualizationWidget : public QWidget
{
    Q_OBJECT

public:
    enum BlockStatus {
        NO_HEADER,      // Don't have the header
        HEADER_ONLY,    // Have header but no block data
        HAVE_BLOCK,     // Have the full block
        PRUNED,         // Had the block but it was pruned
        HAVE_UTXOS,     // Have unspent UTXOs from this block
        WALLET_UTXOS    // Have unspent UTXOs from this block that we own
    };

    explicit BlockVisualizationWidget(interfaces::Node& node, QWidget *parent = nullptr);
    ~BlockVisualizationWidget();

    void updateBlockData();
    void showEvent(QShowEvent *event) override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    struct BlockInfo {
        int height;
        BlockStatus status;
        uint256 hash;
        int64_t time;
        int nTx;
    };

    interfaces::Node& m_node;
    std::vector<BlockInfo> m_blocks;

    int m_blockWidth = 4;  // Width of each block in pixels
    int m_blockHeight = 20; // Height of each block in pixels
    int m_blocksPerRow = 100; // Number of blocks per row

    QTimer* m_resizeTimer = nullptr;

    QColor getColorForStatus(BlockStatus status) const;
    QString getTooltipForBlock(int height) const;
    int getBlockIndexFromPosition(const QPoint& pos) const;
    void calculateLayout();
    void updateBlockStatuses();
    void drawLegend(QPainter& painter);
    void drawStatistics(QPainter& painter);
};

#endif // BITCOIN_QT_BLOCKVISUALIZATIONWIDGET_H
