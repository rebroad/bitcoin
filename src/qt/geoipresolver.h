// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_GEOIPRESOLVER_H
#define BITCOIN_QT_GEOIPRESOLVER_H

#include <QString>

#include <map>
#include <memory>
#include <string>

class GeoIpResolver
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

    GeoIpResolver();
    ~GeoIpResolver();

    GeoData ResolveAddress(const std::string& address_text, bool is_subnet_text);
    QString GeoIpStatusSummary();
    bool GeoIpNeedsAttention();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

#endif // BITCOIN_QT_GEOIPRESOLVER_H

