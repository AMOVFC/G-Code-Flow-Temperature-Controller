// A minimal HTTP server for the local web UI.
//
// Frontend code. It lives in cli/, never in core/ -- ADR-0002's rule is that the core
// links nothing that knows a user exists, and an HTTP server is emphatically a user
// interface.
//
// Deliberately small: single-threaded, HTTP/1.1, no TLS, **bound to 127.0.0.1 only**.
// This process reads and writes files anywhere the user can, so it must never be
// reachable from the network.

#pragma once

#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace sb53::web {

struct Request {
    std::string method;
    std::string path;
    std::string body;

    // Form-encoded body fields (application/x-www-form-urlencoded).
    //
    // Form encoding rather than JSON so the server needs no JSON parser -- only a
    // writer, which is trivial. One less thing to get wrong in a component that parses
    // untrusted input.
    std::map<std::string, std::string> fields;

    [[nodiscard]] std::string field(std::string_view name,
                                    std::string_view fallback = {}) const;
    [[nodiscard]] double number(std::string_view name, double fallback) const;
};

struct Response {
    int status = 200;
    std::string contentType = "text/plain; charset=utf-8";
    std::string body;

    static Response html(std::string body);
    static Response json(std::string body);
    static Response error(int status, std::string message);
};

using Handler = std::function<Response(const Request&)>;

// Blocks until the process is interrupted. Returns false if the port could not be bound.
bool serve(unsigned short port, const Handler& handler);

// Minimal JSON emission. Escapes what must be escaped; no parser needed.
[[nodiscard]] std::string jsonEscape(std::string_view text);
[[nodiscard]] std::string jsonNumber(double value);

} // namespace sb53::web
