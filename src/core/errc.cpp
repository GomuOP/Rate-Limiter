#include "core/errc.hpp"

namespace rl::core {

namespace {

class ErrcCategory final : public std::error_category {
  public:
    const char* name() const noexcept override { return "rl"; }

    std::string message(int ev) const override {
        switch (static_cast<Errc>(ev)) {
        case Errc::ok:
            return "ok";
        case Errc::would_block:
            return "operation would block";
        case Errc::closed_by_peer:
            return "connection closed by peer";
        case Errc::connection_reset:
            return "connection reset";
        case Errc::timed_out:
            return "operation timed out";
        case Errc::protocol_error:
            return "protocol error";
        case Errc::message_too_large:
            return "message too large";
        case Errc::redis_unavailable:
            return "redis unavailable";
        case Errc::redis_error:
            return "redis returned an error";
        }
        return "unknown rl error " + std::to_string(ev);
    }
};

} // namespace

const std::error_category& errc_category() noexcept {
    static const ErrcCategory cat;
    return cat;
}

} // namespace rl::core
