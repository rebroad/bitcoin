// Copyright (c) 2014-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_COMPAT_ENDIAN_H
#define BITCOIN_COMPAT_ENDIAN_H

#include <bit>
#include <stdint.h>

// If we're not using autotools, try to detect system endian functions
#if !defined(HAVE_CONFIG_H)
#if defined(__linux__) || defined(__GLIBC__)
#include <endian.h>
#define HAVE_DECL_HTOBE16 1
#define HAVE_DECL_HTOLE16 1
#define HAVE_DECL_BE16TOH 1
#define HAVE_DECL_LE16TOH 1
#define HAVE_DECL_HTOBE32 1
#define HAVE_DECL_HTOLE32 1
#define HAVE_DECL_BE32TOH 1
#define HAVE_DECL_LE32TOH 1
#define HAVE_DECL_HTOBE64 1
#define HAVE_DECL_HTOLE64 1
#define HAVE_DECL_BE64TOH 1
#define HAVE_DECL_LE64TOH 1
#endif
#endif

#include <compat/byteswap.h>

// Use system-provided functions if available, otherwise define our own
#if HAVE_DECL_HTOBE16
#define bitcoin_htobe16 htobe16
#else
#define bitcoin_htobe16(x) bswap_16(x)
#endif

#if HAVE_DECL_HTOLE16
#define bitcoin_htole16 htole16
#else
#define bitcoin_htole16(x) (x)
#endif

#if HAVE_DECL_BE16TOH
#define bitcoin_be16toh be16toh
#else
#define bitcoin_be16toh(x) bswap_16(x)
#endif

#if HAVE_DECL_LE16TOH
#define bitcoin_le16toh le16toh
#else
#define bitcoin_le16toh(x) (x)
#endif

#if HAVE_DECL_HTOBE32
#define bitcoin_htobe32 htobe32
#else
#define bitcoin_htobe32(x) bswap_32(x)
#endif

#if HAVE_DECL_HTOLE32
#define bitcoin_htole32 htole32
#else
#define bitcoin_htole32(x) (x)
#endif

#if HAVE_DECL_BE32TOH
#define bitcoin_be32toh be32toh
#else
#define bitcoin_be32toh(x) bswap_32(x)
#endif

#if HAVE_DECL_LE32TOH
#define bitcoin_le32toh le32toh
#else
#define bitcoin_le32toh(x) (x)
#endif

#if HAVE_DECL_HTOBE64
#define bitcoin_htobe64 htobe64
#else
#define bitcoin_htobe64(x) bswap_64(x)
#endif

#if HAVE_DECL_HTOLE64
#define bitcoin_htole64 htole64
#else
#define bitcoin_htole64(x) (x)
#endif

#if HAVE_DECL_BE64TOH
#define bitcoin_be64toh be64toh
#else
#define bitcoin_be64toh(x) bswap_64(x)
#endif

#if HAVE_DECL_LE64TOH
#define bitcoin_le64toh le64toh
#else
#define bitcoin_le64toh(x) (x)
#endif

inline BSWAP_CONSTEXPR uint64_t htole64_internal(uint64_t host_64bits)
{
    if constexpr (std::endian::native == std::endian::big) return internal_bswap_64(host_64bits);
        else return host_64bits;
}

#endif // BITCOIN_COMPAT_ENDIAN_H
