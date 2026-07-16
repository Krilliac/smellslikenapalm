#include "TestFramework.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include "Network/BitReader.h"
#include "Network/BitWriter.h"
#include "Network/ActorReplication.h"
#include "Network/DeploymentReplication.h"

namespace {

struct EncodedRpc {
    std::vector<uint8_t> bytes;
    size_t bits = 0;
};

std::vector<uint8_t> Hex(const char* text) {
    std::vector<uint8_t> bytes;
    for (size_t i = 0; text[i] != '\0'; i += 2) {
        auto nibble = [](char value) -> uint8_t {
            if (value >= '0' && value <= '9') return value - '0';
            if (value >= 'a' && value <= 'f') return value - 'a' + 10;
            if (value >= 'A' && value <= 'F') return value - 'A' + 10;
            return 0;
        };
        bytes.push_back(static_cast<uint8_t>(
            (nibble(text[i]) << 4u) | nibble(text[i + 1])));
    }
    return bytes;
}

void FlipBit(std::vector<uint8_t>& bytes, size_t bit) {
    bytes[bit / 8u] ^= static_cast<uint8_t>(1u << (bit % 8u));
}

EncodedRpc EncodeSpawnSelect(bool present, uint8_t selection = 0) {
    BitWriter writer;
    writer.SerializeInt(DeploymentRepl::kServerSetSpawnSelectHandle,
                        DeploymentRepl::kRoPlayerControllerMaxHandle);
    writer.WriteBit(present);
    if (present) writer.WriteByte(selection);
    return {writer.GetBytes(), writer.NumBits()};
}

EncodedRpc EncodeReady(bool present, uint8_t status = 0) {
    BitWriter writer;
    writer.SerializeInt(DeploymentRepl::kServerSetReadyToSpawnHandle,
                        DeploymentRepl::kRoPlayerControllerMaxHandle);
    writer.WriteBit(present);
    if (present) {
        writer.WriteBits(status, DeploymentRepl::kReadyStatusBits);
    }
    return {writer.GetBytes(), writer.NumBits()};
}

void AppendVivoxStateChange(BitWriter& writer, uint32_t otherPriChannel,
                            uint32_t localPcChannel = 2) {
    writer.SerializeInt(DeploymentRepl::kChangeVivoxChannelsStateHandle,
                        DeploymentRepl::kRoPlayerControllerMaxHandle);
    writer.WriteBit(true);
    ActorRepl::WriteNetGUID(
        writer, ActorRepl::NetGUIDRef{/*isDynamic=*/true, otherPriChannel});
    writer.WriteBit(true);
    ActorRepl::WriteNetGUID(
        writer, ActorRepl::NetGUIDRef{/*isDynamic=*/true, localPcChannel});
}

} // namespace

TEST(DeploymentReplication, DecodesExactVivoxStateChangeBatches) {
    BitWriter single;
    AppendVivoxStateChange(single, 512u);
    ASSERT_EQ(single.NumBits(),
              DeploymentRepl::kChangeVivoxChannelsStateRecordBits);
    const std::vector<uint8_t> singleBytes = single.GetBytes();

    const auto decodedSingle =
        DeploymentRepl::DecodeChangeVivoxChannelsStateBunch(
            singleBytes.data(), singleBytes.size(),
            single.NumBits());
    ASSERT_TRUE(decodedSingle.valid());
    ASSERT_EQ(decodedSingle.bunch.recordCount, 1u);
    EXPECT_EQ(decodedSingle.bunch.records[0].otherPlayerPriChannel, 512u);
    EXPECT_EQ(decodedSingle.bunch.records[0].localPlayerControllerChannel, 2u);

    BitWriter batch;
    for (uint32_t i = 0; i < 13u; ++i) {
        AppendVivoxStateChange(batch, 512u + i * 2u);
    }
    ASSERT_EQ(batch.NumBits(), 13u *
              DeploymentRepl::kChangeVivoxChannelsStateRecordBits);
    const std::vector<uint8_t> batchBytes = batch.GetBytes();
    const auto decodedBatch =
        DeploymentRepl::DecodeChangeVivoxChannelsStateBunch(
            batchBytes.data(), batchBytes.size(), batch.NumBits());
    ASSERT_TRUE(decodedBatch.valid());
    ASSERT_EQ(decodedBatch.bunch.recordCount, 13u);
    EXPECT_EQ(decodedBatch.bunch.records.front().otherPlayerPriChannel,
              512u);
    EXPECT_EQ(decodedBatch.bunch.records[12].otherPlayerPriChannel, 536u);
}

TEST(DeploymentReplication, VivoxStateChangeDecoderIsExactAndTransactional) {
    BitWriter writer;
    AppendVivoxStateChange(writer, 512u);
    const std::vector<uint8_t> bytes = writer.GetBytes();
    for (size_t bits = 1; bits < writer.NumBits(); ++bits) {
        EXPECT_EQ(DeploymentRepl::DecodeChangeVivoxChannelsStateBunch(
                      bytes.data(), bytes.size(), bits).error,
                  DeploymentRepl::DecodeError::Truncated) << bits;
    }

    std::vector<uint8_t> padded = bytes;
    padded.push_back(0u);
    EXPECT_EQ(DeploymentRepl::DecodeChangeVivoxChannelsStateBunch(
                  padded.data(), padded.size(), writer.NumBits() + 1u).error,
              DeploymentRepl::DecodeError::TrailingBits);

    std::vector<uint8_t> wrongHandle = bytes;
    FlipBit(wrongHandle, 0u);
    EXPECT_EQ(DeploymentRepl::DecodeChangeVivoxChannelsStateBunch(
                  wrongHandle.data(), wrongHandle.size(), writer.NumBits()).error,
              DeploymentRepl::DecodeError::UnsupportedHandle);

    // h152 occupies nine bits; bit 10 is OtherPlayerROPRI's dynamic selector.
    std::vector<uint8_t> staticOtherPri = bytes;
    FlipBit(staticOtherPri, 10u);
    EXPECT_FALSE(DeploymentRepl::DecodeChangeVivoxChannelsStateBunch(
        staticOtherPri.data(), staticOtherPri.size(), writer.NumBits()).valid());

    // Presence bits are mandatory for both object parameters.
    std::vector<uint8_t> omittedOtherPri = bytes;
    FlipBit(omittedOtherPri, 9u);
    EXPECT_EQ(DeploymentRepl::DecodeChangeVivoxChannelsStateBunch(
                  omittedOtherPri.data(), omittedOtherPri.size(),
                  writer.NumBits()).error,
              DeploymentRepl::DecodeError::UnsupportedSequence);
}

TEST(DeploymentReplication, VivoxStateChangeBatchHasHardRecordLimit) {
    BitWriter maximum;
    for (size_t i = 0;
         i < DeploymentRepl::kMaximumVivoxChannelStateRecords; ++i) {
        AppendVivoxStateChange(maximum, 26u);
    }
    const std::vector<uint8_t> maximumBytes = maximum.GetBytes();
    const auto accepted =
        DeploymentRepl::DecodeChangeVivoxChannelsStateBunch(
            maximumBytes.data(), maximumBytes.size(), maximum.NumBits());
    ASSERT_TRUE(accepted.valid());
    EXPECT_EQ(accepted.bunch.recordCount,
              DeploymentRepl::kMaximumVivoxChannelStateRecords);

    AppendVivoxStateChange(maximum, 26u);
    const std::vector<uint8_t> overflowBytes = maximum.GetBytes();
    EXPECT_EQ(DeploymentRepl::DecodeChangeVivoxChannelsStateBunch(
                  overflowBytes.data(), overflowBytes.size(),
                  maximum.NumBits()).error,
              DeploymentRepl::DecodeError::UnsupportedSequence);
}

TEST(DeploymentReplication, DecodesDefaultOmittedReadyStatus) {
    const EncodedRpc encoded = EncodeReady(false);
    const auto result = DeploymentRepl::DecodeServerSetReadyToSpawn(
        encoded.bytes.data(), encoded.bytes.size(), encoded.bits);

    ASSERT_TRUE(result.valid());
    EXPECT_FALSE(result.rpc.presentOnWire);
    EXPECT_EQ(result.rpc.status, 0u);
    EXPECT_EQ(result.rpc.consumedBits, encoded.bits);
}

TEST(DeploymentReplication, DecodesEveryValidTwoBitReadyStatus) {
    for (uint8_t status = 0; status <= 2; ++status) {
        const EncodedRpc encoded = EncodeReady(true, status);
        const auto result = DeploymentRepl::DecodeServerSetReadyToSpawn(
            encoded.bytes.data(), encoded.bytes.size(), encoded.bits);

        ASSERT_TRUE(result.valid());
        EXPECT_TRUE(result.rpc.presentOnWire);
        EXPECT_EQ(result.rpc.status, status);
        EXPECT_EQ(result.rpc.consumedBits, encoded.bits);
    }
}

TEST(DeploymentReplication, DecodesExactRetailReadyForceOnlyStopVoiceBunch) {
    const std::vector<uint8_t> payload = Hex("b2d192bd8900");
    const auto decoded = DeploymentRepl::DecodeServerSetReadyToSpawnBunch(
        payload.data(), payload.size(), 41u);

    ASSERT_TRUE(decoded.valid());
    EXPECT_EQ(decoded.bunch.pattern, DeploymentRepl::ReadyToSpawnBunchPattern::
                                         ReadyForceOnlyStopVoiceChat);
    ASSERT_EQ(decoded.bunch.transitionCount, 2u);
    EXPECT_EQ(decoded.bunch.transitions[0].status, 0u);
    EXPECT_FALSE(decoded.bunch.transitions[0].presentOnWire);
    EXPECT_EQ(decoded.bunch.transitions[1].status, 1u);
    EXPECT_TRUE(decoded.bunch.transitions[1].presentOnWire);
    EXPECT_FALSE(decoded.bunch.acknowledgedPawnChannel.has_value());
    EXPECT_FALSE(decoded.bunch.spectatorLocation.has_value());
}

TEST(DeploymentReplication, DecodesExactRetailReadyPossessionBunch) {
    const std::vector<uint8_t> payload = Hex("b2d1923d1647c311");
    const auto decoded = DeploymentRepl::DecodeServerSetReadyToSpawnBunch(
        payload.data(), payload.size(), 62u);

    ASSERT_TRUE(decoded.valid());
    EXPECT_EQ(decoded.bunch.pattern, DeploymentRepl::ReadyToSpawnBunchPattern::
                                         ReadyForceOnlyAcknowledgePossession);
    ASSERT_EQ(decoded.bunch.transitionCount, 2u);
    EXPECT_EQ(decoded.bunch.transitions[0].status, 0u);
    EXPECT_EQ(decoded.bunch.transitions[1].status, 1u);
    EXPECT_EQ(decoded.bunch.acknowledgedPawnChannel,
              std::optional<uint32_t>(209u));
}

TEST(DeploymentReplication,
     DecodesExactCompoundSpawnVolumeForceOnlyBunch) {
    // Live installed VNSK-Compound reliable ch2 seq8:
    // h370(static camera 289102) + h434 ForceOnly + h275(false).
    const std::vector<uint8_t> payload = Hex("72734a2300c8de44");
    const auto decoded = DeploymentRepl::DecodeSpawnVolumeViewTargetBunch(
        payload.data(), payload.size(), 64u);

    ASSERT_TRUE(decoded.valid());
    EXPECT_EQ(decoded.bunch.pattern, DeploymentRepl::
        SpawnVolumeViewTargetBunchPattern::ForceOnlyStopVoiceChat);
    EXPECT_EQ(decoded.bunch.cameraTargetRef,
              DeploymentRepl::kInstalledCompoundSpawnVolumeCameraRef);
    ASSERT_EQ(decoded.bunch.actionCount, 1u);
    EXPECT_EQ(decoded.bunch.actions[0].type, DeploymentRepl::
        SpawnVolumeDeploymentActionType::ReadyToSpawn);
    EXPECT_TRUE(decoded.bunch.actions[0].readyToSpawn.presentOnWire);
    EXPECT_EQ(decoded.bunch.actions[0].readyToSpawn.status, 1u);
}

TEST(DeploymentReplication,
     DecodesExactCompoundSpawnVolumeSkirmishAutoSelectBunch) {
    // Immediately following live reliable ch2 seq9. ResetSpawnSelection and
    // PostFirstRender each contribute h261(128)+h434 Ready(default)+h180.
    const std::vector<uint8_t> payload =
        Hex("72734a2300140c281bad820165a305");
    const auto decoded = DeploymentRepl::DecodeSpawnVolumeViewTargetBunch(
        payload.data(), payload.size(), 116u);

    ASSERT_TRUE(decoded.valid());
    EXPECT_EQ(decoded.bunch.pattern, DeploymentRepl::
        SpawnVolumeViewTargetBunchPattern::SkirmishAutoSelect);
    EXPECT_EQ(decoded.bunch.cameraTargetRef,
              DeploymentRepl::kInstalledCompoundSpawnVolumeCameraRef);
    ASSERT_EQ(decoded.bunch.actionCount, 4u);
    for (uint8_t repetition = 0; repetition < 2u; ++repetition) {
        const auto& spawn = decoded.bunch.actions[repetition * 2u];
        const auto& ready = decoded.bunch.actions[repetition * 2u + 1u];
        EXPECT_EQ(spawn.type, DeploymentRepl::
            SpawnVolumeDeploymentActionType::SpawnSelect);
        EXPECT_TRUE(spawn.spawnSelect.presentOnWire);
        EXPECT_EQ(spawn.spawnSelect.encodedSelection, 128u);
        EXPECT_EQ(ready.type, DeploymentRepl::
            SpawnVolumeDeploymentActionType::ReadyToSpawn);
        EXPECT_FALSE(ready.readyToSpawn.presentOnWire);
        EXPECT_EQ(ready.readyToSpawn.status, 0u);
    }
}

TEST(DeploymentReplication,
     DecodesExactCompoundSpawnVolumeAutoSelectWithSpectatorReset) {
    // Fresh cold-client reliable ch2 seq9: the 116-bit auto-select transaction
    // followed by h89 ServerSetSpectatorLocation(default/absent Vector).
    const std::vector<uint8_t> payload =
        Hex("72734a2300140c281bad820165a39505");
    const auto decoded = DeploymentRepl::DecodeSpawnVolumeViewTargetBunch(
        payload.data(), payload.size(), 126u);

    ASSERT_TRUE(decoded.valid());
    EXPECT_EQ(decoded.bunch.pattern, DeploymentRepl::
        SpawnVolumeViewTargetBunchPattern::
            SkirmishAutoSelectClearSpectatorLocation);
    EXPECT_EQ(decoded.bunch.cameraTargetRef,
              DeploymentRepl::kInstalledCompoundSpawnVolumeCameraRef);
    ASSERT_EQ(decoded.bunch.actionCount, 4u);
    EXPECT_EQ(decoded.bunch.actions[0].spawnSelect.encodedSelection, 128u);
    EXPECT_EQ(decoded.bunch.actions[2].spawnSelect.encodedSelection, 128u);
}

TEST(DeploymentReplication,
     CompoundSpawnVolumeAcceptsMatchingNonzeroNormalSlotOnly) {
    std::vector<uint8_t> slotFive =
        Hex("72734a2300140c281bad820165a305");
    // h261 byte fields begin at bits 52 and 89. Change both 128 values to 133.
    for (const size_t bit : {52u, 54u, 89u, 91u}) {
        FlipBit(slotFive, bit);
    }
    const auto accepted = DeploymentRepl::DecodeSpawnVolumeViewTargetBunch(
        slotFive.data(), slotFive.size(), 116u);
    ASSERT_TRUE(accepted.valid());
    ASSERT_EQ(accepted.bunch.actionCount, 4u);
    EXPECT_EQ(accepted.bunch.actions[0].spawnSelect.encodedSelection, 133u);
    EXPECT_EQ(accepted.bunch.actions[2].spawnSelect.encodedSelection, 133u);

    // ResetSpawnSelection and PostFirstRender must name the same normal slot.
    std::vector<uint8_t> mismatched = slotFive;
    FlipBit(mismatched, 89u);
    EXPECT_EQ(DeploymentRepl::DecodeSpawnVolumeViewTargetBunch(
                  mismatched.data(), mismatched.size(), 116u).error,
              DeploymentRepl::DecodeError::UnsupportedSequence);
}

TEST(DeploymentReplication,
     CompoundSpawnVolumeBunchesRejectTruncationPaddingAndDrift) {
    const std::vector<uint8_t> forceOnly = Hex("72734a2300c8de44");
    for (size_t bits = 1; bits < 64u; ++bits) {
        EXPECT_FALSE(DeploymentRepl::DecodeSpawnVolumeViewTargetBunch(
            forceOnly.data(), forceOnly.size(), bits).valid()) << bits;
    }
    std::vector<uint8_t> paddedForceOnly = forceOnly;
    paddedForceOnly.push_back(0u);
    EXPECT_EQ(DeploymentRepl::DecodeSpawnVolumeViewTargetBunch(
                  paddedForceOnly.data(), paddedForceOnly.size(), 65u).error,
              DeploymentRepl::DecodeError::TrailingBits);

    const std::vector<uint8_t> autoSelect =
        Hex("72734a2300140c281bad820165a305");
    for (size_t bits = 1; bits < 116u; ++bits) {
        EXPECT_FALSE(DeploymentRepl::DecodeSpawnVolumeViewTargetBunch(
            autoSelect.data(), autoSelect.size(), bits).valid()) << bits;
    }
    EXPECT_EQ(DeploymentRepl::DecodeSpawnVolumeViewTargetBunch(
                  autoSelect.data(), autoSelect.size(), 117u).error,
              DeploymentRepl::DecodeError::TrailingBits);

    const std::vector<uint8_t> autoSelectWithSpectatorReset =
        Hex("72734a2300140c281bad820165a39505");
    for (size_t bits = 117u; bits < 126u; ++bits) {
        EXPECT_FALSE(DeploymentRepl::DecodeSpawnVolumeViewTargetBunch(
            autoSelectWithSpectatorReset.data(),
            autoSelectWithSpectatorReset.size(), bits).valid()) << bits;
    }
    std::vector<uint8_t> wrongSpectatorResetHandle =
        autoSelectWithSpectatorReset;
    FlipBit(wrongSpectatorResetHandle, 116u);
    EXPECT_EQ(DeploymentRepl::DecodeSpawnVolumeViewTargetBunch(
                  wrongSpectatorResetHandle.data(),
                  wrongSpectatorResetHandle.size(), 126u).error,
              DeploymentRepl::DecodeError::UnsupportedHandle);
    std::vector<uint8_t> presentSpectatorLocation =
        autoSelectWithSpectatorReset;
    FlipBit(presentSpectatorLocation, 125u);
    EXPECT_EQ(DeploymentRepl::DecodeSpawnVolumeViewTargetBunch(
                  presentSpectatorLocation.data(),
                  presentSpectatorLocation.size(), 126u).error,
              DeploymentRepl::DecodeError::UnsupportedSequence);
    std::vector<uint8_t> paddedSpectatorReset =
        autoSelectWithSpectatorReset;
    paddedSpectatorReset.push_back(0u);
    EXPECT_EQ(DeploymentRepl::DecodeSpawnVolumeViewTargetBunch(
                  paddedSpectatorReset.data(),
                  paddedSpectatorReset.size(), 127u).error,
              DeploymentRepl::DecodeError::TrailingBits);

    // Camera PackageMap refs are map/team-specific. A different nonzero static
    // ref keeps the exact RPC grammar valid.
    std::vector<uint8_t> alternateCamera = autoSelect;
    FlipBit(alternateCamera, 11u);
    const auto alternate = DeploymentRepl::DecodeSpawnVolumeViewTargetBunch(
        alternateCamera.data(), alternateCamera.size(), 116u);
    EXPECT_TRUE(alternate.valid());
    EXPECT_NE(alternate.bunch.cameraTargetRef,
              DeploymentRepl::kInstalledCompoundSpawnVolumeCameraRef);

    // Presence is mandatory and dynamic actor/None refs are not a grounded
    // spawn-volume camera shape.
    std::vector<uint8_t> omittedCamera = autoSelect;
    FlipBit(omittedCamera, 9u);
    EXPECT_EQ(DeploymentRepl::DecodeSpawnVolumeViewTargetBunch(
                  omittedCamera.data(), omittedCamera.size(), 116u).error,
              DeploymentRepl::DecodeError::UnsupportedSequence);

    std::vector<uint8_t> dynamicCamera = autoSelect;
    FlipBit(dynamicCamera, 10u);
    EXPECT_EQ(DeploymentRepl::DecodeSpawnVolumeViewTargetBunch(
                  dynamicCamera.data(), dynamicCamera.size(), 116u).error,
              DeploymentRepl::DecodeError::UnsupportedSequence);

    std::vector<uint8_t> dynamicNone = autoSelect;
    FlipBit(dynamicNone, 10u);
    for (size_t bit = 11u; bit < 21u; ++bit) {
        dynamicNone[bit / 8u] &=
            static_cast<uint8_t>(~(1u << (bit % 8u)));
    }
    EXPECT_EQ(DeploymentRepl::DecodeSpawnVolumeViewTargetBunch(
                  dynamicNone.data(), dynamicNone.size(), 116u).error,
              DeploymentRepl::DecodeError::UnsupportedSequence);

    std::vector<uint8_t> wrongSelection = autoSelect;
    FlipBit(wrongSelection, 52u);
    EXPECT_EQ(DeploymentRepl::DecodeSpawnVolumeViewTargetBunch(
                  wrongSelection.data(), wrongSelection.size(), 116u).error,
              DeploymentRepl::DecodeError::UnsupportedSequence);

    std::vector<uint8_t> forcedVoiceStop = forceOnly;
    FlipBit(forcedVoiceStop, 63u);
    EXPECT_EQ(DeploymentRepl::DecodeSpawnVolumeViewTargetBunch(
                  forcedVoiceStop.data(), forcedVoiceStop.size(), 64u).error,
              DeploymentRepl::DecodeError::UnsupportedSequence);
}

TEST(DeploymentReplication, DecodesExactRetailNotReadySpectatorBunch) {
    const std::vector<uint8_t> payload = Hex("b29be506b3fa4f1201");
    const auto decoded = DeploymentRepl::DecodeServerSetReadyToSpawnBunch(
        payload.data(), payload.size(), 65u);

    ASSERT_TRUE(decoded.valid());
    EXPECT_EQ(decoded.bunch.pattern, DeploymentRepl::ReadyToSpawnBunchPattern::
                                         NotReadySpectatorLocation);
    ASSERT_EQ(decoded.bunch.transitionCount, 1u);
    EXPECT_EQ(decoded.bunch.transitions[0].status, 2u);
    ASSERT_TRUE(decoded.bunch.spectatorLocation.has_value());
    EXPECT_EQ(decoded.bunch.spectatorLocation->x, -831.0f);
    EXPECT_EQ(decoded.bunch.spectatorLocation->y, 4085.0f);
    EXPECT_EQ(decoded.bunch.spectatorLocation->z, 292.0f);
}

TEST(DeploymentReplication, CompoundReadyDecoderRejectsTruncationAndPadding) {
    const std::vector<uint8_t> payload = Hex("b2d192bd8900");
    for (size_t bits = 1; bits < 41u; ++bits) {
        // The first ten bits are independently a complete, observed Ready RPC.
        if (bits == 10u) continue;
        EXPECT_FALSE(DeploymentRepl::DecodeServerSetReadyToSpawnBunch(
            payload.data(), payload.size(), bits).valid()) << bits;
    }
    EXPECT_EQ(DeploymentRepl::DecodeServerSetReadyToSpawnBunch(
                  payload.data(), payload.size(), 42u).error,
              DeploymentRepl::DecodeError::TrailingBits);
    EXPECT_EQ(DeploymentRepl::DecodeServerSetReadyToSpawnBunch(
                  payload.data(), payload.size(), 48u).error,
              DeploymentRepl::DecodeError::TrailingBits);
}

TEST(DeploymentReplication, CompoundReadyDecoderRejectsAlteredCompanions) {
    const std::vector<uint8_t> original41 = Hex("b2d192bd8900");
    for (const size_t bit : {10u, 19u, 29u, 31u, 40u}) {
        std::vector<uint8_t> altered = original41;
        FlipBit(altered, bit);
        EXPECT_FALSE(DeploymentRepl::DecodeServerSetReadyToSpawnBunch(
            altered.data(), altered.size(), 41u).valid()) << bit;
    }

    const std::vector<uint8_t> original62 = Hex("b2d1923d1647c311");
    for (const size_t bit : {31u, 40u, 41u, 52u, 61u}) {
        std::vector<uint8_t> altered = original62;
        FlipBit(altered, bit);
        EXPECT_FALSE(DeploymentRepl::DecodeServerSetReadyToSpawnBunch(
            altered.data(), altered.size(), 62u).valid()) << bit;
    }
}

TEST(DeploymentReplication, RejectsReservedReadyStatusTransactionally) {
    const EncodedRpc encoded = EncodeReady(true, 3);
    BitReader reader(encoded.bytes.data(), encoded.bytes.size(), encoded.bits);
    DeploymentRepl::ServerSetReadyToSpawn output;
    output.status = 99;
    DeploymentRepl::DecodeError error = DeploymentRepl::DecodeError::None;

    EXPECT_FALSE(DeploymentRepl::DecodeOneServerSetReadyToSpawn(
        reader, output, error));
    EXPECT_EQ(error, DeploymentRepl::DecodeError::InvalidReadyStatus);
    EXPECT_EQ(reader.BitPos(), 0u);
    EXPECT_EQ(output.status, 99u);
}

TEST(DeploymentReplication, RejectsEveryTruncatedReadyPrefixTransactionally) {
    const EncodedRpc encoded = EncodeReady(true, 2);
    for (size_t bits = 1; bits < encoded.bits; ++bits) {
        BitReader reader(encoded.bytes.data(), encoded.bytes.size(), bits);
        reader.SetOverflowHandler([](const char*, size_t, size_t, size_t) {});
        DeploymentRepl::ServerSetReadyToSpawn output;
        output.status = 99;
        DeploymentRepl::DecodeError error = DeploymentRepl::DecodeError::None;

        EXPECT_FALSE(DeploymentRepl::DecodeOneServerSetReadyToSpawn(
            reader, output, error));
        EXPECT_EQ(error, DeploymentRepl::DecodeError::Truncated);
        EXPECT_EQ(reader.BitPos(), 0u);
        EXPECT_EQ(output.status, 99u);
    }
}

TEST(DeploymentReplication, RejectsTrailingReadyBitsAndWrongHandle) {
    EncodedRpc trailing = EncodeReady(false);
    BitWriter writer;
    writer.SerializeInt(DeploymentRepl::kServerSetReadyToSpawnHandle,
                        DeploymentRepl::kRoPlayerControllerMaxHandle);
    writer.WriteBit(false);
    writer.WriteBit(false);
    trailing = {writer.GetBytes(), writer.NumBits()};
    EXPECT_EQ(DeploymentRepl::DecodeServerSetReadyToSpawn(
                  trailing.bytes.data(), trailing.bytes.size(), trailing.bits).error,
              DeploymentRepl::DecodeError::TrailingBits);

    const EncodedRpc spawn = EncodeSpawnSelect(true, 128);
    EXPECT_EQ(DeploymentRepl::DecodeServerSetReadyToSpawn(
                  spawn.bytes.data(), spawn.bytes.size(), spawn.bits).error,
              DeploymentRepl::DecodeError::UnsupportedHandle);
}

TEST(DeploymentReplication, DecodesSpawnSelectionPresenceAndByte) {
    const EncodedRpc omitted = EncodeSpawnSelect(false);
    const auto defaultResult = DeploymentRepl::DecodeServerSetSpawnSelect(
        omitted.bytes.data(), omitted.bytes.size(), omitted.bits);
    ASSERT_TRUE(defaultResult.valid());
    EXPECT_FALSE(defaultResult.rpc.presentOnWire);
    EXPECT_EQ(defaultResult.rpc.encodedSelection, 0u);

    const EncodedRpc explicitSelection = EncodeSpawnSelect(true, 137);
    const auto explicitResult = DeploymentRepl::DecodeServerSetSpawnSelect(
        explicitSelection.bytes.data(), explicitSelection.bytes.size(),
        explicitSelection.bits);
    ASSERT_TRUE(explicitResult.valid());
    EXPECT_TRUE(explicitResult.rpc.presentOnWire);
    EXPECT_EQ(explicitResult.rpc.encodedSelection, 137u);
    EXPECT_EQ(explicitResult.rpc.consumedBits, explicitSelection.bits);
}

TEST(DeploymentReplication, RejectsTruncatedAndTrailingSpawnSelection) {
    const EncodedRpc encoded = EncodeSpawnSelect(true, 128);
    for (size_t bits = 1; bits < encoded.bits; ++bits) {
        BitReader reader(encoded.bytes.data(), encoded.bytes.size(), bits);
        reader.SetOverflowHandler([](const char*, size_t, size_t, size_t) {});
        DeploymentRepl::ServerSetSpawnSelect output;
        output.encodedSelection = 99;
        DeploymentRepl::DecodeError error = DeploymentRepl::DecodeError::None;

        EXPECT_FALSE(DeploymentRepl::DecodeOneServerSetSpawnSelect(
            reader, output, error));
        EXPECT_EQ(error, DeploymentRepl::DecodeError::Truncated);
        EXPECT_EQ(reader.BitPos(), 0u);
        EXPECT_EQ(output.encodedSelection, 99u);
    }

    BitWriter writer;
    writer.SerializeInt(DeploymentRepl::kServerSetSpawnSelectHandle,
                        DeploymentRepl::kRoPlayerControllerMaxHandle);
    writer.WriteBit(true);
    writer.WriteByte(128);
    writer.WriteBit(true);
    const auto bytes = writer.GetBytes();
    EXPECT_EQ(DeploymentRepl::DecodeServerSetSpawnSelect(
                  bytes.data(), bytes.size(), writer.NumBits()).error,
              DeploymentRepl::DecodeError::TrailingBits);
}

TEST(DeploymentReplication, RejectsInvalidBuffers) {
    EXPECT_EQ(DeploymentRepl::DecodeServerSetReadyToSpawn(nullptr, 0, 0).error,
              DeploymentRepl::DecodeError::InvalidBuffer);

    const EncodedRpc encoded = EncodeReady(true, 1);
    EXPECT_EQ(DeploymentRepl::DecodeServerSetReadyToSpawn(
                  encoded.bytes.data(), encoded.bytes.size(),
                  encoded.bytes.size() * 8u + 1u).error,
              DeploymentRepl::DecodeError::InvalidBuffer);
}

TEST(DeploymentReplication, EncodesOwnerNextRespawnTimeExactly) {
    constexpr std::array<int32_t, 5> values{
        0, 9999999, -1, std::numeric_limits<int32_t>::min(),
        std::numeric_limits<int32_t>::max()};

    for (const int32_t value : values) {
        BitWriter writer;
        DeploymentRepl::WriteOwnerNextRespawnTime(writer, value);
        ASSERT_EQ(writer.NumBits(),
                  DeploymentRepl::kNextRespawnTimePropertyBits);

        const std::vector<uint8_t> bytes = writer.GetBytes();
        BitReader reader(bytes.data(), bytes.size(), writer.NumBits());
        EXPECT_EQ(reader.SerializeInt(
                      DeploymentRepl::kRoPlayerControllerMaxHandle),
                  DeploymentRepl::kNextRespawnTimeHandle);
        EXPECT_EQ(reader.ReadInt32(), value);
        EXPECT_EQ(reader.BitsLeft(), 0u);
        EXPECT_FALSE(reader.IsOverflowed());
    }
}

TEST(DeploymentReplication, OwnerNextRespawnTimeTruncationOverflowsSafely) {
    BitWriter writer;
    DeploymentRepl::WriteOwnerNextRespawnTime(writer, 9999999);
    const std::vector<uint8_t> bytes = writer.GetBytes();

    for (size_t bits = 0;
         bits < DeploymentRepl::kNextRespawnTimePropertyBits; ++bits) {
        BitReader reader(bytes.data(), bytes.size(), bits);
        reader.SetOverflowHandler([](const char*, size_t, size_t, size_t) {});
        (void)reader.SerializeInt(
            DeploymentRepl::kRoPlayerControllerMaxHandle);
        (void)reader.ReadInt32();
        EXPECT_TRUE(reader.IsOverflowed()) << bits;
    }
}

TEST(DeploymentReplication, EncodesAuthoritativeRemoteHumanPlayerId) {
    DeploymentRepl::RetailParticipantInitialState state;
    state.combat = {
        ParticipantId::Human(7), 83, 4, 2, 11, false};
    state.wirePlayerId = 73;
    state.serverTeamId = 1;
    state.playerName = "RemoteHuman";

    BitWriter writer;
    ASSERT_TRUE(DeploymentRepl::WriteRemotePriInitial(writer, state));
    const std::vector<uint8_t> bytes = writer.GetBytes();
    BitReader reader(bytes.data(), bytes.size(), writer.NumBits());
    constexpr uint32_t maxHandle =
        DeploymentRepl::kRoPlayerReplicationInfoMaxHandle;

    EXPECT_EQ(reader.SerializeInt(maxHandle), 24u);
    EXPECT_EQ(static_cast<int32_t>(reader.ReadUInt32()), 4);
    EXPECT_EQ(reader.SerializeInt(maxHandle), 28u);
    EXPECT_FALSE(reader.ReadBit());
    EXPECT_EQ(reader.SerializeInt(maxHandle), 31u);
    EXPECT_FALSE(reader.ReadBit());
    EXPECT_EQ(reader.SerializeInt(maxHandle), 32u);
    EXPECT_FALSE(reader.ReadBit());
    EXPECT_EQ(reader.SerializeInt(maxHandle), 33u);
    EXPECT_FALSE(reader.ReadBit());
    EXPECT_EQ(reader.SerializeInt(maxHandle), 36u);
    EXPECT_EQ(static_cast<int32_t>(reader.ReadUInt32()), 73);
    EXPECT_EQ(reader.SerializeInt(maxHandle), 37u);
    EXPECT_EQ(reader.ReadString(), std::string("RemoteHuman"));
    EXPECT_EQ(reader.SerializeInt(maxHandle), 39u);
    EXPECT_EQ(static_cast<int32_t>(reader.ReadUInt32()), 2);
    EXPECT_EQ(reader.SerializeInt(maxHandle), 40u);
    EXPECT_EQ(reader.ReadFloat(), 11.0f);
    EXPECT_EQ(reader.SerializeInt(maxHandle), 61u);
    EXPECT_FALSE(reader.ReadBit());
    EXPECT_EQ(reader.BitsLeft(), 0u);
    EXPECT_FALSE(reader.IsOverflowed());
}

TEST(DeploymentReplication, EncodesTaggedBotAndDynamicTeamReference) {
    DeploymentRepl::RetailParticipantInitialState state;
    state.combat = {ParticipantId::Bot(7), 0, 0, 3, 0, true};
    state.serverTeamId = 2;
    state.playerName = "Bot 7";

    BitWriter initial;
    ASSERT_TRUE(DeploymentRepl::WriteRemotePriInitial(initial, state));
    const std::vector<uint8_t> initialBytes = initial.GetBytes();
    BitReader reader(initialBytes.data(), initialBytes.size(), initial.NumBits());
    constexpr uint32_t maxHandle =
        DeploymentRepl::kRoPlayerReplicationInfoMaxHandle;
    EXPECT_EQ(reader.SerializeInt(maxHandle), 24u);
    (void)reader.ReadUInt32();
    EXPECT_EQ(reader.SerializeInt(maxHandle), 28u);
    EXPECT_TRUE(reader.ReadBit());

    BitWriter team;
    ASSERT_TRUE(DeploymentRepl::WriteRemotePriTeam(team, 76));
    const std::vector<uint8_t> teamBytes = team.GetBytes();
    BitReader teamReader(teamBytes.data(), teamBytes.size(), team.NumBits());
    EXPECT_EQ(teamReader.SerializeInt(maxHandle), 35u);
    const ActorRepl::NetGUIDRef reference =
        ActorRepl::ReadNetGUID(teamReader);
    EXPECT_TRUE(reference.isDynamic);
    EXPECT_EQ(reference.index, 76u);
    EXPECT_EQ(teamReader.BitsLeft(), 0u);
}

TEST(DeploymentReplication, CombatAndHealthWritersAreBoundedAndTransactional) {
    DeploymentRepl::RetailParticipantCombatState state{
        ParticipantId::Human(3), 27, 8, 5, 19, true};

    BitWriter pri;
    ASSERT_TRUE(DeploymentRepl::WriteRemotePriCombat(pri, state));
    const std::vector<uint8_t> priBytes = pri.GetBytes();
    BitReader priReader(priBytes.data(), priBytes.size(), pri.NumBits());
    constexpr uint32_t priMax =
        DeploymentRepl::kRoPlayerReplicationInfoMaxHandle;
    EXPECT_EQ(priReader.SerializeInt(priMax), 24u);
    EXPECT_EQ(static_cast<int32_t>(priReader.ReadUInt32()), 8);
    EXPECT_EQ(priReader.SerializeInt(priMax), 39u);
    EXPECT_EQ(static_cast<int32_t>(priReader.ReadUInt32()), 5);
    EXPECT_EQ(priReader.SerializeInt(priMax), 40u);
    EXPECT_EQ(priReader.ReadFloat(), 19.0f);
    EXPECT_EQ(priReader.SerializeInt(priMax), 61u);
    EXPECT_TRUE(priReader.ReadBit());
    EXPECT_EQ(priReader.BitsLeft(), 0u);

    BitWriter unchangedDead;
    ASSERT_TRUE(DeploymentRepl::WriteRemotePriCombat(
        unchangedDead, state, /*includeDead=*/false));
    const std::vector<uint8_t> unchangedDeadBytes = unchangedDead.GetBytes();
    BitReader unchangedDeadReader(
        unchangedDeadBytes.data(), unchangedDeadBytes.size(),
        unchangedDead.NumBits());
    EXPECT_EQ(unchangedDeadReader.SerializeInt(priMax), 24u);
    EXPECT_EQ(static_cast<int32_t>(unchangedDeadReader.ReadUInt32()), 8);
    EXPECT_EQ(unchangedDeadReader.SerializeInt(priMax), 39u);
    EXPECT_EQ(static_cast<int32_t>(unchangedDeadReader.ReadUInt32()), 5);
    EXPECT_EQ(unchangedDeadReader.SerializeInt(priMax), 40u);
    EXPECT_EQ(unchangedDeadReader.ReadFloat(), 19.0f);
    EXPECT_EQ(unchangedDeadReader.BitsLeft(), 0u);
    EXPECT_FALSE(unchangedDeadReader.IsOverflowed());

    BitWriter pawn;
    ASSERT_TRUE(DeploymentRepl::WriteRemotePawnHealth(pawn, state));
    const std::vector<uint8_t> pawnBytes = pawn.GetBytes();
    BitReader pawnReader(pawnBytes.data(), pawnBytes.size(), pawn.NumBits());
    EXPECT_EQ(pawnReader.SerializeInt(DeploymentRepl::kRoPawnMaxHandle), 33u);
    EXPECT_EQ(static_cast<int32_t>(pawnReader.ReadUInt32()), 27);
    EXPECT_EQ(pawnReader.BitsLeft(), 0u);

    BitWriter unchanged;
    unchanged.WriteBit(true);
    const size_t before = unchanged.NumBits();
    state.health = 101;
    EXPECT_FALSE(DeploymentRepl::WriteRemotePriCombat(unchanged, state));
    EXPECT_FALSE(DeploymentRepl::WriteRemotePawnHealth(unchanged, state));
    EXPECT_EQ(unchanged.NumBits(), before);
    EXPECT_FALSE(DeploymentRepl::WriteRemotePriTeam(unchanged, 1024));
    EXPECT_EQ(unchanged.NumBits(), before);
}

TEST(DeploymentReplication, SelectsCaptureBackedRemotePawnArchetypeByTeam) {
    EXPECT_EQ(DeploymentRepl::RemotePawnArchetypeForServerTeam(1),
              std::optional<uint32_t>(
                  DeploymentRepl::kRemoteSouthPawnArchetypeRef));
    EXPECT_EQ(DeploymentRepl::RemotePawnArchetypeForServerTeam(2),
              std::optional<uint32_t>(
                  DeploymentRepl::kRemoteNorthPawnArchetypeRef));
    EXPECT_FALSE(
        DeploymentRepl::RemotePawnArchetypeForServerTeam(0).has_value());
    EXPECT_FALSE(
        DeploymentRepl::RemotePawnArchetypeForServerTeam(3).has_value());
    EXPECT_EQ(DeploymentRepl::kRoPawnMaxHandle, 168u);
    EXPECT_EQ(DeploymentRepl::kRemotePawnMovementIntervalMs, 100u);
    EXPECT_EQ(DeploymentRepl::kRemotePawnCloseDelayMs, 4700u);
}

TEST(DeploymentReplication, EncodesReducedRemotePawnInitialTailExactly) {
    DeploymentRepl::RetailRemotePawnSnapshot snapshot;
    snapshot.participant = ParticipantId::Bot(7);
    snapshot.positionUu = {-8000.0f, 1200.0f, 64.0f};
    snapshot.health = 83;

    BitWriter writer;
    ASSERT_TRUE(DeploymentRepl::WriteRemotePawnInitial(writer, snapshot, 512));
    const std::vector<uint8_t> bytes = writer.GetBytes();
    BitReader reader(bytes.data(), bytes.size(), writer.NumBits());

    EXPECT_EQ(reader.SerializeInt(DeploymentRepl::kRoPawnMaxHandle), 10u);
    EXPECT_EQ(reader.ReadBits(4),
              static_cast<uint32_t>(
                  DeploymentRepl::kRemotePawnWalkingPhysics));
    EXPECT_EQ(reader.SerializeInt(DeploymentRepl::kRoPawnMaxHandle), 32u);
    const ActorRepl::NetGUIDRef pri = ActorRepl::ReadNetGUID(reader);
    EXPECT_TRUE(pri.isDynamic);
    EXPECT_EQ(pri.index, 512u);
    EXPECT_EQ(reader.SerializeInt(DeploymentRepl::kRoPawnMaxHandle), 33u);
    EXPECT_EQ(static_cast<int32_t>(reader.ReadUInt32()), 83);
    EXPECT_EQ(reader.BitsLeft(), 0u);
    EXPECT_FALSE(reader.IsOverflowed());
}

TEST(DeploymentReplication, FramesRemotePawnLifecycleBunchesExactly) {
    DeploymentRepl::RetailRemotePawnSnapshot snapshot;
    snapshot.participant = ParticipantId::Human(7);
    snapshot.positionUu = {-1234.0f, 5678.0f, 90.0f};
    snapshot.velocityUuPerSecond = {10.0f, 20.0f, 30.0f};
    snapshot.health = 75;

    const auto open = DeploymentRepl::MakeRemotePawnOpeningBunch(
        513, 4, 1, 512, snapshot);
    ASSERT_TRUE(open.has_value());
    EXPECT_TRUE(open->bControl);
    EXPECT_TRUE(open->bOpen);
    EXPECT_FALSE(open->bClose);
    EXPECT_TRUE(open->bReliable);
    EXPECT_EQ(open->chIndex, 513u);
    EXPECT_EQ(open->chType, 2u);
    EXPECT_EQ(open->chSequence, 4u);
    BitReader openReader(
        open->payload.data(), open->payload.size(), open->payloadBits);
    const ActorRepl::NetGUIDRef archetype =
        ActorRepl::ReadNetGUID(openReader);
    EXPECT_FALSE(archetype.isDynamic);
    EXPECT_EQ(archetype.index,
              DeploymentRepl::kRemoteSouthPawnArchetypeRef);
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    ActorRepl::ReadCompressedVector(openReader, x, y, z);
    EXPECT_FLOAT_EQ(x, -1234.0f);
    EXPECT_FLOAT_EQ(y, 5678.0f);
    EXPECT_FLOAT_EQ(z, 90.0f);
    // The next bits are h10 directly: no initial-rotation field intervenes.
    EXPECT_EQ(openReader.SerializeInt(DeploymentRepl::kRoPawnMaxHandle), 10u);

    const auto movement = DeploymentRepl::MakeRemotePawnMovementBunch(
        513, snapshot);
    ASSERT_TRUE(movement.has_value());
    EXPECT_FALSE(movement->bReliable);
    EXPECT_EQ(movement->chIndex, 513u);
    EXPECT_EQ(movement->chType, 2u);
    EXPECT_EQ(movement->chSequence, 0u);

    const auto death =
        DeploymentRepl::MakeRemotePawnDeathBunch(513, -62);
    ASSERT_TRUE(death.has_value());
    EXPECT_FALSE(death->bReliable);
    EXPECT_EQ(death->chSequence, 0u);

    const auto close =
        DeploymentRepl::MakeRemotePawnCloseBunch(513, 5);
    ASSERT_TRUE(close.has_value());
    EXPECT_TRUE(close->bControl);
    EXPECT_TRUE(close->bClose);
    EXPECT_FALSE(close->bOpen);
    EXPECT_TRUE(close->bReliable);
    EXPECT_EQ(close->chIndex, 513u);
    EXPECT_EQ(close->chType, 2u);
    EXPECT_EQ(close->chSequence, 5u);
    EXPECT_EQ(close->payloadBits, 0u);

    EXPECT_FALSE(DeploymentRepl::MakeRemotePawnOpeningBunch(
        1, 1, 1, 512, snapshot).has_value());
    EXPECT_FALSE(DeploymentRepl::MakeRemotePawnOpeningBunch(
        513, 1024, 1, 512, snapshot).has_value());
    EXPECT_FALSE(DeploymentRepl::MakeRemotePawnOpeningBunch(
        513, 1, 3, 512, snapshot).has_value());
    EXPECT_FALSE(DeploymentRepl::MakeRemotePawnCloseBunch(
        1024, 1).has_value());
}

TEST(DeploymentReplication, EncodesUnreliableRemotePawnMovementFieldsExactly) {
    DeploymentRepl::RetailRemotePawnSnapshot snapshot;
    snapshot.participant = ParticipantId::Human(7);
    snapshot.positionUu = {101.0f, -202.0f, 303.0f};
    snapshot.velocityUuPerSecond = {400.0f, -500.0f, 60.0f};
    snapshot.pitch = 0x1200;
    snapshot.yaw = 0x3400;
    snapshot.roll = 0x5600;
    snapshot.health = 99;

    BitWriter writer;
    ASSERT_TRUE(DeploymentRepl::WriteRemotePawnMovement(writer, snapshot));
    const std::vector<uint8_t> bytes = writer.GetBytes();
    BitReader reader(bytes.data(), bytes.size(), writer.NumBits());
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    EXPECT_EQ(reader.SerializeInt(DeploymentRepl::kRoPawnMaxHandle), 3u);
    ActorRepl::ReadCompressedVector(reader, x, y, z);
    EXPECT_FLOAT_EQ(x, 400.0f);
    EXPECT_FLOAT_EQ(y, -500.0f);
    EXPECT_FLOAT_EQ(z, 60.0f);
    EXPECT_EQ(reader.SerializeInt(DeploymentRepl::kRoPawnMaxHandle), 10u);
    EXPECT_EQ(reader.ReadBits(4),
              static_cast<uint32_t>(
                  DeploymentRepl::kRemotePawnWalkingPhysics));
    EXPECT_EQ(reader.SerializeInt(DeploymentRepl::kRoPawnMaxHandle), 12u);
    uint16_t pitch = 0;
    uint16_t yaw = 0;
    uint16_t roll = 0;
    ActorRepl::ReadCompressedRotator(reader, pitch, yaw, roll);
    EXPECT_EQ(pitch, 0x1200u);
    EXPECT_EQ(yaw, 0x3400u);
    EXPECT_EQ(roll, 0x5600u);
    EXPECT_EQ(reader.SerializeInt(DeploymentRepl::kRoPawnMaxHandle), 13u);
    ActorRepl::ReadCompressedVector(reader, x, y, z);
    EXPECT_FLOAT_EQ(x, 101.0f);
    EXPECT_FLOAT_EQ(y, -202.0f);
    EXPECT_FLOAT_EQ(z, 303.0f);
    EXPECT_EQ(reader.BitsLeft(), 0u);
    EXPECT_FALSE(reader.IsOverflowed());
}

TEST(DeploymentReplication, EncodesRemotePawnDeathCoreExactly) {
    BitWriter writer;
    ASSERT_TRUE(DeploymentRepl::WriteRemotePawnDeathCore(writer, -20));
    const std::vector<uint8_t> bytes = writer.GetBytes();
    BitReader reader(bytes.data(), bytes.size(), writer.NumBits());

    EXPECT_EQ(reader.SerializeInt(DeploymentRepl::kRoPawnMaxHandle), 20u);
    EXPECT_TRUE(reader.ReadBit());
    EXPECT_EQ(reader.SerializeInt(DeploymentRepl::kRoPawnMaxHandle), 33u);
    EXPECT_EQ(static_cast<int32_t>(reader.ReadUInt32()), -20);
    EXPECT_EQ(reader.SerializeInt(DeploymentRepl::kRoPawnMaxHandle), 32u);
    const ActorRepl::NetGUIDRef pri = ActorRepl::ReadNetGUID(reader);
    EXPECT_TRUE(pri.isDynamic);
    EXPECT_EQ(pri.index, 0u);
    EXPECT_EQ(reader.BitsLeft(), 0u);
    EXPECT_FALSE(reader.IsOverflowed());
}

TEST(DeploymentReplication, RemotePawnWritersRejectInvalidInputTransactionally) {
    DeploymentRepl::RetailRemotePawnSnapshot snapshot;
    snapshot.participant = ParticipantId::Human(2);
    snapshot.positionUu = {1.0f, 2.0f, 3.0f};
    snapshot.velocityUuPerSecond = {4.0f, 5.0f, 6.0f};
    snapshot.health = 100;

    BitWriter writer;
    writer.WriteBit(true);
    const size_t before = writer.NumBits();
    EXPECT_FALSE(DeploymentRepl::WriteRemotePawnInitial(writer, snapshot, 0));
    EXPECT_EQ(writer.NumBits(), before);

    snapshot.positionUu.x = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(DeploymentRepl::WriteRemotePawnMovement(writer, snapshot));
    EXPECT_EQ(writer.NumBits(), before);

    snapshot.positionUu.x = 1048576.0f;
    EXPECT_FALSE(DeploymentRepl::WriteRemotePawnInitial(writer, snapshot, 512));
    EXPECT_EQ(writer.NumBits(), before);

    snapshot.positionUu.x = 0.0f;
    snapshot.velocityUuPerSecond.y = 1048576.0f;
    EXPECT_FALSE(DeploymentRepl::WriteRemotePawnMovement(writer, snapshot));
    EXPECT_EQ(writer.NumBits(), before);
    EXPECT_FALSE(DeploymentRepl::WriteRemotePawnDeathCore(writer, 0));
    EXPECT_EQ(writer.NumBits(), before);
}

RS2V_TEST_MAIN()
