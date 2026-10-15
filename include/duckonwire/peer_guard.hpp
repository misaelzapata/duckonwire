#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace duckonwire {

struct PeerGuardOptions {
    std::uint32_t auth_failures = 5;
    std::uint32_t auth_window_seconds = 60;
    std::uint32_t ban_seconds = 300;
    std::uint32_t connections_per_minute = 120;
    std::uint32_t requests_per_minute = 6000;
    std::size_t max_tracked_peers = 4096;
};

// Share one guard across all public protocols. Keys must be the numeric address
// from the accepted socket, never an address supplied by a client or HTTP header.
// Each peer has constant-size accounting. Full tracking capacity rejects new
// peers; active bans and recent accounting are never evicted to admit them.
// Counters use fixed windows starting at the first event. The configured rate
// is a burst allowance within that window; ban expiry resets every counter.
// Numeric IPv4/IPv6 keys are canonicalized; IPv4-mapped IPv6 shares IPv4 state.
class PeerGuard {
public:
    using Clock = std::function<std::chrono::steady_clock::time_point()>;
    explicit PeerGuard(PeerGuardOptions options = {});
    // An injectable monotonic clock allows deterministic expiry checks. Clock
    // callbacks execute under the guard's mutex and must not call this guard.
    PeerGuard(PeerGuardOptions options, Clock clock);
    ~PeerGuard();
    PeerGuard(const PeerGuard &) = delete;
    PeerGuard &operator=(const PeerGuard &) = delete;

    bool AdmitConnection(const std::string &peer);
    bool AdmitRequest(const std::string &peer);
    void AuthenticationFailed(const std::string &peer);
    bool IsBanned(const std::string &peer);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace duckonwire
