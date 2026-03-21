// Copyright (c) 2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <net_types.h>

#include <logging.h>
#include <netaddress.h>
#include <netbase.h>
#include <univalue.h>

static const char* BANMAN_JSON_VERSION_KEY{"version"};

CBanEntry::CBanEntry(const UniValue& json)
    : nVersion(json[BANMAN_JSON_VERSION_KEY].get_int()),
      nCreateTime(json["ban_created"].get_int64()),
      nBanUntil(json["banned_until"].get_int64()),
      m_is_on_probation(json.exists("is_on_probation") ? json["is_on_probation"].get_bool() : false),
      nProbationUntil(json.exists("probation_until") ? json["probation_until"].get_int64() : 0),
      m_ban_count(json.exists("ban_count") ? json["ban_count"].get_int() : 0),
      m_source_asn(json.exists("source_asn") ? json["source_asn"].get_str() : "")
{
}

UniValue CBanEntry::ToJson() const
{
    UniValue json(UniValue::VOBJ);
    json.pushKV(BANMAN_JSON_VERSION_KEY, nVersion);
    json.pushKV("ban_created", nCreateTime);
    json.pushKV("banned_until", nBanUntil);
    json.pushKV("is_on_probation", m_is_on_probation);
    json.pushKV("probation_until", nProbationUntil);
    json.pushKV("ban_count", m_ban_count);
    if (!m_source_asn.empty()) {
        json.pushKV("source_asn", m_source_asn);
    }
    return json;
}

CAsnBanEntry::CAsnBanEntry(const UniValue& json)
    : nVersion(json[BANMAN_JSON_VERSION_KEY].get_int()),
      nCreateTime(json["ban_created"].get_int64()),
      nBanUntil(json["banned_until"].get_int64()),
      m_is_on_probation(json.exists("is_on_probation") ? json["is_on_probation"].get_bool() : false),
      nProbationUntil(json.exists("probation_until") ? json["probation_until"].get_int64() : 0),
      m_ban_count(json.exists("ban_count") ? json["ban_count"].get_int() : 0),
      nLastResolved(json.exists("last_resolved") ? json["last_resolved"].get_int64() : 0)
{
    if (json.exists("resolved_cidrs")) {
        for (const auto& cidr : json["resolved_cidrs"].getValues()) {
            m_resolved_cidrs.push_back(cidr.get_str());
        }
    }
}

UniValue CAsnBanEntry::ToJson() const
{
    UniValue json(UniValue::VOBJ);
    json.pushKV(BANMAN_JSON_VERSION_KEY, nVersion);
    json.pushKV("ban_created", nCreateTime);
    json.pushKV("banned_until", nBanUntil);
    json.pushKV("is_on_probation", m_is_on_probation);
    json.pushKV("probation_until", nProbationUntil);
    json.pushKV("ban_count", m_ban_count);
    json.pushKV("last_resolved", nLastResolved);
    UniValue cidrs(UniValue::VARR);
    for (const auto& cidr : m_resolved_cidrs) cidrs.push_back(cidr);
    json.pushKV("resolved_cidrs", cidrs);
    return json;
}

static const char* BANMAN_JSON_ADDR_KEY = "address";

/**
 * Convert a `banmap_t` object to a JSON array.
 * @param[in] bans Bans list to convert.
 * @return a JSON array, similar to the one returned by the `listbanned` RPC. Suitable for
 * passing to `BanMapFromJson()`.
 */
UniValue BanMapToJson(const banmap_t& bans)
{
    UniValue bans_json(UniValue::VARR);
    for (const auto& it : bans) {
        const auto& address = it.first;
        const auto& ban_entry = it.second;
        UniValue j = ban_entry.ToJson();
        j.pushKV(BANMAN_JSON_ADDR_KEY, address.ToString());
        bans_json.push_back(j);
    }
    return bans_json;
}

/**
 * Convert a JSON array to a `banmap_t` object.
 * @param[in] bans_json JSON to convert, must be as returned by `BanMapToJson()`.
 * @param[out] bans Bans list to create from the JSON.
 * @throws std::runtime_error if the JSON does not have the expected fields or they contain
 * unparsable values.
 */
void BanMapFromJson(const UniValue& bans_json, banmap_t& bans)
{
    for (const auto& ban_entry_json : bans_json.getValues()) {
        const int version{ban_entry_json[BANMAN_JSON_VERSION_KEY].get_int()};
        if (version != CBanEntry::CURRENT_VERSION) {
            LogPrintf("Dropping entry with unknown version (%s) from ban list\n", version);
            continue;
        }
        CSubNet subnet;
        const auto& subnet_str = ban_entry_json[BANMAN_JSON_ADDR_KEY].get_str();
        if (!LookupSubNet(subnet_str, subnet)) {
            LogPrintf("Dropping entry with unparseable address or subnet (%s) from ban list\n", subnet_str);
            continue;
        }
        bans.insert_or_assign(subnet, CBanEntry{ban_entry_json});
    }
}

UniValue AsnBanMapToJson(const asnbanmap_t& bans)
{
    UniValue bans_json(UniValue::VARR);
    for (const auto& it : bans) {
        UniValue j = it.second.ToJson();
        j.pushKV("asn", it.first);
        bans_json.push_back(j);
    }
    return bans_json;
}

void AsnBanMapFromJson(const UniValue& bans_json, asnbanmap_t& bans)
{
    for (const auto& ban_entry_json : bans_json.getValues()) {
        const int version{ban_entry_json[BANMAN_JSON_VERSION_KEY].get_int()};
        if (version != CAsnBanEntry::CURRENT_VERSION) {
            LogPrintf("Dropping ASN entry with unknown version (%s) from ban list\n", version);
            continue;
        }
        if (!ban_entry_json.exists("asn")) continue;
        const std::string asn_id = ban_entry_json["asn"].get_str();
        if (asn_id.empty()) continue;
        bans.insert_or_assign(asn_id, CAsnBanEntry{ban_entry_json});
    }
}
