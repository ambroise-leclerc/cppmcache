// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ambroise Leclerc
/**
 * @file Http.cppm
 * @brief The part of HTTP/1.1 a cache client needs: URLs, request formatting, response parsing.
 *
 * Nothing here touches the network, so all of it is tested without a server.
 */
export module cppmcache.http;

import std;

export namespace cppmcache {

// An `http://host[:port][/prefix]` URL. The prefix has no trailing slash.
struct HttpUrl {
    std::string   host;
    std::uint16_t port = 80;
    std::string   prefix;

    // Value of the Host header: the port is only written when it is not the default one.
    std::string authority() const {
        const std::string name = host.contains(':') ? std::format("[{}]", host) : host;
        return port == 80 ? name : std::format("{}:{}", name, port);
    }
};

// Parses an HTTP URL. Only the `http` scheme is accepted: there is no TLS.
std::optional<HttpUrl> parseHttpUrl(std::string_view text) {
    constexpr std::string_view scheme = "http://";
    if (!text.starts_with(scheme)) {
        return std::nullopt;
    }
    text.remove_prefix(scheme.size());
    const auto             slash     = text.find('/');
    std::string_view       authority = text.substr(0, slash);
    const std::string_view path      = slash == std::string_view::npos ? std::string_view{} : text.substr(slash);
    if (authority.empty() || authority.contains('@') || path.contains('?') || path.contains('#')) {
        return std::nullopt;
    }
    HttpUrl          url;
    std::string_view port;
    if (authority.starts_with('[')) {
        // IPv6 literal: [::1]:8080
        const auto close = authority.find(']');
        if (close == std::string_view::npos) {
            return std::nullopt;
        }
        url.host = authority.substr(1, close - 1);
        authority.remove_prefix(close + 1);
        if (!authority.empty()) {
            if (!authority.starts_with(':')) {
                return std::nullopt;
            }
            port = authority.substr(1);
        }
    } else if (const auto colon = authority.find(':'); colon != std::string_view::npos) {
        url.host = authority.substr(0, colon);
        port     = authority.substr(colon + 1);
    } else {
        url.host = authority;
    }
    if (url.host.empty()) {
        return std::nullopt;
    }
    if (!port.empty()) {
        unsigned value          = 0;
        const auto [end, error] = std::from_chars(port.data(), port.data() + port.size(), value);
        if (error != std::errc{} || end != port.data() + port.size() || value == 0 || value > 65535) {
            return std::nullopt;
        }
        url.port = static_cast<std::uint16_t>(value);
    } else if (authority.ends_with(':')) {
        return std::nullopt;
    }
    url.prefix = path;
    while (url.prefix.ends_with('/')) {
        url.prefix.pop_back();
    }
    return url;
}

// Formats a request. `target` is appended to the URL's prefix and starts with a slash. A body is
// sent with PUT only, with its exact length.
std::string formatHttpRequest(std::string_view method, const HttpUrl& url, std::string_view target, std::string_view body = {}) {
    std::string request = std::format("{} {}{} HTTP/1.1\r\nHost: {}\r\nUser-Agent: cppmcache\r\nAccept: */*\r\n", method, url.prefix, target, url.authority());
    if (method == "PUT") {
        request += std::format("Content-Type: application/octet-stream\r\nContent-Length: {}\r\n", body.size());
    }
    request += "\r\n";
    if (method == "PUT") {
        request += body;
    }
    return request;
}

struct HttpResponse {
    int         status = 0;
    std::string body;
    bool        keepAlive = true;  // the connection can carry another request
};

// Incremental parser of one response. Bytes are fed as they arrive; `endOfStream()` tells it the
// peer closed the connection, which ends a response that has no declared length.
class HttpResponseReader {
public:
    // `headRequest`: the response to a HEAD request has headers but never a body.
    explicit HttpResponseReader(bool headRequest = false) : headRequest_(headRequest) {}

    void feed(std::string_view bytes) {
        if (state_ == State::complete || state_ == State::failed) {
            return;
        }
        raw_.append(bytes);
        advance();
    }

    void endOfStream() {
        if (state_ == State::untilClose) {
            response_.body.append(raw_, cursor_);
            state_ = State::complete;
        } else if (state_ != State::complete) {
            state_ = State::failed;
        }
    }

    bool complete() const {
        return state_ == State::complete;
    }
    bool failed() const {
        return state_ == State::failed;
    }
    const HttpResponse& response() const {
        return response_;
    }

private:
    enum class State { headers, fixedLength, chunkSize, chunkData, chunkEnd, trailers, untilClose, complete, failed };

    static bool equalsIgnoreCase(std::string_view left, std::string_view right) {
        return std::ranges::equal(left, right, [](char a, char b) {
            const auto lower = [](char c) {
                return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
            };
            return lower(a) == lower(b);
        });
    }

    static std::string_view trim(std::string_view text) {
        while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
            text.remove_prefix(1);
        }
        while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
            text.remove_suffix(1);
        }
        return text;
    }

    // Next line of `raw_` without its terminator, or nothing if it has not fully arrived.
    std::optional<std::string_view> line() {
        const auto end = raw_.find("\r\n", cursor_);
        if (end == std::string::npos) {
            return std::nullopt;
        }
        const std::string_view text = std::string_view{raw_}.substr(cursor_, end - cursor_);
        cursor_                     = end + 2;
        return text;
    }

    bool parseHeaders() {
        const auto end = raw_.find("\r\n\r\n", cursor_);
        if (end == std::string::npos) {
            return false;
        }
        const auto status = line();
        // "HTTP/1.1 200 OK"
        if (!status || !status->starts_with("HTTP/1.") || status->size() < 12 || (*status)[8] != ' ') {
            state_ = State::failed;
            return false;
        }
        const auto [statusEnd, error] = std::from_chars(status->data() + 9, status->data() + 12, response_.status);
        if (error != std::errc{} || statusEnd != status->data() + 12) {
            state_ = State::failed;
            return false;
        }
        response_.keepAlive                  = (*status)[7] == '1';
        bool                         chunked = false;
        std::optional<std::uint64_t> length;
        while (const auto header = line()) {
            if (header->empty()) {
                break;
            }
            const auto colon = header->find(':');
            if (colon == std::string_view::npos) {
                continue;
            }
            const std::string_view name  = trim(header->substr(0, colon));
            const std::string_view value = trim(header->substr(colon + 1));
            if (equalsIgnoreCase(name, "content-length")) {
                std::uint64_t parsed = 0;
                if (std::from_chars(value.data(), value.data() + value.size(), parsed).ec != std::errc{}) {
                    state_ = State::failed;
                    return false;
                }
                length = parsed;
            } else if (equalsIgnoreCase(name, "transfer-encoding")) {
                chunked = value.size() >= 7 && equalsIgnoreCase(value.substr(value.size() - 7), "chunked");
            } else if (equalsIgnoreCase(name, "connection")) {
                response_.keepAlive = !equalsIgnoreCase(value, "close");
            }
        }
        // An interim response (100 Continue…) is followed by the real one.
        if (response_.status >= 100 && response_.status < 200) {
            response_ = {};
            return true;
        }
        if (headRequest_ || response_.status == 204 || response_.status == 304) {
            state_ = State::complete;
        } else if (chunked) {
            state_ = State::chunkSize;
        } else if (length) {
            remaining_ = *length;
            state_     = remaining_ == 0 ? State::complete : State::fixedLength;
        } else {
            response_.keepAlive = false;
            state_              = State::untilClose;
        }
        return true;
    }

    // Moves up to `remaining_` available bytes into the body.
    void takeBody() {
        const std::uint64_t available = raw_.size() - cursor_;
        const auto          take      = static_cast<std::size_t>(std::min(available, remaining_));
        response_.body.append(raw_, cursor_, take);
        cursor_    += take;
        remaining_ -= take;
    }

    void advance() {
        for (;;) {
            switch (state_) {
                case State::headers:
                    if (!parseHeaders()) {
                        return;
                    }
                    break;
                case State::fixedLength:
                    takeBody();
                    if (remaining_ != 0) {
                        return;
                    }
                    state_ = State::complete;
                    break;
                case State::chunkSize: {
                    const auto size = line();
                    if (!size) {
                        return;
                    }
                    // The size may be followed by extensions after a semicolon.
                    const std::string_view digits = trim(size->substr(0, size->find(';')));
                    if (digits.empty() || std::from_chars(digits.data(), digits.data() + digits.size(), remaining_, 16).ec != std::errc{}) {
                        state_ = State::failed;
                        return;
                    }
                    state_ = remaining_ == 0 ? State::trailers : State::chunkData;
                    break;
                }
                case State::chunkData:
                    takeBody();
                    if (remaining_ != 0) {
                        return;
                    }
                    state_ = State::chunkEnd;
                    break;
                case State::chunkEnd:
                    // The data of a chunk is followed by exactly one line terminator.
                    if (raw_.size() - cursor_ < 2) {
                        return;
                    }
                    if (raw_.compare(cursor_, 2, "\r\n") != 0) {
                        state_ = State::failed;
                        return;
                    }
                    cursor_ += 2;
                    state_   = State::chunkSize;
                    break;
                case State::trailers: {
                    const auto trailer = line();
                    if (!trailer) {
                        return;
                    }
                    if (trailer->empty()) {
                        state_ = State::complete;
                    }
                    break;
                }
                case State::untilClose:
                    response_.body.append(raw_, cursor_);
                    cursor_ = raw_.size();
                    return;
                case State::complete:
                case State::failed:
                    return;
            }
        }
    }

    bool          headRequest_;
    State         state_ = State::headers;
    std::string   raw_;
    std::size_t   cursor_    = 0;
    std::uint64_t remaining_ = 0;
    HttpResponse  response_;
};

}  // namespace cppmcache
