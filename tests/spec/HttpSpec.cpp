// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ambroise Leclerc
/**
 * @file HttpSpec.cpp
 * @brief Scenarios for the HTTP client of the remote cache: URLs, requests and response parsing.
 */
import std;
import speclab;
import cppmcache.http;

namespace {

using namespace cppmcache;
using speclab::core::Assertions;
using speclab::core::Checks;

// Feeds a response to a reader one byte at a time, which exercises every resumption point.
HttpResponseReader readByteByByte(std::string_view response, bool head = false) {
    HttpResponseReader reader{head};
    for (const char byte : response) {
        reader.feed(std::string_view{&byte, 1});
    }
    return reader;
}

const speclab::Register urls{
    "HTTP: parsing the URL of a remote cache",
    "unit,http",
    [] {
        return speclab::Test("HTTP-001")
            .Then("host, port and prefix are extracted; anything but plain http is refused",
                  [] {
                      const auto simple = parseHttpUrl("http://cache.example");
                      const auto full   = parseHttpUrl("http://cache.example:8080/team/a/");
                      const auto ipv6   = parseHttpUrl("http://[::1]:9090");
                      Assertions::require(simple && full && ipv6, "a valid URL was refused");
                      Checks checks;
                      checks.expect(simple->host == "cache.example" && simple->port == 80 && simple->prefix.empty(), "default port, no prefix");
                      checks.expect(full->host == "cache.example" && full->port == 8080 && full->prefix == "/team/a",
                                    "port and prefix without a trailing slash");
                      checks.expect(ipv6->host == "::1" && ipv6->port == 9090, "IPv6 literal");
                      checks.expect(simple->authority() == "cache.example" && full->authority() == "cache.example:8080" && ipv6->authority() == "[::1]:9090",
                                    "Host header value");
                      checks.expect(!parseHttpUrl("https://cache.example"), "https is refused: there is no TLS");
                      checks.expect(!parseHttpUrl("cache.example:8080"), "missing scheme");
                      checks.expect(!parseHttpUrl("http://"), "missing host");
                      checks.expect(!parseHttpUrl("http://host:0") && !parseHttpUrl("http://host:65536") && !parseHttpUrl("http://host:x"), "invalid port");
                      checks.expect(!parseHttpUrl("http://host:"), "empty port");
                      checks.expect(!parseHttpUrl("http://user@host"), "credentials in the URL");
                      checks.expect(!parseHttpUrl("http://host/a?b"), "query string");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register requests{
    "HTTP: formatting requests",
    "unit,http",
    [] {
        return speclab::Test("HTTP-002")
            .Then("the target follows the prefix and only PUT carries a body",
                  [] {
                      const HttpUrl url = *parseHttpUrl("http://cache.example:8080/team");
                      Checks        checks;
                      checks.expect(formatHttpRequest("GET", url, "/ac/abc")
                                        == "GET /team/ac/abc HTTP/1.1\r\nHost: cache.example:8080\r\nUser-Agent: cppmcache\r\nAccept: */*\r\n\r\n",
                                    "GET");
                      checks.expect(formatHttpRequest("PUT", url, "/cas/abc", "body")
                                        == "PUT /team/cas/abc HTTP/1.1\r\nHost: cache.example:8080\r\nUser-Agent: cppmcache\r\nAccept: */*\r\n"
                                           "Content-Type: application/octet-stream\r\nContent-Length: 4\r\n\r\nbody",
                                    "PUT with a body");
                      checks.expect(formatHttpRequest("PUT", url, "/cas/abc").contains("Content-Length: 0\r\n"), "PUT of an empty blob declares a zero length");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register fixedLength{"HTTP: response with a declared length", "unit,http", [] {
                                        return speclab::Test("HTTP-003")
                                            .Then("the body is complete exactly at its length, whatever the fragmentation",
                                                  [] {
                                                      const std::string_view response = "HTTP/1.1 200 OK\r\ncontent-LENGTH: 5\r\nX-Other: y\r\n\r\nhello";
                                                      HttpResponseReader     whole;
                                                      whole.feed(response.substr(0, response.size() - 1));
                                                      const bool incompleteBeforeLastByte = !whole.complete();
                                                      whole.feed(response.substr(response.size() - 1));
                                                      const auto byByte = readByteByByte(response);

                                                      Checks checks;
                                                      checks.expect(incompleteBeforeLastByte, "not complete before the last byte");
                                                      checks.expect(whole.complete() && whole.response().status == 200 && whole.response().body == "hello",
                                                                    "status and body");
                                                      checks.expect(whole.response().keepAlive, "HTTP/1.1 keeps the connection by default");
                                                      checks.expect(byByte.complete() && byByte.response().body == "hello", "one byte at a time");
                                                      const std::string binaryBody{"\r\n\0\n", 4};
                                                      const auto        binary = readByteByByte("HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\n" + binaryBody);
                                                      checks.expect(binary.complete() && binary.response().body == binaryBody,
                                                                    "binary body with line terminators and a zero byte");
                                                      checks.raise();
                                                  })
                                            .Execute();
                                    }};

const speclab::Register chunked{
    "HTTP: chunked response",
    "unit,http",
    [] {
        return speclab::Test("HTTP-004")
            .Then("chunks are joined, extensions and trailers are skipped",
                  [] {
                      const std::string_view response = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                                                        "4;name=value\r\nWiki\r\n5\r\npedia\r\nA\r\n in chunks\r\n0\r\nTrailer: x\r\n\r\n";
                      HttpResponseReader     whole;
                      whole.feed(response);
                      const auto byByte = readByteByByte(response);
                      Checks     checks;
                      checks.expect(whole.complete() && whole.response().body == "Wikipedia in chunks", "fed at once");
                      checks.expect(byByte.complete() && byByte.response().body == "Wikipedia in chunks", "one byte at a time");
                      checks.expect(readByteByByte("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n").failed(), "invalid chunk size");
                      checks.expect(readByteByByte("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1\r\naXX").failed(),
                                    "chunk not followed by a line end");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register bodiless{"HTTP: responses without a body", "unit,http", [] {
                                     return speclab::Test("HTTP-005")
                                         .Then("HEAD, 204 and a zero length end with the headers; an interim response is skipped",
                                               [] {
                                                   Checks     checks;
                                                   const auto head = readByteByByte("HTTP/1.1 200 OK\r\nContent-Length: 1234\r\n\r\n", true);
                                                   checks.expect(head.complete() && head.response().status == 200 && head.response().body.empty(),
                                                                 "HEAD: the length describes a body that is not sent");
                                                   const auto missing = readByteByByte("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");
                                                   checks.expect(missing.complete() && missing.response().status == 404, "404 with a zero length");
                                                   checks.expect(readByteByByte("HTTP/1.1 204 No Content\r\n\r\n").complete(), "204");
                                                   const auto interim = readByteByByte(
                                                       "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
                                                   checks.expect(interim.complete() && interim.response().status == 200 && interim.response().body == "ok",
                                                                 "100 Continue then the real response");
                                                   checks.raise();
                                               })
                                         .Execute();
                                 }};

const speclab::Register connectionEnd{"HTTP: connection handling and malformed responses", "unit,http", [] {
                                          return speclab::Test("HTTP-006")
                                              .Then("a body without a length ends with the stream; a truncated response fails",
                                                    [] {
                                                        Checks checks;

                                                        auto       untilClose         = readByteByByte("HTTP/1.0 200 OK\r\n\r\npartial");
                                                        const bool pendingBeforeClose = !untilClose.complete();
                                                        untilClose.endOfStream();
                                                        checks.expect(pendingBeforeClose && untilClose.complete() && untilClose.response().body == "partial",
                                                                      "body delimited by the end of the stream");
                                                        checks.expect(!untilClose.response().keepAlive, "such a connection cannot be reused");

                                                        const auto closing = readByteByByte(
                                                            "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
                                                        checks.expect(closing.complete() && !closing.response().keepAlive, "Connection: close");

                                                        auto truncated = readByteByByte("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nshort");
                                                        truncated.endOfStream();
                                                        checks.expect(truncated.failed(), "stream ended before the declared length");

                                                        auto noHeaders = readByteByByte("HTTP/1.1 200 OK\r\nContent-Le");
                                                        noHeaders.endOfStream();
                                                        checks.expect(noHeaders.failed(), "stream ended inside the headers");

                                                        checks.expect(readByteByByte("SSH-2.0-OpenSSH\r\n\r\n").failed(), "not an HTTP response");
                                                        checks.expect(readByteByByte("HTTP/1.1 abc OK\r\n\r\n").failed(), "non-numeric status");
                                                        checks.raise();
                                                    })
                                              .Execute();
                                      }};

}  // namespace
