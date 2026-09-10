#pragma once

#include <string>

#include "core/config.hpp"
#include "http/request.hpp"

namespace rl::limit {

/// Turns a request into a limit key according to KeyExtractorConfig (design 0.5).
class KeyExtractor {
  public:
    struct Result {
        bool ok = false;   // false -> respond 400
        std::string key;   // valid only when ok
        std::string error; // human-readable reason when !ok
    };

    explicit KeyExtractor(core::KeyExtractorConfig cfg) : cfg_(std::move(cfg)) {}

    Result extract(const http::Request& req) const;

    /// Percent-decoding for `application/x-www-form-urlencoded` query values
    /// ('+' -> space, "%XX" -> byte). Exposed for testing.
    static std::string url_decode(std::string_view in);

  private:
    core::KeyExtractorConfig cfg_;
};

} // namespace rl::limit
