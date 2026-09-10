#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace rl::http {

/// Percent-decode an `application/x-www-form-urlencoded` token: `%XX` escapes
/// and `+` for space. Malformed `%` escapes are left verbatim.
std::string url_decode(std::string_view in);

/// First value for `name` in a raw `a=1&b=2` query string, percent-decoded.
/// Returns nullopt when the key is absent (a present-but-empty value like
/// `a=&b=2` yields an empty string, not nullopt).
std::optional<std::string> query_param(std::string_view query, std::string_view name);

} // namespace rl::http
