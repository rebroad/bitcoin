// SPDX-License-Identifier: MIT
// Phase 1 HIP prototype: prefix matching over precomputed onion address payloads.

#include <hip/hip_runtime.h>

#include <cstddef>
#include <cstdint>

namespace {
constexpr int kAddressLen = 35;
constexpr int kServiceIdLen = 56;
constexpr const char* kBase32Alphabet = "abcdefghijklmnopqrstuvwxyz234567";

__device__ __forceinline__ bool IsNonLetterBase32Char(char c)
{
    return c >= '2' && c <= '7';
}

__device__ __forceinline__ int EncodePrefixBase32(const uint8_t* address35, char* out, int out_len)
{
    uint32_t buffer = 0;
    int bits_left = 0;
    int out_pos = 0;

    for (int i = 0; i < kAddressLen && out_pos < out_len; ++i) {
        buffer = (buffer << 8) | address35[i];
        bits_left += 8;
        while (bits_left >= 5 && out_pos < out_len) {
            const int idx = (buffer >> (bits_left - 5)) & 0x1f;
            out[out_pos++] = kBase32Alphabet[idx];
            bits_left -= 5;
        }
    }
    if (bits_left > 0 && out_pos < out_len) {
        const int idx = (buffer << (5 - bits_left)) & 0x1f;
        out[out_pos++] = kBase32Alphabet[idx];
    }
    return out_pos;
}

__device__ __forceinline__ bool PrefixMatches(const char* service_id, int service_id_len, const char* prefix, int prefix_len)
{
    if (service_id_len < prefix_len) return false;
    for (int i = 0; i < prefix_len; ++i) {
        const char pc = prefix[i];
        const char sc = service_id[i];
        if (pc == '.') {
            if (!IsNonLetterBase32Char(sc)) return false;
            continue;
        }
        if (pc != sc) return false;
    }
    return true;
}
} // namespace

extern "C" __global__ void VanityPrefixMatchKernel(const uint8_t* addresses,
                                                   const char* prefixes,
                                                   const uint8_t* prefix_lens,
                                                   int prefix_count,
                                                   int max_prefix_len,
                                                   int need_len,
                                                   int candidate_count,
                                                   uint8_t* hit_flags)
{
    const int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= candidate_count) return;

    const uint8_t* address = addresses + (idx * kAddressLen);
    char service_prefix[kServiceIdLen];
    const int encoded_len = EncodePrefixBase32(address, service_prefix, need_len);

    bool hit = false;
    for (int i = 0; i < prefix_count && !hit; ++i) {
        const char* pref = prefixes + (i * max_prefix_len);
        const int pref_len = static_cast<int>(prefix_lens[i]);
        hit = PrefixMatches(service_prefix, encoded_len, pref, pref_len);
    }
    hit_flags[idx] = hit ? 1 : 0;
}

