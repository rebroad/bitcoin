// Copyright (c) 2011-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_BANTABLEMODEL_H
#define BITCOIN_QT_BANTABLEMODEL_H

#include <addrdb.h>
#include <net.h>

#include <memory>

#include <QAbstractTableModel>
#include <QStringList>
#include <QString>

class BanTablePriv;

namespace interfaces {
    class Node;
}

struct CCombinedBan {
    CSubNet subnet;
    CBanEntry banEntry;
    QString countryFlag;
    QString countryTooltip;
    QString displayAddress;
    QString asnId;
    QString asnDisplay;
    QString asnTooltip;
    QString asnNetwork;
    QStringList asnNetworks;
    QStringList asnBanTargets;
    int asnBannedCount{0};
    QString rawSubnet;
};

class BannedNodeLessThan
{
public:
    BannedNodeLessThan(int nColumn, Qt::SortOrder fOrder) :
        column(nColumn), order(fOrder) {}
    bool operator()(const CCombinedBan& left, const CCombinedBan& right) const;

private:
    int column;
    Qt::SortOrder order;
};

/**
   Qt model providing information about connected peers, similar to the
   "getpeerinfo" RPC call. Used by the rpc console UI.
 */
class BanTableModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    explicit BanTableModel(interfaces::Node& node, QObject* parent);
    ~BanTableModel();
    void startAutoRefresh();
    void stopAutoRefresh();

    enum ColumnIndex {
        Address = 0,
        Bantime = 1,
        Status = 2,
        BanCount = 3,
        ASN = 4
    };

    enum {
        AsnNetworkRole = Qt::UserRole,
        AsnNetworksRole,
        AsnBanTargetsRole,
        AsnIdRole,
        RawSubnetRole
    };

    /** @name Methods overridden from QAbstractTableModel
        @{*/
    int rowCount(const QModelIndex &parent) const override;
    int columnCount(const QModelIndex &parent) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    QModelIndex index(int row, int column, const QModelIndex &parent) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    void sort(int column, Qt::SortOrder order) override;
    /*@}*/

    bool shouldShow();

public Q_SLOTS:
    void refresh();

private:
    interfaces::Node& m_node;
    QStringList columns;
    std::unique_ptr<BanTablePriv> priv;
};

#endif // BITCOIN_QT_BANTABLEMODEL_H
