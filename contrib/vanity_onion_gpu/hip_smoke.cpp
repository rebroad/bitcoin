// SPDX-License-Identifier: MIT
// Minimal HIP host harness for VanityPrefixMatchKernel.

#include <hip/hip_runtime.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

extern "C" __global__ void VanityPrefixMatchKernel(const uint8_t* addresses,
                                                   const char* prefixes,
                                                   const uint8_t* prefix_lens,
                                                   int prefix_count,
                                                   int max_prefix_len,
                                                   int need_len,
                                                   int candidate_count,
                                                   uint8_t* hit_flags);

namespace {
constexpr int kAddressLen = 35;

void CheckHip(hipError_t err, const char* what)
{
    if (err == hipSuccess) return;
    std::cerr << what << ": " << hipGetErrorString(err) << "\n";
    std::exit(1);
}
} // namespace

int main()
{
    // Fake payloads for kernel smoke test.
    const int candidate_count = 4;
    std::vector<uint8_t> h_addresses(candidate_count * kAddressLen);
    for (size_t i = 0; i < h_addresses.size(); ++i) h_addresses[i] = static_cast<uint8_t>(i);

    const std::vector<std::string> prefixes{"a", "ab", "...."};
    int max_prefix_len = 0;
    int need_len = 0;
    for (const auto& p : prefixes) {
        max_prefix_len = std::max(max_prefix_len, static_cast<int>(p.size()));
        need_len = std::max(need_len, static_cast<int>(p.size()));
    }
    std::vector<char> h_prefixes(prefixes.size() * max_prefix_len, '\0');
    std::vector<uint8_t> h_prefix_lens(prefixes.size(), 0);
    for (size_t i = 0; i < prefixes.size(); ++i) {
        std::memcpy(h_prefixes.data() + i * max_prefix_len, prefixes[i].data(), prefixes[i].size());
        h_prefix_lens[i] = static_cast<uint8_t>(prefixes[i].size());
    }
    std::vector<uint8_t> h_hit_flags(candidate_count, 0);

    uint8_t* d_addresses = nullptr;
    char* d_prefixes = nullptr;
    uint8_t* d_prefix_lens = nullptr;
    uint8_t* d_hit_flags = nullptr;

    CheckHip(hipMalloc(&d_addresses, h_addresses.size()), "hipMalloc addresses");
    CheckHip(hipMalloc(&d_prefixes, h_prefixes.size()), "hipMalloc prefixes");
    CheckHip(hipMalloc(&d_prefix_lens, h_prefix_lens.size()), "hipMalloc prefix_lens");
    CheckHip(hipMalloc(&d_hit_flags, h_hit_flags.size()), "hipMalloc hit_flags");

    CheckHip(hipMemcpy(d_addresses, h_addresses.data(), h_addresses.size(), hipMemcpyHostToDevice), "copy addresses");
    CheckHip(hipMemcpy(d_prefixes, h_prefixes.data(), h_prefixes.size(), hipMemcpyHostToDevice), "copy prefixes");
    CheckHip(hipMemcpy(d_prefix_lens, h_prefix_lens.data(), h_prefix_lens.size(), hipMemcpyHostToDevice), "copy prefix_lens");
    CheckHip(hipMemset(d_hit_flags, 0, h_hit_flags.size()), "memset hit_flags");

    constexpr int threads = 256;
    const int blocks = (candidate_count + threads - 1) / threads;
    hipLaunchKernelGGL(VanityPrefixMatchKernel,
                       dim3(blocks),
                       dim3(threads),
                       0,
                       0,
                       d_addresses,
                       d_prefixes,
                       d_prefix_lens,
                       static_cast<int>(prefixes.size()),
                       max_prefix_len,
                       need_len,
                       candidate_count,
                       d_hit_flags);
    CheckHip(hipDeviceSynchronize(), "kernel sync");
    CheckHip(hipMemcpy(h_hit_flags.data(), d_hit_flags, h_hit_flags.size(), hipMemcpyDeviceToHost), "copy hit_flags back");

    for (int i = 0; i < candidate_count; ++i) {
        std::cout << "candidate " << i << " hit=" << static_cast<int>(h_hit_flags[i]) << "\n";
    }

    hipFree(d_addresses);
    hipFree(d_prefixes);
    hipFree(d_prefix_lens);
    hipFree(d_hit_flags);
    return 0;
}

