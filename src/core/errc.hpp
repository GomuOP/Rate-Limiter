#pragma once

#include <system_error>

namespace rl::core {

/// Hot-path error values (design 0.10). Startup failures still throw; everything
/// on accept/read/parse/check/respond returns one of these as an error_code.
enum class Errc {
    ok = 0,
    would_block,
    closed_by_peer,
    connection_reset,
    timed_out,
    protocol_error,
    message_too_large,
    redis_unavailable,
    redis_error,
};

const std::error_category& errc_category() noexcept;

inline std::error_code make_error_code(Errc e) noexcept {
    return {static_cast<int>(e), errc_category()};
}

} // namespace rl::core

template <> struct std::is_error_code_enum<rl::core::Errc> : std::true_type {};
