// Copyright (c) 2011-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/peertablemodel.h>

#include <qt/guiconstants.h>
#include <qt/guiutil.h>

#include <interfaces/node.h>
#include <netbase.h>
#include <sync.h>
#include <util/system.h>
#include <util/strencodings.h>
#include <validation.h> // For cs_main
#include <QElapsedTimer>
#include <QDebug>
#include <QFileInfo>
#include <QLocale>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <qt/locktiming.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <map>
#include <memory>
#include <limits>
#include <tuple>
#include <utility>
#include <vector>

#include <QList>
#include <QTimer>

static std::string ExtractHost(const std::string& addr)
{
    uint16_t port{0};
    std::string host;
    SplitHostPort(addr, port, host);
    return host.empty() ? addr : host;
}

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

class CountryFlagResolver
{
public:
    QString TooltipForPeerAddress(const std::string& peer_addr)
    {
        return PresentationForPeerAddress(peer_addr).tooltip;
    }

    QString FlagForPeerAddress(const std::string& peer_addr)
    {
        return PresentationForPeerAddress(peer_addr).flag;
    }

    QString GeoIpStatusSummary()
    {
        InitOnce();
        return m_geoip_status;
    }

    bool GeoIpNeedsAttention()
    {
        InitOnce();
        return m_geoip_needs_attention;
    }

private:
    struct GeoPresentation {
        QString flag{};
        QString tooltip{};
    };

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

    GeoPresentation PresentationForPeerAddress(const std::string& peer_addr)
    {
        const std::string host = ExtractHost(peer_addr);
        if (host.empty()) return {};

        const auto cached = m_host_to_presentation.find(host);
        if (cached != m_host_to_presentation.end()) return cached->second;

        CNetAddr net_addr;
        if (!LookupNumericHost(host, net_addr)) {
            return Cache(host, {});
        }

        const GeoPresentation presentation = BuildPresentation(host, net_addr);
        return Cache(host, presentation);
    }

    GeoPresentation Cache(const std::string& host, const GeoPresentation& presentation)
    {
        const auto [it, _] = m_host_to_presentation.emplace(host, presentation);
        return it->second;
    }

    static QString CountryNameFromIso(const std::string& iso)
    {
        if (!IsUpperIso2(iso)) return {};

        static const QMap<QString, QString> iso_to_country_name = [] {
            QMap<QString, QString> out;
            const auto locales = QLocale::matchingLocales(QLocale::AnyLanguage, QLocale::AnyScript, QLocale::AnyCountry);
            for (const QLocale& locale : locales) {
                const QString locale_name = locale.name(); // e.g. hu_HU
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

    GeoPresentation BuildPresentation(const std::string& host, const CNetAddr& net_addr)
    {
        const std::string iso = CountryIso(host, net_addr);
        if (iso.empty()) return {};

        GeoPresentation presentation;
        presentation.flag = IsoToFlag(iso);

        QString country_name = CountryNameFromIso(iso);
        QString city_name;
        if (m_mmdb_available) {
            const std::string mm_country = LookupMaxMindName(host, QStringLiteral("country"));
            const std::string mm_city = LookupMaxMindName(host, QStringLiteral("city"));
            if (!mm_country.empty()) country_name = QString::fromStdString(mm_country);
            if (!mm_city.empty()) city_name = QString::fromStdString(mm_city);
        }

        if (!country_name.isEmpty()) {
            const QString location_text = city_name.isEmpty()
                ? country_name
                : QStringLiteral("%1 (%2)").arg(country_name, city_name);
            // Temporary debug aid: include the exact host/IP used for lookup.
            presentation.tooltip = QStringLiteral("%1\n%2").arg(location_text, QString::fromStdString(host));
            if (m_geoip_needs_attention && !m_geoip_status.isEmpty()) {
                presentation.tooltip += QStringLiteral("\n%1").arg(m_geoip_status);
            }
        }

        return presentation;
    }

    int StaleWarnDays() const
    {
        const int64_t parsed = gArgs.GetIntArg("-qtgeoipstalewarn", 7);
        return static_cast<int>(std::clamp<int64_t>(parsed, 1, 365));
    }

    QString DbAgeStatus(const QString& path) const
    {
        const QFileInfo file(path);
        if (!file.exists()) return QStringLiteral("GeoIP database file missing");
        const QDateTime modified = file.lastModified().toUTC();
        if (!modified.isValid()) return QStringLiteral("GeoIP database timestamp unavailable");
        const int age_days = modified.daysTo(QDateTime::currentDateTimeUtc());
        const int warn_days = StaleWarnDays();
        const QString base = QStringLiteral("GeoIP DB age: %1 days (updated %2 UTC)")
                                 .arg(QString::number(std::max(age_days, 0)),
                                      modified.toString(QStringLiteral("yyyy-MM-dd")));
        if (age_days > warn_days) {
            return QStringLiteral("GeoIP DB STALE: %1 (warn >= %2 days)").arg(base, QString::number(warn_days));
        }
        return base;
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
                m_mmdb_database_path = path;
                break;
            }
        }

        m_mmdb_available = !m_mmdb_binary.isEmpty() && !m_mmdb_database_path.isEmpty();
        if (!m_mmdb_available) {
            m_tor_geoip4_available = LoadTorGeoIp4("/usr/share/tor/geoip");
            m_tor_geoip6_available = LoadTorGeoIp6("/usr/share/tor/geoip6");
        }

        if (m_mmdb_available) {
            m_geoip_status = DbAgeStatus(m_mmdb_database_path);
            m_geoip_needs_attention = m_geoip_status.startsWith(QStringLiteral("GeoIP DB STALE"));
        } else if (m_tor_geoip4_available || m_tor_geoip6_available) {
            m_geoip_status = QStringLiteral("GeoIP fallback in use: tor geoip (country only)");
            m_geoip_needs_attention = true;
        } else {
            m_geoip_status = QStringLiteral("GeoIP unavailable: no MaxMind DB or tor geoip data");
            m_geoip_needs_attention = true;
        }
    }

    std::string CountryIso(const std::string& host, const CNetAddr& net_addr)
    {
        InitOnce();

        if (m_mmdb_available) {
            const std::string iso = LookupMaxMindIso(host);
            if (!iso.empty()) return iso;
        }
        return LookupTorCountryIso(net_addr);
    }

    std::string LookupMaxMindIso(const std::string& host)
    {
        for (const QString& path : {QStringLiteral("country"), QStringLiteral("registered_country"), QStringLiteral("represented_country")}) {
            const std::string iso = LookupMaxMindIsoPath(host, path);
            if (!iso.empty()) return iso;
        }
        return {};
    }

    std::string LookupMaxMindName(const std::string& host, const QString& top_level)
    {
        if (top_level == QStringLiteral("country")) {
            for (const QString& path : {QStringLiteral("country"), QStringLiteral("registered_country"), QStringLiteral("represented_country")}) {
                const std::string country_name = LookupMaxMindStringPath(host, {path, QStringLiteral("names"), QStringLiteral("en")});
                if (!country_name.empty()) return country_name;
            }
            return {};
        }
        if (top_level == QStringLiteral("city")) {
            return LookupMaxMindStringPath(host, {QStringLiteral("city"), QStringLiteral("names"), QStringLiteral("en")});
        }
        return {};
    }

    std::string LookupMaxMindStringPath(const std::string& host, const std::initializer_list<QString>& path_components)
    {
        QProcess process;
        QStringList args{QStringLiteral("--file"), m_mmdb_database_path, QStringLiteral("--ip"), QString::fromStdString(host)};
        for (const QString& c : path_components) args << c;
        process.start(m_mmdb_binary, args);
        if (!process.waitForFinished(2000)) {
            process.kill();
            process.waitForFinished(100);
            return {};
        }

        const QString output = QString::fromUtf8(process.readAllStandardOutput());
        const auto match = QRegularExpression(QStringLiteral("\"([^\"]+)\"")).match(output);
        return match.hasMatch() ? match.captured(1).toStdString() : std::string{};
    }

    std::string LookupMaxMindIsoPath(const std::string& host, const QString& path)
    {
        QProcess process;
        process.start(m_mmdb_binary, {
                                      QStringLiteral("--file"),
                                      m_mmdb_database_path,
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
    bool m_mmdb_available{false};
    bool m_tor_geoip4_available{false};
    bool m_tor_geoip6_available{false};
    QString m_mmdb_binary{};
    QString m_mmdb_database_path{};
    QString m_geoip_status{};
    bool m_geoip_needs_attention{false};
    std::map<std::string, GeoPresentation> m_host_to_presentation{};
    std::vector<TorGeoIp4Range> m_tor_geoip4{};
    std::vector<TorGeoIp6Range> m_tor_geoip6{};
};

PeerTableModel::PeerTableModel(interfaces::Node& node, QObject* parent) :
    QAbstractTableModel(parent),
    m_node(node),
    m_country_flag_resolver(std::make_unique<CountryFlagResolver>()),
    timer(nullptr)
{
    // set up timer for auto refresh
    timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &PeerTableModel::refresh);
    timer->setInterval(MODEL_UPDATE_DELAY);

    // load initial data
    refresh();
}

PeerTableModel::~PeerTableModel()
{
    // Intentionally left empty
}

void PeerTableModel::startAutoRefresh()
{
    timer->start();
}

void PeerTableModel::stopAutoRefresh()
{
    timer->stop();
}

QString PeerTableModel::geoIpStatusSummary() const
{
    return m_country_flag_resolver->GeoIpStatusSummary();
}

bool PeerTableModel::geoIpNeedsAttention() const
{
    return m_country_flag_resolver->GeoIpNeedsAttention();
}

int PeerTableModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return m_peers_data.size();
}

int PeerTableModel::columnCount(const QModelIndex& parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return columns.length();
}

QVariant PeerTableModel::data(const QModelIndex& index, int role) const
{
    if(!index.isValid())
        return QVariant();

    CNodeCombinedStats *rec = static_cast<CNodeCombinedStats*>(index.internalPointer());

    const auto column = static_cast<ColumnIndex>(index.column());
    if (role == Qt::DisplayRole) {
        switch (column) {
        case NetNodeId:
            return (qint64)rec->nodeStats.nodeid;
        case Address:
            return QString::fromStdString(rec->nodeStats.m_addr_name);
        case Direction:
            return rec->nodeStats.fInbound
                ? QString("%1 %2")
                      .arg(
                          //: An Inbound Connection from a Peer.
                          tr("↓"),
                          m_country_flag_resolver->FlagForPeerAddress(rec->nodeStats.m_addr_name))
                      .trimmed()
                : QString("%1 %2")
                      .arg(
                          //: An Outbound Connection to a Peer.
                          tr("↑"),
                          m_country_flag_resolver->FlagForPeerAddress(rec->nodeStats.m_addr_name))
                      .trimmed();
        //case ConnectionType:
        //    return GUIUtil::ConnectionTypeToQString(rec->nodeStats.m_conn_type, rec->nodeStats.fErlay, /* prepend_direction */ false);
        //case Network:
        //    return GUIUtil::NetworkToQString(rec->nodeStats.m_network);
        case Ping:
            return GUIUtil::formatPingTime(rec->nodeStats.m_min_ping_time);
        case Sent: {
            int64_t now = GetTimeSeconds();
            if (now != count_seconds(rec->nodeStats.m_connected)) // Avoid division by zero
                return GUIUtil::formatBps(rec->nodeStats.nSendBytes * 8.0 / (now - count_seconds(rec->nodeStats.m_connected)));
            else
                return QString::fromStdString("");
        }
        case Recv: {
            int64_t now = GetTimeSeconds();
            if (rec->nodeStats.nRecvBytesSnapOld && rec->nodeStats.nTimeSnapOld != now)
                return GUIUtil::formatBps((rec->nodeStats.nRecvBytes - rec->nodeStats.nRecvBytesSnapOld) * 8.0 / (now - rec->nodeStats.nTimeSnapOld));
            else if (now != count_seconds(rec->nodeStats.m_connected))
                return GUIUtil::formatBps(rec->nodeStats.nRecvBytes * 8.0 / (now - count_seconds(rec->nodeStats.m_connected)));
            else
                return QString::fromStdString("");
        }
        case TxBpsPct: {
            int64_t now = GetTimeSeconds();
            std::string dots;
            if (now - count_seconds(rec->nodeStats.m_connected) >= 120) dots="";
            else if (now - count_seconds(rec->nodeStats.m_connected) >= 60) dots=".";
            else dots="..";
            const uint64_t recv_bytes_base{rec->nodeStats.nRecvBytesSnapOld};
            const uint64_t mempool_bytes_base{rec->nodeStats.nMempoolBytesSnapOld};
            const uint64_t recv_bytes{rec->nodeStats.nRecvBytes > recv_bytes_base ? rec->nodeStats.nRecvBytes - recv_bytes_base : rec->nodeStats.nRecvBytes};
            const uint64_t mempool_bytes{rec->nodeStats.nMempoolBytes > mempool_bytes_base ? rec->nodeStats.nMempoolBytes - mempool_bytes_base : rec->nodeStats.nMempoolBytes};
            if (recv_bytes > 0) {
                int nTxBpsPct = int((100.0 * mempool_bytes / recv_bytes) + 0.5);
                int nBTxBpsPct = int(rec->nodeStats.nBTxBpsPct + 0.5);
                return QString::fromStdString(strprintf("%s%d%s", dots, nTxBpsPct, nBTxBpsPct ? strprintf("+%d", nBTxBpsPct) : ""));
            } else
                return QString::fromStdString(dots);
        }
        case MPpm: {
            int64_t now = GetTimeSeconds();
            const unsigned int mempool_txs_base{rec->nodeStats.nMempoolTXsSnapOld};
            const int64_t time_base{rec->nodeStats.nTimeSnapOld > 0 ? rec->nodeStats.nTimeSnapOld : count_seconds(rec->nodeStats.m_connected)};
            if (now > time_base) {
                const unsigned int mempool_txs{rec->nodeStats.nMempoolTXs > mempool_txs_base ? rec->nodeStats.nMempoolTXs - mempool_txs_base : rec->nodeStats.nMempoolTXs};
                float nMPpm = 60.0 * mempool_txs / (now - time_base);
                float nBTxpm = rec->nodeStats.nBTXpm;
                std::string strMPpm; std::string strBTpm;
                if (nMPpm < 1) strMPpm = strprintf("%d", 0.1 * (int)(nMPpm * 10));
                else strMPpm = strprintf("%d", (int)nMPpm);
                if (nBTxpm < 1) strBTpm = strprintf("%d", 0.1 * (int)(nBTxpm * 10));
                else strBTpm = strprintf("%d", (int)nBTxpm);
                return QString::fromStdString(strprintf("%s%s", strMPpm, nBTxpm ? strprintf("+%s", strBTpm) : ""));
            } else
                return {};
        }
        case Subversion:
            return QString::fromStdString(rec->nodeStats.cleanSubVer);
        } // no default case, so the compiler can warn about missing cases
        assert(false);
    } else if (role == Qt::TextAlignmentRole) {
        switch (column) {
        case NetNodeId:
        case Address:
        case Direction:
        //case ConnectionType:
        //case Network:
        case Ping:
        case Sent:
        case Recv:
        case TxBpsPct:
        case MPpm:
            return QVariant(Qt::AlignCenter);
        case Subversion:
            return QVariant(Qt::AlignLeft | Qt::AlignVCenter);
        } // no default case, so the compiler can warn about missing cases
        assert(false);
    } else if (role == StatsRole) {
        return QVariant::fromValue(rec);
    } else if (role == Qt::ToolTipRole) {
        switch (column) {
        case Direction:
            return m_country_flag_resolver->TooltipForPeerAddress(rec->nodeStats.m_addr_name);
        default:
            return {};
        }
    }

    return QVariant();
}

QVariant PeerTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if(orientation == Qt::Horizontal)
    {
        if(role == Qt::DisplayRole && section < columns.size())
            return columns[section];
        if (static_cast<ColumnIndex>(section) == Subversion)
            return QVariant(Qt::AlignLeft | Qt::AlignVCenter);
    }
    return QVariant();
}

Qt::ItemFlags PeerTableModel::flags(const QModelIndex &index) const
{
    if (!index.isValid()) return Qt::NoItemFlags;

    Qt::ItemFlags retval = Qt::ItemIsSelectable | Qt::ItemIsEnabled;
    return retval;
}

QModelIndex PeerTableModel::index(int row, int column, const QModelIndex& parent) const
{
    Q_UNUSED(parent);

    if (0 <= row && row < rowCount() && 0 <= column && column < columnCount()) {
        return createIndex(row, column, const_cast<CNodeCombinedStats*>(&m_peers_data[row]));
    }

    return QModelIndex();
}

void PeerTableModel::refresh()
{
    // Try to get fresh data directly if cs_main is free
    TIME_CS_MAIN_LOCK(10);
    
    if (lock.owns_lock()) {
        // We got the lock! Get fresh data and update cache
        interfaces::Node::NodesStats nodes_stats;
        m_node.getNodesStats(nodes_stats);
        decltype(m_peers_data) new_peers_data;
        new_peers_data.reserve(nodes_stats.size());
        for (const auto& node_stats : nodes_stats) {
            const CNodeCombinedStats stats{std::get<0>(node_stats), std::get<2>(node_stats), std::get<1>(node_stats)};
            new_peers_data.append(stats);
        }

        // Handle peer addition or removal as suggested in Qt Docs. See:
        // - https://doc.qt.io/qt-5/model-view-programming.html#inserting-and-removing-rows
        // - https://doc.qt.io/qt-5/model-view-programming.html#resizable-models
        // We take advantage of the fact that the std::vector returned
        // by interfaces::Node::getNodesStats is sorted by nodeid.
        for (int i = 0; i < m_peers_data.size();) {
            if (i < new_peers_data.size() && m_peers_data.at(i).nodeStats.nodeid == new_peers_data.at(i).nodeStats.nodeid) {
                ++i;
                continue;
            }
            // A peer has been removed from the table.
            beginRemoveRows(QModelIndex(), i, i);
            m_peers_data.erase(m_peers_data.begin() + i);
            endRemoveRows();
        }

        if (m_peers_data.size() < new_peers_data.size()) {
            // Some peers have been added to the end of the table.
            beginInsertRows(QModelIndex(), m_peers_data.size(), new_peers_data.size() - 1);
            m_peers_data.swap(new_peers_data);
            endInsertRows();
        } else {
            m_peers_data.swap(new_peers_data);
        }

        const auto top_left = index(0, 0);
        const auto bottom_right = index(rowCount() - 1, columnCount() - 1);
        // Only emit dataChanged if both indices are valid
        if (top_left.isValid() && bottom_right.isValid())
            Q_EMIT dataChanged(top_left, bottom_right);
    }
    // If cs_main is busy, skip this update (non-blocking)
}
