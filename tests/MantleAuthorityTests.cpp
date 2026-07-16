#include "TestFramework.h"

#include <limits>

#include "Game/MantleAuthority.h"

namespace {

using MantleAuthority::Authority;
using MantleAuthority::Decision;
using MantleAuthority::RejectReason;
using MantleAuthority::Request;

Request ValidRequest(double timeSeconds = 10.0,
                     bool wantsToClimb = false) {
    Request request;
    request.playerId = 7;
    request.gameplayActive = true;
    request.playerAlive = true;
    request.wantsToClimb = wantsToClimb;
    request.serverTimeSeconds = timeSeconds;
    request.authoritativePosition = {-6725.0f, 8165.0f, -409.0f};
    request.clientTracePosition = {-6725.0f, 8165.0f, -409.0f};
    request.hitLocation = {-6667.0f, 8171.0f, -405.0f};
    request.hitNormal = {-1.0f, -1.0f, 0.0f};
    request.worldBounds = {
        {-10000.0f, -10000.0f, -1000.0f},
        {10000.0f, 20000.0f, 5000.0f}};
    return request;
}

TEST(MantleAuthority, AcceptsCapturedVaultGeometryFromAuthorityPosition) {
    Authority authority;
    const Request request = ValidRequest();
    const Decision decision = authority.TryAccept(request);

    ASSERT_TRUE(decision.Accepted());
    EXPECT_EQ(decision.reason, RejectReason::None);
    EXPECT_EQ(decision.traversal.specialMove, 1u);
    EXPECT_FALSE(decision.traversal.mantleCrouched);
    EXPECT_EQ(decision.traversal.startLocation,
              request.authoritativePosition);
    EXPECT_FLOAT_EQ(decision.traversal.height, 48.0f);
    EXPECT_FLOAT_EQ(decision.traversal.endLocation.z, -361.0f);
    EXPECT_NEAR(decision.traversal.wallNormal.Length(), 100.0f, 0.001f);
}

TEST(MantleAuthority, ClimbUsesServerOwnedMoveAndHeight) {
    Authority authority;
    const Decision decision = authority.TryAccept(ValidRequest(10.0, true));

    ASSERT_TRUE(decision.Accepted());
    EXPECT_EQ(decision.traversal.specialMove, 2u);
    EXPECT_TRUE(decision.traversal.mantleCrouched);
    EXPECT_FLOAT_EQ(decision.traversal.height, 72.5f);
    EXPECT_FLOAT_EQ(decision.traversal.endLocation.z, -336.5f);
}

TEST(MantleAuthority, RejectsUnavailablePlayerOrInactiveGameplay) {
    Authority authority;
    Request request = ValidRequest();
    request.playerAlive = false;
    EXPECT_EQ(authority.TryAccept(request).reason,
              RejectReason::PlayerUnavailable);

    request = ValidRequest();
    request.gameplayActive = false;
    EXPECT_EQ(authority.TryAccept(request).reason,
              RejectReason::GameplayInactive);
}

TEST(MantleAuthority, RejectsNonFiniteInputWithoutConsumingCooldown) {
    Authority authority;
    Request malformed = ValidRequest();
    malformed.hitLocation.x = std::numeric_limits<float>::infinity();
    EXPECT_EQ(authority.TryAccept(malformed).reason,
              RejectReason::InvalidInput);

    // Rejection must not mutate the accepted-at clock.
    EXPECT_TRUE(authority.TryAccept(ValidRequest()).Accepted());
}

TEST(MantleAuthority, RejectsInvalidOrEscapingWorldBounds) {
    Authority authority;
    Request request = ValidRequest();
    request.worldBounds = {};
    EXPECT_EQ(authority.TryAccept(request).reason,
              RejectReason::InvalidWorldBounds);

    request = ValidRequest();
    request.worldBounds.max.x = -6700.0f;
    EXPECT_EQ(authority.TryAccept(request).reason,
              RejectReason::OutsideWorld);

    // The hit is in bounds but the server-computed clearance destination is not.
    request = ValidRequest();
    request.authoritativePosition = {0.0f, 0.0f, 0.0f};
    request.clientTracePosition = {0.0f, 0.0f, 0.0f};
    request.hitLocation = {10.0f, 0.0f, 0.0f};
    request.hitNormal = {-1.0f, 0.0f, 0.0f};
    request.worldBounds = {{-100.0f, -100.0f, -100.0f},
                           {50.0f, 100.0f, 100.0f}};
    EXPECT_EQ(authority.TryAccept(request).reason,
              RejectReason::InvalidDestination);
}

TEST(MantleAuthority, RejectsStaleTraceAndImplausibleSurface) {
    Authority authority;
    Request request = ValidRequest();
    request.authoritativePosition.x -= 401.0f;
    EXPECT_EQ(authority.TryAccept(request).reason,
              RejectReason::AuthorityMismatch);

    request = ValidRequest();
    request.hitNormal = {1.0f, 1.0f, 0.0f};
    EXPECT_EQ(authority.TryAccept(request).reason,
              RejectReason::SurfaceFacesAway);

    request = ValidRequest();
    request.hitNormal = {0.0f, 0.0f, 1.0f};
    EXPECT_EQ(authority.TryAccept(request).reason,
              RejectReason::InvalidSurfaceNormal);

    request = ValidRequest();
    request.hitLocation.z = request.clientTracePosition.z + 161.0f;
    EXPECT_EQ(authority.TryAccept(request).reason,
              RejectReason::InvalidHitHeight);
}

TEST(MantleAuthority, EnforcesAcceptedAttemptCooldownPerPlayer) {
    Authority authority;
    ASSERT_TRUE(authority.TryAccept(ValidRequest(10.0)).Accepted());
    EXPECT_EQ(authority.TryAccept(ValidRequest(10.2)).reason,
              RejectReason::Cooldown);
    EXPECT_TRUE(authority.TryAccept(ValidRequest(10.36)).Accepted());

    Request other = ValidRequest(10.0);
    other.playerId = 8;
    EXPECT_TRUE(authority.TryAccept(other).Accepted());
}

TEST(MantleAuthority, ClockRegressionRejectsAndClearsStaleCooldown) {
    Authority authority;
    ASSERT_TRUE(authority.TryAccept(ValidRequest(10.0)).Accepted());
    EXPECT_EQ(authority.TryAccept(ValidRequest(9.0)).reason,
              RejectReason::InvalidClock);
    EXPECT_TRUE(authority.TryAccept(ValidRequest(9.0)).Accepted());
}

TEST(MantleAuthority, ForgetPlayerClearsLifecycleCooldown) {
    Authority authority;
    ASSERT_TRUE(authority.TryAccept(ValidRequest()).Accepted());
    authority.ForgetPlayer(7);
    EXPECT_TRUE(authority.TryAccept(ValidRequest()).Accepted());
}

} // namespace

RS2V_TEST_MAIN()
