// Platform-neutral helpers shared by native network-counter adapters and tests.

#pragma once

#include <cstdint>
#include <limits>

namespace Telemetry::Detail {

struct NetworkInterfaceCounterSample {
    bool isLoopback = false;
    bool isFilterInterface = false;
    uint64_t bytesSent = 0;
    uint64_t bytesReceived = 0;
};

struct NetworkInterfaceCounterTotals {
    uint64_t bytesSent = 0;
    uint64_t bytesReceived = 0;
};

constexpr uint64_t SaturatingAdd(uint64_t left, uint64_t right) noexcept {
    constexpr uint64_t maximum = std::numeric_limits<uint64_t>::max();
    return right > maximum - left ? maximum : left + right;
}

constexpr void AccumulateNetworkInterfaceCounters(
    NetworkInterfaceCounterTotals& totals,
    const NetworkInterfaceCounterSample& sample) noexcept {
    // Loopback traffic is not external server I/O. Windows filter interfaces
    // observe traffic that is already accounted for by their underlying
    // interface, so including them would double count bytes.
    if (sample.isLoopback || sample.isFilterInterface) {
        return;
    }

    totals.bytesSent = SaturatingAdd(totals.bytesSent, sample.bytesSent);
    totals.bytesReceived = SaturatingAdd(totals.bytesReceived, sample.bytesReceived);
}

} // namespace Telemetry::Detail
