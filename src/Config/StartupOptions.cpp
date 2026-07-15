#include "Config/StartupOptions.h"

#include <charconv>
#include <limits>
#include <utility>

namespace {
bool IsSafeSingleLineValue(const std::string& text, std::size_t maxLength) {
    if (text.empty() || text.size() > maxLength) return false;
    bool hasNonWhitespace = false;
    for (const unsigned char character : text) {
        if (character < 0x20u || character == 0x7Fu) return false;
        if (character > 0x20u) hasNonWhitespace = true;
    }
    return hasNonWhitespace;
}

bool ParsePort(const std::string& text, std::uint16_t& port) {
    if (text.empty()) return false;
    unsigned int parsed = 0;
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto conversion = std::from_chars(begin, end, parsed, 10);
    if (conversion.ec != std::errc{} || conversion.ptr != end || parsed == 0 ||
        parsed > std::numeric_limits<std::uint16_t>::max()) {
        return false;
    }
    port = static_cast<std::uint16_t>(parsed);
    return true;
}

StartupOptions Invalid(StartupOptions options, std::string error) {
    options.valid = false;
    options.error = std::move(error);
    return options;
}
}  // namespace

StartupOptions ParseStartupOptions(
    const std::vector<std::string>& arguments) {
    StartupOptions options;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const std::string& argument = arguments[index];
        if (argument == "-h" || argument == "--help") {
            options.help = true;
            continue;
        }
        if (argument == "-v" || argument == "--version") {
            options.version = true;
            continue;
        }
        if (argument == "-c" || argument == "--config") {
            if (++index >= arguments.size()) {
                return Invalid(std::move(options),
                               "missing value for " + argument);
            }
            if (!IsSafeSingleLineValue(arguments[index], 32767)) {
                return Invalid(std::move(options),
                               "config file must be a non-empty single-line path");
            }
            options.configFile = arguments[index];
            continue;
        }
        if (argument == "-p" || argument == "--port") {
            if (++index >= arguments.size()) {
                return Invalid(std::move(options),
                               "missing value for " + argument);
            }
            if (!ParsePort(arguments[index], options.port)) {
                return Invalid(
                    std::move(options),
                    "port must be an integer from 1 through 65535: " +
                        arguments[index]);
            }
            continue;
        }
        if (argument == "--eac-port") {
            if (++index >= arguments.size()) {
                return Invalid(std::move(options),
                               "missing value for " + argument);
            }
            if (!ParsePort(arguments[index], options.eacPort)) {
                return Invalid(
                    std::move(options),
                    "EAC port must be an integer from 1 through 65535: " +
                        arguments[index]);
            }
            continue;
        }
        if (argument == "-m" || argument == "--map") {
            if (++index >= arguments.size()) {
                return Invalid(std::move(options),
                               "missing value for " + argument);
            }
            if (!IsSafeSingleLineValue(arguments[index], 128)) {
                return Invalid(std::move(options),
                               "map name must contain 1 through 128 characters");
            }
            options.mapName = arguments[index];
            continue;
        }
        return Invalid(std::move(options), "unknown argument: " + argument);
    }
    return options;
}
