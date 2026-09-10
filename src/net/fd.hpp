#pragma once

#include <unistd.h>

#include <utility>

namespace rl::net {

/// Move-only owning wrapper around a file descriptor.
class Fd {
  public:
    Fd() = default;
    explicit Fd(int fd) : fd_(fd) {}

    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;

    Fd(Fd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }

    ~Fd() { reset(); }

    int get() const { return fd_; }
    explicit operator bool() const { return fd_ >= 0; }

    int release() { return std::exchange(fd_, -1); }

    void reset(int fd = -1) {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = fd;
    }

  private:
    int fd_ = -1;
};

} // namespace rl::net
