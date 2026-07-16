#pragma once

#include <cstddef>
#include <string_view>

// Wire policy for Engine.GameReplicationInfo.ServerName (h24). BitWriter emits
// std::string values as a positive-length, single-byte UE3 FString, so accepting
// arbitrary UTF-8 or control bytes here would produce ambiguous client text.
namespace ServerNamePolicy {

inline constexpr std::size_t kMaxEncodedBytes = 128u;
inline constexpr std::string_view kRetailFallback =
    "Rising Storm 2: Vietnam Server";

enum class ValidationError {
    None,
    Empty,
    TooLong,
    WhitespaceOnly,
    ControlCharacter,
    NonAscii,
};

inline constexpr ValidationError Validate(std::string_view value) noexcept {
    if (value.empty()) return ValidationError::Empty;
    if (value.size() > kMaxEncodedBytes) return ValidationError::TooLong;

    bool hasNonSpace = false;
    for (const char character : value) {
        const unsigned char byte = static_cast<unsigned char>(character);
        if (byte < 0x20u || byte == 0x7fu) {
            return ValidationError::ControlCharacter;
        }
        if (byte > 0x7eu) return ValidationError::NonAscii;
        hasNonSpace = hasNonSpace || byte != 0x20u;
    }
    return hasNonSpace ? ValidationError::None
                       : ValidationError::WhitespaceOnly;
}

inline constexpr bool IsValid(std::string_view value) noexcept {
    return Validate(value) == ValidationError::None;
}

inline constexpr const char* Describe(ValidationError error) noexcept {
    switch (error) {
        case ValidationError::None:
            return "valid";
        case ValidationError::Empty:
            return "must not be empty";
        case ValidationError::TooLong:
            return "exceeds the encoded-byte wire limit";
        case ValidationError::WhitespaceOnly:
            return "must contain at least one non-space character";
        case ValidationError::ControlCharacter:
            return "contains a control character or embedded NUL";
        case ValidationError::NonAscii:
            return "contains a non-ASCII byte";
    }
    return "has an unknown validation error";
}

} // namespace ServerNamePolicy
