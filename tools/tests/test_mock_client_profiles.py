#!/usr/bin/env python3
"""Focused unit tests for mock_client spawn-profile and GRI validation."""

import sys
import struct
import unittest
from pathlib import Path
from unittest import mock


TOOLS_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS_DIR))
import mock_client  # noqa: E402


class RecordingSocket:
    def __init__(self):
        self.sent = []
        self.closed = False

    def sendto(self, data, address):
        self.sent.append((data, address))
        return len(data)

    def settimeout(self, _seconds):
        pass

    def recvfrom(self, _size):
        raise mock_client.socket.timeout()

    def close(self):
        self.closed = True


class SpawnProfileTests(unittest.TestCase):
    @staticmethod
    def _bits_hex(bits):
        encoded = bytearray((len(bits) + 7) // 8)
        for index, bit in enumerate(bits):
            encoded[index >> 3] |= int(bool(bit)) << (index & 7)
        return bytes(encoded).hex()

    @staticmethod
    def _write_handle(writer, handle):
        writer.wint(handle, mock_client.OBJECTIVE_GRI_MAX_HANDLE)

    @classmethod
    def _write_int32(cls, writer, handle, value):
        cls._write_handle(writer, handle)
        writer.wu(value & 0xFFFFFFFF, 32)

    @classmethod
    def _write_int_array(cls, writer, handle, slot, value):
        cls._write_handle(writer, handle)
        writer.wu(slot, 8)
        writer.wu(value & 0xFFFFFFFF, 32)

    @classmethod
    def _write_byte_array(cls, writer, handle, slot, value):
        cls._write_handle(writer, handle)
        writer.wu(slot, 8)
        writer.wu(value, 8)

    def test_resort_profile_preserves_legacy_defaults(self):
        mapping, gri, channels = mock_client.resolve_spawn_profile()
        self.assertEqual(mapping, [0, 1, 2, 3, 4])
        self.assertEqual(gri, 54)
        self.assertEqual(channels, {2, 21, 26, 54, 56, 76})

    def test_spawn_team_contracts_pin_south_and_north_graphs(self):
        south = mock_client.resolve_spawn_team()
        self.assertEqual(south.team_id, 1)
        self.assertEqual(south.retail_team_id, 1)
        self.assertEqual(south.pawn_class_ref, 286151)
        self.assertEqual(
            south.loadout_class_map,
            {210: 286374, 211: 286391, 212: 286464,
             213: 286109, 214: 286389, 219: 82735})
        self.assertEqual(south.weapon_next_map,
                         {210: 211, 211: 212, 212: 213, 213: 214, 214: 0})
        self.assertEqual(south.minimum_switch_best_weapon_count, 6)

        north = mock_client.resolve_spawn_team(2)
        self.assertEqual(north.retail_team_id, 0)
        self.assertEqual(north.pawn_class_ref, 286147)
        self.assertEqual(north.role_payload_hex, "af4456150080c301")
        self.assertEqual(
            north.loadout_class_map,
            {210: 286271, 212: 286804, 214: 286758, 219: 82735})
        self.assertEqual(north.weapon_next_map, {210: 212, 212: 214, 214: 0})
        self.assertEqual(north.weapon_max_handle_map,
                         {210: 99, 212: 101, 214: 101})
        self.assertEqual(north.minimum_switch_best_weapon_count, 4)

        for invalid in (0, 3, True, "2", None):
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                mock_client.resolve_spawn_team(invalid)

    def test_team_and_role_request_builders_are_capture_exact(self):
        self.assertEqual(
            self._bits_hex(mock_client.build_team_selection_bits()), "aa0600")
        self.assertEqual(
            self._bits_hex(mock_client.build_team_selection_bits(2)), "aa00")
        self.assertEqual(len(mock_client.build_team_selection_bits(2)), 10)

        self.assertEqual(
            self._bits_hex(mock_client.build_role_selection_bits()),
            "af465c150080c301")
        self.assertEqual(
            self._bits_hex(mock_client.build_role_selection_bits(2)),
            "af4456150080c301")
        self.assertEqual(len(mock_client.build_role_selection_bits(2)), 57)

    def test_north_attachment_and_tail_literals_decode_semantically(self):
        north = mock_client.NORTH_SPAWN_TEAM
        reader = mock_client.BitReader(
            bytes.fromhex(north.attachment_payload_hex),
            north.attachment_payload_bits)
        decoded_attachments = []
        for _ in north.attachments:
            self.assertEqual(reader.rint(168), 167)
            slot = reader.ru(8)
            self.assertEqual(reader.bit(), 0)
            self.assertEqual(reader.bit(), 0)  # static NetGUID selector
            decoded_attachments.append((slot, reader.rint(0x80000000)))
        self.assertEqual(tuple(decoded_attachments), north.attachments)
        self.assertEqual(reader.rint(168), 148)
        encumbrance = struct.unpack("<f", reader.ru(32).to_bytes(4, "little"))[0]
        self.assertAlmostEqual(encumbrance, 9.92, places=5)
        self.assertFalse(reader.error)
        self.assertEqual(reader.p, reader.n)

        tail = mock_client.BitReader(
            bytes.fromhex(north.final_tail_hex), north.final_tail_bits)
        self.assertEqual(tail.rint(mock_client.ROPC_MAXHANDLE), 28)
        self.assertEqual(tail.bit(), 0)
        self.assertEqual(tail.rint(mock_client.ROPC_MAXHANDLE), 28)
        self.assertEqual(tail.bit(), 0)
        self.assertEqual(
            [tail.rint(mock_client.ROPC_MAXHANDLE) for _ in range(3)],
            [390, 226, 101])
        self.assertEqual([tail.bit() for _ in range(4)], [0, 1, 0, 0])
        self.assertFalse(tail.error)
        self.assertEqual(tail.p, tail.n)

    def test_hue_city_profile_uses_live_bootstrap_channels_and_cooked_ids(self):
        mapping, gri, channels = mock_client.resolve_spawn_profile("hue-city")
        self.assertEqual(mapping, [1, 2, 3, 4, 5, 6])
        self.assertEqual(gri, 3)
        self.assertEqual(channels, {2, 3, 4, 5, 26})

    def test_cu_chi_profile_uses_live_bootstrap_channels_and_cooked_ids(self):
        mapping, gri, channels = mock_client.resolve_spawn_profile("cu-chi")
        self.assertEqual(mapping, [2, 3, 4, 7, 5, 10, 9])
        self.assertEqual(gri, 3)
        self.assertEqual(channels, {2, 3, 4, 5, 26})

    def test_compound_profile_uses_live_bootstrap_channels_and_cooked_ids(self):
        mapping, gri, channels = mock_client.resolve_spawn_profile("compound")
        self.assertEqual(mapping, [1, 2, 3])
        self.assertEqual(gri, 3)
        self.assertEqual(channels, {2, 3, 4, 5, 26})

    def test_remote_participant_pool_classifies_only_odd_pawn_channels(self):
        for channel in (512, 514, 766):
            self.assertFalse(
                mock_client.is_remote_participant_pawn_channel(channel))

    def test_leading_actor_rpc_probe_detects_only_matching_non_open_bunch(self):
        writer = mock_client.BitWriter()
        writer.wint(210, mock_client.ROPC_MAXHANDLE)
        bunch = {
            "chIndex": 2,
            "bOpen": 0,
            "payloadHex": writer.to_bytes().hex(),
            "bits": len(writer.bits),
        }
        packet = {"ok": True, "bunches": [bunch]}

        self.assertTrue(mock_client.packet_starts_with_actor_rpc(
            packet, 2, 210, mock_client.ROPC_MAXHANDLE))
        self.assertFalse(mock_client.packet_starts_with_actor_rpc(
            packet, 3, 210, mock_client.ROPC_MAXHANDLE))
        self.assertFalse(mock_client.packet_starts_with_actor_rpc(
            packet, 2, 211, mock_client.ROPC_MAXHANDLE))

        open_packet = {"ok": True, "bunches": [{**bunch, "bOpen": 1}]}
        self.assertFalse(mock_client.packet_starts_with_actor_rpc(
            open_packet, 2, 210, mock_client.ROPC_MAXHANDLE))
        for channel in (513, 515, 767):
            self.assertTrue(
                mock_client.is_remote_participant_pawn_channel(channel))
        for channel in (511, 768, None, "513"):
            self.assertFalse(
                mock_client.is_remote_participant_pawn_channel(channel))

    def test_expected_objective_count_overrides_profile_mapping(self):
        mapping, gri, channels = mock_client.resolve_spawn_profile(
            "hue-city", expected_objectives=4)
        self.assertEqual(mapping, [0, 1, 2, 3])
        self.assertEqual(gri, 3)
        self.assertEqual(channels, {2, 3, 4, 5, 26})

    def test_explicit_mapping_and_channels_override_profile(self):
        mapping, gri, channels = mock_client.resolve_spawn_profile(
            "compound", objective_mapping=[7, 9], gri_channel=99,
            menu_channels={2, 26, 99})
        self.assertEqual(mapping, [7, 9])
        self.assertEqual(gri, 99)
        self.assertEqual(channels, {2, 26, 99})

    def test_cli_profile_resolution_and_validation_are_wired(self):
        with mock.patch.object(
                sys, "argv", ["mock_client.py", "spawn", "--profile", "hue-city"]), \
                mock.patch.object(mock_client, "spawn", return_value=0) as spawn:
            self.assertEqual(mock_client.main(), 0)
        spawn.assert_called_once_with(
            "127.0.0.1", 7777, 35.0, [1, 2, 3, 4, 5, 6], 3,
            {2, 3, 4, 5, 26}, 0.0, 1)

        invalid_cases = (
            {"expected_objectives": 0},
            {"objective_mapping": [1, 1]},
            {"gri_channel": 3, "menu_channels": {2, 4, 5, 26}},
        )
        for overrides in invalid_cases:
            with self.subTest(overrides=overrides):
                with self.assertRaises(ValueError):
                    mock_client.resolve_spawn_profile("hue-city", **overrides)

    def test_control_close_is_canonical_empty_reliable_ch0_bunch(self):
        bunch = mock_client.make_control_close(27)
        wire = mock_client.encode_packet(91, [bunch], 1280)
        decoded = mock_client.decode_packet(wire, bd_max=1280 * 8)

        self.assertTrue(decoded["ok"])
        self.assertEqual(decoded["pid"], 91)
        self.assertEqual(len(decoded["bunches"]), 1)
        close = decoded["bunches"][0]
        self.assertEqual(
            (close["bControl"], close["bOpen"], close["bClose"],
             close["bReliable"], close["chIndex"], close["chType"],
             close["chSeq"], close["bits"], close["payloadHex"]),
            (1, 0, 1, 1, 0, 1, 27, 0, ""))

    def test_ack_helper_retires_only_data_bearing_server_packets(self):
        sock = RecordingSocket()
        server = ("127.0.0.1", 7777)
        next_pid, acknowledged = mock_client.acknowledge_server_bunch_packet(
            sock, server, mock_client.MAX_PACKETID - 1,
            {"ok": True, "pid": 321, "bunches": [{"chIndex": 0}]})

        self.assertTrue(acknowledged)
        self.assertEqual(next_pid, 0)
        self.assertEqual(sock.sent[0][1], server)
        decoded = mock_client.decode_packet(
            sock.sent[0][0], bd_max=1280 * 8)
        self.assertTrue(decoded["ok"])
        self.assertEqual(decoded["pid"], mock_client.MAX_PACKETID - 1)
        self.assertEqual(decoded["acks"], [321])
        self.assertEqual(decoded["bunches"], [])

        unchanged, acknowledged = mock_client.acknowledge_server_bunch_packet(
            sock, server, next_pid,
            {"ok": True, "pid": 322, "acks": [1], "bunches": []})
        self.assertFalse(acknowledged)
        self.assertEqual(unchanged, next_pid)
        self.assertEqual(len(sock.sent), 1)

    def test_graceful_close_sender_wraps_packet_and_channel_cursors(self):
        sock = RecordingSocket()
        server = ("127.0.0.1", 7777)
        next_pid, next_seq = mock_client.send_graceful_control_close(
            sock, server, mock_client.MAX_PACKETID - 1, 1023)

        self.assertEqual((next_pid, next_seq), (0, 0))
        self.assertEqual(sock.sent[0][1], server)
        decoded = mock_client.decode_packet(
            sock.sent[0][0], bd_max=1280 * 8)
        self.assertTrue(decoded["ok"])
        self.assertEqual(decoded["pid"], mock_client.MAX_PACKETID - 1)
        self.assertEqual(len(decoded["bunches"]), 1)
        close = decoded["bunches"][0]
        self.assertEqual(
            (close["bControl"], close["bOpen"], close["bClose"],
             close["bReliable"], close["chIndex"], close["chType"],
             close["chSeq"], close["bits"]),
            (1, 0, 1, 1, 0, 1, 1023, 0))

    def test_react_closes_even_when_validation_fails(self):
        sock = RecordingSocket()
        with mock.patch.object(mock_client.socket, "socket", return_value=sock), \
                mock.patch("builtins.print"):
            self.assertEqual(mock_client.react("127.0.0.1", 7777), 1)

        self.assertTrue(sock.closed)
        decoded = mock_client.decode_packet(
            sock.sent[-1][0], bd_max=1280 * 8)
        close = decoded["bunches"][0]
        self.assertEqual(
            (close["bClose"], close["chIndex"], close["chSeq"]),
            (1, 0, 5))
    def test_reconnect_closes_replacement_session_even_on_failure(self):
        sock = RecordingSocket()
        clock = iter(float(index) * 0.2 for index in range(1000))
        with mock.patch.object(mock_client.socket, "socket", return_value=sock), \
                mock.patch.object(mock_client.time, "time",
                                  side_effect=lambda: next(clock)), \
                mock.patch("builtins.print"):
            self.assertEqual(mock_client.reconnect("127.0.0.1", 7777), 1)

        self.assertTrue(sock.closed)
        decoded = mock_client.decode_packet(
            sock.sent[-1][0], bd_max=1280 * 8)
        close = decoded["bunches"][0]
        self.assertEqual(
            (close["bClose"], close["chIndex"], close["chSeq"]),
            (1, 0, 5))

    def test_spawn_closes_even_when_validation_fails_before_bootstrap(self):
        sock = RecordingSocket()
        with mock.patch.object(mock_client.socket, "socket", return_value=sock), \
                mock.patch("builtins.print"):
            result = mock_client.spawn(
                "127.0.0.1", 7777, 0.0, [0, 1, 2, 3, 4], 54,
                {2, 21, 26, 54, 56, 76}, 0.0)

        self.assertEqual(result, 1)
        self.assertTrue(sock.closed)
        decoded = mock_client.decode_packet(
            sock.sent[-1][0], bd_max=1280 * 8)
        close = decoded["bunches"][0]
        self.assertEqual(
            (close["bClose"], close["chIndex"], close["chSeq"]),
            (1, 0, 5))
        sent_channels = {
            bunch["chIndex"]
            for wire, _ in sock.sent
            for bunch in mock_client.decode_packet(
                wire, bd_max=1280 * 8).get("bunches", [])
        }
        self.assertNotIn(219, sent_channels)

    def test_cli_linger_is_bounded_and_forwarded_to_spawn(self):
        with mock.patch.object(
                sys, "argv", ["mock_client.py", "spawn", "--linger", "12.5"]), \
                mock.patch.object(mock_client, "spawn", return_value=0) as spawn:
            self.assertEqual(mock_client.main(), 0)
        self.assertEqual(spawn.call_args.args[-2:], (12.5, 1))

        with mock.patch.object(
                sys, "argv", ["mock_client.py", "spawn", "--team", "2"]), \
                mock.patch.object(mock_client, "spawn", return_value=0) as spawn:
            self.assertEqual(mock_client.main(), 0)
        self.assertEqual(spawn.call_args.args[-1], 2)

        for invalid in ("-0.1", "600.1", "nan", "inf"):
            with self.subTest(invalid=invalid), \
                    mock.patch.object(
                        sys, "argv", ["mock_client.py", "spawn", "--linger", invalid]), \
                    mock.patch.object(sys, "stderr"), \
                    self.assertRaises(SystemExit):
                mock_client.main()

    def test_supremacy_fields_do_not_hide_objective_baseline(self):
        writer = mock_client.BitWriter()
        self._write_int_array(writer, 46, 0, 3)
        self._write_int_array(writer, 46, 1, 8)
        self._write_int32(writer, 47, -37)
        self._write_int32(writer, 48, 500)
        self._write_handle(writer, 100)
        writer.bit(0)
        self._write_byte_array(writer, 178, 0, 0x50)
        self._write_byte_array(writer, 179, 0, 1)

        decoded = mock_client.decode_objective_gri_payload(
            writer.to_bytes(), len(writer.bits))

        self.assertTrue(decoded["ok"])
        self.assertTrue(decoded["complete"])
        self.assertIsNone(decoded["unknown_handle"])
        self.assertEqual(decoded["fields"][(46, 0)], 3)
        self.assertEqual(decoded["fields"][(46, 1)], 8)
        self.assertEqual(decoded["scalars"][47], -37)
        self.assertEqual(decoded["scalars"][48], 500)
        self.assertFalse(decoded["scalars"][100])
        self.assertEqual(decoded["fields"][(178, 0)], 0x50)
        self.assertEqual(decoded["fields"][(179, 0)], 1)

    def test_skirmish_fields_do_not_hide_objective_baseline(self):
        writer = mock_client.BitWriter()
        self._write_int_array(writer, 37, 2, 100)
        self._write_int_array(writer, 38, 1, 200)
        for handle, value in ((39, 2), (40, 5), (41, 3), (67, 140)):
            self._write_int32(writer, handle, value)
        for handle, value in ((116, 1), (117, 0)):
            self._write_handle(writer, handle)
            writer.bit(value)
        self._write_handle(writer, 126)
        writer.wu(1, 8)
        self._write_byte_array(writer, 129, 0, 3)
        self._write_handle(writer, 100)
        writer.bit(0)
        self._write_byte_array(writer, 178, 0, 0x70)
        self._write_byte_array(writer, 179, 0, 7)

        decoded = mock_client.decode_objective_gri_payload(
            writer.to_bytes(), len(writer.bits))

        self.assertTrue(decoded["ok"])
        self.assertTrue(decoded["complete"])
        self.assertEqual(decoded["fields"][(37, 2)], 100)
        self.assertEqual(decoded["fields"][(38, 1)], 200)
        self.assertEqual(
            {handle: decoded["scalars"][handle]
             for handle in (39, 40, 41, 67)},
            {39: 2, 40: 5, 41: 3, 67: 140})
        self.assertTrue(decoded["scalars"][116])
        self.assertFalse(decoded["scalars"][117])
        self.assertEqual(decoded["scalars"][126], 1)
        self.assertEqual(decoded["fields"][(129, 0)], 3)
        self.assertFalse(decoded["scalars"][100])
        self.assertEqual(decoded["fields"][(178, 0)], 0x70)
        self.assertEqual(decoded["fields"][(179, 0)], 7)

    def test_objective_payload_truncation_and_unknown_handle_fail_closed(self):
        truncated = mock_client.BitWriter()
        self._write_handle(truncated, 46)
        truncated.wu(1, 8)
        truncated.wu(0xAB, 8)

        decoded = mock_client.decode_objective_gri_payload(
            truncated.to_bytes(), len(truncated.bits))
        self.assertFalse(decoded["ok"])
        self.assertFalse(decoded["complete"])
        self.assertNotIn((46, 1), decoded["fields"])

        unknown = mock_client.BitWriter()
        self._write_handle(unknown, 42)
        self._write_handle(unknown, 100)
        unknown.bit(0)
        decoded = mock_client.decode_objective_gri_payload(
            unknown.to_bytes(), len(unknown.bits))
        self.assertTrue(decoded["ok"])
        self.assertFalse(decoded["complete"])
        self.assertEqual(decoded["unknown_handle"], 42)
        self.assertNotIn(100, decoded["scalars"])


if __name__ == "__main__":
    unittest.main()
