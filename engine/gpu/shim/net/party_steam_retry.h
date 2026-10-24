// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <set>
#include <vector>

namespace bbnet::steam {

// Tracks the Steam peers this process still wants and when each should be dialed
// again. Not thread-safe: the Steam transport owns it under State::mu.
class ConnectRetry {
public:
    using Clock = std::chrono::steady_clock;
    using Duration = std::chrono::milliseconds;

    explicit ConnectRetry(Duration initial = Duration(2000), Duration maximum = Duration(30000))
        : initial_(initial), maximum_(std::max(initial, maximum)) {}

    // A fresh request: dial as soon as the transport is ready.
    void request(std::uint64_t id, Clock::time_point now) {
        if (!id) return;
        pending_.insert(id);
        retry_at_[id] = now;
        backoff_[id] = initial_;
    }

    // Keep the peer pending, but do not dial before `when`.
    void defer(std::uint64_t id, Clock::time_point when) {
        if (!id || pending_.find(id) == pending_.end()) return;
        retry_at_[id] = when;
    }

    // Keep the peer pending, but never move its next attempt earlier.
    void defer_at_least(std::uint64_t id, Clock::time_point when) {
        if (!id || pending_.find(id) == pending_.end()) return;
        auto& at = retry_at_[id];
        if (at < when) at = when;
    }

    // A dial or connection failed: wait the current backoff, then double it.
    void failed(std::uint64_t id, Clock::time_point now) {
        if (!id || pending_.find(id) == pending_.end()) return;
        const Duration wait = backoff(id);
        retry_at_[id] = now + wait;
        backoff_[id] = std::min(wait * 2, maximum_);
    }

    // A transient Connecting callback: retry after the current backoff without growing it.
    void connecting(std::uint64_t id, Clock::time_point now) {
        if (!id || pending_.find(id) == pending_.end()) return;
        defer_at_least(id, now + backoff(id));
    }

    // The peer is connected (or an incoming handle owns it): stop retrying.
    void connected(std::uint64_t id) { clear(id); }

    void clear(std::uint64_t id) {
        pending_.erase(id);
        retry_at_.erase(id);
        backoff_.erase(id);
    }

    void clear_all() {
        pending_.clear();
        retry_at_.clear();
        backoff_.clear();
    }

    bool pending(std::uint64_t id) const { return pending_.find(id) != pending_.end(); }

    Duration backoff(std::uint64_t id) const {
        const auto it = backoff_.find(id);
        return it == backoff_.end() ? initial_ : it->second;
    }

    Clock::time_point retry_at(std::uint64_t id) const {
        const auto it = retry_at_.find(id);
        return it == retry_at_.end() ? Clock::time_point{} : it->second;
    }

    // Pending peers whose next dial is due. A pending peer without a time is due now.
    std::vector<std::uint64_t> due(Clock::time_point now) const {
        std::vector<std::uint64_t> out;
        for (const std::uint64_t id : pending_) {
            const auto it = retry_at_.find(id);
            if (it == retry_at_.end() || it->second <= now) out.push_back(id);
        }
        return out;
    }

private:
    Duration initial_;
    Duration maximum_;
    std::set<std::uint64_t> pending_;
    std::map<std::uint64_t, Clock::time_point> retry_at_;
    std::map<std::uint64_t, Duration> backoff_;
};

}  // namespace bbnet::steam
