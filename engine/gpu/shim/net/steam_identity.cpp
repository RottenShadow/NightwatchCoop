// SPDX-License-Identifier: GPL-3.0-or-later
#include "steam_identity.h"

#include "../party/party_crypto.h"

#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

namespace bbnet::steam {
namespace {

std::uint64_t fnv1a(std::uint64_t v) {
    std::uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < 8; ++i) {
        h ^= (v >> (8 * i)) & 0xffu;
        h *= 1099511628211ull;
    }
    return h;
}

std::uint32_t nbo_of(const std::array<std::uint8_t, 4>& b) {
    std::uint32_t nbo = 0;
    std::memcpy(&nbo, b.data(), 4);
    return nbo;
}

}  // namespace

std::array<std::uint8_t, 4> synthetic_bytes(std::uint64_t steamid) {
    const std::uint32_t v = static_cast<std::uint32_t>(fnv1a(steamid) & (kSyntheticSpan - 1));
    return {kSyntheticFirst, static_cast<std::uint8_t>(kSyntheticSecond + (v >> 16)),
            static_cast<std::uint8_t>((v >> 8) & 0xff), static_cast<std::uint8_t>(v & 0xff)};
}

std::uint32_t synthetic_nbo(std::uint64_t steamid) { return nbo_of(synthetic_bytes(steamid)); }

bool is_synthetic_nbo(std::uint32_t nbo) {
    const auto* b = reinterpret_cast<const std::uint8_t*>(&nbo);
    return b[0] == kSyntheticFirst && (b[1] & 0xfeu) == kSyntheticSecond;
}

std::string synthetic_text(std::uint64_t steamid) {
    const auto b = synthetic_bytes(steamid);
    char text[24];
    std::snprintf(text, sizeof text, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return text;
}

std::string nbo_text(std::uint32_t nbo) {
    const auto* b = reinterpret_cast<const std::uint8_t*>(&nbo);
    char text[24];
    std::snprintf(text, sizeof text, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return text;
}

std::array<std::uint8_t, 8> party_secret(std::string_view password, std::uint64_t host_steamid,
                                         std::uint32_t appid) {
    std::vector<std::uint8_t> material;
    const char tag[] = "nightwatch-steam-party-v1";
    material.insert(material.end(), tag, tag + sizeof tag - 1);
    for (int i = 0; i < 4; ++i) material.push_back(static_cast<std::uint8_t>(appid >> (8 * i)));
    for (int i = 0; i < 8; ++i) material.push_back(static_cast<std::uint8_t>(host_steamid >> (8 * i)));
    material.insert(material.end(), password.begin(), password.end());
    const party::crypto::Key h = party::crypto::blake2b256(material.data(), material.size());
    std::array<std::uint8_t, 8> out{};
    std::memcpy(out.data(), h.data(), out.size());
    return out;
}

bool AddressBook::add(std::uint64_t steamid, std::uint32_t* nbo, std::string* error) {
    const std::uint32_t addr = synthetic_nbo(steamid);
    const auto same = by_steamid_.find(steamid);
    if (same != by_steamid_.end()) {
        if (nbo) *nbo = same->second;
        return true;
    }
    const auto clash = by_address_.find(addr);
    if (clash != by_address_.end() && clash->second != steamid) {
        if (error)
            *error = "SteamID " + std::to_string(steamid) + " and " + std::to_string(clash->second) +
                     " map to the same virtual address " + nbo_text(addr);
        return false;
    }
    by_steamid_.emplace(steamid, addr);
    by_address_.emplace(addr, steamid);
    if (nbo) *nbo = addr;
    return true;
}

bool AddressBook::address_of(std::uint64_t steamid, std::uint32_t* nbo) const {
    const auto it = by_steamid_.find(steamid);
    if (it == by_steamid_.end()) return false;
    if (nbo) *nbo = it->second;
    return true;
}

bool AddressBook::steamid_of(std::uint32_t nbo, std::uint64_t* steamid) const {
    const auto it = by_address_.find(nbo);
    if (it == by_address_.end()) return false;
    if (steamid) *steamid = it->second;
    return true;
}

void AddressBook::clear() {
    by_steamid_.clear();
    by_address_.clear();
}

}  // namespace bbnet::steam
