// SPDX-License-Identifier: MIT
// CPU reference implementation for prefix matching over onion address payloads.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {
constexpr int kAddressLen = 35;
constexpr int kServiceIdLen = 56;
constexpr const char* kBase32Alphabet = "abcdefghijklmnopqrstuvwxyz234567";

bool IsNonLetterBase32Char(char c)
{
    return c >= '2' && c <= '7';
}

int EncodePrefixBase32(const uint8_t* address35, char* out, int out_len)
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

bool PrefixMatches(const char* service_id, int service_id_len, const std::string& prefix)
{
    if (service_id_len < static_cast<int>(prefix.size())) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
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

int main()
{
    // Minimal smoke input: deterministic bytes to validate parser and matcher behavior.
    uint8_t address[kAddressLen];
    for (int i = 0; i < kAddressLen; ++i) address[i] = static_cast<uint8_t>(i);

    const std::vector<std::string> prefixes{"a", "ab", "....", "zzzz"};
    int max_prefix_len = 0;
    for (const auto& p : prefixes) max_prefix_len = std::max(max_prefix_len, static_cast<int>(p.size()));

    char encoded[kServiceIdLen];
    const int encoded_len = EncodePrefixBase32(address, encoded, max_prefix_len);

    std::cout << "encoded_prefix=" << std::string(encoded, encoded_len) << "\n";
    for (const auto& p : prefixes) {
        std::cout << p << " => " << (PrefixMatches(encoded, encoded_len, p) ? "hit" : "miss") << "\n";
    }
    return 0;
}
