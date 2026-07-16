#!/usr/bin/env python3
"""Focused tests for source-grounded RS2 vehicle actor-open evidence."""

import sys
import unittest
from pathlib import Path


TOOLS_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS_DIR))
import extract_vehicle_capture_evidence as vehicle_evidence  # noqa: E402


SAMPLES = (
    (
        "AH1G",
        919,
        "54ba080070ac129911c9fcf3df00a8313c385689cc8864fe092c04d064f38bd6"
        "eac325e98217e803bb7f400040e53f000000761be0bf1df0df16f86f0ffcb709"
        "fedb05ff6d83fff6c17f1be1bf9df0df56f86f2ffcb719fedb0dff6d87fff6c3"
        "7f1be2bf1df1df96f86f4ffcb729fed706fe00",
        285994,
        "ROGameContent.ROHeli_AH1G_Content",
        (-92829, 9010, -412),
    ),
    (
        "OH6",
        973,
        "58ba0800f03b139c11a9fcf3df00a8313cf89d09ce8854fe05c07cd47e2473d5"
        "10d0a1ef82177403bb7f400040e5ff030000761be0bf1df0df16f86f0ffcb709f"
        "edb05ff6d83fff6c17f1be1bf9df0df56f86f2ffcb719fedb0dff6d87fff6c37"
        "f1be2bf1df1df96f86f4ffcb729fedb15ff6d8bfff6c5ffba00",
        285996,
        "ROGameContent.ROHeli_OH6_Content",
        (-91681, 9016, -428),
    ),
    (
        "UH1H",
        1206,
        "acba08007038137f0fc1fcf3df005063787038137f0fc1fcfb7f669c4118cfb2"
        "4551adda052a500776ff800080caffffff87e403000000761be0bf1df0df16f86"
        "f0ffcb709fedb05ff6d83fff6c17f1be1bf9df0df56f86f2ffcb719fedb0dff6"
        "d87fff6c37f1be2bf1df1df96f86f4ffcb729fedb15ff6d8bfff6c57f1be3bf9"
        "df1dfd6f86f6ffcb739fedb1dff6d8fffb540a480a400",
        286038,
        "ROGameContent.ROHeli_UH1H_Content",
        (-91709, 7934, -416),
    ),
)


class VehicleOpenEvidenceTests(unittest.TestCase):
    def test_exact_capture_samples_resolve_class_and_location(self):
        for name, bits, payload_hex, index, path, location in SAMPLES:
            with self.subTest(name=name):
                decoded = vehicle_evidence.decode_vehicle_open(
                    bytes.fromhex(payload_hex), bits
                )
                self.assertIsNotNone(decoded)
                self.assertEqual(decoded.archetype.static_index, index)
                self.assertEqual(decoded.archetype.class_path, path)
                self.assertEqual(decoded.location, location)
                self.assertGreater(decoded.bits_consumed, 32)
                self.assertLess(decoded.bits_consumed, bits)

    def test_compiled_export_indices_reconstruct_capture_static_refs(self):
        specs = list(vehicle_evidence.ARCHETYPES.values())
        specs.extend(vehicle_evidence.FACTORY_ARCHETYPES.values())
        for spec in specs:
            self.assertEqual(
                vehicle_evidence.ROGAMECONTENT_OBJECT_BASE
                + spec.package_export_index,
                spec.static_index,
            )

    def test_unknown_and_dynamic_refs_are_not_misclassified(self):
        payload = bytearray.fromhex(SAMPLES[0][2])
        payload[:4] = bytes.fromhex("60c10100")  # known non-vehicle PC ref 57520
        self.assertIsNone(
            vehicle_evidence.decode_vehicle_open(bytes(payload), SAMPLES[0][1])
        )

        payload = bytearray.fromhex(SAMPLES[0][2])
        payload[0] |= 1  # dynamic selector
        self.assertIsNone(
            vehicle_evidence.decode_vehicle_open(bytes(payload), SAMPLES[0][1])
        )

    def test_recognized_vehicle_ref_with_truncated_location_fails_closed(self):
        payload = bytes.fromhex(SAMPLES[0][2])
        with self.assertRaises(vehicle_evidence.EvidenceError):
            vehicle_evidence.decode_vehicle_open(payload[:4], 32)

        with self.assertRaises(vehicle_evidence.EvidenceError):
            vehicle_evidence.decode_vehicle_open(payload, len(payload) * 8 + 1)

    def test_generated_handle_tables_pin_vehicle_and_factory_fields(self):
        expected = {
            "netfields_u_ROVehicleFactory.txt": (25, "23", "bHasLockedVehicle"),
            "netfields_u_ROVehicleHelicopter.txt": (122, "75", "SeatMask"),
            "netfields_u_ROHeli_AH1G_Content.txt": (130, "103", "bEngineOn"),
            "netfields_u_ROHeli_OH6_Content.txt": (129, "109", "VehHitZoneHealths"),
            "netfields_u_ROHeli_UH1H_Content.txt": (150, "149", "DoorMGLeftFlashLocation"),
        }
        for file_name, (max_handle, handle, field) in expected.items():
            with self.subTest(file=file_name):
                text = (TOOLS_DIR / file_name).read_text(encoding="utf-8-sig")
                self.assertIn("maxHandle={}".format(max_handle), text.splitlines()[0])
                matching = [
                    line for line in text.splitlines()
                    if len(line.split()) >= 4 and line.split()[3] == field
                ]
                self.assertEqual(len(matching), 1)
                self.assertEqual(matching[0].split()[0], handle)

    def test_target_property_fixtures_consume_every_exact_bit(self):
        team = vehicle_evidence.decode_target_property_block(
            bytes.fromhex("32c12500"), 25, "teamInfo"
        )
        self.assertEqual(len(team), 1)
        self.assertEqual(
            (team[0].handle, team[0].array_index, team[0].value),
            (50, 4, vehicle_evidence.ObjectRef("dynamic", 75)),
        )
        self.assertEqual(team[0].bits_consumed, 25)

        pri = vehicle_evidence.decode_target_property_block(
            bytes.fromhex("40821f00"), 29, "pri"
        )
        self.assertEqual(
            [(field.handle, field.value) for field in pri],
            [(64, 4), (63, 0)],
        )
        self.assertEqual(sum(field.bits_consumed for field in pri), 29)

        team_ref = vehicle_evidence.decode_target_property_block(
            bytes.fromhex("631c00"), 17, "pri"
        )
        self.assertEqual(
            team_ref[0].value, vehicle_evidence.ObjectRef("dynamic", 56)
        )

    def test_factory_child_fixture_is_typed_only_as_a_whole_block(self):
        writer = vehicle_evidence.mc.BitWriter()
        writer.wint(23, vehicle_evidence.FACTORY_MAX_HANDLE)
        writer.bit(1)
        writer.wint(24, vehicle_evidence.FACTORY_MAX_HANDLE)
        writer.bit(1)
        writer.wint(201, 1024)
        fields = vehicle_evidence.decode_target_property_block(
            writer.to_bytes(), len(writer.bits), "factory"
        )
        self.assertEqual(
            [(field.handle, field.value) for field in fields],
            [
                (23, True),
                (24, vehicle_evidence.ObjectRef("dynamic", 201)),
            ],
        )

        with self.assertRaises(vehicle_evidence.EvidenceError):
            vehicle_evidence.decode_target_property_block(
                writer.to_bytes(), len(writer.bits) - 1, "factory"
            )

        unrelated = vehicle_evidence.mc.BitWriter()
        unrelated.wint(13, vehicle_evidence.FACTORY_MAX_HANDLE)
        self.assertIsNone(vehicle_evidence.decode_target_property_block(
            unrelated.to_bytes(), len(unrelated.bits), "factory"
        ))

    def test_static_array_index_and_fstring_validation_fail_closed(self):
        invalid_index = vehicle_evidence.mc.BitWriter()
        invalid_index.wint(50, vehicle_evidence.TEAMINFO_MAX_HANDLE)
        invalid_index.wu(10, 8)
        invalid_index.bit(1)
        invalid_index.wint(5, 1024)
        with self.assertRaises(vehicle_evidence.EvidenceError):
            vehicle_evidence.decode_target_property_block(
                invalid_index.to_bytes(), len(invalid_index.bits), "teamInfo"
            )

        missing_terminator = vehicle_evidence.mc.BitWriter()
        missing_terminator.wint(48, vehicle_evidence.TEAMINFO_MAX_HANDLE)
        missing_terminator.wu(0, 8)
        missing_terminator.wu(2, 32)
        missing_terminator.wu(ord("X"), 8)
        missing_terminator.wu(ord("Y"), 8)
        with self.assertRaises(vehicle_evidence.EvidenceError):
            vehicle_evidence.decode_target_property_block(
                missing_terminator.to_bytes(), len(missing_terminator.bits), "teamInfo"
            )

    def test_correlations_respect_channel_generations_and_join_seat_state(self):
        correlator = vehicle_evidence.EvidenceCorrelator()
        endpoint = "127.0.0.1:7777"
        team_actor = correlator.open_actor(
            endpoint, 56, "teamInfo", "ROGame.ROTeamInfo", 10
        )
        vehicle_actor = correlator.open_actor(
            endpoint, 75, "vehicle", "ROGameContent.ROHeli_UH1H_Content", 11,
            {"classPath": "ROGameContent.ROHeli_UH1H_Content", "location": {"x": 1, "y": 2, "z": 3}},
        )
        pri_actor = correlator.open_actor(
            endpoint, 14, "pri", "ROGame.ROPlayerReplicationInfo", 12
        )

        def context(frame):
            return vehicle_evidence.EvidenceContext(
                frame, float(frame), frame, 25, "A" * 64
            )

        team_slot = vehicle_evidence.decode_target_property_block(
            bytes.fromhex("32c12500"), 25, "teamInfo"
        )
        direct = correlator.observe_block(team_actor, team_slot, context(13))
        self.assertEqual(len(direct), 1)
        self.assertEqual(direct[0]["vehicleActor"]["channelGeneration"], 1)

        team_ref = vehicle_evidence.decode_target_property_block(
            bytes.fromhex("631c00"), 17, "pri"
        )
        self.assertEqual(correlator.observe_block(pri_actor, team_ref, context(14)), [])
        seat = vehicle_evidence.decode_target_property_block(
            bytes.fromhex("40821f00"), 29, "pri"
        )
        joined = correlator.observe_block(pri_actor, seat, context(15))
        self.assertEqual(len(joined), 1)
        self.assertEqual(joined[0]["correlation"], "pri.helicopterSeat")
        self.assertEqual((joined[0]["arrayIndex"], joined[0]["seatIndex"]), (4, 0))
        self.assertEqual(correlator.observe_block(pri_actor, seat, context(16)), [])

        correlator.close_actor(endpoint, 75)
        replacement = correlator.open_actor(
            endpoint, 75, "vehicle", "ROGameContent.ROHeli_UH1H_Content", 20,
            {"classPath": "ROGameContent.ROHeli_UH1H_Content", "location": {"x": 4, "y": 5, "z": 6}},
        )
        self.assertNotEqual(vehicle_actor, replacement)
        self.assertEqual(correlator.observe_block(pri_actor, seat, context(21)), [])
        rebound = correlator.observe_block(team_actor, team_slot, context(22))
        self.assertEqual(
            [record["correlation"] for record in rebound],
            ["teamInfo.TeamHelicopterArray"],
        )
        self.assertEqual(rebound[0]["vehicleActor"]["channelGeneration"], 2)
        refreshed_seat = correlator.observe_block(pri_actor, seat, context(23))
        self.assertEqual(
            [record["correlation"] for record in refreshed_seat],
            ["pri.helicopterSeat"],
        )
        self.assertEqual(
            refreshed_seat[0]["vehicleActor"]["channelGeneration"], 2
        )

    def test_packet_gap_invalidates_source_and_target_identities(self):
        correlator = vehicle_evidence.EvidenceCorrelator()
        endpoint = "127.0.0.1:7777"
        team_actor = correlator.open_actor(
            endpoint, 56, "teamInfo", "ROGame.ROTeamInfo", 10
        )
        correlator.open_actor(
            endpoint, 75, "vehicle", "ROGameContent.ROHeli_UH1H_Content", 11,
            {
                "classPath": "ROGameContent.ROHeli_UH1H_Content",
                "location": {"x": 1, "y": 2, "z": 3},
            },
        )
        team_slot = vehicle_evidence.decode_target_property_block(
            bytes.fromhex("32c12500"), 25, "teamInfo"
        )
        context = vehicle_evidence.EvidenceContext(
            12, 12.0, 12, 25, "A" * 64
        )
        self.assertEqual(
            len(correlator.observe_block(team_actor, team_slot, context)), 1
        )

        invalidated = correlator.invalidate_endpoint(endpoint)
        self.assertEqual(
            [(actor.channel, actor.kind) for actor in invalidated],
            [(56, "teamInfo"), (75, "vehicle")],
        )
        self.assertIsNone(correlator.current_actor(endpoint, 56))
        self.assertIsNone(correlator.current_actor(endpoint, 75))
        self.assertEqual(
            correlator.observe_block(team_actor, team_slot, context), []
        )

    def test_factory_child_ref_tracks_the_current_vehicle_generation(self):
        correlator = vehicle_evidence.EvidenceCorrelator()
        endpoint = "127.0.0.1:7777"
        factory = correlator.open_actor(
            endpoint, 30, "factory",
            "ROGameContent.ROVehicleFactory_UH1H", 10,
        )
        first_vehicle = correlator.open_actor(
            endpoint, 201, "vehicle", "ROGameContent.ROHeli_UH1H_Content", 11,
            {
                "classPath": "ROGameContent.ROHeli_UH1H_Content",
                "location": {"x": 1, "y": 2, "z": 3},
            },
        )
        writer = vehicle_evidence.mc.BitWriter()
        writer.wint(24, vehicle_evidence.FACTORY_MAX_HANDLE)
        writer.bit(1)
        writer.wint(201, 1024)
        child = vehicle_evidence.decode_target_property_block(
            writer.to_bytes(), len(writer.bits), "factory"
        )
        context = vehicle_evidence.EvidenceContext(
            12, 12.0, 12, len(writer.bits), "A" * 64
        )
        first = correlator.observe_block(factory, child, context)
        self.assertEqual(
            [record["correlation"] for record in first],
            ["factory.ChildVehicle"],
        )
        self.assertEqual(first[0]["vehicleActor"]["channelGeneration"], 1)

        correlator.close_actor(endpoint, 201)
        replacement = correlator.open_actor(
            endpoint, 201, "vehicle", "ROGameContent.ROHeli_UH1H_Content", 20,
            {
                "classPath": "ROGameContent.ROHeli_UH1H_Content",
                "location": {"x": 4, "y": 5, "z": 6},
            },
        )
        self.assertNotEqual(first_vehicle, replacement)
        rebound = correlator.observe_block(factory, child, context)
        self.assertEqual(
            [record["correlation"] for record in rebound],
            ["factory.ChildVehicle"],
        )
        self.assertEqual(rebound[0]["vehicleActor"]["channelGeneration"], 2)


if __name__ == "__main__":
    unittest.main()
