// SPDX-License-Identifier: MIT
// Generate Tor v3 onion service keys that match vanity prefixes.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <sstream>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <crypto/sha3.h>
#include <span.h>

#include <sodium.h>

namespace fs = std::filesystem;

static const char* BASE32_ALPHABET = "abcdefghijklmnopqrstuvwxyz234567";
static const char* ONION_CHECKSUM_PREFIX = ".onion checksum";
static const uint8_t ONION_VERSION = 3;
static constexpr size_t ONION_CHECKSUM_PREFIX_LEN = 15;

static std::atomic<bool> g_stop{false};

static void SignalHandler(int) { g_stop.store(true); }

static void Die(const std::string& msg) {
    std::cerr << "error: " << msg << "\n";
    std::exit(2);
}

static bool IsValidBase32Char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= '2' && c <= '7');
}

static bool IsNonLetterBase32Char(char c) {
    return (c >= '2' && c <= '7');
}

static std::string NormalizePrefix(std::string p) {
    while (!p.empty() && (p.back() == '\r' || p.back() == '\n' || p.back() == ' ' || p.back() == '\t')) p.pop_back();
    while (!p.empty() && (p.front() == ' ' || p.front() == '\t')) p.erase(p.begin());
    for (char& c : p) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const std::string suffix = ".onion";
    if (p.size() >= suffix.size() && p.compare(p.size() - suffix.size(), suffix.size(), suffix) == 0) {
        p.erase(p.size() - suffix.size());
    }
    if (p.empty()) Die("empty prefix");
    if (p.size() > 56) Die("prefix too long (max 56 chars)");
    for (char c : p) {
        if (c == '.') continue; // '.' means any non-letter base32 char (2-7)
        if (!IsValidBase32Char(c)) Die("invalid characters in prefix (allowed: a-z2-7 or '.')");
    }
    return p;
}

static std::vector<std::string> LoadPrefixes(const std::string& prefixes_arg, const std::string& prefix_file) {
    std::set<std::string> prefixes;
    if (!prefixes_arg.empty()) {
        size_t start = 0;
        while (start <= prefixes_arg.size()) {
            size_t comma = prefixes_arg.find(',', start);
            if (comma == std::string::npos) comma = prefixes_arg.size();
            std::string token = prefixes_arg.substr(start, comma - start);
            if (!token.empty()) prefixes.insert(NormalizePrefix(token));
            start = comma + 1;
        }
    }
    if (!prefix_file.empty()) {
        std::ifstream f(prefix_file);
        if (!f.is_open()) Die("could not open prefix file");
        std::string line;
        while (std::getline(f, line)) {
            const size_t first_non_ws = line.find_first_not_of(" \t\r");
            if (first_non_ws == std::string::npos) continue;
            if (line[first_non_ws] == '#') continue;
            std::istringstream line_in(line);
            std::string token;
            while (line_in >> token) {
                prefixes.insert(NormalizePrefix(token));
            }
        }
    }
    if (prefixes.empty()) Die("no prefixes provided");
    return std::vector<std::string>(prefixes.begin(), prefixes.end());
}

static std::vector<std::string> EffectivePrefixes(std::vector<std::string> prefixes) {
    std::sort(prefixes.begin(), prefixes.end(), [](const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return a.size() < b.size();
        return a < b;
    });
    std::vector<std::string> out;
    for (const auto& p : prefixes) {
        bool covered = false;
        for (const auto& e : out) {
            if (p.rfind(e, 0) == 0) { covered = true; break; }
        }
        if (!covered) out.push_back(p);
    }
    return out;
}

static bool PrefixHasWildcard(const std::string& p) {
    return p.find('.') != std::string::npos;
}

static bool PrefixMatchesServiceId(const std::string& service_id, const std::string& prefix) {
    if (service_id.size() < prefix.size()) return false;
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

static double HitProbability(const std::vector<std::string>& prefixes) {
    bool has_wildcards = false;
    for (const auto& p : prefixes) {
        if (PrefixHasWildcard(p)) {
            has_wildcards = true;
            break;
        }
    }
    if (has_wildcards) {
        // Approximate union probability with overlap ignored for wildcard rules.
        double p = 0.0;
        for (const auto& pref : prefixes) {
            double term = 1.0;
            for (char c : pref) {
                term *= (c == '.') ? (6.0 / 32.0) : (1.0 / 32.0);
            }
            p += term;
        }
        return std::min(1.0, p);
    }
    auto eff = EffectivePrefixes(prefixes);
    double p = 0.0;
    for (const auto& pref : eff) {
        p += std::pow(32.0, -static_cast<int>(pref.size()));
    }
    return p;
}

static double AttemptsForProbability(double p_hit, double target) {
    if (p_hit <= 0.0 || target <= 0.0 || target >= 1.0) return std::numeric_limits<double>::infinity();
    return std::log(1.0 - target) / std::log(1.0 - p_hit);
}

static std::string FormatDuration(double seconds) {
    if (seconds >= 24.0 * 3600.0) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1) << (seconds / 86400.0) << " days";
        return oss.str();
    }
    if (seconds >= 3600.0) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1) << (seconds / 3600.0) << " hours";
        return oss.str();
    }
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1) << (seconds / 60.0) << " min";
    return oss.str();
}

static std::string FormatEtaClock(double seconds) {
    if (!std::isfinite(seconds) || seconds <= 0.0) return "--:--";
    const uint64_t total = static_cast<uint64_t>(std::llround(seconds));
    const uint64_t h = total / 3600;
    const uint64_t m = (total % 3600) / 60;
    const uint64_t s = total % 60;
    std::ostringstream oss;
    if (h > 0) {
        oss << std::setw(2) << std::setfill('0') << h << ":"
            << std::setw(2) << std::setfill('0') << m << ":"
            << std::setw(2) << std::setfill('0') << s;
    } else {
        oss << std::setw(2) << std::setfill('0') << m << ":"
            << std::setw(2) << std::setfill('0') << s;
    }
    return oss.str();
}

static std::string Base32Encode(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve((len * 8 + 4) / 5);
    uint32_t buffer = 0;
    int bits_left = 0;
    for (size_t i = 0; i < len; ++i) {
        buffer = (buffer << 8) | data[i];
        bits_left += 8;
        while (bits_left >= 5) {
            int idx = (buffer >> (bits_left - 5)) & 0x1f;
            out.push_back(BASE32_ALPHABET[idx]);
            bits_left -= 5;
        }
    }
    if (bits_left > 0) {
        int idx = (buffer << (5 - bits_left)) & 0x1f;
        out.push_back(BASE32_ALPHABET[idx]);
    }
    return out;
}

static std::string OnionServiceIdFromPubkey(const uint8_t* pubkey32) {
    uint8_t checksum_full[SHA3_256::OUTPUT_SIZE];
    SHA3_256 hasher;
    hasher.Write(Span<const unsigned char>(reinterpret_cast<const unsigned char*>(ONION_CHECKSUM_PREFIX), ONION_CHECKSUM_PREFIX_LEN));
    hasher.Write(Span<const unsigned char>(pubkey32, 32));
    hasher.Write(Span<const unsigned char>(&ONION_VERSION, 1));
    hasher.Finalize(checksum_full);

    uint8_t address[32 + 2 + 1];
    std::memcpy(address, pubkey32, 32);
    address[32] = checksum_full[0];
    address[33] = checksum_full[1];
    address[34] = ONION_VERSION;

    return Base32Encode(address, sizeof(address));
}

static void ExpandSeedToTorKey(const uint8_t* seed32, uint8_t* out_scalar32, uint8_t* out_prf32) {
    // TODO(perf): Generate Tor key material directly to avoid per-attempt SHA512 when safe for key format guarantees.
    unsigned char h[crypto_hash_sha512_BYTES];
    crypto_hash_sha512(h, seed32, 32);
    // Clamp per Ed25519 spec.
    h[0] &= 248;
    h[31] &= 63;
    h[31] |= 64;
    std::memcpy(out_scalar32, h, 32);
    std::memcpy(out_prf32, h + 32, 32);
}

static std::string OnionServiceIdFromTorKey(const uint8_t* tor_key64) {
    uint8_t pk[crypto_sign_PUBLICKEYBYTES];
    if (crypto_scalarmult_ed25519_base(pk, tor_key64) != 0) {
        Die("failed to derive public key from tor key scalar");
    }
    return OnionServiceIdFromPubkey(pk);
}

static bool WriteKeyFile(const fs::path& outdir, const std::string& service_id, const std::string& key_b64, fs::path* out_path) {
    fs::path path = outdir / service_id;
    if (out_path) *out_path = path;
    if (fs::exists(path)) return false;
    std::ofstream f(path, std::ios::binary | std::ios::out);
    if (!f.is_open()) return false;
    f << "ED25519-V3:" << key_b64;
    f.close();
    return true;
}

struct Options {
    std::string prefixes;
    std::string prefix_file;
    fs::path outdir{"onion_v3_private_keys"};
    std::string timing_file;
    bool estimate{false};
    uint64_t count{1};
    uint32_t workers{0};
    double status_interval{5.0};
};

static void PrintUsage();

static std::string DefaultTimingFile() {
#ifdef __linux__
    const char* home = std::getenv("HOME");
    if (home && home[0]) {
        fs::path p = fs::path(home) / ".cache" / "bitcoin-vanity";
        std::error_code ec;
        fs::create_directories(p, ec);
        return (p / "onion-timing.log").string();
    }
#endif
    return "onion-timing.log";
}

struct TimingProfile {
    uint32_t threads;
    uint64_t avg_ns;
};

static bool ParseThreadField(const std::string& line, uint32_t& out) {
    const std::string key = "threads=";
    const size_t p = line.find(key);
    if (p == std::string::npos) return false;
    size_t i = p + key.size();
    if (i >= line.size() || !std::isdigit(static_cast<unsigned char>(line[i]))) return false;
    uint64_t v = 0;
    while (i < line.size() && std::isdigit(static_cast<unsigned char>(line[i]))) {
        v = v * 10 + static_cast<uint64_t>(line[i] - '0');
        if (v > std::numeric_limits<uint32_t>::max()) return false;
        ++i;
    }
    out = static_cast<uint32_t>(v);
    return true;
}

static bool ParseAvgNsField(const std::string& line, uint64_t& out) {
    const std::string key = "avg_ns=";
    const size_t p = line.find(key);
    if (p == std::string::npos) return false;
    size_t i = p + key.size();
    if (i >= line.size() || !std::isdigit(static_cast<unsigned char>(line[i]))) return false;
    uint64_t v = 0;
    while (i < line.size() && std::isdigit(static_cast<unsigned char>(line[i]))) {
        v = v * 10 + static_cast<uint64_t>(line[i] - '0');
        ++i;
    }
    out = v;
    return true;
}

static std::vector<TimingProfile> LoadTimingProfiles(const std::string& file) {
    std::ifstream in(file);
    if (!in.is_open()) return {};
    std::unordered_map<uint32_t, uint64_t> latest;
    std::string line;
    while (std::getline(in, line)) {
        uint32_t t = 0;
        uint64_t avg = 0;
        if (!ParseThreadField(line, t) || !ParseAvgNsField(line, avg) || avg == 0) continue;
        latest[t] = avg;
    }
    std::vector<TimingProfile> profiles;
    profiles.reserve(latest.size());
    for (const auto& kv : latest) profiles.push_back({kv.first, kv.second});
    std::sort(profiles.begin(), profiles.end(), [](const TimingProfile& a, const TimingProfile& b) {
        if (a.avg_ns != b.avg_ns) return a.avg_ns < b.avg_ns;
        return a.threads < b.threads;
    });
    return profiles;
}

static bool UpsertTimingEntry(const std::string& file, uint32_t threads, uint64_t tries, double elapsed_seconds) {
    if (tries == 0 || elapsed_seconds <= 0.0) return false;
    const uint64_t avg_ns = static_cast<uint64_t>(std::llround((elapsed_seconds * 1e9) / static_cast<double>(tries)));
    const double rate = static_cast<double>(tries) / elapsed_seconds;

    std::vector<std::string> keep;
    {
        std::ifstream in(file);
        std::string line;
        while (std::getline(in, line)) {
            uint32_t t = 0;
            if (ParseThreadField(line, t) && t == threads) continue;
            if (!line.empty()) keep.push_back(line);
        }
    }

    std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char ts[64];
    std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
    std::ostringstream newline;
    newline << ts
            << " threads=" << threads
            << " tries=" << tries
            << " elapsed_s=" << elapsed_seconds
            << " avg_ns=" << avg_ns
            << " rate_ids_per_sec=" << std::fixed << std::setprecision(2) << rate;
    keep.push_back(newline.str());

    std::ofstream out(file, std::ios::out | std::ios::trunc);
    if (!out.is_open()) return false;
    for (const auto& l : keep) out << l << "\n";
    return out.good();
}

static Options ParseArgs(int argc, char** argv) {
    Options opts;
    opts.timing_file = DefaultTimingFile();
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](const std::string& name) -> std::string {
            if (i + 1 >= argc) Die("missing value for " + name);
            return argv[++i];
        };
        if (a == "--prefixes") opts.prefixes = need(a);
        else if (a == "--prefix-file") opts.prefix_file = need(a);
        else if (a == "--outdir") opts.outdir = need(a);
        else if (a == "--timing-file") opts.timing_file = need(a);
        else if (a == "--estimate") opts.estimate = true;
        else if (a == "--count") opts.count = std::stoull(need(a));
        else if (a == "--workers") opts.workers = static_cast<uint32_t>(std::stoul(need(a)));
        else if (a == "--status-interval") {
            opts.status_interval = std::stod(need(a));
            if (opts.status_interval < 1.0) Die("--status-interval must be >= 1");
        }
        else if (a == "--help" || a == "-h") {
            PrintUsage();
            std::exit(0);
        } else {
            Die("unknown arg: " + a);
        }
    }
    return opts;
}

static void PrintUsage() {
    std::cout << "Usage: vanity_onion --prefixes <p1,p2> | --prefix-file <file>\n"
                 "Options:\n"
                 "  --outdir <dir>               Output directory (default: onion_v3_private_keys)\n"
                 "  --timing-file <file>         Timing profile file (default: $HOME/.cache/bitcoin-vanity/onion-timing.log)\n"
                 "  --estimate                   Print probability + 50% estimates from timing profiles and exit\n"
                 "  --count <n>                  Number of matches (default: 1)\n"
                 "  --workers <n>                Worker threads (default: CPU count)\n"
                 "  --status-interval <sec>      Status interval (default: 5)\n";
}

int main(int argc, char** argv) {
    if (sodium_init() < 0) Die("libsodium init failed");

    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);

    if (argc == 1) {
        PrintUsage();
        return 0;
    }

    Options opts = ParseArgs(argc, argv);
    if (opts.count < 1) Die("--count must be >= 1");

    auto prefixes = LoadPrefixes(opts.prefixes, opts.prefix_file);
    const uint32_t hw = std::max(1u, std::thread::hardware_concurrency());
    const uint32_t workers = opts.workers == 0 ? hw : std::max(1u, opts.workers);

    std::cout << "prefixes: ";
    for (size_t i = 0; i < prefixes.size(); ++i) {
        if (i) std::cout << ", ";
        std::cout << prefixes[i];
    }
    std::cout << "\noutput: " << opts.outdir << "\nworkers: " << workers << "\n";

    double p_hit = HitProbability(prefixes);
    double n50 = AttemptsForProbability(p_hit, 0.5);
    if (opts.estimate) {
        std::cout << "probability_per_try: " << std::setprecision(12) << p_hit << "\n";
        if (std::isfinite(n50)) std::cout << "tries_for_50pct: " << std::fixed << std::setprecision(0) << n50 << "\n";
        else std::cout << "tries_for_50pct: unknown\n";
        auto profiles = LoadTimingProfiles(opts.timing_file);
        std::cout << "timing_profiles: " << profiles.size() << "\n";
        for (const auto& p : profiles) {
            const double rate = p.avg_ns > 0 ? (1e9 / static_cast<double>(p.avg_ns)) : 0.0;
            std::cout << "threads=" << p.threads
                      << " avg_ns=" << p.avg_ns
                      << " est_rate_ids_per_sec=" << std::fixed << std::setprecision(2) << rate;
            if (std::isfinite(n50) && rate > 0.0) {
                std::cout << " est_time_for_50pct=" << FormatDuration(n50 / rate);
            } else {
                std::cout << " est_time_for_50pct=unknown";
            }
            std::cout << "\n";
        }
        return 0;
    }

    if (std::isfinite(n50)) {
        std::cout << "per-attempt hit probability ~ " << std::scientific << std::setprecision(6) << p_hit
                  << ", 50% chance in ~ " << std::fixed << std::setprecision(0) << n50 << " attempts\n";
    }

    std::error_code ec;
    fs::create_directories(opts.outdir, ec);
    if (ec) Die("could not create output directory: " + opts.outdir.string());

    std::atomic<uint64_t> attempts{0};
    std::atomic<uint64_t> matches{0};
    std::mutex io_mu;

    auto worker_fn = [&](uint32_t) {
        uint8_t seed[32];
        uint8_t tor_key[crypto_sign_SECRETKEYBYTES];
        while (!g_stop.load()) {
            randombytes_buf(seed, sizeof(seed));
            ExpandSeedToTorKey(seed, tor_key, tor_key + 32);
            std::string service_id = OnionServiceIdFromTorKey(tor_key);
            bool hit = false;
            for (const auto& pref : prefixes) {
                if (PrefixMatchesServiceId(service_id, pref)) { hit = true; break; }
            }
            attempts.fetch_add(1, std::memory_order_relaxed);
            if (!hit) continue;

            char b64[sodium_base64_ENCODED_LEN(crypto_sign_SECRETKEYBYTES, sodium_base64_VARIANT_ORIGINAL)];
            sodium_bin2base64(b64, sizeof(b64), tor_key, crypto_sign_SECRETKEYBYTES, sodium_base64_VARIANT_ORIGINAL);
            bool wrote = WriteKeyFile(opts.outdir, service_id, b64, nullptr);
            if (wrote) {
                uint64_t m = matches.fetch_add(1) + 1;
                std::lock_guard<std::mutex> lock(io_mu);
                std::cout << "match " << m << "/" << opts.count << ": " << service_id << ".onion\n";
                if (m >= opts.count) {
                    g_stop.store(true);
                    break;
                }
            }
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(workers);
    for (uint32_t i = 0; i < workers; ++i) {
        threads.emplace_back(worker_fn, i);
    }

    auto start = std::chrono::steady_clock::now();
    auto last_status = start;
    uint64_t last_attempts = 0;
    struct RateSample {
        std::chrono::steady_clock::time_point ts;
        uint64_t attempts_delta;
        double elapsed_seconds;
    };
    std::deque<RateSample> rate_samples;
    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::duration<double>(opts.status_interval));
        uint64_t a = attempts.load();
        uint64_t delta = a - last_attempts;
        last_attempts = a;
        auto now = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(now - start).count();
        const double status_elapsed = std::chrono::duration<double>(now - last_status).count();
        last_status = now;
        const double rate = elapsed > 0 ? static_cast<double>(a) / elapsed : 0.0;
        rate_samples.push_back({now, delta, status_elapsed});
        const auto oldest_needed = now - std::chrono::seconds(15 * 60);
        while (!rate_samples.empty() && rate_samples.front().ts < oldest_needed) rate_samples.pop_front();
        auto boxcar_rate = [&rate_samples, now](double window_seconds) -> double {
            const auto cutoff = now - std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(window_seconds));
            uint64_t sum_delta = 0;
            double sum_elapsed = 0.0;
            for (const RateSample& s : rate_samples) {
                if (s.ts >= cutoff) {
                    sum_delta += s.attempts_delta;
                    sum_elapsed += s.elapsed_seconds;
                }
            }
            return (sum_elapsed > 0.0) ? (static_cast<double>(sum_delta) / sum_elapsed) : 0.0;
        };
        const double rate_1m = boxcar_rate(60.0);
        const double rate_5m = boxcar_rate(300.0);
        const double rate_15m = boxcar_rate(900.0);
        double p_so_far = (p_hit > 0.0) ? (1.0 - std::pow(1.0 - p_hit, static_cast<double>(a))) : 0.0;
        std::cout << "attempts: " << a << " (" << std::fixed << std::setprecision(0) << rate << " per sec)";
        if (std::isfinite(n50)) {
            const bool reached50 = (static_cast<double>(a) >= n50);
            const double remaining_attempts = reached50 ? 0.0 : std::max(0.0, n50 - static_cast<double>(a));
            std::string eta_1m = "??:??";
            std::string eta_5m = "??:??";
            std::string eta_15m = "??:??";
            if (reached50) {
                eta_1m = "--:--";
                eta_5m = "--:--";
                eta_15m = "--:--";
            } else {
                if (rate_1m > 0.0) eta_1m = FormatEtaClock(remaining_attempts / rate_1m);
                if (rate_5m > 0.0) eta_5m = FormatEtaClock(remaining_attempts / rate_5m);
                if (rate_15m > 0.0) eta_15m = FormatEtaClock(remaining_attempts / rate_15m);
            }
            if (elapsed < 60.0) {
                std::cout << ", 50% ETA(1m) " << eta_1m;
            } else if (elapsed < 300.0) {
                std::cout << ", 50% ETA(1m/5m) " << eta_1m << " / " << eta_5m;
            } else {
                std::cout << ", 50% ETA(1m/5m/15m) " << eta_1m << " / " << eta_5m << " / " << eta_15m;
            }
        }
        std::cout << ", current hit chance ~ " << std::fixed << std::setprecision(2) << (p_so_far * 100.0) << "%";
        std::cout << "\n";
        if (matches.load() >= opts.count) break;
        if (delta == 0 && g_stop.load()) break;
    }

    for (auto& t : threads) t.join();
    {
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (!UpsertTimingEntry(opts.timing_file, workers, attempts.load(), elapsed)) {
            std::cout << "warning: unable to update timing file " << opts.timing_file << "\n";
        }
    }
    return 0;
}
