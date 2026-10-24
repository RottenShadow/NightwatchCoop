// SPDX-License-Identifier: GPL-3.0-or-later
#include "party_steam.h"

#include "steam_identity.h"
#include "bbnet_internal.h"

#if defined(_WIN32) && defined(BB_STEAM)

#include <windows.h>

#include <steam/steam_api.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace bbnet::steam {
namespace {

using InitFlatFn = ESteamAPIInitResult(S_CALLTYPE*)(SteamErrMsg*);
using ShutdownFn = void(S_CALLTYPE*)();
using RunCallbacksFn = void(S_CALLTYPE*)();
using UserFn = ISteamUser*(S_CALLTYPE*)();
using FriendsFn = ISteamFriends*(S_CALLTYPE*)();
using UtilsFn = ISteamUtils*(S_CALLTYPE*)();
using NetUtilsFn = ISteamNetworkingUtils*(S_CALLTYPE*)();
using SocketsFn = ISteamNetworkingSockets*(S_CALLTYPE*)();
using RegisterCallbackFn = void(S_CALLTYPE*)(CCallbackBase*, int);
using UnregisterCallbackFn = void(S_CALLTYPE*)(CCallbackBase*);

// steam_api64.dll 1.61 exports only the flat C API. Calling the classic interface
// methods through their vtables returns garbage (wrong SteamID, failed friend checks,
// failed P2P connects), so every interface method goes through one of these.
struct FlatApi {
    using GetSteamIDFn = std::uint64_t(S_CALLTYPE*)(ISteamUser*);
    using BLoggedOnFn = bool(S_CALLTYPE*)(ISteamUser*);

    using HasFriendFn = bool(S_CALLTYPE*)(ISteamFriends*, std::uint64_t, int);
    using SetRichPresenceFn = bool(S_CALLTYPE*)(ISteamFriends*, const char*, const char*);
    using ClearRichPresenceFn = void(S_CALLTYPE*)(ISteamFriends*);
    using ActivateInviteFn = void(S_CALLTYPE*)(ISteamFriends*, const char*);
    using GetFriendCountFn = int(S_CALLTYPE*)(ISteamFriends*, int);
    using GetFriendByIndexFn = std::uint64_t(S_CALLTYPE*)(ISteamFriends*, int, int);
    using GetFriendPersonaNameFn = const char*(S_CALLTYPE*)(ISteamFriends*, std::uint64_t);
    using GetFriendPersonaStateFn = EPersonaState(S_CALLTYPE*)(ISteamFriends*, std::uint64_t);
    using GetFriendGamePlayedFn = bool(S_CALLTYPE*)(ISteamFriends*, std::uint64_t, FriendGameInfo_t*);

    using ConnectP2PFn = HSteamNetConnection(S_CALLTYPE*)(ISteamNetworkingSockets*, const SteamNetworkingIdentity&,
                                                        int, int, const SteamNetworkingConfigValue_t*);
    using CreateListenP2PFn =
        HSteamListenSocket(S_CALLTYPE*)(ISteamNetworkingSockets*, int, int, const SteamNetworkingConfigValue_t*);
    using CreatePollGroupFn = HSteamNetPollGroup(S_CALLTYPE*)(ISteamNetworkingSockets*);
    using DestroyPollGroupFn = bool(S_CALLTYPE*)(ISteamNetworkingSockets*, HSteamNetPollGroup);
    using AcceptConnectionFn = EResult(S_CALLTYPE*)(ISteamNetworkingSockets*, HSteamNetConnection);
    using SetConnPollGroupFn =
        bool(S_CALLTYPE*)(ISteamNetworkingSockets*, HSteamNetConnection, HSteamNetPollGroup);
    using CloseConnectionFn = bool(S_CALLTYPE*)(ISteamNetworkingSockets*, HSteamNetConnection, int, const char*,
                                               bool);
    using CloseListenSocketFn = bool(S_CALLTYPE*)(ISteamNetworkingSockets*, HSteamListenSocket);
    using SendMessageFn =
        EResult(S_CALLTYPE*)(ISteamNetworkingSockets*, HSteamNetConnection, const void*, std::uint32_t, int,
                             std::int64_t*);
    using ReceiveMessagesFn = int(S_CALLTYPE*)(ISteamNetworkingSockets*, HSteamNetPollGroup, SteamNetworkingMessage_t**,
                                              int);
    using GetConnectionInfoFn = bool(S_CALLTYPE*)(ISteamNetworkingSockets*, HSteamNetConnection, SteamNetConnectionInfo_t*);

    using SetStatusCbFn = bool(S_CALLTYPE*)(ISteamNetworkingUtils*, FnSteamNetConnectionStatusChanged);
    using InitRelayFn = void(S_CALLTYPE*)(ISteamNetworkingUtils*);
    using RelayStatusFn =
        ESteamNetworkingAvailability(S_CALLTYPE*)(ISteamNetworkingUtils*, SteamRelayNetworkStatus_t*);

    GetSteamIDFn get_steamid = nullptr;
    BLoggedOnFn b_logged_on = nullptr;

    HasFriendFn has_friend = nullptr;
    SetRichPresenceFn set_rich_presence = nullptr;
    ClearRichPresenceFn clear_rich_presence = nullptr;
    ActivateInviteFn activate_invite = nullptr;
    GetFriendCountFn get_friend_count = nullptr;
    GetFriendByIndexFn get_friend_by_index = nullptr;
    GetFriendPersonaNameFn get_friend_persona_name = nullptr;
    GetFriendPersonaStateFn get_friend_persona_state = nullptr;
    GetFriendGamePlayedFn get_friend_game_played = nullptr;

    ConnectP2PFn connect_p2p = nullptr;
    CreateListenP2PFn create_listen_p2p = nullptr;
    CreatePollGroupFn create_poll_group = nullptr;
    DestroyPollGroupFn destroy_poll_group = nullptr;
    AcceptConnectionFn accept_connection = nullptr;
    SetConnPollGroupFn set_conn_poll_group = nullptr;
    CloseConnectionFn close_connection = nullptr;
    CloseListenSocketFn close_listen_socket = nullptr;
    SendMessageFn send_message = nullptr;
    ReceiveMessagesFn receive_messages = nullptr;
    GetConnectionInfoFn get_connection_info = nullptr;

    SetStatusCbFn set_status_cb = nullptr;
    InitRelayFn init_relay = nullptr;
    RelayStatusFn relay_status = nullptr;

    // Returns nullptr when every function was resolved, else the first missing export name.
    const char* load(HMODULE dll);
};

constexpr auto kPollInterval = std::chrono::milliseconds(5);
constexpr auto kStartTimeout = std::chrono::seconds(20);
constexpr auto kRedialInterval = std::chrono::seconds(2);
constexpr int kMaxMessages = 64;

const char* connection_state_name(ESteamNetworkingConnectionState state) {
    switch (state) {
        case k_ESteamNetworkingConnectionState_None:
            return "none";
        case k_ESteamNetworkingConnectionState_Connecting:
            return "connecting";
        case k_ESteamNetworkingConnectionState_FindingRoute:
            return "finding-route";
        case k_ESteamNetworkingConnectionState_Connected:
            return "connected";
        case k_ESteamNetworkingConnectionState_ClosedByPeer:
            return "closed-by-peer";
        case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
            return "problem-locally";
        case k_ESteamNetworkingConnectionState_FinWait:
            return "fin-wait";
        case k_ESteamNetworkingConnectionState_Linger:
            return "linger";
        case k_ESteamNetworkingConnectionState_Dead:
            return "dead";
        default:
            return "unknown";
    }
}

struct State {
    std::mutex mu;
    std::condition_variable cv;
    std::thread thread;
    std::atomic<bool> running{false};
    std::atomic<bool> stopping{false};
    bool host = true;
    std::uint32_t appid = 480;
    std::uint64_t local_steamid = 0;
    std::uint64_t host_steamid = 0;
    bool starting = false;
    bool failed = false;
    std::string error;
    std::string status;

    std::deque<Datagram> datagrams;
    std::deque<PeerEvent> events;
    std::deque<JoinRequest> join_requests;
    std::deque<std::uint64_t> connect_queue;
    std::set<std::uint64_t> pending_connects;
    std::map<std::uint64_t, HSteamNetConnection> conn_by_peer;
    std::map<HSteamNetConnection, std::uint64_t> peer_by_conn;
    std::map<std::uint64_t, std::chrono::steady_clock::time_point> dialed_at;
    std::set<std::uint64_t> connected;
    std::set<std::uint64_t> trusted;
    std::set<std::uint64_t> heard_from;
    std::uint64_t rx_datagrams = 0;
    std::uint64_t rx_bytes = 0;
    std::uint64_t tx_datagrams = 0;
    std::uint64_t tx_bytes = 0;
    std::uint64_t tx_failures = 0;
    AddressBook book;

    std::mutex api_mu;
    FlatApi flat;
    InitFlatFn init_flat = nullptr;
    ShutdownFn shutdown = nullptr;
    RunCallbacksFn run_callbacks = nullptr;
    RegisterCallbackFn register_callback = nullptr;
    UnregisterCallbackFn unregister_callback = nullptr;
    bool join_callback_registered = false;
    ISteamUser* user = nullptr;
    ISteamFriends* friends = nullptr;
    ISteamUtils* utils = nullptr;
    ISteamNetworkingUtils* net_utils = nullptr;
    ISteamNetworkingSockets* sockets = nullptr;
    HSteamListenSocket listen = k_HSteamListenSocket_Invalid;
    HSteamNetPollGroup poll = k_HSteamNetPollGroup_Invalid;
};

State* g_state = nullptr;

const char* FlatApi::load(HMODULE dll) {
    const char* missing = nullptr;
    const auto bind = [&](auto& target, const char* name) {
        using Fn = std::remove_reference_t<decltype(target)>;
        target = reinterpret_cast<Fn>(GetProcAddress(dll, name));
        if (!target && !missing) missing = name;
    };
    bind(get_steamid, "SteamAPI_ISteamUser_GetSteamID");
    bind(b_logged_on, "SteamAPI_ISteamUser_BLoggedOn");
    bind(has_friend, "SteamAPI_ISteamFriends_HasFriend");
    bind(set_rich_presence, "SteamAPI_ISteamFriends_SetRichPresence");
    bind(clear_rich_presence, "SteamAPI_ISteamFriends_ClearRichPresence");
    bind(get_friend_count, "SteamAPI_ISteamFriends_GetFriendCount");
    bind(get_friend_by_index, "SteamAPI_ISteamFriends_GetFriendByIndex");
    bind(get_friend_persona_name, "SteamAPI_ISteamFriends_GetFriendPersonaName");
    bind(get_friend_persona_state, "SteamAPI_ISteamFriends_GetFriendPersonaState");
    bind(get_friend_game_played, "SteamAPI_ISteamFriends_GetFriendGamePlayed");
    bind(connect_p2p, "SteamAPI_ISteamNetworkingSockets_ConnectP2P");
    bind(create_listen_p2p, "SteamAPI_ISteamNetworkingSockets_CreateListenSocketP2P");
    bind(create_poll_group, "SteamAPI_ISteamNetworkingSockets_CreatePollGroup");
    bind(destroy_poll_group, "SteamAPI_ISteamNetworkingSockets_DestroyPollGroup");
    bind(accept_connection, "SteamAPI_ISteamNetworkingSockets_AcceptConnection");
    bind(set_conn_poll_group, "SteamAPI_ISteamNetworkingSockets_SetConnectionPollGroup");
    bind(close_connection, "SteamAPI_ISteamNetworkingSockets_CloseConnection");
    bind(close_listen_socket, "SteamAPI_ISteamNetworkingSockets_CloseListenSocket");
    bind(send_message, "SteamAPI_ISteamNetworkingSockets_SendMessageToConnection");
    bind(receive_messages, "SteamAPI_ISteamNetworkingSockets_ReceiveMessagesOnPollGroup");
    get_connection_info = reinterpret_cast<GetConnectionInfoFn>(
        GetProcAddress(dll, "SteamAPI_ISteamNetworkingSockets_GetConnectionInfo"));
    // Optional: older 1.61 builds may not export the invite-dialog helper.
    activate_invite = reinterpret_cast<ActivateInviteFn>(
        GetProcAddress(dll, "SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialogConnectString"));
    bind(set_status_cb, "SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_SteamNetConnectionStatusChanged");
    bind(init_relay, "SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess");
    bind(relay_status, "SteamAPI_ISteamNetworkingUtils_GetRelayNetworkStatus");
    return missing;
}

std::uint64_t steamid_from_connect(const char* text) {
    if (!text) return 0;
    const char* p = std::strstr(text, "steam:");
    if (p) p += 6;
    else p = text;
    while (*p && !std::isdigit(static_cast<unsigned char>(*p))) ++p;
    char* end = nullptr;
    const unsigned long long id = std::strtoull(p, &end, 10);
    return end && end != p ? static_cast<std::uint64_t>(id) : 0;
}

struct JoinCallback : CCallbackBase {
    void Run(void* pvParam) override {
        State* s = g_state;
        if (!s || !pvParam) return;
        const auto* p = static_cast<const GameRichPresenceJoinRequested_t*>(pvParam);
        JoinRequest req;
        req.steamid = p->m_steamIDFriend.ConvertToUint64();
        req.connect = p->m_rgchConnect;
        if (!req.steamid) req.steamid = steamid_from_connect(req.connect.c_str());
        std::lock_guard<std::mutex> lock(s->mu);
        s->join_requests.push_back(std::move(req));
    }
    void Run(void* pvParam, bool, SteamAPICall_t) override { Run(pvParam); }
    int GetCallbackSizeBytes() override { return sizeof(GameRichPresenceJoinRequested_t); }
};
JoinCallback g_join_callback;

std::string steam_error(const SteamErrMsg& msg) {
    if (msg[0]) return std::string(msg);
    return "SteamAPI_InitFlat failed";
}

bool publish_failure(State& s, const std::string& why) {
    {
        std::lock_guard<std::mutex> lock(s.mu);
        s.starting = false;
        s.failed = true;
        s.error = why;
        s.status = why;
    }
    s.cv.notify_all();
    return false;
}

bool friend_of(const FlatApi& api, ISteamFriends* friends, std::uint64_t id) {
    return friends && id && api.has_friend && api.has_friend(friends, id, k_EFriendFlagImmediate);
}

void drop_connection(State& s, std::uint64_t id);

// Opens one Steam P2P connection. A handle that never reached Connected (ConnectP2P issued
// before the host listened, or a status callback that never arrived) is replaced after a short
// delay: returning at the first handle leaves the guest looping on a dead connection forever.
bool connect_internal(State& s, std::uint64_t id) {
    if (!id) return false;
    HSteamNetConnection stale = k_HSteamNetConnection_Invalid;
    {
        std::lock_guard<std::mutex> lock(s.mu);
        s.pending_connects.erase(id);
        if (s.connected.find(id) != s.connected.end()) return true;
        const auto it = s.conn_by_peer.find(id);
        if (it != s.conn_by_peer.end()) {
            const auto last = s.dialed_at.find(id);
            // No dial time: this is an incoming connection we accepted; it owns the peer until it
            // closes. Only a handle we dialed is replaced.
            if (last == s.dialed_at.end()) return true;
            if (std::chrono::steady_clock::now() - last->second < kRedialInterval) return true;
            stale = it->second;
        }
    }
    if (stale != k_HSteamNetConnection_Invalid) {
        bbnet::log("Steam P2P: connection %u to %llu never opened; re-dialing", static_cast<unsigned>(stale),
                   static_cast<unsigned long long>(id));
        drop_connection(s, id);
    }
    if (!s.sockets || !s.flat.connect_p2p) return false;
    SteamNetworkingIdentity ident;
    ident.SetSteamID64(id);
    const HSteamNetConnection conn = s.flat.connect_p2p(s.sockets, ident, kVirtualPort, 0, nullptr);
    if (conn == k_HSteamNetConnection_Invalid) {
        bbnet::log("Steam P2P: ConnectP2P to %llu returned an invalid connection",
                   static_cast<unsigned long long>(id));
        std::lock_guard<std::mutex> lock(s.mu);
        s.dialed_at[id] = std::chrono::steady_clock::now();
        return false;
    }
    if (!s.flat.set_conn_poll_group(s.sockets, conn, s.poll)) {
        bbnet::log("Steam P2P: SetConnectionPollGroup for %llu (connection %u) failed",
                   static_cast<unsigned long long>(id), static_cast<unsigned>(conn));
        s.flat.close_connection(s.sockets, conn, k_ESteamNetConnectionEnd_App_Generic, "poll group", false);
        std::lock_guard<std::mutex> lock(s.mu);
        s.dialed_at[id] = std::chrono::steady_clock::now();
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(s.mu);
        s.conn_by_peer[id] = conn;
        s.peer_by_conn[conn] = id;
        s.dialed_at[id] = std::chrono::steady_clock::now();
    }
    bbnet::log("Steam P2P: connecting to %llu (connection %u)", static_cast<unsigned long long>(id),
               static_cast<unsigned>(conn));
    return true;
}

void drop_connection(State& s, std::uint64_t id) {
    HSteamNetConnection conn = k_HSteamNetConnection_Invalid;
    {
        std::lock_guard<std::mutex> lock(s.mu);
        const auto it = s.conn_by_peer.find(id);
        if (it != s.conn_by_peer.end()) {
            conn = it->second;
            s.conn_by_peer.erase(it);
        }
        if (conn != k_HSteamNetConnection_Invalid) s.peer_by_conn.erase(conn);
        s.connected.erase(id);
    }
    if (conn != k_HSteamNetConnection_Invalid)
        s.flat.close_connection(s.sockets, conn, k_ESteamNetConnectionEnd_App_Generic, "replaced", false);
}

void on_status(SteamNetConnectionStatusChangedCallback_t* p) {
    State* s = g_state;
    if (!s) return;
    ISteamNetworkingSockets* sockets = s->sockets;
    if (!sockets) return;
    const std::uint64_t id = p->m_info.m_identityRemote.GetSteamID64();
    const bool incoming = p->m_info.m_hListenSocket != k_HSteamListenSocket_Invalid;
    const auto state = p->m_info.m_eState;
    bbnet::log("Steam P2P: connection %u %s (%d): peer %llu, incoming %d, listen %u, end %d%s%s",
               static_cast<unsigned>(p->m_hConn), connection_state_name(state), static_cast<int>(state),
               static_cast<unsigned long long>(id), incoming ? 1 : 0,
               static_cast<unsigned>(p->m_info.m_hListenSocket), p->m_info.m_eEndReason,
               p->m_info.m_szEndDebug[0] ? ": " : "", p->m_info.m_szEndDebug);
    if (!id) {
        bbnet::log("Steam P2P: closing connection %u: the peer has no SteamID",
                   static_cast<unsigned>(p->m_hConn));
        s->flat.close_connection(sockets, p->m_hConn, k_ESteamNetConnectionEnd_App_Generic, "not a SteamID", false);
        return;
    }

    if (state == k_ESteamNetworkingConnectionState_Connecting) {
        // Accept any incoming Steam P2P connection at the network level.
        // Admission is enforced by PartyLink's AUTH handshake and the party password
        // (BB_PARTY_PASSWORD); no Steam friendship is required.
        if (!incoming) {
            bbnet::log("Steam P2P: %llu is opening the connection", static_cast<unsigned long long>(id));
            return;
        }
        // Incoming connection on the listen socket: accept unconditionally at the Steam layer.
        // PartyLink's HELLO/AUTH handshake (password + party code) enforces authentication.
        {
            std::lock_guard<std::mutex> lock(s->mu);
            const auto it = s->conn_by_peer.find(id);
            if (it != s->conn_by_peer.end()) {
                if (it->second == p->m_hConn) return;
                bbnet::log("Steam P2P: closing duplicate incoming connection %u from %llu (keeping %u)",
                           static_cast<unsigned>(p->m_hConn), static_cast<unsigned long long>(id),
                           static_cast<unsigned>(it->second));
                s->flat.close_connection(sockets, p->m_hConn, k_ESteamNetConnectionEnd_App_Generic, "duplicate",
                                         false);
                return;
            }
        }
        const EResult accepted = s->flat.accept_connection(sockets, p->m_hConn);
        if (accepted != k_EResultOK) {
            bbnet::log("Steam P2P: accepting %llu (connection %u) failed: %d",
                       static_cast<unsigned long long>(id), static_cast<unsigned>(p->m_hConn),
                       static_cast<int>(accepted));
            s->flat.close_connection(sockets, p->m_hConn, k_ESteamNetConnectionEnd_App_Generic, "accept failed",
                                     false);
            return;
        }
        if (!s->flat.set_conn_poll_group(sockets, p->m_hConn, s->poll)) {
            bbnet::log("Steam P2P: SetConnectionPollGroup failed for accepted connection %u from %llu",
                       static_cast<unsigned>(p->m_hConn), static_cast<unsigned long long>(id));
            s->flat.close_connection(sockets, p->m_hConn, k_ESteamNetConnectionEnd_App_Generic, "poll group",
                                     false);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(s->mu);
            s->conn_by_peer[id] = p->m_hConn;
            s->peer_by_conn[p->m_hConn] = id;
            s->dialed_at.erase(id);
        }
        bbnet::log("Steam P2P: accepted connection %u from %llu", static_cast<unsigned>(p->m_hConn),
                   static_cast<unsigned long long>(id));
        return;
    }

    if (state == k_ESteamNetworkingConnectionState_Connected) {
        bool duplicate = false;
        {
            std::lock_guard<std::mutex> lock(s->mu);
            const auto it = s->conn_by_peer.find(id);
            if (it != s->conn_by_peer.end() && it->second != p->m_hConn && s->connected.find(id) != s->connected.end()) {
                duplicate = true;
            } else {
                s->conn_by_peer[id] = p->m_hConn;
                s->peer_by_conn[p->m_hConn] = id;
                s->dialed_at.erase(id);
                s->connected.insert(id);
                s->book.add(id, nullptr);
                s->events.push_back({id, true});
            }
        }
        if (duplicate) {
            bbnet::log("Steam P2P: closing duplicate connection %u to %llu",
                       static_cast<unsigned>(p->m_hConn), static_cast<unsigned long long>(id));
            s->flat.close_connection(sockets, p->m_hConn, k_ESteamNetConnectionEnd_App_Generic, "duplicate",
                                     false);
            return;
        }
        bbnet::log("Steam P2P: connected to %llu (connection %u)", static_cast<unsigned long long>(id),
                   static_cast<unsigned>(p->m_hConn));
        return;
    }

    if (state == k_ESteamNetworkingConnectionState_ClosedByPeer ||
        state == k_ESteamNetworkingConnectionState_ProblemDetectedLocally) {
        bool current = false;
        {
            std::lock_guard<std::mutex> lock(s->mu);
            const auto pc = s->peer_by_conn.find(p->m_hConn);
            if (pc != s->peer_by_conn.end()) {
                s->peer_by_conn.erase(pc);
                const auto it = s->conn_by_peer.find(id);
                if (it != s->conn_by_peer.end() && it->second == p->m_hConn) {
                    current = true;
                    s->conn_by_peer.erase(it);
                    s->connected.erase(id);
                    s->events.push_back({id, false});
                }
            }
        }
        if (current)
            bbnet::log("Steam P2P: connection to %llu closed (%s, end %d%s%s)", static_cast<unsigned long long>(id),
                       connection_state_name(state), p->m_info.m_eEndReason,
                       p->m_info.m_szEndDebug[0] ? ": " : "", p->m_info.m_szEndDebug);
        else
            bbnet::log("Steam P2P: stale connection %u to %llu closed", static_cast<unsigned>(p->m_hConn),
                       static_cast<unsigned long long>(id));
        s->flat.close_connection(sockets, p->m_hConn, k_ESteamNetConnectionEnd_App_Generic, "closed", false);
    }
}

// Every 5 s: the state of connections that have not reached Connected yet, so a stuck
// ConnectP2P (no status callback, no route) is visible in the log.
void log_pending_connections(State& s) {
    if (!s.flat.get_connection_info || !s.sockets) return;
    std::vector<std::pair<std::uint64_t, HSteamNetConnection>> pending;
    {
        std::lock_guard<std::mutex> lock(s.mu);
        for (const auto& [peer, conn] : s.conn_by_peer)
            if (s.connected.find(peer) == s.connected.end()) pending.emplace_back(peer, conn);
    }
    for (const auto& [peer, conn] : pending) {
        SteamNetConnectionInfo_t info{};
        if (s.flat.get_connection_info(s.sockets, conn, &info)) {
            bbnet::log("Steam P2P: pending %llu (connection %u): %s, end %d%s%s",
                       static_cast<unsigned long long>(peer), static_cast<unsigned>(conn),
                       connection_state_name(info.m_eState), info.m_eEndReason,
                       info.m_szEndDebug[0] ? ": " : "", info.m_szEndDebug);
        } else {
            bbnet::log("Steam P2P: pending %llu (connection %u): GetConnectionInfo failed",
                       static_cast<unsigned long long>(peer), static_cast<unsigned>(conn));
        }
    }
}

void finish_failed(State& s, HMODULE dll, bool initialized) {
    if (initialized) {
        std::lock_guard<std::mutex> lock(s.api_mu);
        if (s.friends && s.flat.clear_rich_presence) s.flat.clear_rich_presence(s.friends);
        if (s.join_callback_registered && s.unregister_callback) {
            s.unregister_callback(&g_join_callback);
            s.join_callback_registered = false;
        }
        if (s.net_utils && s.flat.set_status_cb) s.flat.set_status_cb(s.net_utils, nullptr);
        if (s.sockets) {
            for (const auto& entry : s.peer_by_conn)
                if (s.flat.close_connection)
                    s.flat.close_connection(s.sockets, entry.first, k_ESteamNetConnectionEnd_App_Generic, "shutdown",
                                            false);
            if (s.listen != k_HSteamListenSocket_Invalid && s.flat.close_listen_socket)
                s.flat.close_listen_socket(s.sockets, s.listen);
            if (s.poll != k_HSteamNetPollGroup_Invalid && s.flat.destroy_poll_group)
                s.flat.destroy_poll_group(s.sockets, s.poll);
        }
        s.user = nullptr;
        s.friends = nullptr;
        s.utils = nullptr;
        s.net_utils = nullptr;
        s.sockets = nullptr;
        s.listen = k_HSteamListenSocket_Invalid;
        s.poll = k_HSteamNetPollGroup_Invalid;
        if (s.shutdown) s.shutdown();
    }
    if (dll) FreeLibrary(dll);
    g_state = nullptr;
}

void thread_main(Config config) {
    State& s = *g_state;
    HMODULE dll = LoadLibraryW(L"steam_api64.dll");
    if (!dll) {
        publish_failure(s, "steam_api64.dll was not found; Steam P2P needs the Steam client installed");
        g_state = nullptr;
        return;
    }
    s.init_flat = reinterpret_cast<InitFlatFn>(GetProcAddress(dll, "SteamAPI_InitFlat"));
    s.shutdown = reinterpret_cast<ShutdownFn>(GetProcAddress(dll, "SteamAPI_Shutdown"));
    s.run_callbacks = reinterpret_cast<RunCallbacksFn>(GetProcAddress(dll, "SteamAPI_RunCallbacks"));
    s.register_callback = reinterpret_cast<RegisterCallbackFn>(GetProcAddress(dll, "SteamAPI_RegisterCallback"));
    s.unregister_callback =
        reinterpret_cast<UnregisterCallbackFn>(GetProcAddress(dll, "SteamAPI_UnregisterCallback"));
    const auto user_fn = reinterpret_cast<UserFn>(GetProcAddress(dll, "SteamAPI_SteamUser_v023"));
    const auto friends_fn = reinterpret_cast<FriendsFn>(GetProcAddress(dll, "SteamAPI_SteamFriends_v017"));
    const auto utils_fn = reinterpret_cast<UtilsFn>(GetProcAddress(dll, "SteamAPI_SteamUtils_v010"));
    const auto net_utils_fn =
        reinterpret_cast<NetUtilsFn>(GetProcAddress(dll, "SteamAPI_SteamNetworkingUtils_SteamAPI_v004"));
    const auto sockets_fn =
        reinterpret_cast<SocketsFn>(GetProcAddress(dll, "SteamAPI_SteamNetworkingSockets_SteamAPI_v012"));
    if (!s.init_flat || !s.shutdown || !s.run_callbacks || !s.register_callback || !s.unregister_callback ||
        !user_fn || !friends_fn || !utils_fn || !net_utils_fn || !sockets_fn) {
        publish_failure(s, "the installed steam_api64.dll is too old for this Steam P2P build");
        FreeLibrary(dll);
        g_state = nullptr;
        return;
    }
    if (const char* missing = s.flat.load(dll)) {
        publish_failure(s, std::string("the installed steam_api64.dll is missing flat API export: ") + missing);
        FreeLibrary(dll);
        g_state = nullptr;
        return;
    }

    const std::string appid = std::to_string(config.appid);
    SetEnvironmentVariableA("SteamAppId", appid.c_str());
    SetEnvironmentVariableA("SteamGameId", appid.c_str());
    SteamErrMsg msg{};
    if (s.init_flat(&msg) != k_ESteamAPIInitResult_OK) {
        publish_failure(s, steam_error(msg));
        FreeLibrary(dll);
        g_state = nullptr;
        return;
    }

    ISteamUser* user = user_fn();
    ISteamFriends* friends = friends_fn();
    ISteamUtils* utils = utils_fn();
    ISteamNetworkingUtils* net_utils = net_utils_fn();
    ISteamNetworkingSockets* sockets = sockets_fn();
    if (!user || !friends || !utils || !net_utils || !sockets || !s.flat.b_logged_on(user)) {
        publish_failure(s, "Steam is not logged in");
        finish_failed(s, dll, true);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(s.api_mu);
        s.user = user;
        s.friends = friends;
        s.utils = utils;
        s.net_utils = net_utils;
        s.sockets = sockets;
    }

    const std::uint64_t local = s.flat.get_steamid(user);
    if (!local) {
        publish_failure(s, "Steam did not return a SteamID");
        finish_failed(s, dll, true);
        return;
    }

    s.register_callback(&g_join_callback, GameRichPresenceJoinRequested_t::k_iCallback);
    s.join_callback_registered = true;

    if (!s.flat.set_status_cb(net_utils, &on_status)) {
        publish_failure(s, "Steam rejected the P2P connection status callback");
        finish_failed(s, dll, true);
        return;
    }
    bbnet::log("Steam P2P: connection status callback installed; local SteamID %llu",
               static_cast<unsigned long long>(local));
    s.flat.init_relay(net_utils);
    // pDetails is optional (the vendored header only forward-declares the struct).
    const ESteamNetworkingAvailability availability = s.flat.relay_status(net_utils, nullptr);
    bbnet::log("Steam P2P: relay network status %d (SteamNetworkingAvailability)", static_cast<int>(availability));

    if (config.host) {
        s.listen = s.flat.create_listen_p2p(sockets, kVirtualPort, 0, nullptr);
        s.poll = s.flat.create_poll_group(sockets);
        bbnet::log("Steam P2P: listen socket %u, poll group %u", static_cast<unsigned>(s.listen),
                   static_cast<unsigned>(s.poll));
        if (s.listen == k_HSteamListenSocket_Invalid || s.poll == k_HSteamNetPollGroup_Invalid) {
            publish_failure(s, "could not open the Steam P2P listen socket");
            finish_failed(s, dll, true);
            return;
        }
        char connect[64];
        std::snprintf(connect, sizeof connect, "+connect steam:%llu", static_cast<unsigned long long>(local));
        char group[32];
        std::snprintf(group, sizeof group, "%llu", static_cast<unsigned long long>(local));
        const bool connect_set = s.flat.set_rich_presence(friends, "connect", connect);
        s.flat.set_rich_presence(friends, "status", "Nightwatch Co-op");
        s.flat.set_rich_presence(friends, "steam_player_group", group);
        s.flat.set_rich_presence(friends, "steam_player_group_size", "1");
        bbnet::log("Steam P2P: rich presence \"%s\" %s", connect, connect_set ? "set" : "REJECTED");
    } else {
        s.poll = s.flat.create_poll_group(sockets);
        bbnet::log("Steam P2P: guest poll group %u", static_cast<unsigned>(s.poll));
        if (s.poll == k_HSteamNetPollGroup_Invalid) {
            publish_failure(s, "could not create the Steam P2P poll group");
            finish_failed(s, dll, true);
            return;
        }
    }

    {
        std::lock_guard<std::mutex> lock(s.mu);
        s.local_steamid = local;
        s.host_steamid = config.host ? local : config.host_steamid;
        s.host = config.host;
        s.appid = config.appid;
        s.book.add(local, nullptr);
        s.starting = false;
        s.failed = false;
        s.status = "Steam P2P ready";
        s.running = true;
    }
    s.cv.notify_all();
    bbnet::log("Steam P2P: ready as %s (host SteamID %llu, poll %u)", config.host ? "host" : "guest",
               static_cast<unsigned long long>(config.host ? local : config.host_steamid),
               static_cast<unsigned>(s.poll));

    auto last_pending_log = std::chrono::steady_clock::now();
    while (!s.stopping.load(std::memory_order_relaxed)) {
        std::vector<std::uint64_t> connects;
        {
            std::lock_guard<std::mutex> lock(s.mu);
            while (!s.connect_queue.empty()) {
                connects.push_back(s.connect_queue.front());
                s.connect_queue.pop_front();
            }
        }
        for (const std::uint64_t id : connects)
            if (id != local) connect_internal(s, id);

        s.run_callbacks();

        SteamNetworkingMessage_t* messages[kMaxMessages];
        const int got = s.flat.receive_messages(sockets, s.poll, messages, kMaxMessages);
        for (int i = 0; i < got; ++i) {
            SteamNetworkingMessage_t* m = messages[i];
            std::uint64_t id = 0;
            {
                std::lock_guard<std::mutex> lock(s.mu);
                const auto it = s.peer_by_conn.find(m->m_conn);
                if (it != s.peer_by_conn.end()) id = it->second;
            }
            if (id && m->m_cbSize > 0) {
                Datagram d;
                d.steamid = id;
                const auto* bytes = static_cast<const std::uint8_t*>(m->m_pData);
                d.bytes.assign(bytes, bytes + m->m_cbSize);
                bool first = false;
                {
                    std::lock_guard<std::mutex> lock(s.mu);
                    first = s.heard_from.insert(id).second;
                    ++s.rx_datagrams;
                    s.rx_bytes += m->m_cbSize;
                    s.datagrams.push_back(std::move(d));
                }
                if (first)
                    bbnet::log("Steam P2P: receiving from %llu (%u bytes)",
                               static_cast<unsigned long long>(id), static_cast<unsigned>(m->m_cbSize));
            }
            m->Release();
        }
        if (std::chrono::steady_clock::now() - last_pending_log >= std::chrono::seconds(5)) {
            last_pending_log = std::chrono::steady_clock::now();
            log_pending_connections(s);
        }
        std::this_thread::sleep_for(kPollInterval);
    }

    {
        std::lock_guard<std::mutex> lock(s.mu);
        s.running = false;
        s.status = "Steam P2P stopped";
        s.connected.clear();
        s.conn_by_peer.clear();
        s.pending_connects.clear();
        s.dialed_at.clear();
        s.heard_from.clear();
    }
    finish_failed(s, dll, true);
}

}  // namespace

SteamNet::SteamNet() = default;
SteamNet::~SteamNet() { stop(); }

SteamNet& SteamNet::instance() {
    static SteamNet net;
    return net;
}

bool SteamNet::start(const Config& config, std::string* error) {
    if (!config.appid) {
        if (error) *error = "Steam AppID is required";
        return false;
    }
    if (g_state) {
        if (error) *error = "Steam P2P is already running";
        return false;
    }
    auto* s = new State;
    s->appid = config.appid;
    s->host = config.host;
    s->host_steamid = config.host_steamid;
    s->starting = true;
    g_state = s;
    s->thread = std::thread(thread_main, config);

    std::unique_lock<std::mutex> lock(s->mu);
    const bool ready = s->cv.wait_for(lock, kStartTimeout, [&] { return s->failed || !s->starting; });
    if (!ready || s->failed) {
        const std::string why = ready ? s->error : "Steam P2P initialization timed out";
        if (error) *error = why;
        lock.unlock();
        s->stopping = true;
        if (s->thread.joinable()) s->thread.join();
        if (g_state == s) g_state = nullptr;
        delete s;
        return false;
    }
    return true;
}

void SteamNet::stop() {
    State* s = g_state;
    if (!s) return;
    s->stopping = true;
    if (s->thread.joinable()) s->thread.join();
    if (g_state == s) g_state = nullptr;
    delete s;
}

void SteamNet::tick() {}

bool SteamNet::set_connect_string(const std::string& connect) {
    State* s = g_state;
    if (!s || !s->friends) return false;
    std::lock_guard<std::mutex> lock(s->api_mu);
    if (!s->flat.set_rich_presence) return false;
    return s->flat.set_rich_presence(s->friends, "connect", connect.c_str());
}

void SteamNet::clear_connect_string() {
    State* s = g_state;
    if (!s || !s->friends) return;
    std::lock_guard<std::mutex> lock(s->api_mu);
    if (s->flat.clear_rich_presence) s->flat.clear_rich_presence(s->friends);
}

bool SteamNet::activate_invite_overlay(const std::string& connect) {
    State* s = g_state;
    if (!s || !s->friends) return false;
    std::lock_guard<std::mutex> lock(s->api_mu);
    if (!s->flat.activate_invite) return false;
    s->flat.activate_invite(s->friends, connect.c_str());
    return true;
}

bool SteamNet::running() const { return g_state && g_state->running.load(); }

std::string SteamNet::status() const {
    State* s = g_state;
    if (!s) return "Steam P2P is off";
    std::lock_guard<std::mutex> lock(s->mu);
    return s->status;
}

bool SteamNet::host() const {
    State* s = g_state;
    if (!s) return false;
    std::lock_guard<std::mutex> lock(s->mu);
    return s->host;
}

std::uint32_t SteamNet::appid() const {
    State* s = g_state;
    if (!s) return 0;
    std::lock_guard<std::mutex> lock(s->mu);
    return s->appid;
}

std::uint64_t SteamNet::local_steamid() const {
    State* s = g_state;
    if (!s) return 0;
    std::lock_guard<std::mutex> lock(s->mu);
    return s->local_steamid;
}

std::uint64_t SteamNet::host_steamid() const {
    State* s = g_state;
    if (!s) return 0;
    std::lock_guard<std::mutex> lock(s->mu);
    return s->host_steamid;
}

bool SteamNet::join_host(std::uint64_t steamid) {
    State* s = g_state;
    if (!s || !steamid) return false;
    std::lock_guard<std::mutex> lock(s->mu);
    if (!s->running.load() || steamid == s->local_steamid) return false;
    // A Steam host that receives an overlay Join Game request switches to guest: stop
    // advertising/listening, then connect to the friend's host. The poll group stays.
    const bool was_host = s->host;
    if (!was_host && s->host_steamid && s->host_steamid != steamid) {
        s->status = "Steam P2P is already joining another host";
        return false;
    }
    std::string error;
    if (!s->book.add(steamid, nullptr, &error)) {
        s->status = error;
        return false;
    }
    if (was_host && s->sockets) {
        if (s->listen != k_HSteamListenSocket_Invalid && s->flat.close_listen_socket)
            s->flat.close_listen_socket(s->sockets, s->listen);
        s->listen = k_HSteamListenSocket_Invalid;
        if (s->friends && s->flat.clear_rich_presence) s->flat.clear_rich_presence(s->friends);
        s->host = false;
        bbnet::log("Steam P2P: stopped hosting (listen socket closed) to join %llu",
                   static_cast<unsigned long long>(steamid));
    }
    s->host_steamid = steamid;
    if (s->connected.find(steamid) == s->connected.end() && s->pending_connects.insert(steamid).second)
        s->connect_queue.push_back(steamid);
    return true;
}

bool SteamNet::pop_join_request(JoinRequest* out) {
    State* s = g_state;
    if (!s || !out) return false;
    std::lock_guard<std::mutex> lock(s->mu);
    if (s->join_requests.empty()) return false;
    *out = std::move(s->join_requests.front());
    s->join_requests.pop_front();
    return true;
}

bool SteamNet::connect_peer(std::uint64_t steamid) {
    State* s = g_state;
    if (!s || !s->running.load() || !steamid || steamid == s->local_steamid) return false;
    std::lock_guard<std::mutex> lock(s->mu);
    if (s->connected.find(steamid) != s->connected.end()) return true;
    // A handle in conn_by_peer is not enough: the worker replaces it when it never reached
    // Connected, so every reconnect attempt re-issues ConnectP2P.
    if (s->pending_connects.insert(steamid).second) s->connect_queue.push_back(steamid);
    return true;
}

bool SteamNet::disconnect_peer(std::uint64_t steamid) {
    State* s = g_state;
    if (!s || !steamid) return false;
    HSteamNetConnection conn = k_HSteamNetConnection_Invalid;
    {
        std::lock_guard<std::mutex> lock(s->mu);
        const auto it = s->conn_by_peer.find(steamid);
        if (it != s->conn_by_peer.end()) conn = it->second;
    }
    {
        std::lock_guard<std::mutex> lock(s->api_mu);
        if (conn != k_HSteamNetConnection_Invalid && s->flat.close_connection)
            s->flat.close_connection(s->sockets, conn, k_ESteamNetConnectionEnd_App_Generic, "peer left", false);
    }
    {
        std::lock_guard<std::mutex> lock(s->mu);
        s->conn_by_peer.erase(steamid);
        s->peer_by_conn.erase(conn);
        s->pending_connects.erase(steamid);
        s->connected.erase(steamid);
        s->trusted.erase(steamid);
        s->heard_from.erase(steamid);
    }
    return true;
}

bool SteamNet::send(std::uint64_t steamid, const void* data, std::size_t size) {
    State* s = g_state;
    if (!s || !data || !size || size > 512u * 1024u) return false;
    HSteamNetConnection conn = k_HSteamNetConnection_Invalid;
    {
        std::lock_guard<std::mutex> lock(s->mu);
        const auto it = s->conn_by_peer.find(steamid);
        if (it == s->conn_by_peer.end()) return false;
        conn = it->second;
    }
    std::lock_guard<std::mutex> lock(s->api_mu);
    if (!s->sockets || !s->flat.send_message) return false;
    const EResult result = s->flat.send_message(s->sockets, conn, data, static_cast<std::uint32_t>(size),
                                                k_nSteamNetworkingSend_UnreliableNoDelay, nullptr);
    if (result == k_EResultOK) {
        std::lock_guard<std::mutex> ml(s->mu);
        ++s->tx_datagrams;
        s->tx_bytes += size;
        return true;
    }
    std::uint64_t failures = 0;
    {
        std::lock_guard<std::mutex> ml(s->mu);
        failures = ++s->tx_failures;
    }
    if (failures == 1 || failures % 100 == 0)
        bbnet::log("Steam P2P: send to %llu (connection %u) failed: %d (%llu failures)",
                   static_cast<unsigned long long>(steamid), static_cast<unsigned>(conn),
                   static_cast<int>(result), static_cast<unsigned long long>(failures));
    // A dead handle would fail every send: drop it so the next reconnect re-dials.
    if (result == k_EResultNoConnection) drop_connection(*s, steamid);
    return false;
}

bool SteamNet::broadcast(const void* data, std::size_t size) {
    State* s = g_state;
    if (!s) return false;
    std::vector<std::uint64_t> peers;
    {
        std::lock_guard<std::mutex> lock(s->mu);
        peers.assign(s->connected.begin(), s->connected.end());
    }
    bool any = false;
    for (const std::uint64_t id : peers) any = send(id, data, size) || any;
    return any;
}

bool SteamNet::pop_datagram(Datagram* out) {
    State* s = g_state;
    if (!s || !out) return false;
    std::lock_guard<std::mutex> lock(s->mu);
    if (s->datagrams.empty()) return false;
    *out = std::move(s->datagrams.front());
    s->datagrams.pop_front();
    return true;
}

bool SteamNet::pop_event(PeerEvent* out) {
    State* s = g_state;
    if (!s || !out) return false;
    std::lock_guard<std::mutex> lock(s->mu);
    if (s->events.empty()) return false;
    *out = s->events.front();
    s->events.pop_front();
    return true;
}

bool SteamNet::is_connected(std::uint64_t steamid) const {
    State* s = g_state;
    if (!s) return false;
    std::lock_guard<std::mutex> lock(s->mu);
    return s->connected.find(steamid) != s->connected.end();
}

void SteamNet::trust_peer(std::uint64_t steamid) {
    State* s = g_state;
    if (!s || !steamid) return;
    std::lock_guard<std::mutex> lock(s->mu);
    s->trusted.insert(steamid);
}

void SteamNet::set_trusted_peers(const std::vector<std::uint64_t>& steamids) {
    State* s = g_state;
    if (!s) return;
    std::lock_guard<std::mutex> lock(s->mu);
    s->trusted.clear();
    for (const std::uint64_t id : steamids)
        if (id && id != s->local_steamid) s->trusted.insert(id);
}

bool SteamNet::is_trusted(std::uint64_t steamid) const {
    State* s = g_state;
    if (!s || !steamid) return false;
    std::lock_guard<std::mutex> lock(s->mu);
    return s->trusted.find(steamid) != s->trusted.end();
}

bool SteamNet::is_friend(std::uint64_t steamid) const {
    State* s = g_state;
    if (!s || !steamid) return false;
    std::lock_guard<std::mutex> lock(s->api_mu);
    return friend_of(s->flat, s->friends, steamid);
}

std::vector<Friend> SteamNet::friends() const {
    std::vector<Friend> out;
    State* s = g_state;
    if (!s) return out;
    std::lock_guard<std::mutex> lock(s->api_mu);
    if (!s->friends) return out;
    const int count = s->flat.get_friend_count(s->friends, k_EFriendFlagImmediate);
    for (int i = 0; i < count; ++i) {
        const std::uint64_t id = s->flat.get_friend_by_index(s->friends, i, k_EFriendFlagImmediate);
        Friend f;
        f.steamid = id;
        const char* name = s->flat.get_friend_persona_name(s->friends, id);
        f.name = name ? name : "";
        const EPersonaState state = s->flat.get_friend_persona_state(s->friends, id);
        f.online = state != k_EPersonaStateOffline && state != k_EPersonaStateInvisible;
        FriendGameInfo_t game{};
        f.in_game = s->flat.get_friend_game_played(s->friends, id, &game) && game.m_gameID.IsValid();
        out.push_back(std::move(f));
    }
    return out;
}

std::string SteamNet::peer_name(std::uint64_t steamid) const {
    State* s = g_state;
    if (!s || !steamid) return {};
    std::lock_guard<std::mutex> lock(s->api_mu);
    if (!s->friends) return {};
    const char* name = s->flat.get_friend_persona_name(s->friends, steamid);
    return name ? name : "";
}

bool SteamNet::learn_peer(std::uint64_t steamid) {
    State* s = g_state;
    if (!s || !steamid) return false;
    std::string error;
    std::lock_guard<std::mutex> lock(s->mu);
    if (!s->book.add(steamid, nullptr, &error)) {
        s->status = error;
        return false;
    }
    return true;
}

bool SteamNet::virtual_for_steamid(std::uint64_t steamid, std::uint32_t* nbo) const {
    State* s = g_state;
    if (!s || !steamid) return false;
    std::lock_guard<std::mutex> lock(s->mu);
    return s->book.address_of(steamid, nbo);
}

bool SteamNet::steamid_for_virtual(std::uint32_t nbo, std::uint64_t* steamid) const {
    State* s = g_state;
    if (!s) return false;
    std::lock_guard<std::mutex> lock(s->mu);
    return s->book.steamid_of(nbo, steamid);
}

}  // namespace bbnet::steam

#else

#include <cstdlib>
#include <deque>
#include <fstream>
#include <mutex>
#include <set>

namespace bbnet::steam {

#if defined(BB_STEAM_STUB)

namespace {
struct StubState {
    std::mutex mu;
    bool running = false;
    bool host = true;
    std::uint32_t appid = 480;
    std::uint64_t local_steamid = 76561198000000001ull;
    std::uint64_t host_steamid = 0;
    AddressBook book;
    std::set<std::uint64_t> connected;
    std::set<std::uint64_t> trusted;
    std::deque<Datagram> datagrams;
    std::deque<PeerEvent> events;
    std::deque<JoinRequest> joins;
    std::string status = "Steam P2P stub";
};
StubState& stub() {
    static StubState s;
    return s;
}
}  // namespace

SteamNet::SteamNet() = default;
SteamNet::~SteamNet() = default;
SteamNet& SteamNet::instance() {
    static SteamNet net;
    return net;
}

bool SteamNet::start(const Config& config, std::string* error) {
    StubState& s = stub();
    std::lock_guard<std::mutex> lock(s.mu);
    if (s.running) {
        if (error) *error = "Steam P2P stub is already running";
        return false;
    }
    s.running = true;
    s.host = config.host;
    s.appid = config.appid ? config.appid : 480;
    s.host_steamid = config.host_steamid;
    s.book.clear();
    s.connected.clear();
    s.trusted.clear();
    s.datagrams.clear();
    s.events.clear();
    s.joins.clear();
    s.status = "Steam P2P stub";
    if (!s.host && s.host_steamid) {
        s.connected.insert(s.host_steamid);
        s.book.add(s.host_steamid, nullptr);
        s.events.push_back(PeerEvent{s.host_steamid, true});
    }
    return true;
}

void SteamNet::stop() {
    StubState& s = stub();
    std::lock_guard<std::mutex> lock(s.mu);
    s.running = false;
    s.connected.clear();
    s.trusted.clear();
    s.datagrams.clear();
    s.events.clear();
    s.joins.clear();
    s.book.clear();
}

void SteamNet::tick() {}

bool SteamNet::set_connect_string(const std::string&) { return stub().running; }
void SteamNet::clear_connect_string() {}
bool SteamNet::activate_invite_overlay(const std::string&) { return stub().running; }

bool SteamNet::running() const { return stub().running; }
std::string SteamNet::status() const { return stub().status; }
bool SteamNet::host() const { return stub().host; }
std::uint32_t SteamNet::appid() const { return stub().appid; }
std::uint64_t SteamNet::local_steamid() const { return stub().local_steamid; }
std::uint64_t SteamNet::host_steamid() const { return stub().host_steamid; }

bool SteamNet::join_host(std::uint64_t steamid) {
    StubState& s = stub();
    std::lock_guard<std::mutex> lock(s.mu);
    if (!s.running || !steamid || steamid == s.local_steamid) return false;
    s.host = false;
    s.host_steamid = steamid;
    if (s.connected.insert(steamid).second) {
        s.book.add(steamid, nullptr);
        s.events.push_back(PeerEvent{steamid, true});
    }
    return true;
}

bool SteamNet::pop_join_request(JoinRequest* out) {
    StubState& s = stub();
    std::lock_guard<std::mutex> lock(s.mu);
    if (!out || s.joins.empty()) return false;
    *out = std::move(s.joins.front());
    s.joins.pop_front();
    return true;
}

bool SteamNet::connect_peer(std::uint64_t steamid) {
    StubState& s = stub();
    std::lock_guard<std::mutex> lock(s.mu);
    if (!s.running || !steamid || steamid == s.local_steamid) return false;
    s.book.add(steamid, nullptr);
    if (s.connected.insert(steamid).second) s.events.push_back(PeerEvent{steamid, true});
    return true;
}

bool SteamNet::disconnect_peer(std::uint64_t steamid) {
    StubState& s = stub();
    std::lock_guard<std::mutex> lock(s.mu);
    if (!s.running || !steamid || steamid == s.local_steamid) return false;
    const bool was = s.connected.erase(steamid) != 0;
    s.trusted.erase(steamid);
    for (auto it = s.datagrams.begin(); it != s.datagrams.end();) {
        if (it->steamid == steamid)
            it = s.datagrams.erase(it);
        else
            ++it;
    }
    if (was) s.events.push_back(PeerEvent{steamid, false});
    return was;
}

bool SteamNet::send(std::uint64_t steamid, const void* data, std::size_t size) {
    StubState& s = stub();
    std::lock_guard<std::mutex> lock(s.mu);
    if (!s.running || !data || !size || size > 512u * 1024u) return false;
    if (steamid != s.local_steamid && s.connected.find(steamid) == s.connected.end()) return false;
    Datagram d;
    d.steamid = steamid;
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    d.bytes.assign(bytes, bytes + size);
    s.datagrams.push_back(std::move(d));
    return true;
}

bool SteamNet::broadcast(const void* data, std::size_t size) {
    StubState& s = stub();
    std::lock_guard<std::mutex> lock(s.mu);
    if (!s.running || !data || !size || size > 512u * 1024u) return false;
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (const std::uint64_t id : s.connected) {
        Datagram d;
        d.steamid = id;
        d.bytes.assign(bytes, bytes + size);
        s.datagrams.push_back(std::move(d));
    }
    return true;
}

bool SteamNet::pop_datagram(Datagram* out) {
    StubState& s = stub();
    std::lock_guard<std::mutex> lock(s.mu);
    if (!out || s.datagrams.empty()) return false;
    *out = std::move(s.datagrams.front());
    s.datagrams.pop_front();
    return true;
}

bool SteamNet::pop_event(PeerEvent* out) {
    StubState& s = stub();
    std::lock_guard<std::mutex> lock(s.mu);
    if (!out || s.events.empty()) return false;
    *out = s.events.front();
    s.events.pop_front();
    return true;
}

bool SteamNet::is_connected(std::uint64_t steamid) const {
    const StubState& s = stub();
    return s.running && (steamid == s.local_steamid || s.connected.find(steamid) != s.connected.end());
}

void SteamNet::trust_peer(std::uint64_t steamid) {
    StubState& s = stub();
    std::lock_guard<std::mutex> lock(s.mu);
    if (steamid && steamid != s.local_steamid) s.trusted.insert(steamid);
}

void SteamNet::set_trusted_peers(const std::vector<std::uint64_t>& steamids) {
    StubState& s = stub();
    std::lock_guard<std::mutex> lock(s.mu);
    s.trusted.clear();
    for (const std::uint64_t id : steamids)
        if (id && id != s.local_steamid) s.trusted.insert(id);
}

bool SteamNet::is_trusted(std::uint64_t steamid) const {
    StubState& s = stub();
    std::lock_guard<std::mutex> lock(s.mu);
    return s.trusted.find(steamid) != s.trusted.end();
}

bool SteamNet::is_friend(std::uint64_t steamid) const { return steamid != 0; }

std::vector<Friend> SteamNet::friends() const { return {}; }

std::string SteamNet::peer_name(std::uint64_t steamid) const { return std::to_string(steamid); }

bool SteamNet::learn_peer(std::uint64_t steamid) {
    StubState& s = stub();
    std::lock_guard<std::mutex> lock(s.mu);
    if (!steamid) return false;
    s.book.add(steamid, nullptr);
    return true;
}

bool SteamNet::virtual_for_steamid(std::uint64_t steamid, std::uint32_t* nbo) const {
    const StubState& s = stub();
    return steamid && s.book.address_of(steamid, nbo);
}

bool SteamNet::steamid_for_virtual(std::uint32_t nbo, std::uint64_t* steamid) const {
    const StubState& s = stub();
    return s.book.steamid_of(nbo, steamid);
}

#else

SteamNet::SteamNet() = default;
SteamNet::~SteamNet() = default;
SteamNet& SteamNet::instance() {
    static SteamNet net;
    return net;
}
bool SteamNet::start(const Config&, std::string* error) {
    if (error) *error = "Steam P2P is only supported on Windows";
    return false;
}
void SteamNet::stop() {}
void SteamNet::tick() {}
bool SteamNet::set_connect_string(const std::string&) { return false; }
void SteamNet::clear_connect_string() {}
bool SteamNet::activate_invite_overlay(const std::string&) { return false; }
bool SteamNet::running() const { return false; }
std::string SteamNet::status() const { return "Steam P2P is only supported on Windows"; }
bool SteamNet::host() const { return false; }
std::uint32_t SteamNet::appid() const { return 0; }
std::uint64_t SteamNet::local_steamid() const { return 0; }
std::uint64_t SteamNet::host_steamid() const { return 0; }
bool SteamNet::join_host(std::uint64_t) { return false; }
bool SteamNet::pop_join_request(JoinRequest*) { return false; }
bool SteamNet::connect_peer(std::uint64_t) { return false; }
bool SteamNet::send(std::uint64_t, const void*, std::size_t) { return false; }
bool SteamNet::broadcast(const void*, std::size_t) { return false; }
bool SteamNet::pop_datagram(Datagram*) { return false; }
bool SteamNet::pop_event(PeerEvent*) { return false; }
bool SteamNet::is_connected(std::uint64_t) const { return false; }
bool SteamNet::disconnect_peer(std::uint64_t) { return false; }
void SteamNet::trust_peer(std::uint64_t) {}
void SteamNet::set_trusted_peers(const std::vector<std::uint64_t>&) {}
bool SteamNet::is_trusted(std::uint64_t) const { return false; }
bool SteamNet::is_friend(std::uint64_t) const { return false; }
std::vector<Friend> SteamNet::friends() const { return {}; }
std::string SteamNet::peer_name(std::uint64_t) const { return {}; }
bool SteamNet::learn_peer(std::uint64_t) { return false; }
bool SteamNet::virtual_for_steamid(std::uint64_t, std::uint32_t*) const { return false; }
bool SteamNet::steamid_for_virtual(std::uint32_t, std::uint64_t*) const { return false; }

#endif

}  // namespace bbnet::steam

#endif

namespace bbnet {

namespace {

std::uint32_t steam_appid_from_env_or_file() {
    if (const char* value = std::getenv("BB_STEAM_APPID")) {
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(value, &end, 10);
        if (end && *end == '\0' && parsed > 0 && parsed <= 0xfffffffful)
            return static_cast<std::uint32_t>(parsed);
    }
    std::ifstream file("steam_appid.txt");
    if (!file) file.open("engine/steam_appid.txt");
    std::uint32_t appid = 480;
    if (file) {
        std::string text;
        std::getline(file, text);
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(text.c_str(), &end, 10);
        if (end != text.c_str() && parsed > 0 && parsed <= 0xfffffffful)
            appid = static_cast<std::uint32_t>(parsed);
    }
    return appid;
}

std::uint64_t steamid_from_env(const char* name) {
    const char* value = std::getenv(name);
    if (!value || !*value) return 0;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value, &end, 10);
    return end && *end == '\0' ? static_cast<std::uint64_t>(parsed) : 0;
}

bool connect_string_chars_ok(const std::string& s) {
    for (const unsigned char c : s) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                        c == '_' || c == '.' || c == '-' || c == '~' || c == '=' || c == '&';
        if (!ok) return false;
    }
    return true;
}

}  // namespace

struct SteamPartyLink::Impl {
    party::PartyLink* link = nullptr;
};

SteamPartyLink::SteamPartyLink() : impl_(new Impl) {}

SteamPartyLink::~SteamPartyLink() { shutdown(); }

std::unique_ptr<SteamPartyLink> SteamPartyLink::create(std::string* error) {
    steam::Config config;
    config.appid = steam_appid_from_env_or_file();
    const char* party = std::getenv("BB_PARTY");
    config.host = !(party && std::string(party) == "join");
    if (const char* host = std::getenv("BB_PARTY_HOST")) config.host = host[0] != '0';
    config.host_steamid = steamid_from_env("BB_STEAM_HOST");
    if (!config.host_steamid) config.host_steamid = steamid_from_env("BB_PARTY_STEAM_HOST");
    if (!config.host_steamid) config.host_steamid = steamid_from_env("BB_PARTY_CODE");
    std::string why;
    if (!steam::SteamNet::instance().start(config, &why)) {
        if (error) *error = why;
        return nullptr;
    }
    return std::unique_ptr<SteamPartyLink>(new SteamPartyLink());
}

void SteamPartyLink::wire(party::PartyLink& link) { impl_->link = &link; }

void SteamPartyLink::tick() { steam::SteamNet::instance().tick(); }

void SteamPartyLink::shutdown() { steam::SteamNet::instance().stop(); }

std::string SteamPartyPresence::encode_connect_string(std::uint64_t host_steamid,
                                                      bool password_required) {
    if (!host_steamid) return {};
    std::string out = "sid=" + std::to_string(host_steamid);
    out += password_required ? "&pw=1" : "&pw=0";
    if (out.size() > 256 || !connect_string_chars_ok(out)) return {};
    return out;
}

bool SteamPartyPresence::decode_connect_string(const std::string& s, std::uint64_t* host_steamid,
                                               bool* password_required) {
    if (host_steamid) *host_steamid = 0;
    if (password_required) *password_required = false;
    if (s.empty() || s.size() > 256 || !connect_string_chars_ok(s)) return false;
    const std::string prefix = "sid=";
    if (s.compare(0, prefix.size(), prefix) != 0) return false;
    const std::size_t amp = s.find('&', prefix.size());
    if (amp == std::string::npos) return false;
    const std::string id = s.substr(prefix.size(), amp - prefix.size());
    if (id.empty() || id.size() > 20) return false;
    for (const char c : id)
        if (c < '0' || c > '9') return false;
    const std::string suffix = s.substr(amp);
    if (suffix.size() != 5 || suffix.compare(0, 4, "&pw=") != 0) return false;
    if (suffix[4] != '0' && suffix[4] != '1') return false;
    const unsigned long long parsed = std::strtoull(id.c_str(), nullptr, 10);
    if (parsed == 0) return false;
    if (host_steamid) *host_steamid = static_cast<std::uint64_t>(parsed);
    if (password_required) *password_required = suffix[4] == '1';
    return true;
}

void SteamPartyPresence::set_host_connect_string(std::uint64_t host_steamid,
                                                 bool password_required) {
    const std::string text = encode_connect_string(host_steamid, password_required);
    if (!text.empty()) steam::SteamNet::instance().set_connect_string(text);
}

void SteamPartyPresence::clear_connect_string() {
    steam::SteamNet::instance().clear_connect_string();
}

void SteamPartyPresence::open_invite_overlay(const std::string& connect_string) {
    steam::SteamNet::instance().activate_invite_overlay(connect_string);
}

}  // namespace bbnet
