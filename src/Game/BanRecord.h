// src/Game/BanRecord.h — transport-neutral ban description.
//
// A small value type used to surface ban information across layer boundaries
// (SecurityManager/BanManager -> ConnectionLoginBridge -> GameServer ->
// AdminManager / command system) WITHOUT leaking the Security headers (which
// pull in a ClientAddress definition that clashes with the Network layer). The
// bridge maps its internal BanEntry to this neutral struct.

#pragma once

#include <string>

struct BanRecord {
    std::string steamId;
    bool        permanent = false;
    long long   remainingSeconds = 0;  // 0 when permanent
    std::string reason;
};
