#pragma once

#include <cstddef>
#include <cstring>
#include <string_view>
#include <vector>

namespace rl::net {

/// Growable byte buffer with separate read and write cursors.
///
/// Layout:  [ 0 .. r_ )  consumed   [ r_ .. w_ )  readable   [ w_ .. cap )  free
/// When the readable region is drained it is compacted back to offset 0.
class Buffer {
  public:
    explicit Buffer(std::size_t initial = 4096) : data_(initial) {}

    std::size_t readable() const { return w_ - r_; }
    const char* read_ptr() const { return data_.data() + r_; }
    std::string_view view() const { return {read_ptr(), readable()}; }
    bool empty() const { return r_ == w_; }

    void consume(std::size_t n) {
        r_ += n;
        if (r_ == w_) {
            r_ = w_ = 0; // fully drained -> reset to front
        }
    }

    /// Ensure at least `n` contiguous writable bytes, compacting/growing as needed.
    char* reserve(std::size_t n) {
        if (writable() >= n) {
            return data_.data() + w_;
        }
        if (r_ > 0) { // compact
            std::memmove(data_.data(), data_.data() + r_, readable());
            w_ -= r_;
            r_ = 0;
        }
        if (writable() < n) {
            data_.resize(w_ + n);
        }
        return data_.data() + w_;
    }

    void commit(std::size_t n) { w_ += n; }

    void append(std::string_view s) {
        std::memcpy(reserve(s.size()), s.data(), s.size());
        commit(s.size());
    }

    void clear() { r_ = w_ = 0; }
    std::size_t capacity() const { return data_.size(); }

  private:
    std::size_t writable() const { return data_.size() - w_; }

    std::vector<char> data_;
    std::size_t r_ = 0;
    std::size_t w_ = 0;
};

} // namespace rl::net
