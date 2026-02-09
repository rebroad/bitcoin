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
#include <boost/filesystem.hpp>
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
#include <fstream>
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
 * this is belt-and-suspenders sanity limit to prevent memory exhaustion. */
static const int MAX_LINE_LENGTH = 100000;
/** Directory monitoring interval in seconds */
static const int DIRECTORY_MONITOR_INTERVAL = 10;
/** Onion service private key prefix */
static constexpr const char* ONION_KEY_PREFIX = "ED25519-V3:";
static constexpr size_t ONION_KEY_PREFIX_LEN = 11; // strlen("ED25519-V3:")
/****** Low-level TorControlConnection ********/

TorControlConnection::TorControlConnection(struct event_base *_base):
    base(_base), b_conn(nullptr)
{
    // TODO - is this function needed given it's empty?
}

TorControlConnection::~TorControlConnection() {
    if (b_conn) bufferevent_free(b_conn);
}

void TorControlConnection::readcb(struct bufferevent *bev, void *ctx) {
    TorControlConnection *self = static_cast<TorControlConnection*>(ctx);
    struct evbuffer *input = bufferevent_get_input(bev);
    size_t n_read_out = 0;
    char *line;
    assert(input);
    //  If there is not a whole line to read, evbuffer_readln returns nullptr
    while((line = evbuffer_readln(input, &n_read_out, EVBUFFER_EOL_CRLF)) != nullptr) {
        std::string s(line, n_read_out);
        free(line);
        if (s.size() < 4) continue; // Short line
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
                } else
                    LogPrint(BCLog::TOR, "tor: Received unexpected sync reply %i\n", self->message.code);
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

void TorControlConnection::eventcb(struct bufferevent *bev, short what, void *ctx) {
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

static TorController* gTorController = nullptr;

TorController::TorController(struct event_base* _base, const std::string& tor_control_center, const CService& target):
    base(_base),
    m_tor_control_center(tor_control_center),
    conn(base),
    reconnect(true),
    reconnect_ev(0),
    m_target(target),
    num_services(static_cast<size_t>(gArgs.GetIntArg("-numonion", 1))),
    directory_monitor_ev(0),
    reconnect_timeout(RECONNECT_TIMEOUT_START)
{
    gTorController = this;
    // Initialize the service vectors to the right size
    service_ids.resize(num_services);
    services.resize(num_services);

    reconnect_ev = event_new(base, -1, 0, reconnect_cb, this);
    if (!reconnect_ev)
        LogPrintf("tor: Failed to create event for reconnection: out of memory?\n");

    // Create directory monitoring event
    directory_monitor_ev = event_new(base, -1, EV_PERSIST, directory_monitor_cb, this);
    if (!directory_monitor_ev) {
        LogPrintf("tor: Failed to create event for directory monitoring: out of memory?\n");
    } else {
        // Set up a timer to check the directory periodically
        struct timeval monitor_time = {DIRECTORY_MONITOR_INTERVAL, 0};
        if (event_add(directory_monitor_ev, &monitor_time) < 0) {
            LogPrintf("tor: Failed to add timer for directory monitoring\n");
            event_free(directory_monitor_ev);
            directory_monitor_ev = nullptr;
        } else {
            LogPrint(BCLog::TOR, "tor: Directory monitoring enabled with %d second interval\n", DIRECTORY_MONITOR_INTERVAL);
        }
    }

    // Start connection attempts immediately
    if (!conn.Connect(m_tor_control_center, std::bind(&TorController::connected_cb, this, std::placeholders::_1),
         std::bind(&TorController::disconnected_cb, this, std::placeholders::_1) )) {
        LogPrintf("tor: Initiating connection to Tor control port %s failed\n", m_tor_control_center);
    }

    // First try to load keys from the directory
    LoadPrivateKeysFromDirectory();

    // If no valid keys were loaded from directory, fall back to the single file (backwards compatibility)
    if (private_keys.empty()) {
        // Read service private keys if cached (one per line)
        std::pair<bool,std::string> pkf = ReadBinaryFile(GetPrivateKeyFile());
        if (pkf.first) {
            LogPrint(BCLog::TOR, "tor: Reading cached private keys from %s\n", fs::PathToString(GetPrivateKeyFile()));
            // Split the file contents by newlines to get multiple keys
            std::string key_file_content = pkf.second;
            std::vector<std::string> key_lines;

            // Split by any combination of CR and LF for cross-platform compatibility
            boost::split(key_lines, key_file_content, boost::is_any_of("\r\n"));
            LogPrint(BCLog::TOR, "tor: Found %d lines in key file\n", key_lines.size());

            // Add each non-empty line as a private key
            private_keys.clear(); // Ensure we start with an empty vector
            int line_idx = 0;
            for (const std::string& key_line : key_lines) {
                line_idx++;
                LogPrint(BCLog::TOR, "tor: Processing key file line %d, length: %d, empty: %s\n",
                         line_idx, key_line.length(), key_line.empty() ? "true" : "false");
                if (!key_line.empty()) {
                    // Use shared validation function with a description identifying line number
                    std::string filename = "onion_v3_private_key (line " + std::to_string(line_idx) + ")";
                    std::pair<bool, std::string> validation_result = ValidateOnionKey(key_line, filename);

                    if (validation_result.first) {
                        // Valid key found
                        LogPrint(BCLog::TOR, "tor: Validated key format: %s...\n",
                                 validation_result.second.substr(0, std::min(12, (int)validation_result.second.length())));
                        private_keys.push_back(validation_result.second);
                    } else {
                        // Invalid key
                        LogPrintf("tor: Skipping invalid private key format in key file: %s...\n",
                                 validation_result.second.substr(0, std::min(12, (int)validation_result.second.length())));
                    }
                }
            }

            // If no valid keys were found, initialize with an empty vector
            if (private_keys.empty()) {
                LogPrint(BCLog::TOR, "tor: No valid private keys found in key file\n");
            } else {
                LogPrint(BCLog::TOR, "tor: Loaded %d private key(s) from key file\n", private_keys.size());
                LogPrint(BCLog::TOR, "tor: Vector sizes - private_keys: %d, service_ids: %d, services: %d\n",
                         private_keys.size(), service_ids.size(), services.size());

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
}

TorController::~TorController() {
    gTorController = nullptr;
    if (reconnect_ev) {
        event_free(reconnect_ev);
        reconnect_ev = nullptr;
    }
    if (directory_monitor_ev) {
        event_free(directory_monitor_ev);
        directory_monitor_ev = nullptr;

        // Clean up any monitored services
        for (const std::string& filepath : monitored_files) {
            LogPrint(BCLog::TOR, "tor: Cleaning up monitored file: %s\n", filepath);
        }
        monitored_files.clear();
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

    LogPrint(BCLog::TOR, "tor: add_onion_cb called for service index %d, vectors sizes - private_keys: %d, service_ids: %d, services: %d\n",
             service_index, private_keys.size(), service_ids.size(), services.size());

    if (reply.code == 250) {
        LogPrint(BCLog::TOR, "tor: ADD_ONION successful for service %d\n", service_index);

        // Temporary variables to store service data from this callback
        std::string new_service_id;
        std::string new_private_key;
        int line_idx = 0;
        for (const std::string &s : reply.lines) {
            line_idx++;
            LogPrint(BCLog::TOR, "tor: Processing reply line %d: %s\n", line_idx, s.substr(0, 20) + (s.length() > 20 ? "..." : ""));
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
        // If we received a key from Tor, store it. Otherwise keep the key we sent.
        if (!new_private_key.empty()) {
            private_keys[service_index] = new_private_key;
        }
        services[service_index] = LookupNumeric(std::string(new_service_id+".onion"), Params().GetDefaultPort());

        LogPrintf("tor: Got service ID %s, advertising service %s\n",
                  new_service_id, services[service_index].ToString());

        // Create the private keys directory if it doesn't exist
        fs::path key_directory = GetPrivateKeyDirectory();
        bool directory_exists = false;

        try {
            if (!fs::exists(key_directory)) {
                if (fs::create_directories(key_directory)) {
                    LogPrint(BCLog::TOR, "tor: Created private key directory %s\n", fs::PathToString(key_directory));
                    directory_exists = true;
                } else {
                    LogPrintf("tor: Failed to create private key directory %s\n", fs::PathToString(key_directory));
                }
            } else {
                directory_exists = true;
            }
        } catch (const fs::filesystem_error& e) {
            LogPrintf("tor: Error creating private key directory %s: %s\n",
                      fs::PathToString(key_directory), e.what());
        }

        // Write key to individual file in the directory
        if (directory_exists) {
            // Save the key for the current service (if both service ID and key are present)
            const std::string& service_id = service_ids[service_index];
            const std::string& key = private_keys[service_index];

            LogPrint(BCLog::TOR, "tor: Checking current service at index %d - service_id length: %d, key length: %d\n",
                     service_index, service_id.length(), key.length());

            if (!service_id.empty() && !key.empty()) {
                fs::path key_file = key_directory / service_id;
                LogPrint(BCLog::TOR, "tor: Attempting to save key for service %s to file %s\n",
                         service_id, fs::PathToString(key_file));
                try {
                    if (WriteBinaryFile(key_file, key)) {
                        // Add to monitored files cache for the directory monitor
                        monitored_files.insert(fs::PathToString(key_file));
                        monitored_keys_cache[fs::PathToString(key_file)] = key;

                        LogPrint(BCLog::TOR, "tor: Successfully saved private key for service %s to %s (key format: %s...)\n",
                                 service_id, fs::PathToString(key_file),
                                 key.substr(0, std::min(12, (int)key.length())));
                    } else {
                        LogPrintf("tor: Error writing private key file for service %s to %s\n",
                                  service_id, fs::PathToString(key_file));
                    }
                } catch (const std::exception& e) {
                    LogPrintf("tor: Error saving key file for service %s: %s\n", service_id, e.what());
                }
            } else {
                LogPrint(BCLog::TOR, "tor: Skipping save for service %s - empty id or key\n", service_id);
            }
        } else {
            LogPrintf("tor: Could not access or create private key directory - keys not saved\n");
        }

        // NOTE: The legacy single-file storage is no longer used by default,
        // but can be created for backward compatibility if needed

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

        LogPrint(BCLog::TOR, "tor: After adding service %d, vector sizes - private_keys: %d, service_ids: %d, services: %d\n",
                 service_index, private_keys.size(), service_ids.size(), services.size());

        if (current_service_index < num_services) {
            // Create the next service
            LogPrint(BCLog::TOR, "tor: Creating next onion service (%d of %d)\n",
                    current_service_index + 1, num_services);

            // If we have a stored private key for this index, use it
            if (current_service_index < private_keys.size() && !private_keys[current_service_index].empty()) {
                LogPrint(BCLog::TOR, "tor: Using stored private key for service %d\n", current_service_index);
            } else {
                // No private key for this index, generate a new one
                private_keys[current_service_index] = "NEW:ED25519-V3";
                LogPrint(BCLog::TOR, "tor: Generating new private key for service %d\n", current_service_index);
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

        if (services_initialized) {
            LogPrint(BCLog::TOR, "tor: Services already initialized; skipping service creation on re-auth\n");
            return;
        }

        // Get the number of onion services to create
        size_t num_services = static_cast<size_t>(gArgs.GetIntArg("-numonion", 1));

        // Ensure we have at least one service
        if (num_services < 1) {
            num_services = 1;
            LogPrint(BCLog::TOR, "tor: Invalid -numonion value, defaulting to 1 service\n");
        }

        LogPrint(BCLog::TOR, "tor: Creating %d onion service(s)\n", num_services);
        LogPrint(BCLog::TOR, "tor: Before resizing - private_keys: %d, service_ids: %d, services: %d\n",
                 private_keys.size(), service_ids.size(), services.size());

        // Initialize/resize the service vectors to hold num_services entries
        private_keys.resize(num_services);
        service_ids.resize(num_services);
        services.resize(num_services);

        LogPrint(BCLog::TOR, "tor: After resizing - private_keys: %d, service_ids: %d, services: %d\n",
                 private_keys.size(), service_ids.size(), services.size());

        // Start with the first service
        current_service_index = 0;

        // If we have a stored private key for this index, use it
        if (current_service_index < private_keys.size() && !private_keys[current_service_index].empty()) {
            LogPrint(BCLog::TOR, "tor: Using stored private key for service %d (key length: %d)\n",
                     current_service_index, private_keys[current_service_index].length());
        } else {
            // No private key for this index, generate a new one
            private_keys[current_service_index] = "NEW:ED25519-V3"; // Explicitly request key type - see issue #9214
            LogPrint(BCLog::TOR, "tor: Generating new private key for service %d\n", current_service_index);
        }

        // Request onion service, redirect port.
        // Note that the 'virtual' port is always the default port to avoid decloaking nodes using other ports.
        _conn.Command(strprintf("ADD_ONION %s Port=%i,%s", private_keys[current_service_index],
                    Params().GetDefaultPort(), m_target.ToStringIPPort()),
            std::bind(&TorController::add_onion_cb, this, std::placeholders::_1, std::placeholders::_2));

        services_initialized = true;
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

/** Validate an onion service private key format with detailed logging */
std::pair<bool, std::string> TorController::ValidateOnionKey(const std::string& key_data, const std::string& filename)
{
    LogPrint(BCLog::TOR, "tor: Key validation steps for file %s:\n", filename.c_str());
    LogPrint(BCLog::TOR, "tor: 1. Raw key length: %d\n", key_data.length());

    // Handle trailing newlines and whitespace
    std::string key_str = key_data;
    if (!key_str.empty() && key_str.back() == '\n') {
        key_str.pop_back();  // Remove trailing newline if present
        LogPrint(BCLog::TOR, "tor: Removed trailing newline\n");
    }

    std::string trimmed_key = boost::algorithm::trim_copy(key_str);
    LogPrint(BCLog::TOR, "tor: 2. After trim length: %d\n", trimmed_key.length());

    // Only check for ED25519-V3 format
    bool valid_key = (trimmed_key.length() > ONION_KEY_PREFIX_LEN &&
                      trimmed_key.compare(0, ONION_KEY_PREFIX_LEN, ONION_KEY_PREFIX) == 0);
    LogPrint(BCLog::TOR, "tor: Key validation %s\n", valid_key ? "PASSED" : "FAILED");

    return std::make_pair(valid_key, trimmed_key);
}

fs::path TorController::GetPrivateKeyFile()
{
    return gArgs.GetDataDirNet() / "onion_v3_private_key";
}

fs::path TorController::GetPrivateKeyDirectory()
{
    return gArgs.GetDataDirNet() / "onion_v3_private_keys";
}

bool TorController::LoadPrivateKeysFromDirectory()
{
    fs::path directory = GetPrivateKeyDirectory();
    LogPrint(BCLog::TOR, "tor: Loading private keys from directory: %s\n", fs::PathToString(directory));

    // Check if directory exists, if not create it
    try {
        if (!fs::exists(directory)) {
            LogPrint(BCLog::TOR, "tor: Private key directory doesn't exist, creating: %s\n", fs::PathToString(directory));
            fs::create_directories(directory);
        }

        if (!fs::is_directory(directory)) {
            LogPrintf("tor: Failed to create or access private key directory %s\n", fs::PathToString(directory));
            return false;
        }
    } catch (const fs::filesystem_error& e) {
        LogPrintf("tor: Error creating private key directory %s: %s\n",
                fs::PathToString(directory), e.what());
        return false;
    }

    private_keys.clear();
    monitored_files.clear();
    monitored_keys_cache.clear();
    int total_files_checked = 0;
    bool loaded_any = false;

    try {
        for (const auto& entry : fs::directory_iterator(directory)) {
            total_files_checked++;
            std::string filename = fs::PathToString(entry.path());
            if (!fs::is_regular_file(entry.status())) {
                continue;
            }

            try {
                std::ifstream keyfile(entry.path());
                if (!keyfile.is_open()) {
                    LogPrintf("tor: Failed to open private key file %s\n", filename);
                    continue;
                }

                std::string key_data((std::istreambuf_iterator<char>(keyfile)),
                                std::istreambuf_iterator<char>());
                keyfile.close();

                if (key_data.empty()) {
                    LogPrintf("tor: Empty private key file %s\n", filename);
                    continue;
                }

                std::pair<bool, std::string> validation_result = ValidateOnionKey(key_data, filename);

                if (validation_result.first) {
                    LogPrint(BCLog::TOR, "tor: Valid private key format found in file %s\n", filename);
                    private_keys.push_back(validation_result.second);
                    monitored_files.insert(fs::PathToString(entry.path()));
                    monitored_keys_cache[fs::PathToString(entry.path())] = validation_result.second;
                    LogPrint(BCLog::TOR, "tor: Added private key from file %s to monitoring\n", filename);
                    loaded_any = true;
                } else {
                    LogPrintf("tor: Skipping invalid private key format in file %s\n", filename);
                }
            } catch (const std::exception& e) {
                LogPrintf("tor: Error processing key file %s: %s\n", filename, e.what());
            }
        }
    } catch (const fs::filesystem_error& e) {
        LogPrintf("tor: Error iterating directory %s: %s\n",
                fs::PathToString(directory), e.what());
    }

    if (loaded_any) {
        LogPrint(BCLog::TOR, "tor: Loaded %d private key(s) from directory, processed %d total files\n",
                private_keys.size(), total_files_checked);
        LogPrint(BCLog::TOR, "tor: Vector sizes after loading - private_keys: %d, service_ids: %d, services: %d, monitored_files: %d\n",
                private_keys.size(), service_ids.size(), services.size(), monitored_files.size());

        // Monitor the directory for changes
        if (!directory_monitor_ev) {
            struct event* new_ev = event_new(base, -1, EV_PERSIST,
                                        directory_monitor_cb, this);
            if (!new_ev) {
                LogPrintf("tor: Failed to create directory monitor event\n");
                return false;
            }
            directory_monitor_ev = new_ev;
            struct timeval tv = { DIRECTORY_MONITOR_INTERVAL, 0 };
            event_add(directory_monitor_ev, &tv);
        }

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

    return loaded_any;
}

void TorController::reconnect_cb(evutil_socket_t fd, short what, void *arg)
{
    TorController *self = static_cast<TorController*>(arg);
    self->Reconnect();
}

void TorController::directory_monitor_cb(evutil_socket_t fd, short what, void *arg)
{
    TorController *self = static_cast<TorController*>(arg);
    fs::path directory = self->GetPrivateKeyDirectory();

    try {
        if (!fs::exists(directory) || !fs::is_directory(directory)) {
            return;
        }
    } catch (const fs::filesystem_error& e) {
        LogPrintf("tor: Error checking directory %s: %s\n",
                  fs::PathToString(directory), e.what());
        return;
    }

    // Check for new files
    std::set<std::string> current_files;
    try {
        for (const auto& entry : fs::directory_iterator(directory)) {
            try {
                // Skip .disabled directory and any other subdirectories
                if (fs::is_directory(entry.path())) continue;
                if (fs::is_regular_file(entry.path())) current_files.insert(fs::PathToString(entry.path()));
            } catch (const fs::filesystem_error& e) {
                LogPrintf("tor: Error checking file type: %s\n", e.what());
            } catch (const std::exception& e) {
                LogPrintf("tor: Error processing file path: %s\n", e.what());
            }
        }
    } catch (const fs::filesystem_error& e) {
        LogPrintf("tor: Error iterating directory %s: %s\n",
                  fs::PathToString(directory), e.what());
        return;
    }

    // Find added files
    try {
        for (const std::string& filepath : current_files) {
            try {
                if (filepath.empty()) {
                    LogPrintf("tor: Empty filepath in current_files list, skipping\n");
                    continue;
                }

                // Check if this is a new file we're not monitoring yet
                if (self->monitored_files.find(filepath) == self->monitored_files.end()) {
                    LogPrint(BCLog::TOR, "tor: New private key file detected: %s\n", filepath);

                    // Read the new key file
                    std::pair<bool, std::string> key_data;
                    try {
                        key_data = ReadBinaryFile(fs::PathFromString(filepath));
                    } catch (const fs::filesystem_error& e) {
                        LogPrintf("tor: Error reading new file %s: %s\n", filepath, e.what());
                        continue;
                    } catch (const std::exception& e) {
                        LogPrintf("tor: Unexpected error reading file %s: %s\n", filepath, e.what());
                        continue;
                    }

                    if (key_data.first && !key_data.second.empty()) {
                        std::string trimmed_key;
                        try {
                            // Add detailed logging for key validation
                            LogPrint(BCLog::TOR, "tor: Validating key: length=%d, prefix='%.10s'\n",
                                key_data.second.length(),
                                key_data.second.substr(0, std::min(size_t(10), key_data.second.length())).c_str());

                            // Also ensure key validation accepts both with and without trailing newline
                            std::string key_str = key_data.second;
                            if (key_str.length() > 0 && key_str.back() == '\n') {
                                key_str.pop_back();  // Remove trailing newline if present
                                LogPrint(BCLog::TOR, "tor: Removed trailing newline from key\n");
                            }

                            // Add debug output before actual validation
                            LogPrint(BCLog::TOR, "tor: Key after newline handling: length=%d\n", key_str.length());

                            trimmed_key = boost::algorithm::trim_copy(key_str);
                        } catch (const std::exception& e) {
                            LogPrintf("tor: Error trimming key data: %s\n", e.what());
                            continue;
                        }
                        if (trimmed_key.empty()) {
                            LogPrintf("tor: Empty key found in file %s, skipping\n", filepath);
                            continue;
                        }

                        // Validate key format with safe string operations
                        bool valid_key = false;
                        try {
                            if (trimmed_key.size() > 4 && trimmed_key.compare(0, 4, "NEW:") == 0)
                                valid_key = true;
                            else if (trimmed_key.size() > ONION_KEY_PREFIX_LEN &&
                                     trimmed_key.compare(0, ONION_KEY_PREFIX_LEN, ONION_KEY_PREFIX) == 0)
                                valid_key = true;
                            else if (trimmed_key.find(":") != std::string::npos)
                                valid_key = true;
                        } catch (const std::exception& e) {
                            LogPrintf("tor: Error validating key format: %s\n", e.what());
                            continue;
                        }

                        if (valid_key) {
                            // Check if adding this key would exceed the configured limit
                            size_t num_services = static_cast<size_t>(gArgs.GetIntArg("-numonion", 1));
                            if (self->monitored_files.size() >= num_services) {
                                LogPrintf("tor: Skipping new key file %s - would exceed configured limit of %d onion services\n",
                                         filepath, num_services);

                                // Create .disabled subdirectory if it doesn't exist
                                fs::path disabled_dir = directory / ".disabled";
                                try {
                                    if (!fs::exists(disabled_dir)) fs::create_directories(disabled_dir);

                                    // Move the file to .disabled directory
                                    fs::path source_path = fs::PathFromString(filepath);
                                    fs::path target_path = disabled_dir / source_path.filename();
                                    fs::rename(source_path, target_path);
                                    LogPrint(BCLog::TOR, "tor: Moved skipped key file to %s\n", fs::PathToString(target_path));
                                } catch (const fs::filesystem_error& e) {
                                    LogPrintf("tor: Error moving skipped file to .disabled directory: %s\n", e.what());
                                }
                                continue;
                            }

                            // Add to monitored files and cache
                            try {
                                self->monitored_files.insert(filepath);
                                self->monitored_keys_cache[filepath] = trimmed_key;
                            } catch (const std::exception& e) {
                                LogPrintf("tor: Error adding file to monitoring: %s\n", e.what());
                                continue;
                            }
                            // Create a new onion service with this key
                            // Only if we're connected - otherwise it will be loaded on next connection
                            try {
                                if (self->conn.Command("GETINFO status/circuit-established",
                                    [self, trimmed_key](TorControlConnection& conn, const TorControlReply& reply) {
                                        try {
                                            if (reply.code == 250 && reply.lines.size() > 0 && reply.lines[0] == "1") {
                                                // Connected to Tor, add the new service
                                                try {
                                                    conn.Command(strprintf("ADD_ONION %s Port=%i,%s",
                                                                trimmed_key,
                                                                Params().GetDefaultPort(),
                                                                self->m_target.ToStringIPPort()),
                                                        std::bind(&TorController::add_onion_cb, self,
                                                                std::placeholders::_1, std::placeholders::_2));
                                                } catch (const std::exception& e) {
                                                    LogPrintf("tor: Error creating onion service: %s\n", e.what());
                                                }
                                            }
                                        } catch (const std::exception& e) {
                                            LogPrintf("tor: Error in circuit status callback: %s\n", e.what());
                                        }
                                    })) {
                                    LogPrint(BCLog::TOR, "tor: Attempting to create service with new key\n");
                                } else {
                                    LogPrintf("tor: Failed to send GETINFO command for circuit status\n");
                                }
                            } catch (const std::exception& e) {
                                LogPrintf("tor: Error checking Tor connectivity: %s\n", e.what());
                            }
                        } else {
                            LogPrintf("tor: New file contains invalid key format, ignoring\n");
                        }
                    } else {
                        LogPrintf("tor: Invalid or empty key data in file %s\n", filepath);
                    }
                }
            } catch (const std::exception& e) {
                LogPrintf("tor: Unexpected error processing new file: %s\n", e.what());
            }
        }
    } catch (const std::exception& e) {
        LogPrintf("tor: Error processing new files: %s\n", e.what());
    }

    // Find removed files
    std::vector<std::string> removed_files;
    try {
        for (const std::string& filepath : self->monitored_files) {
            if (!filepath.empty() && current_files.find(filepath) == current_files.end()) {
                removed_files.push_back(filepath);
            }
        }
    } catch (const std::exception& e) {
        LogPrintf("tor: Error identifying removed files: %s\n", e.what());
        return;
    }

    // Handle removed files
    for (const std::string& filepath : removed_files) {
        try {
            if (filepath.empty()) {
                LogPrintf("tor: Empty filepath in removed_files list, skipping\n");
                continue;
            }

            LogPrint(BCLog::TOR, "tor: Private key file removed: %s\n", filepath);

            // Get the private key from cache instead of reading the file
            auto it = self->monitored_keys_cache.find(filepath);
            if (it == self->monitored_keys_cache.end()) {
                LogPrintf("tor: No cached key found for removed file %s\n", filepath);
                continue;
            }

            std::string removed_key = it->second;

            // Find the index of this key in our private_keys vector
            bool found_key = false;
            size_t key_index = 0;
            try {
                for (size_t i = 0; i < self->private_keys.size(); i++) {
                    if (self->private_keys[i] == removed_key) {
                        key_index = i;
                        found_key = true;
                        break;
                    }
                }

                if (found_key) {
                    LogPrint(BCLog::TOR, "tor: Found service corresponding to removed key file at index %d\n", key_index);

                    // Check if we have a valid service to remove
                    if (key_index < self->services.size() && self->services[key_index].IsValid()) {
                        try {
                            std::string service_id = key_index < self->service_ids.size() ?
                                self->service_ids[key_index] : "unknown";

                            // First try to remove the service from Tor if we're connected
                            if (self->conn.Command("GETINFO status/circuit-established",
                                [self, key_index, service_id](TorControlConnection& conn, const TorControlReply& reply) {
                                    try {
                                        if (reply.code == 250 && reply.lines.size() > 0 && reply.lines[0] == "1") {
                                            // Connected to Tor, try to remove the service
                                            if (!service_id.empty() && service_id != "unknown") {
                                                conn.Command(strprintf("DEL_ONION %s", service_id),
                                                    [key_index, service_id](TorControlConnection& conn, const TorControlReply& reply) {
                                                        if (reply.code == 250) {
                                                            LogPrint(BCLog::TOR, "tor: Successfully removed service %s from Tor\n", service_id);
                                                        } else {
                                                            LogPrintf("tor: Failed to remove service %s from Tor, code %d\n",
                                                                    service_id, reply.code);
                                                        }
                                                    });
                                            }
                                        }
                                    } catch (const std::exception& e) {
                                        LogPrintf("tor: Error in circuit status callback: %s\n", e.what());
                                    }
                                })) {
                                LogPrint(BCLog::TOR, "tor: Attempting to remove service from Tor\n");
                            }

                            // Remove the service from our local address list
                            RemoveLocal(self->services[key_index]);
                            LogPrint(BCLog::TOR, "tor: Removed service %s from local address list\n",
                                    self->services[key_index].ToString());

                            // Clear service data
                            self->services[key_index] = CService();
                            if (key_index < self->service_ids.size()) {
                                self->service_ids[key_index] = "";
                            }
                            if (key_index < self->private_keys.size()) {
                                self->private_keys[key_index] = "";
                            }
                        } catch (const std::exception& e) {
                            LogPrintf("tor: Error removing service: %s\n", e.what());
                        }
                    }
                }
            } catch (const std::exception& e) {
                LogPrintf("tor: Error searching for key index: %s\n", e.what());
            }

            // Remove from monitored files and cache
            self->monitored_files.erase(filepath);
            self->monitored_keys_cache.erase(filepath);
            LogPrint(BCLog::TOR, "tor: Removed %s from monitored files and cache\n", filepath);
        } catch (const std::exception& e) {
            LogPrintf("tor: Error processing removed file: %s\n", e.what());
        }
    }
}

// Accessor for the global TorController instance
TorController* GetTorController() {
    return gTorController;
}

/****** Thread ********/
static struct event_base *gBase;
static std::thread torControlThread;

static void TorControlThread(CService onion_service_target) {
    SetSyscallSandboxPolicy(SyscallSandboxPolicy::TOR_CONTROL);
    TorController ctrl(gBase, gArgs.GetArg("-torcontrol", DEFAULT_TOR_CONTROL), onion_service_target);

    event_base_dispatch(gBase);
}

void StartTorControl(CService onion_service_target) {
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

void InterruptTorControl() {
    if (gBase) {
        LogPrintf("tor: Thread interrupt\n");
        event_base_once(gBase, -1, EV_TIMEOUT, [](evutil_socket_t, short, void*) {
            event_base_loopbreak(gBase);
        }, nullptr, nullptr);
    }
}

void StopTorControl() {
    if (gBase) {
        torControlThread.join();
        event_base_free(gBase);
        gBase = nullptr;
    }
}

void ResetTorBackoff() {
    // If the tor control thread is running, schedule an immediate reconnect
    if (!torControlThread.joinable() || !gBase) return;

    TorController* ctrl = GetTorController();
    if (!ctrl) {
        LogPrint(BCLog::TOR, "tor: ResetTorBackoff called but TorController is not available\n");
        return;
    }

    struct ResetEvent {
        TorController* ctrl;
    };

    auto* data = new ResetEvent{ctrl};
    if (event_base_once(gBase, -1, EV_TIMEOUT, [](evutil_socket_t, short, void* arg) {
            auto* data = static_cast<ResetEvent*>(arg);
            data->ctrl->ResetReconnectBackoff();
            delete data;
        }, data, nullptr) < 0) {
        delete data;
        LogPrint(BCLog::TOR, "tor: Failed to schedule tor reconnect\n");
    }
}

CService DefaultOnionServiceTarget() {
    struct in_addr onion_service_target;
    onion_service_target.s_addr = htonl(INADDR_LOOPBACK);
    return {onion_service_target, BaseParams().OnionServiceTargetPort()};
}
/**
 * Generate a private key for an onion service that produces an address with the desired prefix.
 *
 * @param prefix The desired prefix for the onion address
 * @param[out] generated_private_key The generated private key if successful
 * @return true if a matching key was found, false otherwise */
