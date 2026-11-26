// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ambroise Leclerc
/**
 * @file Remote.cppm
 * @brief Client of a remote cache that speaks Bazel's HTTP caching protocol.
 *
 * The server stores blobs under two paths: `/ac/<sha256>` for action results and `/cas/<sha256>`
 * for content. That is the layout bazel-remote, Bazel's `--remote_cache=http://…` and ccache's
 * `http` storage with `layout=bazel` use, so the keys and the ActionResult messages are those of
 * the Remote Execution API even though the transport is plain HTTP.
 */
module;

#include "RemoteTransport.hpp"

export module cppmcache.remote;

import std;
import cppmcache.http;

export namespace cppmcache {

class RemoteCache {
public:
    RemoteCache(HttpUrl url, int timeoutMilliseconds) : url_(std::move(url)), timeout_(timeoutMilliseconds) {}
    RemoteCache(const RemoteCache&)            = delete;
    RemoteCache& operator=(const RemoteCache&) = delete;
    ~RemoteCache() {
        disconnect();
    }

    // Whether this build contains a network transport.
    static bool available() {
        return transport::available();
    }

    // Content stored under kind/hash, or nothing if it is absent or the server cannot be reached.
    std::optional<std::string> get(std::string_view kind, std::string_view hash) {
        const auto response = request("GET", target(kind, hash));
        if (!response || response->status == 404) {
            return std::nullopt;
        }
        if (response->status != 200) {
            fail(std::format("GET {}: HTTP status {}", target(kind, hash), response->status));
            return std::nullopt;
        }
        return response->body;
    }

    bool has(std::string_view kind, std::string_view hash) {
        const auto response = request("HEAD", target(kind, hash));
        return response && response->status == 200;
    }

    bool put(std::string_view kind, std::string_view hash, std::string_view content) {
        const auto response = request("PUT", target(kind, hash), content);
        if (!response) {
            return false;
        }
        if (response->status < 200 || response->status >= 300) {
            fail(std::format("PUT {}: HTTP status {}", target(kind, hash), response->status));
            return false;
        }
        return true;
    }

    // Number of requests that failed, and the message of the last failure.
    int errors() const {
        return errors_;
    }
    const std::string& lastError() const {
        return lastError_;
    }

private:
    static std::string target(std::string_view kind, std::string_view hash) {
        return std::format("/{}/{}", kind, hash);
    }

    void fail(std::string message) {
        ++errors_;
        lastError_ = std::move(message);
    }

    void disconnect() {
        if (connection_ != nullptr) {
            transport::close(connection_);
            connection_ = nullptr;
        }
    }

    bool connect() {
        if (connection_ != nullptr) {
            return true;
        }
        std::array<char, 256> error{};
        // Connecting to a server that is down must not cost a whole operation timeout per compilation.
        connection_ = transport::open(url_.host.c_str(), url_.port, std::min(timeout_, 2000), error.data(), error.size());
        if (connection_ == nullptr) {
            fail(std::format("cannot connect to {}: {}", url_.authority(), error.data()));
            unreachable_ = true;
            return false;
        }
        return true;
    }

    // One exchange on the current connection. Nothing is returned on a transport failure.
    std::optional<HttpResponse> exchange(std::string_view method, const std::string& message, std::string& error) {
        std::array<char, 256> detail{};
        if (!transport::sendAll(connection_, message.data(), message.size(), timeout_, detail.data(), detail.size())) {
            error = std::format("send: {}", detail.data());
            return std::nullopt;
        }
        HttpResponseReader reader{method == "HEAD"};
        std::vector<char>  buffer(64 * 1024);
        while (!reader.complete() && !reader.failed()) {
            const long long received = transport::receiveSome(connection_, buffer.data(), buffer.size(), timeout_, detail.data(), detail.size());
            if (received < 0) {
                error = std::format("receive: {}", detail.data());
                return std::nullopt;
            }
            if (received == 0) {
                reader.endOfStream();
                break;
            }
            reader.feed(std::string_view{buffer.data(), static_cast<std::size_t>(received)});
        }
        if (!reader.complete()) {
            error = "malformed or truncated response";
            return std::nullopt;
        }
        return reader.response();
    }

    std::optional<HttpResponse> request(std::string_view method, const std::string& path, std::string_view body = {}) {
        // After a failed connection the server is left alone for the rest of this process.
        if (unreachable_) {
            return std::nullopt;
        }
        const std::string message = formatHttpRequest(method, url_, path, body);
        std::string       error;
        // A kept-alive connection may have been closed by the server in the meantime: one retry
        // on a fresh connection tells that apart from a server that is really failing.
        for (int attempt = 0; attempt < 2; ++attempt) {
            const bool reused = connection_ != nullptr;
            if (!connect()) {
                return std::nullopt;
            }
            auto response = exchange(method, message, error);
            if (response) {
                if (!response->keepAlive) {
                    disconnect();
                }
                return response;
            }
            disconnect();
            if (!reused) {
                break;
            }
        }
        fail(std::format("{} {}: {}", method, path, error));
        unreachable_ = true;
        return std::nullopt;
    }

    HttpUrl                url_;
    int                    timeout_;
    transport::Connection* connection_  = nullptr;
    bool                   unreachable_ = false;
    int                    errors_      = 0;
    std::string            lastError_;
};

}  // namespace cppmcache
