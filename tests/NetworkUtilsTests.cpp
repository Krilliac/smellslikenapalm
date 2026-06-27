// tests/NetworkUtilsTests.cpp
//
// Native unit tests for src/Network/NetworkUtils.{h,cpp}: IPv4 validation /
// normalization, delimiter split/join, and host<->network port byte order.

#include "TestFramework.h"

#include "Network/NetworkUtils.h"

#include <cstdint>
#include <string>
#include <vector>

TEST(NetworkUtils, IsValidIPv4) {
    EXPECT_TRUE(NetworkUtils::IsValidIPv4("0.0.0.0"));
    EXPECT_TRUE(NetworkUtils::IsValidIPv4("127.0.0.1"));
    EXPECT_TRUE(NetworkUtils::IsValidIPv4("255.255.255.255"));
    EXPECT_FALSE(NetworkUtils::IsValidIPv4("256.0.0.1"));
    EXPECT_FALSE(NetworkUtils::IsValidIPv4("1.2.3"));
    EXPECT_FALSE(NetworkUtils::IsValidIPv4("abc.def.ghi.jkl"));
    EXPECT_FALSE(NetworkUtils::IsValidIPv4(""));
}

TEST(NetworkUtils, NormalizeIPv4) {
    auto a = NetworkUtils::NormalizeIPv4("192.168.001.010");
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(*a, "192.168.1.10");

    auto b = NetworkUtils::NormalizeIPv4("10.0.0.1");
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(*b, "10.0.0.1");

    // Out-of-range octet and wrong octet count are rejected.
    EXPECT_FALSE(NetworkUtils::NormalizeIPv4("192.168.1.256").has_value());
    EXPECT_FALSE(NetworkUtils::NormalizeIPv4("1.2.3").has_value());
}

TEST(NetworkUtils, SplitJoinRoundTrip) {
    auto parts = NetworkUtils::SplitString("a:b:c", ':');
    ASSERT_EQ(parts.size(), 3u);
    EXPECT_EQ(parts[0], "a");
    EXPECT_EQ(parts[2], "c");
    EXPECT_EQ(NetworkUtils::JoinString(parts, ':'), "a:b:c");

    // SplitString keeps interior empty fields (unlike StringUtils::Split).
    auto withEmpty = NetworkUtils::SplitString("a::c", ':');
    ASSERT_EQ(withEmpty.size(), 3u);
    EXPECT_EQ(withEmpty[1], "");
}

TEST(NetworkUtils, PortByteOrderRoundTrip) {
    for (uint16_t port : {uint16_t(0), uint16_t(80), uint16_t(8080),
                          uint16_t(27015), uint16_t(65535)}) {
        uint16_t net = NetworkUtils::HostToNetworkPort(port);
        EXPECT_EQ(NetworkUtils::NetworkToHostPort(net), port) << "port: " << port;
    }
}

TEST(NetworkUtils, ChecksumIsDeterministic) {
    std::vector<uint8_t> data = {0x10, 0x20, 0x30, 0x40};
    uint8_t c1 = NetworkUtils::ComputeChecksum(data.data(), data.size());
    uint8_t c2 = NetworkUtils::ComputeChecksum(data.data(), data.size());
    EXPECT_EQ(c1, c2);
    // A changed byte changes the checksum.
    std::vector<uint8_t> changed = {0x10, 0x20, 0x30, 0x41};
    EXPECT_NE(NetworkUtils::ComputeChecksum(changed.data(), changed.size()), c1);
}

RS2V_TEST_MAIN()
