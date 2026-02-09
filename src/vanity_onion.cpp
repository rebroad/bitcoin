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
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <sstream>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <crypto/sha3.h>
#include <span.h>

#include <sodium.h>

namespace fs = std::filesystem;

static const char* BASE32_ALPHABET = "abcdefghijklmnopqrstuvwxyz234567";
static const char* ONION_CHECKSUM_PREFIX = ".onion checksum";
static const uint8_t ONION_VERSION = 3;

static std::atomic<bool> g_stop{false};

static void SignalHandler(int) { g_stop.store(true); }

static void Die(const std::string& msg) {
    std::cerr << "error: " << msg << "\n";
    std::exit(2);
}

static bool IsValidBase32Char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= '2' && c <= '7');
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
        if (!IsValidBase32Char(c)) Die("invalid characters in prefix (allowed: a-z2-7)");
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
            if (line.empty() || line[0] == '#') continue;
            prefixes.insert(NormalizePrefix(line));
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

static double HitProbability(const std::vector<std::string>& prefixes) {
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
    hasher.Write(Span<const unsigned char>(reinterpret_cast<const unsigned char*>(ONION_CHECKSUM_PREFIX), std::strlen(ONION_CHECKSUM_PREFIX)));
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
    fs::create_directories(outdir);
    fs::path path = outdir / service_id;
    if (out_path) *out_path = path;
    if (fs::exists(path)) return false;
    std::ofstream f(path, std::ios::binary | std::ios::out);
    if (!f.is_open()) return false;
    f << "ED25519-V3:" << key_b64;
    f.close();
    return true;
}

static std::string NormalizeServiceId(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const std::string suffix = ".onion";
    if (s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0) {
        s.erase(s.size() - suffix.size());
    }
    return s;
}

static std::string TrimWhitespace(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
    return s;
}

static bool ExtractTorV3KeyB64(const std::string& key_data, std::string& key_b64_out) {
    const std::string prefix = "ED25519-V3:";
    if (key_data.rfind(prefix, 0) != 0) return false;
    key_b64_out = TrimWhitespace(key_data.substr(prefix.size()));
    return !key_b64_out.empty();
}

static bool ExtractTorV3Key(const std::string& key_data, std::array<uint8_t, crypto_sign_SECRETKEYBYTES>& sk_out) {
    std::string key_b64;
    if (!ExtractTorV3KeyB64(key_data, key_b64)) return false;
    size_t sk_len = 0;
    if (sodium_base642bin(sk_out.data(), sk_out.size(),
                          key_b64.data(), key_b64.size(),
                          nullptr, &sk_len, nullptr,
                          sodium_base64_VARIANT_ORIGINAL) != 0 || sk_len != crypto_sign_SECRETKEYBYTES) {
        return false;
    }
    return true;
}

static std::string OnionServiceIdFromTorKey(const std::array<uint8_t, crypto_sign_SECRETKEYBYTES>& sk) {
    // Tor v3 key file stores 64 bytes: [secret scalar(32) || PRF secret(32)].
    return OnionServiceIdFromTorKey(sk.data());
}

class TorControlClient {
public:
    explicit TorControlClient(std::string control, std::string cookie_path, std::string password)
        : m_control(std::move(control)), m_cookie_path(std::move(cookie_path)), m_password(std::move(password)) {}

    bool ConnectAndAuth(std::string& err) {
        if (!Connect(err)) return false;
        return Authenticate(err);
    }

    bool AddOnionServiceId(const std::string& key_b64, std::string& service_id, std::string& err) {
        const std::string cmd = "ADD_ONION ED25519-V3:" + key_b64 + " Port=80,127.0.0.1:1";
        std::vector<std::string> lines;
        if (!Command(cmd, lines, err)) return false;
        for (const auto& line : lines) {
            const std::string prefix = "250-ServiceID=";
            if (line.rfind(prefix, 0) == 0) {
                service_id = line.substr(prefix.size());
                return true;
            }
        }
        err = "missing ServiceID in response";
        return false;
    }

    void DelOnion(const std::string& service_id) {
        std::string err;
        std::vector<std::string> lines;
        Command("DEL_ONION " + service_id, lines, err);
    }

    ~TorControlClient() {
        if (m_fd >= 0) {
            close(m_fd);
            m_fd = -1;
        }
    }

private:
    bool Connect(std::string& err) {
        std::string host = "127.0.0.1";
        std::string port = "9051";
        const auto colon = m_control.find(':');
        if (colon != std::string::npos) {
            host = m_control.substr(0, colon);
            port = m_control.substr(colon + 1);
        }

        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0) {
            err = "getaddrinfo failed for control address";
            return false;
        }

        int fd = -1;
        for (addrinfo* p = res; p; p = p->ai_next) {
            fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
            if (fd < 0) continue;
            if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
            close(fd);
            fd = -1;
        }
        freeaddrinfo(res);

        if (fd < 0) {
            err = "failed to connect to control port";
            return false;
        }

        m_fd = fd;
        return true;
    }

    bool Authenticate(std::string& err) {
        if (!m_password.empty()) {
            return SimpleCommand("AUTHENTICATE \"" + m_password + "\"", err);
        }
        if (!m_cookie_path.empty()) {
            std::ifstream f(m_cookie_path, std::ios::binary | std::ios::in);
            if (!f.is_open()) {
                err = "failed to open control auth cookie";
                return false;
            }
            std::string cookie((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            f.close();
            if (cookie.size() != 32) {
                err = "auth cookie size is not 32 bytes";
                return false;
            }
            static const char hexmap[] = "0123456789ABCDEF";
            std::string hex;
            hex.resize(cookie.size() * 2);
            for (size_t i = 0; i < cookie.size(); ++i) {
                unsigned char c = static_cast<unsigned char>(cookie[i]);
                hex[2 * i] = hexmap[(c >> 4) & 0x0F];
                hex[2 * i + 1] = hexmap[c & 0x0F];
            }
            return SimpleCommand("AUTHENTICATE " + hex, err);
        }
        err = "no authentication method provided";
        return false;
    }

    bool SimpleCommand(const std::string& cmd, std::string& err) {
        std::vector<std::string> lines;
        return Command(cmd, lines, err);
    }

    bool Command(const std::string& cmd, std::vector<std::string>& lines, std::string& err) {
        if (m_fd < 0) {
            err = "not connected";
            return false;
        }
        std::string wire = cmd + "\r\n";
        if (!WriteAll(wire, err)) return false;
        return ReadReply(lines, err);
    }

    bool WriteAll(const std::string& data, std::string& err) {
        size_t off = 0;
        while (off < data.size()) {
            ssize_t n = send(m_fd, data.data() + off, data.size() - off, 0);
            if (n <= 0) {
                err = "failed to write to control port";
                return false;
            }
            off += static_cast<size_t>(n);
        }
        return true;
    }

    bool ReadReply(std::vector<std::string>& lines, std::string& err) {
        lines.clear();
        std::string line;
        while (true) {
            if (!ReadLine(line, err)) return false;
            lines.push_back(line);
            if (line.size() >= 4 && line.rfind("250 ", 0) == 0) return true;
            if (line.size() >= 3 && (line[0] == '5' || line[0] == '4')) {
                err = line;
                return false;
            }
        }
    }

    bool ReadLine(std::string& out, std::string& err) {
        out.clear();
        while (true) {
            auto pos = m_buf.find("\r\n");
            if (pos != std::string::npos) {
                out = m_buf.substr(0, pos);
                m_buf.erase(0, pos + 2);
                return true;
            }
            char tmp[512];
            ssize_t n = recv(m_fd, tmp, sizeof(tmp), 0);
            if (n <= 0) {
                err = "failed to read from control port";
                return false;
            }
            m_buf.append(tmp, tmp + n);
        }
    }

    std::string m_control;
    std::string m_cookie_path;
    std::string m_password;
    int m_fd{-1};
    std::string m_buf;
};

// TEMPORARY: extra verification during algorithm development. Remove once key generation is trusted.
static bool VerifyKeyFileMatchesFilename(const fs::path& path) {
    std::ifstream f(path, std::ios::binary | std::ios::in);
    if (!f.is_open()) {
        std::cerr << "temp-verify: could not open key file " << path << "\n";
        return false;
    }

    std::string key_data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    f.close();
    if (key_data.empty()) {
        std::cerr << "temp-verify: empty key file " << path << "\n";
        return false;
    }

    std::array<uint8_t, crypto_sign_SECRETKEYBYTES> sk;
    if (!ExtractTorV3Key(key_data, sk)) {
        std::cerr << "temp-verify: invalid key format in " << path << "\n";
        return false;
    }

    const std::string computed_id = OnionServiceIdFromTorKey(sk);
    const std::string filename_id = NormalizeServiceId(path.filename().string());
    if (computed_id != filename_id) {
        std::cerr << "temp-verify: mismatch in " << path << " (file name "
                  << filename_id << " vs computed " << computed_id << ")\n";
        return false;
    }

    return true;
}

struct Options {
    std::string prefixes;
    std::string prefix_file;
    fs::path outdir{"onion_v3_private_keys"};
    uint64_t count{1};
    uint32_t workers{0};
    double status_interval{5.0};
    bool temp_verify{true};
    bool temp_verify_dir_mode{false};
    fs::path temp_verify_dir;
    bool temp_verify_dir_tor_mode{false};
    fs::path temp_verify_dir_tor;
    std::string tor_control{"127.0.0.1:9051"};
    std::string tor_cookie{"/var/run/tor/control.authcookie"};
    std::string tor_password;
};

static void PrintUsage();

static Options ParseArgs(int argc, char** argv) {
    Options opts;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](const std::string& name) -> std::string {
            if (i + 1 >= argc) Die("missing value for " + name);
            return argv[++i];
        };
        if (a == "--prefixes") opts.prefixes = need(a);
        else if (a == "--prefix-file") opts.prefix_file = need(a);
        else if (a == "--outdir") opts.outdir = need(a);
        else if (a == "--count") opts.count = std::stoull(need(a));
        else if (a == "--workers") opts.workers = static_cast<uint32_t>(std::stoul(need(a)));
        else if (a == "--status-interval") opts.status_interval = std::stod(need(a));
        else if (a == "--temp-verify-dir") { opts.temp_verify_dir_mode = true; opts.temp_verify_dir = need(a); }
        else if (a == "--temp-verify-dir-tor") { opts.temp_verify_dir_tor_mode = true; opts.temp_verify_dir_tor = need(a); }
        else if (a == "--tor-control") { opts.tor_control = need(a); }
        else if (a == "--tor-cookie") { opts.tor_cookie = need(a); }
        else if (a == "--tor-password") { opts.tor_password = need(a); }
        else if (a == "--help" || a == "-h") {
            PrintUsage();
            std::exit(0);
        } else if (a == "--no-temp-verify") {
            opts.temp_verify = false;  // TEMP: option exists only while validating the generation algorithm
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
                 "  --count <n>                  Number of matches (default: 1)\n"
                 "  --workers <n>                Worker threads (default: CPU count)\n"
                 "  --status-interval <sec>      Status interval (default: 5)\n"
                 "  --no-temp-verify             TEMP: Disable key/filename verification\n"
                 "  --temp-verify-dir <dir>      TEMP: Scan existing directory and verify keys\n"
                 "  --temp-verify-dir-tor <dir>  TEMP: Verify directory via Tor control port\n"
                 "  --tor-control <host:port>    Tor control address (default: 127.0.0.1:9051)\n"
                 "  --tor-cookie <path>          Tor control auth cookie path\n"
                 "  --tor-password <pw>          Tor control password (overrides cookie)\n";
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
    if (opts.temp_verify_dir_tor_mode) {
        if (!fs::exists(opts.temp_verify_dir_tor) || !fs::is_directory(opts.temp_verify_dir_tor)) {
            Die("temp verify directory does not exist or is not a directory");
        }
        TorControlClient client(opts.tor_control, opts.tor_cookie, opts.tor_password);
        std::string err;
        if (!client.ConnectAndAuth(err)) {
            Die("tor control auth failed: " + err);
        }
        size_t total = 0;
        size_t bad = 0;
        for (const auto& entry : fs::directory_iterator(opts.temp_verify_dir_tor)) {
            if (!fs::is_regular_file(entry.status())) continue;
            std::ifstream f(entry.path(), std::ios::binary | std::ios::in);
            if (!f.is_open()) {
                std::cerr << "temp-verify-tor: could not open " << entry.path() << "\n";
                bad++;
                continue;
            }
            std::string key_data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            f.close();

            std::string key_b64;
            if (!ExtractTorV3KeyB64(key_data, key_b64)) {
                std::cerr << "temp-verify-tor: invalid key format in " << entry.path() << "\n";
                bad++;
                continue;
            }

            std::string service_id;
            if (!client.AddOnionServiceId(key_b64, service_id, err)) {
                std::cerr << "temp-verify-tor: ADD_ONION failed for " << entry.path() << ": " << err << "\n";
                bad++;
                continue;
            }
            client.DelOnion(service_id);

            const std::string filename_id = NormalizeServiceId(entry.path().filename().string());
            if (NormalizeServiceId(service_id) != filename_id) {
                std::cerr << "temp-verify-tor: mismatch in " << entry.path() << " (file name "
                          << filename_id << " vs tor " << service_id << ")\n";
                bad++;
            }
            total++;
        }
        std::cout << "temp-verify-tor: scanned " << total << " file(s), failures: " << bad << "\n";
        return bad == 0 ? 0 : 1;
    }

    if (opts.temp_verify_dir_mode) {
        if (!fs::exists(opts.temp_verify_dir) || !fs::is_directory(opts.temp_verify_dir)) {
            Die("temp verify directory does not exist or is not a directory");
        }
        size_t total = 0;
        size_t bad = 0;
        for (const auto& entry : fs::directory_iterator(opts.temp_verify_dir)) {
            if (!fs::is_regular_file(entry.status())) continue;
            total++;
            if (!VerifyKeyFileMatchesFilename(entry.path())) bad++;
        }
        std::cout << "temp-verify: scanned " << total << " file(s), failures: " << bad << "\n";
        return bad == 0 ? 0 : 1;
    }

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
    if (std::isfinite(n50)) {
        std::cout << "per-attempt hit probability ~ " << std::scientific << std::setprecision(6) << p_hit
                  << ", 50% chance in ~ " << std::fixed << std::setprecision(0) << n50 << " attempts\n";
    }

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
                if (service_id.rfind(pref, 0) == 0) { hit = true; break; }
            }
            attempts.fetch_add(1, std::memory_order_relaxed);
            if (!hit) continue;

            uint8_t pk_verify[crypto_sign_PUBLICKEYBYTES];
            uint8_t sk_verify[crypto_sign_SECRETKEYBYTES];
            crypto_sign_seed_keypair(pk_verify, sk_verify, seed);
            const std::string verify_id = OnionServiceIdFromPubkey(pk_verify);
            if (verify_id != service_id) {
                std::lock_guard<std::mutex> lock(io_mu);
                std::cout << "sanity check failed: key maps to " << verify_id
                          << ".onion, expected " << service_id << ".onion\n";
                continue;
            }

            char b64[sodium_base64_ENCODED_LEN(crypto_sign_SECRETKEYBYTES, sodium_base64_VARIANT_ORIGINAL)];
            sodium_bin2base64(b64, sizeof(b64), tor_key, crypto_sign_SECRETKEYBYTES, sodium_base64_VARIANT_ORIGINAL);
            fs::path out_path;
            bool wrote = WriteKeyFile(opts.outdir, service_id, b64, &out_path);
            if (wrote) {
                uint64_t m = matches.fetch_add(1) + 1;
                std::lock_guard<std::mutex> lock(io_mu);
                std::cout << "match " << m << "/" << opts.count << ": " << service_id << ".onion\n";
                // TEMPORARY: extra verification while the algorithm is being validated.
                if (opts.temp_verify && !VerifyKeyFileMatchesFilename(out_path)) {
                    std::cout << "temp-verify failed for " << out_path << "\n";
                }
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
    uint64_t last_attempts = 0;
    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::duration<double>(opts.status_interval));
        uint64_t a = attempts.load();
        uint64_t delta = a - last_attempts;
        last_attempts = a;
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - start).count();
        double rate = elapsed > 0 ? static_cast<double>(a) / elapsed : 0.0;
        double p_so_far = (p_hit > 0.0) ? (1.0 - std::pow(1.0 - p_hit, static_cast<double>(a))) : 0.0;
        std::cout << "attempts: " << a << " (" << std::fixed << std::setprecision(0) << rate << " per sec)";
        if (std::isfinite(n50) && rate > 0) {
            double remaining_attempts = std::max(0.0, n50 - static_cast<double>(a));
            double eta_seconds = remaining_attempts / rate;
            std::cout << ", est 50% time ~ " << FormatDuration(eta_seconds);
        }
        std::cout << ", current hit chance ~ " << std::fixed << std::setprecision(2) << (p_so_far * 100.0) << "%";
        std::cout << "\n";
        if (matches.load() >= opts.count) break;
        if (delta == 0 && g_stop.load()) break;
    }

    for (auto& t : threads) t.join();
    return 0;
}
