// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <net.h>

#include <addrdb.h>
#include <addrman.h>
#include <banman.h>
#include <clientversion.h>
#include <compat.h>
#include <consensus/consensus.h>
#include <crypto/sha256.h>
#include <fs.h>
#include <i2p.h>
#include <net_permissions.h>
#include <netaddress.h>
#include <netbase.h>
#include <torcontrol.h>
#include <node/ui_interface.h>
#include <protocol.h>
#include <random.h>
#include <scheduler.h>
#include <util/netif.h>
#include <util/sock.h>
#include <util/strencodings.h>
#include <util/syscall_sandbox.h>
#include <util/system.h>
#include <util/thread.h>
#include <torcontrol.h>
#include <util/trace.h>
#include <util/translation.h>
#include <util/perfmon.h>

#ifdef WIN32
#include <string.h>
#else
#include <fcntl.h>
#endif

#if HAVE_DECL_GETIFADDRS && HAVE_DECL_FREEIFADDRS
#include <ifaddrs.h>
#endif

#ifdef USE_POLL
#include <poll.h>
#endif

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include <math.h>

/** Maximum number of block-relay-only anchor connections */
static constexpr size_t MAX_BLOCK_RELAY_ONLY_ANCHORS = 2;
static_assert (MAX_BLOCK_RELAY_ONLY_ANCHORS <= static_cast<size_t>(MAX_BLOCK_RELAY_ONLY_CONNECTIONS), "MAX_BLOCK_RELAY_ONLY_ANCHORS must not exceed MAX_BLOCK_RELAY_ONLY_CONNECTIONS.");
/** Anchor IP address database file name */
const char* const ANCHORS_DATABASE_FILENAME = "txanchors.dat";
/** Block anchor IP address database file name */
const char* const BLOCK_ANCHORS_DATABASE_FILENAME = "blkanchors.dat";
/** Legacy IBD anchor IP address database file name (read fallback only). */
const char* const LEGACY_IBD_ANCHORS_DATABASE_FILENAME = "IBDanchors.dat";

static constexpr int64_t IBD_SPEED_MEASUREMENT_WINDOW{180};   // 3 minutes
static constexpr int64_t IBD_SLOW_PEER_DISCONNECT_DELAY{180}; // 3 minutes
static constexpr int64_t IBD_CALIBRATION_WINDOW{180};         // 3 minutes
static constexpr int64_t IBD_ANCHOR_DUMP_INTERVAL{300};       // 5 minutes

// How often to dump addresses to peers.dat
static constexpr std::chrono::minutes DUMP_PEERS_INTERVAL{15};

/** Number of DNS seeds to query when the number of connections is low. */
static constexpr int DNSSEEDS_TO_QUERY_AT_ONCE = 3;

/** How long to delay before querying DNS seeds
 *
 * If we have more than THRESHOLD entries in addrman, then it's likely
 * that we got those addresses from having previously connected to the P2P
 * network, and that we'll be able to successfully reconnect to the P2P
 * network via contacting one of them. So if that's the case, spend a
 * little longer trying to connect to known peers before querying the
 * DNS seeds.
 */
static constexpr std::chrono::seconds DNSSEEDS_DELAY_FEW_PEERS{11};
static constexpr std::chrono::minutes DNSSEEDS_DELAY_MANY_PEERS{5};
static constexpr int DNSSEEDS_DELAY_PEER_THRESHOLD = 1000; // "many" vs "few" peers

/** The default timeframe for -maxuploadtarget. 1 day. */
static constexpr std::chrono::seconds MAX_UPLOAD_TIMEFRAME{60 * 60 * 24};

// We add a random period time (0 to 1 seconds) to feeler connections to prevent synchronization.
#define FEELER_SLEEP_WINDOW 1

/** Used to pass flags to the Bind() function */
enum BindFlags {
    BF_NONE         = 0,
    BF_EXPLICIT     = (1U << 0),
    BF_REPORT_ERROR = (1U << 1),
    /**
     * Do not call AddLocal() for our special addresses, e.g., for incoming
     * Tor connections, to prevent gossiping them over the network.
     */
    BF_DONT_ADVERTISE = (1U << 2),
};

// The set of sockets cannot be modified while waiting
// The sleep time needs to be small to avoid new sockets stalling
static const uint64_t SELECT_TIMEOUT_MILLISECONDS = 50;

const std::string NET_MESSAGE_COMMAND_OTHER = "*other*";

static const uint64_t RANDOMIZER_ID_NETGROUP = 0x6c0edd8036ef4036ULL; // SHA256("netgroup")[0:8]
static const uint64_t RANDOMIZER_ID_LOCALHOSTNONCE = 0xd93e69e2bbfa5735ULL; // SHA256("localhostnonce")[0:8]
static const uint64_t RANDOMIZER_ID_ADDRCACHE = 0x1cf2e4ddd306dda9ULL; // SHA256("addrcache")[0:8]
//
// Global state variables
//
bool fDiscover = true;
bool fListen = true;
Mutex g_maplocalhost_mutex;
std::map<CNetAddr, LocalServiceInfo> mapLocalHost GUARDED_BY(g_maplocalhost_mutex);
static bool vfLimited[NET_MAX] GUARDED_BY(g_maplocalhost_mutex) = {};
std::string strSubVersion;
std::atomic<int> nBlocksToBeProcessed(0);
static int nAnchorTryAgain = -1; // -1 so that we skip the sleeps on the first ReadAnchor

void CConnman::AddAddrFetch(const std::string& strDest)
{
    LOCK(m_addr_fetches_mutex);
    m_addr_fetches.push_back(strDest);
}

uint16_t GetListenPort()
{
    return static_cast<uint16_t>(gArgs.GetIntArg("-port", Params().GetDefaultPort()));
}

// find 'best' local address for a particular peer
bool GetLocal(CService& addr, const CNetAddr *paddrPeer)
{
    if (!fListen)
        return false;

    int nBestScore = -1;
    int nBestReachability = -1;
    {
        LOCK(g_maplocalhost_mutex);
        for (const auto& entry : mapLocalHost)
        {
            int nScore = entry.second.nScore;
            int nReachability = entry.first.GetReachabilityFrom(paddrPeer);
            if (nReachability > nBestReachability || (nReachability == nBestReachability && nScore > nBestScore))
            {
                addr = CService(entry.first, entry.second.nPort);
                nBestReachability = nReachability;
                nBestScore = nScore;
            }
        }
    }
    return nBestScore >= 0;
}

//! Convert the serialized seeds into usable address objects.
static std::vector<CAddress> ConvertSeeds(const std::vector<uint8_t> &vSeedsIn)
{
    // It'll only connect to one or two seed nodes because once it connects,
    // it'll get a pile of addresses with newer timestamps.
    // Seed nodes are given a random 'last seen time' of between one and two
    // weeks ago.
    const int64_t nOneWeek = 7*24*60*60;
    std::vector<CAddress> vSeedsOut;
    FastRandomContext rng;
    CDataStream s(vSeedsIn, SER_NETWORK, PROTOCOL_VERSION | ADDRV2_FORMAT);
    while (!s.eof()) {
        CService endpoint;
        s >> endpoint;
        CAddress addr{endpoint, GetDesirableServiceFlags(NODE_NONE)};
        addr.nTime = GetTime() - rng.randrange(nOneWeek) - nOneWeek;
        LogPrintf("Added hardcoded seed: %s\n", addr.ToString());
        vSeedsOut.push_back(addr);
    }
    return vSeedsOut;
}

// get best local address for a particular peer as a CAddress
// Otherwise, return the unroutable 0.0.0.0 but filled in with
// the normal parameters, since the IP may be changed to a useful
// one by discovery.
CAddress GetLocalAddress(const CNetAddr *paddrPeer, ServiceFlags nLocalServices)
{
    CAddress ret(CService(CNetAddr(),GetListenPort()), nLocalServices);
    CService addr;
    if (GetLocal(addr, paddrPeer))
    {
        ret = CAddress(addr, nLocalServices);
    }
    ret.nTime = GetAdjustedTime();
    return ret;
}

static int GetnScore(const CService& addr)
{
    LOCK(g_maplocalhost_mutex);
    const auto it = mapLocalHost.find(addr);
    return (it != mapLocalHost.end()) ? it->second.nScore : 0;
}

// Is our peer's addrLocal potentially useful as an external IP source?
bool IsPeerAddrLocalGood(CNode *pnode)
{
    CService addrLocal = pnode->GetAddrLocal();
    return fDiscover && pnode->addr.IsRoutable() && addrLocal.IsRoutable() &&
           IsReachable(addrLocal.GetNetwork());
}

std::optional<CAddress> GetLocalAddrForPeer(CNode *pnode)
{
    CAddress addrLocal = GetLocalAddress(&pnode->addr, pnode->GetLocalServices());
    if (gArgs.GetBoolArg("-addrmantest", false)) {
        // use IPv4 loopback during addrmantest
        addrLocal = CAddress(CService(LookupNumeric("127.0.0.1", GetListenPort())), pnode->GetLocalServices());
    }
    // If discovery is enabled, sometimes give our peer the address it
    // tells us that it sees us as in case it has a better idea of our
    // address than we do.
    FastRandomContext rng;
    if (IsPeerAddrLocalGood(pnode) && (!addrLocal.IsRoutable() ||
         rng.randbits((GetnScore(addrLocal) > LOCAL_MANUAL) ? 3 : 1) == 0))
    {
        addrLocal.SetIP(pnode->GetAddrLocal());
    }
    if (addrLocal.IsRoutable() || gArgs.GetBoolArg("-addrmantest", false))
    {
        LogPrint(BCLog::NET, "Advertising address %s to peer=%d\n", addrLocal.ToString(), pnode->GetId());
        return addrLocal;
    }
    // Address is unroutable. Don't advertise.
    return std::nullopt;
}

/**
 * If an IPv6 address belongs to the address range used by the CJDNS network and
 * the CJDNS network is reachable (-cjdnsreachable config is set), then change
 * the type from NET_IPV6 to NET_CJDNS.
 * @param[in] service Address to potentially convert.
 * @return a copy of `service` either unmodified or changed to CJDNS.
 */
CService MaybeFlipIPv6toCJDNS(const CService& service)
{
    CService ret{service};
    if (ret.m_net == NET_IPV6 && ret.m_addr[0] == 0xfc && IsReachable(NET_CJDNS)) {
        ret.m_net = NET_CJDNS;
    }
    return ret;
}

// learn a new local address
bool AddLocal(const CService& addr_, int nScore)
{
    CService addr{MaybeFlipIPv6toCJDNS(addr_)};

    if (!addr.IsRoutable())
        return false;

    if (!fDiscover && nScore < LOCAL_MANUAL)
        return false;

    if (!IsReachable(addr))
        return false;

    LogPrintf("AddLocal(%s,%i)\n", addr.ToString(), nScore);

    {
        LOCK(g_maplocalhost_mutex);
        const auto [it, is_newly_added] = mapLocalHost.emplace(addr, LocalServiceInfo());
        LocalServiceInfo &info = it->second;
        if (is_newly_added || nScore >= info.nScore) {
            info.nScore = nScore + (is_newly_added ? 0 : 1);
            info.nPort = addr.GetPort();
        }
    }

    return true;
}

bool AddLocal(const CNetAddr &addr, int nScore)
{
    return AddLocal(CService(addr, GetListenPort()), nScore);
}

void RemoveLocal(const CService& addr)
{
    LOCK(g_maplocalhost_mutex);
    LogPrintf("RemoveLocal(%s)\n", addr.ToString());
    mapLocalHost.erase(addr);
}

void SetReachable(enum Network net, bool reachable)
{
    if (net == NET_UNROUTABLE || net == NET_INTERNAL)
        return;
    LOCK(g_maplocalhost_mutex);
    vfLimited[net] = !reachable;
}

bool IsReachable(enum Network net)
{
    LOCK(g_maplocalhost_mutex);
    return !vfLimited[net];
}

bool IsReachable(const CNetAddr &addr)
{
    return IsReachable(addr.GetNetwork());
}

/** vote for a local address */
bool SeenLocal(const CService& addr)
{
    LOCK(g_maplocalhost_mutex);
    const auto it = mapLocalHost.find(addr);
    if (it == mapLocalHost.end()) return false;
    ++it->second.nScore;
    return true;
}


/** check whether a given address is potentially local */
bool IsLocal(const CService& addr)
{
    LOCK(g_maplocalhost_mutex);
    return mapLocalHost.count(addr) > 0;
}

CNode* CConnman::FindNode(const CNetAddr& ip)
{
    LOCK(m_nodes_mutex);
    for (CNode* pnode : m_nodes) {
      if (static_cast<CNetAddr>(pnode->addr) == ip) {
            return pnode;
        }
    }
    return nullptr;
}

CNode* CConnman::FindNode(const CSubNet& subNet)
{
    LOCK(m_nodes_mutex);
    for (CNode* pnode : m_nodes) {
        if (subNet.Match(static_cast<CNetAddr>(pnode->addr))) {
            return pnode;
        }
    }
    return nullptr;
}

CNode* CConnman::FindNode(const std::string& addrName)
{
    LOCK(m_nodes_mutex);
    for (CNode* pnode : m_nodes) {
        if (pnode->m_addr_name == addrName) {
            return pnode;
        }
    }
    return nullptr;
}

CNode* CConnman::FindNode(const CService& addr)
{
    LOCK(m_nodes_mutex);
    for (CNode* pnode : m_nodes) {
        if (static_cast<CService>(pnode->addr) == addr) {
            return pnode;
        }
    }
    return nullptr;
}

bool CConnman::AlreadyConnectedToAddress(const CAddress& addr)
{
    return FindNode(static_cast<CNetAddr>(addr)) || FindNode(addr.ToStringIPPort());
}

bool CConnman::CheckIncomingNonce(uint64_t nonce)
{
    LOCK(m_nodes_mutex);
    for (const CNode* pnode : m_nodes) {
        if (!pnode->fSuccessfullyConnected && !pnode->IsInboundConn() && pnode->GetLocalNonce() == nonce)
            return false;
    }
    return true;
}

/** Get the bind address for a socket as CAddress */
static CAddress GetBindAddress(SOCKET sock)
{
    CAddress addr_bind;
    struct sockaddr_storage sockaddr_bind;
    socklen_t sockaddr_bind_len = sizeof(sockaddr_bind);
    if (sock != INVALID_SOCKET) {
        if (!getsockname(sock, (struct sockaddr*)&sockaddr_bind, &sockaddr_bind_len)) {
            addr_bind.SetSockAddr((const struct sockaddr*)&sockaddr_bind);
        } else {
            LogPrint(BCLog::NET, "Warning: getsockname failed\n");
        }
    }
    return addr_bind;
}

CNode* CConnman::ConnectNode(CAddress addrConnect, const char *pszDest, bool fCountFailure, ConnectionType conn_type)
{
    assert(conn_type != ConnectionType::INBOUND);

    if (pszDest == nullptr) {
        if (IsLocal(addrConnect))
            return nullptr;

        // Look for an existing connection
        CNode* pnode = FindNode(static_cast<CService>(addrConnect));
        if (pnode)
        {
            LogPrintf("Failed to open new connection, already connected\n");
            return nullptr;
        }
    }

    /// debug print
    size_t m_nodesSize;
    {
        LOCK(m_nodes_mutex);
        m_nodesSize = m_nodes.size();
    }
    LogPrint(BCLog::CONN, "trying %s connection(%d) %s lastseen=%s\n", ConnectionTypeAsString(conn_type),
        m_nodesSize, pszDest ? pszDest : addrConnect.ToString(), // REBTODO - lastseen by us? new?
        pszDest ? "now" : strAge(GetAdjustedTime() - addrConnect.nTime));

    // Resolve
    const uint16_t default_port{pszDest != nullptr ? Params().GetDefaultPort(pszDest) :
                                                     Params().GetDefaultPort()};
    if (pszDest) {
        std::vector<CService> resolved;
        if (Lookup(pszDest, resolved,  default_port, fNameLookup && !HaveNameProxy(), 256) && !resolved.empty()) {
            const CService rnd{resolved[GetRand(resolved.size())]};
            addrConnect = CAddress{MaybeFlipIPv6toCJDNS(rnd), NODE_NONE};
            if (!addrConnect.IsValid()) {
                LogPrint(BCLog::NET, "Resolver returned invalid address %s for %s\n", addrConnect.ToString(), pszDest);
                return nullptr;
            }
            // It is possible that we already have a connection to the IP/port pszDest resolved to.
            // In that case, drop the connection that was just created.
            LOCK(m_nodes_mutex);
            CNode* pnode = FindNode(static_cast<CService>(addrConnect));
            if (pnode) {
                LogPrintf("Failed to open new connection, already connected\n");
                return nullptr;
            }
        }
    }

    // Connect
    bool connected = false;
    std::unique_ptr<Sock> sock;
    proxyType proxy;
    CAddress addr_bind;
    assert(!addr_bind.IsValid());

    if (addrConnect.IsValid()) {
        bool proxyConnectionFailed = false;

        if (addrConnect.GetNetwork() == NET_I2P && GetI2POutgoingSession() != nullptr) {
            i2p::Connection conn;
            if (GetI2POutgoingSession()->Connect(addrConnect, conn, proxyConnectionFailed)) {
                connected = true;
                sock = std::move(conn.sock);
                addr_bind = CAddress{conn.me, NODE_NONE};
            }
        } else if (GetProxy(addrConnect.GetNetwork(), proxy)) {
            sock = CreateSock(proxy.proxy);
            if (!sock) {
                return nullptr;
            }
            connected = ConnectThroughProxy(proxy, addrConnect.ToStringIP(), addrConnect.GetPort(),
                                            *sock, nConnectTimeout, proxyConnectionFailed);
        } else {
            // no proxy needed (none set for target network)
            sock = CreateSock(addrConnect);
            if (!sock) {
                return nullptr;
            }
            connected = ConnectSocketDirectly(addrConnect, *sock, nConnectTimeout,
                                              conn_type == ConnectionType::MANUAL);
        }
        if (!proxyConnectionFailed)
            // If a connection to the node was attempted, and failure (if any) is not caused by a problem connecting to
            // the proxy, mark this as an attempt.
            addrman.Attempt(addrConnect, fCountFailure);
    } else if (pszDest && GetNameProxy(proxy)) {
        sock = CreateSock(proxy.proxy);
        if (!sock) return nullptr;
        std::string host;
        uint16_t port{default_port};
        SplitHostPort(std::string(pszDest), port, host);
        bool proxyConnectionFailed;
        connected = ConnectThroughProxy(proxy, host, port, *sock, nConnectTimeout,
                                        proxyConnectionFailed);
    }
    if (!connected) return nullptr;

    // Add node
    NodeId id = GetNewNodeId();
    uint64_t nonce = GetDeterministicRandomizer(RANDOMIZER_ID_LOCALHOSTNONCE).Write(id).Finalize();
    if (!addr_bind.IsValid()) addr_bind = GetBindAddress(sock->Get());
    CNode* pnode = new CNode(id, nLocalServices, std::move(sock), addrConnect,
            CalculateKeyedNetGroup(addrConnect),
            nonce, addr_bind, pszDest ? pszDest : "", conn_type, /*inbound_onion=*/false);
    pnode->AddRef(1); // REB - Creation (out)
    if (pnode->GetId() == 0)
        LogPrintf("%s: Created pnode=%d GRC=%d\n", __func__, pnode->GetId(), pnode->GetRefCount());

    // We're making a new connection, harvest entropy from the time (and our peer count)
    RandAddEvent((uint32_t)id);

    return pnode;
}

void CNode::CloseSocketDisconnect() {
    fDisconnect = true;
    LOCK(m_sock_mutex);
    if (m_sock) {
        LogPrint(BCLog::NET, "disconnecting peer=%d\n", id);
        m_sock.reset();
    }
}

void CConnman::AddWhitelistPermissionFlags(NetPermissionFlags& flags, const CNetAddr &addr) const {
    for (const auto& subnet : vWhitelistedRange) {
        if (subnet.m_subnet.Match(addr)) NetPermissions::AddFlag(flags, subnet.m_flags);
    }
}

std::string ConnectionTypeAsString(ConnectionType conn_type)
{
    switch (conn_type) {
    case ConnectionType::INBOUND:
        return "inbound";
    case ConnectionType::MANUAL:
        return "manual";
    case ConnectionType::FEELER:
        return "feeler";
    case ConnectionType::OUTBOUND_FULL_RELAY:
        return "full-relay";
    case ConnectionType::BLOCK_RELAY:
        return "block-relay";
    case ConnectionType::ADDR_FETCH:
        return "addr-fetch";
    } // no default case, so the compiler can warn about missing cases

    assert(false);
}

CService CNode::GetAddrLocal() const
{
    AssertLockNotHeld(m_addr_local_mutex);
    LOCK(m_addr_local_mutex);
    return addrLocal;
}

void CNode::SetAddrLocal(const CService& addrLocalIn) {
    AssertLockNotHeld(m_addr_local_mutex);
    LOCK(m_addr_local_mutex);
    if (addrLocal.IsValid()) {
        error("Addr local already set for node: %i. Refusing to change from %s to %s", id, addrLocal.ToString(), addrLocalIn.ToString());
    } else {
        addrLocal = addrLocalIn;
    }
}

Network CNode::ConnectedThroughNetwork() const {
    return m_inbound_onion ? NET_ONION : addr.GetNetClass();
}

#undef X
#define X(name) stats.name = name
void CNode::CopyStats(CNodeStats& stats) {
    stats.nodeid = this->GetId();
    X(nServices);
    X(addr);
    X(addrBind);
    stats.m_network = ConnectedThroughNetwork();
    if (m_tx_relay != nullptr) {
        LOCK(m_tx_relay->cs_filter);
        stats.fRelayTxes = m_tx_relay->fRelayTxes;
    } else
        stats.fRelayTxes = false;
    X(m_last_send);
    X(m_last_recv);
    X(m_last_tx_time);
    X(m_last_block_time);
    X(m_connected);
    X(nTimeOffset);
    X(m_addr_name);
    X(nVersion);
    {
        LOCK(m_subver_mutex);
        X(cleanSubVer);
    }
    stats.fInbound = IsInboundConn();
    X(fErlay);
    X(m_bip152_highbandwidth_to);
    X(m_bip152_highbandwidth_from);
    {
        LOCK(cs_vSend);
        X(mapSendBytesPerMsgCmd);
        X(nSendBytes);
    }
    {
        LOCK(cs_vRecv);
        X(mapRecvBytesPerMsgCmd);
        X(nRecvBytes);
    }
    X(nMempoolBytes);
    X(nMempoolBytesSnapOld);
    X(nMempoolTXs);
    X(nMempoolTXsSnapOld);
    X(nBlockBytes);
    X(nBlockBytesSnapOld);
    X(nBlockTXs);
    X(nBlockTXsSnapOld);
    X(nSendBytesSnapOld);
    X(nRecvBytesSnapOld);
    X(nTimeSnapOld);
    X(nBTxBpsPct);
    X(nBTXpm);
    X(m_permissionFlags);
    if (m_tx_relay != nullptr) {
        stats.minFeeFilter = m_tx_relay->minFeeFilter;
    } else {
        stats.minFeeFilter = 0;
    }

    X(m_last_ping_time);
    X(m_min_ping_time);

    // Leave string empty if addrLocal invalid (not filled in yet)
    CService addrLocalUnlocked = GetAddrLocal();
    stats.addrLocal = addrLocalUnlocked.IsValid() ? addrLocalUnlocked.ToString() : "";

    X(m_conn_type);
}
#undef X

bool CNode::ReceiveMsgBytes(Span<const uint8_t> msg_bytes, bool& complete)
{
    const bool use_peer_stats_snapshots{gArgs.GetBoolArg("-peerstatssnapshots", false)};
    complete = false;
    const auto time = GetTime<std::chrono::microseconds>();
    LOCK(cs_vRecv);
    {
        PERF_MONITOR("net_receive_msg_bytes_account");
        m_last_recv = std::chrono::duration_cast<std::chrono::seconds>(time);
        nRecvBytes += msg_bytes.size();
    }
    while (msg_bytes.size() > 0) {
        // absorb network data
        int handled = 0;
        {
            PERF_MONITOR("net_receive_msg_bytes_deserialize");
            handled = m_deserializer->Read(msg_bytes);
        }
        if (handled < 0) {
            // Serious header problem, disconnect from the peer.
            return false;
        }

        if (m_deserializer->Complete()) {
            // decompose a transport agnostic CNetMessage from the deserializer
            bool reject_message{false};
            CNetMessage msg{[&] {
                PERF_MONITOR("net_receive_msg_bytes_get_message");
                return m_deserializer->GetMessage(time, reject_message);
            }()};
            if (reject_message) {
                // Message deserialization failed.  Drop the message but don't disconnect the peer.
                // store the size of the corrupt message
                mapRecvBytesPerMsgCmd.at(NET_MESSAGE_COMMAND_OTHER) += msg.m_raw_message_size;
                continue;
            }

            {
                PERF_MONITOR("net_receive_msg_bytes_queue");
                if (use_peer_stats_snapshots && (msg.m_type == NetMsgType::INV || msg.m_type == NetMsgType::BLOCKTXN || msg.m_type == NetMsgType::TX) && !nRecvBytesSnapOld) { // Fine as long this happens within 300 seconds of connection
                    nRecvBytesSnap = nRecvBytes - msg.m_raw_message_size - msg_bytes.size();
                    nRecvBytesSnapOld = nRecvBytesSnap - 1; // -1 to avoid divide by zero
                    {
                        LOCK(cs_vSend);
                        nSendBytesSnap = nSendBytes;
                    }
                    nSendBytesSnapOld = nSendBytesSnap > 0 ? nSendBytesSnap - 1 : 0; // -1 to avoid divide by zero
                    nTimeSnap = count_seconds(m_last_recv);
                    nTimeSnapOld = nTimeSnap - 1; // -1 to avoid divide by zero
                    LogPrintf("%s: 1stTx %s t=%d size=%d nRB1TX=%d nRB=%d handled=%d msg_bytes=%d peer=%d\n", __func__, msg.m_type, nTimeSnap - count_seconds(m_connected), msg.m_raw_message_size, nRecvBytesSnap, nRecvBytes, handled, msg_bytes.size(), GetId());
                }

                // REBTODO - best place to do this?
                if (use_peer_stats_snapshots && (count_seconds(m_last_recv) - nTimeSnap) >= 300) {
                    nTimeSnapOld = nTimeSnap;
                    nTimeSnap = count_seconds(m_last_recv);
                    nMempoolBytesSnapOld = nMempoolBytesSnap;
                    nMempoolBytesSnap = nMempoolBytes;
                    nMempoolTXsSnapOld = nMempoolTXsSnap;
                    nMempoolTXsSnap = nMempoolTXs;
                    nRecvBytesSnapOld = nRecvBytesSnap;
                    nRecvBytesSnap = nRecvBytes;
                    {
                        LOCK(cs_vSend);
                        nSendBytesSnapOld = nSendBytesSnap;
                        nSendBytesSnap = nSendBytes;
                    }
                }

                if (msg.m_type == NetMsgType::BLOCK || msg.m_type == NetMsgType::BLOCKTXN) {
                    nBlocksToBeProcessed++;
                    if (nBlocksToBeProcessed == 1)
                        LogPrintf("%s: BlockToBeProcessed peer=%d\n", __func__, GetId());
                    ::nBlocksToBeProcessed++;
					if (msg.m_type == NetMsgType::BLOCK)
                        nLastBlock = count_seconds(m_last_recv);
                }

                // Store received bytes per message command
                // to prevent a memory DOS, only allow valid commands
                auto i = mapRecvBytesPerMsgCmd.find(msg.m_type);
                if (i == mapRecvBytesPerMsgCmd.end()) {
                    i = mapRecvBytesPerMsgCmd.find(NET_MESSAGE_COMMAND_OTHER);
                }
                assert(i != mapRecvBytesPerMsgCmd.end());
                i->second += msg.m_raw_message_size;

                // push the message to the process queue,
                vRecvMsg.push_back(std::move(msg));

                complete = true;
            }
        }
    }

    return true;
}

int V1TransportDeserializer::readHeader(Span<const uint8_t> msg_bytes)
{
    // copy data to temporary parsing buffer
    unsigned int nRemaining = CMessageHeader::HEADER_SIZE - nHdrPos;
    unsigned int nCopy = std::min<unsigned int>(nRemaining, msg_bytes.size());

    memcpy(&hdrbuf[nHdrPos], msg_bytes.data(), nCopy);
    nHdrPos += nCopy;

    // if header incomplete, exit
    if (nHdrPos < CMessageHeader::HEADER_SIZE)
        return nCopy;

    // deserialize to CMessageHeader
    try {
        hdrbuf >> hdr;
    }
    catch (const std::exception&) {
        LogPrint(BCLog::NET, "Header error: Unable to deserialize, peer=%d\n", m_node_id);
        return -1;
    }

    // Check start string, network magic
    if (memcmp(hdr.pchMessageStart, m_chain_params.MessageStart(), CMessageHeader::MESSAGE_START_SIZE) != 0) {
        LogPrint(BCLog::NET, "Header error: Wrong MessageStart %s received, peer=%d\n", HexStr(hdr.pchMessageStart), m_node_id);
        return -1;
    }

    // reject messages larger than MAX_SIZE or MAX_PROTOCOL_MESSAGE_LENGTH
    if (hdr.nMessageSize > MAX_SIZE || hdr.nMessageSize > MAX_PROTOCOL_MESSAGE_LENGTH) {
        LogPrint(BCLog::NET, "Header error: Size too large (%s, %u bytes), peer=%d\n", SanitizeString(hdr.GetCommand()), hdr.nMessageSize, m_node_id);
        return -1;
    }

    // switch state to reading message data
    in_data = true;

    return nCopy;
}

int V1TransportDeserializer::readData(Span<const uint8_t> msg_bytes)
{
    unsigned int nRemaining = hdr.nMessageSize - nDataPos;
    unsigned int nCopy = std::min<unsigned int>(nRemaining, msg_bytes.size());

    if (vRecv.size() < nDataPos + nCopy) {
        // Allocate up to 256 KiB ahead, but never more than the total message size.
        vRecv.resize(std::min(hdr.nMessageSize, nDataPos + nCopy + 256 * 1024));
    }

    hasher.Write(msg_bytes.first(nCopy));
    memcpy(&vRecv[nDataPos], msg_bytes.data(), nCopy);
    nDataPos += nCopy;

    return nCopy;
}

const uint256& V1TransportDeserializer::GetMessageHash() const
{
    assert(Complete());
    if (data_hash.IsNull())
        hasher.Finalize(data_hash);
    return data_hash;
}

CNetMessage V1TransportDeserializer::GetMessage(const std::chrono::microseconds time, bool& reject_message)
{
    // Initialize out parameter
    reject_message = false;
    // decompose a single CNetMessage from the TransportDeserializer
    CNetMessage msg(std::move(vRecv));

    // store command string, time, and sizes
    msg.m_type = hdr.GetCommand();
    msg.m_time = time;
    msg.m_message_size = hdr.nMessageSize;
    msg.m_raw_message_size = hdr.nMessageSize + CMessageHeader::HEADER_SIZE;

    uint256 hash = GetMessageHash();

    // We just received a message off the wire, harvest entropy from the time (and the message checksum)
    RandAddEvent(ReadLE32(hash.begin()));

    // Check checksum and header command string
    if (memcmp(hash.begin(), hdr.pchChecksum, CMessageHeader::CHECKSUM_SIZE) != 0) {
        LogPrint(BCLog::NET, "Header error: Wrong checksum (%s, %u bytes), expected %s was %s, peer=%d\n",
                 SanitizeString(msg.m_type), msg.m_message_size,
                 HexStr(Span{hash}.first(CMessageHeader::CHECKSUM_SIZE)),
                 HexStr(hdr.pchChecksum),
                 m_node_id);
        reject_message = true;
    } else if (!hdr.IsCommandValid()) {
        LogPrint(BCLog::NET, "Header error: Invalid message type (%s, %u bytes), peer=%d\n",
                 SanitizeString(hdr.GetCommand()), msg.m_message_size, m_node_id);
        reject_message = true;
    }

    // Always reset the network deserializer (prepare for the next message)
    Reset();
    return msg;
}

void V1TransportSerializer::prepareForTransport(CSerializedNetMsg& msg, std::vector<unsigned char>& header) {
    // create dbl-sha256 checksum
    uint256 hash = Hash(msg.data);

    // create header
    CMessageHeader hdr(Params().MessageStart(), msg.m_type.c_str(), msg.data.size());
    memcpy(hdr.pchChecksum, hash.begin(), CMessageHeader::CHECKSUM_SIZE);

    // serialize header
    header.reserve(CMessageHeader::HEADER_SIZE);
    CVectorWriter{SER_NETWORK, INIT_PROTO_VERSION, header, 0, hdr};
}

size_t CConnman::SocketSendData(CNode& node) const
{
    auto it = node.vSendMsg.begin();
    size_t nSentSize = 0;

    while (it != node.vSendMsg.end()) {
        const auto& data = *it;
        assert(data.size() > node.nSendOffset);
        int nBytes = 0;
        {
            LOCK(node.m_sock_mutex);
            if (!node.m_sock) {
                break;
            }
            nBytes = node.m_sock->Send(reinterpret_cast<const char*>(data.data()) + node.nSendOffset, data.size() - node.nSendOffset, MSG_NOSIGNAL | MSG_DONTWAIT);
        }
        if (nBytes > 0) {
            node.m_last_send = GetTime<std::chrono::seconds>();
            node.nSendBytes += nBytes;
            size_t bytes_to_attribute = static_cast<size_t>(nBytes);
            while (bytes_to_attribute > 0 && !node.m_send_msg_cmd_sizes.empty()) {
                auto& [msg_type, msg_bytes_remaining] = node.m_send_msg_cmd_sizes.front();
                const size_t bytes_for_msg = std::min(bytes_to_attribute, msg_bytes_remaining);
                node.mapSendBytesPerMsgCmd[msg_type] += bytes_for_msg;
                msg_bytes_remaining -= bytes_for_msg;
                bytes_to_attribute -= bytes_for_msg;
                if (msg_bytes_remaining == 0) {
                    node.m_send_msg_cmd_sizes.pop_front();
                }
            }
            if (bytes_to_attribute > 0) {
                node.mapSendBytesPerMsgCmd[NET_MESSAGE_COMMAND_OTHER] += bytes_to_attribute;
            }
            node.nSendOffset += nBytes;
            nSentSize += nBytes;
            if (node.nSendOffset == data.size()) {
                node.nSendOffset = 0;
                node.nSendSize -= data.size();
                node.fPauseSend = node.nSendSize > nSendBufferMaxSize;
                it++;
            } else {
                // could not send full message; stop sending more
                break;
            }
        } else {
            if (nBytes < 0) {
                // error
                int nErr = WSAGetLastError();
                if (nErr != WSAEWOULDBLOCK && nErr != WSAEMSGSIZE && nErr != WSAEINTR && nErr != WSAEINPROGRESS) {
                    LogPrintf("%s: socket send error for peer=%d: %s\n", __func__, node.GetId(), NetworkErrorString(nErr));
                    node.CloseSocketDisconnect();
                }
            }
            // couldn't send anything at all
            break;
        }
    }

    if (it == node.vSendMsg.end()) {
        assert(node.nSendOffset == 0);
        assert(node.nSendSize == 0);
    }
    node.vSendMsg.erase(node.vSendMsg.begin(), it);
    return nSentSize;
}

static bool ReverseCompareNodeMinPingTime(const NodeEvictionCandidate &a, const NodeEvictionCandidate &b)
{
    return a.m_min_ping_time > b.m_min_ping_time;
}

static bool ReverseCompareNodeTimeConnected(const NodeEvictionCandidate &a, const NodeEvictionCandidate &b)
{
    return a.m_connected > b.m_connected;
}

static bool CompareNetGroupKeyed(const NodeEvictionCandidate &a, const NodeEvictionCandidate &b) {
    return a.nKeyedNetGroup < b.nKeyedNetGroup;
}

static bool CompareNodeBlockTime(const NodeEvictionCandidate &a, const NodeEvictionCandidate &b)
{
    // There is a fall-through here because it is common for a node to have many peers which have not yet relayed a block.
    if (a.m_last_block_time != b.m_last_block_time) return a.m_last_block_time < b.m_last_block_time;
    if (a.fRelevantServices != b.fRelevantServices) return b.fRelevantServices;
    return a.m_connected > b.m_connected;
}

static bool CompareNodeTXTime(const NodeEvictionCandidate &a, const NodeEvictionCandidate &b)
{
    // There is a fall-through here because it is common for a node to have more than a few peers that have not yet relayed txn.
    if (a.m_last_tx_time != b.m_last_tx_time) return a.m_last_tx_time < b.m_last_tx_time;
    if (a.fRelayTxes != b.fRelayTxes) return b.fRelayTxes;
    if (a.fBloomFilter != b.fBloomFilter) return a.fBloomFilter;
    return a.m_connected > b.m_connected;
}

// Pick out the potential block-relay only peers, and sort them by last block time.
static bool CompareNodeBlockRelayOnlyTime(const NodeEvictionCandidate &a, const NodeEvictionCandidate &b)
{
    if (a.fRelayTxes != b.fRelayTxes) return a.fRelayTxes;
    if (a.m_last_block_time != b.m_last_block_time) return a.m_last_block_time < b.m_last_block_time;
    if (a.fRelevantServices != b.fRelevantServices) return b.fRelevantServices;
    return a.m_connected > b.m_connected;
}

/**
 * Sort eviction candidates by network/localhost and connection uptime.
 * Candidates near the beginning are more likely to be evicted, and those
 * near the end are more likely to be protected, e.g. less likely to be evicted.
 * - First, nodes that are not `is_local` and that do not belong to `network`,
 *   sorted by increasing uptime (from most recently connected to connected longer).
 * - Then, nodes that are `is_local` or belong to `network`, sorted by increasing uptime.
 */
struct CompareNodeNetworkTime {
    const bool m_is_local;
    const Network m_network;
    CompareNodeNetworkTime(bool is_local, Network network) : m_is_local(is_local), m_network(network) {}
    bool operator()(const NodeEvictionCandidate& a, const NodeEvictionCandidate& b) const
    {
        if (m_is_local && a.m_is_local != b.m_is_local) return b.m_is_local;
        if ((a.m_network == m_network) != (b.m_network == m_network)) return b.m_network == m_network;
        return a.m_connected > b.m_connected;
    };
};

//! Sort an array by the specified comparator, then erase the last K elements where predicate is true.
template <typename T, typename Comparator>
static void EraseLastKElements(
    std::vector<T>& elements, Comparator comparator, size_t k,
    std::function<bool(const NodeEvictionCandidate&)> predicate = [](const NodeEvictionCandidate& n) { return true; })
{
    std::sort(elements.begin(), elements.end(), comparator);
    size_t eraseSize = std::min(k, elements.size());
    elements.erase(std::remove_if(elements.end() - eraseSize, elements.end(), predicate), elements.end());
}

void ProtectEvictionCandidatesByRatio(std::vector<NodeEvictionCandidate>& eviction_candidates)
{
    // Protect the half of the remaining nodes which have been connected the longest.
    // This replicates the non-eviction implicit behavior, and precludes attacks that start later.
    // To favorise the diversity of our peer connections, reserve up to half of these protected
    // spots for Tor/onion, localhost and I2P peers, even if they're not longest uptime overall.
    // This helps protect these higher-latency peers that tend to be otherwise
    // disadvantaged under our eviction criteria.
    const size_t initial_size = eviction_candidates.size();
    const size_t total_protect_size{initial_size / 2};

    // Disadvantaged networks to protect: I2P, localhost, Tor/onion. In case of equal counts, earlier
    // array members have first opportunity to recover unused slots from the previous iteration.
    struct Net { bool is_local; Network id; size_t count; };
    std::array<Net, 3> networks{
        {{false, NET_I2P, 0}, {/* localhost */ true, NET_MAX, 0}, {false, NET_ONION, 0}}};

    // Count and store the number of eviction candidates per network.
    for (Net& n : networks) {
        n.count = std::count_if(eviction_candidates.cbegin(), eviction_candidates.cend(),
                                [&n](const NodeEvictionCandidate& c) {
                                    return n.is_local ? c.m_is_local : c.m_network == n.id;
                                });
    }
    // Sort `networks` by ascending candidate count, to give networks having fewer candidates
    // the first opportunity to recover unused protected slots from the previous iteration.
    std::stable_sort(networks.begin(), networks.end(), [](Net a, Net b) { return a.count < b.count; });

    // Protect up to 25% of the eviction candidates by disadvantaged network.
    const size_t max_protect_by_network{total_protect_size / 2};
    size_t num_protected{0};

    while (num_protected < max_protect_by_network) {
        // Count the number of disadvantaged networks from which we have peers to protect.
        auto num_networks = std::count_if(networks.begin(), networks.end(), [](const Net& n) { return n.count; });
        if (num_networks == 0) {
            break;
        }
        const size_t disadvantaged_to_protect{max_protect_by_network - num_protected};
        const size_t protect_per_network{std::max(disadvantaged_to_protect / num_networks, static_cast<size_t>(1))};
        // Early exit flag if there are no remaining candidates by disadvantaged network.
        bool protected_at_least_one{false};

        for (Net& n : networks) {
            if (n.count == 0) continue;
            const size_t before = eviction_candidates.size();
            EraseLastKElements(eviction_candidates, CompareNodeNetworkTime(n.is_local, n.id),
                               protect_per_network, [&n](const NodeEvictionCandidate& c) {
                                   return n.is_local ? c.m_is_local : c.m_network == n.id;
                               });
            const size_t after = eviction_candidates.size();
            if (before > after) {
                protected_at_least_one = true;
                const size_t delta{before - after};
                num_protected += delta;
                if (num_protected >= max_protect_by_network) {
                    break;
                }
                n.count -= delta;
            }
        }
        if (!protected_at_least_one) {
            break;
        }
    }

    // Calculate how many we removed, and update our total number of peers that
    // we want to protect based on uptime accordingly.
    assert(num_protected == initial_size - eviction_candidates.size());
    const size_t remaining_to_protect{total_protect_size - num_protected};
    EraseLastKElements(eviction_candidates, ReverseCompareNodeTimeConnected, remaining_to_protect);
}

[[nodiscard]] std::optional<NodeId> SelectNodeToEvict(std::vector<NodeEvictionCandidate>&& vEvictionCandidates)
{
    // Protect connections with certain characteristics

    // Deterministically select 4 peers to protect by netgroup.
    // An attacker cannot predict which netgroups will be protected
    EraseLastKElements(vEvictionCandidates, CompareNetGroupKeyed, 4);
    // Protect the 8 nodes with the lowest minimum ping time.
    // An attacker cannot manipulate this metric without physically moving nodes closer to the target.
    EraseLastKElements(vEvictionCandidates, ReverseCompareNodeMinPingTime, 8);
    // Protect 4 nodes that most recently sent us novel transactions accepted into our mempool.
    // An attacker cannot manipulate this metric without performing useful work.
    EraseLastKElements(vEvictionCandidates, CompareNodeTXTime, 4);
    // Protect up to 8 non-tx-relay peers that have sent us novel blocks.
    EraseLastKElements(vEvictionCandidates, CompareNodeBlockRelayOnlyTime, 8,
                       [](const NodeEvictionCandidate& n) { return !n.fRelayTxes && n.fRelevantServices; });

    // Protect 4 nodes that most recently sent us novel blocks.
    // An attacker cannot manipulate this metric without performing useful work.
    EraseLastKElements(vEvictionCandidates, CompareNodeBlockTime, 4);

    // Protect some of the remaining eviction candidates by ratios of desirable
    // or disadvantaged characteristics.
    ProtectEvictionCandidatesByRatio(vEvictionCandidates);

    if (vEvictionCandidates.empty()) return std::nullopt;

    // If any remaining peers are preferred for eviction consider only them.
    // This happens after the other preferences since if a peer is really the best by other criteria (esp relaying blocks)
    //  then we probably don't want to evict it no matter what.
    if (std::any_of(vEvictionCandidates.begin(),vEvictionCandidates.end(),[](NodeEvictionCandidate const &n){return n.prefer_evict;})) {
        vEvictionCandidates.erase(std::remove_if(vEvictionCandidates.begin(),vEvictionCandidates.end(),
                                  [](NodeEvictionCandidate const &n){return !n.prefer_evict;}),vEvictionCandidates.end());
    }

    // Identify the network group with the most connections and youngest member.
    // (vEvictionCandidates is already sorted by reverse connect time)
    uint64_t naMostConnections;
    unsigned int nMostConnections = 0;
    std::chrono::seconds nMostConnectionsTime{0};
    std::map<uint64_t, std::vector<NodeEvictionCandidate> > mapNetGroupNodes;
    for (const NodeEvictionCandidate &node : vEvictionCandidates) {
        std::vector<NodeEvictionCandidate> &group = mapNetGroupNodes[node.nKeyedNetGroup];
        group.push_back(node);
        const auto grouptime{group[0].m_connected};

        if (group.size() > nMostConnections || (group.size() == nMostConnections && grouptime > nMostConnectionsTime)) {
            nMostConnections = group.size();
            nMostConnectionsTime = grouptime;
            naMostConnections = node.nKeyedNetGroup;
        }
    }

    // Reduce to the network group with the most connections
    vEvictionCandidates = std::move(mapNetGroupNodes[naMostConnections]);

    // Disconnect from the network group with the most connections
    return vEvictionCandidates.front().id;
}

/** Try to find a connection to evict when the node is full.
 *  Extreme care must be taken to avoid opening the node to attacker
 *   triggered network partitioning.
 *  The strategy used here is to protect a small number of peers
 *   for each of several distinct characteristics which are difficult
 *   to forge.  In order to partition a node the attacker must be
 *   simultaneously better at all of them than honest peers.
 */
bool CConnman::AttemptToEvictConnection()
{
    std::vector<NodeEvictionCandidate> vEvictionCandidates;
    {

        LOCK(m_nodes_mutex);
        for (const CNode* node : m_nodes) {
            if (node->HasPermission(NetPermissionFlags::NoBan))
                continue;
            if (!node->IsInboundConn())
                continue;
            if (node->fDisconnect)
                continue;
            bool peer_relay_txes = false;
            bool peer_filter_not_null = false;
            if (node->m_tx_relay != nullptr) {
                LOCK(node->m_tx_relay->cs_filter);
                peer_relay_txes = node->m_tx_relay->fRelayTxes;
                peer_filter_not_null = node->m_tx_relay->pfilter != nullptr;
            }
            NodeEvictionCandidate candidate = {node->GetId(), node->m_connected, node->m_min_ping_time,
                                               node->m_last_block_time, node->m_last_tx_time,
                                               HasAllDesirableServiceFlags(node->nServices),
                                               peer_relay_txes, peer_filter_not_null, node->nKeyedNetGroup,
                                               node->m_prefer_evict, node->addr.IsLocal(),
                                               node->ConnectedThroughNetwork()};
            vEvictionCandidates.push_back(candidate);
        }
    }
    const std::optional<NodeId> node_id_to_evict = SelectNodeToEvict(std::move(vEvictionCandidates));
    if (!node_id_to_evict) {
        return false;
    }
    LOCK(m_nodes_mutex);
    for (CNode* pnode : m_nodes) {
        if (pnode->GetId() == *node_id_to_evict) {
            LogPrintf("selected %s connection for eviction peer=%d; disconnecting\n", pnode->ConnectionTypeAsString(), pnode->GetId());
            pnode->fDisconnect = true;
            return true;
        }
    }
    return false;
}

void CConnman::AcceptConnection(const ListenSocket& hListenSocket) {
    struct sockaddr_storage sockaddr;
    socklen_t len = sizeof(sockaddr);
    auto sock = hListenSocket.sock->Accept((struct sockaddr*)&sockaddr, &len);
    CAddress addr;

    if (!sock) {
        const int nErr = WSAGetLastError();
        if (nErr != WSAEWOULDBLOCK) {
            LogPrintf("socket error accept failed: %s\n", NetworkErrorString(nErr));
        }
        return;
    }

    if (!addr.SetSockAddr((const struct sockaddr*)&sockaddr)) {
        LogPrintf("Warning: Unknown socket family\n");
    } else {
        addr = CAddress{MaybeFlipIPv6toCJDNS(addr), NODE_NONE};
    }

    const CAddress addr_bind{MaybeFlipIPv6toCJDNS(GetBindAddress(sock->Get())), NODE_NONE};

    NetPermissionFlags permissionFlags = NetPermissionFlags::None;
    hListenSocket.AddSocketPermissionFlags(permissionFlags);

    CreateNodeFromAcceptedSocket(std::move(sock), permissionFlags, addr_bind, addr);
}

void CConnman::CreateNodeFromAcceptedSocket(std::unique_ptr<Sock>&& sock,
                                            NetPermissionFlags permissionFlags,
                                            const CAddress& addr_bind, const CAddress& addr)
{
    int nInbound = 0, nMaxInbound = nMaxConnections - m_max_outbound;

    AddWhitelistPermissionFlags(permissionFlags, addr);
    if (NetPermissions::HasFlag(permissionFlags, NetPermissionFlags::Implicit)) {
        NetPermissions::ClearFlag(permissionFlags, NetPermissionFlags::Implicit);
        if (gArgs.GetBoolArg("-whitelistforcerelay", DEFAULT_WHITELISTFORCERELAY)) NetPermissions::AddFlag(permissionFlags, NetPermissionFlags::ForceRelay);
        if (gArgs.GetBoolArg("-whitelistrelay", DEFAULT_WHITELISTRELAY)) NetPermissions::AddFlag(permissionFlags, NetPermissionFlags::Relay);
        NetPermissions::AddFlag(permissionFlags, NetPermissionFlags::Mempool);
        NetPermissions::AddFlag(permissionFlags, NetPermissionFlags::NoBan);
    }

    {
        LOCK(m_nodes_mutex);
        for (const CNode* pnode : m_nodes) {
            if (pnode->IsInboundConn()) nInbound++;
        }
    }

    if (!fNetworkActive) {
        LogPrint(BCLog::NET, "connection from %s dropped: not accepting new connections\n", addr.ToString());
        return;
    }

    if (!IsSelectableSocket(sock->Get()))
    {
        LogPrintf("connection from %s dropped: non-selectable socket\n", addr.ToString());
        return;
    }

    // According to the internet TCP_NODELAY is not carried into accepted sockets
    // on all platforms.  Set it again here just to be sure.
    SetSocketNoDelay(sock->Get());

    // Don't accept connections from banned peers.
    bool banned = m_banman && m_banman->IsBanned(addr);
    bool on_probation = m_banman && m_banman->IsOnProbation(addr);
    if (!NetPermissions::HasFlag(permissionFlags, NetPermissionFlags::NoBan) && banned)
    {
        LogPrint(BCLog::BANMAN, "connection from %s dropped (banned)\n", addr.ToString());
        return;
    }

    // Only accept connections from discouraged peers if our inbound slots aren't (almost) full.
    bool discouraged = m_banman && m_banman->IsDiscouraged(addr);
    if (!NetPermissions::HasFlag(permissionFlags, NetPermissionFlags::NoBan) && nInbound + 1 >= nMaxInbound && discouraged)
    {
        LogPrint(BCLog::NET, "connection from %s dropped (discouraged)\n", addr.ToString());
        return;
    }

    if (nInbound >= nMaxInbound)
    {
        if (!AttemptToEvictConnection()) {
            // No connection to evict, disconnect the new connection
            LogPrint(BCLog::NET, "failed to find an eviction candidate - connection dropped (full)\n");
            return;
        }
    }

    NodeId id = GetNewNodeId();
    uint64_t nonce = GetDeterministicRandomizer(RANDOMIZER_ID_LOCALHOSTNONCE).Write(id).Finalize();

    ServiceFlags nodeServices = nLocalServices;
    if (NetPermissions::HasFlag(permissionFlags, NetPermissionFlags::BloomFilter))
        nodeServices = static_cast<ServiceFlags>(nodeServices | NODE_BLOOM);

    const bool inbound_onion = std::find(m_onion_binds.begin(), m_onion_binds.end(), addr_bind) != m_onion_binds.end();
    CNode* pnode = new CNode(id, nodeServices, std::move(sock), addr,
           CalculateKeyedNetGroup(addr),
           nonce, addr_bind, /*addrNameIn=*/"", ConnectionType::INBOUND, inbound_onion);

    // Log the onion address for Tor inbound connections
    if (inbound_onion) {
        TorController* torController = GetTorController();
        if (torController) {
            const std::vector<CService>& onionServices = torController->GetOnionServices();
            for (size_t i = 0; i < onionServices.size(); ++i) {
                // Compare the port of addr_bind with the onion service's local port
                // Note: onionServices[i].GetPort() is the external port (e.g., 8333),
                // but we need to match against the internal mapped port if available
                if (onionServices[i].GetPort() == addr_bind.GetPort()) {
                    LogPrint(BCLog::NET, "Incoming Tor connection bound to onion address %s (service index %zu), local bind %s\n", onionServices[i].ToString(), i, addr_bind.ToString());
                    break;
                }
            }
        }
    }

    pnode->AddRef(1); // REB - Creation (in)
    pnode->m_permissionFlags = permissionFlags;
    pnode->m_prefer_evict = discouraged || on_probation;
    m_msgproc->InitializeNode(pnode);

    LogPrint(BCLog::NET, "connection from %s accepted\n", addr.ToString());

    {
        LOCK(m_nodes_mutex);
        m_nodes.push_back(pnode);
    }

    // We received a new connection, harvest entropy from the time (and our peer count)
    RandAddEvent((uint32_t)id);
}

bool CConnman::AddConnection(const std::string& address, ConnectionType conn_type)
{
    std::optional<int> max_connections;
    switch (conn_type) {
    case ConnectionType::INBOUND:
    case ConnectionType::MANUAL:
        return false;
    case ConnectionType::OUTBOUND_FULL_RELAY:
        break;
    case ConnectionType::BLOCK_RELAY:
        max_connections = GetTargetOutboundBlockRelay();
        break;
    // no limit for ADDR_FETCH because -seednode has no limit either
    case ConnectionType::ADDR_FETCH:
        break;
    // no limit for FEELER connections since they're short-lived
    case ConnectionType::FEELER:
        break;
    } // no default case, so the compiler can warn about missing cases

    // Count existing connections
    int existing_connections = WITH_LOCK(m_nodes_mutex,
                                         return std::count_if(m_nodes.begin(), m_nodes.end(), [conn_type](CNode* node) { return node->m_conn_type == conn_type; }););

    // Max connections of specified type already exist
    if (max_connections != std::nullopt && existing_connections >= max_connections) return false;

    // Max total outbound connections already exist
    CSemaphoreGrant grant(*semOutbound, true);
    if (!grant) return false;

    OpenNetworkConnection(CAddress(), false, &grant, address.c_str(), conn_type);
    return true;
}

void CConnman::DisconnectNodes()
{
    {
        LOCK(m_nodes_mutex);

        if (!fNetworkActive) {
            // Disconnect any connected nodes
            for (CNode* pnode : m_nodes) {
                if (!pnode->fDisconnect) {
                    LogPrint(BCLog::NET, "Network not active, dropping peer=%d\n", pnode->GetId());
                    pnode->fDisconnect = true;
                }
            }
        }

        // Disconnect unused nodes
        std::vector<CNode*> nodes_copy = m_nodes;
        for (CNode* pnode : nodes_copy)
        {
            if (pnode->fDisconnect && pnode->nBlocksToBeProcessed < 1)
            {
                // remove from m_nodes
                int m_nodesSizeBefore = m_nodes.size();
                m_nodes.erase(remove(m_nodes.begin(), m_nodes.end(), pnode), m_nodes.end());
                int m_nodesSizeAfter = m_nodes.size();

                // release outbound grant (if any)
                pnode->grantOutbound.Release();

                // close socket and cleanup
                pnode->CloseSocketDisconnect();

                // hold in disconnected pool until all refs are released
                LogPrint(BCLog::CONN, "%s: Add to m_nodes_disconnected m_nodes.size %d->%d GRC=%d %speer=%d\n", __func__, m_nodesSizeBefore, m_nodesSizeAfter, pnode->GetRefCount(), pnode->IsFeelerConn() ? "feel " : pnode->IsInboundConn() ? "incoming ":"", pnode->GetId());
                pnode->Release(1); // REB - deletion
                m_nodes_disconnected.push_back(pnode);
            } else if (pnode->fDisconnect && pnode->fSuccessfullyConnected) {
                LogPrint(BCLog::CONN, "%s: fDisconnect but %d blocks still to process. peer=%d\n", __func__,
                    pnode->nBlocksToBeProcessed, pnode->id);
                pnode->fSuccessfullyConnected = false; // Allow space for new nodes to be connected
                pnode->CloseSocketDisconnect();
            }
        }
    }
    std::list<CNode*> nodes_disconnected_copy = m_nodes_disconnected;
    {
        // Delete disconnected nodes
        for (CNode* pnode : nodes_disconnected_copy)
        {
            // Destroy the object only after other threads have stopped using it.
            if (pnode->GetRefCount() <= 0) {
                m_nodes_disconnected.remove(pnode);
                LogPrint(BCLog::CONN, "%s: Calling DeleteNode GRC=%d from m_nodes_disconnected loop. peer=%d\n", __func__, pnode->GetRefCount(), pnode->GetId());
                DeleteNode(pnode);
            }
        }
    }
    LOCK(m_nodes_mutex);
    if (m_nodes.size() == 0 && nodes_disconnected_copy.size() > 0 && m_nodes_disconnected.size() == 0) {
        LogPrintf("NO PEERS CONNECTED. Resetting NodeId\n");
        nAnchorTryAgain = 0;
        ResetNewNodeId();
    }
}

void CConnman::NotifyNumConnectionsChanged()
{
    size_t nodes_size;
    {
        LOCK(m_nodes_mutex);
        nodes_size = m_nodes.size();
    }
    if(nodes_size != nPrevNodeCount) {
        nPrevNodeCount = nodes_size;
        if (m_client_interface) {
            m_client_interface->NotifyNumConnectionsChanged(nodes_size);
        }
    }
}

bool CConnman::ShouldRunInactivityChecks(const CNode& node, std::chrono::seconds now) const
{
    return node.m_connected + m_peer_connect_timeout < now;
}

bool CConnman::InactivityCheck(const CNode& node) const
{
    // Tests that see disconnects after using mocktime can start nodes with a
    // large timeout. For example, -peertimeout=999999999.
    const auto now{GetTime<std::chrono::seconds>()};
    const auto last_send{node.m_last_send.load()};
    const auto last_recv{node.m_last_recv.load()};

    if (!ShouldRunInactivityChecks(node, now)) return false;

    if (last_recv.count() == 0 || last_send.count() == 0) {
        LogPrintf("socket no message in first %i seconds, %d %d disconnect peer=%d\n", count_seconds(m_peer_connect_timeout), last_recv.count() != 0, last_send.count() != 0, node.GetId());
        return true;
    }

    if (now > last_send + TIMEOUT_INTERVAL) {
        LogPrintf("socket sending timeout: %s disconnect peer=%d\n", strAge(count_seconds(now - last_send)), node.GetId());
        return true;
    }

    if (now > last_recv + TIMEOUT_INTERVAL) {
        LogPrintf("socket receive timeout: %s disconnect peer=%d\n", strAge(count_seconds(now - last_recv)), node.GetId());
        return true;
    }

    if (!node.fSuccessfullyConnected) {
        LogPrintf("version handshake timeout disconnect peer=%d\n", node.GetId());
        return true;
    }

    return false;
}

bool CConnman::GenerateSelectSet(const std::vector<CNode*>& nodes,
                                 std::set<SOCKET>& recv_set,
                                 std::set<SOCKET>& send_set,
                                 std::set<SOCKET>& error_set)
{
    for (const ListenSocket& hListenSocket : vhListenSocket) {
        recv_set.insert(hListenSocket.sock->Get());
    }

    for (CNode* pnode : nodes) {
        // Implement the following logic:
        // * If there is data to send, select() for sending data. As this only
        //   happens when optimistic write failed, we choose to first drain the
        //   write buffer in this case before receiving more. This avoids
        //   needlessly queueing received data, if the remote peer is not themselves
        //   receiving data. This means properly utilizing TCP flow control signalling.
        // * Otherwise, if there is space left in the receive buffer, select() for
        //   receiving data.
        // * Hand off all complete messages to the processor, to be handled without
        //   blocking here.

        bool select_recv = !pnode->fPauseRecv;
        bool select_send;
        {
            LOCK(pnode->cs_vSend);
            select_send = !pnode->vSendMsg.empty();
        }

        LOCK(pnode->m_sock_mutex);
        if (!pnode->m_sock) {
            continue;
        }

        error_set.insert(pnode->m_sock->Get());
        if (select_send) {
            send_set.insert(pnode->m_sock->Get());
            continue;
        }
        if (select_recv) {
            recv_set.insert(pnode->m_sock->Get());
        }
    }

    return !recv_set.empty() || !send_set.empty() || !error_set.empty();
}

#ifdef USE_POLL
void CConnman::SocketEvents(const std::vector<CNode*>& nodes,
                            std::set<SOCKET>& recv_set,
                            std::set<SOCKET>& send_set,
                            std::set<SOCKET>& error_set)
{
    std::set<SOCKET> recv_select_set, send_select_set, error_select_set;
    if (!GenerateSelectSet(nodes, recv_select_set, send_select_set, error_select_set)) {
        interruptNet.sleep_for(std::chrono::milliseconds(SELECT_TIMEOUT_MILLISECONDS));
        return;
    }

    std::unordered_map<SOCKET, struct pollfd> pollfds;
    for (SOCKET socket_id : recv_select_set) {
        pollfds[socket_id].fd = socket_id;
        pollfds[socket_id].events |= POLLIN;
    }

    for (SOCKET socket_id : send_select_set) {
        pollfds[socket_id].fd = socket_id;
        pollfds[socket_id].events |= POLLOUT;
    }

    for (SOCKET socket_id : error_select_set) {
        pollfds[socket_id].fd = socket_id;
        // These flags are ignored, but we set them for clarity
        pollfds[socket_id].events |= POLLERR|POLLHUP;
    }

    std::vector<struct pollfd> vpollfds;
    vpollfds.reserve(pollfds.size());
    for (auto it : pollfds) {
        vpollfds.push_back(std::move(it.second));
    }

    if (poll(vpollfds.data(), vpollfds.size(), SELECT_TIMEOUT_MILLISECONDS) < 0) return;

    if (interruptNet) return;

    for (struct pollfd pollfd_entry : vpollfds) {
        if (pollfd_entry.revents & POLLIN)            recv_set.insert(pollfd_entry.fd);
        if (pollfd_entry.revents & POLLOUT)           send_set.insert(pollfd_entry.fd);
        if (pollfd_entry.revents & (POLLERR|POLLHUP)) error_set.insert(pollfd_entry.fd);
    }
}
#else
void CConnman::SocketEvents(const std::vector<CNode*>& nodes,
                            std::set<SOCKET>& recv_set,
                            std::set<SOCKET>& send_set,
                            std::set<SOCKET>& error_set)
{
    std::set<SOCKET> recv_select_set, send_select_set, error_select_set;
    if (!GenerateSelectSet(nodes, recv_select_set, send_select_set, error_select_set)) {
        interruptNet.sleep_for(std::chrono::milliseconds(SELECT_TIMEOUT_MILLISECONDS));
        return;
    }

    //
    // Find which sockets have data to receive
    //
    struct timeval timeout;
    timeout.tv_sec  = 0;
    timeout.tv_usec = SELECT_TIMEOUT_MILLISECONDS * 1000; // frequency to poll pnode->vSend

    fd_set fdsetRecv;
    fd_set fdsetSend;
    fd_set fdsetError;
    FD_ZERO(&fdsetRecv);
    FD_ZERO(&fdsetSend);
    FD_ZERO(&fdsetError);
    SOCKET hSocketMax = 0;

    for (SOCKET hSocket : recv_select_set) {
        FD_SET(hSocket, &fdsetRecv);
        hSocketMax = std::max(hSocketMax, hSocket);
    }

    for (SOCKET hSocket : send_select_set) {
        FD_SET(hSocket, &fdsetSend);
        hSocketMax = std::max(hSocketMax, hSocket);
    }

    for (SOCKET hSocket : error_select_set) {
        FD_SET(hSocket, &fdsetError);
        hSocketMax = std::max(hSocketMax, hSocket);
    }

    int nSelect = select(hSocketMax + 1, &fdsetRecv, &fdsetSend, &fdsetError, &timeout);

    if (interruptNet)
        return;

    if (nSelect == SOCKET_ERROR)
    {
        int nErr = WSAGetLastError();
        LogPrintf("socket select error %s\n", NetworkErrorString(nErr));
        for (unsigned int i = 0; i <= hSocketMax; i++)
            FD_SET(i, &fdsetRecv);
        FD_ZERO(&fdsetSend);
        FD_ZERO(&fdsetError);
        if (!interruptNet.sleep_for(std::chrono::milliseconds(SELECT_TIMEOUT_MILLISECONDS)))
            return;
    }

    for (SOCKET hSocket : recv_select_set) {
        if (FD_ISSET(hSocket, &fdsetRecv)) {
            recv_set.insert(hSocket);
        }
    }

    for (SOCKET hSocket : send_select_set) {
        if (FD_ISSET(hSocket, &fdsetSend)) {
            send_set.insert(hSocket);
        }
    }

    for (SOCKET hSocket : error_select_set) {
        if (FD_ISSET(hSocket, &fdsetError)) {
            error_set.insert(hSocket);
        }
    }
}
#endif

void CConnman::SocketHandler()
{
    PERF_MONITOR("net_socket_handler");
    std::set<SOCKET> recv_set;
    std::set<SOCKET> send_set;
    std::set<SOCKET> error_set;

    {
        const NodesSnapshot snap{*this, 2, /*shuffle=*/false};

        // Check for the readiness of the already connected sockets and the
        // listening sockets in one call ("readiness" as in poll(2) or
        // select(2)). If none are ready, wait for a short while and return
        // empty sets.
        SocketEvents(snap.Nodes(), recv_set, send_set, error_set);

        // Service (send/receive) each of the already connected nodes.
        SocketHandlerConnected(snap.Nodes(), recv_set, send_set, error_set);
    }

    // Accept new connections from listening sockets.
    SocketHandlerListening(recv_set);
}

void CConnman::SocketHandlerConnected(const std::vector<CNode*>& nodes,
                                      const std::set<SOCKET>& recv_set,
                                      const std::set<SOCKET>& send_set,
                                      const std::set<SOCKET>& error_set)
{
    int64_t latestOutboundConn = 0;
    int64_t latestSnapOld = 0;
    uint64_t nTotalBytesRecv = 0;
    uint64_t nTotalMempoolBytes = 0;
    int nOutboundFullRelay = 0;
    int nOutboundBlockRelay = 0;
    double nLowestScore{std::numeric_limits<double>::infinity()};
    double nSecondLowestScore{std::numeric_limits<double>::infinity()};
    double nLowestEffectiveTXpm{0.0};
    double nLatestNodeScore{0.0};
    std::vector<std::pair<NodeId, std::pair<double, double>>> outbound_peer_metrics;
    NodeId latestNode = -1;
    NodeId worstNode = -1;
    static NodeId lastWorst = -1;
    float nGlobalTXpm = 0;
    float nGlobalBps = 0;
    int64_t now = GetTimeSeconds();
    static int64_t tWorstChanged = now;
    static int64_t tIBDEnded = now;
    static int64_t m_last_block_time = 0;
    static int64_t lastnow = 0;
    int nPeersIBD = 0;
    static bool IsIBD = true;
    static std::unordered_map<NodeId, std::deque<std::pair<int64_t, uint64_t>>> ibd_block_samples;
    static std::unordered_map<NodeId, int64_t> ibd_slow_since;
    static int64_t ibd_calibration_started{0};
    static int ibd_max_full_outbound{0};
    static int ibd_target_peers{0};
    static int64_t blk_anchor_last_dump{0};
    static std::vector<CAddress> blk_anchor_last_dumped;
    std::unordered_map<NodeId, uint64_t> ibd_block_bytes_now;
    std::unordered_set<NodeId> full_outbound_ids;
    const bool use_peer_stats_snapshots{gArgs.GetBoolArg("-peerstatssnapshots", false)};
    if (now != lastnow) {
        m_max_outbound_full_relay = std::min(nMaxConnections, (int)gArgs.GetIntArg("-maxoutboundrelay", MAX_OUTBOUND_FULL_RELAY_CONNECTIONS));
        m_max_outbound_block_relay = std::clamp<int>(
            gArgs.GetIntArg("-blockrelaypeers", MAX_BLOCK_RELAY_ONLY_CONNECTIONS),
            0, std::max(0, nMaxConnections - m_max_outbound_full_relay));
        m_max_outbound = m_max_outbound_full_relay + GetTargetOutboundBlockRelay() + nMaxFeeler;
        for (CNode* pnode : nodes) {
            uint64_t nRecvBytes;
            {
                LOCK(pnode->cs_vRecv);
                nRecvBytes = pnode->nRecvBytes;
            }
            uint64_t nSendBytes;
            {
                LOCK(pnode->cs_vSend);
                nSendBytes = pnode->nSendBytes;
            }
            const uint64_t mempool_bytes_base{use_peer_stats_snapshots ? pnode->nMempoolBytesSnapOld : 0};
            const unsigned int mempool_txs_base{use_peer_stats_snapshots ? pnode->nMempoolTXsSnapOld : 0};
            const uint64_t send_bytes_base{use_peer_stats_snapshots ? pnode->nSendBytesSnapOld : 0};
            const uint64_t recv_bytes_base{use_peer_stats_snapshots ? pnode->nRecvBytesSnapOld : 0};
            const int64_t interval_start_time{
                (use_peer_stats_snapshots && pnode->nTimeSnapOld > 0)
                    ? pnode->nTimeSnapOld
                    : count_seconds(pnode->m_connected)};
            const uint64_t nMempoolBytes{pnode->nMempoolBytes > mempool_bytes_base ? pnode->nMempoolBytes - mempool_bytes_base : 0};
            const unsigned int nMempoolTXs{pnode->nMempoolTXs > mempool_txs_base ? pnode->nMempoolTXs - mempool_txs_base : 0};
            const uint64_t nRecvBytesInterval{nRecvBytes > recv_bytes_base ? nRecvBytes - recv_bytes_base : 0};
            const uint64_t nSendBytesInterval{nSendBytes > send_bytes_base ? nSendBytes - send_bytes_base : 0};
            if ((pnode->nLastBlock >= now - 60) || (pnode->m_tx_relay && pnode->m_tx_relay->lastSentFeeFilter > 9000000)) nPeersIBD++;
            if (count_seconds(pnode->m_last_block_time) > m_last_block_time) m_last_block_time = count_seconds(pnode->m_last_block_time);
            float nMempoolPct = 100.0 * nMempoolBytes / (nRecvBytesInterval + nSendBytesInterval + 1);
            int64_t m_connected = count_seconds(pnode->m_connected);
            if (pnode->IsFullOutboundConn()) {
                full_outbound_ids.insert(pnode->GetId());
                ibd_block_bytes_now.emplace(pnode->GetId(), pnode->nBlockBytes);
                nTotalBytesRecv += nRecvBytesInterval;
                nTotalMempoolBytes += nMempoolBytes;
                latestNode = pnode->GetId();
                nOutboundFullRelay++;
                if (interval_start_time > latestSnapOld) latestSnapOld = interval_start_time;
                if (m_connected > latestOutboundConn) latestOutboundConn = m_connected;
                //int nMempoolBps = nMempoolPct * .08 * (nRecvBytes - pnode-nRecvBytesSnapOld) / (now - pnode->nTimeSnapOld);
                float nMempoolBps = 0;
                if (now > interval_start_time) nMempoolBps = nMempoolBytes * 8.0 / (now - interval_start_time);
                nGlobalBps += nMempoolBps;
                float nTXpm = 0;
                if (now > interval_start_time) nTXpm = 60.0 * nMempoolTXs / (now - interval_start_time);
                nGlobalTXpm += (int)nTXpm;
                const double nMempoolPctEffective{pnode->nBTxBpsPct > 0 ? pnode->nBTxBpsPct : nMempoolPct};
                const double nTXpmEffective{pnode->nBTXpm > 0 ? pnode->nBTXpm : nTXpm};
                outbound_peer_metrics.emplace_back(pnode->GetId(), std::make_pair(nMempoolPctEffective, nTXpmEffective));
            } else if (pnode->IsInboundConn()) {
                float nRecvBps = 0; float nSendBps = 0;
                if(now > m_connected) {
                    nRecvBps = 8 * (float)nRecvBytes / (now - m_connected);
                    nSendBps = 8 * (float)nSendBytes / (now - m_connected);
                }
                if ((now - m_connected >= 120) && (nMempoolPct < 10) && ((nRecvBps > 120) || (nSendBps > 1200))) {
                    if (!pnode->HasPermission(NetPermissionFlags::NoBan) && !pnode->fDisconnect) {
                        pnode->fDisconnect = 1;
                        if (m_banman) m_banman->Ban(pnode->addr, 60 * 60); // Ban for 1 hour
                        LogPrintf("%s: Pct=%d%% Send=%s Recv=%s TimeConn=%d %s disconnect incoming peer=%d\n", __func__, nMempoolPct, nSendBps, nRecvBps, now - m_connected, pnode->addr.ToString(), pnode->GetId());
                        DisconnectNode(pnode->addr);
                    }
                }
            } else if (pnode->IsBlockOnlyConn()) nOutboundBlockRelay++;
        } // for (CNode* pnode : nodes)

        // Rank block-relay-only peers by observed block download throughput and
        // persist top candidates for future block-focused anchor selection.
        const int block_relay_target = GetTargetOutboundBlockRelay();
        if (m_collect_block_anchors && block_relay_target > 0) {
            std::vector<std::pair<double, CAddress>> ranked_block_relays;
            ranked_block_relays.reserve(nodes.size());
            for (CNode* pnode : nodes) {
                if (!pnode->IsBlockOnlyConn() || pnode->fDisconnect) continue;
                const int64_t connected_since = count_seconds(pnode->m_connected);
                int64_t interval_start = connected_since;
                uint64_t block_bytes = pnode->nBlockBytes;
                if (use_peer_stats_snapshots && pnode->nTimeSnapOld > 0 && pnode->nBlockBytes >= pnode->nBlockBytesSnapOld) {
                    interval_start = pnode->nTimeSnapOld;
                    block_bytes = pnode->nBlockBytes - pnode->nBlockBytesSnapOld;
                }
                if (now <= interval_start) continue;
                const double block_bps = static_cast<double>(block_bytes) / static_cast<double>(now - interval_start);
                if (block_bps <= 0.0) continue;
                ranked_block_relays.emplace_back(block_bps, pnode->addr);
            }

            const bool dump_due = blk_anchor_last_dump == 0 || now - blk_anchor_last_dump >= IBD_ANCHOR_DUMP_INTERVAL;
            if (dump_due && static_cast<int>(ranked_block_relays.size()) >= block_relay_target) {
                std::sort(ranked_block_relays.begin(), ranked_block_relays.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
                std::vector<CAddress> block_anchors_to_dump;
                for (size_t i = 0; i < ranked_block_relays.size() && static_cast<int>(i) < block_relay_target; ++i) {
                    block_anchors_to_dump.push_back(ranked_block_relays[i].second);
                }
                if (!block_anchors_to_dump.empty() && block_anchors_to_dump != blk_anchor_last_dumped) {
                    DumpBlockAnchors(gArgs.GetDataDirNet() / BLOCK_ANCHORS_DATABASE_FILENAME, block_anchors_to_dump);
                    blk_anchor_last_dumped = block_anchors_to_dump;
                }
                blk_anchor_last_dump = now;
            }
        }
        if (nPeersIBD == 0 && IsIBD) {
            tIBDEnded = now;
            latestOutboundConn = now;
            IsIBD = false;
        } else if (nPeersIBD) IsIBD = true;

        if (nPeersIBD > 1) {
            const int max_target_peers = std::max(2, m_max_outbound_full_relay);
            if (ibd_calibration_started == 0) {
                ibd_calibration_started = now;
                ibd_max_full_outbound = nOutboundFullRelay;
                LogPrintf("IBD anchor calibration started. peers=%d nIBD=%d\n", nOutboundFullRelay, nPeersIBD);
            } else {
                ibd_max_full_outbound = std::max(ibd_max_full_outbound, nOutboundFullRelay);
            }

            if (ibd_target_peers == 0 && now - ibd_calibration_started >= IBD_CALIBRATION_WINDOW) {
                ibd_target_peers = std::clamp(ibd_max_full_outbound, 2, max_target_peers);
                LogPrintf("IBD anchor calibration complete. target peers=%d (max observed=%d)\n", ibd_target_peers, ibd_max_full_outbound);
            }

            for (const auto& [node_id, block_bytes] : ibd_block_bytes_now) {
                auto& samples = ibd_block_samples[node_id];
                samples.emplace_back(now, block_bytes);
                while (samples.size() > 2 && samples.front().first < now - IBD_SPEED_MEASUREMENT_WINDOW) {
                    samples.pop_front();
                }
            }

            for (auto it = ibd_block_samples.begin(); it != ibd_block_samples.end();) {
                if (!full_outbound_ids.count(it->first)) {
                    ibd_slow_since.erase(it->first);
                    it = ibd_block_samples.erase(it);
                } else {
                    ++it;
                }
            }

            std::unordered_map<NodeId, double> ibd_peer_bps;
            double fastest_bps{0.0};
            for (const auto& [node_id, samples] : ibd_block_samples) {
                if (samples.size() < 2) continue;
                const int64_t dt = samples.back().first - samples.front().first;
                if (dt < IBD_SPEED_MEASUREMENT_WINDOW) continue;
                const int64_t dbytes = static_cast<int64_t>(samples.back().second) - static_cast<int64_t>(samples.front().second);
                if (dbytes <= 0) continue;
                const double bps = static_cast<double>(dbytes) / static_cast<double>(dt);
                ibd_peer_bps[node_id] = bps;
                fastest_bps = std::max(fastest_bps, bps);
            }

            if (fastest_bps > 0.0) {
                NodeId disconnect_candidate{-1};
                double disconnect_candidate_bps{0.0};

                for (CNode* pnode : nodes) {
                    if (!pnode->IsFullOutboundConn()) continue;
                    if (pnode->HasPermission(NetPermissionFlags::NoBan)) continue;
                    const NodeId node_id = pnode->GetId();
                    const auto speed_it = ibd_peer_bps.find(node_id);
                    if (speed_it == ibd_peer_bps.end()) continue;

                    const double peer_bps = speed_it->second;
                    if (peer_bps < (fastest_bps * 0.5) && !pnode->fDisconnect) {
                        int64_t& slow_since = ibd_slow_since[node_id];
                        if (slow_since == 0) slow_since = now;
                        if (now - slow_since >= IBD_SLOW_PEER_DISCONNECT_DELAY) {
                            if (disconnect_candidate == -1 || peer_bps < disconnect_candidate_bps) {
                                disconnect_candidate = node_id;
                                disconnect_candidate_bps = peer_bps;
                            }
                        }
                    } else {
                        ibd_slow_since[node_id] = 0;
                    }
                }

                if (disconnect_candidate != -1 && nOutboundFullRelay > 2) {
                    for (CNode* pnode : nodes) {
                        if (pnode->GetId() != disconnect_candidate) continue;
                        if (pnode->HasPermission(NetPermissionFlags::NoBan)) continue;
                        pnode->fDisconnect = true;
                        LogPrintf("IBD speed eviction: peer=%d speed=%sB/s fastest=%sB/s window=%ds\n",
                                  disconnect_candidate, strprintf("%.2f", disconnect_candidate_bps), strprintf("%.2f", fastest_bps), (int)IBD_SPEED_MEASUREMENT_WINDOW);
                        break;
                    }
                }
            }

        } else {
            ibd_block_samples.clear();
            ibd_slow_since.clear();
            ibd_calibration_started = 0;
            ibd_max_full_outbound = 0;
            ibd_target_peers = 0;
        }
    } // if (now != lastnow)

    bool fLatestNodeScoreDegrading = false;
    if (!IsIBD && lastnow != now) {
        auto Median = [](std::vector<double> values) {
            if (values.empty()) return 0.0;
            std::sort(values.begin(), values.end());
            const size_t mid = values.size() / 2;
            if (values.size() % 2 == 1) return values[mid];
            return (values[mid - 1] + values[mid]) / 2.0;
        };
        constexpr double EPSILON{1e-6};
        std::vector<double> mp_pct_values;
        std::vector<double> mpm_values;
        mp_pct_values.reserve(outbound_peer_metrics.size());
        mpm_values.reserve(outbound_peer_metrics.size());
        for (const auto& [_, metrics] : outbound_peer_metrics) {
            mp_pct_values.push_back(metrics.first);
            mpm_values.push_back(metrics.second);
        }
        const double median_mp_pct{std::max(Median(std::move(mp_pct_values)), EPSILON)};
        const double median_mppm{std::max(Median(std::move(mpm_values)), EPSILON)};
        for (const auto& [node_id, metrics] : outbound_peer_metrics) {
            const double p{metrics.first / median_mp_pct};
            const double t{metrics.second / median_mppm};
            const double score{2.0 / ((1.0 / (p + EPSILON)) + (1.0 / (t + EPSILON)))};
            if (node_id == latestNode) nLatestNodeScore = score;
            if (score < nLowestScore) {
                nSecondLowestScore = nLowestScore;
                nLowestScore = score;
                nLowestEffectiveTXpm = metrics.second;
                worstNode = node_id;
            } else if (score < nSecondLowestScore) {
                nSecondLowestScore = score;
            }
        }
        if (lastWorst != worstNode) {
            tWorstChanged = now;
            LogPrintf("worst: score %d -> %d (%.3f:%.3f) Global: TXpm=%d Pct=%d %sbps\n",
                      lastWorst, worstNode, nLowestScore, nSecondLowestScore,
                      nGlobalTXpm, 100 * nTotalMempoolBytes / (nTotalBytesRecv + 1), strUnit(nGlobalBps));
            lastWorst = worstNode;
        }
        static double last_latest_node_score{0.0};
        if (nLatestNodeScore < last_latest_node_score) fLatestNodeScoreDegrading = true;
        last_latest_node_score = nLatestNodeScore;
    }

    for (CNode* pnode : nodes) {
        if (interruptNet)
            return;

        // Evict worst performing outbound connection
        if (!IsIBD && lastnow != now) {
            const double nLowest = nLowestScore;
            const double nSecondLowest = nSecondLowestScore;
            const bool fLatestNodeDegrading = fLatestNodeScoreDegrading;
            if (tIBDEnded > tWorstChanged) tWorstChanged = tIBDEnded;
            bool MaxedOut = nOutboundFullRelay >= (int)m_max_outbound_full_relay;
            bool DoIt = false;
            std::string strReason;
            std::string strDetails;
            int64_t m_connected = std::max(count_seconds(pnode->m_connected), tIBDEnded);
            if (pnode->GetId() == worstNode && !pnode->fDisconnect && !pnode->HasPermission(NetPermissionFlags::NoBan)) {
                if (MaxedOut) {
                    // A block came in and so the lowest will always be the lowest - disconnect it
                    if (m_last_block_time > latestOutboundConn && (pnode->nBTXpm || (pnode->nBTXpm == 0 && m_last_block_time - m_connected >= 120))) {
                        strReason += "R1";
                        strDetails += strprintf("LastBlk=%d", now - m_last_block_time);
                        DoIt = true;
                    }
                    // If no change for over 45 seconds and lowest either very low, or no new connections for over 2 minutes
                    if ((now - tWorstChanged >= 45) && (!fLatestNodeDegrading || worstNode == latestNode) && (nLowest <= nSecondLowest / 2 || now - latestSnapOld >= 120)) {
                        strReason += "R2";
                        strDetails += strprintf("Changed=%d", now - tWorstChanged);
                        DoIt = true;
                    }
                    // Disconnect any nodes where out TX input is zero and connected over 3 minutes
                    if (now - m_connected >= 180 && nLowestEffectiveTXpm == 0) {
                        strReason += "R3";
                        DoIt = true;
                    }
                }
            }
            if (DoIt) {
                pnode->fDisconnect = 1; nOutboundFullRelay--;
                LogPrintf("Evict: score=%.3f,%.3f %s %s TimeConn=%d LastOut=%d LastSnapOld=%d %sdisconnect peer=%d\n",
                          nLowest, nSecondLowest, strReason, strDetails, now - m_connected, now - latestOutboundConn,
                          now - latestSnapOld, MaxedOut ? "MO " : "", pnode->GetId());
                if ((now - latestOutboundConn) >= 120 && MaxedOut
                        && !nAnchorTryAgain && (now - latestSnapOld) >= 120) {
                    std::vector<CAddress> anchors_to_dump = GetCurrentFullNodesOnlyConns();
                    if (anchors_to_dump.size() > (size_t)m_max_outbound_full_relay - 1) {
                        anchors_to_dump.resize(m_max_outbound_full_relay - 1);
                    }
                    std::vector<CAddress> anchors_blockrelay = GetCurrentBlockRelayOnlyConns();
                    if (anchors_blockrelay.size() > MAX_BLOCK_RELAY_ONLY_ANCHORS) {
                        anchors_blockrelay.resize(MAX_BLOCK_RELAY_ONLY_ANCHORS);
                    }
                    anchors_to_dump.insert(anchors_to_dump.end(), anchors_blockrelay.begin(), anchors_blockrelay.end());
                    if (anchors_to_dump.size() == (size_t)m_max_outbound_full_relay + MAX_BLOCK_RELAY_ONLY_ANCHORS - 1)
                        DumpAnchors(gArgs.GetDataDirNet() / ANCHORS_DATABASE_FILENAME, anchors_to_dump);
                }
            }
        }

        //
        // Receive
        //
        int recvSet = 0;
        bool sendSet = false;
        int errorSet = 0;
        {
            LOCK(pnode->m_sock_mutex);
            if (!pnode->m_sock) {
                continue;
            }
            recvSet = recv_set.count(pnode->m_sock->Get());
            sendSet = send_set.count(pnode->m_sock->Get()) > 0;
            errorSet = error_set.count(pnode->m_sock->Get());
        }
        if (recvSet > 0 || errorSet > 0)
        {
            // typical socket buffer is 8K-64K
            uint8_t pchBuf[0x10000];
            int nBytes = 0;
            {
                LOCK(pnode->m_sock_mutex);
                if (!pnode->m_sock) {
                    continue;
                }
                nBytes = pnode->m_sock->Recv(pchBuf, sizeof(pchBuf), MSG_DONTWAIT);
            }
            if (nBytes > 0)
            {
                bool notify = false;
                if (!pnode->ReceiveMsgBytes({pchBuf, (size_t)nBytes}, notify)) {
                    LogPrintf("%s: ReceiveMsgBytes failed. Disconnect peer=%d\n", __func__, pnode->GetId());
                    pnode->CloseSocketDisconnect();
                }
                RecordBytesRecv(nBytes);
                if (notify) {
                    size_t nSizeAdded = 0;
                    auto it(pnode->vRecvMsg.begin());
                    for (; it != pnode->vRecvMsg.end(); ++it) {
                        // vRecvMsg contains only completed CNetMessage
                        // the single possible partially deserialized message are held by TransportDeserializer
                        nSizeAdded += it->m_raw_message_size;
                    }
                    {
                        LOCK(pnode->cs_vProcessMsg);
                        pnode->vProcessMsg.splice(pnode->vProcessMsg.end(), pnode->vRecvMsg, pnode->vRecvMsg.begin(), it);
                        pnode->nProcessQueueSize += nSizeAdded;
                        pnode->fPauseRecv = pnode->nProcessQueueSize > nReceiveFloodSize;
                    }
                    WakeMessageHandler();
                }
            }
            else if (nBytes == 0)
            {
                // socket closed gracefully
                if (!pnode->fDisconnect && !pnode->IsInboundConn())
                    LogPrintf("%s: nBytes=0 recvSet=%d errorSet=%d Disconnect peer=%d\n", __func__, recvSet, errorSet, pnode->GetId());
                pnode->CloseSocketDisconnect();
            }
            else if (nBytes < 0)
            {
                // error
                int nErr = WSAGetLastError();
                if (nErr != WSAEWOULDBLOCK && nErr != WSAEMSGSIZE && nErr != WSAEINTR && nErr != WSAEINPROGRESS)
                {
                    LogPrintf("%s: socket recv error for peer=%d: %s\n", __func__, pnode->GetId(), NetworkErrorString(nErr));
                    pnode->CloseSocketDisconnect();
                }
            }
        }

        if (sendSet) {
            // Send data
            size_t bytes_sent = WITH_LOCK(pnode->cs_vSend, return SocketSendData(*pnode));
            if (bytes_sent) RecordBytesSent(bytes_sent);
        }

        if (InactivityCheck(*pnode)) pnode->fDisconnect = true;
    }
    lastnow = now;
}

void CConnman::SocketHandlerListening(const std::set<SOCKET>& recv_set)
{
    for (const ListenSocket& listen_socket : vhListenSocket) {
        if (interruptNet) {
            return;
        }
        if (recv_set.count(listen_socket.sock->Get()) > 0) {
            AcceptConnection(listen_socket);
        }
    }
}

void CConnman::ThreadSocketHandler()
{
    PERF_MONITOR("net_socket_handler_thread");
    SetSyscallSandboxPolicy(SyscallSandboxPolicy::NET);
    while (!interruptNet)
    {
        {
            PERF_MONITOR("net_socket_handler_iteration");
            DisconnectNodes();
            NotifyNumConnectionsChanged();
            SocketHandler();
        }
    }
}

void CConnman::WakeMessageHandler()
{
    {
        LOCK(mutexMsgProc);
        fMsgProcWake = true;
    }
    condMsgProc.notify_one();
}

void CConnman::ThreadDNSAddressSeed()
{
    PERF_MONITOR("net_dns_seed_thread");
    SetSyscallSandboxPolicy(SyscallSandboxPolicy::INITIALIZATION_DNS_SEED);
    FastRandomContext rng;
    std::vector<std::string> seeds = Params().DNSSeeds();
    Shuffle(seeds.begin(), seeds.end(), rng);
    int seeds_right_now = 0; // Number of seeds left before testing if we have enough connections
    int found = 0;

    if (gArgs.GetBoolArg("-forcednsseed", DEFAULT_FORCEDNSSEED)) {
        // When -forcednsseed is provided, query all.
        seeds_right_now = seeds.size();
    } else if (addrman.size() == 0) {
        // If we have no known peers, query all.
        // This will occur on the first run, or if peers.dat has been
        // deleted.
        seeds_right_now = seeds.size();
    }

    // goal: only query DNS seed if address need is acute
    // * If we have a reasonable number of peers in addrman, spend
    //   some time trying them first. This improves user privacy by
    //   creating fewer identifying DNS requests, reduces trust by
    //   giving seeds less influence on the network topology, and
    //   reduces traffic to the seeds.
    // * When querying DNS seeds query a few at once, this ensures
    //   that we don't give DNS seeds the ability to eclipse nodes
    //   that query them.
    // * If we continue having problems, eventually query all the
    //   DNS seeds, and if that fails too, also try the fixed seeds.
    //   (done in ThreadOpenConnections)
    const std::chrono::seconds seeds_wait_time = (addrman.size() >= DNSSEEDS_DELAY_PEER_THRESHOLD ? DNSSEEDS_DELAY_MANY_PEERS : DNSSEEDS_DELAY_FEW_PEERS);

    for (const std::string& seed : seeds) {
        if (seeds_right_now == 0) {
            seeds_right_now += DNSSEEDS_TO_QUERY_AT_ONCE;

            if (addrman.size() > 0) {
                LogPrintf("Waiting %d seconds before querying DNS seeds.\n", seeds_wait_time.count());
                std::chrono::seconds to_wait = seeds_wait_time;
                while (to_wait.count() > 0) {
                    // if sleeping for the MANY_PEERS interval, wake up
                    // early to see if we have enough peers and can stop
                    // this thread entirely freeing up its resources
                    std::chrono::seconds w = std::min(DNSSEEDS_DELAY_FEW_PEERS, to_wait);
                    if (!interruptNet.sleep_for(w)) return;
                    to_wait -= w;

                    int nRelevant = 0;
                    {
                        LOCK(m_nodes_mutex);
                        for (const CNode* pnode : m_nodes) {
                            if (pnode->fSuccessfullyConnected && pnode->IsFullOutboundConn()) ++nRelevant;
                        }
                    }
                    if (nRelevant >= 2) {
                        if (found > 0) {
                            LogPrintf("%d addresses found from DNS seeds\n", found);
                            LogPrintf("P2P peers available. Finished DNS seeding.\n");
                        } else {
                            LogPrintf("P2P peers available. Skipped DNS seeding.\n");
                        }
                        return;
                    }
                }
            }
        }

        if (interruptNet) return;

        // hold off on querying seeds if P2P network deactivated
        if (!fNetworkActive) {
            LogPrintf("Waiting for network to be reactivated before querying DNS seeds.\n");
            do {
                if (!interruptNet.sleep_for(std::chrono::seconds{1})) return;
            } while (!fNetworkActive);
        }

        LogPrintf("Loading addresses from DNS seed %s\n", seed); // REBTODO - if bitnodes make them all "good"
        if (HaveNameProxy()) { // We're using a proxy server
            AddAddrFetch(seed); // We'll ask the peer directly in a special "address" mode.
        } else { // Use the DNS way
            std::vector<CNetAddr> vIPs;
            std::vector<CAddress> vAdd;
            ServiceFlags requiredServiceBits = GetDesirableServiceFlags(NODE_NONE);
            std::string host = strprintf("x%x.%s", requiredServiceBits, seed);
            CNetAddr resolveSource;
            if (!resolveSource.SetInternal(host)) {
                continue;
            }
            unsigned int nMaxIPs = 256; // Limits number of IPs learned from a DNS seed - REBTODO - increase for bitnodes
            if (LookupHost(host, vIPs, nMaxIPs, true)) {
                for (const CNetAddr& ip : vIPs) {
                    int nOneDay = 24*3600;
                    CAddress addr = CAddress(CService(ip, Params().GetDefaultPort()), requiredServiceBits);
                    addr.nTime = GetTime() - 3*nOneDay - rng.randrange(4*nOneDay); // use a random age between 3 and 7 days old
                    vAdd.push_back(addr);
                    found++;
                }
                addrman.Add(vAdd, resolveSource);
            } else {
                // We now avoid directly using results from DNS Seeds which do not support service bit filtering,
                // instead using them as a addrfetch to get nodes with our desired service bits.
                AddAddrFetch(seed);
            }
        }
        --seeds_right_now;
    }
    LogPrintf("%d addresses found from DNS seeds\n", found);
}

void CConnman::DumpAddresses()
{
    int64_t nStart = GetTimeMillis();

    DumpPeerAddresses(::gArgs, addrman);

    LogPrint(BCLog::NET, "Flushed %d addresses to peers.dat  %dms\n",
           addrman.size(), GetTimeMillis() - nStart);
}

void CConnman::ProcessAddrFetch()
{
    std::string strDest;
    {
        LOCK(m_addr_fetches_mutex);
        if (m_addr_fetches.empty())
            return;
        strDest = m_addr_fetches.front();
        m_addr_fetches.pop_front();
    }
    CAddress addr;
    CSemaphoreGrant grant(*semOutbound, true);
    if (grant) {
        OpenNetworkConnection(addr, false, &grant, strDest.c_str(), ConnectionType::ADDR_FETCH);
    }
}

bool CConnman::GetTryNewOutboundPeer() const
{
    return m_try_another_outbound_peer;
}

void CConnman::SetBlockAnchorCollectionActive(bool active)
{
    m_collect_block_anchors = active;
}

void CConnman::SetTryNewOutboundPeer(bool flag)
{
    m_try_another_outbound_peer = flag;
    LogPrint(BCLog::CONN, "net: setting try another outbound peer=%s\n", flag ? "true" : "false");
}

// Return the number of peers we have over our outbound connection limit
// Exclude peers that are marked for disconnect, or are going to be
// disconnected soon (eg ADDR_FETCH and FEELER)
// Also exclude peers that haven't finished initial connection handshake yet
// (so that we don't decide we're over our desired connection limit, and then
// evict some peer that has finished the handshake)
int CConnman::GetExtraFullOutboundCount() const
{
    int full_outbound_peers = 0;
    const int full_target = std::max(1, m_max_outbound_full_relay);
    {
        LOCK(m_nodes_mutex);
        for (const CNode* pnode : m_nodes) {
            if (pnode->fSuccessfullyConnected && !pnode->fDisconnect && pnode->IsFullOutboundConn()) {
                ++full_outbound_peers;
            }
        }
    }
    return std::max(full_outbound_peers - full_target, 0);
}

int CConnman::GetExtraBlockRelayCount() const
{
    int block_relay_peers = 0;
    const int block_relay_target = GetTargetOutboundBlockRelay();
    {
        LOCK(m_nodes_mutex);
        for (const CNode* pnode : m_nodes) {
            if (pnode->fSuccessfullyConnected && !pnode->fDisconnect && pnode->IsBlockOnlyConn()) {
                ++block_relay_peers;
            }
        }
    }
    return std::max(block_relay_peers - block_relay_target, 0);
}

int CConnman::GetTargetOutboundBlockRelay() const
{
    const int configured_target = m_max_outbound_block_relay;
    const int max_allowed = std::max(0, nMaxConnections - m_max_outbound_full_relay);
    if (!m_msgproc || !m_msgproc->IsInitialBlockDownload()) {
        return std::clamp(configured_target, 0, max_allowed);
    }
    return std::clamp(std::max(configured_target, IBD_BLOCK_RELAY_ONLY_CONNECTIONS), 0, max_allowed);
}

void CConnman::ThreadOpenConnections(const std::vector<std::string> connect)
{
    PERF_MONITOR("net_open_connections_thread");
    SetSyscallSandboxPolicy(SyscallSandboxPolicy::NET_OPEN_CONNECTION);
    // Connect to specific addresses
    if (!connect.empty())
    {
        for (int64_t nLoop = 0;; nLoop++)
        {
            ProcessAddrFetch(); // REBTODO - what's this?
            for (const std::string& strAddr : connect)
            {
                CAddress addr(CService(), NODE_NONE);
                OpenNetworkConnection(addr, false, nullptr, strAddr.c_str(), ConnectionType::MANUAL);
                for (int i = 0; i < 10 && i < nLoop; i++)
                {
                    if (!interruptNet.sleep_for(std::chrono::milliseconds(500)))
                        return;
                }
            }
            if (!interruptNet.sleep_for(std::chrono::milliseconds(500)))
                return;
        }
    }

    // Initiate network connections
    auto start = GetTime<std::chrono::microseconds>();

    // Minimum time before next feeler connection (in microseconds).
    auto next_feeler = GetExponentialRand(start, FEELER_INTERVAL);
    auto next_extra_block_relay = GetExponentialRand(start, EXTRA_BLOCK_RELAY_ONLY_PEER_INTERVAL);
    const bool dnsseed = gArgs.GetBoolArg("-dnsseed", DEFAULT_DNSSEED);
    bool add_fixed_seeds = gArgs.GetBoolArg("-fixedseeds", DEFAULT_FIXEDSEEDS);
    constexpr int64_t IBD_ANCHOR_STALE_BLOCK_AGE{2 * 60 * 60};

    if (!add_fixed_seeds) {
        LogPrintf("Fixed seeds are disabled\n");
    }

    while (!interruptNet)
    {
        ProcessAddrFetch();

        if (!interruptNet.sleep_for(std::chrono::milliseconds(500)))
            return;

        CSemaphoreGrant grant(*semOutbound);
        if (interruptNet)
            return;

        if (add_fixed_seeds && addrman.size() == 0) {
            // When the node starts with an empty peers.dat, there are a few other sources of peers before
            // we fallback on to fixed seeds: -dnsseed, -seednode, -addnode
            // If none of those are available, we fallback on to fixed seeds immediately, else we allow
            // 60 seconds for any of those sources to populate addrman.
            bool add_fixed_seeds_now = false;
            // It is cheapest to check if enough time has passed first.
            if (GetTime<std::chrono::seconds>() > start + std::chrono::minutes{1}) {
                add_fixed_seeds_now = true;
                LogPrintf("Adding fixed seeds as 60 seconds have passed and addrman is empty\n");
            }

            // Checking !dnsseed is cheaper before locking 2 mutexes.
            if (!add_fixed_seeds_now && !dnsseed) {
                LOCK2(m_addr_fetches_mutex, m_added_nodes_mutex);
                if (m_addr_fetches.empty() && m_added_nodes.empty()) {
                    add_fixed_seeds_now = true;
                    LogPrintf("Adding fixed seeds as -dnsseed=0, -addnode is not provided and all -seednode(s) attempted\n");
                }
            }

            if (add_fixed_seeds_now) {
                CNetAddr local;
                local.SetInternal("fixedseeds");
                addrman.Add(ConvertSeeds(Params().FixedSeeds()), local);
                add_fixed_seeds = false;
            }
        } // No addresses

        //
        // Choose an address to connect to based on most recently seen
        //
        CAddress addrConnect;

        // Only connect out to one peer per network group (/16 for IPv4).
        int nOutboundFullRelay = 0;
        int nOutboundBlockRelay = 0;
        int nPeersIBD = 0;
        std::set<std::vector<unsigned char> > setConnected;

        {
            LOCK(m_nodes_mutex);
            for (const CNode* pnode : m_nodes) {
                if (pnode->IsFullOutboundConn()) nOutboundFullRelay++;
                if (pnode->IsBlockOnlyConn()) nOutboundBlockRelay++;
                if (pnode->m_tx_relay && pnode->m_tx_relay->lastSentFeeFilter > 9000000) nPeersIBD++; // REBTODO - is this number reliable?

                // Netgroups for inbound and manual peers are not excluded because our goal here
                // is to not use multiple of our limited outbound slots on a single netgroup
                // but inbound and manual peers do not use our outbound slots. Inbound peers
                // also have the added issue that they could be attacker controlled and used
                // to prevent us from connecting to particular hosts if we used them here.
                switch (pnode->m_conn_type) {
                    case ConnectionType::INBOUND:
                    case ConnectionType::MANUAL:
                        break;
                    case ConnectionType::OUTBOUND_FULL_RELAY:
                    case ConnectionType::BLOCK_RELAY:
                    case ConnectionType::ADDR_FETCH:
                    case ConnectionType::FEELER:
                        setConnected.insert(pnode->addr.GetGroup(addrman.GetAsmap()));
                } // no default case, so the compiler can warn about missing cases
            } // for loop through nodes
        } // LOCK(m_nodes_mutex)
        const int nOutboundCountTotal = nOutboundFullRelay + nOutboundBlockRelay;
        static bool stale_tip_no_outbound_mode{false};
        if (nOutboundCountTotal > 0) {
            stale_tip_no_outbound_mode = false;
        }
        const bool in_ibd_anchor_mode = nPeersIBD > 0 || stale_tip_no_outbound_mode;
        const int full_relay_target = std::max(1, m_max_outbound_full_relay);
        const int block_relay_target = GetTargetOutboundBlockRelay();

        ConnectionType conn_type = ConnectionType::OUTBOUND_FULL_RELAY;
        auto now = GetTime<std::chrono::microseconds>();
        static int anchor = 0;
        static bool anchor_queue_ibd_only{false};
        if (m_anchors.size() >= MAX_BLOCK_RELAY_ONLY_ANCHORS + m_max_outbound_full_relay - 1)
            anchor = 0;
        bool fFeeler = false;

        // Determine what type of connection to open. Opening
        // BLOCK_RELAY connections to addresses from anchors.dat gets the highest
        // priority. Then we open OUTBOUND_FULL_RELAY priority until we
        // meet our full-relay capacity. Then we open BLOCK_RELAY connection
        // until we hit our block-relay-only peer limit.
        // GetTryNewOutboundPeer() gets set when a stale tip is detected, so we
        // try opening an additional OUTBOUND_FULL_RELAY connection. If none of
        // these conditions are met, check to see if it's time to try an extra
        // block-relay-only peer (to confirm our tip is current, see below) or the next_feeler
        // timer to decide if we should open a FEELER.

        if ((!m_anchors.empty() && anchor < block_relay_target) || nOutboundBlockRelay < block_relay_target) {
            conn_type = ConnectionType::BLOCK_RELAY;
        } else if (nOutboundFullRelay < full_relay_target) {
            // OUTBOUND_FULL_RELAY
        } else if (GetTryNewOutboundPeer()) {
            // OUTBOUND_FULL_RELAY
        } else if (now > next_extra_block_relay && m_start_extra_block_relay_peers) {
            // Periodically connect to a peer (using regular outbound selection
            // methodology from addrman) and stay connected long enough to sync
            // headers, but not much else.
            //
            // Then disconnect the peer, if we haven't learned anything new.
            //
            // The idea is to make eclipse attacks very difficult to pull off,
            // because every few minutes we're finding a new peer to learn headers
            // from.
            //
            // This is similar to the logic for trying extra outbound (full-relay)
            // peers, except:
            // - we do this all the time on an exponential timer, rather than just when
            //   our tip is stale
            // - we potentially disconnect our next-youngest block-relay-only peer, if our
            //   newest block-relay-only peer delivers a block more recently.
            //   See the eviction logic in net_processing.cpp.
            //
            // Because we can promote these connections to block-relay-only
            // connections, they do not get their own ConnectionType enum
            // (similar to how we deal with extra outbound peers).
            next_extra_block_relay = GetExponentialRand(now, EXTRA_BLOCK_RELAY_ONLY_PEER_INTERVAL);
            conn_type = ConnectionType::BLOCK_RELAY;
        } else if (now > next_feeler) {
            next_feeler = GetExponentialRand(now, FEELER_INTERVAL);
            conn_type = ConnectionType::FEELER;
            fFeeler = true;
        } else {
            // skip to next iteration of while loop
            continue;
        }

        addrman.ResolveCollisions();

        int64_t nANow = GetAdjustedTime(); // REBTODO - why the Adjusted one?
        int nTries = 0;
        static bool was_in_ibd_anchor_mode{true};
        if (!in_ibd_anchor_mode && was_in_ibd_anchor_mode && !m_anchors.empty()) {
            std::vector<CAddress> ibd_anchors_on_disk = ReadBlockAnchors(gArgs.GetDataDirNet() / BLOCK_ANCHORS_DATABASE_FILENAME);
            if (ibd_anchors_on_disk.empty()) {
                ibd_anchors_on_disk = ReadBlockAnchors(gArgs.GetDataDirNet() / LEGACY_IBD_ANCHORS_DATABASE_FILENAME);
            }
            if (!ibd_anchors_on_disk.empty()) {
                const std::set<CAddress> ibd_anchor_set(ibd_anchors_on_disk.begin(), ibd_anchors_on_disk.end());
                const size_t before = m_anchors.size();
                m_anchors.erase(std::remove_if(m_anchors.begin(), m_anchors.end(), [&](const CAddress& addr) {
                    return ibd_anchor_set.count(addr) > 0;
                }), m_anchors.end());
                const size_t removed = before - m_anchors.size();
                if (removed > 0) {
                    LogPrintf("IBD complete: removed %d IBD anchors from in-memory anchor queue\n", (int)removed);
                }
            }
        }
        was_in_ibd_anchor_mode = in_ibd_anchor_mode;
        while (!interruptNet)
        {
            static int nLastOutboundCount = MAX_OUTBOUND_FULL_RELAY_CONNECTIONS; // On startup read anchors
            int nOutboundCount = nOutboundFullRelay + nOutboundBlockRelay;
            if (nOutboundCount > nLastOutboundCount) {
                int nLastLast = nLastOutboundCount;
                nLastOutboundCount = std::min(nOutboundCount, m_max_outbound_full_relay +
                    (int)MAX_BLOCK_RELAY_ONLY_ANCHORS);
                if (nLastLast != nLastOutboundCount)
                    LogPrintf("anchor LOC %d -> %d\n", nLastLast, nLastOutboundCount);
            }
            if (!m_anchors.empty() && (anchor == 0 || m_nodes.size())) {
                anchor++;
                const CAddress addr = m_anchors.back();
                m_anchors.pop_back();
                if (nAnchorTryAgain < 0) nAnchorTryAgain = 0;
                if (!addr.IsValid() || IsLocal(addr) || !IsReachable(addr) ||
                        setConnected.count(addr.GetGroup(addrman.GetAsmap()))) break;
                addrConnect = addr;
                if (anchor_queue_ibd_only) {
                    LogPrintf("Trying(%d) to make an IBD anchor(%d) connection to %s\n", nAnchorTryAgain, anchor, addrConnect.ToString());
                } else {
                    LogPrintf("Trying(%d) to make a %s anchor(%d) connection to %s\n", nAnchorTryAgain,
                        ConnectionTypeAsString(conn_type), anchor, addrConnect.ToString());
                }
                break; // out of while
            } else if (anchor && m_anchors.empty()) {
                std::string strComment;
                nAnchorTryAgain++;
                if (nOutboundFullRelay >= anchor - block_relay_target) {
                    strComment = strprintf("No further action needed! (tries=%d)", nAnchorTryAgain);
                    nAnchorTryAgain = 0;
                } else {
                    if (nAnchorTryAgain >= 3) { // One retry is sufficient, 2nd retry rarely finds anything new.
                        strComment = strprintf("Oh well, I guess we'll find new ones. (tries=%d)", nAnchorTryAgain);
                        nAnchorTryAgain = 0;
                    } else {
                        if ((nAnchorTryAgain == 1 && nOutboundCount > 0) ||
                                (nAnchorTryAgain > 1 && nPeersIBD <= 1 && nOutboundCount >= 2))
                            strComment = strprintf("Oh dear, let's retry(%d) once more...nodes=%d IBD=%d", nAnchorTryAgain, nOutboundCount, nPeersIBD);
                        else
                            strComment = strprintf("Oh dear, we'll retry(%d) again shortly. nodes=%d IBD=%d", nAnchorTryAgain, nOutboundCount, nPeersIBD);
                    }
                }
                if (in_ibd_anchor_mode && !nAnchorTryAgain) {
                    const int ibd_signal = stale_tip_no_outbound_mode ? 1 : nPeersIBD;
                    strComment += strprintf(" but let's try after IBD(%d) anyway!", ibd_signal);
                    nAnchorTryAgain = 2;
                }
                LogPrintf("Finished connecting to %d anchors. Connections=%d+%d. %s\n", anchor, nOutboundBlockRelay, nOutboundFullRelay, strComment);
                anchor = 0;
            } // m_anchor not empty but anchor != 0

            if (m_anchors.empty() && ((nAnchorTryAgain == 1 && nOutboundCount > 0) ||
                    (nAnchorTryAgain > 1 && nPeersIBD <= 1 && nOutboundCount >= 2) ||
                    (nOutboundCount < (nLastOutboundCount+1)*2/3))) { // or a sudden drop in connections
                if (nOutboundCount < (nLastOutboundCount+1)*2/3)
                    LogPrintf("Outbound count dropped (%d < %d) LOC=%d\n", nOutboundCount, (nLastOutboundCount+1)*2/3, nLastOutboundCount);
                else
                    LogPrintf("ATA=%D OC=%d PIBD=%d\n", nAnchorTryAgain, nOutboundCount, nPeersIBD);
                nLastOutboundCount = nOutboundCount;
                if (nAnchorTryAgain >= 0 && !interruptNet.sleep_for(std::chrono::milliseconds(500)))
                        return;
                if (interruptNet) return;
                int64_t tip_block_age{0};
                if (nOutboundCountTotal == 0 && m_msgproc) {
                    const int64_t now_seconds = GetTimeSeconds();
                    const std::optional<int64_t> tip_block_time = m_msgproc->GetTipBlockTime();
                    stale_tip_no_outbound_mode = tip_block_time &&
                        now_seconds - *tip_block_time > IBD_ANCHOR_STALE_BLOCK_AGE;
                    if (tip_block_time) tip_block_age = now_seconds - *tip_block_time;
                } else if (nOutboundCountTotal > 0) {
                    stale_tip_no_outbound_mode = false;
                }
                const bool reload_ibd_anchor_mode = nPeersIBD > 0 || stale_tip_no_outbound_mode;
                m_anchors.clear();
                // Load only one anchor set at a time: IBD anchors while IBD appears active,
                // otherwise tx anchors.
                if (reload_ibd_anchor_mode) {
                    if (stale_tip_no_outbound_mode) {
                        LogPrintf("Using IBD anchors: tip block age=%ds and outbound count is zero\n", tip_block_age);
                    }
                    std::vector<CAddress> ibd_anchors = ReadBlockAnchors(gArgs.GetDataDirNet() / BLOCK_ANCHORS_DATABASE_FILENAME);
                    if (ibd_anchors.empty()) {
                        ibd_anchors = ReadBlockAnchors(gArgs.GetDataDirNet() / LEGACY_IBD_ANCHORS_DATABASE_FILENAME);
                    }
                    for (auto it = ibd_anchors.rbegin(); it != ibd_anchors.rend(); ++it) {
                        m_anchors.push_back(*it);
                    }
                    anchor_queue_ibd_only = true;
                } else {
                    std::vector<CAddress> tx_anchors = ReadAnchors(gArgs.GetDataDirNet() / ANCHORS_DATABASE_FILENAME);
                    for (auto it = tx_anchors.rbegin(); it != tx_anchors.rend(); ++it) {
                        m_anchors.push_back(*it);
                    }
                    anchor_queue_ibd_only = false;
                }
                if (nAnchorTryAgain >= 0 && !interruptNet.sleep_for(std::chrono::milliseconds(500)))
                    return;
                break;
            } // This'll get picked up the next time we hit the check for m_anchors above.

            // If we didn't find an appropriate destination after trying 100 addresses fetched from addrman,
            // stop this loop, and let the outer loop run again (which sleeps, adds seed nodes, recalculates
            // already-connected network ranges, ...) before trying new addrman addresses.
            nTries++;
            if (nTries > 100)
                break;

            CAddress addr;
            int64_t addr_last_try{0};

            if (fFeeler) {
                // First, try to get a tried table collision address. This returns
                // an empty (invalid) address if there are no collisions to try.
                std::tie(addr, addr_last_try) = addrman.SelectTriedCollision();

                if (!addr.IsValid()) {
                    // No tried table collisions. Select a new table address
                    // for our feeler.
                    std::tie(addr, addr_last_try) = addrman.Select(true);
                } else if (AlreadyConnectedToAddress(addr)) {
                    // If test-before-evict logic would have us connect to a
                    // peer that we're already connected to, just mark that
                    // address as Good(). We won't be able to initiate the
                    // connection anyway, so this avoids inadvertently evicting
                    // a currently-connected peer.
                    addrman.Good(addr); // REBTODO - we need to do this for nodes seeded from bitnodes
                    // Select a new table address for our feeler instead.
                    std::tie(addr, addr_last_try) = addrman.Select(true);
                }
            } else {
                // Not a feeler
                std::tie(addr, addr_last_try) = addrman.Select();
            }

            // Require outbound connections, other than feelers, to be to distinct network groups
            if (!fFeeler && setConnected.count(addr.GetGroup(addrman.GetAsmap()))) {
                break;
            }

            // if we selected an invalid or local address, restart
            if (!addr.IsValid() || IsLocal(addr)) {
                break;
            }

            if (!IsReachable(addr))
                continue;

            // only consider very recently tried nodes after 30 failed attempts
            if (nANow - addr_last_try < 600 && nTries < 30)
                continue;

            // for non-feelers, require all the services we'll want,
            // for feelers, only require they be a full node (only because most
            // SPV clients don't have a good address DB available)
            if (!fFeeler && !HasAllDesirableServiceFlags(addr.nServices)) {
                continue;
            } else if (fFeeler && !MayHaveUsefulAddressDB(addr.nServices)) {
                continue;
            }

            // Do not connect to bad ports, unless 50 invalid addresses have been selected already.
            if (IsBadPort(addr.GetPort()) && (addr.IsIPv4() || addr.IsIPv6()) && nTries < 50) {
                continue;
            }

            addrConnect = addr;
            break;
        } // while - meaning we've selected an address to try or addrConnect is invalid

        if (addrConnect.IsValid()) {

            if (fFeeler) {
                // Add small amount of random noise before connection to avoid synchronization.
                int randsleep = GetRandInt(FEELER_SLEEP_WINDOW * 1000);
                if (!interruptNet.sleep_for(std::chrono::milliseconds(randsleep)))
                    return;
                LogPrint(BCLog::NET, "Making feeler connection to %s\n", addrConnect.ToString());
            }

            // Check for interruption before attempting connection
            if (interruptNet) return;

            OpenNetworkConnection(addrConnect, (int)setConnected.size() >= std::min(nMaxConnections - 1, 2), &grant, nullptr, conn_type);
        }
    }
}

std::vector<CAddress> CConnman::GetCurrentBlockRelayOnlyConns() const
{
    std::vector<CAddress> ret;
    LOCK(m_nodes_mutex);
    for (const CNode* pnode : m_nodes) {
        if (pnode->IsBlockOnlyConn()) {
            ret.push_back(pnode->addr);
        }
    }

    return ret;
}

std::vector<CAddress> CConnman::GetCurrentFullNodesOnlyConns() const
{
    std::vector<CAddress> ret;
    LOCK(m_nodes_mutex);
    for (const CNode* pnode : m_nodes) {
        if (pnode->IsFullOutboundConn() && !pnode->fDisconnect) {
            ret.push_back(pnode->addr);
        }
    }

    return ret;
}

std::vector<AddedNodeInfo> CConnman::GetAddedNodeInfo() const
{
    std::vector<AddedNodeInfo> ret;

    std::list<std::string> lAddresses(0);
    {
        LOCK(m_added_nodes_mutex);
        ret.reserve(m_added_nodes.size());
        std::copy(m_added_nodes.cbegin(), m_added_nodes.cend(), std::back_inserter(lAddresses));
    }


    // Build a map of all already connected addresses (by IP:port and by name) to inbound/outbound and resolved CService
    std::map<CService, bool> mapConnected;
    std::map<std::string, std::pair<bool, CService>> mapConnectedByName;
    {
        LOCK(m_nodes_mutex);
        for (const CNode* pnode : m_nodes) {
            if (pnode->addr.IsValid()) {
                mapConnected[pnode->addr] = pnode->IsInboundConn();
            }
            std::string addrName{pnode->m_addr_name};
            if (!addrName.empty()) {
                mapConnectedByName[std::move(addrName)] = std::make_pair(pnode->IsInboundConn(), static_cast<const CService&>(pnode->addr));
            }
        }
    }

    for (const std::string& strAddNode : lAddresses) {
        CService service(LookupNumeric(strAddNode, Params().GetDefaultPort(strAddNode)));
        AddedNodeInfo addedNode{strAddNode, CService(), false, false};
        if (service.IsValid()) {
            // strAddNode is an IP:port
            auto it = mapConnected.find(service);
            if (it != mapConnected.end()) {
                addedNode.resolvedAddress = service;
                addedNode.fConnected = true;
                addedNode.fInbound = it->second;
            }
        } else {
            // strAddNode is a name
            auto it = mapConnectedByName.find(strAddNode);
            if (it != mapConnectedByName.end()) {
                addedNode.resolvedAddress = it->second.second;
                addedNode.fConnected = true;
                addedNode.fInbound = it->second.first;
            }
        }
        ret.emplace_back(std::move(addedNode));
    }

    return ret;
}

void CConnman::ThreadOpenAddedConnections()
{
    PERF_MONITOR("net_open_added_connections_thread");
    SetSyscallSandboxPolicy(SyscallSandboxPolicy::NET_ADD_CONNECTION);
    while (true)
    {
        CSemaphoreGrant grant(*semAddnode);
        std::vector<AddedNodeInfo> vInfo = GetAddedNodeInfo();
        bool tried = false;
        for (const AddedNodeInfo& info : vInfo) {
            if (!info.fConnected) {
                if (!grant.TryAcquire()) {
                    // If we've used up our semaphore and need a new one, let's not wait here since while we are waiting
                    // the addednodeinfo state might change.
                    break;
                }
                tried = true;
                CAddress addr(CService(), NODE_NONE);
                OpenNetworkConnection(addr, false, &grant, info.strAddedNode.c_str(), ConnectionType::MANUAL);
                if (!interruptNet.sleep_for(std::chrono::milliseconds(500)))
                    return;
            }
        }
        // Retry every 60 seconds if a connection was attempted, otherwise two seconds
        if (!interruptNet.sleep_for(std::chrono::seconds(tried ? 60 : 2)))
            return;
    }
}

// if successful, this moves the passed grant to the constructed node
void CConnman::OpenNetworkConnection(const CAddress& addrConnect, bool fCountFailure, CSemaphoreGrant *grantOutbound, const char *pszDest, ConnectionType conn_type) {
    assert(conn_type != ConnectionType::INBOUND);

    //
    // Initiate outbound network connection
    //
    if (interruptNet) return;
    if (!fNetworkActive) return;
    if (!pszDest) {
        bool banned_or_discouraged = m_banman && (m_banman->IsDiscouraged(addrConnect) || m_banman->IsBanned(addrConnect));
        bool on_probation = m_banman && m_banman->IsOnProbation(addrConnect);
        if (IsLocal(addrConnect) || banned_or_discouraged || on_probation || AlreadyConnectedToAddress(addrConnect))
            return;
    } else if (FindNode(std::string(pszDest))) return;

    CNode* pnode = ConnectNode(addrConnect, pszDest, fCountFailure, conn_type);

    if (!pnode) return;
    if (grantOutbound) grantOutbound->MoveTo(pnode->grantOutbound);

    m_msgproc->InitializeNode(pnode);
    {
        LOCK(m_nodes_mutex);
        m_nodes.push_back(pnode);
    }
}

void CConnman::ThreadMessageHandler()
{
    PERF_MONITOR("net_message_handler_thread");
    SetSyscallSandboxPolicy(SyscallSandboxPolicy::MESSAGE_HANDLER);
    while (!flagInterruptMsgProc || nBlocksToBeProcessed > 0)
    {
        PERF_MONITOR("net_message_handler_iteration");
        bool fMoreWork = false;

        {
            // Randomize the order in which we process messages from/to our peers.
            // This prevents attacks in which an attacker exploits having multiple
            // consecutive connections in the m_nodes list.
            const NodesSnapshot snap{*this, 4, /*shuffle=*/true};

            static bool fToggle = false; // So that net_processing can see this loop
            for (CNode* pnode : snap.Nodes()) {
                if (pnode->fDisconnect) {
                    if (pnode->nBlocksToBeProcessed < 1)
                        continue;
                    else
                        LogPrintf("%s: Force ProcessMessages() as blks2b=%d vProcessMsgs=%d fDisconnect=%s peer=%d\n", __func__, pnode->nBlocksToBeProcessed, pnode->vProcessMsg.size(), pnode->fDisconnect, pnode->GetId());
                }

                // Receive messages
                bool fMoreNodeWork = m_msgproc->ProcessMessages(pnode, flagInterruptMsgProc, fToggle);
                fMoreWork |= (fMoreNodeWork && !pnode->fPauseSend);
                if (flagInterruptMsgProc)
                    return;
                // Send messages
                {
                    LOCK(pnode->cs_sendProcessing);
                    m_msgproc->SendMessages(pnode);
                }

                if (flagInterruptMsgProc)
                    return;
            }
            fToggle = !fToggle;
        }

        WAIT_LOCK(mutexMsgProc, lock);
        if (!fMoreWork) {
            // Use wait_for (relative timeout) instead of wait_until to avoid EINVAL from
            // absolute-time conversion in pthread_cond_* on some glibc/kernel combinations.
            condMsgProc.wait_for(lock, std::chrono::milliseconds(100), [this]() EXCLUSIVE_LOCKS_REQUIRED(mutexMsgProc) { return fMsgProcWake; });
        }
        fMsgProcWake = false;
    }
}

i2p::sam::Session* CConnman::GetI2POutgoingSession()
{
    return m_i2p_sam_sessions.empty() ? nullptr : m_i2p_sam_sessions.front().get();
}

void CConnman::ThreadI2PAcceptIncoming(i2p::sam::Session* session)
{
    PERF_MONITOR("net_i2p_accept_thread");
    static constexpr auto err_wait_begin = 1s;
    static constexpr auto err_wait_cap = 5min;
    auto err_wait = err_wait_begin;

    bool advertising_listen_addr = false;
    i2p::Connection conn;

    while (!interruptNet) {

        if (!session->Listen(conn)) {
            if (advertising_listen_addr && conn.me.IsValid()) {
                RemoveLocal(conn.me);
                advertising_listen_addr = false;
            }

            interruptNet.sleep_for(err_wait);
            if (err_wait < err_wait_cap) {
                err_wait *= 2;
            }

            continue;
        }

        if (!advertising_listen_addr) {
            AddLocal(conn.me, LOCAL_MANUAL);
            advertising_listen_addr = true;
        }

        if (!session->Accept(conn)) {
            continue;
        }

        CreateNodeFromAcceptedSocket(std::move(conn.sock), NetPermissionFlags::None,
                                     CAddress{conn.me, NODE_NONE}, CAddress{conn.peer, NODE_NONE});
    }
}

bool CConnman::BindListenPort(const CService& addrBind, bilingual_str& strError, NetPermissionFlags permissions)
{
    PERF_MONITOR("net_bind_listen_port");
    int nOne = 1;

    // Create socket for listening for incoming connections
    struct sockaddr_storage sockaddr;
    socklen_t len = sizeof(sockaddr);
    if (!addrBind.GetSockAddr((struct sockaddr*)&sockaddr, &len))
    {
        strError = strprintf(Untranslated("Error: Bind address family for %s not supported"), addrBind.ToString());
        LogPrintf("%s\n", strError.original);
        return false;
    }

    std::unique_ptr<Sock> sock = CreateSock(addrBind);
    if (!sock) {
        strError = strprintf(Untranslated("Error: Couldn't open socket for incoming connections (socket returned error %s)"), NetworkErrorString(WSAGetLastError()));
        LogPrintf("%s\n", strError.original);
        return false;
    }

    // Allow binding if the port is still in TIME_WAIT state after
    // the program was closed and restarted.
    setsockopt(sock->Get(), SOL_SOCKET, SO_REUSEADDR, (sockopt_arg_type)&nOne, sizeof(int));

    // some systems don't have IPV6_V6ONLY but are always v6only; others do have the option
    // and enable it by default or not. Try to enable it, if possible.
    if (addrBind.IsIPv6()) {
#ifdef IPV6_V6ONLY
        setsockopt(sock->Get(), IPPROTO_IPV6, IPV6_V6ONLY, (sockopt_arg_type)&nOne, sizeof(int));
#endif
#ifdef WIN32
        int nProtLevel = PROTECTION_LEVEL_UNRESTRICTED;
        setsockopt(sock->Get(), IPPROTO_IPV6, IPV6_PROTECTION_LEVEL, (const char*)&nProtLevel, sizeof(int));
#endif
    }

    if (::bind(sock->Get(), (struct sockaddr*)&sockaddr, len) == SOCKET_ERROR)
    {
        int nErr = WSAGetLastError();
        if (nErr == WSAEADDRINUSE)
            strError = strprintf(_("Unable to bind to %s on this computer. %s is probably already running."), addrBind.ToString(), PACKAGE_NAME);
        else
            strError = strprintf(_("Unable to bind to %s on this computer (bind returned error %s)"), addrBind.ToString(), NetworkErrorString(nErr));
        LogPrintf("%s\n", strError.original);
        return false;
    }
    LogPrintf("Bound to %s\n", addrBind.ToString());

    // Listen for incoming connections
    if (listen(sock->Get(), SOMAXCONN) == SOCKET_ERROR)
    {
        strError = strprintf(_("Error: Listening for incoming connections failed (listen returned error %s)"), NetworkErrorString(WSAGetLastError()));
        LogPrintf("%s\n", strError.original);
        return false;
    }

    vhListenSocket.emplace_back(std::move(sock), permissions);
    return true;
}

void Discover()
{
    if (!fDiscover)
        return;

    for (const CNetAddr &addr : GetLocalAddresses()) {
        if (AddLocal(addr, LOCAL_IF))
            LogPrintf("%s: %s\n", __func__, addr.ToString());
    }
}

void CConnman::SetNetworkActive(bool active)
{
    LogPrintf("%s: %s\n", __func__, active);

    if (fNetworkActive == active) {
        return;
    }

    fNetworkActive = active;

    // Reset Tor connection backoff if enabled
    if (active && gArgs.GetArg("-torcontrol", "").empty() == false) ResetTorBackoff();

    if (m_client_interface) {
        m_client_interface->NotifyNetworkActiveChanged(fNetworkActive);
    }
}

CConnman::CConnman(uint64_t nSeed0In, uint64_t nSeed1In, AddrMan& addrman_in, bool network_active)
    : addrman(addrman_in), nSeed0(nSeed0In), nSeed1(nSeed1In)
{
    SetTryNewOutboundPeer(false);

    Options connOptions;
    Init(connOptions);
    SetNetworkActive(network_active);
}

void CConnman::ResetNewNodeId()
{
    nLastNodeId = 0;
}

NodeId CConnman::GetNewNodeId()
{
    return nLastNodeId.fetch_add(1, std::memory_order_relaxed);
}


bool CConnman::Bind(const CService& addr_, unsigned int flags, NetPermissionFlags permissions)
{
    const CService addr{MaybeFlipIPv6toCJDNS(addr_)};

    if (!(flags & BF_EXPLICIT) && !IsReachable(addr)) {
        return false;
    }
    bilingual_str strError;
    if (!BindListenPort(addr, strError, permissions)) {
        if ((flags & BF_REPORT_ERROR) && m_client_interface) {
            m_client_interface->ThreadSafeMessageBox(strError, "", CClientUIInterface::MSG_ERROR);
        }
        return false;
    }

    if (addr.IsRoutable() && fDiscover && !(flags & BF_DONT_ADVERTISE) && !NetPermissions::HasFlag(permissions, NetPermissionFlags::NoBan)) {
        AddLocal(addr, LOCAL_BIND);
    }

    return true;
}

bool CConnman::InitBinds(const Options& options)
{
    bool fBound = false;
    for (const auto& addrBind : options.vBinds) {
        fBound |= Bind(addrBind, (BF_EXPLICIT | BF_REPORT_ERROR), NetPermissionFlags::None);
    }
    for (const auto& addrBind : options.vWhiteBinds) {
        fBound |= Bind(addrBind.m_service, (BF_EXPLICIT | BF_REPORT_ERROR), addrBind.m_flags);
    }
    for (const auto& addr_bind : options.onion_binds) {
        fBound |= Bind(addr_bind, BF_EXPLICIT | BF_DONT_ADVERTISE, NetPermissionFlags::None);
    }
    if (options.bind_on_any) {
        struct in_addr inaddr_any;
        inaddr_any.s_addr = htonl(INADDR_ANY);
        struct in6_addr inaddr6_any = IN6ADDR_ANY_INIT;
        fBound |= Bind(CService(inaddr6_any, GetListenPort()), BF_NONE, NetPermissionFlags::None);
        fBound |= Bind(CService(inaddr_any, GetListenPort()), !fBound ? BF_REPORT_ERROR : BF_NONE, NetPermissionFlags::None);
    }
    return fBound;
}

static std::vector<fs::path> GetI2PPrivateKeyFiles()
{
    std::vector<fs::path> files;
    std::set<fs::path> seen;

    const fs::path main_key = gArgs.GetDataDirNet() / "i2p_private_key";
    files.push_back(main_key);
    seen.insert(main_key);

    const fs::path directory = gArgs.GetDataDirNet() / "i2p_private_keys";
    try {
        if (!fs::exists(directory)) {
            fs::create_directories(directory);
        }

        if (!fs::is_directory(directory)) {
            LogPrintf("I2P: Failed to access private key directory %s\n", fs::PathToString(directory));
            return files;
        }

        for (const auto& entry : fs::directory_iterator(directory)) {
            if (!fs::is_regular_file(entry.status())) {
                continue;
            }
            const fs::path path = entry.path();
            if (seen.insert(path).second) {
                files.push_back(path);
            }
        }
    } catch (const fs::filesystem_error& e) {
        LogPrintf("I2P: Error reading private key directory %s: %s\n",
                  fs::PathToString(directory), e.what());
    }

    return files;
}

bool CConnman::Start(CScheduler& scheduler, const Options& connOptions)
{
    Init(connOptions);

    if (fListen && !InitBinds(connOptions)) {
        if (m_client_interface) {
            m_client_interface->ThreadSafeMessageBox(
                _("Failed to listen on any port. Use -listen=0 if you want this."),
                "", CClientUIInterface::MSG_ERROR);
        }
        return false;
    }

    proxyType i2p_sam;
    if (GetProxy(NET_I2P, i2p_sam)) {
        for (const auto& key_file : GetI2PPrivateKeyFiles()) {
            m_i2p_sam_sessions.push_back(std::make_unique<i2p::sam::Session>(key_file, i2p_sam.proxy,
                                                                            &interruptNet));
        }
    }

    for (const auto& strDest : connOptions.vSeedNodes) {
        AddAddrFetch(strDest);
    }

    if (m_client_interface) {
        m_client_interface->InitMessage(_("Starting network threads…").translated);
    }

    fAddressesInitialized = true;

    if (semOutbound == nullptr) {
        // initialize semaphore
        semOutbound = std::make_unique<CSemaphore>(nMaxConnections);
    }
    if (semAddnode == nullptr) {
        // initialize semaphore
        semAddnode = std::make_unique<CSemaphore>(nMaxAddnode);
    }

    //
    // Start threads
    //
    assert(m_msgproc);
    InterruptSocks5(false);
    interruptNet.reset();
    flagInterruptMsgProc = false;

    {
        LOCK(mutexMsgProc);
        fMsgProcWake = false;
    }

    // Send and receive from sockets, accept connections
    threadSocketHandler = std::thread(&util::TraceThread, "net", [this] { ThreadSocketHandler(); });

    if (!gArgs.GetBoolArg("-dnsseed", DEFAULT_DNSSEED))
        LogPrintf("DNS seeding disabled\n");
    else
        threadDNSAddressSeed = std::thread(&util::TraceThread, "dnsseed", [this] { ThreadDNSAddressSeed(); });

    // Initiate manual connections
    threadOpenAddedConnections = std::thread(&util::TraceThread, "addcon", [this] { ThreadOpenAddedConnections(); });

    if (connOptions.m_use_addrman_outgoing && !connOptions.m_specified_outgoing.empty()) {
        if (m_client_interface) {
            m_client_interface->ThreadSafeMessageBox(
                _("Cannot provide specific connections and have addrman find outgoing connections at the same time."),
                "", CClientUIInterface::MSG_ERROR);
        }
        return false;
    }
    if (connOptions.m_use_addrman_outgoing || !connOptions.m_specified_outgoing.empty()) {
        threadOpenConnections = std::thread(
            &util::TraceThread, "opencon",
            [this, connect = connOptions.m_specified_outgoing] { ThreadOpenConnections(connect); });
    }

    // Process messages
    threadMessageHandler = std::thread(&util::TraceThread, "msghand", [this] { ThreadMessageHandler(); });

    if (connOptions.m_i2p_accept_incoming && !m_i2p_sam_sessions.empty()) {
        threadI2PAcceptIncoming.reserve(m_i2p_sam_sessions.size());
        for (auto& sess : m_i2p_sam_sessions) {
            i2p::sam::Session* session = sess.get();
            threadI2PAcceptIncoming.emplace_back(&util::TraceThread, "i2paccept",
                                                 [this, session] { ThreadI2PAcceptIncoming(session); });
        }
    }

    // Validate blocks
    threadValidation = std::thread(&util::TraceThread, "validate", [this] { ThreadValidation(); });

    // Dump network addresses
    scheduler.scheduleEvery([this] { DumpAddresses(); }, DUMP_PEERS_INTERVAL);

    return true;
}

class CNetCleanup
{
public:
    CNetCleanup() {}

    ~CNetCleanup()
    {
#ifdef WIN32
        // Shutdown Windows Sockets
        WSACleanup();
#endif
    }
};
static CNetCleanup instance_of_cnetcleanup;

void CConnman::Interrupt()
{
    LogPrintf("%s: Start\n", __func__);
    {
        LOCK(mutexMsgProc);
        flagInterruptMsgProc = true;
    }
    condMsgProc.notify_all();

    interruptNet();
    InterruptSocks5(true);

    if (semOutbound) {
        for (int i=0; i<m_max_outbound; i++) {
            semOutbound->post();
        }
    }

    if (semAddnode) {
        for (int i=0; i<nMaxAddnode; i++) {
            semAddnode->post();
        }
    }
}

void CConnman::StopThreads()
{
    for (auto& t : threadI2PAcceptIncoming) {
        if (t.joinable()) {
            t.join();
        }
    }
    if (threadMessageHandler.joinable())
        threadMessageHandler.join();
    if (threadValidation.joinable())
        threadValidation.join();
    if (threadOpenConnections.joinable())
        threadOpenConnections.join();
    if (threadOpenAddedConnections.joinable())
        threadOpenAddedConnections.join();
    if (threadDNSAddressSeed.joinable())
        threadDNSAddressSeed.join();
    if (threadSocketHandler.joinable())
        threadSocketHandler.join();
}

void CConnman::StopNodes()
{
    if (fAddressesInitialized) {
        DumpAddresses();
        fAddressesInitialized = false;
    }

    // Delete peer connections.
    std::vector<CNode*> nodes;
    WITH_LOCK(m_nodes_mutex, nodes.swap(m_nodes));
    for (CNode* pnode : nodes) {
        pnode->CloseSocketDisconnect();
        LogPrint(BCLog::CONN, "%s: Calling DeleteNode GRC=%d from Delete peer connection. peer=%d\n", __func__, pnode->GetRefCount(), pnode->GetId());
        DeleteNode(pnode);
    }

    for (CNode* pnode : m_nodes_disconnected) {
        LogPrint(BCLog::CONN, "%s: Calling DeleteNode GRC=%d from m_nodes_disconnected. peer=%d\n", __func__, pnode->GetRefCount(), pnode->GetId());
        DeleteNode(pnode);
    }
    m_nodes_disconnected.clear();
    vhListenSocket.clear();
    semOutbound.reset();
    semAddnode.reset();
}

void CConnman::DeleteNode(CNode* pnode)
{
    assert(pnode);
    m_msgproc->FinalizeNode(*pnode);
    delete pnode;
}

CConnman::~CConnman()
{
    Interrupt();
    Stop();
}

std::vector<CAddress> CConnman::GetAddresses(size_t max_addresses, size_t max_pct, std::optional<Network> network) const
{
    std::vector<CAddress> addresses = addrman.GetAddr(max_addresses, max_pct, network);
    if (m_banman) {
        addresses.erase(std::remove_if(addresses.begin(), addresses.end(),
                        [this](const CAddress& addr){return m_banman->IsDiscouraged(addr) || m_banman->IsBanned(addr);}),
                        addresses.end());
    }
    return addresses;
}

std::vector<AddrManAddressInfo> CConnman::GetAddressesInfo(size_t max_addresses, size_t max_pct, std::optional<Network> network) const
{
    return addrman.GetAddrInfo(max_addresses, max_pct, network);
}

std::vector<CAddress> CConnman::GetAddresses(CNode& requestor, size_t max_addresses, size_t max_pct)
{
    auto local_socket_bytes = requestor.addrBind.GetAddrBytes();
    uint64_t cache_id = GetDeterministicRandomizer(RANDOMIZER_ID_ADDRCACHE)
        .Write(requestor.addr.GetNetwork())
        .Write(local_socket_bytes.data(), local_socket_bytes.size())
        .Finalize();
    const auto current_time = GetTime<std::chrono::microseconds>();
    auto r = m_addr_response_caches.emplace(cache_id, CachedAddrResponse{});
    CachedAddrResponse& cache_entry = r.first->second;
    if (cache_entry.m_cache_entry_expiration < current_time) { // If emplace() added new one it has expiration 0.
        cache_entry.m_addrs_response_cache = GetAddresses(max_addresses, max_pct, /* network */ std::nullopt);
        // Choosing a proper cache lifetime is a trade-off between the privacy leak minimization
        // and the usefulness of ADDR responses to honest users.
        //
        // Longer cache lifetime makes it more difficult for an attacker to scrape
        // enough AddrMan data to maliciously infer something useful.
        // By the time an attacker scraped enough AddrMan records, most of
        // the records should be old enough to not leak topology info by
        // e.g. analyzing real-time changes in timestamps.
        //
        // It takes only several hundred requests to scrape everything from an AddrMan containing 100,000 nodes,
        // so ~24 hours of cache lifetime indeed makes the data less inferable by the time
        // most of it could be scraped (considering that timestamps are updated via
        // ADDR self-announcements and when nodes communicate).
        // We also should be robust to those attacks which may not require scraping *full* victim's AddrMan
        // (because even several timestamps of the same handful of nodes may leak privacy).
        //
        // On the other hand, longer cache lifetime makes ADDR responses
        // outdated and less useful for an honest requestor, e.g. if most nodes
        // in the ADDR response are no longer active.
        //
        // However, the churn in the network is known to be rather low. Since we consider
        // nodes to be "terrible" (see IsTerrible()) if the timestamps are older than 30 days,
        // max. 24 hours of "penalty" due to cache shouldn't make any meaningful difference
        // in terms of the freshness of the response.
        cache_entry.m_cache_entry_expiration = current_time + std::chrono::hours(21) + GetRandMillis(std::chrono::hours(6));
    }
    return cache_entry.m_addrs_response_cache;
}

bool CConnman::AddNode(const std::string& strNode)
{
    LOCK(m_added_nodes_mutex);
    for (const std::string& it : m_added_nodes) {
        if (strNode == it) return false;
    }

    m_added_nodes.push_back(strNode);
    return true;
}

bool CConnman::RemoveAddedNode(const std::string& strNode)
{
    LOCK(m_added_nodes_mutex);
    for(std::vector<std::string>::iterator it = m_added_nodes.begin(); it != m_added_nodes.end(); ++it) {
        if (strNode == *it) {
            m_added_nodes.erase(it);
            return true;
        }
    }
    return false;
}

size_t CConnman::GetNodeCount(ConnectionDirection flags) const
{
    LOCK(m_nodes_mutex);
    if (flags == ConnectionDirection::Both) // Shortcut if we want total
        return m_nodes.size();

    int nNum = 0;
    for (const auto& pnode : m_nodes) {
        if (flags & (pnode->IsInboundConn() ? ConnectionDirection::In : ConnectionDirection::Out)) {
            nNum++;
        }
    }

    return nNum;
}

void CConnman::GetNodeStats(std::vector<CNodeStats>& vstats) const
{
    vstats.clear();
    LOCK(m_nodes_mutex);
    vstats.reserve(m_nodes.size());
    for (CNode* pnode : m_nodes) {
        vstats.emplace_back();
        pnode->CopyStats(vstats.back());
        vstats.back().m_mapped_as = pnode->addr.GetMappedAS(addrman.GetAsmap());
    }
}

bool CConnman::DisconnectNode(const std::string& strNode)
{
    LOCK(m_nodes_mutex);
    if (CNode* pnode = FindNode(strNode)) {
        LogPrintf("disconnect by address%s matched; disconnecting peer=%d\n", (fLogIPs ? strprintf("=%s", strNode) : ""), pnode->GetId());
        pnode->fDisconnect = true;
        return true;
    }
    return false;
}

bool CConnman::DisconnectNode(const CSubNet& subnet)
{
    bool disconnected = false;
    LOCK(m_nodes_mutex);
    for (CNode* pnode : m_nodes) {
        if (subnet.Match(pnode->addr)) {
            LogPrintf("disconnect by subnet%s matched; disconnecting peer=%d\n", (fLogIPs ? strprintf("=%s", subnet.ToString()) : ""), pnode->GetId());
            pnode->fDisconnect = true;
            disconnected = true;
        }
    }
    return disconnected;
}

bool CConnman::DisconnectNode(const CNetAddr& addr)
{
    return DisconnectNode(CSubNet(addr));
}

bool CConnman::DisconnectNode(NodeId id)
{
    LOCK(m_nodes_mutex);
    for(CNode* pnode : m_nodes) {
        if (id == pnode->GetId()) {
            LogPrintf("disconnect by id peer=%d; disconnecting\n", pnode->GetId());
            pnode->fDisconnect = true;
            return true;
        }
    }
    return false;
}

void CConnman::RecordBytesRecv(uint64_t bytes)
{
    nTotalBytesRecv += bytes;
}

void CConnman::RecordBytesSent(uint64_t bytes)
{
    LOCK(cs_totalBytesSent);
    nTotalBytesSent += bytes;

    const auto now = GetTime<std::chrono::seconds>();
    if (nMaxOutboundCycleStartTime + MAX_UPLOAD_TIMEFRAME < now)
    {
        // timeframe expired, reset cycle
        nMaxOutboundCycleStartTime = now;
        nMaxOutboundTotalBytesSentInCycle = 0;
    }

    nMaxOutboundTotalBytesSentInCycle += bytes;
}

uint64_t CConnman::GetMaxOutboundTarget() const
{
    LOCK(cs_totalBytesSent);
    return nMaxOutboundLimit;
}

std::chrono::seconds CConnman::GetMaxOutboundTimeframe() const
{
    return MAX_UPLOAD_TIMEFRAME;
}

std::chrono::seconds CConnman::GetMaxOutboundTimeLeftInCycle() const
{
    LOCK(cs_totalBytesSent);
    if (nMaxOutboundLimit == 0)
        return 0s;

    if (nMaxOutboundCycleStartTime.count() == 0)
        return MAX_UPLOAD_TIMEFRAME;

    const std::chrono::seconds cycleEndTime = nMaxOutboundCycleStartTime + MAX_UPLOAD_TIMEFRAME;
    const auto now = GetTime<std::chrono::seconds>();
    return (cycleEndTime < now) ? 0s : cycleEndTime - now;
}

bool CConnman::OutboundTargetReached(bool historicalBlockServingLimit) const
{
    LOCK(cs_totalBytesSent);
    if (nMaxOutboundLimit == 0)
        return false;

    if (historicalBlockServingLimit)
    {
        // keep a large enough buffer to at least relay each block once
        const std::chrono::seconds timeLeftInCycle = GetMaxOutboundTimeLeftInCycle();
        const uint64_t buffer = timeLeftInCycle / std::chrono::minutes{10} * MAX_BLOCK_SERIALIZED_SIZE;
        if (buffer >= nMaxOutboundLimit || nMaxOutboundTotalBytesSentInCycle >= nMaxOutboundLimit - buffer)
            return true;
    }
    else if (nMaxOutboundTotalBytesSentInCycle >= nMaxOutboundLimit)
        return true;

    return false;
}

uint64_t CConnman::GetOutboundTargetBytesLeft() const
{
    LOCK(cs_totalBytesSent);
    if (nMaxOutboundLimit == 0)
        return 0;

    return (nMaxOutboundTotalBytesSentInCycle >= nMaxOutboundLimit) ? 0 : nMaxOutboundLimit - nMaxOutboundTotalBytesSentInCycle;
}

uint64_t CConnman::GetTotalBytesRecv() const
{
    return nTotalBytesRecv;
}

uint64_t CConnman::GetTotalBytesSent() const
{
    LOCK(cs_totalBytesSent);
    return nTotalBytesSent;
}

void CConnman::SetTotalBytesRecv(uint64_t bytes) { nTotalBytesRecv.store(bytes); }

void CConnman::SetTotalBytesSent(uint64_t bytes)
{
    LOCK(cs_totalBytesSent);
    nTotalBytesSent = bytes;
}

ServiceFlags CConnman::GetLocalServices() const
{
    return nLocalServices;
}

unsigned int CConnman::GetReceiveFloodSize() const { return nReceiveFloodSize; }

CNode::CNode(NodeId idIn, ServiceFlags nLocalServicesIn, std::shared_ptr<Sock> sock, const CAddress& addrIn, uint64_t nKeyedNetGroupIn, uint64_t nLocalHostNonceIn, const CAddress& addrBindIn, const std::string& addrNameIn, ConnectionType conn_type_in, bool inbound_onion)
    : m_sock{sock},
      m_connected{GetTime<std::chrono::seconds>()},
      addr(addrIn),
      addrBind(addrBindIn),
      m_addr_name{addrNameIn.empty() ? addr.ToStringIPPort() : addrNameIn},
      m_inbound_onion(inbound_onion),
      nKeyedNetGroup(nKeyedNetGroupIn),
      id(idIn),
      nLocalHostNonce(nLocalHostNonceIn),
      m_conn_type(conn_type_in),
      nLocalServices(nLocalServicesIn)
{
    if (inbound_onion) assert(conn_type_in == ConnectionType::INBOUND);
    if (conn_type_in != ConnectionType::BLOCK_RELAY) {
        m_tx_relay = std::make_unique<TxRelay>();
    }

    for (const std::string &msg : getAllNetMessageTypes())
        mapRecvBytesPerMsgCmd[msg] = 0;
    mapRecvBytesPerMsgCmd[NET_MESSAGE_COMMAND_OTHER] = 0;

    if (fLogIPs) {
        LogPrint(BCLog::NET, "Added connection to %s peer=%d\n", m_addr_name, id);
    } else {
        LogPrint(BCLog::NET, "Added connection peer=%d\n", id);
    }

    m_deserializer = std::make_unique<V1TransportDeserializer>(V1TransportDeserializer(Params(), id, SER_NETWORK, INIT_PROTO_VERSION));
    m_serializer = std::make_unique<V1TransportSerializer>(V1TransportSerializer());
}

bool CConnman::NodeFullyConnected(const CNode* pnode)
{
    return pnode && pnode->fSuccessfullyConnected && !pnode->fDisconnect;
}

void CConnman::PushMessage(CNode* pnode, CSerializedNetMsg&& msg)
{
    size_t nMessageSize = msg.data.size();
    LogPrint(BCLog::NET, "sending %s (%d bytes) peer=%d\n", msg.m_type, nMessageSize, pnode->GetId());
    if (gArgs.GetBoolArg("-capturemessages", false)) {
        CaptureMessage(pnode->addr, msg.m_type, msg.data, /* incoming traffic */ false);
    }

    TRACE6(net, outbound_message,
        pnode->GetId(),
        pnode->m_addr_name.c_str(),
        pnode->ConnectionTypeAsString().c_str(),
        msg.m_type.c_str(),
        msg.data.size(),
        msg.data.data()
    );

    // make sure we use the appropriate network transport format
    std::vector<unsigned char> serializedHeader;
    pnode->m_serializer->prepareForTransport(msg, serializedHeader);
    size_t nTotalSize = nMessageSize + serializedHeader.size();

    size_t nBytesSent = 0;
    {
        LOCK(pnode->cs_vSend);
        bool optimisticSend(pnode->vSendMsg.empty());

        // Track queued bytes by message type; actual sent bytes are accounted in SocketSendData().
        pnode->m_send_msg_cmd_sizes.emplace_back(msg.m_type, nTotalSize);
        pnode->nSendSize += nTotalSize;

        if (pnode->nSendSize > nSendBufferMaxSize) pnode->fPauseSend = true;
        pnode->vSendMsg.push_back(std::move(serializedHeader));
        if (nMessageSize) pnode->vSendMsg.push_back(std::move(msg.data));

        // If write queue empty, attempt "optimistic write"
        if (optimisticSend) nBytesSent = SocketSendData(*pnode);
    }
    if (nBytesSent) RecordBytesSent(nBytesSent);
}

bool CConnman::ForNode(NodeId id, std::function<bool(CNode* pnode)> func)
{
    CNode* found = nullptr;
    LOCK(m_nodes_mutex);
    for (auto&& pnode : m_nodes) {
        if(pnode->GetId() == id) {
            found = pnode;
            break;
        }
    }
    return found != nullptr && NodeFullyConnected(found) && func(found);
}

CSipHasher CConnman::GetDeterministicRandomizer(uint64_t id) const
{
    return CSipHasher(nSeed0, nSeed1).Write(id);
}

uint64_t CConnman::CalculateKeyedNetGroup(const CAddress& ad) const
{
    std::vector<unsigned char> vchNetGroup(ad.GetGroup(addrman.GetAsmap()));

    return GetDeterministicRandomizer(RANDOMIZER_ID_NETGROUP).Write(vchNetGroup.data(), vchNetGroup.size()).Finalize();
}

void CaptureMessage(const CAddress& addr, const std::string& msg_type, const Span<const unsigned char>& data, bool is_incoming)
{
    // Note: This function captures the message at the time of processing,
    // not at socket receive/send time.
    // This ensures that the messages are always in order from an application
    // layer (processing) perspective.
    auto now = GetTime<std::chrono::microseconds>();

    // Windows folder names cannot include a colon
    std::string clean_addr = addr.ToString();
    std::replace(clean_addr.begin(), clean_addr.end(), ':', '_');

    fs::path base_path = gArgs.GetDataDirNet() / "message_capture" / clean_addr;
    fs::create_directories(base_path);

    fs::path path = base_path / (is_incoming ? "msgs_recv.dat" : "msgs_sent.dat");
    CAutoFile f(fsbridge::fopen(path, "ab"), SER_DISK, CLIENT_VERSION);

    ser_writedata64(f, now.count());
    f.write(MakeByteSpan(msg_type));
    for (auto i = msg_type.length(); i < CMessageHeader::COMMAND_SIZE; ++i) {
        f << uint8_t{'\0'};
    }
    uint32_t size = data.size();
    ser_writedata32(f, size);
    f.write(AsBytes(data));
}
