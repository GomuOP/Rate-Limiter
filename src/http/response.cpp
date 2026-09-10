#include "http/response.hpp"

namespace rl::http {

const char* Response::reason_phrase(int code) {
    switch (code) {
    case 200:
        return "OK";
    case 400:
        return "Bad Request";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 413:
        return "Payload Too Large";
    case 429:
        return "Too Many Requests";
    case 431:
        return "Request Header Fields Too Large";
    case 500:
        return "Internal Server Error";
    case 501:
        return "Not Implemented";
    case 503:
        return "Service Unavailable";
    default:
        return "Status";
    }
}

std::string Response::serialize() const {
    std::string out;
    out.reserve(128 + body.size());

    out += "HTTP/1.1 ";
    out += std::to_string(status);
    out += ' ';
    out += reason_phrase(status);
    out += "\r\n";

    out += "Server: ratelimiter\r\n";
    for (const auto& [k, v] : headers) {
        out += k;
        out += ": ";
        out += v;
        out += "\r\n";
    }
    out += "Content-Length: ";
    out += std::to_string(body.size());
    out += "\r\n";
    out += "Connection: ";
    out += keep_alive ? "keep-alive" : "close";
    out += "\r\n\r\n";
    out += body;
    return out;
}

} // namespace rl::http
