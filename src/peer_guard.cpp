#include "duckonwire/peer_guard.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace duckonwire {

struct PeerGuard::Impl {
    using Time = std::chrono::steady_clock::time_point;
    struct Window {
        Time start {};
        std::uint32_t count = 0;
        void Expire(Time now, std::chrono::seconds duration) {
            if (count && now - start >= duration) count = 0;
        }
        void Add(Time now) {
            if (!count) start = now;
            ++count;
        }
    };
    struct Peer {
        Time last_seen {}, ban_until {};
        bool banned = false;
        Window authentication, connections, requests;
    };
    PeerGuardOptions options;
    Clock clock;
    std::mutex mutex;
    std::unordered_map<std::string, Peer> peers;
    const std::chrono::seconds auth_window, idle_expiry;
    Time next_expiry = Time::max();

    Impl(PeerGuardOptions config, Clock source)
        : options(config), clock(std::move(source)), auth_window(config.auth_window_seconds),
          idle_expiry(std::max<std::uint32_t>(60, config.auth_window_seconds)) {
        if (!clock || !options.auth_failures || !options.auth_window_seconds || !options.ban_seconds ||
            !options.connections_per_minute || !options.requests_per_minute || !options.max_tracked_peers)
            throw std::invalid_argument("Peer protection requires positive limits and a monotonic clock");
    }

    static std::string CanonicalKey(const std::string &peer) {
        // Parse numeric addresses locally; never resolve a hostname or accept
        // a sentinel such as "unknown" as a usable peer identity.
        if (peer.empty() || peer.size() > 64 || peer.find('\0') != std::string::npos ||
            peer.find('%') != std::string::npos) return {};
        in_addr ipv4 {};
        in6_addr ipv6 {};
        char canonical[INET6_ADDRSTRLEN] {};
        if (inet_pton(AF_INET, peer.c_str(), &ipv4) == 1)
            return inet_ntop(AF_INET, &ipv4, canonical, sizeof(canonical)) ? canonical : std::string();
        if (inet_pton(AF_INET6, peer.c_str(), &ipv6) != 1) return {};
        // Dual-stack sockets can report an IPv4 source as ::ffff:a.b.c.d. Its
        // accounting must match the same source on an IPv4-only listener.
        if (IN6_IS_ADDR_V4MAPPED(&ipv6)) {
            std::memcpy(&ipv4, ipv6.s6_addr + 12, sizeof(ipv4));
            return inet_ntop(AF_INET, &ipv4, canonical, sizeof(canonical)) ? canonical : std::string();
        }
        return inet_ntop(AF_INET6, &ipv6, canonical, sizeof(canonical)) ? canonical : std::string();
    }
    void Expire(Peer &peer, Time now) {
        if (peer.banned && now >= peer.ban_until) {
            peer.banned = false;
            peer.authentication.count = peer.connections.count = peer.requests.count = 0;
            // A read-only ban check must not pin an expired, empty record. A
            // subsequent admitted event will give it a fresh activity timestamp.
            peer.last_seen = now - idle_expiry;
        }
        peer.authentication.Expire(now, auth_window);
        peer.connections.Expire(now, std::chrono::seconds(60));
        peer.requests.Expire(now, std::chrono::seconds(60));
    }
    void Purge(Time now) {
        next_expiry = Time::max();
        for (auto entry = peers.begin(); entry != peers.end();) {
            const auto &peer = entry->second;
            const bool expired_ban = peer.banned && now >= peer.ban_until;
            const bool inactive = !peer.banned && now - peer.last_seen >= idle_expiry;
            if (expired_ban || inactive) entry = peers.erase(entry);
            else {
                next_expiry = std::min(next_expiry, peer.banned ? peer.ban_until : peer.last_seen + idle_expiry);
                ++entry;
            }
        }
    }
    Peer *FindOrTrack(const std::string &key, Time now) {
        auto existing = peers.find(key);
        if (existing != peers.end()) {
            Expire(existing->second, now);
            return &existing->second;
        }
        // Only scan on a new peer. Established sessions use constant-time
        // lookups, while expired entries are recovered before any capacity deny.
        if (now >= next_expiry) Purge(now);
        if (peers.size() >= options.max_tracked_peers) return nullptr;
        auto inserted = peers.emplace(key, Peer {});
        inserted.first->second.last_seen = now;
        next_expiry = std::min(next_expiry, now + idle_expiry);
        return &inserted.first->second;
    }
    void Ban(Peer &peer, Time now) {
        peer.banned = true;
        peer.ban_until = now + std::chrono::seconds(options.ban_seconds);
        peer.last_seen = now;
        next_expiry = std::min(next_expiry, peer.ban_until);
    }
    bool Admit(const std::string &raw_peer, bool connection) {
        std::lock_guard<std::mutex> lock(mutex);
        const auto key = CanonicalKey(raw_peer);
        if (key.empty()) return false;
        const auto now = clock();
        auto peer = FindOrTrack(key, now);
        if (!peer || peer->banned) return false;
        auto &window = connection ? peer->connections : peer->requests;
        const auto limit = connection ? options.connections_per_minute : options.requests_per_minute;
        if (window.count >= limit) { Ban(*peer, now); return false; }
        window.Add(now);
        peer->last_seen = now;
        return true;
    }
};

PeerGuard::PeerGuard(PeerGuardOptions options)
    : PeerGuard(options, [] { return std::chrono::steady_clock::now(); }) {}
PeerGuard::PeerGuard(PeerGuardOptions options, Clock clock)
    : impl_(std::make_unique<Impl>(options, std::move(clock))) {}
PeerGuard::~PeerGuard() = default;
bool PeerGuard::AdmitConnection(const std::string &peer) { return impl_->Admit(peer, true); }
bool PeerGuard::AdmitRequest(const std::string &peer) { return impl_->Admit(peer, false); }
void PeerGuard::AuthenticationFailed(const std::string &raw_peer) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto key = Impl::CanonicalKey(raw_peer);
    if (key.empty()) return;
    const auto now = impl_->clock();
    auto peer = impl_->FindOrTrack(key, now);
    if (!peer || peer->banned) return;
    peer->authentication.Add(now);
    peer->last_seen = now;
    if (peer->authentication.count >= impl_->options.auth_failures) impl_->Ban(*peer, now);
}
bool PeerGuard::IsBanned(const std::string &raw_peer) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto key = Impl::CanonicalKey(raw_peer);
    if (key.empty()) return true;
    const auto found = impl_->peers.find(key);
    if (found == impl_->peers.end()) return false;
    impl_->Expire(found->second, impl_->clock());
    return found->second.banned;
}

} // namespace duckonwire
