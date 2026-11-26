// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ambroise Leclerc
/**
 * @file RemoteTransportDisabled.cpp
 * @brief Stand-in for the transport of the remote cache in a build configured without it.
 */
#include "RemoteTransport.hpp"

namespace cppmcache::transport {

namespace {

void describe(char* error, unsigned long long capacity) noexcept {
    static constexpr char message[] = "this cppmcache was built without remote cache support";
    if (error == nullptr || capacity == 0) {
        return;
    }
    unsigned long long length = 0;
    while (length + 1 < capacity && message[length] != '\0') {
        error[length] = message[length];
        ++length;
    }
    error[length] = '\0';
}

}  // namespace

bool available() noexcept {
    return false;
}

Connection* open(const char*, unsigned short, int, char* error, unsigned long long errorCapacity) noexcept {
    describe(error, errorCapacity);
    return nullptr;
}

bool sendAll(Connection*, const char*, unsigned long long, int, char* error, unsigned long long errorCapacity) noexcept {
    describe(error, errorCapacity);
    return false;
}

long long receiveSome(Connection*, char*, unsigned long long, int, char* error, unsigned long long errorCapacity) noexcept {
    describe(error, errorCapacity);
    return -1;
}

void close(Connection*) noexcept {}

}  // namespace cppmcache::transport
