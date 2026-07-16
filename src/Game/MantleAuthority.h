// Server-owned admission gate for retail mantle/vault requests.
//
// The wire decoder only supplies an untrusted LastGoodMantleInfo observation.
// This class validates that observation against authoritative player/world state
// and owns the per-player cooldown. It deliberately has no networking concerns.

#pragma once

#include <cstdint>
#include <mutex>
#include <unordered_map>

#include "Game/Bounds.h"
#include "Math/Vector3.h"

namespace MantleAuthority {

enum class RejectReason : uint8_t {
    None,
    InvalidConfiguration,
    GameplayInactive,
    PlayerUnavailable,
    InvalidClock,
    InvalidInput,
    InvalidWorldBounds,
    OutsideWorld,
    AuthorityMismatch,
    InvalidHitRange,
    InvalidHitHeight,
    InvalidSurfaceNormal,
    SurfaceFacesAway,
    InvalidDestination,
    Cooldown,
};

struct Config {
    float absoluteWorldLimitUu = 100000.0f;
    float maximumAuthorityErrorUu = 400.0f;
    float minimumHitDistanceUu = 5.0f;
    float maximumHitDistanceUu = 250.0f;
    float minimumHitHeightUu = -40.0f;
    float maximumHitHeightUu = 160.0f;
    float minimumHorizontalNormalLength = 0.25f;
    float maximumHorizontalNormalLength = 1000.0f;
    float maximumSurfaceFacingDot = -0.25f;
    float maximumTraversalDistanceUu = 350.0f;
    float wallClearanceUu = 45.0f;
    float vaultHeightUu = 48.0f;
    float climbHeightUu = 72.5f;
    double cooldownSeconds = 0.35;
};

struct Request {
    uint32_t playerId = 0;
    bool gameplayActive = false;
    bool playerAlive = false;
    bool wantsToClimb = false;
    double serverTimeSeconds = 0.0;
    Vector3 authoritativePosition{};
    Vector3 clientTracePosition{};
    Vector3 hitLocation{};
    Vector3 hitNormal{};
    Bounds worldBounds{};
};

struct Traversal {
    uint8_t specialMove = 0;
    bool mantleCrouched = false;
    Vector3 startLocation{};
    Vector3 endLocation{};
    Vector3 wallNormal{};
    float height = 0.0f;
};

struct Decision {
    RejectReason reason = RejectReason::InvalidInput;
    Traversal traversal{};

    [[nodiscard]] bool Accepted() const noexcept {
        return reason == RejectReason::None;
    }
};

class Authority {
public:
    explicit Authority(const Config& config = Config{});

    [[nodiscard]] bool IsConfigurationValid() const noexcept;
    [[nodiscard]] Decision TryAccept(const Request& request);
    void ForgetPlayer(uint32_t playerId);
    void Clear();

private:
    Config m_config;
    std::mutex m_mutex;
    std::unordered_map<uint32_t, double> m_lastAcceptedSeconds;
};

} // namespace MantleAuthority
