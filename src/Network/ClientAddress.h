// src/Network/ClientAddress.h
//
// Canonical (ip, port) client identity and its std::hash specialization.
//
// This type was previously redefined verbatim in four separate headers
// (Network/BandwidthManager.h, Network/NetworkInterface.h,
// Security/NetworkBlocker.h, Time/TimeSync.h). Because the struct and the
// std::hash<ClientAddress> specialization were defined at namespace scope in
// each, pulling any two of those headers into the same translation unit was an
// ODR violation / redefinition error. All four now include this single header.

#pragma once

#include <string>
#include <cstdint>
#include <functional>

struct ClientAddress {
    std::string ip;
    uint16_t    port = 0;

    bool operator==(const ClientAddress& o) const {
        return ip == o.ip && port == o.port;
    }
};

namespace std {
template<> struct hash<ClientAddress> {
    std::size_t operator()(const ClientAddress& a) const noexcept {
        return std::hash<std::string>()(a.ip) ^ (std::hash<uint16_t>()(a.port) << 1);
    }
};
} // namespace std
