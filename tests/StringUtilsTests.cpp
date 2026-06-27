// tests/StringUtilsTests.cpp
//
// Native unit tests for src/Utils/StringUtils.{h,cpp} — the string helpers used
// throughout config parsing, URL/option handling, and logging. Exercises the
// real namespace API (the legacy UtilsTests.cpp wrongly treated these as a
// class; this file tests them as the free functions they actually are).

#include "TestFramework.h"

#include "Utils/StringUtils.h"

#include <string>
#include <vector>

TEST(StringUtils, TrimRemovesSurroundingWhitespace) {
    EXPECT_EQ(StringUtils::Trim("  hello  "), "hello");
    EXPECT_EQ(StringUtils::Trim("\t\n value \r\n"), "value");
    EXPECT_EQ(StringUtils::Trim("noedge"), "noedge");
    EXPECT_EQ(StringUtils::Trim("   "), "");
    EXPECT_EQ(StringUtils::Trim(""), "");
}

TEST(StringUtils, SplitDropsEmptyByDefault) {
    auto t = StringUtils::Split("a,b,c", ',');
    ASSERT_EQ(t.size(), 3u);
    EXPECT_EQ(t[0], "a");
    EXPECT_EQ(t[1], "b");
    EXPECT_EQ(t[2], "c");

    // Empty tokens are dropped unless keepEmpty=true.
    auto dropped = StringUtils::Split("a,,b", ',');
    ASSERT_EQ(dropped.size(), 2u);
    EXPECT_EQ(dropped[0], "a");
    EXPECT_EQ(dropped[1], "b");

    auto kept = StringUtils::Split("a,,b", ',', /*keepEmpty=*/true);
    ASSERT_EQ(kept.size(), 3u);
    EXPECT_EQ(kept[1], "");
}

TEST(StringUtils, SplitEmptyInput) {
    EXPECT_TRUE(StringUtils::Split("", ',').empty());
    auto single = StringUtils::Split("solo", ',');
    ASSERT_EQ(single.size(), 1u);
    EXPECT_EQ(single[0], "solo");
}

TEST(StringUtils, JoinIsInverseOfSplitForNonEmptyTokens) {
    std::vector<std::string> parts = {"x", "y", "z"};
    EXPECT_EQ(StringUtils::Join(parts, "-"), "x-y-z");
    EXPECT_EQ(StringUtils::Join({}, "-"), "");
    EXPECT_EQ(StringUtils::Join({"only"}, "/"), "only");

    // Round-trip: split then join recovers the original (no empty tokens).
    std::string original = "alpha.beta.gamma";
    EXPECT_EQ(StringUtils::Join(StringUtils::Split(original, '.'), "."), original);
}

TEST(StringUtils, CaseConversion) {
    EXPECT_EQ(StringUtils::ToLower("HeLLo123"), "hello123");
    EXPECT_EQ(StringUtils::ToUpper("HeLLo123"), "HELLO123");
    EXPECT_TRUE(StringUtils::EqualsIgnoreCase("Server", "SERVER"));
    EXPECT_FALSE(StringUtils::EqualsIgnoreCase("Server", "Client"));
}

TEST(StringUtils, PrefixSuffix) {
    EXPECT_TRUE(StringUtils::StartsWith("rs2v_server", "rs2v"));
    EXPECT_FALSE(StringUtils::StartsWith("rs2v_server", "server"));
    EXPECT_TRUE(StringUtils::EndsWith("packet.cpp", ".cpp"));
    EXPECT_FALSE(StringUtils::EndsWith("packet.cpp", ".h"));
    // Empty prefix/suffix is trivially satisfied.
    EXPECT_TRUE(StringUtils::StartsWith("anything", ""));
    EXPECT_TRUE(StringUtils::EndsWith("anything", ""));
}

TEST(StringUtils, ReplaceAll) {
    EXPECT_EQ(StringUtils::ReplaceAll("a.b.c", ".", "/"), "a/b/c");
    EXPECT_EQ(StringUtils::ReplaceAll("no-match", "x", "y"), "no-match");
    EXPECT_EQ(StringUtils::ReplaceAll("aaa", "a", "aa"), "aaaaaa");
}

TEST(StringUtils, ToBoolRecognizesCommonForms) {
    for (const char* t : {"true", "TRUE", "yes", "1", "on", " On "}) {
        EXPECT_TRUE(StringUtils::ToBool(t)) << "value: " << t;
    }
    for (const char* f : {"false", "no", "0", "off"}) {
        EXPECT_FALSE(StringUtils::ToBool(f)) << "value: " << f;
    }
    // Unrecognized falls back to the provided default.
    EXPECT_TRUE(StringUtils::ToBool("maybe", true));
    EXPECT_FALSE(StringUtils::ToBool("maybe", false));
}

TEST(StringUtils, NumericParsing) {
    auto i = StringUtils::ToInt("42");
    ASSERT_TRUE(i.has_value());
    EXPECT_EQ(*i, 42);
    EXPECT_FALSE(StringUtils::ToInt("notanumber").has_value());

    auto d = StringUtils::ToDouble("3.5");
    ASSERT_TRUE(d.has_value());
    EXPECT_NEAR(*d, 3.5, 1e-9);
    EXPECT_FALSE(StringUtils::ToDouble("").has_value());
}

TEST(StringUtils, ToStringTemplate) {
    EXPECT_EQ(StringUtils::ToString(123), "123");
    EXPECT_EQ(StringUtils::ToString(true), "1");
}

TEST(StringUtils, RemoveWhitespace) {
    EXPECT_EQ(StringUtils::RemoveWhitespace(" a b\tc\n"), "abc");
    EXPECT_EQ(StringUtils::RemoveWhitespace("clean"), "clean");
}

RS2V_TEST_MAIN()
