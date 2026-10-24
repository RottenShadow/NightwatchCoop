// SPDX-License-Identifier: GPL-3.0-or-later
// Steam guest retry policy. Build and run:
//   ninja -C out/gpu party-steam-retry-test && out/gpu/party-steam-retry-test.exe
#include "party_steam_retry.h"

#include <cstdint>
#include <cstdio>
#include <vector>

using bbnet::steam::ConnectRetry;

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

using Clock = ConnectRetry::Clock;
using Duration = ConnectRetry::Duration;

void test_request_and_failure_backoff() {
    ConnectRetry retry(Duration(2000), Duration(30000));
    const Clock::time_point t0 = Clock::now();

    retry.request(42, t0);
    CHECK(retry.pending(42));
    CHECK(retry.backoff(42) == Duration(2000));
    CHECK(retry.retry_at(42) == t0);
    CHECK(retry.due(t0).size() == 1);

    retry.failed(42, t0);
    CHECK(retry.pending(42));
    CHECK(retry.backoff(42) == Duration(4000));
    CHECK(retry.retry_at(42) == t0 + Duration(2000));
    CHECK(retry.due(t0 + Duration(1999)).empty());
    CHECK(retry.due(t0 + Duration(2000)).size() == 1);

    retry.connecting(42, t0 + Duration(3000));
    CHECK(retry.backoff(42) == Duration(4000));
    CHECK(retry.retry_at(42) == t0 + Duration(7000));

    retry.failed(42, t0 + Duration(3000));
    CHECK(retry.backoff(42) == Duration(8000));
    CHECK(retry.retry_at(42) == t0 + Duration(7000));
}

void test_backoff_cap() {
    ConnectRetry retry(Duration(2000), Duration(30000));
    const Clock::time_point t0 = Clock::now();
    retry.request(7, t0);
    for (int i = 0; i < 10; ++i) retry.failed(7, t0);
    CHECK(retry.pending(7));
    CHECK(retry.backoff(7) == Duration(30000));
    CHECK(retry.retry_at(7) == t0 + Duration(30000));
    CHECK(retry.due(t0 + Duration(29999)).empty());
    CHECK(retry.due(t0 + Duration(30000)).size() == 1);
}

void test_clear_and_fresh_request() {
    ConnectRetry retry(Duration(2000), Duration(30000));
    const Clock::time_point t0 = Clock::now();
    retry.request(9, t0);
    retry.failed(9, t0);
    retry.connected(9);
    CHECK(!retry.pending(9));
    CHECK(retry.due(t0 + Duration(60000)).empty());

    retry.request(9, t0 + Duration(60000));
    CHECK(retry.pending(9));
    CHECK(retry.backoff(9) == Duration(2000));
    CHECK(retry.retry_at(9) == t0 + Duration(60000));

    retry.clear(9);
    CHECK(!retry.pending(9));
}

void test_multiple_peers_and_ignored_ids() {
    ConnectRetry retry(Duration(2000), Duration(30000));
    const Clock::time_point t0 = Clock::now();
    retry.request(1, t0);
    retry.request(2, t0);
    retry.failed(1, t0);

    const std::vector<std::uint64_t> due = retry.due(t0);
    CHECK(due.size() == 1);
    CHECK(due[0] == 2);
    CHECK(retry.pending(1));
    CHECK(retry.pending(2));

    retry.request(0, t0);
    retry.failed(0, t0);
    retry.connecting(0, t0);
    retry.connected(0);
    CHECK(!retry.pending(0));
    CHECK(retry.due(t0 + Duration(60000)).size() == 2);

    retry.clear_all();
    CHECK(retry.due(t0 + Duration(60000)).empty());
}

void test_defer_does_not_move_earlier() {
    ConnectRetry retry(Duration(2000), Duration(30000));
    const Clock::time_point t0 = Clock::now();
    retry.request(3, t0);
    retry.defer_at_least(3, t0 + Duration(5000));
    CHECK(retry.retry_at(3) == t0 + Duration(5000));
    retry.defer_at_least(3, t0 + Duration(1000));
    CHECK(retry.retry_at(3) == t0 + Duration(5000));
    retry.defer(3, t0 + Duration(2000));
    CHECK(retry.retry_at(3) == t0 + Duration(2000));
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("request and failure backoff\n");
    test_request_and_failure_backoff();
    std::printf("backoff cap\n");
    test_backoff_cap();
    std::printf("clear and fresh request\n");
    test_clear_and_fresh_request();
    std::printf("multiple peers and ignored ids\n");
    test_multiple_peers_and_ignored_ids();
    std::printf("defer does not move earlier\n");
    test_defer_does_not_move_earlier();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    std::printf(g_failures ? "FAILED\n" : "OK\n");
    return g_failures ? 1 : 0;
}
