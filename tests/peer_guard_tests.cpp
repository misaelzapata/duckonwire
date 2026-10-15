#include "duckonwire/json.hpp"
#include "duckonwire/peer_guard.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
using duckonwire::PeerGuard;
using duckonwire::PeerGuardOptions;

void Require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
struct ManualClock {
    std::atomic<std::int64_t> milliseconds {0};
    PeerGuard::Clock Source() {
        return [this] { return std::chrono::steady_clock::time_point {} +
                              std::chrono::milliseconds(milliseconds.load()); };
    }
    void Advance(std::int64_t amount) { milliseconds.fetch_add(amount); }
};
PeerGuardOptions SmallLimits() {
    PeerGuardOptions options;
    options.auth_failures = 3;
    options.connections_per_minute = 4;
    options.requests_per_minute = 6;
    return options;
}
template<class Function>
void Concurrent(unsigned threads, const Function &body) {
    std::atomic<unsigned> ready {0};
    std::atomic<bool> start {false};
    std::vector<std::thread> workers;
    for (unsigned index = 0; index < threads; ++index) workers.emplace_back([&] {
        ready.fetch_add(1);
        while (!start.load()) std::this_thread::yield();
        body();
    });
    while (ready.load() != threads) std::this_thread::yield();
    start.store(true);
    for (auto &worker : workers) worker.join();
}
} // namespace

int main() {
    std::vector<std::pair<std::string, std::string>> checks;
    const auto test = [&](const std::string &name, const std::function<void()> &body) {
        try { body(); checks.emplace_back(name, ""); }
        catch (const std::exception &error) { checks.emplace_back(name, error.what()); }
    };
    test("default_authentication_threshold_blocks_both_protocol_admissions", [] {
        ManualClock clock; PeerGuard guard({}, clock.Source());
        for (unsigned attempt = 0; attempt < 4; ++attempt) {
            guard.AuthenticationFailed("192.0.2.1");
            Require(!guard.IsBanned("192.0.2.1"), "Authentication banned before configured threshold");
        }
        guard.AuthenticationFailed("192.0.2.1");
        Require(guard.IsBanned("192.0.2.1"), "Authentication threshold did not ban");
        Require(!guard.AdmitConnection("192.0.2.1") && !guard.AdmitRequest("192.0.2.1"), "Ban did not cover both admissions");
    });
    test("ban_expiry_is_exact_and_blocked_attempts_do_not_extend_it", [] {
        ManualClock clock; auto options = SmallLimits(); options.auth_failures = 1;
        PeerGuard guard(options, clock.Source()); guard.AuthenticationFailed("192.0.2.1");
        clock.Advance(299999);
        Require(guard.IsBanned("192.0.2.1") && !guard.AdmitConnection("192.0.2.1"), "Ban ended early");
        guard.AuthenticationFailed("192.0.2.1");
        clock.Advance(1);
        Require(!guard.IsBanned("192.0.2.1") && guard.AdmitConnection("192.0.2.1"), "Blocked activity extended ban");
    });
    test("authentication_window_discards_expired_failures", [] {
        ManualClock clock; auto options = SmallLimits(); options.auth_failures = 2;
        PeerGuard guard(options, clock.Source()); guard.AuthenticationFailed("192.0.2.1");
        clock.Advance(60000); guard.AuthenticationFailed("192.0.2.1");
        Require(!guard.IsBanned("192.0.2.1"), "Expired authentication failure counted");
        guard.AuthenticationFailed("192.0.2.1");
        Require(guard.IsBanned("192.0.2.1"), "New authentication window did not enforce threshold");
    });
    test("short_ban_expiry_resets_all_counters", [] {
        ManualClock clock; auto options = SmallLimits(); options.auth_failures = 2; options.ban_seconds = 1;
        options.connections_per_minute = options.requests_per_minute = 1;
        PeerGuard guard(options, clock.Source());
        Require(guard.AdmitConnection("192.0.2.1") && guard.AdmitRequest("192.0.2.1"), "Initial budget unavailable");
        guard.AuthenticationFailed("192.0.2.1"); guard.AuthenticationFailed("192.0.2.1");
        clock.Advance(1000);
        Require(guard.AdmitConnection("192.0.2.1") && guard.AdmitRequest("192.0.2.1"), "Rate budget survived ban expiry");
        guard.AuthenticationFailed("192.0.2.1");
        Require(!guard.IsBanned("192.0.2.1"), "Failure budget survived ban expiry");
        guard.AuthenticationFailed("192.0.2.1");
        Require(guard.IsBanned("192.0.2.1"), "Fresh failure budget was not enforced");
    });
    test("connection_rate_overuse_bans_at_first_excess_connection", [] {
        ManualClock clock; auto options = SmallLimits(); PeerGuard guard(options, clock.Source());
        for (unsigned attempt = 0; attempt < options.connections_per_minute; ++attempt)
            Require(guard.AdmitConnection("192.0.2.1"), "Connection denied within budget");
        Require(!guard.AdmitConnection("192.0.2.1") && guard.IsBanned("192.0.2.1"), "Connection overuse did not ban");
        Require(!guard.AdmitRequest("192.0.2.1"), "Existing session request escaped connection ban");
    });
    test("request_rate_overuse_bans_existing_peer_sessions", [] {
        ManualClock clock; auto options = SmallLimits(); PeerGuard guard(options, clock.Source());
        for (unsigned attempt = 0; attempt < options.requests_per_minute; ++attempt)
            Require(guard.AdmitRequest("192.0.2.1"), "Request denied within budget");
        Require(!guard.AdmitRequest("192.0.2.1") && guard.IsBanned("192.0.2.1"), "Request overuse did not ban");
        Require(!guard.AdmitConnection("192.0.2.1"), "New connection escaped request ban");
    });
    test("connection_and_request_windows_renew_after_one_minute", [] {
        ManualClock clock; auto options = SmallLimits(); PeerGuard guard(options, clock.Source());
        for (unsigned attempt = 0; attempt < options.connections_per_minute; ++attempt)
            Require(guard.AdmitConnection("192.0.2.1"), "Initial connection admission failed");
        for (unsigned attempt = 0; attempt < options.requests_per_minute; ++attempt)
            Require(guard.AdmitRequest("192.0.2.1"), "Initial request admission failed");
        clock.Advance(60000);
        Require(guard.AdmitConnection("192.0.2.1") && guard.AdmitRequest("192.0.2.1"), "Expired rates were not reset");
    });
    test("distinct_peers_have_independent_failures_rates_and_bans", [] {
        ManualClock clock; auto options = SmallLimits(); options.auth_failures = 1;
        PeerGuard guard(options, clock.Source()); guard.AuthenticationFailed("192.0.2.1");
        Require(!guard.IsBanned("192.0.2.2"), "Authentication ban spread to another peer");
        Require(guard.AdmitConnection("192.0.2.2") && guard.AdmitRequest("192.0.2.2"), "Ban exhausted another peer's budget");
        for (unsigned attempt = 1; attempt <= options.requests_per_minute; ++attempt) guard.AdmitRequest("192.0.2.2");
        Require(guard.IsBanned("192.0.2.2") && guard.AdmitRequest("192.0.2.3"), "Request ban spread to another peer");
    });
    test("concurrent_authentication_failures_are_counted_once_without_loss", [] {
        ManualClock clock; auto options = SmallLimits(); options.auth_failures = 1000;
        PeerGuard guard(options, clock.Source());
        Concurrent(9, [&] { for (unsigned attempt = 0; attempt < 111; ++attempt) guard.AuthenticationFailed("192.0.2.1"); });
        Require(!guard.IsBanned("192.0.2.1"), "Concurrent failures banned early");
        guard.AuthenticationFailed("192.0.2.1");
        Require(guard.IsBanned("192.0.2.1"), "Concurrent failure increments were lost");
    });
    test("concurrent_connection_admissions_never_exceed_shared_limit", [] {
        ManualClock clock; auto options = SmallLimits(); options.connections_per_minute = 256;
        PeerGuard guard(options, clock.Source()); std::atomic<unsigned> admitted {0};
        Concurrent(16, [&] { for (unsigned attempt = 0; attempt < 32; ++attempt)
            if (guard.AdmitConnection("192.0.2.1")) admitted.fetch_add(1); });
        Require(admitted.load() == 256 && guard.IsBanned("192.0.2.1"), "Concurrent connections bypassed shared accounting");
    });
    test("concurrent_requests_never_exceed_shared_limit", [] {
        ManualClock clock; auto options = SmallLimits(); options.requests_per_minute = 256;
        PeerGuard guard(options, clock.Source()); std::atomic<unsigned> admitted {0};
        Concurrent(16, [&] { for (unsigned attempt = 0; attempt < 32; ++attempt)
            if (guard.AdmitRequest("192.0.2.1")) admitted.fetch_add(1); });
        Require(admitted.load() == 256 && guard.IsBanned("192.0.2.1"), "Concurrent requests bypassed shared accounting");
    });
    test("tracking_capacity_fails_closed_and_churn_cannot_evict_active_bans", [] {
        ManualClock clock; auto options = SmallLimits(); options.auth_failures = 1; options.max_tracked_peers = 2;
        PeerGuard guard(options, clock.Source()); guard.AuthenticationFailed("192.0.2.1");
        Require(guard.AdmitConnection("192.0.2.2"), "Free tracking slot unavailable");
        for (unsigned peer = 3; peer < 250; ++peer) {
            const auto address = "192.0.2." + std::to_string(peer);
            Require(!guard.AdmitConnection(address) && !guard.AdmitRequest(address), "Capacity allowed untracked peer");
            guard.AuthenticationFailed(address);
        }
        Require(guard.IsBanned("192.0.2.1") && !guard.AdmitConnection("192.0.2.1"), "Peer churn evicted active ban");
        Require(guard.AdmitRequest("192.0.2.2"), "Full tracking table harmed known peer");
    });
    test("inactive_entries_expire_without_releasing_active_bans", [] {
        ManualClock clock; auto options = SmallLimits(); options.auth_failures = 1; options.max_tracked_peers = 2;
        PeerGuard guard(options, clock.Source()); guard.AuthenticationFailed("192.0.2.1");
        Require(guard.AdmitConnection("192.0.2.2"), "Initial tracking failed");
        clock.Advance(60000);
        Require(guard.AdmitConnection("192.0.2.3"), "Inactive peer was not reclaimed");
        Require(guard.IsBanned("192.0.2.1") && !guard.AdmitConnection("192.0.2.4"), "Active ban slot was reclaimed");
        clock.Advance(240000);
        Require(guard.AdmitConnection("192.0.2.4") && !guard.IsBanned("192.0.2.1"), "Expired ban did not release tracking capacity");
        options.max_tracked_peers = 1; options.ban_seconds = 1;
        PeerGuard observed(options, clock.Source()); observed.AuthenticationFailed("192.0.2.1");
        clock.Advance(1000);
        Require(!observed.IsBanned("192.0.2.1") && observed.AdmitConnection("192.0.2.2"),
            "Observing ban expiry pinned an empty tracking record");
    });
    test("authentication_history_retention_covers_configured_window", [] {
        ManualClock clock; auto options = SmallLimits(); options.auth_failures = 2;
        options.auth_window_seconds = 120; options.max_tracked_peers = 1;
        PeerGuard guard(options, clock.Source()); guard.AuthenticationFailed("192.0.2.1");
        clock.Advance(60000);
        Require(!guard.AdmitConnection("192.0.2.2"), "Auth history was reclaimed before configured window");
        guard.AuthenticationFailed("192.0.2.1");
        Require(guard.IsBanned("192.0.2.1"), "Tracked authentication history was lost");
    });
    test("peer_keys_and_configuration_have_explicit_bounds", [] {
        ManualClock clock; PeerGuard guard({}, clock.Source());
        for (const auto &key : {std::string(), std::string(65, '1'), std::string("192.0.2.1\0extra", 15),
                std::string("unknown"), std::string("localhost"), std::string("192.0.2.999"),
                std::string("192.0.2.1:9496"), std::string("[2001:db8::1]"), std::string("fe80::1%lo0")}) {
            Require(!guard.AdmitConnection(key) && !guard.AdmitRequest(key) && guard.IsBanned(key), "Unbounded peer key accepted");
            guard.AuthenticationFailed(key);
        }
        Require(guard.AdmitConnection("192.0.2.1"), "Invalid keys consumed tracking capacity");
        auto aliases = SmallLimits(); aliases.auth_failures = 2; aliases.max_tracked_peers = 1;
        PeerGuard shared(aliases, clock.Source());
        Require(shared.AdmitConnection("192.0.2.1") && shared.AdmitRequest("::ffff:c000:201"),
            "IPv4 mapped form created a second tracking identity");
        shared.AuthenticationFailed("::FFFF:192.0.2.1"); shared.AuthenticationFailed("192.0.2.1");
        Require(shared.IsBanned("192.0.2.1") && shared.IsBanned("::ffff:c000:201") &&
            !shared.AdmitConnection("::ffff:192.0.2.1"), "IPv4 mapped form escaped shared ban");
        PeerGuard ipv6(aliases, clock.Source());
        Require(ipv6.AdmitConnection("2001:db8::1") && ipv6.AdmitRequest("2001:0DB8:0:0:0:0:0:1"),
            "Equivalent IPv6 forms created separate tracking identities");
        ipv6.AuthenticationFailed("2001:db8::1"); ipv6.AuthenticationFailed("2001:0DB8:0:0:0:0:0:1");
        Require(ipv6.IsBanned("2001:DB8::1"), "Equivalent IPv6 form escaped shared ban");
        std::vector<PeerGuardOptions> invalid(6);
        invalid[0].auth_failures = 0; invalid[1].auth_window_seconds = 0; invalid[2].ban_seconds = 0;
        invalid[3].connections_per_minute = 0; invalid[4].requests_per_minute = 0; invalid[5].max_tracked_peers = 0;
        for (const auto &options : invalid) {
            bool rejected = false;
            try { PeerGuard unsafe(options, clock.Source()); } catch (const std::invalid_argument &) { rejected = true; }
            Require(rejected, "Zero limit silently disabled protection");
        }
    });
    bool passed = !checks.empty(); std::cout << "{\"checks\":[";
    for (std::size_t index = 0; index < checks.size(); ++index) {
        if (index) std::cout << ',';
        const auto &check = checks[index]; passed = passed && check.second.empty();
        std::cout << "{\"name\":" << duckonwire::JsonString(check.first)
                  << ",\"passed\":" << (check.second.empty() ? "true" : "false");
        if (!check.second.empty()) std::cout << ",\"error\":" << duckonwire::JsonString(check.second);
        std::cout << '}';
    }
    std::cout << "],\"passed\":" << (passed ? "true" : "false") << "}\n";
    return passed ? 0 : 1;
}
