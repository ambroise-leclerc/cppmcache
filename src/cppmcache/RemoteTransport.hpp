// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ambroise Leclerc
/**
 * @file RemoteTransport.hpp
 * @brief Blocking TCP transport of the remote cache, behind a boundary free of library headers.
 *
 * The modules of cppmcache use `import std;`, and a translation unit must not mix that with
 * `#include`s of the standard library. The networking library is header-based, so it lives in one
 * classic translation unit (RemoteTransport.cpp) and is reached through this header, which
 * deliberately includes nothing and uses built-in types only.
 */
#pragma once

namespace cppmcache::transport {

struct Connection;

// Whether this build of cppmcache contains a transport at all.
bool available() noexcept;

// Connects to host:port. Returns nothing on failure, with a message in `error`.
Connection* open(const char* host, unsigned short port, int timeoutMilliseconds, char* error, unsigned long long errorCapacity) noexcept;

// Sends the whole buffer. Returns false on failure or timeout, with a message in `error`.
bool sendAll(Connection*        connection,
             const char*        data,
             unsigned long long size,
             int                timeoutMilliseconds,
             char*              error,
             unsigned long long errorCapacity) noexcept;

// Receives what is available: the number of bytes read, 0 at the end of the stream, or -1 on
// failure or timeout, with a message in `error`.
long long
receiveSome(Connection* connection, char* buffer, unsigned long long capacity, int timeoutMilliseconds, char* error, unsigned long long errorCapacity) noexcept;

void close(Connection* connection) noexcept;

}  // namespace cppmcache::transport
