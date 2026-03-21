// Copyright (c) 2011-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/bantablemodel.h>

#include <interfaces/node.h>
#include <net_types.h> // For banmap_t
#include <netbase.h>
#include <qt/geoipresolver.h>
#include <qt/locktiming.h>
#include <sync.h>
#include <validation.h> // For cs_main

#include <QElapsedTimer>
#include <QDateTime>
#include <QHash>
#include <QList>
#include <QLocale>
#include <QModelIndex>
#include <QSet>
#include <QVariant>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

static bool LookupNumericHostLocal(const std::string& host, CNetAddr& out)
{
    return LookupHost(host, out, /*fAllowLookup=*/false) && (out.IsIPv4() || out.IsIPv6());
}

static QString DisplaySubnetText(const QString& raw_subnet)
{
    if (raw_subnet.endsWith(QStringLiteral("/32"))) {
        return raw_subnet.left(raw_subnet.size() - 3);
    }
    return raw_subnet;
}

class BanGeoResolver
{
public:
    GeoIpResolver::GeoData ResolveSubnet(const std::string& subnet)
    {
        return m_resolver.ResolveAddress(subnet, /*is_subnet_text=*/true);
    }

private:
    GeoIpResolver m_resolver{};
};

static QString AsnKey(const CCombinedBan& entry)
{
    if (!entry.asnId.isEmpty()) return entry.asnId;
    return QStringLiteral("UNKNOWN:%1").arg(entry.rawSubnet);
}

static std::optional<uint64_t> AddressSpaceSizeForCidr(const QString& network)
{
    const int slash = network.indexOf('/');
    if (slash <= 0) return std::nullopt;

    bool ok{false};
    const int prefix = network.mid(slash + 1).toInt(&ok);
    if (!ok) return std::nullopt;

    CNetAddr net_addr;
    if (!LookupNumericHostLocal(network.left(slash).toStdString(), net_addr)) return std::nullopt;
    if (!net_addr.IsIPv4() || prefix < 0 || prefix > 32) return std::nullopt;
    const int host_bits = 32 - prefix;
    return host_bits == 32 ? (uint64_t{1} << 32) : (uint64_t{1} << host_bits);
}

static QString SummedAddressSpaceSize(const QSet<QString>& networks)
{
    if (networks.isEmpty()) return {};
    uint64_t sum{0};
    for (const QString& network : networks) {
        const auto size = AddressSpaceSizeForCidr(network);
        if (!size.has_value()) return {};
        if (std::numeric_limits<uint64_t>::max() - sum < *size) return {};
        sum += *size;
    }
    return QLocale::system().toString(sum);
}

static QString FormatAsnDisplay(const CCombinedBan& entry, int asn_count, const QString& size_text)
{
    const QString label = entry.asnId;
    if (label.isEmpty()) {
        return QStringLiteral("Unknown (%1)").arg(QLocale::system().toString(asn_count));
    }
    if (size_text.isEmpty()) return QStringLiteral("%1 (%2)").arg(label, QLocale::system().toString(asn_count));
    return QStringLiteral("%1 (%2/%3)").arg(label, QLocale::system().toString(asn_count), size_text);
}

bool BannedNodeLessThan::operator()(const CCombinedBan& left, const CCombinedBan& right) const
{
    const CCombinedBan* pLeft = &left;
    const CCombinedBan* pRight = &right;

    if (order == Qt::DescendingOrder) {
        std::swap(pLeft, pRight);
    }

    switch (static_cast<BanTableModel::ColumnIndex>(column)) {
    case BanTableModel::Address:
        return pLeft->displayAddress.compare(pRight->displayAddress) < 0;
    case BanTableModel::Bantime: {
        const int64_t left_time = pLeft->banEntry.m_is_on_probation ? pLeft->banEntry.nProbationUntil : pLeft->banEntry.nBanUntil;
        const int64_t right_time = pRight->banEntry.m_is_on_probation ? pRight->banEntry.nProbationUntil : pRight->banEntry.nBanUntil;
        return left_time < right_time;
    }
    case BanTableModel::Status:
        return pLeft->banEntry.m_is_on_probation < pRight->banEntry.m_is_on_probation;
    case BanTableModel::BanCount:
        return pLeft->banEntry.m_ban_count < pRight->banEntry.m_ban_count;
    case BanTableModel::ASN:
        if (pLeft->asnBannedCount != pRight->asnBannedCount) {
            return pLeft->asnBannedCount < pRight->asnBannedCount;
        }
        return pLeft->asnId.compare(pRight->asnId) < 0;
    }
    assert(false);
}

// private implementation
class BanTablePriv
{
public:
    /** Local cache of peer information */
    QList<CCombinedBan> cachedBanlist;
    /** Column to sort nodes by (default to unsorted) */
    int sortColumn{-1};
    /** Order (ascending or descending) to sort nodes by */
    Qt::SortOrder sortOrder;

    /** Pull a full list of banned nodes from CNode into our cache */
    void refreshBanlist(interfaces::Node& node)
    {
        TIME_CS_MAIN_LOCK(10);

        if (!lock.owns_lock()) {
            return;
        }

        banmap_t banMap;
        node.getBanned(banMap);

        cachedBanlist.clear();
        cachedBanlist.reserve(banMap.size());

        QHash<QString, int> asn_counts;
        QHash<QString, QSet<QString>> asn_networks;
        QHash<QString, QSet<QString>> asn_raw_subnets;
        QHash<QString, QString> asn_sizes;

        for (const auto& entry : banMap) {
            CCombinedBan ban_entry;
            ban_entry.subnet = entry.first;
            ban_entry.banEntry = entry.second;
            ban_entry.rawSubnet = QString::fromStdString(entry.first.ToString());
            ban_entry.displayAddress = DisplaySubnetText(ban_entry.rawSubnet);

            const GeoIpResolver::GeoData geo = m_geo_resolver.ResolveSubnet(entry.first.ToString());
            ban_entry.countryFlag = geo.flag;
            ban_entry.countryTooltip = geo.tooltip;
            ban_entry.asnId = geo.asn_data.asn;
            ban_entry.asnNetwork = geo.asn_data.network;
            ban_entry.asnTooltip = geo.asn_data.tooltip;

            const QString key = AsnKey(ban_entry);
            if (!key.isEmpty()) {
                asn_counts[key] = asn_counts.value(key) + 1;
                if (!ban_entry.asnNetwork.isEmpty()) {
                    asn_networks[key].insert(ban_entry.asnNetwork);
                }
                if (!ban_entry.asnId.isEmpty()) {
                    asn_raw_subnets[key].insert(ban_entry.rawSubnet);
                }
            }

            cachedBanlist.append(ban_entry);
        }

        for (CCombinedBan& cached : cachedBanlist) {
            const QString key = AsnKey(cached);
            if (key.isEmpty()) continue;
            cached.asnBannedCount = asn_counts.value(key);
            cached.asnNetworks = asn_networks.value(key).values();
            asn_sizes[key] = SummedAddressSpaceSize(asn_networks.value(key));
            cached.asnDisplay = FormatAsnDisplay(cached, cached.asnBannedCount, asn_sizes.value(key));
            if (cached.asnNetwork.isEmpty() && !cached.asnNetworks.isEmpty()) {
                cached.asnNetwork = cached.asnNetworks.first();
            }
            if (!cached.asnNetworks.isEmpty()) {
                cached.asnBanTargets = cached.asnNetworks;
            } else {
                cached.asnBanTargets = asn_raw_subnets.value(key).values();
            }
        }

        if (sortColumn >= 0) {
            std::stable_sort(cachedBanlist.begin(), cachedBanlist.end(), BannedNodeLessThan(sortColumn, sortOrder));
        }
    }

    int size() const
    {
        return cachedBanlist.size();
    }

    CCombinedBan* index(int idx)
    {
        if (idx >= 0 && idx < cachedBanlist.size()) {
            return &cachedBanlist[idx];
        }
        return nullptr;
    }

private:
    BanGeoResolver m_geo_resolver{};
};

BanTableModel::BanTableModel(interfaces::Node& node, QObject* parent) :
    QAbstractTableModel(parent),
    m_node(node)
{
    columns << tr("IP/Netmask") << tr("Banned Until") << tr("Status") << tr("Ban Count") << tr("ASN (Banned/Size)");
    priv = std::make_unique<BanTablePriv>();

    refresh();
}

BanTableModel::~BanTableModel()
{
}

int BanTableModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return priv->size();
}

int BanTableModel::columnCount(const QModelIndex& parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return columns.length();
}

QVariant BanTableModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid()) {
        return {};
    }

    auto* rec = static_cast<CCombinedBan*>(index.internalPointer());
    const auto column = static_cast<ColumnIndex>(index.column());

    if (role == Qt::DisplayRole) {
        switch (column) {
        case Address:
            return rec->countryFlag.isEmpty() ? rec->displayAddress : QStringLiteral("%1 %2").arg(rec->countryFlag, rec->displayAddress);
        case Bantime: {
            QDateTime date = QDateTime::fromMSecsSinceEpoch(0);
            date = date.addSecs(rec->banEntry.m_is_on_probation ? rec->banEntry.nProbationUntil : rec->banEntry.nBanUntil);
            return QLocale::system().toString(date, QLocale::LongFormat);
        }
        case Status:
            return rec->banEntry.m_is_on_probation ? tr("On Probation") : tr("Banned");
        case BanCount:
            return QString::number(rec->banEntry.m_ban_count);
        case ASN:
            return rec->asnDisplay;
        }
        assert(false);
    }

    if (role == Qt::TextAlignmentRole) {
        switch (column) {
        case Address:
        case Bantime:
        case Status:
        case BanCount:
            return QVariant(Qt::AlignCenter);
        case ASN:
            return QVariant(Qt::AlignLeft | Qt::AlignVCenter);
        }
        assert(false);
    }

    if (role == Qt::ToolTipRole) {
        switch (column) {
        case Address:
            return rec->countryTooltip;
        case ASN:
            return rec->asnTooltip;
        default:
            return {};
        }
    }

    if (role == AsnNetworkRole) {
        return rec->asnNetwork;
    }

    if (role == AsnNetworksRole) {
        return rec->asnNetworks;
    }

    if (role == AsnBanTargetsRole) {
        return rec->asnBanTargets;
    }

    if (role == AsnIdRole) {
        return rec->asnId;
    }

    if (role == RawSubnetRole) {
        return rec->rawSubnet;
    }

    return {};
}

QVariant BanTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation == Qt::Horizontal) {
        if (role == Qt::DisplayRole && section < columns.size()) {
            return columns[section];
        }
        if (role == Qt::TextAlignmentRole && static_cast<ColumnIndex>(section) == ASN) {
            return QVariant(Qt::AlignLeft | Qt::AlignVCenter);
        }
    }
    return {};
}

Qt::ItemFlags BanTableModel::flags(const QModelIndex& index) const
{
    if (!index.isValid()) return Qt::NoItemFlags;
    return Qt::ItemIsSelectable | Qt::ItemIsEnabled;
}

QModelIndex BanTableModel::index(int row, int column, const QModelIndex& parent) const
{
    Q_UNUSED(parent);
    CCombinedBan* data = priv->index(row);

    if (data) {
        return createIndex(row, column, data);
    }
    return QModelIndex();
}

void BanTableModel::refresh()
{
    Q_EMIT layoutAboutToBeChanged();
    priv->refreshBanlist(m_node);
    Q_EMIT layoutChanged();
}

void BanTableModel::sort(int column, Qt::SortOrder order)
{
    priv->sortColumn = column;
    priv->sortOrder = order;
    refresh();
}

bool BanTableModel::shouldShow()
{
    return priv->size() > 0;
}
