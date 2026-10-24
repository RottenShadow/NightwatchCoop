// SPDX-License-Identifier: GPL-3.0-or-later
// Steam P2P transport used by the party UDP path. This header does not include Steamworks:
// the implementation loads steam_api64.dll at runtime, so Direct/VPN mode needs no DLL.
//
// All public methods are safe to call from the game thread. Steam's own callbacks and poll
// group run on one dedicated thread; datagrams and peer changes are queued for the game.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bbnet::steam {

struct Config {
    std::uint32_t appid = 480;
    bool host = true;
    std::uint64_t host_steamid = 0;  // guest: the SteamID of the party host
};

struct Datagram {
    std::uint64_t steamid = 0;
    std::vector<std::uint8_t> bytes;
};

struct PeerEvent {
    std::uint64_t steamid = 0;
    bool connected = false;
};

// A friend chose Join Game in the Steam overlay. The connect string is the host's rich-presence
// token (also accepted as a command-line "+connect" value); steamid is the friend Steam chose.
struct JoinRequest {
    std::uint64_t steamid = 0;
    std::string connect;
};

struct Friend {
    std::uint64_t steamid = 0;
    std::string name;
    bool online = false;
    bool in_game = false;
};

class SteamNet {
public:
    static SteamNet& instance();

    bool start(const Config& config, std::string* error);
    void stop();
    bool running() const;
    std::string status() const;

    bool host() const;
    std::uint32_t appid() const;
    std::uint64_t local_steamid() const;
    std::uint64_t host_steamid() const;

    // Call this when the Steam overlay delivers a Join Game request. A host closes its listen
    // socket and rich presence, then becomes a guest; a guest with no host yet also uses it.
    // The connection is opened on the Steam thread.
    bool join_host(std::uint64_t steamid);
    bool pop_join_request(JoinRequest* out);

    // Party mesh: any member may connect to any other known member on demand.
    bool connect_peer(std::uint64_t steamid);
    bool send(std::uint64_t steamid, const void* data, std::size_t size);
    bool broadcast(const void* data, std::size_t size);
    bool pop_datagram(Datagram* out);
    bool pop_event(PeerEvent* out);
    bool is_connected(std::uint64_t steamid) const;
    // Drop one peer (a member that left) without touching the other sessions.
    bool disconnect_peer(std::uint64_t steamid);
    // Stop retrying a peer but keep its Steam handle (PartyLink rejected the password and may
    // rejoin through the same connection).
    bool clear_retry(std::uint64_t steamid);

    // Set only after PartyLink has authenticated this SteamID. Until then game traffic from it
    // is dropped (net_socket's Steam hook enforces this).
    void trust_peer(std::uint64_t steamid);
    // Replace the trusted set (the host's authenticated roster); a peer that left loses access.
    void set_trusted_peers(const std::vector<std::uint64_t>& steamids);
    bool is_trusted(std::uint64_t steamid) const;

    bool is_friend(std::uint64_t steamid) const;
    std::vector<Friend> friends() const;
    std::string peer_name(std::uint64_t steamid) const;

    // Virtual 198.18/15 address book for the peers of this party.
    bool learn_peer(std::uint64_t steamid);
    bool virtual_for_steamid(std::uint64_t steamid, std::uint32_t* nbo) const;
    bool steamid_for_virtual(std::uint32_t nbo, std::uint64_t* steamid) const;

    // No-op in the threaded implementation; present so the party loop can call the same
    // transport contract as the stub. Steam callbacks are pumped by the transport thread.
    void tick();

    // Rich presence and overlay helpers (ISteamFriends). Safe no-ops when Steam is off/stubbed.
    bool set_connect_string(const std::string& connect);
    void clear_connect_string();
    bool activate_invite_overlay(const std::string& connect);

    SteamNet(const SteamNet&) = delete;
    SteamNet& operator=(const SteamNet&) = delete;

private:
    SteamNet();
    ~SteamNet();
};

}  // namespace bbnet::steam

namespace bbnet {

namespace party {
class PartyLink;
}

// Public transport facade used by the party runtime and the tests. The implementation lives in
// party_steam.cpp; Steamworks types stay out of this header so Direct mode never needs the SDK.
class SteamPartyLink {
public:
    static std::unique_ptr<SteamPartyLink> create(std::string* error = nullptr);

    // Call before PartyLink::start_host / start_guest. The socket transport already carries
    // synthetic Steam addresses through net_socket; wire() keeps the link association.
    void wire(party::PartyLink& link);
    void tick();
    void shutdown();
    ~SteamPartyLink();

private:
    SteamPartyLink();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class SteamPartyPresence {
public:
    static std::string encode_connect_string(std::uint64_t host_steamid, bool password_required);
    static bool decode_connect_string(const std::string& s, std::uint64_t* host_steamid,
                                      bool* password_required);

    void set_host_connect_string(std::uint64_t host_steamid, bool password_required);
    void clear_connect_string();
    void open_invite_overlay(const std::string& connect_string);
};

}  // namespace bbnet
