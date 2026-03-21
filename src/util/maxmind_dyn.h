// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_UTIL_MAXMIND_DYN_H
#define BITCOIN_UTIL_MAXMIND_DYN_H

#include <dlfcn.h>

namespace util {
namespace maxmind {

inline void* OpenLibrary()
{
    void* lib = dlopen("libmaxminddb.so.0", RTLD_NOW);
    if (!lib) lib = dlopen("libmaxminddb.so", RTLD_NOW);
    return lib;
}

template <typename Fn>
inline bool LoadSymbol(void* lib, const char* name, Fn& out)
{
    out = reinterpret_cast<Fn>(dlsym(lib, name));
    return out != nullptr;
}

} // namespace maxmind
} // namespace util

#endif // BITCOIN_UTIL_MAXMIND_DYN_H

