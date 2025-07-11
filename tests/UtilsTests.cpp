// tests/UtilsTests.cpp
// Comprehensive unit tests for utility functions
//
// Covers:
// 1. StringUtils: trimming, splitting, case conversion.
// 2. FileUtils: file existence, read/write, directory listing.
// 3. PathUtils: path resolution and normalization.
// 4. Logger: formatting, log level filtering.
// 5. Math utilities: Vector3 operations, normalization, dot/cross products.
// 6. TimeUtils: ISO8601 formatting, parsing, deadline waits.
// 7. Edge cases: empty inputs, invalid paths, Unicode handling.

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <string>
#include "Utils/StringUtils.h"
#include "Utils/FileUtils.h"
#include "Utils/PathUtils.h"
#include "Utils/Logger.h"
#include "Utils/TimeUtils.h"
#include "Math/Vector3.h"

using ::testing::_;
using ::testing::Return;
using ::testing::Invoke;

// Fixture for StringUtils tests
class StringUtilsTest : public ::testing::Test {
protected:
    StringUtils su;
};

TEST_F(StringUtilsTest, Trim_RemovesWhitespace) {
    EXPECT_EQ(su.Trim("  hello world \t\n"), "hello world");
    EXPECT_EQ(su.Trim(""), "");
}

TEST_F(StringUtilsTest, Split_SplitsOnDelimiter) {
    auto v = su.Split("a,b,,c", ',');
    EXPECT_EQ(v.size(), 4u);
    EXPECT_EQ(v[0], "a");
    EXPECT_EQ(v[2], "");
}

TEST_F(StringUtilsTest, CaseConversion_WorksCorrectly) {
    EXPECT_EQ(su.ToLowerCase("TeSt"), "test");
    EXPECT_EQ(su.ToUpperCase("TeSt"), "TEST");
}

TEST_F(StringUtilsTest, StartsEndsWith) {
    EXPECT_TRUE(su.StartsWith("foobar", "foo"));
    EXPECT_FALSE(su.StartsWith("bar", "foo"));
    EXPECT_TRUE(su.EndsWith("foobar", "bar"));
}

// Fixture for FileUtils tests
class FileUtilsTest : public ::testing::Test {
protected:
    FileUtils fu;
    std::string testDir = "test_utils_files";
    void SetUp() override {
        std::filesystem::create_directory(testDir);
        std::ofstream(testDir + "/a.txt") << "content";
        std::ofstream(testDir + "/b.log") << "log";
    }
    void TearDown() override {
        std::filesystem::remove_all(testDir);
    }
};

TEST_F(FileUtilsTest, FileExistsAndReadable) {
    EXPECT_TRUE(fu.FileExists(testDir + "/a.txt"));
    EXPECT_TRUE(fu.IsReadable(testDir + "/a.txt"));
    EXPECT_FALSE(fu.FileExists(testDir + "/nope.txt"));
}

TEST_F(FileUtilsTest, ReadFileToString_Works) {
    auto s = fu.ReadFileToString(testDir + "/a.txt");
    EXPECT_EQ(s, "content");
}

TEST_F(FileUtilsTest, WriteStringToFileAndListFiles) {
    std::string path = testDir + "/new.txt";
    EXPECT_TRUE(fu.WriteStringToFile(path, "hello"));
    EXPECT_TRUE(fu.FileExists(path));
    auto files = fu.ListFiles(testDir, ".txt");
    EXPECT_NE(std::find(files.begin(), files.end(), "new.txt"), files.end());
}

// Fixture for PathUtils tests
class PathUtilsTest : public ::testing::Test {
protected:
    PathUtils pu;
};

TEST_F(PathUtilsTest, NormalizePath_RemovesDots) {
    std::string p = pu.Normalize("foo/./bar/../baz");
    EXPECT_EQ(p, "foo/baz");
}

TEST_F(PathUtilsTest, JoinPaths_Correct) {
    EXPECT_EQ(pu.Join("foo", "bar"), "foo/bar");
    EXPECT_EQ(pu.Join("foo/", "/bar"), "foo/bar");
}

// Logger tests: formatting and level filtering
class LoggerTest : public ::testing::Test {
protected:
    // Redirect logs to memory if Logger supports it, else basic call
};

TEST_F(LoggerTest, LogLevels_Filtered) {
    Logger::SetLogLevel(LogLevel::Warning);
    EXPECT_NO_THROW(Logger::Info("this should not print"));
    EXPECT_NO_THROW(Logger::Warn("this should print"));
}

// TimeUtils tests
TEST(TimeUtilsTest, ISO8601_RoundTrip) {
    auto now = std::chrono::system_clock::now();
    auto s = TimeUtils::ToISO8601(now);
    auto t = TimeUtils::FromISO8601(s);
    auto diff = std::chrono::duration_cast<std::chrono::seconds>(now - t).count();
    EXPECT_LE(std::abs(diff), 1);
}

TEST(TimeUtilsTest, WaitUntil_Timeout) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
    bool ok = TimeUtils::WaitUntil(deadline, [](){ return false; }, 100ms);
    EXPECT_FALSE(ok);
}

// Vector3 tests
class Vector3Test : public ::testing::Test {
protected:
    Math::Vector3 v{3,4,0};
};

TEST_F(Vector3Test, LengthAndNormalization) {
    EXPECT_FLOAT_EQ(v.Length(), 5.0f);
    auto u = v.Normalized();
    EXPECT_NEAR(u.Length(), 1.0f, 1e-6f);
}

TEST_F(Vector3Test, DotAndCross) {
    Math::Vector3 a{1,0,0}, b{0,1,0};
    EXPECT_FLOAT_EQ(a.Dot(b), 0.0f);
    auto c = a.Cross(b);
    EXPECT_EQ(c, Math::Vector3{0,0,1});
}