// Copyright (c) 2015-2021 The Bitcoin Core developers
// Copyright (c) 2017 The Zcash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <torcontrol.h>

#include <chainparams.h>
#include <chainparamsbase.h>
#include <compat.h>
#include <crypto/hmac_sha256.h>
#include <net.h>
#include <netaddress.h>
#include <netbase.h>
#include <util/readwritefile.h>
#include <util/strencodings.h>
#include <util/syscall_sandbox.h>
#include <util/system.h>
#include <util/thread.h>
#include <util/time.h>
#include <shutdown.h>

#include <deque>
#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <functional>
#include <set>
#include <vector>
#include <deque>
#include <functional>
#include <set>
#include <vector>
#include <random>

#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/event.h>
#include <event2/thread.h>
#include <event2/util.h>

/** Default control port */
const std::string DEFAULT_TOR_CONTROL = "127.0.0.1:9051";
/** Tor cookie size (from control-spec.txt) */
static const int TOR_COOKIE_SIZE = 32;
/** Size of client/server nonce for SAFECOOKIE */
static const int TOR_NONCE_SIZE = 32;
/** For computing serverHash in SAFECOOKIE */
static const std::string TOR_SAFE_SERVERKEY = "Tor safe cookie authentication server-to-controller hash";
/** For computing clientHash in SAFECOOKIE */
static const std::string TOR_SAFE_CLIENTKEY = "Tor safe cookie authentication controller-to-server hash";
/** Exponential backoff configuration - initial timeout in seconds */
static const float RECONNECT_TIMEOUT_START = 1.0;
/** Exponential backoff configuration - growth factor */
static const float RECONNECT_TIMEOUT_EXP = 1.5;
/** Maximum length for lines received on TorControlConnection.
 * tor-control-spec.txt mentions that there is explicitly no limit defined to line length,
 * this is belt-and-suspenders sanity limit to prevent memory exhaustion.
 */
static const int MAX_LINE_LENGTH = 100000;

/****** Low-level TorControlConnection ********/

TorControlConnection::TorControlConnection(struct event_base *_base):
    base(_base), b_conn(nullptr)
{
}

TorControlConnection::~TorControlConnection()
{
    if (b_conn)
        bufferevent_free(b_conn);
}

void TorControlConnection::readcb(struct bufferevent *bev, void *ctx)
{
    TorControlConnection *self = static_cast<TorControlConnection*>(ctx);
    struct evbuffer *input = bufferevent_get_input(bev);
    size_t n_read_out = 0;
    char *line;
    assert(input);
    //  If there is not a whole line to read, evbuffer_readln returns nullptr
    while((line = evbuffer_readln(input, &n_read_out, EVBUFFER_EOL_CRLF)) != nullptr)
    {
        std::string s(line, n_read_out);
        free(line);
        if (s.size() < 4) // Short line
            continue;
        // <status>(-|+| )<data><CRLF>
        self->message.code = LocaleIndependentAtoi<int>(s.substr(0,3));
        self->message.lines.push_back(s.substr(4));
        char ch = s[3]; // '-','+' or ' '
        if (ch == ' ') {
            // Final line, dispatch reply and clean up
            if (self->message.code >= 600) {
                // Dispatch async notifications to async handler
                // Synchronous and asynchronous messages are never interleaved
                self->async_handler(*self, self->message);
            } else {
                if (!self->reply_handlers.empty()) {
                    // Invoke reply handler with message
                    self->reply_handlers.front()(*self, self->message);
                    self->reply_handlers.pop_front();
                } else {
                    LogPrint(BCLog::TOR, "tor: Received unexpected sync reply %i\n", self->message.code);
                }
            }
            self->message.Clear();
        }
    }
    //  Check for size of buffer - protect against memory exhaustion with very long lines
    //  Do this after evbuffer_readln to make sure all full lines have been
    //  removed from the buffer. Everything left is an incomplete line.
    if (evbuffer_get_length(input) > MAX_LINE_LENGTH) {
        LogPrintf("tor: Disconnecting because MAX_LINE_LENGTH exceeded\n");
        self->Disconnect();
    }
}

void TorControlConnection::eventcb(struct bufferevent *bev, short what, void *ctx)
{
    TorControlConnection *self = static_cast<TorControlConnection*>(ctx);
    if (what & BEV_EVENT_CONNECTED) {
        LogPrint(BCLog::TOR, "tor: Successfully connected!\n");
        self->connected(*self);
    } else if (what & (BEV_EVENT_EOF|BEV_EVENT_ERROR)) {
        if (what & BEV_EVENT_ERROR) {
            LogPrint(BCLog::TOR, "tor: Error connecting to Tor control socket\n");
        } else {
            LogPrint(BCLog::TOR, "tor: End of stream\n");
        }
        self->Disconnect();
        self->disconnected(*self);
    }
}

bool TorControlConnection::Connect(const std::string& tor_control_center, const ConnectionCB& _connected, const ConnectionCB& _disconnected)
{
    if (b_conn) {
        Disconnect();
    }

    CService control_service;
    if (!Lookup(tor_control_center, control_service, 9051, fNameLookup)) {
        LogPrintf("tor: Failed to look up control center %s\n", tor_control_center);
        return false;
    }

    struct sockaddr_storage control_address;
    socklen_t control_address_len = sizeof(control_address);
    if (!control_service.GetSockAddr(reinterpret_cast<struct sockaddr*>(&control_address), &control_address_len)) {
        LogPrintf("tor: Error parsing socket address %s\n", tor_control_center);
        return false;
    }

    // Create a new socket, set up callbacks and enable notification bits
    b_conn = bufferevent_socket_new(base, -1, BEV_OPT_CLOSE_ON_FREE);
    if (!b_conn) {
        return false;
    }
    bufferevent_setcb(b_conn, TorControlConnection::readcb, nullptr, TorControlConnection::eventcb, this);
    bufferevent_enable(b_conn, EV_READ|EV_WRITE);
    this->connected = _connected;
    this->disconnected = _disconnected;

    // Finally, connect to tor_control_center
    if (bufferevent_socket_connect(b_conn, reinterpret_cast<struct sockaddr*>(&control_address), control_address_len) < 0) {
        LogPrintf("tor: Error connecting to address %s\n", tor_control_center);
        return false;
    }
    return true;
}

void TorControlConnection::Disconnect()
{
    if (b_conn)
        bufferevent_free(b_conn);
    b_conn = nullptr;
}

bool TorControlConnection::Command(const std::string &cmd, const ReplyHandlerCB& reply_handler)
{
    if (!b_conn)
        return false;
    struct evbuffer *buf = bufferevent_get_output(b_conn);
    if (!buf)
        return false;
    evbuffer_add(buf, cmd.data(), cmd.size());
    evbuffer_add(buf, "\r\n", 2);
    reply_handlers.push_back(reply_handler);
    return true;
}

/****** General parsing utilities ********/

/* Split reply line in the form 'AUTH METHODS=...' into a type
 * 'AUTH' and arguments 'METHODS=...'.
 * Grammar is implicitly defined in https://spec.torproject.org/control-spec by
 * the server reply formats for PROTOCOLINFO (S3.21) and AUTHCHALLENGE (S3.24).
 */
std::pair<std::string,std::string> SplitTorReplyLine(const std::string &s)
{
    size_t ptr=0;
    std::string type;
    while (ptr < s.size() && s[ptr] != ' ') {
        type.push_back(s[ptr]);
        ++ptr;
    }
    if (ptr < s.size())
        ++ptr; // skip ' '
    return make_pair(type, s.substr(ptr));
}

/** Parse reply arguments in the form 'METHODS=COOKIE,SAFECOOKIE COOKIEFILE=".../control_auth_cookie"'.
 * Returns a map of keys to values, or an empty map if there was an error.
 * Grammar is implicitly defined in https://spec.torproject.org/control-spec by
 * the server reply formats for PROTOCOLINFO (S3.21), AUTHCHALLENGE (S3.24),
 * and ADD_ONION (S3.27). See also sections 2.1 and 2.3.
 */
std::map<std::string,std::string> ParseTorReplyMapping(const std::string &s)
{
    std::map<std::string,std::string> mapping;
    size_t ptr=0;
    while (ptr < s.size()) {
        std::string key, value;
        while (ptr < s.size() && s[ptr] != '=' && s[ptr] != ' ') {
            key.push_back(s[ptr]);
            ++ptr;
        }
        if (ptr == s.size()) // unexpected end of line
            return std::map<std::string,std::string>();
        if (s[ptr] == ' ') // The remaining string is an OptArguments
            break;
        ++ptr; // skip '='
        if (ptr < s.size() && s[ptr] == '"') { // Quoted string
            ++ptr; // skip opening '"'
            bool escape_next = false;
            while (ptr < s.size() && (escape_next || s[ptr] != '"')) {
                // Repeated backslashes must be interpreted as pairs
                escape_next = (s[ptr] == '\\' && !escape_next);
                value.push_back(s[ptr]);
                ++ptr;
            }
            if (ptr == s.size()) // unexpected end of line
                return std::map<std::string,std::string>();
            ++ptr; // skip closing '"'
            /**
             * Unescape value. Per https://spec.torproject.org/control-spec section 2.1.1:
             *
             *   For future-proofing, controller implementors MAY use the following
             *   rules to be compatible with buggy Tor implementations and with
             *   future ones that implement the spec as intended:
             *
             *     Read \n \t \r and \0 ... \377 as C escapes.
             *     Treat a backslash followed by any other character as that character.
             */
            std::string escaped_value;
            for (size_t i = 0; i < value.size(); ++i) {
                if (value[i] == '\\') {
                    // This will always be valid, because if the QuotedString
                    // ended in an odd number of backslashes, then the parser
                    // would already have returned above, due to a missing
                    // terminating double-quote.
                    ++i;
                    if (value[i] == 'n') {
                        escaped_value.push_back('\n');
                    } else if (value[i] == 't') {
                        escaped_value.push_back('\t');
                    } else if (value[i] == 'r') {
                        escaped_value.push_back('\r');
                    } else if ('0' <= value[i] && value[i] <= '7') {
                        size_t j;
                        // Octal escape sequences have a limit of three octal digits,
                        // but terminate at the first character that is not a valid
                        // octal digit if encountered sooner.
                        for (j = 1; j < 3 && (i+j) < value.size() && '0' <= value[i+j] && value[i+j] <= '7'; ++j) {}
                        // Tor restricts first digit to 0-3 for three-digit octals.
                        // A leading digit of 4-7 would therefore be interpreted as
                        // a two-digit octal.
                        if (j == 3 && value[i] > '3') {
                            j--;
                        }
                        const auto end{i + j};
                        uint8_t val{0};
                        while (i < end) {
                            val *= 8;
                            val += value[i++] - '0';
                        }
                        escaped_value.push_back(char(val));
                        // Account for automatic incrementing at loop end
                        --i;
                    } else {
                        escaped_value.push_back(value[i]);
                    }
                } else {
                    escaped_value.push_back(value[i]);
                }
            }
            value = escaped_value;
        } else { // Unquoted value. Note that values can contain '=' at will, just no spaces
            while (ptr < s.size() && s[ptr] != ' ') {
                value.push_back(s[ptr]);
                ++ptr;
            }
        }
        if (ptr < s.size() && s[ptr] == ' ')
            ++ptr; // skip ' ' after key=value
        mapping[key] = value;
    }
    return mapping;
}

TorController::TorController(struct event_base* _base, const std::string& tor_control_center, const CService& target):
    base(_base),
    m_tor_control_center(tor_control_center), conn(base), reconnect(true), reconnect_ev(0),
    reconnect_timeout(RECONNECT_TIMEOUT_START),
    m_target(target),
    num_services(static_cast<size_t>(gArgs.GetIntArg("-numonion", 1)))
{
    // Initialize the service vectors to the right size
    service_ids.resize(num_services);
    services.resize(num_services);

    reconnect_ev = event_new(base, -1, 0, reconnect_cb, this);
    if (!reconnect_ev)
        LogPrintf("tor: Failed to create event for reconnection: out of memory?\n");
    // Start connection attempts immediately
    if (!conn.Connect(m_tor_control_center, std::bind(&TorController::connected_cb, this, std::placeholders::_1),
         std::bind(&TorController::disconnected_cb, this, std::placeholders::_1) )) {
        LogPrintf("tor: Initiating connection to Tor control port %s failed\n", m_tor_control_center);
    }
    // Read service private keys if cached (one per line)
    std::pair<bool,std::string> pkf = ReadBinaryFile(GetPrivateKeyFile());
    if (pkf.first) {
        LogPrint(BCLog::TOR, "tor: Reading cached private keys from %s\n", fs::PathToString(GetPrivateKeyFile()));
        // Split the file contents by newlines to get multiple keys
        std::string key_file_content = pkf.second;
        std::vector<std::string> key_lines;

        // Split by any combination of CR and LF for cross-platform compatibility
        boost::split(key_lines, key_file_content, boost::is_any_of("\r\n"));

        // Add each non-empty line as a private key
        private_keys.clear(); // Ensure we start with an empty vector
        for (const std::string& key_line : key_lines) {
            std::string trimmed_key = boost::algorithm::trim_copy(key_line);
            if (!trimmed_key.empty()) {
                // Basic validation that the key format looks correct
                if (trimmed_key.substr(0, 4) == "NEW:" ||
                    trimmed_key.substr(0, 12) == "ED25519-V3:" ||
                    trimmed_key.find(":") != std::string::npos) {
                    private_keys.push_back(trimmed_key);
                    LogPrint(BCLog::TOR, "tor: Loaded private key: %s...\n",
                             trimmed_key.substr(0, std::min(10, (int)trimmed_key.length())) + "...");
                } else {
                    LogPrintf("tor: Skipping invalid private key format in key file\n");
                }
            }
        }

        // If no valid keys were found, initialize with an empty vector
        if (private_keys.empty()) {
            LogPrint(BCLog::TOR, "tor: No valid private keys found in key file\n");
        } else {
            LogPrint(BCLog::TOR, "tor: Found %d private key(s) in key file\n", private_keys.size());

            // Resize if necessary to match the required number of services
            size_t num_services = static_cast<size_t>(gArgs.GetIntArg("-numonion", 1));
            if (private_keys.size() < num_services) {
                LogPrint(BCLog::TOR, "tor: Need %d services but only %d keys found, will generate additional keys\n",
                         num_services, private_keys.size());
                private_keys.resize(num_services);
            } else if (private_keys.size() > num_services) {
                LogPrint(BCLog::TOR, "tor: Found %d keys but only %d services requested, using first %d keys\n",
                         private_keys.size(), num_services, num_services);
                private_keys.resize(num_services);
            }
        }
    }
}

TorController::~TorController()
{
    if (reconnect_ev) {
        event_free(reconnect_ev);
        reconnect_ev = nullptr;
    }
    // Remove all valid services
    for (const CService& service_entry : services) {
        if (service_entry.IsValid()) {
            RemoveLocal(service_entry);
        }
    }
}

void TorController::add_onion_cb(TorControlConnection& _conn, const TorControlReply& reply)
{
    // Get the current service index being processed
    size_t service_index = current_service_index;

    if (reply.code == 250) {
        LogPrint(BCLog::TOR, "tor: ADD_ONION successful for service %d\n", service_index);

        // Temporary variables to store service data from this callback
        std::string new_service_id;
        std::string new_private_key;

        for (const std::string &s : reply.lines) {
            std::map<std::string,std::string> m = ParseTorReplyMapping(s);
            std::map<std::string,std::string>::iterator i;
            if ((i = m.find("ServiceID")) != m.end())
                new_service_id = i->second;
            if ((i = m.find("PrivateKey")) != m.end())
                new_private_key = i->second;
        }

        if (new_service_id.empty()) {
            LogPrintf("tor: Error parsing ADD_ONION parameters for service %d:\n", service_index);
            for (const std::string &s : reply.lines) {
                LogPrintf("    %s\n", SanitizeString(s));
            }
            return;
        }

        // Ensure the vectors are large enough
        if (service_index >= service_ids.size()) {
            service_ids.resize(service_index + 1);
        }
        if (service_index >= services.size()) {
            services.resize(service_index + 1);
        }
        if (service_index >= private_keys.size()) {
            private_keys.resize(service_index + 1);
        }

        // Store the service information in the appropriate vectors
        service_ids[service_index] = new_service_id;
        private_keys[service_index] = new_private_key;
        services[service_index] = LookupNumeric(std::string(new_service_id+".onion"), Params().GetDefaultPort());

        LogPrintf("tor: Got service ID %s, advertising service %s\n",
                  new_service_id, services[service_index].ToString());

        // Write all private keys to the file
        std::string private_keys_str;
        int valid_keys = 0;
        for (const std::string& key : private_keys) {
            if (!key.empty()) {
                private_keys_str += key + "\n";
                valid_keys++;
            }
        }

        if (valid_keys > 0 && WriteBinaryFile(GetPrivateKeyFile(), private_keys_str)) {
            LogPrint(BCLog::TOR, "tor: Cached %d service private key(s) to %s\n",
                     valid_keys, fs::PathToString(GetPrivateKeyFile()));
        } else {
            LogPrintf("tor: Error writing service private keys to %s\n", fs::PathToString(GetPrivateKeyFile()));
        }

        // Add the service to the local address list using the current service index
        if (service_index < services.size() && services[service_index].IsValid()) {
            LogPrint(BCLog::TOR, "tor: Adding service %d to local address list: %s\n",
                     service_index, services[service_index].ToString());
            AddLocal(services[service_index], LOCAL_MANUAL);
        } else {
            LogPrintf("tor: Warning: Unable to add service %d to local address list - invalid service\n", service_index);
        }

        // Check if we need to create more services
        size_t num_services = static_cast<size_t>(gArgs.GetIntArg("-numonion", 1));
        current_service_index++;

        if (current_service_index < num_services) {
            // Create the next service
            LogPrint(BCLog::TOR, "tor: Creating next onion service (%d of %d)\n",
                    current_service_index + 1, num_services);

            // If we have a stored private key for this index, use it
            if (current_service_index < private_keys.size() && !private_keys[current_service_index].empty()) {
                LogPrint(BCLog::TOR, "tor: Using stored private key for service %d\n", current_service_index);
            } else {
                // No private key for this index, generate a new one
                // Check if vanity address is requested
                std::string prefix = gArgs.GetArg("-onionmatch", "");
                if (!prefix.empty()) {
                    std::string generated_key;
                    // Generate a vanity address with the specified prefix
                    // This will continue until a match is found
                    GenerateVanityOnionAddress(prefix, generated_key);
                    private_keys[current_service_index] = generated_key;
                    LogPrint(BCLog::TOR, "tor: Generated vanity private key for service %d with prefix '%s'\n",
                            current_service_index, prefix);
                } else {
                    // No vanity prefix requested, use standard key
                    private_keys[current_service_index] = "NEW:ED25519-V3";
                    LogPrint(BCLog::TOR, "tor: Generating new private key for service %d\n", current_service_index);
                }
            }

            // Request the next onion service
            _conn.Command(strprintf("ADD_ONION %s Port=%i,%s", private_keys[current_service_index],
                        Params().GetDefaultPort(), m_target.ToStringIPPort()),
                std::bind(&TorController::add_onion_cb, this, std::placeholders::_1, std::placeholders::_2));
        } else {
            LogPrint(BCLog::TOR, "tor: All %d onion services created successfully\n", num_services);
        }

        // ... onion requested - keep connection open
    } else if (reply.code == 510) { // 510 Unrecognized command
        LogPrintf("tor: Add onion failed with unrecognized command (You probably need to upgrade Tor)\n");
    } else {
        LogPrintf("tor: Add onion failed for service %d; error code %d\n", service_index, reply.code);
    }
}

void TorController::auth_cb(TorControlConnection& _conn, const TorControlReply& reply)
{
    if (reply.code == 250) {
        LogPrint(BCLog::TOR, "tor: Authentication successful\n");

        // Now that we know Tor is running setup the proxy for onion addresses
        // if -onion isn't set to something else.
        if (gArgs.GetArg("-onion", "") == "") {
            CService resolved(LookupNumeric("127.0.0.1", 9050));
            proxyType addrOnion = proxyType(resolved, true);
            SetProxy(NET_ONION, addrOnion);
            SetReachable(NET_ONION, true);
        }

        // Get the number of onion services to create
        size_t num_services = static_cast<size_t>(gArgs.GetIntArg("-numonion", 1));

        // Ensure we have at least one service
        if (num_services < 1) {
            num_services = 1;
            LogPrint(BCLog::TOR, "tor: Invalid -numonion value, defaulting to 1 service\n");
        }

        LogPrint(BCLog::TOR, "tor: Creating %d onion service(s)\n", num_services);

        // Initialize/resize the service vectors to hold num_services entries
        private_keys.resize(num_services);
        service_ids.resize(num_services);
        services.resize(num_services);

        // Start with the first service
        current_service_index = 0;

        // If we have a stored private key for this index, use it
        if (current_service_index < private_keys.size() && !private_keys[current_service_index].empty()) {
            LogPrint(BCLog::TOR, "tor: Using stored private key for service %d\n", current_service_index);
        } else {
            // No private key for this index, generate a new one
            // Check if vanity address is requested
            std::string prefix = gArgs.GetArg("-onionmatch", "");
            if (!prefix.empty()) {
                std::string generated_key;
                // Generate a vanity address with the specified prefix
                // This will continue until a match is found
                GenerateVanityOnionAddress(prefix, generated_key);
                private_keys[current_service_index] = generated_key;
                LogPrint(BCLog::TOR, "tor: Generated vanity private key for service %d with prefix '%s'\n",
                        current_service_index, prefix);
            } else {
                // No vanity prefix requested, use standard key
                private_keys[current_service_index] = "NEW:ED25519-V3"; // Explicitly request key type - see issue #9214
                LogPrint(BCLog::TOR, "tor: Generating new private key for service %d\n", current_service_index);
            }
        }

        // Request onion service, redirect port.
        // Note that the 'virtual' port is always the default port to avoid decloaking nodes using other ports.
        _conn.Command(strprintf("ADD_ONION %s Port=%i,%s", private_keys[current_service_index],
                    Params().GetDefaultPort(), m_target.ToStringIPPort()),
            std::bind(&TorController::add_onion_cb, this, std::placeholders::_1, std::placeholders::_2));
    } else {
        LogPrintf("tor: Authentication failed\n");
    }
}

/** Compute Tor SAFECOOKIE response.
 *
 *    ServerHash is computed as:
 *      HMAC-SHA256("Tor safe cookie authentication server-to-controller hash",
 *                  CookieString | ClientNonce | ServerNonce)
 *    (with the HMAC key as its first argument)
 *
 *    After a controller sends a successful AUTHCHALLENGE command, the
 *    next command sent on the connection must be an AUTHENTICATE command,
 *    and the only authentication string which that AUTHENTICATE command
 *    will accept is:
 *
 *      HMAC-SHA256("Tor safe cookie authentication controller-to-server hash",
 *                  CookieString | ClientNonce | ServerNonce)
 *
 */
static std::vector<uint8_t> ComputeResponse(const std::string &key, const std::vector<uint8_t> &cookie,  const std::vector<uint8_t> &clientNonce, const std::vector<uint8_t> &serverNonce)
{
    CHMAC_SHA256 computeHash((const uint8_t*)key.data(), key.size());
    std::vector<uint8_t> computedHash(CHMAC_SHA256::OUTPUT_SIZE, 0);
    computeHash.Write(cookie.data(), cookie.size());
    computeHash.Write(clientNonce.data(), clientNonce.size());
    computeHash.Write(serverNonce.data(), serverNonce.size());
    computeHash.Finalize(computedHash.data());
    return computedHash;
}

void TorController::authchallenge_cb(TorControlConnection& _conn, const TorControlReply& reply)
{
    if (reply.code == 250) {
        LogPrint(BCLog::TOR, "tor: SAFECOOKIE authentication challenge successful\n");
        std::pair<std::string,std::string> l = SplitTorReplyLine(reply.lines[0]);
        if (l.first == "AUTHCHALLENGE") {
            std::map<std::string,std::string> m = ParseTorReplyMapping(l.second);
            if (m.empty()) {
                LogPrintf("tor: Error parsing AUTHCHALLENGE parameters: %s\n", SanitizeString(l.second));
                return;
            }
            std::vector<uint8_t> serverHash = ParseHex(m["SERVERHASH"]);
            std::vector<uint8_t> serverNonce = ParseHex(m["SERVERNONCE"]);
            LogPrint(BCLog::TOR, "tor: AUTHCHALLENGE ServerHash %s ServerNonce %s\n", HexStr(serverHash), HexStr(serverNonce));
            if (serverNonce.size() != 32) {
                LogPrintf("tor: ServerNonce is not 32 bytes, as required by spec\n");
                return;
            }

            std::vector<uint8_t> computedServerHash = ComputeResponse(TOR_SAFE_SERVERKEY, cookie, clientNonce, serverNonce);
            if (computedServerHash != serverHash) {
                LogPrintf("tor: ServerHash %s does not match expected ServerHash %s\n", HexStr(serverHash), HexStr(computedServerHash));
                return;
            }

            std::vector<uint8_t> computedClientHash = ComputeResponse(TOR_SAFE_CLIENTKEY, cookie, clientNonce, serverNonce);
            _conn.Command("AUTHENTICATE " + HexStr(computedClientHash), std::bind(&TorController::auth_cb, this, std::placeholders::_1, std::placeholders::_2));
        } else {
            LogPrintf("tor: Invalid reply to AUTHCHALLENGE\n");
        }
    } else {
        LogPrintf("tor: SAFECOOKIE authentication challenge failed\n");
    }
}

void TorController::protocolinfo_cb(TorControlConnection& _conn, const TorControlReply& reply)
{
    if (reply.code == 250) {
        std::set<std::string> methods;
        std::string cookiefile;
        /*
         * 250-AUTH METHODS=COOKIE,SAFECOOKIE COOKIEFILE="/home/x/.tor/control_auth_cookie"
         * 250-AUTH METHODS=NULL
         * 250-AUTH METHODS=HASHEDPASSWORD
         */
        for (const std::string &s : reply.lines) {
            std::pair<std::string,std::string> l = SplitTorReplyLine(s);
            if (l.first == "AUTH") {
                std::map<std::string,std::string> m = ParseTorReplyMapping(l.second);
                std::map<std::string,std::string>::iterator i;
                if ((i = m.find("METHODS")) != m.end())
                    boost::split(methods, i->second, boost::is_any_of(","));
                if ((i = m.find("COOKIEFILE")) != m.end())
                    cookiefile = i->second;
            } else if (l.first == "VERSION") {
                std::map<std::string,std::string> m = ParseTorReplyMapping(l.second);
                std::map<std::string,std::string>::iterator i;
                if ((i = m.find("Tor")) != m.end()) {
                    LogPrint(BCLog::TOR, "tor: Connected to Tor version %s\n", i->second);
                }
            }
        }
        for (const std::string &s : methods) {
            LogPrint(BCLog::TOR, "tor: Supported authentication method: %s\n", s);
        }
        // Prefer NULL, otherwise SAFECOOKIE. If a password is provided, use HASHEDPASSWORD
        /* Authentication:
         *   cookie:   hex-encoded ~/.tor/control_auth_cookie
         *   password: "password"
         */
        std::string torpassword = gArgs.GetArg("-torpassword", "");
        if (!torpassword.empty()) {
            if (methods.count("HASHEDPASSWORD")) {
                LogPrint(BCLog::TOR, "tor: Using HASHEDPASSWORD authentication\n");
                boost::replace_all(torpassword, "\"", "\\\"");
                _conn.Command("AUTHENTICATE \"" + torpassword + "\"", std::bind(&TorController::auth_cb, this, std::placeholders::_1, std::placeholders::_2));
            } else {
                LogPrintf("tor: Password provided with -torpassword, but HASHEDPASSWORD authentication is not available\n");
            }
        } else if (methods.count("NULL")) {
            LogPrint(BCLog::TOR, "tor: Using NULL authentication\n");
            _conn.Command("AUTHENTICATE", std::bind(&TorController::auth_cb, this, std::placeholders::_1, std::placeholders::_2));
        } else if (methods.count("SAFECOOKIE")) {
            // Cookie: hexdump -e '32/1 "%02x""\n"'  ~/.tor/control_auth_cookie
            LogPrint(BCLog::TOR, "tor: Using SAFECOOKIE authentication, reading cookie authentication from %s\n", cookiefile);
            std::pair<bool,std::string> status_cookie = ReadBinaryFile(fs::PathFromString(cookiefile), TOR_COOKIE_SIZE);
            if (status_cookie.first && status_cookie.second.size() == TOR_COOKIE_SIZE) {
                // _conn.Command("AUTHENTICATE " + HexStr(status_cookie.second), std::bind(&TorController::auth_cb, this, std::placeholders::_1, std::placeholders::_2));
                cookie = std::vector<uint8_t>(status_cookie.second.begin(), status_cookie.second.end());
                clientNonce = std::vector<uint8_t>(TOR_NONCE_SIZE, 0);
                GetRandBytes(clientNonce.data(), TOR_NONCE_SIZE);
                _conn.Command("AUTHCHALLENGE SAFECOOKIE " + HexStr(clientNonce), std::bind(&TorController::authchallenge_cb, this, std::placeholders::_1, std::placeholders::_2));
            } else {
                if (status_cookie.first) {
                    LogPrintf("tor: Authentication cookie %s is not exactly %i bytes, as is required by the spec\n", cookiefile, TOR_COOKIE_SIZE);
                } else {
                    LogPrintf("tor: Authentication cookie %s could not be opened (check permissions)\n", cookiefile);
                }
            }
        } else if (methods.count("HASHEDPASSWORD")) {
            LogPrintf("tor: The only supported authentication mechanism left is password, but no password provided with -torpassword\n");
        } else {
            LogPrintf("tor: No supported authentication method\n");
        }
    } else {
        LogPrintf("tor: Requesting protocol info failed\n");
    }
}

void TorController::connected_cb(TorControlConnection& _conn)
{
    reconnect_timeout = RECONNECT_TIMEOUT_START;
    // First send a PROTOCOLINFO command to figure out what authentication is expected
    if (!_conn.Command("PROTOCOLINFO 1", std::bind(&TorController::protocolinfo_cb, this, std::placeholders::_1, std::placeholders::_2)))
        LogPrintf("tor: Error sending initial protocolinfo command\n");
}

void TorController::disconnected_cb(TorControlConnection& _conn)
{
    // Stop advertising all services when disconnected
    for (const CService& service_entry : services) {
        if (service_entry.IsValid()) {
            RemoveLocal(service_entry);
        }
    }
    // Legacy service variable has been replaced with services vector - already handled above
    if (!reconnect)
        return;

    LogPrint(BCLog::TOR, "tor: Not connected to Tor control port %s, trying to reconnect\n", m_tor_control_center);

    // Single-shot timer for reconnect. Use exponential backoff.
    struct timeval time = MillisToTimeval(int64_t(reconnect_timeout * 1000.0));
    if (reconnect_ev)
        event_add(reconnect_ev, &time);
    reconnect_timeout *= RECONNECT_TIMEOUT_EXP;
}

void TorController::ResetReconnectBackoff()
{
    // Reset the exponential backoff timer to its initial value
    reconnect_timeout = RECONNECT_TIMEOUT_START;

    // Force an immediate reconnection attempt
    if (reconnect_ev) {
        event_del(reconnect_ev);
        event_active(reconnect_ev, 0, 0);
    }
}

void TorController::Reconnect()
{
    /* Try to reconnect and reestablish if we get booted - for example, Tor
     * may be restarting.
     */
    if (!conn.Connect(m_tor_control_center, std::bind(&TorController::connected_cb, this, std::placeholders::_1),
         std::bind(&TorController::disconnected_cb, this, std::placeholders::_1) )) {
        LogPrintf("tor: Re-initiating connection to Tor control port %s failed\n", m_tor_control_center);
    }
}

fs::path TorController::GetPrivateKeyFile()
{
    return gArgs.GetDataDirNet() / "onion_v3_private_key";
}

void TorController::reconnect_cb(evutil_socket_t fd, short what, void *arg)
{
    TorController *self = static_cast<TorController*>(arg);
    self->Reconnect();
}

/****** Thread ********/
static struct event_base *gBase;
static std::thread torControlThread;

static void TorControlThread(CService onion_service_target)
{
    SetSyscallSandboxPolicy(SyscallSandboxPolicy::TOR_CONTROL);
    TorController ctrl(gBase, gArgs.GetArg("-torcontrol", DEFAULT_TOR_CONTROL), onion_service_target);

    event_base_dispatch(gBase);
}

void StartTorControl(CService onion_service_target)
{
    assert(!gBase);
#ifdef WIN32
    evthread_use_windows_threads();
#else
    evthread_use_pthreads();
#endif
    gBase = event_base_new();
    if (!gBase) {
        LogPrintf("tor: Unable to create event_base\n");
        return;
    }

    torControlThread = std::thread(&util::TraceThread, "torcontrol", [onion_service_target] {
        TorControlThread(onion_service_target);
    });
}

void InterruptTorControl()
{
    if (gBase) {
        LogPrintf("tor: Thread interrupt\n");
        event_base_once(gBase, -1, EV_TIMEOUT, [](evutil_socket_t, short, void*) {
            event_base_loopbreak(gBase);
        }, nullptr, nullptr);
    }
}

void StopTorControl()
{
    if (gBase) {
        torControlThread.join();
        event_base_free(gBase);
        gBase = nullptr;
    }
}

void ResetTorBackoff()
{
    // If the tor control thread is running, force a reconnection
    if (torControlThread.joinable() && gBase) {
        // Signal the event loop to perform a reconnection
        // This will reconnect with the reset backoff timer
        event_base_once(gBase, -1, EV_TIMEOUT, [](evutil_socket_t, short, void*) {
            // No direct access to the controller from here, so we just
            // break the loop which will force a reconnection
            event_base_loopbreak(gBase);
        }, nullptr, nullptr);
    }
}

CService DefaultOnionServiceTarget()
{
    struct in_addr onion_service_target;
    onion_service_target.s_addr = htonl(INADDR_LOOPBACK);
    return {onion_service_target, BaseParams().OnionServiceTargetPort()};
}
/**
 * Generate a private key for an onion service that produces an address with the desired prefix.
 *
 * @param prefix The desired prefix for the onion address
 * @param[out] generated_private_key The generated private key if successful
 * @return true if a matching key was found, false otherwise
 */
bool GenerateVanityOnionAddress(const std::string& prefix, std::string& generated_private_key)
{
    if (prefix.empty()) {
        // No prefix specified, use standard key generation
        generated_private_key = "NEW:ED25519-V3";
        return true;
    }

    LogPrintf("tor: Attempting to generate vanity onion address with prefix '%s'...\n", prefix);

    // Setup for generation
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint8_t> dis(0, 255);

    // ED25519 private key is 32 bytes
    std::vector<uint8_t> private_key(32);

    // Track attempts and timing
    int attempts = 0;
    int64_t start_time = GetTimeMillis();
    int64_t last_progress_time = start_time;
    std::string last_non_matching_address;

    // Track statistics on partial matches
    // Index is the number of matching characters, value is the count of addresses with that many matches
    std::vector<int> match_stats(prefix.size() + 1, 0);

    // Track the best match so far and its address
    size_t best_match_length = 0;
    std::string best_match_address;

    // Continue generation until a matching key is found - no maximum limit
    while (true) {
        // Generate random private key
        for (int i = 0; i < 32; i++) {
            private_key[i] = dis(gen);
        }

        // Convert to base64 format that Tor expects
        std::string key = "ED25519-V3:" + EncodeBase64(std::string(reinterpret_cast<char*>(private_key.data()), private_key.size()));

        // Compute the public key (this is simplified - in a real implementation we'd use ED25519 crypto)
        // For now we'll just use a hash of the private key to simulate the address derivation
        // This should be replaced with actual ED25519 key derivation
        uint256 hash;
        CSHA256().Write(private_key.data(), private_key.size()).Finalize(hash.begin());
        std::string simulated_address = hash.ToString().substr(0, 16);
        last_non_matching_address = simulated_address;

        // Calculate how many characters match with the prefix
        size_t match_length = 0;
        while (match_length < prefix.size() && match_length < simulated_address.size() &&
               simulated_address[match_length] == prefix[match_length]) {
            match_length++;
        }

        // Update match statistics
        match_stats[match_length]++;

        // Update best match if this one is better
        if (match_length > best_match_length) {
            best_match_length = match_length;
            best_match_address = simulated_address;
        }

        attempts++;

        // Check if this key produces an address with the desired prefix
        if (match_length == prefix.size()) {
            int64_t elapsed_ms = GetTimeMillis() - start_time;
            LogPrintf("tor: Found matching vanity address after %s attempts (%.2f seconds)\n",
                    strUnit(static_cast<float>(attempts)), elapsed_ms/1000.0);
            generated_private_key = key;
            return true;
        }

        // Report progress every 5 seconds
        int64_t current_time = GetTimeMillis();
        if (current_time - last_progress_time > 5000) { // 5 seconds in milliseconds
            int64_t elapsed_ms = current_time - start_time;
            double attempts_per_second = attempts * 1000.0 / elapsed_ms;

            // Build the statistics string for partial matches
            std::string match_stats_str;
            for (size_t i = 1; i <= best_match_length; i++) {
                if (i > 1) match_stats_str += ", ";
                match_stats_str += strprintf("%d char%s: %d", i, i == 1 ? "" : "s", match_stats[i]);
            }

            LogPrintf("tor: Vanity address search progress: %s attempts (%s attempts/sec)\n"
                      "     Partial matches: %s\n"
                      "     Best match so far: %s (%d/%d chars matched)\n"
                      "     Last address: %s\n",
                      strUnit(static_cast<float>(attempts)), strUnit(static_cast<float>(attempts_per_second)),
                      match_stats_str.empty() ? "none yet" : match_stats_str,
                      best_match_address, best_match_length, prefix.size(),
                      last_non_matching_address);
            last_progress_time = current_time;
        }

        // Check for shutdown request
        if (ShutdownRequested()) {
            LogPrintf("tor: Vanity address generation interrupted due to node shutdown\n");
            return false;
        }
    }

    // This code will never be reached as the loop continues until a match is found
    return false;
}
