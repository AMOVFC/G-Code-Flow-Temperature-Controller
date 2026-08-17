#include "HttpServer.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// windows.h defines min/max as macros, which breaks std::min at the call site below.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
#define closesocket ::close
#endif

namespace sb53::web {
namespace {

[[nodiscard]] int hexValue(char c) noexcept
{
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    return -1;
}

[[nodiscard]] std::string urlDecode(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '+') {
            out += ' ';
        } else if (text[i] == '%' && i + 2 < text.size()) {
            const int hi = hexValue(text[i + 1]);
            const int lo = hexValue(text[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>(hi * 16 + lo);
                i += 2;
            } else {
                out += text[i];
            }
        } else {
            out += text[i];
        }
    }
    return out;
}

void parseFields(std::string_view body, std::map<std::string, std::string>& out)
{
    std::size_t start = 0;
    while (start <= body.size()) {
        const auto amp = body.find('&', start);
        const auto pair = body.substr(start, amp == std::string_view::npos
                                                 ? std::string_view::npos
                                                 : amp - start);
        if (!pair.empty()) {
            const auto eq = pair.find('=');
            if (eq != std::string_view::npos) {
                out[urlDecode(pair.substr(0, eq))] = urlDecode(pair.substr(eq + 1));
            }
        }
        if (amp == std::string_view::npos) {
            break;
        }
        start = amp + 1;
    }
}

[[nodiscard]] std::string statusText(int status)
{
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    default:  return "Error";
    }
}

bool sendAll(socket_t sock, const std::string& data)
{
    std::size_t sent = 0;
    while (sent < data.size()) {
        const auto n = ::send(sock, data.data() + sent,
                              static_cast<int>(data.size() - sent), 0);
        if (n <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

// Caps the request body. Nothing legitimate here is large, and an unbounded read is a
// trivial way to exhaust memory.
constexpr std::size_t kMaxBody = 1u << 20;

} // namespace

std::string Request::field(std::string_view name, std::string_view fallback) const
{
    const auto it = fields.find(std::string(name));
    return it == fields.end() ? std::string(fallback) : it->second;
}

double Request::number(std::string_view name, double fallback) const
{
    const auto it = fields.find(std::string(name));
    if (it == fields.end() || it->second.empty()) {
        return fallback;
    }
    try {
        return std::stod(it->second);
    } catch (...) {
        return fallback;
    }
}

Response Response::html(std::string body)
{
    return Response{200, "text/html; charset=utf-8", std::move(body)};
}

Response Response::json(std::string body)
{
    return Response{200, "application/json; charset=utf-8", std::move(body)};
}

Response Response::error(int status, std::string message)
{
    return Response{status, "application/json; charset=utf-8",
                    "{\"error\":\"" + jsonEscape(message) + "\"}"};
}

std::string jsonEscape(std::string_view text)
{
    std::string out;
    out.reserve(text.size() + 8);
    for (const char c : text) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x",
                              static_cast<unsigned char>(c));
                out += buf;
            } else {
                out += c;
            }
        }
    }
    return out;
}

std::string jsonNumber(double value)
{
    // JSON has no NaN or Infinity. Emitting them produces a page that fails to parse
    // with no clue why, so map them to null.
    if (!std::isfinite(value)) {
        return "null";
    }
    char buf[40];
    const auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), value,
                                         std::chars_format::fixed, 4);
    if (ec != std::errc{}) {
        return "null";
    }
    std::string out(buf, ptr);
    if (out.find('.') != std::string::npos) {
        out.erase(out.find_last_not_of('0') + 1);
        if (!out.empty() && out.back() == '.') {
            out.pop_back();
        }
    }
    return out.empty() ? "0" : out;
}

namespace {

// Serves one connection to completion, then closes it.
//
// Runs on its own thread. Two reasons, both of which broke the browser before threading
// existed:
//
// 1. Browsers open SPECULATIVE connections and frequently send nothing on them. A
//    single-threaded server accepts one, blocks in recv() waiting for a request that
//    never arrives, and never accepts the real one. The page reports
//    "TypeError: Failed to fetch" because the request genuinely never reached us.
// 2. Processing a large G-code file takes ten seconds or more, and everything else the
//    page asks for would queue behind it.
void handleConnection(socket_t client, const Handler& handler)
{
    // Drop a connection that goes quiet instead of blocking on it forever. This is what
    // reaps those speculative connections.
#ifdef _WIN32
    DWORD timeout = 15000;                       // milliseconds
#else
    timeval timeout{15, 0};
#endif
    ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&timeout), sizeof(timeout));

    std::string raw;
    char buffer[8192];
    std::size_t headerEnd = std::string::npos;

    while (headerEnd == std::string::npos) {
        const auto n = ::recv(client, buffer, sizeof(buffer), 0);
        if (n <= 0) {
            break;                               // closed, or the read timed out
        }
        raw.append(buffer, static_cast<std::size_t>(n));
        headerEnd = raw.find("\r\n\r\n");
        if (raw.size() > kMaxBody) {
            break;
        }
    }

    if (headerEnd == std::string::npos) {
        closesocket(client);
        return;
    }

    Request request;
    {
        const auto lineEnd = raw.find("\r\n");
        const std::string requestLine = raw.substr(0, lineEnd);
        const auto sp1 = requestLine.find(' ');
        const auto sp2 = requestLine.find(' ', sp1 + 1);
        if (sp1 != std::string::npos && sp2 != std::string::npos) {
            request.method = requestLine.substr(0, sp1);
            request.path = requestLine.substr(sp1 + 1, sp2 - sp1 - 1);
        }
    }

    std::size_t contentLength = 0;
    {
        const std::string headers = raw.substr(0, headerEnd);
        auto pos = headers.find("Content-Length:");
        if (pos == std::string::npos) {
            pos = headers.find("content-length:");
        }
        if (pos != std::string::npos) {
            const auto valueStart = headers.find_first_not_of(" ", pos + 15);
            contentLength = static_cast<std::size_t>(
                std::strtoul(headers.c_str() + valueStart, nullptr, 10));
            contentLength = std::min(contentLength, kMaxBody);
        }
    }

    std::string body = raw.substr(headerEnd + 4);
    while (body.size() < contentLength) {
        const auto n = ::recv(client, buffer, sizeof(buffer), 0);
        if (n <= 0) {
            break;
        }
        body.append(buffer, static_cast<std::size_t>(n));
    }
    request.body = body.substr(0, std::min(contentLength, body.size()));
    parseFields(request.body, request.fields);

    if (const auto q = request.path.find('?'); q != std::string::npos) {
        parseFields(std::string_view(request.path).substr(q + 1), request.fields);
        request.path = request.path.substr(0, q);
    }

    Response response;
    try {
        response = handler(request);
    } catch (const std::exception& e) {
        response = Response::error(500, e.what());
    } catch (...) {
        response = Response::error(500, "unknown error");
    }

    std::string out = "HTTP/1.1 " + std::to_string(response.status) + " " +
                      statusText(response.status) + "\r\n";
    out += "Content-Type: " + response.contentType + "\r\n";
    out += "Content-Length: " + std::to_string(response.body.size()) + "\r\n";
    out += "Cache-Control: no-store\r\n";
    out += "Connection: close\r\n\r\n";
    out += response.body;

    sendAll(client, out);
    closesocket(client);
}

} // namespace

bool serve(unsigned short port, const Handler& handler)
{
#ifdef _WIN32
    WSADATA wsa{};
    if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::fprintf(stderr, "error: WSAStartup failed\n");
        return false;
    }
#endif

    const socket_t listener = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener == kInvalidSocket) {
        std::fprintf(stderr, "error: could not create a socket\n");
        return false;
    }

    int reuse = 1;
    ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = ::htons(port);
    // Loopback ONLY. This process reads and writes arbitrary files; it must not be
    // reachable from the network.
    address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);

    if (::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        std::fprintf(stderr, "error: port %u is already in use.\n", port);
        closesocket(listener);
        return false;
    }
    if (::listen(listener, 16) != 0) {
        std::fprintf(stderr, "error: listen failed\n");
        closesocket(listener);
        return false;
    }

    for (;;) {
        const socket_t client = ::accept(listener, nullptr, nullptr);
        if (client == kInvalidSocket) {
            continue;
        }
        std::thread(handleConnection, client, std::cref(handler)).detach();
    }
}

} // namespace sb53::web
