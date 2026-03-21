// Copyright (c) 2011-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/bantablemodel.h>

#include <interfaces/node.h>
#include <sync.h>
#include <validation.h> // For cs_main
#include <net_types.h> // For banmap_t
#include <QElapsedTimer>
#include <qt/locktiming.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <map>
#include <tuple>
#include <utility>
#include <vector>

#include <QDateTime>
#include <QFileInfo>
#include <QHash>
#include <QList>
#include <QLocale>
#include <QModelIndex>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QVariant>

static bool IsUpperIso2(const std::string& iso)
{
    return iso.size() == 2 && iso[0] >= 'A' && iso[0] <= 'Z' && iso[1] >= 'A' && iso[1] <= 'Z';
}

static std::string NormalizeIso2(const std::string& iso)
{
    if (iso.size() != 2) return {};
    std::string out = iso;
    out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
    out[1] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[1])));
    return IsUpperIso2(out) ? out : std::string{};
}

static QString IsoToFlag(const std::string& iso)
{
    if (!IsUpperIso2(iso)) return {};
    const uint codepoints[2] = {
        0x1F1E6u + static_cast<uint>(iso[0] - 'A'),
        0x1F1E6u + static_cast<uint>(iso[1] - 'A'),
    };
    return QString::fromUcs4(codepoints, 2);
}

static bool ParseU64(const std::string& input, uint64_t& out)
{
    if (input.empty()) return false;
    char* end_ptr{nullptr};
    errno = 0;
    const auto parsed = std::strtoull(input.c_str(), &end_ptr, 10);
    if (errno != 0 || end_ptr == nullptr || *end_ptr != '\0') return false;
    out = parsed;
    return true;
}

static bool ParseIpv4Uint(const CNetAddr& addr, uint32_t& out)
{
    struct in_addr in4;
    if (!addr.GetInAddr(&in4)) return false;
    out = ntohl(in4.s_addr);
    return true;
}

static bool ParseIpv6Bytes(const CNetAddr& addr, std::array<uint8_t, 16>& out)
{
    struct in6_addr in6;
    if (!addr.GetIn6Addr(&in6)) return false;
    std::copy(std::begin(in6.s6_addr), std::end(in6.s6_addr), out.begin());
    return true;
}

static bool LookupNumericHost(const std::string& host, CNetAddr& out)
{
    return LookupHost(host, out, /*fAllowLookup=*/false) && (out.IsIPv4() || out.IsIPv6());
}

static std::string ExtractHostFromSubnetText(const std::string& subnet)
{
    const std::size_t slash = subnet.find('/');
    const std::string before_mask = slash == std::string::npos ? subnet : subnet.substr(0, slash);
    uint16_t port{0};
    std::string host;
    SplitHostPort(before_mask, port, host);
    return host.empty() ? before_mask : host;
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
    struct AsnData {
        QString asn;
        QString network;
        QString tooltip;
    };

    struct GeoData {
        QString flag;
        QString tooltip;
        AsnData asn_data;
    };

    GeoData ResolveSubnet(const std::string& subnet)
    {
        const std::string host = ExtractHostFromSubnetText(subnet);
        if (host.empty()) return {};

        const auto cached = m_host_to_geo.find(host);
        if (cached != m_host_to_geo.end()) return cached->second;

        CNetAddr net_addr;
        if (!LookupNumericHost(host, net_addr)) return Cache(host, {});

        GeoData geo;

        const std::string iso = CountryIso(host, net_addr);
        if (!iso.empty()) {
            QString country_name = CountryNameFromIso(iso);
            QString city_name;
            if (m_city_db_available) {
                const QString mm_country = LookupMaxMindName(host, QStringLiteral("country"));
                const QString mm_city = LookupMaxMindName(host, QStringLiteral("city"));
                if (!mm_country.isEmpty()) country_name = mm_country;
                if (!mm_city.isEmpty()) city_name = mm_city;
            }

            if (!country_name.isEmpty()) {
                const QString location_text = city_name.isEmpty()
                    ? country_name
                    : QStringLiteral("%1 (%2)").arg(country_name, city_name);
                geo.tooltip = QStringLiteral("%1\n%2").arg(location_text, QString::fromStdString(host));
                geo.flag = IsoToFlag(iso);
            }
        }

        geo.asn_data = LookupAsn(host);

        return Cache(host, geo);
    }

private:
    struct TorGeoIp4Range {
        uint32_t start;
        uint32_t end;
        std::string iso;
    };
    struct TorGeoIp6Range {
        std::array<uint8_t, 16> start;
        std::array<uint8_t, 16> end;
        std::string iso;
    };

    static QString CountryNameFromIso(const std::string& iso)
    {
        if (!IsUpperIso2(iso)) return {};

        static const QMap<QString, QString> iso_to_country_name = [] {
            QMap<QString, QString> out;
            const auto locales = QLocale::matchingLocales(QLocale::AnyLanguage, QLocale::AnyScript, QLocale::AnyCountry);
            for (const QLocale& locale : locales) {
                const QString locale_name = locale.name();
                const int sep = locale_name.indexOf('_');
                if (sep <= 0) continue;
                const QString code = locale_name.mid(sep + 1).toUpper();
                if (code.size() != 2) continue;
                const QString country_name = QLocale::countryToString(locale.country());
                if (!country_name.isEmpty() && country_name != QStringLiteral("Default") && !out.contains(code)) {
                    out.insert(code, country_name);
                }
            }
            return out;
        }();

        const QString iso_q = QString::fromStdString(iso);
        const auto it = iso_to_country_name.find(iso_q);
        return it == iso_to_country_name.end() ? iso_q : it.value();
    }

    void InitOnce()
    {
        if (m_initialized) return;
        m_initialized = true;

        m_mmdb_binary = QStandardPaths::findExecutable("mmdblookup");

        for (const QString& path : {
                 QStringLiteral("/var/lib/geoip/GeoLite2-City.mmdb"),
                 QStringLiteral("/usr/share/GeoIP/GeoLite2-City.mmdb"),
                 QStringLiteral("/var/lib/geoip/GeoLite2-Country.mmdb"),
                 QStringLiteral("/usr/share/GeoIP/GeoLite2-Country.mmdb"),
             }) {
            if (QFileInfo::exists(path)) {
                m_city_db_path = path;
                break;
            }
        }

        for (const QString& path : {
                 QStringLiteral("/var/lib/geoip/GeoLite2-ASN.mmdb"),
                 QStringLiteral("/usr/share/GeoIP/GeoLite2-ASN.mmdb"),
             }) {
            if (QFileInfo::exists(path)) {
                m_asn_db_path = path;
                break;
            }
        }

        m_city_db_available = !m_mmdb_binary.isEmpty() && !m_city_db_path.isEmpty();
        m_asn_db_available = !m_mmdb_binary.isEmpty() && !m_asn_db_path.isEmpty();

        if (!m_city_db_available) {
            m_tor_geoip4_available = LoadTorGeoIp4("/usr/share/tor/geoip");
            m_tor_geoip6_available = LoadTorGeoIp6("/usr/share/tor/geoip6");
        }
    }

    GeoData Cache(const std::string& host, const GeoData& geo)
    {
        const auto [it, _] = m_host_to_geo.emplace(host, geo);
        return it->second;
    }

    std::string CountryIso(const std::string& host, const CNetAddr& net_addr)
    {
        InitOnce();
        if (m_city_db_available) {
            const std::string iso = LookupMaxMindIso(host);
            if (!iso.empty()) return iso;
        }
        return LookupTorCountryIso(net_addr);
    }

    std::string LookupMaxMindIso(const std::string& host)
    {
        for (const QString& path : {QStringLiteral("country"), QStringLiteral("registered_country"), QStringLiteral("represented_country")}) {
            const std::string iso = LookupMaxMindIsoPath(m_city_db_path, host, path);
            if (!iso.empty()) return iso;
        }
        return {};
    }

    QString LookupMaxMindName(const std::string& host, const QString& top_level)
    {
        if (top_level == QStringLiteral("country")) {
            for (const QString& path : {QStringLiteral("country"), QStringLiteral("registered_country"), QStringLiteral("represented_country")}) {
                const QString country_name = LookupMaxMindStringPath(m_city_db_path, host, {path, QStringLiteral("names"), QStringLiteral("en")});
                if (!country_name.isEmpty()) return country_name;
            }
            return {};
        }
        if (top_level == QStringLiteral("city")) {
            return LookupMaxMindStringPath(m_city_db_path, host, {QStringLiteral("city"), QStringLiteral("names"), QStringLiteral("en")});
        }
        return {};
    }

    AsnData LookupAsn(const std::string& host)
    {
        InitOnce();
        if (m_mmdb_binary.isEmpty()) return {};

        QString asn_number;
        QString asn_org;
        QString asn_network;

        if (m_asn_db_available) {
            asn_number = LookupMaxMindUIntPath(m_asn_db_path, host, {QStringLiteral("autonomous_system_number")});
            asn_org = LookupMaxMindStringPath(m_asn_db_path, host, {QStringLiteral("autonomous_system_organization")});
            asn_network = LookupMaxMindStringPath(m_asn_db_path, host, {QStringLiteral("network")});
        }

        if ((asn_number.isEmpty() || asn_org.isEmpty() || asn_network.isEmpty()) && m_city_db_available) {
            if (asn_number.isEmpty()) {
                asn_number = LookupMaxMindUIntPath(m_city_db_path, host, {QStringLiteral("traits"), QStringLiteral("autonomous_system_number")});
            }
            if (asn_org.isEmpty()) {
                asn_org = LookupMaxMindStringPath(m_city_db_path, host, {QStringLiteral("traits"), QStringLiteral("autonomous_system_organization")});
            }
            if (asn_network.isEmpty()) {
                asn_network = LookupMaxMindStringPath(m_city_db_path, host, {QStringLiteral("traits"), QStringLiteral("network")});
            }
        }

        AsnData result;
        if (!asn_number.isEmpty()) {
            result.asn = QStringLiteral("AS%1").arg(asn_number);
        }
        result.network = asn_network;
        if (!result.asn.isEmpty() || !asn_org.isEmpty() || !asn_network.isEmpty()) {
            QStringList parts;
            if (!result.asn.isEmpty()) parts << result.asn;
            if (!asn_org.isEmpty()) parts << asn_org;
            if (!asn_network.isEmpty()) parts << asn_network;
            result.tooltip = parts.join(QStringLiteral("\n"));
        }
        return result;
    }

    QString LookupMaxMindStringPath(const QString& db_path, const std::string& host, const std::initializer_list<QString>& path_components)
    {
        if (m_mmdb_binary.isEmpty() || db_path.isEmpty()) return {};

        QProcess process;
        QStringList args{QStringLiteral("--file"), db_path, QStringLiteral("--ip"), QString::fromStdString(host)};
        for (const QString& c : path_components) args << c;
        process.start(m_mmdb_binary, args);
        if (!process.waitForFinished(2000)) {
            process.kill();
            process.waitForFinished(100);
            return {};
        }

        const QString output = QString::fromUtf8(process.readAllStandardOutput());
        const auto match = QRegularExpression(QStringLiteral("\"([^\"]+)\"")).match(output);
        return match.hasMatch() ? match.captured(1) : QString{};
    }

    QString LookupMaxMindUIntPath(const QString& db_path, const std::string& host, const std::initializer_list<QString>& path_components)
    {
        if (m_mmdb_binary.isEmpty() || db_path.isEmpty()) return {};

        QProcess process;
        QStringList args{QStringLiteral("--file"), db_path, QStringLiteral("--ip"), QString::fromStdString(host)};
        for (const QString& c : path_components) args << c;
        process.start(m_mmdb_binary, args);
        if (!process.waitForFinished(2000)) {
            process.kill();
            process.waitForFinished(100);
            return {};
        }

        const QString output = QString::fromUtf8(process.readAllStandardOutput());
        const auto match = QRegularExpression(QStringLiteral("<uint(?:16|32|64)>\\s+([0-9]+)")).match(output);
        return match.hasMatch() ? match.captured(1) : QString{};
    }

    std::string LookupMaxMindIsoPath(const QString& db_path, const std::string& host, const QString& path)
    {
        if (m_mmdb_binary.isEmpty() || db_path.isEmpty()) return {};

        QProcess process;
        process.start(m_mmdb_binary, {
            QStringLiteral("--file"),
            db_path,
            QStringLiteral("--ip"),
            QString::fromStdString(host),
            path,
            QStringLiteral("iso_code"),
        });
        if (!process.waitForFinished(2000)) {
            process.kill();
            process.waitForFinished(100);
            return {};
        }

        const QString output = QString::fromUtf8(process.readAllStandardOutput());
        const auto match = QRegularExpression(QStringLiteral("\"([A-Za-z]{2})\"")).match(output);
        if (!match.hasMatch()) return {};

        return NormalizeIso2(match.captured(1).toStdString());
    }

    bool LoadTorGeoIp4(const char* path)
    {
        std::ifstream in(path);
        if (!in.is_open()) return false;

        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line.front() == '#') continue;

            const std::size_t c1 = line.find(',');
            if (c1 == std::string::npos) continue;
            const std::size_t c2 = line.find(',', c1 + 1);
            if (c2 == std::string::npos) continue;

            const std::string iso = NormalizeIso2(line.substr(c2 + 1));
            if (iso.empty()) continue;

            uint64_t start_u64{0};
            uint64_t end_u64{0};
            if (!ParseU64(line.substr(0, c1), start_u64) || !ParseU64(line.substr(c1 + 1, c2 - (c1 + 1)), end_u64)) continue;
            if (start_u64 > std::numeric_limits<uint32_t>::max() || end_u64 > std::numeric_limits<uint32_t>::max() || end_u64 < start_u64) continue;

            m_tor_geoip4.push_back({
                static_cast<uint32_t>(start_u64),
                static_cast<uint32_t>(end_u64),
                iso,
            });
        }

        if (m_tor_geoip4.empty()) return false;

        std::sort(m_tor_geoip4.begin(), m_tor_geoip4.end(), [](const TorGeoIp4Range& a, const TorGeoIp4Range& b) {
            return std::tie(a.start, a.end) < std::tie(b.start, b.end);
        });
        return true;
    }

    bool LoadTorGeoIp6(const char* path)
    {
        std::ifstream in(path);
        if (!in.is_open()) return false;

        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line.front() == '#') continue;

            const std::size_t c1 = line.find(',');
            if (c1 == std::string::npos) continue;
            const std::size_t c2 = line.find(',', c1 + 1);
            if (c2 == std::string::npos) continue;

            CNetAddr start_addr;
            CNetAddr end_addr;
            if (!LookupNumericHost(line.substr(0, c1), start_addr) || !LookupNumericHost(line.substr(c1 + 1, c2 - (c1 + 1)), end_addr)) continue;
            if (!start_addr.IsIPv6() || !end_addr.IsIPv6()) continue;

            std::array<uint8_t, 16> start{};
            std::array<uint8_t, 16> end{};
            if (!ParseIpv6Bytes(start_addr, start) || !ParseIpv6Bytes(end_addr, end) || start > end) continue;

            const std::string iso = NormalizeIso2(line.substr(c2 + 1));
            if (iso.empty()) continue;

            m_tor_geoip6.push_back({start, end, iso});
        }

        if (m_tor_geoip6.empty()) return false;

        std::sort(m_tor_geoip6.begin(), m_tor_geoip6.end(), [](const TorGeoIp6Range& a, const TorGeoIp6Range& b) {
            return std::tie(a.start, a.end) < std::tie(b.start, b.end);
        });
        return true;
    }

    std::string LookupTorCountryIso(const CNetAddr& addr) const
    {
        if (addr.IsIPv4() && m_tor_geoip4_available) {
            uint32_t ip{0};
            if (!ParseIpv4Uint(addr, ip)) return {};

            const auto it = std::upper_bound(
                m_tor_geoip4.begin(), m_tor_geoip4.end(), ip,
                [](const uint32_t value, const TorGeoIp4Range& range) { return value < range.start; });
            if (it != m_tor_geoip4.begin()) {
                const auto& range = *(it - 1);
                if (ip <= range.end) return range.iso;
            }
            return {};
        }

        if (addr.IsIPv6() && m_tor_geoip6_available) {
            std::array<uint8_t, 16> ip{};
            if (!ParseIpv6Bytes(addr, ip)) return {};

            const auto it = std::upper_bound(
                m_tor_geoip6.begin(), m_tor_geoip6.end(), ip,
                [](const std::array<uint8_t, 16>& value, const TorGeoIp6Range& range) { return value < range.start; });
            if (it != m_tor_geoip6.begin()) {
                const auto& range = *(it - 1);
                if (ip <= range.end) return range.iso;
            }
        }
        return {};
    }

    bool m_initialized{false};
    bool m_city_db_available{false};
    bool m_asn_db_available{false};
    bool m_tor_geoip4_available{false};
    bool m_tor_geoip6_available{false};

    QString m_mmdb_binary{};
    QString m_city_db_path{};
    QString m_asn_db_path{};

    std::map<std::string, GeoData> m_host_to_geo{};
    std::vector<TorGeoIp4Range> m_tor_geoip4{};
    std::vector<TorGeoIp6Range> m_tor_geoip6{};
};

static QString AsnKey(const CCombinedBan& entry)
{
    if (!entry.asnDisplay.isEmpty()) return entry.asnDisplay;
    return entry.asnNetwork;
}

static QString AddressSpaceSizeForCidr(const QString& network)
{
    const int slash = network.indexOf('/');
    if (slash <= 0) return {};

    bool ok{false};
    const int prefix = network.mid(slash + 1).toInt(&ok);
    if (!ok) return {};

    CNetAddr net_addr;
    if (!LookupNumericHost(network.left(slash).toStdString(), net_addr)) return {};

    if (net_addr.IsIPv4()) {
        if (prefix < 0 || prefix > 32) return {};
        const int host_bits = 32 - prefix;
        const uint64_t size = host_bits == 32 ? (uint64_t{1} << 32) : (uint64_t{1} << host_bits);
        return QLocale::system().toString(size);
    }

    if (net_addr.IsIPv6()) {
        if (prefix < 0 || prefix > 128) return {};
        return QStringLiteral("2^%1").arg(128 - prefix);
    }

    return {};
}

static QString FormatAsnDisplay(const CCombinedBan& entry, int asn_count)
{
    const QString label = !entry.asnDisplay.isEmpty() ? entry.asnDisplay : entry.asnNetwork;
    if (label.isEmpty()) return {};

    const QString size_text = AddressSpaceSizeForCidr(entry.asnNetwork);
    if (size_text.isEmpty()) {
        return QStringLiteral("%1 (%2/?)").arg(label, QLocale::system().toString(asn_count));
    }
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
        return pLeft->asnDisplay.compare(pRight->asnDisplay) < 0;
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

        for (const auto& entry : banMap) {
            CCombinedBan ban_entry;
            ban_entry.subnet = entry.first;
            ban_entry.banEntry = entry.second;
            ban_entry.rawSubnet = QString::fromStdString(entry.first.ToString());
            ban_entry.displayAddress = DisplaySubnetText(ban_entry.rawSubnet);

            const BanGeoResolver::GeoData geo = m_geo_resolver.ResolveSubnet(entry.first.ToString());
            ban_entry.countryFlag = geo.flag;
            ban_entry.countryTooltip = geo.tooltip;
            ban_entry.asnDisplay = geo.asn_data.asn;
            ban_entry.asnNetwork = geo.asn_data.network;
            ban_entry.asnTooltip = geo.asn_data.tooltip;

            const QString key = AsnKey(ban_entry);
            if (!key.isEmpty()) {
                asn_counts[key] = asn_counts.value(key) + 1;
            }

            cachedBanlist.append(ban_entry);
        }

        for (CCombinedBan& cached : cachedBanlist) {
            const QString key = AsnKey(cached);
            if (key.isEmpty()) continue;
            cached.asnDisplay = FormatAsnDisplay(cached, asn_counts.value(key));
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
