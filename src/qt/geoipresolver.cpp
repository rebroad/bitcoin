// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/geoipresolver.h>

#include <netaddress.h>
#include <netbase.h>
#include <util/maxmind_dyn.h>
#include <util/system.h>

#include <QDateTime>
#include <QFileInfo>
#include <QLocale>
#include <QMap>
#include <QStringList>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <tuple>
#include <utility>
#include <vector>

#if __has_include(<maxminddb.h>)
#include <maxminddb.h>
#define HAVE_MAXMINDDB_HEADER 1
#else
#define HAVE_MAXMINDDB_HEADER 0
#endif

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

static std::string ExtractHostFromAddressText(const std::string& address_text, bool is_subnet_text)
{
    std::string base = address_text;
    if (is_subnet_text) {
        const std::size_t slash = address_text.find('/');
        base = slash == std::string::npos ? address_text : address_text.substr(0, slash);
    }
    uint16_t port{0};
    std::string host;
    SplitHostPort(base, port, host);
    return host.empty() ? base : host;
}

struct GeoIpResolver::Impl
{
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

    struct MaxMindApi {
        using OpenFn = int (*)(const char*, uint32_t, MMDB_s*);
        using CloseFn = void (*)(MMDB_s*);
        using LookupStringFn = MMDB_lookup_result_s (*)(const MMDB_s*, const char*, int*, int*);
        using GetValueFn = int (*)(const MMDB_entry_s*, MMDB_entry_data_s*, ...);
        void* lib{nullptr};
        OpenFn open{nullptr};
        CloseFn close{nullptr};
        LookupStringFn lookup_string{nullptr};
        GetValueFn get_value{nullptr};

        bool Load()
        {
            if (lib) return true;
            lib = util::maxmind::OpenLibrary();
            if (!lib) return false;
            return util::maxmind::LoadSymbol(lib, "MMDB_open", open) &&
                   util::maxmind::LoadSymbol(lib, "MMDB_close", close) &&
                   util::maxmind::LoadSymbol(lib, "MMDB_lookup_string", lookup_string) &&
                   util::maxmind::LoadSymbol(lib, "MMDB_get_value", get_value);
        }
    };

    ~Impl()
    {
#if HAVE_MAXMINDDB_HEADER
        if (m_city_mmdb_open && m_maxmind_api.close) {
            m_maxmind_api.close(&m_city_mmdb);
            m_city_mmdb_open = false;
        }
        if (m_asn_mmdb_open && m_maxmind_api.close) {
            m_maxmind_api.close(&m_asn_mmdb);
            m_asn_mmdb_open = false;
        }
#endif
    }

    GeoIpResolver::GeoData ResolveAddress(const std::string& address_text, bool is_subnet_text)
    {
        InitOnce();
        const std::string host = ExtractHostFromAddressText(address_text, is_subnet_text);
        if (host.empty()) return {};

        const auto cached = m_host_to_geo.find(host);
        if (cached != m_host_to_geo.end()) return cached->second;

        CNetAddr net_addr;
        if (!LookupNumericHost(host, net_addr)) return Cache(host, {});

        GeoIpResolver::GeoData geo;
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
                const QString location_text = city_name.isEmpty() ? country_name : QStringLiteral("%1 (%2)").arg(country_name, city_name);
                geo.tooltip = QStringLiteral("%1\n%2").arg(location_text, QString::fromStdString(host));
                if (m_geoip_needs_attention && !m_geoip_status.isEmpty()) geo.tooltip += QStringLiteral("\n%1").arg(m_geoip_status);
                geo.flag = IsoToFlag(iso);
            }
        }

        geo.asn_data = LookupAsn(host);
        return Cache(host, geo);
    }

    GeoIpResolver::GeoData Cache(const std::string& host, const GeoIpResolver::GeoData& geo)
    {
        const auto [it, _] = m_host_to_geo.emplace(host, geo);
        return it->second;
    }

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
                if (!country_name.isEmpty() && country_name != QStringLiteral("Default") && !out.contains(code)) out.insert(code, country_name);
            }
            return out;
        }();
        const QString iso_q = QString::fromStdString(iso);
        const auto it = iso_to_country_name.find(iso_q);
        return it == iso_to_country_name.end() ? iso_q : it.value();
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
                                 .arg(QString::number(std::max(age_days, 0)), modified.toString(QStringLiteral("yyyy-MM-dd")));
        if (age_days > warn_days) return QStringLiteral("GeoIP DB STALE: %1 (warn >= %2 days)").arg(base, QString::number(warn_days));
        return base;
    }

#if HAVE_MAXMINDDB_HEADER
    bool MmdbLookup(const MMDB_s& db, const std::string& host, MMDB_lookup_result_s& out) const
    {
        if (!m_maxmind_api.lookup_string) return false;
        int gai_error{0};
        int mmdb_error{0};
        out = m_maxmind_api.lookup_string(&db, host.c_str(), &gai_error, &mmdb_error);
        return gai_error == 0 && mmdb_error == MMDB_SUCCESS && out.found_entry;
    }

    bool MmdbGetPath(const MMDB_entry_s& entry, MMDB_entry_data_s& data, const std::initializer_list<const char*>& path) const
    {
        if (!m_maxmind_api.get_value || path.size() == 0 || path.size() > 4) return false;
        std::array<const char*, 4> p{};
        std::copy(path.begin(), path.end(), p.begin());
        int status{MMDB_INVALID_METADATA_ERROR};
        switch (path.size()) {
        case 1: status = m_maxmind_api.get_value(&entry, &data, p[0], nullptr); break;
        case 2: status = m_maxmind_api.get_value(&entry, &data, p[0], p[1], nullptr); break;
        case 3: status = m_maxmind_api.get_value(&entry, &data, p[0], p[1], p[2], nullptr); break;
        case 4: status = m_maxmind_api.get_value(&entry, &data, p[0], p[1], p[2], p[3], nullptr); break;
        default: return false;
        }
        return status == MMDB_SUCCESS && data.has_data;
    }

    QString NetworkFromLookup(const std::string& host, uint16_t netmask) const
    {
        CNetAddr net_addr;
        if (!LookupNumericHost(host, net_addr)) return {};
        if ((net_addr.IsIPv4() && netmask > 32) || (net_addr.IsIPv6() && netmask > 128)) return {};
        CSubNet subnet(net_addr, static_cast<uint8_t>(netmask));
        if (!subnet.IsValid()) return {};
        return QString::fromStdString(subnet.ToString());
    }
#endif

    void InitOnce()
    {
        if (m_initialized) return;
        m_initialized = true;

        for (const QString& path : {QStringLiteral("/var/lib/geoip/GeoLite2-City.mmdb"),
                                    QStringLiteral("/usr/share/GeoIP/GeoLite2-City.mmdb"),
                                    QStringLiteral("/var/lib/geoip/GeoLite2-Country.mmdb"),
                                    QStringLiteral("/usr/share/GeoIP/GeoLite2-Country.mmdb")}) {
            if (QFileInfo::exists(path)) {
                m_city_db_path = path;
                break;
            }
        }
        for (const QString& path : {QStringLiteral("/var/lib/geoip/GeoLite2-ASN.mmdb"), QStringLiteral("/usr/share/GeoIP/GeoLite2-ASN.mmdb")}) {
            if (QFileInfo::exists(path)) {
                m_asn_db_path = path;
                break;
            }
        }

#if HAVE_MAXMINDDB_HEADER
        const bool mmdb_api_ok = m_maxmind_api.Load();
        if (mmdb_api_ok && !m_city_db_path.isEmpty()) {
            m_city_db_available = m_maxmind_api.open(m_city_db_path.toStdString().c_str(), MMDB_MODE_MMAP, &m_city_mmdb) == MMDB_SUCCESS;
            m_city_mmdb_open = m_city_db_available;
        }
        if (mmdb_api_ok && !m_asn_db_path.isEmpty()) {
            m_asn_db_available = m_maxmind_api.open(m_asn_db_path.toStdString().c_str(), MMDB_MODE_MMAP, &m_asn_mmdb) == MMDB_SUCCESS;
            m_asn_mmdb_open = m_asn_db_available;
        }
#endif
        if (!m_city_db_available) {
            m_tor_geoip4_available = LoadTorGeoIp4("/usr/share/tor/geoip");
            m_tor_geoip6_available = LoadTorGeoIp6("/usr/share/tor/geoip6");
        }

        if (m_city_db_available) {
            m_geoip_status = DbAgeStatus(m_city_db_path);
            m_geoip_needs_attention = m_geoip_status.startsWith(QStringLiteral("GeoIP DB STALE"));
        } else if (m_tor_geoip4_available || m_tor_geoip6_available) {
            m_geoip_status = m_city_db_path.isEmpty() ? QStringLiteral("GeoIP fallback in use: tor geoip (country only)")
                                                     : QStringLiteral("GeoIP fallback in use: MaxMind API unavailable; using tor geoip (country only)");
            m_geoip_needs_attention = true;
        } else {
            m_geoip_status = QStringLiteral("GeoIP unavailable: no MaxMind DB or tor geoip data");
            m_geoip_needs_attention = true;
        }
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
            const std::string iso = LookupMaxMindIsoPath(host, path);
            if (!iso.empty()) return iso;
        }
        return {};
    }

    QString LookupMaxMindName(const std::string& host, const QString& top_level) const
    {
#if !HAVE_MAXMINDDB_HEADER
        Q_UNUSED(host);
        Q_UNUSED(top_level);
        return {};
#else
        if (!m_city_db_available || !m_city_mmdb_open) return {};
        MMDB_lookup_result_s result{};
        if (!MmdbLookup(m_city_mmdb, host, result)) return {};

        auto lookup_name = [&](const std::initializer_list<const char*>& path) -> QString {
            MMDB_entry_data_s data{};
            if (!MmdbGetPath(result.entry, data, path)) return {};
            if (data.type != MMDB_DATA_TYPE_UTF8_STRING || data.utf8_string == nullptr) return {};
            return QString::fromUtf8(data.utf8_string, data.data_size);
        };

        if (top_level == QStringLiteral("country")) {
            for (const auto* path : {"country", "registered_country", "represented_country"}) {
                const QString country_name = lookup_name({path, "names", "en"});
                if (!country_name.isEmpty()) return country_name;
            }
            return {};
        }
        if (top_level == QStringLiteral("city")) return lookup_name({"city", "names", "en"});
        return {};
#endif
    }

    GeoIpResolver::AsnData LookupAsn(const std::string& host)
    {
        InitOnce();
        QString asn_number;
        QString asn_org;
        QString asn_network;
#if HAVE_MAXMINDDB_HEADER
        MMDB_lookup_result_s asn_result{};
        const bool has_asn_result = m_asn_db_available && m_asn_mmdb_open && MmdbLookup(m_asn_mmdb, host, asn_result);
        if (has_asn_result) {
            MMDB_entry_data_s data{};
            if (MmdbGetPath(asn_result.entry, data, {"autonomous_system_number"})) {
                if (data.type == MMDB_DATA_TYPE_UINT16) asn_number = QString::number(data.uint16);
                else if (data.type == MMDB_DATA_TYPE_UINT32) asn_number = QString::number(data.uint32);
                else if (data.type == MMDB_DATA_TYPE_UINT64) asn_number = QString::number(data.uint64);
            }
            if (MmdbGetPath(asn_result.entry, data, {"autonomous_system_organization"}) &&
                data.type == MMDB_DATA_TYPE_UTF8_STRING && data.utf8_string) {
                asn_org = QString::fromUtf8(data.utf8_string, data.data_size);
            }
            if (MmdbGetPath(asn_result.entry, data, {"network"}) &&
                data.type == MMDB_DATA_TYPE_UTF8_STRING && data.utf8_string) {
                asn_network = QString::fromUtf8(data.utf8_string, data.data_size);
            }
            if (asn_network.isEmpty() && asn_result.netmask > 0) asn_network = NetworkFromLookup(host, asn_result.netmask);
        }

        MMDB_lookup_result_s city_result{};
        const bool has_city_result = m_city_db_available && m_city_mmdb_open && MmdbLookup(m_city_mmdb, host, city_result);
        if ((asn_number.isEmpty() || asn_org.isEmpty() || asn_network.isEmpty()) && has_city_result) {
            MMDB_entry_data_s data{};
            if (asn_number.isEmpty() && MmdbGetPath(city_result.entry, data, {"traits", "autonomous_system_number"})) {
                if (data.type == MMDB_DATA_TYPE_UINT16) asn_number = QString::number(data.uint16);
                else if (data.type == MMDB_DATA_TYPE_UINT32) asn_number = QString::number(data.uint32);
                else if (data.type == MMDB_DATA_TYPE_UINT64) asn_number = QString::number(data.uint64);
            }
            if (asn_org.isEmpty() && MmdbGetPath(city_result.entry, data, {"traits", "autonomous_system_organization"}) &&
                data.type == MMDB_DATA_TYPE_UTF8_STRING && data.utf8_string) {
                asn_org = QString::fromUtf8(data.utf8_string, data.data_size);
            }
            if (asn_network.isEmpty() && MmdbGetPath(city_result.entry, data, {"traits", "network"}) &&
                data.type == MMDB_DATA_TYPE_UTF8_STRING && data.utf8_string) {
                asn_network = QString::fromUtf8(data.utf8_string, data.data_size);
            }
            if (asn_network.isEmpty() && city_result.netmask > 0) asn_network = NetworkFromLookup(host, city_result.netmask);
        }
#endif
        GeoIpResolver::AsnData result;
        if (!asn_number.isEmpty()) result.asn = QStringLiteral("AS%1").arg(asn_number);
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

    std::string LookupMaxMindIsoPath(const std::string& host, const QString& path) const
    {
#if !HAVE_MAXMINDDB_HEADER
        Q_UNUSED(host);
        Q_UNUSED(path);
        return {};
#else
        if (!m_city_db_available || !m_city_mmdb_open) return {};
        MMDB_lookup_result_s result{};
        if (!MmdbLookup(m_city_mmdb, host, result)) return {};
        MMDB_entry_data_s data{};
        std::string key = path.toStdString();
        if (!MmdbGetPath(result.entry, data, {key.c_str(), "iso_code"})) return {};
        if (data.type != MMDB_DATA_TYPE_UTF8_STRING || data.utf8_string == nullptr) return {};
        return NormalizeIso2(QString::fromUtf8(data.utf8_string, data.data_size).toStdString());
#endif
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
            m_tor_geoip4.push_back({static_cast<uint32_t>(start_u64), static_cast<uint32_t>(end_u64), iso});
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
            const auto it = std::upper_bound(m_tor_geoip4.begin(), m_tor_geoip4.end(), ip,
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
            const auto it = std::upper_bound(m_tor_geoip6.begin(), m_tor_geoip6.end(), ip,
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
    QString m_city_db_path{};
    QString m_asn_db_path{};
    QString m_geoip_status{};
    bool m_geoip_needs_attention{false};
#if HAVE_MAXMINDDB_HEADER
    MaxMindApi m_maxmind_api{};
    MMDB_s m_city_mmdb{};
    MMDB_s m_asn_mmdb{};
    bool m_city_mmdb_open{false};
    bool m_asn_mmdb_open{false};
#endif
    std::map<std::string, GeoIpResolver::GeoData> m_host_to_geo{};
    std::vector<TorGeoIp4Range> m_tor_geoip4{};
    std::vector<TorGeoIp6Range> m_tor_geoip6{};
};

GeoIpResolver::GeoIpResolver() : m_impl(std::make_unique<Impl>()) {}
GeoIpResolver::~GeoIpResolver() = default;

GeoIpResolver::GeoData GeoIpResolver::ResolveAddress(const std::string& address_text, bool is_subnet_text)
{
    return m_impl->ResolveAddress(address_text, is_subnet_text);
}

QString GeoIpResolver::GeoIpStatusSummary()
{
    m_impl->InitOnce();
    return m_impl->m_geoip_status;
}

bool GeoIpResolver::GeoIpNeedsAttention()
{
    m_impl->InitOnce();
    return m_impl->m_geoip_needs_attention;
}

