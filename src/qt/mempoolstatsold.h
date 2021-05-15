// Copyright (c) 2016 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_MEMPOOLSTATSOLD_H
#define BITCOIN_QT_MEMPOOLSTATSOLD_H

#include <QWidget>
#include <QGraphicsLineItem>
#include <QGraphicsPixmapItem>

#include <QCheckBox>
#include <QGraphicsProxyWidget>

#include <QEvent>

class ClientModel;

class ClickableTextItemOld : public QGraphicsTextItem
{
    Q_OBJECT
public:
    void setEnabled(bool state);
protected:
    void mousePressEvent(QGraphicsSceneMouseEvent *event);
Q_SIGNALS:
    void objectClicked(QGraphicsItem*);
};


namespace Ui {
    class MempoolStatsOld;
}

class MempoolStatsOld : public QWidget
{
    Q_OBJECT

public:
    MempoolStatsOld(QWidget *parent = 0);
    ~MempoolStatsOld();

    void setClientModel(ClientModel *model);

public Q_SLOTS:
    void drawChart();
    void objectClicked(QGraphicsItem *);

private:
    ClientModel *clientModel;

    virtual void resizeEvent(QResizeEvent *event);
    virtual void showEvent(QShowEvent *event);

    QGraphicsTextItem *titleItem;
    QGraphicsLineItem *titleLine;
    QGraphicsTextItem *noDataItem;

    QGraphicsTextItem *dynMemUsageValueItem;
    QGraphicsTextItem *txCountValueItem;
    QGraphicsTextItem *minFeeValueItem;

    ClickableTextItemOld *lastHourLabel;
    ClickableTextItemOld *last3HoursLabel;
    ClickableTextItemOld *lastDayLabel;
    ClickableTextItemOld *allDataLabel;

    QGraphicsProxyWidget *txCountSwitch;
    QGraphicsProxyWidget *minFeeSwitch;
    QGraphicsProxyWidget *dynMemUsageSwitch;

    QGraphicsScene *scene;
    QVector<QGraphicsItem*> redrawItems;

    QCheckBox *cbShowMemUsage;
    QCheckBox *cbShowNumTxns;
    QCheckBox *cbShowMinFeerate;

    int64_t timeFilter;

    Ui::MempoolStatsOld *ui;
};

#endif // BITCOIN_QT_MEMPOOLSTATSOLD_H
