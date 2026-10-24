// SPDX-License-Identifier: GPL-3.0-or-later
// Steam mode's identity: a SteamID64 maps to a virtual IPv4 address in 198.18.0.0/15, and the
// shared party secret comes from the password, the host's SteamID and the AppID. No Steam API is
// used here so the mapping and the key derivation can be tested without the SDK.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace bbnet::steam {

constexpr std::uint16_t kVirtualPort = 9307;

// 198.18.0.0/15: the benchmarking range, not routed on the internet and not one of the ranges
// Router treats as a synthetic IPv6 key (240/5). Bytes are a.b.c.d.
constexpr std::uint8_t kSyntheticFirst = 198;
constexpr std::uint8_t kSyntheticSecond = 18;
constexpr std::uint32_t kSyntheticSpan = 1u << 17;

std::array<std::uint8_t, 4> synthetic_bytes(std::uint64_t steamid);
std::uint32_t synthetic_nbo(std::uint64_t steamid);
bool is_synthetic_nbo(std::uint32_t nbo);
std::string synthetic_text(std::uint64_t steamid);
std::string nbo_text(std::uint32_t nbo);

// The party secret PartyLink uses in Steam mode: BLAKE2b-256 over a domain tag, the AppID, the
// host's SteamID and the password, first 8 bytes. Host and guest derive the same value.
std::array<std::uint8_t, 8> party_secret(std::string_view password, std::uint64_t host_steamid,
                                         std::uint32_t appid);

// The peers of one Steam party: SteamID64 -> virtual address, and back. A hash collision is
// reported instead of silently aliasing two players (at most 5 peers, so it is vanishingly rare).
class AddressBook {
public:
    bool add(std::uint64_t steamid, std::uint32_t* nbo, std::string* error = nullptr);
    bool address_of(std::uint64_t steamid, std::uint32_t* nbo) const;
    bool steamid_of(std::uint32_t nbo, std::uint64_t* steamid) const;
    void clear();
    std::size_t size() const { return by_steamid_.size(); }

private:
    std::map<std::uint64_t, std::uint32_t> by_steamid_;
    std::map<std::uint32_t, std::uint64_t> by_address_;
};

}  // namespace bbnet::steam
