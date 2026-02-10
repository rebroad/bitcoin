// SPDX-License-Identifier: MIT
// Generate I2P destinations via SAM that match vanity .b32.i2p prefixes.

#include <crypto/sha256.h>
#include <netbase.h>
#include <span.h>
#include <threadinterrupt.h>
#include <util/sock.h>
#include <util/strencodings.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cctype>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

static constexpr size_t MAX_MSG_SIZE{65536};
static constexpr auto RECV_TIMEOUT = std::chrono::minutes{3};
static constexpr auto SEND_TIMEOUT = std::chrono::seconds{30};

static std::atomic<bool> g_stop{false};

static void SignalHandler(int) { g_stop.store(true); }

static void Die(const std::string& msg)
{
    std::cerr << "error: " << msg << "\n";
    std::exit(2);
}

static bool IsValidBase32Char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= '2' && c <= '7');
}

static std::string NormalizePrefix(std::string p)
{
    while (!p.empty() && (p.back() == '\r' || p.back() == '\n' || p.back() == ' ' || p.back() == '\t')) p.pop_back();
    while (!p.empty() && (p.front() == ' ' || p.front() == '\t')) p.erase(p.begin());
    for (char& c : p) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const std::string suffix = ".b32.i2p";
    if (p.size() >= suffix.size() && p.compare(p.size() - suffix.size(), suffix.size(), suffix) == 0) {
        p.erase(p.size() - suffix.size());
    }
    if (p.empty()) Die("empty prefix");
    if (p.size() > 52) Die("prefix too long (max 52 chars)");
    for (char c : p) {
        if (!IsValidBase32Char(c)) Die("invalid characters in prefix (allowed: a-z2-7)");
    }
    return p;
}

static std::vector<std::string> LoadPrefixes(const std::string& prefixes_arg, const std::string& prefix_file)
{
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

static std::vector<std::string> EffectivePrefixes(std::vector<std::string> prefixes)
{
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

static double HitProbability(const std::vector<std::string>& prefixes)
{
    auto eff = EffectivePrefixes(prefixes);
    double p = 0.0;
    for (const auto& pref : eff) {
        p += std::pow(32.0, -static_cast<int>(pref.size()));
    }
    return p;
}

static double AttemptsForProbability(double p_hit, double target)
{
    if (p_hit <= 0.0 || target <= 0.0 || target >= 1.0) return std::numeric_limits<double>::infinity();
    return std::log(1.0 - target) / std::log(1.0 - p_hit);
}

static std::string FormatDuration(double seconds)
{
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

static std::string SwapBase64(const std::string& from)
{
    std::string to;
    to.resize(from.size());
    for (size_t i = 0; i < from.size(); ++i) {
        switch (from[i]) {
        case '-': to[i] = '+'; break;
        case '~': to[i] = '/'; break;
        case '+': to[i] = '-'; break;
        case '/': to[i] = '~'; break;
        default: to[i] = from[i]; break;
        }
    }
    return to;
}

static std::vector<unsigned char> DecodeI2PBase64(const std::string& i2p_b64)
{
    const std::string std_b64 = SwapBase64(i2p_b64);
    bool invalid = false;
    auto decoded = DecodeBase64(std_b64.c_str(), &invalid);
    if (invalid) {
        throw std::runtime_error("cannot decode base64");
    }
    return decoded;
}

static std::string DestToB32(const std::vector<unsigned char>& dest)
{
    CSHA256 hasher;
    hasher.Write(dest.data(), dest.size());
    unsigned char hash[CSHA256::OUTPUT_SIZE];
    hasher.Finalize(hash);
    return EncodeBase32(Span<const unsigned char>(hash, CSHA256::OUTPUT_SIZE), false);
}

struct Reply {
    std::string full;
    std::unordered_map<std::string, std::optional<std::string>> keys;

    std::string Get(const std::string& key) const
    {
        const auto it = keys.find(key);
        if (it == keys.end() || !it->second.has_value()) {
            throw std::runtime_error("missing key in reply: " + key);
        }
        return it->second.value();
    }
};

static Reply ParseReply(const std::string& full)
{
    Reply reply;
    reply.full = full;
    size_t start = 0;
    while (start <= full.size()) {
        size_t end = full.find(' ', start);
        if (end == std::string::npos) end = full.size();
        if (end > start) {
            std::string token = full.substr(start, end - start);
            size_t eq = token.find('=');
            if (eq != std::string::npos) {
                reply.keys.emplace(token.substr(0, eq), token.substr(eq + 1));
            } else {
                reply.keys.emplace(token, std::nullopt);
            }
        }
        start = end + 1;
    }
    return reply;
}

static Reply SendRequestAndGetReply(const Sock& sock, const std::string& request, CThreadInterrupt& interrupt, bool check_ok = true)
{
    sock.SendComplete(request + "\n", SEND_TIMEOUT, interrupt);
    Reply reply = ParseReply(sock.RecvUntilTerminator('\n', RECV_TIMEOUT, interrupt, MAX_MSG_SIZE));
    if (check_ok) {
        const std::string result = reply.Get("RESULT");
        if (result != "OK") {
            throw std::runtime_error("unexpected reply to \"" + request + "\": \"" + reply.full + "\"");
        }
    }
    return reply;
}

static std::unique_ptr<Sock> ConnectSam(const CService& sam_addr, CThreadInterrupt& interrupt)
{
    auto sock = CreateSock(sam_addr);
    if (!sock) {
        throw std::runtime_error("cannot create socket");
    }
    if (!ConnectSocketDirectly(sam_addr, *sock, nConnectTimeout, true)) {
        throw std::runtime_error("cannot connect to SAM at " + sam_addr.ToString());
    }
    SendRequestAndGetReply(*sock, "HELLO VERSION MIN=3.1 MAX=3.1", interrupt);
    return sock;
}

static bool WriteKeyFile(const fs::path& path, const std::vector<unsigned char>& priv)
{
    fs::create_directories(path.parent_path());
    if (fs::exists(path)) return false;
    std::ofstream f(fs::PathToString(path), std::ios::binary | std::ios::out);
    if (!f.is_open()) return false;
    f.write(reinterpret_cast<const char*>(priv.data()), priv.size());
    f.close();
    return true;
}

struct Options {
    std::string prefixes;
    std::string prefix_file;
    std::string sam{"127.0.0.1:7656"};
    fs::path outdir{fs::PathFromString("i2p_private_keys")};
    fs::path outfile;
    uint64_t count{1};
    uint32_t workers{0};
    double status_interval{5.0};
};

static void PrintUsage();

static Options ParseArgs(int argc, char** argv)
{
    Options opts;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](const std::string& name) -> std::string {
            if (i + 1 >= argc) Die("missing value for " + name);
            return argv[++i];
        };
        if (a == "--prefixes") opts.prefixes = need(a);
        else if (a == "--prefix-file") opts.prefix_file = need(a);
        else if (a == "--sam") opts.sam = need(a);
        else if (a == "--outdir") opts.outdir = fs::PathFromString(need(a));
        else if (a == "--outfile") opts.outfile = fs::PathFromString(need(a));
        else if (a == "--count") opts.count = std::stoull(need(a));
        else if (a == "--workers") opts.workers = static_cast<uint32_t>(std::stoul(need(a)));
        else if (a == "--status-interval") opts.status_interval = std::stod(need(a));
        else if (a == "--help" || a == "-h") {
            PrintUsage();
            std::exit(0);
        } else {
            Die("unknown arg: " + a);
        }
    }
    return opts;
}

static void PrintUsage()
{
    std::cout << "Usage: vanity_i2p --prefixes <p1,p2> | --prefix-file <file>\n"
                 "Options:\n"
                 "  --sam <ip:port>              SAM host (default: 127.0.0.1:7656)\n"
                 "  --outdir <dir>               Output directory (default: i2p_private_keys)\n"
                 "  --outfile <file>             Output file (single match only)\n"
                 "  --count <n>                  Number of matches (default: 1)\n"
                 "  --workers <n>                Worker threads (default: CPU count)\n"
                 "  --status-interval <sec>      Status interval (default: 5)\n";
}

int main(int argc, char** argv)
{
    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);

    if (argc == 1) {
        PrintUsage();
        return 0;
    }

    Options opts = ParseArgs(argc, argv);
    if (opts.count < 1) Die("--count must be >= 1");
    if (!opts.outfile.empty() && opts.count != 1) Die("--outfile requires --count 1");

    const auto prefixes = LoadPrefixes(opts.prefixes, opts.prefix_file);
    const uint32_t hw = std::max(1u, std::thread::hardware_concurrency());
    const uint32_t workers = opts.workers == 0 ? hw : std::max(1u, opts.workers);

    const CService sam_addr = LookupNumeric(opts.sam, 7656);
    if (!sam_addr.IsValid()) Die("invalid --sam address");

    std::cout << "prefixes: ";
    for (size_t i = 0; i < prefixes.size(); ++i) {
        if (i) std::cout << ", ";
        std::cout << prefixes[i];
    }
    std::cout << "\nSAM: " << opts.sam << "\n";
    if (!opts.outfile.empty()) {
        std::cout << "outfile: " << opts.outfile << "\n";
    } else {
        std::cout << "outdir: " << opts.outdir << "\n";
    }
    std::cout << "workers: " << workers << "\n";

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
        CThreadInterrupt interrupt;
        std::unique_ptr<Sock> sock;

        while (!g_stop.load()) {
            try {
                if (!sock) {
                    sock = ConnectSam(sam_addr, interrupt);
                }

                Reply reply = SendRequestAndGetReply(*sock, "DEST GENERATE SIGNATURE_TYPE=7", interrupt, false);
                const std::string pub_b64 = reply.Get("PUB");
                const std::string priv_b64 = reply.Get("PRIV");

                const std::vector<unsigned char> dest = DecodeI2PBase64(pub_b64);
                const std::vector<unsigned char> priv = DecodeI2PBase64(priv_b64);
                const std::string service_id = DestToB32(dest);

                attempts.fetch_add(1, std::memory_order_relaxed);
                bool hit = false;
                for (const auto& pref : prefixes) {
                    if (service_id.rfind(pref, 0) == 0) { hit = true; break; }
                }
                if (!hit) continue;

                fs::path out_path;
                if (!opts.outfile.empty()) {
                    out_path = opts.outfile;
                } else {
                    out_path = opts.outdir / (service_id + ".b32.i2p");
                }

                bool wrote = WriteKeyFile(out_path, priv);
                if (wrote) {
                    uint64_t m = matches.fetch_add(1) + 1;
                    std::lock_guard<std::mutex> lock(io_mu);
                    std::cout << "match " << m << "/" << opts.count << ": " << service_id << ".b32.i2p\n";
                    if (m >= opts.count) {
                        g_stop.store(true);
                        break;
                    }
                }
            } catch (const std::exception& e) {
                std::lock_guard<std::mutex> lock(io_mu);
                std::cout << "SAM error: " << e.what() << "\n";
                sock.reset();
                std::this_thread::sleep_for(std::chrono::seconds(1));
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
