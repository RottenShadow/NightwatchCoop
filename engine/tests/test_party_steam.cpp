// SPDX-License-Identifier: GPL-3.0-or-later
// Steam mode without the Steamworks SDK: the BB_STEAM_STUB transport, the connect string and the
// virtual address/secret helpers. Build and run:
//   ninja -C out/gpu party-steam-identity-test && out/gpu/party-steam-identity-test.exe
#include "party_steam.h"
#include "steam_identity.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using namespace bbnet;
using namespace bbnet::steam;

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

namespace {

std::uint64_t random_steamid(std::mt19937_64& rng) {
    std::uint64_t id = rng();
    if (!id) id = 76561198000000001ull;
    return id;
}

// Feature: steam-connection-layer, Property 1: loopback datagram identity
void test_loopback_identity() {
    SteamNet& net = SteamNet::instance();
    Config cfg;
    cfg.appid = 480;
    cfg.host = true;
    std::string error;
    CHECK(net.start(cfg, &error));
    if (!net.running()) return;

    const std::uint64_t peer = 76561198000000042ull;
    CHECK(net.connect_peer(peer));

    std::mt19937_64 rng(0x5eed1234ull);
    std::uniform_int_distribution<std::size_t> size_dist(1, 512u * 1024u);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    for (int i = 0; i < 200; ++i) {
        std::vector<std::uint8_t> payload(size_dist(rng));
        for (std::uint8_t& b : payload) b = static_cast<std::uint8_t>(byte_dist(rng));
        CHECK(net.send(peer, payload.data(), payload.size()));
        Datagram out;
        CHECK(net.pop_datagram(&out));
        CHECK(out.steamid == peer);
        CHECK(out.bytes == payload);
    }
    net.stop();
}

// Feature: steam-connection-layer, Property 2: connect-string round-trip
void test_connect_string_round_trip() {
    std::mt19937_64 rng(0xc0ffeeull);
    for (int i = 0; i < 200; ++i) {
        const std::uint64_t id = random_steamid(rng);
        const bool pw = (rng() & 1) != 0;
        const std::string text = SteamPartyPresence::encode_connect_string(id, pw);
        CHECK(!text.empty());
        std::uint64_t decoded = 0;
        bool decoded_pw = !pw;
        CHECK(SteamPartyPresence::decode_connect_string(text, &decoded, &decoded_pw));
        CHECK(decoded == id);
        CHECK(decoded_pw == pw);
    }
}

// Feature: steam-connection-layer, Property 3: connect-string invariants
void test_connect_string_invariants() {
    std::mt19937_64 rng(0xbadc0deull);
    const std::string allowed = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.-~=&";
    for (int i = 0; i < 200; ++i) {
        const std::uint64_t id = random_steamid(rng);
        const bool pw = (rng() & 1) != 0;
        const std::string text = SteamPartyPresence::encode_connect_string(id, pw);
        CHECK(text.size() <= 256);
        for (const char c : text) CHECK(allowed.find(c) != std::string::npos);
    }
}

// Feature: steam-connection-layer, Property 4: malformed string rejection
void test_malformed_connect_strings() {
    const std::string valid = "sid=76561198000000001&pw=0";
    const std::vector<std::string> malformed = {
        "",
        "sid=",
        "sid=123",
        "sid=abc&pw=0",
        "sid=123&pw=2",
        "sid=123&pw=00",
        "sid=123&pw=0x",
        "sid= 123&pw=0",
        "sid=+123&pw=0",
        "sid=123&pw=",
        "sid=123&pw=1\n",
        "sid=123&pw=1 ",
        "sid=123&pw=1;rm",
        std::string(257, 'a'),
        std::string("sid=1&pw=0\0junk", 16),
        std::string("sid=1\r&pw=0"),
        std::string("sid=1\t&pw=0"),
    };
    for (const std::string& s : malformed) {
        std::uint64_t id = 0xdeadbeefull;
        bool pw = true;
        CHECK(!SteamPartyPresence::decode_connect_string(s, &id, &pw));
        CHECK(id == 0);
        CHECK(!pw);
    }
    for (std::size_t n = 0; n < valid.size(); ++n) {
        const std::string cut = valid.substr(0, n);
        std::uint64_t id = 0xdeadbeefull;
        bool pw = true;
        CHECK(!SteamPartyPresence::decode_connect_string(cut, &id, &pw));
        CHECK(id == 0);
        CHECK(!pw);
    }
    std::mt19937_64 rng(0xfeedfaceull);
    const std::string bad_chars = "\x01\x02\x7f <>/\\'\"?#@$%^*()[]{},;:!|`";
    for (int i = 0; i < 200; ++i) {
        std::string s;
        const std::size_t n = static_cast<std::size_t>(rng() % 64);
        for (std::size_t j = 0; j < n; ++j) s.push_back(bad_chars[rng() % bad_chars.size()]);
        std::uint64_t id = 0xdeadbeefull;
        bool pw = true;
        CHECK(!SteamPartyPresence::decode_connect_string(s, &id, &pw));
        CHECK(id == 0);
        CHECK(!pw);
    }
}

// Feature: steam-connection-layer, Property 5: per-peer session isolation
void test_peer_isolation() {
    SteamNet& net = SteamNet::instance();
    std::mt19937_64 rng(0x15150ull);
    for (int peers = 2; peers <= 4; ++peers) {
        Config cfg;
        cfg.appid = 480;
        cfg.host = true;
        std::string error;
        CHECK(net.start(cfg, &error));
        if (!net.running()) return;
        std::vector<std::uint64_t> ids;
        for (int i = 0; i < peers; ++i) {
            const std::uint64_t id = random_steamid(rng);
            ids.push_back(id);
            CHECK(net.connect_peer(id));
        }
        std::vector<std::vector<std::uint8_t>> payload(static_cast<std::size_t>(peers));
        for (int i = 0; i < peers; ++i) {
            payload[static_cast<std::size_t>(i)].resize(32 + static_cast<std::size_t>(i));
            for (std::uint8_t& b : payload[static_cast<std::size_t>(i)]) b = static_cast<std::uint8_t>(rng());
            CHECK(net.send(ids[static_cast<std::size_t>(i)], payload[static_cast<std::size_t>(i)].data(),
                           payload[static_cast<std::size_t>(i)].size()));
        }
        for (int i = 0; i < peers; ++i) {
            Datagram out;
            CHECK(net.pop_datagram(&out));
            CHECK(out.bytes == payload[static_cast<std::size_t>(i)]);
        }
        const int closed = peers / 2;
        CHECK(net.disconnect_peer(ids[static_cast<std::size_t>(closed)]));
        CHECK(!net.is_connected(ids[static_cast<std::size_t>(closed)]));
        for (int i = 0; i < peers; ++i) {
            if (i == closed) continue;
            CHECK(net.is_connected(ids[static_cast<std::size_t>(i)]));
            CHECK(net.send(ids[static_cast<std::size_t>(i)], payload[static_cast<std::size_t>(i)].data(),
                           payload[static_cast<std::size_t>(i)].size()));
            Datagram out;
            CHECK(net.pop_datagram(&out));
            CHECK(out.bytes == payload[static_cast<std::size_t>(i)]);
        }
        CHECK(!net.send(ids[static_cast<std::size_t>(closed)], payload[0].data(), payload[0].size()));
        net.stop();
    }
}

void test_identity_and_secret() {
    const std::uint64_t a = 76561198000000001ull;
    const std::uint64_t b = 76561198000000002ull;
    CHECK(is_synthetic_nbo(synthetic_nbo(a)));
    CHECK(is_synthetic_nbo(synthetic_nbo(b)));
    CHECK(synthetic_nbo(a) != synthetic_nbo(b));
    CHECK(!is_synthetic_nbo(0x0100007fu));
    const auto bytes = synthetic_bytes(a);
    CHECK(bytes[0] == kSyntheticFirst);
    CHECK(bytes[1] == kSyntheticSecond || bytes[1] == kSyntheticSecond + 1);

    const auto s1 = party_secret("correct horse battery staple", a, 480);
    const auto s2 = party_secret("correct horse battery staple", a, 480);
    const auto s3 = party_secret("another password", a, 480);
    const auto s4 = party_secret("correct horse battery staple", b, 480);
    const auto s5 = party_secret("correct horse battery staple", a, 481);
    CHECK(s1 == s2);
    CHECK(s1 != s3);
    CHECK(s1 != s4);
    CHECK(s1 != s5);

    AddressBook book;
    std::uint32_t nbo = 0;
    CHECK(book.add(a, &nbo));
    CHECK(nbo == synthetic_nbo(a));
    std::uint64_t back = 0;
    CHECK(book.steamid_of(nbo, &back));
    CHECK(back == a);
    std::uint32_t addr = 0;
    CHECK(book.address_of(a, &addr));
    CHECK(addr == nbo);
    CHECK(book.add(a, &addr));
    CHECK(book.size() == 1);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("steam identity and secret\n");
    test_identity_and_secret();
    std::printf("loopback datagram identity (property 1)\n");
    test_loopback_identity();
    std::printf("connect-string round-trip (property 2)\n");
    test_connect_string_round_trip();
    std::printf("connect-string invariants (property 3)\n");
    test_connect_string_invariants();
    std::printf("malformed connect strings (property 4)\n");
    test_malformed_connect_strings();
    std::printf("per-peer session isolation (property 5)\n");
    test_peer_isolation();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    std::printf(g_failures ? "FAILED\n" : "OK\n");
    return g_failures ? 1 : 0;
}
