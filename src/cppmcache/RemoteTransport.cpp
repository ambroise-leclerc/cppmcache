// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ambroise Leclerc
/**
 * @file RemoteTransport.cpp
 * @brief Transport of the remote cache, built on stdnet (https://github.com/ambroise-leclerc/stdnet).
 *
 * Each operation is a stdnet sender run to completion on the calling thread by driving the
 * io_context. A timeout is a stop request sent to the pending operation.
 */
#include "RemoteTransport.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <network>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>

namespace {

namespace net = stdnet;
namespace ex  = stdnet::ex;

struct StopEnvironment {
    ex::inplace_stop_token token;
    ex::inplace_stop_token query(ex::get_stop_token_t) const noexcept {
        return token;
    }
};

template <class Values>
struct Outcome {
    std::optional<Values>          value;
    std::optional<std::error_code> error;
    std::exception_ptr             exception;
    bool                           stopped = false;
    bool                           done    = false;
};

template <class Values>
struct Receiver {
    using receiver_concept = ex::receiver_t;
    Outcome<Values>*       outcome;
    ex::inplace_stop_token token;

    template <class... Vs>
    void set_value(Vs&&... values) && noexcept {
        outcome->value.emplace(std::forward<Vs>(values)...);
        outcome->done = true;
    }
    void set_error(std::error_code error) && noexcept {
        outcome->error = error;
        outcome->done  = true;
    }
    void set_error(std::exception_ptr exception) && noexcept {
        outcome->exception = exception;
        outcome->done      = true;
    }
    void set_stopped() && noexcept {
        outcome->stopped = true;
        outcome->done    = true;
    }
    StopEnvironment get_env() const noexcept {
        return {token};
    }
};

template <class Sender>
using ValuesOf = ex::value_types_of_t<Sender, StopEnvironment, std::tuple, std::type_identity_t>;

// Runs a sender to completion on this thread. At the deadline a stop is requested, which completes
// the operation; the loop then only waits for that completion.
template <class Sender>
Outcome<ValuesOf<Sender>> runFor(net::io_context& context, Sender sender, int timeoutMilliseconds) {
    Outcome<ValuesOf<Sender>> outcome;
    ex::inplace_stop_source   stop;
    auto                      operation = ex::connect(std::move(sender), Receiver<ValuesOf<Sender>>{&outcome, stop.get_token()});
    ex::start(operation);
    const auto deadline      = std::chrono::steady_clock::now() + std::chrono::milliseconds{timeoutMilliseconds};
    bool       stopRequested = false;
    while (!outcome.done) {
        if (!stopRequested && std::chrono::steady_clock::now() >= deadline) {
            stop.request_stop();
            stopRequested = true;
        }
        if (context.stopped()) {
            context.restart();
        }
        context.run_for(std::chrono::milliseconds{20});
    }
    return outcome;
}

void describe(char* error, unsigned long long capacity, std::string_view message) noexcept {
    if (error == nullptr || capacity == 0) {
        return;
    }
    const auto length = static_cast<std::size_t>(capacity - 1) < message.size() ? static_cast<std::size_t>(capacity - 1) : message.size();
    std::memcpy(error, message.data(), length);
    error[length] = '\0';
}

// Message for an operation that did not complete with a value.
template <class Values>
std::string failure(const Outcome<Values>& outcome) {
    if (outcome.stopped) {
        return "timed out";
    }
    if (outcome.error) {
        return outcome.error->message();
    }
    if (outcome.exception) {
        try {
            std::rethrow_exception(outcome.exception);
        } catch (const std::exception& exception) {
            return exception.what();
        } catch (...) {
            return "unknown error";
        }
    }
    return "no result";
}

}  // namespace

namespace cppmcache::transport {

struct Connection {
    net::io_context                context;
    std::optional<net::connection> stream;
};

bool available() noexcept {
    return true;
}

Connection* open(const char* host, unsigned short port, int timeoutMilliseconds, char* error, unsigned long long errorCapacity) noexcept {
    try {
        auto               connection = new Connection;
        net::preconnection preconnection(net::remote_endpoint().with_hostname(host).with_port(port),
                                         net::transport_properties::reliable_stream(),
                                         net::security_parameters::disabled());
        auto outcome = runFor(connection->context, net::initiate(connection->context.get_scheduler(), std::move(preconnection)), timeoutMilliseconds);
        if (!outcome.value) {
            describe(error, errorCapacity, failure(outcome));
            delete connection;
            return nullptr;
        }
        connection->stream.emplace(std::move(std::get<0>(*outcome.value)));
        return connection;
    } catch (const std::exception& exception) {
        describe(error, errorCapacity, exception.what());
    } catch (...) {
        describe(error, errorCapacity, "unknown error");
    }
    return nullptr;
}

bool sendAll(Connection*        connection,
             const char*        data,
             unsigned long long size,
             int                timeoutMilliseconds,
             char*              error,
             unsigned long long errorCapacity) noexcept {
    try {
        const std::string_view bytes{data, static_cast<std::size_t>(size)};
        auto                   outcome = runFor(connection->context, net::async_send(*connection->stream, net::buffer(bytes)), timeoutMilliseconds);
        if (!outcome.value) {
            describe(error, errorCapacity, failure(outcome));
            return false;
        }
        return true;
    } catch (const std::exception& exception) {
        describe(error, errorCapacity, exception.what());
    } catch (...) {
        describe(error, errorCapacity, "unknown error");
    }
    return false;
}

long long receiveSome(Connection*        connection,
                      char*              buffer,
                      unsigned long long capacity,
                      int                timeoutMilliseconds,
                      char*              error,
                      unsigned long long errorCapacity) noexcept {
    try {
        auto outcome = runFor(connection->context,
                              net::async_receive(*connection->stream, net::buffer(buffer, static_cast<std::size_t>(capacity))),
                              timeoutMilliseconds);
        if (!outcome.value) {
            describe(error, errorCapacity, failure(outcome));
            return -1;
        }
        return static_cast<long long>(std::get<0>(*outcome.value));
    } catch (const std::exception& exception) {
        describe(error, errorCapacity, exception.what());
    } catch (...) {
        describe(error, errorCapacity, "unknown error");
    }
    return -1;
}

void close(Connection* connection) noexcept {
    try {
        delete connection;
    } catch (...) {}
}

}  // namespace cppmcache::transport
