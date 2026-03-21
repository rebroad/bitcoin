// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <banman.h>

#include <netaddress.h>
#include <netbase.h>
#include <node/ui_interface.h>
#include <sync.h>
#include <tinyformat.h>
#include <util/system.h>
#include <util/time.h>
#include <util/translation.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <unordered_set>

namespace {
bool IsAsnId(const std::string& asn_id)
{
    if (asn_id.size() < 3 || asn_id[0] != 'A' || asn_id[1] != 'S') return false;
    return std::all_of(asn_id.begin() + 2, asn_id.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

std::string NormalizeAsnId(const std::string& asn_id)
{
    if (asn_id.size() < 3) return {};
    std::string out = asn_id;
    out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
    out[1] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[1])));
    return IsAsnId(out) ? out : std::string{};
}

} // namespace

BanMan::BanMan(fs::path ban_file, CClientUIInterface* client_interface, int64_t default_ban_time)
    : m_client_interface(client_interface), m_ban_db(std::move(ban_file)), m_default_ban_time(default_ban_time)
{
    if (m_client_interface) m_client_interface->InitMessage(_("Loading banlist…").translated);

    int64_t n_start = GetTimeMillis();
    if (m_ban_db.Read(m_banned, m_banned_asns)) {
        RefreshAsnBans();
        SweepBanned(); // sweep out unused entries

        LogPrint(BCLog::BANMAN, "Loaded %d banned node addresses/subnets and %d ASN bans  %dms\n",
                 m_banned.size(), m_banned_asns.size(), GetTimeMillis() - n_start);
    } else {
        LogPrintf("Recreating the banlist database\n");
        m_banned = {};
        m_banned_asns = {};
        m_is_dirty = true;
    }

    DumpBanlist();
}

BanMan::~BanMan()
{
    DumpBanlist();
}

void BanMan::DumpBanlist()
{
    static Mutex dump_mutex;
    LOCK(dump_mutex);

    banmap_t banmap;
    asnbanmap_t asnmap;
    {
        LOCK(m_cs_banned);
        SweepAsnBanned();
        SweepBanned();
        if (!BannedSetIsDirty()) return;
        banmap = m_banned;
        asnmap = m_banned_asns;
        SetBannedSetDirty(false);
    }

    int64_t n_start = GetTimeMillis();
    if (!m_ban_db.Write(banmap, asnmap)) {
        SetBannedSetDirty(true);
    }

    LogPrint(BCLog::BANMAN, "Flushed %d banned node addresses/subnets and %d ASN bans to disk  %dms\n",
             banmap.size(), asnmap.size(), GetTimeMillis() - n_start);
}

void BanMan::ClearBanned()
{
    {
        LOCK(m_cs_banned);
        m_banned.clear();
        m_banned_asns.clear();
        m_is_dirty = true;
    }
    DumpBanlist(); //store banlist to disk
    if (m_client_interface) m_client_interface->BannedListChanged();
}

bool BanMan::IsDiscouraged(const CNetAddr& net_addr)
{
    LOCK(m_cs_banned);
    return m_discouraged.contains(net_addr.GetAddrBytes());
}

bool BanMan::IsOnProbation(const CNetAddr& net_addr) {
    auto current_time = GetTime();
    LOCK(m_cs_banned);
    for (const auto& it : m_banned) {
        CSubNet sub_net = it.first;
        CBanEntry ban_entry = it.second;

        if (ban_entry.m_is_on_probation && current_time < ban_entry.nProbationUntil && sub_net.Match(net_addr)) return true;
    }
    return false;
}

bool BanMan::IsOnProbation(const CSubNet& sub_net) {
    auto current_time = GetTime();
    LOCK(m_cs_banned);
    banmap_t::iterator i = m_banned.find(sub_net);
    if (i != m_banned.end()) {
        CBanEntry ban_entry = (*i).second;
        if (ban_entry.m_is_on_probation && current_time < ban_entry.nProbationUntil) {
            return true;
        }
    }
    return false;
}

bool BanMan::IsBanned(const CNetAddr& net_addr) {
    auto current_time = GetTime();
    LOCK(m_cs_banned);
    for (const auto& it : m_banned) {
        CSubNet sub_net = it.first;
        CBanEntry ban_entry = it.second;

        // Only return true if actively banned (not on probation)
        if (current_time < ban_entry.nBanUntil && sub_net.Match(net_addr)) return true;
    }
    return false;
}

bool BanMan::IsBanned(const CSubNet& sub_net) {
    auto current_time = GetTime();
    LOCK(m_cs_banned);
    banmap_t::iterator i = m_banned.find(sub_net);
    if (i != m_banned.end()) {
        CBanEntry ban_entry = (*i).second;
        // Only return true if actively banned (not on probation)
        if (current_time < ban_entry.nBanUntil) {
            return true;
        }
    }
    return false;
}

bool BanMan::IsAsnBanned(const std::string& asn_id)
{
    LOCK(m_cs_banned);
    const auto it = m_banned_asns.find(NormalizeAsnId(asn_id));
    if (it == m_banned_asns.end()) return false;
    return GetTime() < it->second.nBanUntil;
}

void BanMan::Ban(const CNetAddr& net_addr, int64_t ban_time_offset, bool since_unix_epoch)
{
    CSubNet sub_net(net_addr);
    Ban(sub_net, ban_time_offset, since_unix_epoch);
}

void BanMan::Discourage(const CNetAddr& net_addr)
{
    LOCK(m_cs_banned);
    m_discouraged.insert(net_addr.GetAddrBytes());
}

std::vector<std::string> BanMan::ResolveAsnNetworks(const std::string& asn_id) const
{
    (void)asn_id;
    return {};
}

void BanMan::SyncDerivedSubnetsForAsn(const std::string& asn_id, const CAsnBanEntry& asn_entry)
{
    std::unordered_set<std::string> keep;
    for (const std::string& cidr : asn_entry.m_resolved_cidrs) {
        CSubNet subnet;
        if (!LookupSubNet(cidr, subnet) || !subnet.IsValid()) continue;
        keep.insert(subnet.ToString());
        CBanEntry derived(asn_entry.nCreateTime);
        derived.nBanUntil = asn_entry.nBanUntil;
        derived.m_is_on_probation = asn_entry.m_is_on_probation;
        derived.nProbationUntil = asn_entry.nProbationUntil;
        derived.m_ban_count = asn_entry.m_ban_count;
        derived.m_source_asn = asn_id;
        m_banned[subnet] = derived;
    }
    for (auto it = m_banned.begin(); it != m_banned.end();) {
        if (it->second.m_source_asn == asn_id && keep.count(it->first.ToString()) == 0) {
            it = m_banned.erase(it);
            continue;
        }
        ++it;
    }
}

void BanMan::RefreshAsnBans()
{
    LOCK(m_cs_banned);
    for (auto& asn_it : m_banned_asns) {
        asn_it.second.m_resolved_cidrs = ResolveAsnNetworks(asn_it.first);
        asn_it.second.nLastResolved = GetTime();
        SyncDerivedSubnetsForAsn(asn_it.first, asn_it.second);
    }
    m_is_dirty = true;
}

void BanMan::BanASN(const std::string& asn_id, int64_t ban_time_offset, bool since_unix_epoch)
{
    const std::string normalized = NormalizeAsnId(asn_id);
    if (normalized.empty()) return;

    CAsnBanEntry ban_entry(GetTime());
    int64_t normalized_ban_time_offset = ban_time_offset;
    bool normalized_since_unix_epoch = since_unix_epoch;
    ban_entry.m_ban_count = 1;

    {
        LOCK(m_cs_banned);
        const auto existing = m_banned_asns.find(normalized);
        if (existing != m_banned_asns.end()) {
            const CAsnBanEntry& existing_entry = existing->second;
            const int64_t current_time = GetTime();
            if (existing_entry.m_is_on_probation && current_time < existing_entry.nProbationUntil) {
                normalized_ban_time_offset = 2 * (existing_entry.nBanUntil - existing_entry.nCreateTime);
                ban_entry.m_ban_count = existing_entry.m_ban_count + 1;
            } else {
                ban_entry.m_ban_count = std::max(1, existing_entry.m_ban_count);
            }
        }

        if (ban_time_offset <= 0) {
            normalized_ban_time_offset = m_default_ban_time;
            normalized_since_unix_epoch = false;
        }

        ban_entry.nBanUntil = (normalized_since_unix_epoch ? 0 : GetTime()) + normalized_ban_time_offset;
        ban_entry.m_is_on_probation = false;
        ban_entry.nProbationUntil = 0;
        ban_entry.m_resolved_cidrs = ResolveAsnNetworks(normalized);
        ban_entry.nLastResolved = GetTime();

        if (m_banned_asns[normalized].nBanUntil < ban_entry.nBanUntil) {
            m_banned_asns[normalized] = ban_entry;
            SyncDerivedSubnetsForAsn(normalized, ban_entry);
            m_is_dirty = true;
        } else {
            return;
        }
    }
    if (m_client_interface) m_client_interface->BannedListChanged();
    DumpBanlist();
}

void BanMan::Ban(const CSubNet& sub_net, int64_t ban_time_offset, bool since_unix_epoch)
{
    CBanEntry ban_entry(GetTime());
    int64_t normalized_ban_time_offset = ban_time_offset;
    bool normalized_since_unix_epoch = since_unix_epoch;
    ban_entry.m_ban_count = 1;

    {
        LOCK(m_cs_banned);

        // Check if this address is already in our ban list
        auto existing_ban = m_banned.find(sub_net);
        if (existing_ban != m_banned.end()) {
            CBanEntry& existing_entry = existing_ban->second;
            int64_t current_time = GetTime();

            // If currently on probation and being banned again, double the ban duration
            if (existing_entry.m_is_on_probation && current_time < existing_entry.nProbationUntil) {
                normalized_ban_time_offset = 2 * (existing_entry.nBanUntil - existing_entry.nCreateTime);
                ban_entry.m_ban_count = existing_entry.m_ban_count + 1;
                LogPrint(BCLog::BANMAN, "Address %s on probation banned again, ban duration: %d seconds\n",
                         sub_net.ToString(), normalized_ban_time_offset);
            }
            if (!existing_entry.m_source_asn.empty()) {
                const auto asn_it = m_banned_asns.find(existing_entry.m_source_asn);
                if (asn_it != m_banned_asns.end()) {
                    CAsnBanEntry& asn_entry = asn_it->second;
                    if (asn_entry.m_is_on_probation && current_time < asn_entry.nProbationUntil) {
                        normalized_ban_time_offset = 2 * (asn_entry.nBanUntil - asn_entry.nCreateTime);
                        asn_entry.m_ban_count += 1;
                        asn_entry.nBanUntil = GetTime() + normalized_ban_time_offset;
                        asn_entry.m_is_on_probation = false;
                        asn_entry.nProbationUntil = 0;
                        SyncDerivedSubnetsForAsn(existing_entry.m_source_asn, asn_entry);
                    }
                }
                ban_entry.m_source_asn = existing_entry.m_source_asn;
            }
        }

        if (ban_time_offset <= 0) {
            normalized_ban_time_offset = m_default_ban_time;
            normalized_since_unix_epoch = false;
        }

        ban_entry.nBanUntil = (normalized_since_unix_epoch ? 0 : GetTime()) + normalized_ban_time_offset;
        ban_entry.m_is_on_probation = false;
        ban_entry.nProbationUntil = 0;

        if (m_banned[sub_net].nBanUntil < ban_entry.nBanUntil) {
            m_banned[sub_net] = ban_entry;
            m_is_dirty = true;
        } else
            return;
    }
    if (m_client_interface) m_client_interface->BannedListChanged();

    //store banlist to disk immediately
    DumpBanlist();
}

bool BanMan::Unban(const CNetAddr& net_addr)
{
    CSubNet sub_net(net_addr);
    return Unban(sub_net);
}

bool BanMan::Unban(const CSubNet& sub_net)
{
    {
        LOCK(m_cs_banned);
        if (m_banned.erase(sub_net) == 0) return false;
        m_is_dirty = true;
    }
    if (m_client_interface) m_client_interface->BannedListChanged();
    DumpBanlist(); //store banlist to disk immediately
    return true;
}

bool BanMan::UnbanASN(const std::string& asn_id)
{
    const std::string normalized = NormalizeAsnId(asn_id);
    if (normalized.empty()) return false;
    {
        LOCK(m_cs_banned);
        if (m_banned_asns.erase(normalized) == 0) return false;
        for (auto it = m_banned.begin(); it != m_banned.end();) {
            if (it->second.m_source_asn == normalized) {
                it = m_banned.erase(it);
                continue;
            }
            ++it;
        }
        m_is_dirty = true;
    }
    if (m_client_interface) m_client_interface->BannedListChanged();
    DumpBanlist();
    return true;
}

void BanMan::GetBanned(banmap_t& banmap)
{
    LOCK(m_cs_banned);
    // Sweep the banlist so expired bans are not returned
    SweepAsnBanned();
    SweepBanned();
    banmap = m_banned; //create a thread safe copy
}

void BanMan::GetBannedAsns(asnbanmap_t& asnmap)
{
    LOCK(m_cs_banned);
    SweepAsnBanned();
    asnmap = m_banned_asns;
}

void BanMan::SweepAsnBanned()
{
    int64_t now = GetTime();
    bool notify_ui = false;
    LOCK(m_cs_banned);
    for (auto it = m_banned_asns.begin(); it != m_banned_asns.end();) {
        CAsnBanEntry& ban_entry = it->second;
        if (ban_entry.m_is_on_probation && now > ban_entry.nProbationUntil) {
            const std::string asn = it->first;
            it = m_banned_asns.erase(it);
            for (auto sit = m_banned.begin(); sit != m_banned.end();) {
                if (sit->second.m_source_asn == asn) {
                    sit = m_banned.erase(sit);
                    continue;
                }
                ++sit;
            }
            m_is_dirty = true;
            notify_ui = true;
            continue;
        }
        if (!ban_entry.m_is_on_probation && now > ban_entry.nBanUntil) {
            ban_entry.m_is_on_probation = true;
            ban_entry.nProbationUntil = now + 8 * (ban_entry.nBanUntil - ban_entry.nCreateTime);
            SyncDerivedSubnetsForAsn(it->first, ban_entry);
            m_is_dirty = true;
            notify_ui = true;
        }
        ++it;
    }
    if (notify_ui && m_client_interface) m_client_interface->BannedListChanged();
}

void BanMan::SweepBanned()
{
    int64_t now = GetTime();
    bool notify_ui = false;
    {
        LOCK(m_cs_banned);
        banmap_t::iterator it = m_banned.begin();
        while (it != m_banned.end()) {
            CSubNet sub_net = (*it).first;
            CBanEntry ban_entry = (*it).second;

            if (!sub_net.IsValid()) {
                m_banned.erase(it++);
                m_is_dirty = true;
                notify_ui = true;
                LogPrint(BCLog::BANMAN, "Removed banned node address/subnet: %s\n", sub_net.ToString());
            } else if (!ban_entry.m_source_asn.empty() && m_banned_asns.find(ban_entry.m_source_asn) == m_banned_asns.end()) {
                m_banned.erase(it++);
                m_is_dirty = true;
                notify_ui = true;
            } else if (ban_entry.m_is_on_probation && now > ban_entry.nProbationUntil) {
                // Probation has expired, remove entry
                m_banned.erase(it++);
                m_is_dirty = true;
                notify_ui = true;
                LogPrint(BCLog::BANMAN, "Removed ban probation node address/subnet: %s\n", sub_net.ToString());
            } else if (ban_entry.m_source_asn.empty() && !ban_entry.m_is_on_probation && now > ban_entry.nBanUntil) {
                // Ban has expired, transition to probation
                ban_entry.m_is_on_probation = true;
                ban_entry.nProbationUntil = now + 8 * (ban_entry.nBanUntil - ban_entry.nCreateTime); // Eight times the ban duration
                m_banned[sub_net] = ban_entry;
                m_is_dirty = true;
                notify_ui = true;
                LogPrint(BCLog::BANMAN, "Ban address %s moved to probation until %d (duration: %d seconds)\n",
                         sub_net.ToString(), ban_entry.nProbationUntil,
                         8 * (ban_entry.nBanUntil - ban_entry.nCreateTime));
                ++it;
            } else {
                ++it;
            }
        }
    }
    // update UI
    if (notify_ui && m_client_interface) {
        m_client_interface->BannedListChanged();
    }
}

bool BanMan::BannedSetIsDirty()
{
    LOCK(m_cs_banned);
    return m_is_dirty;
}

void BanMan::SetBannedSetDirty(bool dirty)
{
    LOCK(m_cs_banned); //reuse m_banned lock for the m_is_dirty flag
    m_is_dirty = dirty;
}
