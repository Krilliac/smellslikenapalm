#include "TestFramework.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "Network/ActorReplication.h"
#include "Network/BitReader.h"
#include "Network/BitWriter.h"
#include "Network/WeaponCombatReplication.h"

namespace {

struct EncodedRpc {
    std::vector<uint8_t> bytes;
    size_t bits = 0;
};

std::vector<uint8_t> Hex(const char* text) {
    auto nibble = [](char c) -> uint8_t {
        if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
        return static_cast<uint8_t>(c - 'A' + 10);
    };
    std::vector<uint8_t> bytes;
    while (*text != '\0' && text[1] != '\0') {
        bytes.push_back(static_cast<uint8_t>(
            (nibble(text[0]) << 4u) | nibble(text[1])));
        text += 2;
    }
    return bytes;
}

void WriteName(BitWriter& writer, bool hardcoded, int32_t number = 0) {
    writer.WriteBit(hardcoded);
    if (hardcoded) {
        writer.SerializeInt(0, WeaponCombatRepl::kMaxNetworkedHardcodedName + 1u);
    } else {
        writer.WriteString("Head");
        writer.WriteInt32(number);
    }
}

void WriteImpactInfo(BitWriter& writer, const ActorRepl::NetGUIDRef& hitActor,
                     bool hardcodedBoneName = true, int32_t boneNumber = 0) {
    ActorRepl::WriteNetGUID(writer, hitActor);
    ActorRepl::WriteCompressedVector(writer, 480.0f, 0.0f, 30.0f);
    ActorRepl::WriteCompressedVector(writer, -1.0f, 0.0f, 0.0f);
    // ROWeapon.EncodeSmallVector multiplies the unit direction by 1024.
    ActorRepl::WriteCompressedVector(writer, 1024.0f, 0.0f, 0.0f);
    ActorRepl::WriteCompressedVector(writer, 0.0f, 0.0f, 30.0f);

    // TraceHitInfo declaration order.
    ActorRepl::WriteNetGUID(writer, {true, 0}); // Material=None
    ActorRepl::WriteNetGUID(writer, {true, 0}); // PhysMaterial=None
    writer.WriteInt32(7);
    writer.WriteInt32(2);
    WriteName(writer, hardcodedBoneName, boneNumber);
    ActorRepl::WriteNetGUID(writer, {true, 0}); // HitComponent=None
    writer.WriteBit(false);                    // ImpactInfo.bExitImpact
}

EncodedRpc MakeHitOne(const ActorRepl::NetGUIDRef& hitActor = {true, 209},
                      bool impactPresent = true,
                      bool hardcodedBoneName = true,
                      int32_t boneNumber = 0) {
    BitWriter writer;
    writer.SerializeInt(WeaponCombatRepl::kServerHandleClientHitsOne,
                        WeaponCombatRepl::kRoWeaponMaxHandle);
    writer.WriteBit(impactPresent);
    if (impactPresent) {
        WriteImpactInfo(writer, hitActor, hardcodedBoneName, boneNumber);
        writer.WriteBit(true); // FiredMode present
        writer.WriteByte(1);
        writer.WriteBit(true); // FirstHitLocation present
        ActorRepl::WriteCompressedVector(writer, 480.0f, 0.0f, 30.0f);
        writer.WriteBit(true); // StartTrace present
        ActorRepl::WriteCompressedVector(writer, 0.0f, 0.0f, 30.0f);
    }
    return {writer.GetBytes(), writer.NumBits()};
}

WeaponCombatRepl::ActorResolver KnownPawnResolver() {
    return [](const ActorRepl::NetGUIDRef& reference) {
        if (reference.isDynamic && reference.index == 209) {
            return WeaponCombatRepl::ActorResolution{
                true, ParticipantId::Human(42)};
        }
        return WeaponCombatRepl::ActorResolution{};
    };
}

} // namespace

TEST(WeaponCombatReplication, DecodesCaptureEstablishedH56Layout) {
    const EncodedRpc encoded = MakeHitOne();
    const auto result = WeaponCombatRepl::DecodeServerHandleClientHitsOne(
        encoded.bytes.data(), encoded.bytes.size(), encoded.bits,
        KnownPawnResolver());

    ASSERT_TRUE(result.valid());
    EXPECT_EQ(result.consumedBits, encoded.bits);
    EXPECT_EQ(result.rpc.firedMode, 1u);
    ASSERT_TRUE(result.rpc.impact.participantId.has_value());
    EXPECT_EQ(*result.rpc.impact.participantId, ParticipantId::Human(42));
    EXPECT_FLOAT_EQ(result.rpc.impact.hitLocation.x, 480.0f);
    EXPECT_FLOAT_EQ(result.rpc.impact.hitLocation.z, 30.0f);
    EXPECT_NEAR(result.rpc.impact.rayDirection.x, 1.0f, 1.0e-6f);
    EXPECT_NEAR(result.rpc.impact.rayDirection.y, 0.0f, 1.0e-6f);
    EXPECT_EQ(result.rpc.impact.hitInfo.item, 7);
    EXPECT_EQ(result.rpc.impact.hitInfo.levelIndex, 2);
    EXPECT_TRUE(result.rpc.impact.hitInfo.boneName.hardcoded);
    EXPECT_FALSE(result.rpc.impact.exitImpact);
    EXPECT_FLOAT_EQ(result.rpc.firstHitLocation.x, 480.0f);
    EXPECT_FLOAT_EQ(result.rpc.startTrace.z, 30.0f);
}

TEST(WeaponCombatReplication, PreservesTaggedParticipantIdentity) {
    const EncodedRpc encoded = MakeHitOne();
    const auto human = WeaponCombatRepl::DecodeServerHandleClientHitsOne(
        encoded.bytes.data(), encoded.bytes.size(), encoded.bits,
        [](const ActorRepl::NetGUIDRef&) {
            return WeaponCombatRepl::ActorResolution{
                true, ParticipantId::Human(7)};
        });
    const auto bot = WeaponCombatRepl::DecodeServerHandleClientHitsOne(
        encoded.bytes.data(), encoded.bytes.size(), encoded.bits,
        [](const ActorRepl::NetGUIDRef&) {
            return WeaponCombatRepl::ActorResolution{
                true, ParticipantId::Bot(7)};
        });

    ASSERT_TRUE(human.valid());
    ASSERT_TRUE(bot.valid());
    ASSERT_TRUE(human.rpc.impact.participantId.has_value());
    ASSERT_TRUE(bot.rpc.impact.participantId.has_value());
    EXPECT_EQ(*human.rpc.impact.participantId, ParticipantId::Human(7));
    EXPECT_EQ(*bot.rpc.impact.participantId, ParticipantId::Bot(7));
    EXPECT_NE(*human.rpc.impact.participantId,
              *bot.rpc.impact.participantId);
}

TEST(WeaponCombatReplication, RejectsUnknownNonNullActorReference) {
    const EncodedRpc encoded = MakeHitOne();
    const auto result = WeaponCombatRepl::DecodeServerHandleClientHitsOne(
        encoded.bytes.data(), encoded.bytes.size(), encoded.bits,
        [](const ActorRepl::NetGUIDRef&) {
            return WeaponCombatRepl::ActorResolution{};
        });
    EXPECT_FALSE(result.valid());
    EXPECT_EQ(result.error,
              WeaponCombatRepl::DecodeError::UnknownActorReference);
}

TEST(WeaponCombatReplication, ExplicitNoneActorNeedsNoResolver) {
    const EncodedRpc encoded = MakeHitOne({true, 0});
    const auto result = WeaponCombatRepl::DecodeServerHandleClientHitsOne(
        encoded.bytes.data(), encoded.bytes.size(), encoded.bits, {});
    ASSERT_TRUE(result.valid());
    EXPECT_FALSE(result.rpc.impact.participantId.has_value());
}

TEST(WeaponCombatReplication, KnownStaticWorldActorMayResolveWithoutParticipant) {
    const EncodedRpc encoded = MakeHitOne({false, 12345});
    const auto result = WeaponCombatRepl::DecodeServerHandleClientHitsOne(
        encoded.bytes.data(), encoded.bytes.size(), encoded.bits,
        [](const ActorRepl::NetGUIDRef& reference) {
            return WeaponCombatRepl::ActorResolution{
                !reference.isDynamic && reference.index == 12345, std::nullopt};
        });
    ASSERT_TRUE(result.valid());
    EXPECT_FALSE(result.rpc.impact.participantId.has_value());
}

TEST(WeaponCombatReplication, RejectsEveryTruncatedPrefixTransactionally) {
    const EncodedRpc encoded = MakeHitOne();
    for (size_t bits = 1; bits < encoded.bits; ++bits) {
        BitReader reader(encoded.bytes.data(), encoded.bytes.size(), bits);
        reader.SetOverflowHandler(
            [](const char*, size_t, size_t, size_t) {});
        WeaponCombatRepl::ServerHandleClientHitsOne output;
        output.firedMode = 99;
        WeaponCombatRepl::DecodeError error = WeaponCombatRepl::DecodeError::None;
        EXPECT_FALSE(WeaponCombatRepl::DecodeOne(
            reader, KnownPawnResolver(), output, error));
        EXPECT_EQ(reader.BitPos(), 0u);
        EXPECT_EQ(output.firedMode, 99u);
    }
}

TEST(WeaponCombatReplication, RejectsUnsupportedHandleTransactionally) {
    BitWriter writer;
    writer.SerializeInt(57, WeaponCombatRepl::kRoWeaponMaxHandle);
    const auto bytes = writer.GetBytes();
    BitReader reader(bytes.data(), bytes.size(), writer.NumBits());
    WeaponCombatRepl::ServerHandleClientHitsOne output;
    WeaponCombatRepl::DecodeError error = WeaponCombatRepl::DecodeError::None;
    EXPECT_FALSE(WeaponCombatRepl::DecodeOne(
        reader, KnownPawnResolver(), output, error));
    EXPECT_EQ(error, WeaponCombatRepl::DecodeError::UnsupportedHandle);
    EXPECT_EQ(reader.BitPos(), 0u);
}

TEST(WeaponCombatReplication, RejectsAbsentRequiredImpactParameter) {
    const EncodedRpc encoded = MakeHitOne({true, 209}, false);
    const auto result = WeaponCombatRepl::DecodeServerHandleClientHitsOne(
        encoded.bytes.data(), encoded.bytes.size(), encoded.bits,
        KnownPawnResolver());
    EXPECT_FALSE(result.valid());
    EXPECT_EQ(result.error,
              WeaponCombatRepl::DecodeError::MissingRequiredParameter);
}

TEST(WeaponCombatReplication, RejectsTrailingUnprovenRpcBits) {
    EncodedRpc encoded = MakeHitOne();
    encoded.bytes.push_back(0); // Ensure storage for one explicit trailing bit.
    ++encoded.bits;
    const auto result = WeaponCombatRepl::DecodeServerHandleClientHitsOne(
        encoded.bytes.data(), encoded.bytes.size(), encoded.bits,
        KnownPawnResolver());
    EXPECT_FALSE(result.valid());
    EXPECT_EQ(result.error, WeaponCombatRepl::DecodeError::TrailingBits);
}

TEST(WeaponCombatReplication, DecodesNonHardcodedBoneName) {
    const EncodedRpc encoded = MakeHitOne({true, 209}, true, false, 3);
    const auto result = WeaponCombatRepl::DecodeServerHandleClientHitsOne(
        encoded.bytes.data(), encoded.bytes.size(), encoded.bits,
        KnownPawnResolver());
    ASSERT_TRUE(result.valid());
    EXPECT_FALSE(result.rpc.impact.hitInfo.boneName.hardcoded);
    EXPECT_EQ(result.rpc.impact.hitInfo.boneName.text, std::string("Head"));
    EXPECT_EQ(result.rpc.impact.hitInfo.boneName.number, 3);
}

TEST(WeaponCombatReplication, RejectsNegativeFNameNumber) {
    const EncodedRpc encoded = MakeHitOne({true, 209}, true, false, -1);
    const auto result = WeaponCombatRepl::DecodeServerHandleClientHitsOne(
        encoded.bytes.data(), encoded.bytes.size(), encoded.bits,
        KnownPawnResolver());
    EXPECT_FALSE(result.valid());
    EXPECT_EQ(result.error, WeaponCombatRepl::DecodeError::InvalidName);
}

TEST(WeaponCombatReplication, RejectsInvalidAndOversizedBuffers) {
    EXPECT_EQ(WeaponCombatRepl::DecodeServerHandleClientHitsOne(
                  nullptr, 0, 0, {}).error,
              WeaponCombatRepl::DecodeError::InvalidBuffer);
    std::vector<uint8_t> oversized(
        (WeaponCombatRepl::kMaxRpcBits + 8u) / 8u, 0);
    EXPECT_EQ(WeaponCombatRepl::DecodeServerHandleClientHitsOne(
                  oversized.data(), oversized.size(),
                  WeaponCombatRepl::kMaxRpcBits + 1u, {}).error,
              WeaponCombatRepl::DecodeError::Oversized);
}

TEST(WeaponCombatReplication, M61OpenWithoutInstigatorMatchesRetailCapture) {
    WeaponCombatRepl::M61VisualSnapshot snapshot;
    snapshot.positionUu = {-2010.0f, 7309.0f, 59.0f};
    snapshot.velocityUuPerSecond = {193.0f, -214.0f, 245.0f};
    snapshot.pitch = 58880;
    snapshot.yaw = 8448;
    snapshot.roll = 58880;
    snapshot.fuseSeconds = 4.961390495300293f;

    const auto encoded = WeaponCombatRepl::EncodeM61Open(3, 7, snapshot);
    ASSERT_TRUE(encoded.valid());
    EXPECT_TRUE(encoded.bunch.bControl);
    EXPECT_TRUE(encoded.bunch.bOpen);
    EXPECT_FALSE(encoded.bunch.bClose);
    EXPECT_TRUE(encoded.bunch.bReliable);
    EXPECT_EQ(encoded.bunch.chType, 2u);
    EXPECT_EQ(encoded.bunch.chIndex, 3u);
    EXPECT_EQ(encoded.bunch.chSequence, 7u);
    EXPECT_EQ(encoded.bunch.payloadBits, 182u);
    EXPECT_EQ(encoded.bunch.payload,
              Hex("aad601006c8235f23b20371fd27c5c7015f5b3edb02710"));
}

TEST(WeaponCombatReplication, M61OpenWithInstigatorMatchesRetailCapture) {
    WeaponCombatRepl::M61VisualSnapshot snapshot;
    snapshot.positionUu = {144.0f, 12110.0f, 138.0f};
    snapshot.velocityUuPerSecond = {-1076.0f, -23.0f, 648.0f};
    snapshot.pitch = 5120;
    snapshot.yaw = 33280;
    snapshot.roll = 60160;
    snapshot.fuseSeconds = 4.968961715698242f;
    snapshot.instigator = ActorRepl::NetGUIDRef{true, 7};

    const auto encoded = WeaponCombatRepl::EncodeM61Open(131, 7, snapshot);
    ASSERT_TRUE(encoded.valid());
    EXPECT_EQ(encoded.bunch.payloadBits, 210u);
    EXPECT_EQ(encoded.bunch.payload,
              Hex("aad601000d09747a2b02c9033845c1eb439927fd1035f3067c0201"));
}

TEST(WeaponCombatReplication, M61MovementUpdateMatchesRetailCapture) {
    WeaponCombatRepl::M61VisualSnapshot snapshot;
    snapshot.positionUu = {-2000.0f, 7299.0f, 70.0f};
    snapshot.velocityUuPerSecond = {193.0f, -214.0f, 193.0f};
    snapshot.pitch = 59136;
    snapshot.yaw = 8192;
    snapshot.roll = 59392;
    snapshot.fuseSeconds = 4.911383628845215f;

    const auto encoded = WeaponCombatRepl::EncodeM61Update(3, snapshot);
    ASSERT_TRUE(encoded.valid());
    EXPECT_FALSE(encoded.bunch.bControl);
    EXPECT_FALSE(encoded.bunch.bOpen);
    EXPECT_FALSE(encoded.bunch.bClose);
    EXPECT_FALSE(encoded.bunch.bReliable);
    // ChType is absent on the wire for non-open unreliable bunches.
    EXPECT_EQ(encoded.bunch.chType, 0u);
    EXPECT_EQ(encoded.bunch.chSequence, 0u);
    EXPECT_EQ(encoded.bunch.payloadBits, 154u);
    EXPECT_EQ(encoded.bunch.payload,
              Hex("cd30d8206f04f2f320d1c70557113c3ba8740201"));
}

TEST(WeaponCombatReplication, M61FuseAndDetonationMatchRetailCapture) {
    const auto fuse =
        WeaponCombatRepl::EncodeM61FuseUpdate(3, 3.7878968715667725f);
    ASSERT_TRUE(fuse.valid());
    EXPECT_EQ(fuse.bunch.payloadBits, 37u);
    EXPECT_EQ(fuse.bunch.payload, Hex("f99c4d0e08"));

    const auto detonation = WeaponCombatRepl::EncodeM61Detonation(
        3, -0.009037848562002182f);
    ASSERT_TRUE(detonation.valid());
    EXPECT_FALSE(detonation.bunch.bReliable);
    EXPECT_EQ(detonation.bunch.payloadBits, 49u);
    EXPECT_EQ(detonation.bunch.payload, Hex("129df926287801"));
}

TEST(WeaponCombatReplication, M61CloseIsReliableEmptyActorClose) {
    const auto close = WeaponCombatRepl::EncodeM61Close(3, 8);
    ASSERT_TRUE(close.valid());
    EXPECT_TRUE(close.bunch.bControl);
    EXPECT_FALSE(close.bunch.bOpen);
    EXPECT_TRUE(close.bunch.bClose);
    EXPECT_TRUE(close.bunch.bReliable);
    EXPECT_EQ(close.bunch.chType, 2u);
    EXPECT_EQ(close.bunch.chIndex, 3u);
    EXPECT_EQ(close.bunch.chSequence, 8u);
    EXPECT_EQ(close.bunch.payloadBits, 0u);
    EXPECT_TRUE(close.bunch.payload.empty());
}

TEST(WeaponCombatReplication, M61LifecycleRejectsOutOfOrderAndDuplicateEvents) {
    WeaponCombatRepl::M61VisualSnapshot snapshot;
    snapshot.positionUu = {10.0f, 20.0f, 30.0f};
    snapshot.velocityUuPerSecond = {100.0f, 0.0f, 25.0f};
    snapshot.fuseSeconds = 5.0f;

    WeaponCombatRepl::M61VisualLifecycle lifecycle(300);
    EXPECT_EQ(lifecycle.State(), WeaponCombatRepl::M61VisualState::Dormant);
    EXPECT_EQ(lifecycle.Update(snapshot).error,
              WeaponCombatRepl::M61VisualError::InvalidState);
    EXPECT_EQ(lifecycle.Detonate(0.0f).error,
              WeaponCombatRepl::M61VisualError::InvalidState);
    EXPECT_EQ(lifecycle.Close(1).error,
              WeaponCombatRepl::M61VisualError::InvalidState);

    ASSERT_TRUE(lifecycle.Spawn(17, snapshot).valid());
    EXPECT_EQ(lifecycle.State(), WeaponCombatRepl::M61VisualState::Active);
    EXPECT_EQ(lifecycle.Spawn(18, snapshot).error,
              WeaponCombatRepl::M61VisualError::InvalidState);
    ASSERT_TRUE(lifecycle.Update(snapshot).valid());
    ASSERT_TRUE(lifecycle.UpdateFuse(4.0f).valid());
    ASSERT_TRUE(lifecycle.Detonate(-0.01f).valid());
    EXPECT_EQ(lifecycle.State(), WeaponCombatRepl::M61VisualState::Detonated);
    EXPECT_EQ(lifecycle.Detonate(-0.02f).error,
              WeaponCombatRepl::M61VisualError::InvalidState);
    EXPECT_EQ(lifecycle.Update(snapshot).error,
              WeaponCombatRepl::M61VisualError::InvalidState);
    ASSERT_TRUE(lifecycle.Close(18).valid());
    EXPECT_EQ(lifecycle.State(), WeaponCombatRepl::M61VisualState::Closed);
    EXPECT_EQ(lifecycle.Close(19).error,
              WeaponCombatRepl::M61VisualError::InvalidState);
}

TEST(WeaponCombatReplication, M61ValidationIsTransactionalAndFailClosed) {
    WeaponCombatRepl::M61VisualSnapshot snapshot;
    snapshot.positionUu = {10.0f, 20.0f, 30.0f};
    snapshot.velocityUuPerSecond = {100.0f, 0.0f, 25.0f};
    snapshot.fuseSeconds = 5.0f;

    EXPECT_EQ(WeaponCombatRepl::EncodeM61Open(1, 1, snapshot).error,
              WeaponCombatRepl::M61VisualError::InvalidChannel);
    EXPECT_EQ(WeaponCombatRepl::EncodeM61Open(
                  3, ActorRepl::kDynamicChannelMax, snapshot).error,
              WeaponCombatRepl::M61VisualError::InvalidSequence);

    WeaponCombatRepl::M61VisualLifecycle lifecycle(301);
    WeaponCombatRepl::M61VisualSnapshot invalid = snapshot;
    invalid.positionUu.x = std::numeric_limits<float>::quiet_NaN();
    EXPECT_EQ(lifecycle.Spawn(1, invalid).error,
              WeaponCombatRepl::M61VisualError::NonFiniteValue);
    EXPECT_EQ(lifecycle.State(), WeaponCombatRepl::M61VisualState::Dormant);

    invalid = snapshot;
    invalid.velocityUuPerSecond.x = 1048576.0f;
    EXPECT_EQ(lifecycle.Spawn(1, invalid).error,
              WeaponCombatRepl::M61VisualError::VectorOutOfRange);
    EXPECT_EQ(lifecycle.State(), WeaponCombatRepl::M61VisualState::Dormant);

    invalid = snapshot;
    invalid.instigator = ActorRepl::NetGUIDRef{false, 0};
    EXPECT_EQ(lifecycle.Spawn(1, invalid).error,
              WeaponCombatRepl::M61VisualError::InvalidInstigator);
    EXPECT_EQ(lifecycle.State(), WeaponCombatRepl::M61VisualState::Dormant);

    ASSERT_TRUE(lifecycle.Spawn(1, snapshot).valid());
    invalid = snapshot;
    invalid.fuseSeconds = WeaponCombatRepl::kM61MaxReplicatedFuseSeconds + 1.0f;
    EXPECT_EQ(lifecycle.Update(invalid).error,
              WeaponCombatRepl::M61VisualError::InvalidFuse);
    EXPECT_EQ(lifecycle.State(), WeaponCombatRepl::M61VisualState::Active);
    EXPECT_EQ(lifecycle.Detonate(0.1f).error,
              WeaponCombatRepl::M61VisualError::InvalidFuse);
    EXPECT_EQ(lifecycle.State(), WeaponCombatRepl::M61VisualState::Active);
    ASSERT_TRUE(lifecycle.Detonate(0.0f).valid());
    EXPECT_EQ(lifecycle.Close(ActorRepl::kDynamicChannelMax).error,
              WeaponCombatRepl::M61VisualError::InvalidSequence);
    EXPECT_EQ(lifecycle.State(), WeaponCombatRepl::M61VisualState::Detonated);
}

TEST(WeaponCombatReplication, M61ChannelPoolAllocatesAndSequencesLifecycle) {
    WeaponCombatRepl::M61VisualSnapshot snapshot;
    snapshot.positionUu = {10.0f, 20.0f, 30.0f};
    snapshot.velocityUuPerSecond = {100.0f, 0.0f, 25.0f};
    snapshot.fuseSeconds = 5.0f;

    WeaponCombatRepl::M61VisualChannelPool pool(768, 769);
    ASSERT_TRUE(pool.IsValid());
    const auto first = pool.Spawn(11, snapshot);
    ASSERT_TRUE(first.valid());
    ASSERT_EQ(first.bunches.size(), static_cast<size_t>(1));
    EXPECT_EQ(first.bunches[0].chIndex, 768u);
    EXPECT_EQ(first.bunches[0].chSequence, 1u);
    EXPECT_EQ(pool.ChannelFor(11), std::optional<uint32_t>(768u));

    const auto second = pool.Spawn(12, snapshot);
    ASSERT_TRUE(second.valid());
    EXPECT_EQ(second.bunches[0].chIndex, 769u);
    EXPECT_EQ(pool.Spawn(11, snapshot).error,
              WeaponCombatRepl::M61VisualError::DuplicateProjectile);
    EXPECT_EQ(pool.Spawn(13, snapshot).error,
              WeaponCombatRepl::M61VisualError::ChannelExhausted);

    const auto update = pool.Update(11, snapshot);
    ASSERT_TRUE(update.valid());
    EXPECT_FALSE(update.bunches[0].bReliable);
    const auto completed = pool.DetonateAndClose(11, 0.0f);
    ASSERT_TRUE(completed.valid());
    ASSERT_EQ(completed.bunches.size(), static_cast<size_t>(2));
    EXPECT_FALSE(completed.bunches[0].bReliable);
    EXPECT_TRUE(completed.bunches[1].bReliable);
    EXPECT_TRUE(completed.bunches[1].bClose);
    EXPECT_EQ(completed.bunches[1].chSequence, 2u);

    // Merely queueing a close cannot release the channel: the packet carrying
    // it can be ACKed while an earlier reliable open remains sequence-buffered.
    EXPECT_EQ(pool.Spawn(13, snapshot).error,
              WeaponCombatRepl::M61VisualError::ChannelExhausted);
    EXPECT_TRUE(pool.IsCloseQueued(768));
    EXPECT_TRUE(pool.AcknowledgeClose(768));

    // Allocation wraps after the acknowledged close and its reliable cursor
    // survives the actor incarnation.
    const auto reused = pool.Spawn(13, snapshot);
    ASSERT_TRUE(reused.valid());
    EXPECT_EQ(reused.bunches[0].chIndex, 768u);
    EXPECT_EQ(reused.bunches[0].chSequence, 3u);
}

TEST(WeaponCombatReplication, M61ChannelPoolFailuresAreTransactional) {
    WeaponCombatRepl::M61VisualSnapshot snapshot;
    snapshot.positionUu = {10.0f, 20.0f, 30.0f};
    snapshot.velocityUuPerSecond = {100.0f, 0.0f, 25.0f};
    snapshot.fuseSeconds = 5.0f;

    WeaponCombatRepl::M61VisualChannelPool invalid(1024, 1024);
    EXPECT_FALSE(invalid.IsValid());
    EXPECT_EQ(invalid.Spawn(1, snapshot).error,
              WeaponCombatRepl::M61VisualError::InvalidChannelRange);

    WeaponCombatRepl::M61VisualChannelPool pool(800, 800);
    WeaponCombatRepl::M61VisualSnapshot bad = snapshot;
    bad.positionUu.x = std::numeric_limits<float>::quiet_NaN();
    EXPECT_EQ(pool.Spawn(1, bad).error,
              WeaponCombatRepl::M61VisualError::NonFiniteValue);
    EXPECT_EQ(pool.ActiveCount(), static_cast<size_t>(0));

    ASSERT_TRUE(pool.Spawn(1, snapshot).valid());
    EXPECT_EQ(pool.Update(2, snapshot).error,
              WeaponCombatRepl::M61VisualError::UnknownProjectile);
    EXPECT_EQ(pool.DetonateAndClose(1, 0.5f).error,
              WeaponCombatRepl::M61VisualError::InvalidFuse);
    EXPECT_EQ(pool.ActiveCount(), static_cast<size_t>(1));
    ASSERT_TRUE(pool.Close(1).valid());
    EXPECT_EQ(pool.ActiveCount(), static_cast<size_t>(1));
    EXPECT_EQ(pool.Update(1, snapshot).error,
              WeaponCombatRepl::M61VisualError::InvalidState);
    EXPECT_TRUE(pool.AcknowledgeClose(800));
    EXPECT_EQ(pool.ActiveCount(), static_cast<size_t>(0));
}

TEST(WeaponCombatReplication, M61ChannelPoolHonorsExternalOccupancyAndSuppression) {
    WeaponCombatRepl::M61VisualSnapshot snapshot;
    snapshot.positionUu = {10.0f, 20.0f, 30.0f};
    snapshot.velocityUuPerSecond = {100.0f, 0.0f, 25.0f};
    snapshot.fuseSeconds = 5.0f;

    WeaponCombatRepl::M61VisualChannelPool pool(900, 901);
    const auto spawned = pool.Spawn(
        7, snapshot, [](uint32_t channel) { return channel != 900u; });
    ASSERT_TRUE(spawned.valid());
    EXPECT_EQ(pool.ChannelFor(7), std::optional<uint32_t>(901u));
    EXPECT_EQ(pool.ProjectileForChannel(901), std::optional<uint64_t>(7u));

    EXPECT_TRUE(pool.SuppressChannel(901));
    EXPECT_TRUE(pool.IsSuppressed(901));
    EXPECT_EQ(pool.UpdateFuse(7, 3.0f).error,
              WeaponCombatRepl::M61VisualError::InvalidState);
    EXPECT_FALSE(pool.AcknowledgeClose(901));
    EXPECT_EQ(pool.Spawn(
                  8, snapshot,
                  [](uint32_t channel) { return channel != 900u; }).error,
              WeaponCombatRepl::M61VisualError::ChannelExhausted);
}

RS2V_TEST_MAIN()
