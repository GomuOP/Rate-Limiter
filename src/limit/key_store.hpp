#pragma once

#include <cstddef>
#include <list>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "core/clock.hpp"

namespace rl::limit {

/// Bounded per-key state store (design 0.4).
///
/// - `unordered_map` for O(1) lookup, plus an intrusive LRU list.
/// - On insert past `max_keys`, the least-recently-used entry is evicted.
/// - `sweep_expired()` drops entries whose last touch is older than `idle_ttl`;
///   because every touch moves the entry to the front, the list is ordered
///   newest-touch..oldest-touch and the sweep stops at the first live entry.
///
/// `std::list` nodes are address-stable across splice, so the map can key on a
/// `string_view` into the node-owned key string without a second copy.
template <typename V> class KeyStore {
  public:
    KeyStore(std::size_t max_keys, core::Duration idle_ttl, const core::Clock& clock)
        : max_keys_(max_keys ? max_keys : 1), idle_ttl_(idle_ttl), clock_(clock) {}

    KeyStore(const KeyStore&) = delete;
    KeyStore& operator=(const KeyStore&) = delete;

    /// Return the value for `key`, constructing it with `factory()` if absent.
    /// Marks the entry most-recently-used and may evict the LRU entry.
    template <typename F> V& get_or_create(std::string_view key, F&& factory) {
        const auto now = clock_.now();
        if (auto it = index_.find(key); it != index_.end()) {
            auto node = it->second;
            node->last_touch = now;
            lru_.splice(lru_.begin(), lru_, node); // move to front (MRU)
            return node->value;
        }

        if (lru_.size() >= max_keys_) {
            evict_lru();
        }

        lru_.emplace_front(Entry{std::string(key), std::forward<F>(factory)(), now});
        index_.emplace(std::string_view(lru_.front().key), lru_.begin());
        return lru_.front().value;
    }

    std::size_t size() const { return lru_.size(); }
    std::size_t evictions() const { return evictions_; }

    std::size_t sweep_expired() {
        const auto cutoff = clock_.now() - idle_ttl_;
        std::size_t removed = 0;
        while (!lru_.empty() && lru_.back().last_touch <= cutoff) {
            index_.erase(std::string_view(lru_.back().key));
            lru_.pop_back();
            ++removed;
        }
        return removed;
    }

  private:
    struct Entry {
        std::string key;
        V value;
        core::TimePoint last_touch;
    };

    void evict_lru() {
        index_.erase(std::string_view(lru_.back().key));
        lru_.pop_back();
        ++evictions_;
    }

    std::size_t max_keys_;
    core::Duration idle_ttl_;
    const core::Clock& clock_;
    std::list<Entry> lru_; // front = most recently used
    std::unordered_map<std::string_view, typename std::list<Entry>::iterator> index_;
    std::size_t evictions_ = 0;
};

} // namespace rl::limit
