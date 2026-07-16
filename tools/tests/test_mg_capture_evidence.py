from __future__ import annotations

import copy
import hashlib
import io
import json
import os
import struct
import tempfile
import threading
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

from tools import extract_mg_capture_evidence as audit
from tools import mock_client as mc


SERVER = "203.0.113.9"
CLIENT = "198.51.100.4"
SERVER_PORT = 7777
CLIENT_PORT = 51000


def _ansi(value: str) -> bytes:
    encoded = value.encode("ascii") + b"\0"
    return struct.pack("<i", len(encoded)) + encoded


def _uses(spec: audit.PackageSpec) -> bytes:
    return b"".join((
        bytes((audit.NMT_USES,)),
        bytes.fromhex(spec.raw_guid_hex),
        _ansi(spec.name),
        _ansi(spec.extension),
        struct.pack("<II", spec.flags, spec.generation),
        _ansi("None"),
        bytes(8),
    ))


def _uses_bunch(
    *, sequence: int = 1, specs: tuple[audit.PackageSpec, ...] = audit.PACKAGE_ORDER,
    close: bool = False,
) -> dict[str, object]:
    return {
        "bControl": int(close),
        "bOpen": 0,
        "bClose": int(close),
        "bReliable": 1,
        "chIndex": 0,
        "chType": 1,
        "chSeq": sequence,
        "payload": b"".join(_uses(spec) for spec in specs),
    }


def _role_bunch(*, sequence: int = 1, bits: list[int] | None = None) -> dict[str, object]:
    return {
        "bControl": 0,
        "bOpen": 0,
        "bClose": 0,
        "bReliable": 1,
        "chIndex": audit.ROPC_CHANNEL,
        "chType": 2,
        "chSeq": sequence,
        "bits_payload": bits if bits is not None else mc.packed_bits(
            audit.CONSTRUCTED_H175_HEX, audit.CONSTRUCTED_H175_BITS
        ),
    }


def _actor_open_bits(static_reference: int, tail: tuple[int, ...] = (1, 0, 1)) -> list[int]:
    writer = mc.BitWriter()
    writer.bit(0)
    writer.wint(static_reference, audit.STATIC_OBJECT_MAX)
    writer.wint(0, 20)
    for _ in range(3):
        writer.wint(2, 4)
    writer.bits.extend(tail)
    return list(writer.bits)


def _actor_open_bunch(
    channel: int, static_reference: int, *, sequence: int = 1, close: bool = False,
) -> dict[str, object]:
    return {
        "bControl": 1,
        "bOpen": 1,
        "bClose": int(close),
        "bReliable": 1,
        "chIndex": channel,
        "chType": 2,
        "chSeq": sequence,
        "bits_payload": _actor_open_bits(static_reference),
    }


def _actor_close_bunch(channel: int, *, sequence: int = 2) -> dict[str, object]:
    return {
        "bControl": 1,
        "bOpen": 0,
        "bClose": 1,
        "bReliable": 1,
        "chIndex": channel,
        "chType": 2,
        "chSeq": sequence,
        "payload": b"",
    }


def _frame(
    number: int,
    packet_id: int,
    bunches: list[dict[str, object]],
    *,
    direction: str = "C2S",
    client: str = CLIENT,
    client_port: int = CLIENT_PORT,
    drop_count: int = 0,
    raw: bytes | None = None,
) -> audit.CaptureFrame:
    if direction == "C2S":
        source, source_port = client, client_port
        destination, destination_port = SERVER, SERVER_PORT
        maximum = 1280
    else:
        source, source_port = SERVER, SERVER_PORT
        destination, destination_port = client, client_port
        maximum = 1500
    datagram = raw if raw is not None else mc.encode_packet(packet_id, bunches, maximum)
    return audit.CaptureFrame(
        number,
        number / 10.0,
        source,
        source_port,
        destination,
        destination_port,
        datagram,
        drop_count,
    )


def _candidate_frames(
    *, start_frame: int = 1, client: str = CLIENT, client_port: int = CLIENT_PORT,
) -> list[audit.CaptureFrame]:
    return [
        _frame(start_frame, 10, [_uses_bunch()], client=client, client_port=client_port),
        _frame(start_frame + 1, 11, [_role_bunch()], client=client, client_port=client_port),
    ]


def _base_manifest(pcap: Path, *, captured: int = 1) -> dict[str, object]:
    payload = pcap.read_bytes()
    return {
        "schema": "rs2v.realserver.capture-manifest.v1",
        "capture_id": "SECRET_CAPTURE_TOKEN",
        "operator_token": "SECRET_OPERATOR_TOKEN",
        "scenario": "South Machine Gunner deploy capture",
        "started_utc": "2026-07-15T00:00:00Z",
        "completed_utc": "2026-07-15T00:00:01Z",
        "endpoint": {
            "server_address": SERVER,
            "server_port": SERVER_PORT,
            "capture_filter": f"udp and host {SERVER} and port {SERVER_PORT}",
        },
        "adapter": {
            "name": "SECRET_ADAPTER",
            "description": "SECRET_DESCRIPTION",
            "interface_guid": "SECRET_GUID",
        },
        "route_validation": {
            "validated": True,
            "destination": SERVER,
            "source_address": CLIENT,
        },
        "limits": {
            "duration_seconds": 60,
            "filesize_kib": 1024,
            "promiscuous_mode": False,
        },
        "dumpcap": {
            "path": r"C:\private\dumpcap.exe",
            "version": "Dumpcap 4.6.6",
            "sha256": "A" * 64,
            "sha256_after_capture": "A" * 64,
            "hash_stable": True,
            "exit_code": 0,
        },
        "structural_validation": {
            "tool": "capinfos",
            "path": r"C:\private\capinfos.exe",
            "version": "Capinfos 4.6.6",
            "sha256": "B" * 64,
            "sha256_after_validation": "B" * 64,
            "hash_stable": True,
            "exit_code": 0,
            "structurally_valid": True,
        },
        "capture": {
            "path": str(pcap.resolve()),
            "byte_count": len(payload),
            "sha256": hashlib.sha256(payload).hexdigest().upper(),
        },
        "packet_statistics": {
            "captured": captured,
            "received": captured,
            "dropped": 0,
            "parse_complete": True,
        },
    }


def _write_manifest(path: Path, value: dict[str, object]) -> None:
    path.write_text(json.dumps(value), encoding="utf-8")


def _string_leaves(value: object) -> list[str]:
    if isinstance(value, str):
        return [value]
    if isinstance(value, dict):
        result: list[str] = []
        for key, item in value.items():
            result.extend(_string_leaves(key))
            result.extend(_string_leaves(item))
        return result
    if isinstance(value, (list, tuple)):
        result = []
        for item in value:
            result.extend(_string_leaves(item))
        return result
    return []


def _assert_private_strings_absent(
    testcase: unittest.TestCase, value: object, secrets: tuple[str, ...],
) -> None:
    leaves = _string_leaves(value)
    rendered = json.dumps(value)
    for secret in secrets:
        with testcase.subTest(secret=secret):
            for leaf in leaves:
                testcase.assertNotIn(secret, leaf)
            testcase.assertNotIn(secret, rendered)
            testcase.assertNotIn(json.dumps(secret)[1:-1], rendered)


class _FakeProcess:
    def __init__(
        self, stdout: object, *, ignore_terminate: bool = False,
        unblock: threading.Event | None = None,
    ) -> None:
        self.stdout = stdout
        self.returncode: int | None = None
        self.ignore_terminate = ignore_terminate
        self.unblock = unblock
        self.terminate_calls = 0
        self.kill_calls = 0
        self.wait_timeouts: list[float | None] = []

    def poll(self) -> int | None:
        return self.returncode

    def terminate(self) -> None:
        self.terminate_calls += 1
        if not self.ignore_terminate:
            self.returncode = -15
            if self.unblock is not None:
                self.unblock.set()

    def kill(self) -> None:
        self.kill_calls += 1
        self.returncode = -9
        if self.unblock is not None:
            self.unblock.set()

    def wait(self, timeout: float | None = None) -> int:
        self.wait_timeouts.append(timeout)
        if self.returncode is None:
            raise audit.subprocess.TimeoutExpired("tshark", timeout)
        return self.returncode


class H175CandidateTests(unittest.TestCase):
    def test_constructed_payload_is_only_an_ungrounded_exact_candidate(self) -> None:
        decoded = audit.decode_h175(
            bytes.fromhex(audit.CONSTRUCTED_H175_HEX), audit.CONSTRUCTED_H175_BITS
        )

        self.assertTrue(decoded["matchesConstructedCandidate"])
        self.assertTrue(decoded["sourceCandidateRequestShapeMatch"])
        self.assertFalse(decoded["roleSemanticsResolved"])
        self.assertFalse(decoded["packageMapObjectBaseResolved"])

    def test_truncated_or_wrong_layout_is_rejected(self) -> None:
        payload = bytes.fromhex(audit.CONSTRUCTED_H175_HEX)
        changed = bytearray(payload)
        changed[7] ^= 1
        for candidate, bits in ((payload, 56), (bytes(changed), 57), (payload, 65), (b"", 0)):
            with self.subTest(bits=bits, candidate=candidate.hex()):
                with self.assertRaises(audit.EvidenceError):
                    audit.decode_h175(candidate, bits)


class ManifestTests(unittest.TestCase):
    def test_accepts_capture_script_manifest_shape(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            pcap = root / "capture.pcapng"
            manifest_path = root / "capture.pcapng.manifest.json"
            pcap.write_bytes(b"capture-evidence")
            _write_manifest(manifest_path, _base_manifest(pcap, captured=7))

            manifest = audit.load_capture_manifest(manifest_path, pcap, SERVER_PORT)

            self.assertEqual(manifest.captured_packets, 7)
            self.assertEqual(manifest.dropped_packets, 0)
            self.assertEqual(manifest.server_address, SERVER)
            self.assertEqual(manifest.server_port, SERVER_PORT)

    def test_received_packet_count_is_optional_but_consistent_when_present(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            pcap = root / "capture.pcapng"
            manifest_path = root / "capture.pcapng.manifest.json"
            pcap.write_bytes(b"capture-evidence")
            base = _base_manifest(pcap, captured=7)

            absent = copy.deepcopy(base)
            del absent["packet_statistics"]["received"]  # type: ignore[index]
            _write_manifest(manifest_path, absent)
            self.assertEqual(
                audit.load_capture_manifest(
                    manifest_path, pcap, SERVER_PORT
                ).captured_packets,
                7,
            )

            for received in (None, True, -1, 6, 8, 7.0, "7"):
                with self.subTest(received=received):
                    inconsistent = copy.deepcopy(base)
                    inconsistent["packet_statistics"]["received"] = received  # type: ignore[index]
                    _write_manifest(manifest_path, inconsistent)
                    with self.assertRaises(audit.EvidenceError):
                        audit.load_capture_manifest(manifest_path, pcap, SERVER_PORT)

    def test_rejects_provenance_drift_and_unknown_drop_state(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            pcap = root / "capture.pcapng"
            manifest_path = root / "capture.pcapng.manifest.json"
            pcap.write_bytes(b"capture-evidence")
            base = _base_manifest(pcap)
            cases: tuple[tuple[str, tuple[str, ...], object], ...] = (
                ("capture bytes", ("capture", "byte_count"), 999),
                ("capture hash", ("capture", "sha256"), "C" * 64),
                ("drops unknown", ("packet_statistics", "dropped"), None),
                ("drops nonzero", ("packet_statistics", "dropped"), 1),
                ("stats incomplete", ("packet_statistics", "parse_complete"), False),
                ("structural false", ("structural_validation", "structurally_valid"), False),
                ("tool drift", ("dumpcap", "sha256_after_capture"), "D" * 64),
                ("wrong BPF", ("endpoint", "capture_filter"), "udp port 7777"),
                ("wrong route", ("route_validation", "destination"), CLIENT),
                ("promiscuous", ("limits", "promiscuous_mode"), True),
                ("wrong path", ("capture", "path"), str(root / "other.pcapng")),
            )
            for label, path, replacement in cases:
                with self.subTest(label=label):
                    value = copy.deepcopy(base)
                    target = value
                    for key in path[:-1]:
                        target = target[key]  # type: ignore[index,assignment]
                    target[path[-1]] = replacement  # type: ignore[index]
                    _write_manifest(manifest_path, value)
                    with self.assertRaises(audit.EvidenceError):
                        audit.load_capture_manifest(manifest_path, pcap, SERVER_PORT)

    def test_rejects_duplicate_json_keys(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            manifest_path = Path(temporary) / "capture.manifest.json"
            manifest_path.write_text('{"schema":"x","schema":"y"}', encoding="utf-8")
            with self.assertRaisesRegex(audit.EvidenceError, "duplicate key"):
                audit.load_capture_manifest(manifest_path)


class TransportAndLifecycleTests(unittest.TestCase):
    def test_exact_packet_duplicate_does_not_create_a_second_epoch(self) -> None:
        bunch = {
            "bControl": 1,
            "bOpen": 1,
            "bClose": 0,
            "bReliable": 1,
            "chIndex": 0,
            "chType": 1,
            "chSeq": 1,
            "payload": b"\x1d\x01",
        }
        datagram = mc.encode_packet(25, [bunch], 1280)
        records = audit.analyze_frames([
            _frame(1, 25, [], raw=datagram),
            _frame(2, 25, [], raw=datagram),
        ], server_address=SERVER)
        summary = records[-1]

        self.assertEqual(summary["sessions"], 1)
        self.assertEqual(summary["identityBarriers"], 0)
        self.assertTrue(summary["captureStructurallyComplete"])
        self.assertFalse(summary["complete"])

    def test_sequential_nonduplicate_connection_open_starts_new_epoch(self) -> None:
        first = {
            "bControl": 1, "bOpen": 1, "bClose": 0, "bReliable": 1,
            "chIndex": 0, "chType": 1, "chSeq": 1, "payload": b"first",
        }
        second = dict(first, chSeq=2, payload=b"second")
        analyzer = audit.CaptureAnalyzer(SERVER_PORT, server_address=SERVER)
        analyzer.observe(_frame(1, 20, [first]))
        analyzer.observe(_frame(2, 21, [second]))
        summary = analyzer.finish()[-1]

        self.assertEqual(summary["sessions"], 2)
        self.assertEqual([item.connection_epoch for item in analyzer.sessions], [1, 2])
        self.assertTrue(summary["captureStructurallyComplete"])

    def test_reliable_open_in_new_packet_is_deduped_before_epoch_reset(self) -> None:
        opening = {
            "bControl": 1, "bOpen": 1, "bClose": 0, "bReliable": 1,
            "chIndex": 0, "chType": 1, "chSeq": 1, "payload": b"open",
        }
        records = audit.analyze_frames([
            _frame(1, 20, [opening]),
            _frame(2, 21, [opening]),
        ], server_address=SERVER)

        self.assertEqual(records[-1]["sessions"], 1)
        self.assertEqual(records[-1]["reliableRetransmissionsDeduplicated"], 1)
        self.assertTrue(records[-1]["captureStructurallyComplete"])

        gap = audit.analyze_frames([
            _frame(1, 20, [opening]),
            _frame(2, 22, [opening]),
        ], server_address=SERVER)[-1]
        self.assertEqual(gap["sessions"], 1)
        self.assertFalse(gap["captureStructurallyComplete"])
        self.assertEqual(gap["identityBarriers"], 1)

    def test_reliable_connection_open_close_retransmission_stays_in_one_epoch(self) -> None:
        opening = {
            "bControl": 1, "bOpen": 1, "bClose": 1, "bReliable": 1,
            "chIndex": 0, "chType": 1, "chSeq": 1, "payload": b"open-close",
        }
        analyzer = audit.CaptureAnalyzer(SERVER_PORT, server_address=SERVER)
        analyzer.observe(_frame(1, 20, [opening]))
        self.assertNotIn(("C2S", 0), analyzer.sessions[0].active_lifetimes)
        analyzer.observe(_frame(2, 21, [opening]))
        summary = analyzer.finish()[-1]

        self.assertEqual(summary["sessions"], 1)
        self.assertEqual(summary["reliableRetransmissionsDeduplicated"], 1)
        self.assertEqual(summary["identityBarriers"], 0)
        self.assertTrue(summary["captureStructurallyComplete"])

    def test_duplicate_frame_drop_metadata_is_not_bypassed_by_dedup(self) -> None:
        datagram = mc.encode_packet(25, [], 1280)
        summary = audit.analyze_frames([
            _frame(1, 25, [], raw=datagram),
            _frame(2, 25, [], raw=datagram, drop_count=5),
        ], server_address=SERVER)[-1]

        self.assertEqual(summary["captureReportedDrops"], 5)
        self.assertEqual(summary["identityBarriers"], 1)
        self.assertFalse(summary["captureStructurallyComplete"])

    def test_packet_gap_invalidates_but_packet_id_wrap_is_contiguous(self) -> None:
        empty_1 = _frame(1, 100, [])
        empty_2 = _frame(2, 102, [])
        gap = audit.analyze_frames([empty_1, empty_2], server_address=SERVER)[-1]
        self.assertFalse(gap["captureStructurallyComplete"])
        self.assertEqual(gap["identityBarriers"], 1)

        wrap = audit.analyze_frames([
            _frame(1, audit.PACKET_ID_MAX - 1, []),
            _frame(2, 0, []),
        ], server_address=SERVER)[-1]
        self.assertTrue(wrap["captureStructurallyComplete"])
        self.assertEqual(wrap["identityBarriers"], 0)

    def test_drop_metadata_and_malformed_packet_fail_closed(self) -> None:
        dropped = audit.analyze_frames([
            _frame(1, 1, [], drop_count=2),
        ], server_address=SERVER)[-1]
        self.assertFalse(dropped["captureStructurallyComplete"])
        self.assertEqual(dropped["captureReportedDrops"], 2)

        malformed = audit.analyze_frames([
            _frame(1, 1, [], raw=b"\0"),
        ], server_address=SERVER)[-1]
        self.assertFalse(malformed["captureStructurallyComplete"])
        self.assertEqual(malformed["packetDecodeErrors"], 1)

    def test_transport_barriers_share_the_bounded_event_sink(self) -> None:
        analyzer = audit.CaptureAnalyzer(
            SERVER_PORT, max_events=1, server_address=SERVER
        )
        analyzer.observe(_frame(1, 1, [], raw=b"\0"))
        self.assertEqual(len(analyzer.events), 1)

        with self.assertRaisesRegex(audit.EvidenceError, "max-events"):
            analyzer.observe(_frame(2, 2, [], raw=b"\0"))
        self.assertEqual(len(analyzer.events), 1)

    def test_256_item_decoder_guard_cannot_partially_accept_packet(self) -> None:
        guarded = mc.encode_packet(1, [], 1280, acks=list(range(257)))
        decoded = mc.decode_packet(guarded, bd_max=audit.C2S_BUNCH_BITS)
        self.assertTrue(decoded["ok"])
        self.assertLess(decoded["endpos"], decoded["termbit"])

        summary = audit.analyze_frames([
            _frame(1, 1, [], raw=guarded),
        ], server_address=SERVER)[-1]
        self.assertFalse(summary["captureStructurallyComplete"])
        self.assertEqual(summary["packetDecodeErrors"], 1)
        self.assertEqual(summary["identityBarriers"], 1)

    def test_reliable_retransmission_deduplicates_and_conflict_invalidates(self) -> None:
        bunch = _uses_bunch()
        retransmitted = audit.analyze_frames([
            _frame(1, 1, [bunch]),
            _frame(2, 2, [bunch]),
        ], server_address=SERVER)[-1]
        self.assertTrue(retransmitted["captureStructurallyComplete"])
        self.assertEqual(retransmitted["reliableRetransmissionsDeduplicated"], 1)

        conflicting = copy.deepcopy(bunch)
        conflicting["bControl"] = 1
        conflicting["bClose"] = 1
        conflict = audit.analyze_frames([
            _frame(1, 1, [bunch]),
            _frame(2, 2, [conflicting]),
        ], server_address=SERVER)[-1]
        self.assertFalse(conflict["captureStructurallyComplete"])
        self.assertEqual(conflict["identityBarriers"], 1)

    def test_channel_close_and_reopen_uses_new_direction_local_generation(self) -> None:
        channel = 40
        records = audit.analyze_frames([
            _frame(1, 1, [_uses_bunch()], direction="S2C"),
            _frame(2, 2, [_actor_open_bunch(channel, 286461)], direction="S2C"),
            _frame(3, 3, [_actor_close_bunch(channel)], direction="S2C"),
            _frame(4, 4, [_actor_open_bunch(channel, 328703)], direction="S2C"),
        ], server_address=SERVER)
        opens = [record for record in records if record.get("record") == "opaqueActorOpen"]

        self.assertEqual(
            [record["actor"]["channelGeneration"] for record in opens],  # type: ignore[index]
            [1, 2],
        )
        self.assertTrue(records[-1]["captureStructurallyComplete"])
        rendered = json.dumps(records)
        self.assertNotIn("M60", rendered)
        self.assertNotIn("M61", rendered)
        self.assertNotIn("machineGun", rendered)

    def test_actor_open_close_retires_actor_and_retransmits_same_lifetime(self) -> None:
        channel = 42
        closed_open = _actor_open_bunch(
            channel, 286461, sequence=1, close=True
        )
        analyzer = audit.CaptureAnalyzer(SERVER_PORT, server_address=SERVER)
        analyzer.observe(_frame(1, 1, [_uses_bunch()], direction="S2C"))
        analyzer.observe(_frame(2, 2, [closed_open], direction="S2C"))
        session = analyzer.sessions[0]
        self.assertNotIn(channel, session.actors)
        self.assertNotIn(("S2C", channel), session.active_lifetimes)
        self.assertEqual(session.channel_generations[("S2C", channel)], 1)

        analyzer.observe(_frame(3, 3, [closed_open], direction="S2C"))
        self.assertNotIn(channel, session.actors)
        self.assertEqual(session.retransmissions, 1)

        analyzer.observe(_frame(
            4, 4, [_actor_open_bunch(channel, 328703)], direction="S2C"
        ))
        records = analyzer.finish()
        opens = [record for record in records if record.get("record") == "opaqueActorOpen"]
        self.assertEqual(
            [record["actor"]["channelGeneration"] for record in opens],  # type: ignore[index]
            [1, 2],
        )
        self.assertIn(channel, session.actors)
        self.assertEqual(records[-1]["identityBarriers"], 0)
        self.assertTrue(records[-1]["captureStructurallyComplete"])

    def test_preartifact_close_retires_lifetime_before_channel_reuse(self) -> None:
        channel = 41
        analyzer = audit.CaptureAnalyzer(SERVER_PORT, server_address=SERVER)
        analyzer.observe(_frame(
            1, 1, [_actor_open_bunch(channel, 286461)], direction="S2C"
        ))
        analyzer.observe(_frame(
            2, 2, [_actor_close_bunch(channel)], direction="S2C"
        ))
        analyzer.observe(_frame(
            3, 3, [_actor_open_bunch(channel, 328703)], direction="S2C"
        ))
        summary = analyzer.finish()[-1]

        self.assertEqual(analyzer.sessions[0].channel_generations[("S2C", channel)], 2)
        self.assertEqual(summary["identityBarriers"], 0)
        self.assertTrue(summary["captureStructurallyComplete"])

    def test_reliable_sequence_wrap_replaces_stale_cycle_fingerprint(self) -> None:
        frames: list[audit.CaptureFrame] = []
        for ordinal in range(audit.CHANNEL_MAX):
            bunch = {
                "bControl": 0, "bOpen": 0, "bClose": 0, "bReliable": 1,
                "chIndex": 9, "chType": 2, "chSeq": ordinal,
                "payload": b"old" if ordinal == 0 else b"x",
            }
            frames.append(_frame(ordinal + 1, ordinal, [bunch]))
        wrapped = {
            "bControl": 0, "bOpen": 0, "bClose": 0, "bReliable": 1,
            "chIndex": 9, "chType": 2, "chSeq": 0, "payload": b"new-cycle",
        }
        frames.append(_frame(audit.CHANNEL_MAX + 1, audit.CHANNEL_MAX, [wrapped]))

        summary = audit.analyze_frames(frames, server_address=SERVER)[-1]
        self.assertEqual(summary["identityBarriers"], 0)
        self.assertTrue(summary["captureStructurallyComplete"])


class CandidateAuditTests(unittest.TestCase):
    def test_candidate_requires_direction_local_uses_epoch(self) -> None:
        records = audit.analyze_frames([
            _frame(1, 1, [_uses_bunch()], direction="C2S"),
            _frame(2, 1, [_actor_open_bunch(40, 286461)], direction="S2C"),
        ], server_address=SERVER)

        self.assertFalse(any(record.get("record") == "opaqueActorOpen" for record in records))
        self.assertEqual(records[-1]["preArtifactReferenceBunchesIgnored"], 1)

    def test_wrong_target_identity_order_never_recovers_candidate_epoch(self) -> None:
        wrong = (audit.PACKAGE_ORDER[1],)
        records = audit.analyze_frames([
            _frame(1, 1, [_uses_bunch(specs=wrong)]),
            _frame(2, 2, [_uses_bunch(sequence=2)]),
            _frame(3, 3, [_role_bunch()]),
        ], server_address=SERVER)

        self.assertFalse(any(
            record.get("record") == "packageIdentityCandidateEpoch" for record in records
        ))
        self.assertFalse(any(
            record.get("record") == "sourceCandidateRoleSelection" for record in records
        ))
        self.assertEqual(records[-1]["sourceCandidateSessions"], 0)

    def test_full_uses_stream_is_emitted_without_objectbase_claims(self) -> None:
        records = audit.analyze_frames([
            _frame(1, 1, [_uses_bunch()]),
        ], server_address=SERVER)
        identities = [record for record in records if record.get("record") == "nmtUsesIdentity"]
        epochs = [
            record for record in records
            if record.get("record") == "packageIdentityCandidateEpoch"
        ]

        self.assertEqual([record["package"] for record in identities], [
            "ROGame", "ROGameContent", "VNTE-CuChi"
        ])
        self.assertEqual(len(epochs), 1)
        self.assertFalse(epochs[0]["fullPackageMapOrderGrounded"])
        self.assertFalse(epochs[0]["generationObjectCountsGrounded"])
        self.assertFalse(epochs[0]["objectBaseResolved"])
        self.assertFalse(records[-1]["packageMapGrounded"])
        self.assertTrue(records[-1]["candidateIdentityComplete"])
        self.assertFalse(records[-1]["candidateApplicationComplete"])

    def test_unknown_package_names_are_replaced_by_stable_private_labels(self) -> None:
        private_name = rf"C:\private\captures\TOKEN-{CLIENT}"
        private_extension = rf"endpoint-{SERVER}\SECRET_EXTENSION"
        unknown = audit.PackageSpec(
            private_name, private_extension, "11" * 16, 0x12345678, 9
        )
        records = audit.analyze_frames([
            _frame(1, 1, [_uses_bunch(specs=(unknown,))]),
        ], server_address=SERVER)
        identity = next(
            record for record in records if record.get("record") == "nmtUsesIdentity"
        )

        self.assertFalse(identity["packageIdentityKnown"])
        self.assertNotIn("package", identity)
        self.assertNotIn("extension", identity)
        self.assertEqual(
            identity["packageNameSha256"],
            hashlib.sha256(private_name.encode("utf-8")).hexdigest().upper(),
        )
        self.assertEqual(
            identity["extensionSha256"],
            hashlib.sha256(private_extension.encode("utf-8")).hexdigest().upper(),
        )
        _assert_private_strings_absent(
            self, records,
            (private_name, private_extension, CLIENT, SERVER, "TOKEN", "SECRET_EXTENSION"),
        )

    def test_exact_h175_is_observed_without_semantic_promotion(self) -> None:
        records = audit.analyze_frames(_candidate_frames(), server_address=SERVER)
        selections = [
            record for record in records
            if record.get("record") == "sourceCandidateRoleSelection"
        ]

        self.assertEqual(len(selections), 1)
        self.assertTrue(selections[0]["captureObserved"])
        self.assertFalse(selections[0]["semanticRoleResolved"])
        self.assertFalse(selections[0]["owningGraphEligible"])
        summary = records[-1]
        self.assertTrue(summary["captureStructurallyComplete"])
        self.assertTrue(summary["candidateIdentityComplete"])
        self.assertTrue(summary["candidateApplicationComplete"])
        self.assertFalse(summary["candidateRecordsGrounded"])
        self.assertFalse(summary["packageMapGrounded"])
        self.assertFalse(summary["owningGraphProven"])
        self.assertFalse(summary["runtimeAuthorizes"])
        self.assertFalse(summary["runtimeAuthorized"])
        self.assertFalse(summary["complete"])
        self.assertEqual(
            summary["completeMeaning"],
            "owning acceptance intentionally unavailable; see captureStructurallyComplete",
        )

    def test_truncated_or_unsupported_h175_is_application_incomplete_not_transport_invalid(self) -> None:
        exact_bits = mc.packed_bits(
            audit.CONSTRUCTED_H175_HEX, audit.CONSTRUCTED_H175_BITS
        )
        variants = {
            "truncated-following-rpc": exact_bits[:49],
            "unsupported-following-rpc": (
                exact_bits[:48] + mc.sint_bits(450, audit.ROPC_MAX_HANDLE)
            ),
        }
        for label, bits in variants.items():
            with self.subTest(label=label):
                records = audit.analyze_frames([
                    _frame(1, 10, [_uses_bunch()]),
                    _frame(2, 11, [_role_bunch(bits=bits)]),
                ], server_address=SERVER)
                summary = records[-1]

                self.assertTrue(summary["captureStructurallyComplete"])
                self.assertEqual(summary["identityBarriers"], 0)
                self.assertTrue(summary["candidateIdentityComplete"])
                self.assertFalse(summary["candidateApplicationComplete"])
                self.assertEqual(summary["candidateDiagnostics"], 1)
                self.assertEqual(summary["partialTypedPayloads"], 1)
                self.assertFalse(any(
                    record.get("record") == "identityBarrier" for record in records
                ))
                diagnostics = [
                    record for record in records
                    if record.get("record") == "candidateDiagnostic"
                ]
                self.assertEqual(len(diagnostics), 1)
                self.assertEqual(diagnostics[0]["scope"], "application")
                self.assertFalse(diagnostics[0]["captureTransportInvalidated"])

    def test_second_nonretransmitted_h175_retires_candidate_not_transport(self) -> None:
        records = audit.analyze_frames(
            _candidate_frames() + [
                _frame(3, 12, [_role_bunch(sequence=2)]),
            ],
            server_address=SERVER,
        )
        summary = records[-1]

        self.assertTrue(summary["captureStructurallyComplete"])
        self.assertEqual(summary["identityBarriers"], 0)
        self.assertTrue(summary["candidateIdentityComplete"])
        self.assertFalse(summary["candidateApplicationComplete"])
        self.assertEqual(summary["sourceCandidateSessions"], 0)
        self.assertEqual(summary["candidateDiagnostics"], 1)
        self.assertEqual(len([
            record for record in records
            if record.get("record") == "sourceCandidateRoleSelection"
        ]), 2)
        diagnostic = next(
            record for record in records if record.get("record") == "candidateDiagnostic"
        )
        self.assertTrue(diagnostic["candidateAssociationRetired"])
        self.assertFalse(any(
            record.get("record") == "identityBarrier" for record in records
        ))

    def test_malformed_s2c_known_rpc_is_application_diagnostic_only(self) -> None:
        records = audit.analyze_frames(
            _candidate_frames() + [
                _frame(3, 20, [_uses_bunch()], direction="S2C"),
                _frame(4, 21, [_role_bunch(bits=[])], direction="S2C"),
            ],
            server_address=SERVER,
        )
        summary = records[-1]

        self.assertTrue(summary["captureStructurallyComplete"])
        self.assertEqual(summary["identityBarriers"], 0)
        self.assertTrue(summary["candidateIdentityComplete"])
        self.assertFalse(summary["candidateApplicationComplete"])
        self.assertEqual(summary["candidateDiagnostics"], 1)

    def test_opaque_actor_delta_is_application_diagnostic_only(self) -> None:
        channel = 43
        opaque_delta = {
            "bControl": 0, "bOpen": 0, "bClose": 0, "bReliable": 1,
            "chIndex": channel, "chType": 2, "chSeq": 2, "payload": b"",
        }
        records = audit.analyze_frames(
            _candidate_frames() + [
                _frame(3, 20, [_uses_bunch()], direction="S2C"),
                _frame(
                    4, 21, [_actor_open_bunch(channel, 286461)], direction="S2C"
                ),
                _frame(5, 22, [opaque_delta], direction="S2C"),
            ],
            server_address=SERVER,
        )
        summary = records[-1]

        self.assertTrue(summary["captureStructurallyComplete"])
        self.assertEqual(summary["identityBarriers"], 0)
        self.assertTrue(summary["candidateIdentityComplete"])
        self.assertFalse(summary["candidateApplicationComplete"])
        self.assertEqual(summary["candidateDiagnostics"], 1)

    def test_malformed_uses_payload_is_identity_diagnostic_not_transport_invalid(self) -> None:
        malformed = {
            "bControl": 0, "bOpen": 0, "bClose": 0, "bReliable": 1,
            "chIndex": 0, "chType": 1, "chSeq": 1,
            "payload": bytes((audit.NMT_USES, 1)),
        }
        records = audit.analyze_frames([
            _frame(1, 1, [malformed]),
        ], server_address=SERVER)
        summary = records[-1]

        self.assertTrue(summary["captureStructurallyComplete"])
        self.assertEqual(summary["identityBarriers"], 0)
        self.assertFalse(summary["candidateIdentityComplete"])
        self.assertFalse(summary["candidateApplicationComplete"])
        self.assertEqual(summary["candidateDiagnostics"], 1)

    def test_candidate_diagnostics_share_the_bounded_event_sink(self) -> None:
        def malformed(sequence: int) -> dict[str, object]:
            return {
                "bControl": 0, "bOpen": 0, "bClose": 0, "bReliable": 1,
                "chIndex": 0, "chType": 1, "chSeq": sequence,
                "payload": bytes((audit.NMT_USES, 1)),
            }

        analyzer = audit.CaptureAnalyzer(
            SERVER_PORT, max_events=1, server_address=SERVER
        )
        analyzer.observe(_frame(1, 1, [malformed(1)]))
        self.assertEqual(len(analyzer.events), 1)

        with self.assertRaisesRegex(audit.EvidenceError, "max-events"):
            analyzer.observe(_frame(2, 2, [malformed(2)]))
        self.assertEqual(len(analyzer.events), 1)

    def test_target_package_contradiction_only_invalidates_candidate_association(self) -> None:
        expected = audit.PACKAGE_ORDER[1]
        contradiction = audit.PackageSpec(
            expected.name, expected.extension, "22" * 16,
            expected.flags, expected.generation,
        )
        records = audit.analyze_frames(
            _candidate_frames() + [
                _frame(3, 12, [_uses_bunch(sequence=2, specs=(contradiction,))]),
            ],
            server_address=SERVER,
        )
        summary = records[-1]

        self.assertTrue(summary["captureStructurallyComplete"])
        self.assertEqual(summary["identityBarriers"], 0)
        self.assertEqual(summary["candidateAssociationInvalidations"], 1)
        self.assertFalse(summary["candidateIdentityComplete"])
        self.assertFalse(summary["candidateApplicationComplete"])
        self.assertEqual(summary["sourceCandidateSessions"], 0)
        invalidations = [
            record for record in records
            if record.get("record") == "candidateAssociationInvalidated"
        ]
        self.assertEqual(len(invalidations), 1)
        self.assertFalse(invalidations[0]["captureTransportInvalidated"])
        self.assertFalse(summary["packageMapGrounded"])
        self.assertFalse(summary["owningGraphProven"])
        self.assertFalse(summary["runtimeAuthorizes"])
        self.assertFalse(summary["complete"])

    def test_two_flows_are_explicitly_ambiguous(self) -> None:
        records = audit.analyze_frames(
            _candidate_frames(start_frame=1) +
            _candidate_frames(start_frame=3, client="198.51.100.7", client_port=52000),
            server_address=SERVER,
        )
        summary = records[-1]

        self.assertEqual(summary["sessions"], 2)
        self.assertEqual(summary["sourceCandidateSessions"], 2)
        self.assertTrue(summary["ambiguousSourceCandidateSessions"])
        self.assertFalse(summary["candidateRecordsGrounded"])

    def test_partial_rpc_tail_is_reported_but_not_applied_as_complete_context(self) -> None:
        partial_spawn = (
            mc.sint_bits(audit.SET_SPAWN_HANDLE, audit.ROPC_MAX_HANDLE) +
            [1] + [(128 >> bit) & 1 for bit in range(8)] + [1]
        )
        records = audit.analyze_frames(
            _candidate_frames() + [
                _frame(3, 12, [_role_bunch(sequence=2, bits=partial_spawn)]),
            ],
            server_address=SERVER,
        )
        deployments = [
            record for record in records if record.get("record") == "deploymentRequest"
        ]

        self.assertEqual(len(deployments), 1)
        self.assertEqual(deployments[0]["decoded"]["decodeState"], "partial-unknown-tail")  # type: ignore[index]
        self.assertEqual(records[-1]["partialTypedPayloads"], 1)
        self.assertTrue(records[-1]["captureStructurallyComplete"])
        self.assertTrue(records[-1]["candidateIdentityComplete"])
        self.assertFalse(records[-1]["candidateApplicationComplete"])
        self.assertEqual(records[-1]["candidateDiagnostics"], 1)
        self.assertFalse(records[-1]["candidateRecordsGrounded"])


class ExtractionBoundaryTests(unittest.TestCase):
    @staticmethod
    def _main_args(
        pcap: Path, manifest: Path, tshark: Path, output: Path,
    ) -> list[str]:
        return [
            "--pcap", str(pcap),
            "--manifest", str(manifest),
            "--tshark", str(tshark),
            "--expected-bytes", "1",
            "--expected-sha256", "A" * 64,
            "--output", str(output),
            "--overwrite",
        ]

    def test_tshark_identity_requires_exact_hash_size_and_version(self) -> None:
        completed = SimpleNamespace(
            returncode=0, stdout=audit.PINNED_TSHARK_VERSION + "\nextra\n", stderr=""
        )
        with mock.patch.object(audit, "validate_file_identity") as validate, mock.patch.object(
            audit.subprocess, "run", return_value=completed
        ):
            audit.validate_tshark_identity(Path("tshark.exe"))
        validate.assert_called_once_with(
            Path("tshark.exe"), audit.PINNED_TSHARK_BYTES,
            audit.PINNED_TSHARK_SHA256, "tshark",
        )

        bad = SimpleNamespace(returncode=0, stdout="TShark 4.6.5\n", stderr="")
        with mock.patch.object(audit, "validate_file_identity"), mock.patch.object(
            audit.subprocess, "run", return_value=bad
        ), self.assertRaises(audit.EvidenceError):
            audit.validate_tshark_identity(Path("tshark.exe"))

    def test_tshark_parse_failure_terminates_waits_and_kills_without_stderr_pipe(self) -> None:
        process = _FakeProcess(io.StringIO("malformed-row\n"), ignore_terminate=True)
        with mock.patch.object(audit.subprocess, "Popen", return_value=process) as popen:
            with self.assertRaisesRegex(audit.EvidenceError, "field count"):
                list(audit.iter_tshark_frames(
                    Path("tshark.exe"), Path("capture.pcapng"), SERVER, SERVER_PORT
                ))

        self.assertGreaterEqual(process.terminate_calls, 1)
        self.assertEqual(process.kill_calls, 1)
        self.assertTrue(process.wait_timeouts)
        self.assertTrue(all(
            timeout == audit.TSHARK_PROCESS_WAIT_SECONDS
            for timeout in process.wait_timeouts
        ))
        self.assertEqual(popen.call_args.kwargs["stderr"], audit.subprocess.DEVNULL)

    def test_tshark_consumer_failure_closes_and_reaps_streaming_child(self) -> None:
        row = (
            f"1\t0.1\t{CLIENT}\t{CLIENT_PORT}\t{SERVER}\t{SERVER_PORT}\t00\t0\n"
        )
        process = _FakeProcess(io.StringIO(row))
        with mock.patch.object(
            audit.subprocess, "Popen", return_value=process
        ), mock.patch.object(
            audit.CaptureAnalyzer, "observe", side_effect=RuntimeError("consumer failed")
        ):
            with self.assertRaisesRegex(RuntimeError, "consumer failed"):
                audit.analyze_frames(
                    audit.iter_tshark_frames(
                        Path("tshark.exe"), Path("capture.pcapng"), SERVER, SERVER_PORT
                    ),
                    server_address=SERVER,
                )

        self.assertGreaterEqual(process.terminate_calls, 1)
        self.assertEqual(process.kill_calls, 0)
        self.assertTrue(process.wait_timeouts)

    def test_tshark_stream_has_a_total_deadline(self) -> None:
        unblock = threading.Event()

        class BlockingStdout:
            def __iter__(self) -> object:
                return self

            def __next__(self) -> str:
                unblock.wait(1)
                raise StopIteration

            def close(self) -> None:
                unblock.set()

        process = _FakeProcess(BlockingStdout(), unblock=unblock)
        with mock.patch.object(
            audit.subprocess, "Popen", return_value=process
        ), mock.patch.object(
            audit, "TSHARK_STREAM_TIMEOUT_SECONDS", 0.01
        ), mock.patch.object(
            audit, "TSHARK_PROCESS_WAIT_SECONDS", 0.01
        ):
            with self.assertRaisesRegex(audit.EvidenceError, "timed out"):
                list(audit.iter_tshark_frames(
                    Path("tshark.exe"), Path("capture.pcapng"), SERVER, SERVER_PORT
                ))

        self.assertGreaterEqual(process.terminate_calls, 1)
        self.assertTrue(unblock.is_set())

    def test_output_must_not_directly_or_by_samefile_alias_pinned_tshark(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            pcap = root / "capture.pcapng"
            manifest = root / "capture.pcapng.manifest.json"
            tshark = root / "tshark.exe"
            samefile_output = root / "samefile-output.jsonl"
            pcap.write_bytes(b"p")
            manifest.write_text("{}", encoding="utf-8")
            tshark.write_bytes(b"pinned-tshark")
            os.link(tshark, samefile_output)

            for label, output in (
                ("direct", tshark),
                ("samefile", samefile_output),
            ):
                with self.subTest(label=label), mock.patch.object(
                    audit, "extract_capture"
                ) as extract, mock.patch.object(audit, "write_report") as write:
                    self.assertEqual(
                        audit.main(self._main_args(pcap, manifest, tshark, output)),
                        2,
                    )
                    extract.assert_not_called()
                    write.assert_not_called()
            with self.assertRaisesRegex(audit.EvidenceError, "tshark"):
                audit.write_report(
                    ({"record": "summary", "complete": False},),
                    samefile_output,
                    overwrite=True,
                    protected_inputs=(tshark,),
                )
            self.assertEqual(tshark.read_bytes(), b"pinned-tshark")

    def test_final_write_rechecks_output_alias_after_extraction(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            pcap = root / "capture.pcapng"
            manifest = root / "capture.pcapng.manifest.json"
            tshark = root / "tshark.exe"
            output = root / "evidence.jsonl"
            pcap.write_bytes(b"p")
            manifest.write_text("{}", encoding="utf-8")
            tshark.write_bytes(b"pinned-tshark")

            def alias_before_write(*_args: object) -> tuple[dict[str, object], ...]:
                os.link(tshark, output)
                return ({
                    "record": "summary",
                    "complete": False,
                    "packageMapGrounded": False,
                    "owningGraphProven": False,
                    "runtimeAuthorized": False,
                },)

            with mock.patch.object(
                audit, "extract_capture", side_effect=alias_before_write
            ) as extract, mock.patch.object(audit, "write_report") as write:
                self.assertEqual(
                    audit.main(self._main_args(pcap, manifest, tshark, output)),
                    2,
                )
                extract.assert_called_once()
                write.assert_not_called()
            self.assertEqual(tshark.read_bytes(), b"pinned-tshark")

    def test_raced_final_component_alias_is_replaced_without_following_target(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            pcap = root / "capture.pcapng"
            manifest = root / "capture.pcapng.manifest.json"
            tshark = root / "tshark.exe"
            output = root / "evidence.jsonl"
            pcap.write_bytes(b"p")
            manifest.write_text("{}", encoding="utf-8")
            protected_bytes = b"pinned-tshark-must-not-change"
            tshark.write_bytes(protected_bytes)
            report = ({
                "record": "summary",
                "complete": False,
                "packageMapGrounded": False,
                "owningGraphProven": False,
                "runtimeAuthorizes": False,
                "runtimeAuthorized": False,
            },)
            real_validate = audit._validate_report_destination
            validation_calls = 0

            def validate_then_race(
                destination: Path | None, protected: object,
            ) -> None:
                nonlocal validation_calls
                real_validate(destination, protected)  # type: ignore[arg-type]
                validation_calls += 1
                if validation_calls == 3:
                    try:
                        output.symlink_to(tshark)
                    except (NotImplementedError, OSError):
                        if os.path.lexists(output):
                            output.unlink()
                        os.link(tshark, output)

            with mock.patch.object(
                audit, "extract_capture", return_value=report
            ), mock.patch.object(
                audit, "_validate_report_destination", side_effect=validate_then_race
            ):
                result = audit.main(
                    self._main_args(pcap, manifest, tshark, output)
                )

            self.assertEqual(validation_calls, 3)
            self.assertIn(result, (2, 67))
            self.assertEqual(tshark.read_bytes(), protected_bytes)
            if result == 67:
                self.assertEqual(json.loads(output.read_text(encoding="utf-8")), report[0])
                self.assertNotIn(b"\r", output.read_bytes())
                self.assertTrue(output.read_bytes().endswith(b"\n"))
            self.assertEqual(list(root.glob(output.name + ".*.tmp")), [])

    def test_extract_rechecks_immutable_pcap_after_streaming(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            pcap = root / "capture.pcapng"
            manifest_path = root / "capture.pcapng.manifest.json"
            tshark = root / "tshark.exe"
            pcap.write_bytes(b"pcap-data")
            tshark.write_bytes(b"fake")
            _write_manifest(manifest_path, _base_manifest(pcap))
            size = pcap.stat().st_size
            sha256 = hashlib.sha256(pcap.read_bytes()).hexdigest().upper()

            def mutating_frames(*_args: object) -> object:
                yield _frame(1, 1, [])
                pcap.write_bytes(b"changed-after-yield")

            with mock.patch.object(audit, "validate_tshark_identity"), mock.patch.object(
                audit, "iter_tshark_frames", side_effect=mutating_frames
            ), self.assertRaisesRegex(audit.EvidenceError, "identity"):
                audit.extract_capture(
                    pcap, manifest_path, tshark, size, sha256,
                    SERVER_PORT, 16, audit.MAX_RELEVANT_EVENTS,
                )

    def test_extract_output_omits_endpoint_adapter_and_local_paths(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            pcap = root / "capture.pcapng"
            manifest_path = root / "capture.pcapng.manifest.json"
            tshark = root / "private-tshark.exe"
            pcap.write_bytes(b"pcap-data")
            tshark.write_bytes(b"fake")
            _write_manifest(manifest_path, _base_manifest(pcap))
            size = pcap.stat().st_size
            sha256 = hashlib.sha256(pcap.read_bytes()).hexdigest().upper()

            with mock.patch.object(audit, "validate_tshark_identity"), mock.patch.object(
                audit, "iter_tshark_frames", return_value=iter([_frame(1, 1, [_uses_bunch()])])
            ):
                records = audit.extract_capture(
                    pcap, manifest_path, tshark, size, sha256,
                    SERVER_PORT, 16, audit.MAX_RELEVANT_EVENTS,
                )
            _assert_private_strings_absent(self, records, (
                SERVER, CLIENT, "SECRET_ADAPTER", "SECRET_DESCRIPTION", "SECRET_GUID",
                str(pcap), str(manifest_path), str(tshark), r"C:\private\dumpcap.exe",
                r"C:\private\capinfos.exe", "SECRET_CAPTURE_TOKEN",
                "SECRET_OPERATOR_TOKEN",
            ))
            self.assertFalse(records[0]["packageMapGrounded"])
            self.assertFalse(records[0]["candidateRecordsGrounded"])
            self.assertFalse(records[0]["owningGraphProven"])
            self.assertFalse(records[0]["runtimeAuthorizes"])

    def test_report_write_is_atomic_and_requires_explicit_overwrite(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            destination = Path(temporary) / "evidence.jsonl"
            destination.write_text("preserve-me\n", encoding="utf-8")
            records = ({"record": "summary", "complete": False},)

            with self.assertRaises(audit.EvidenceError):
                audit.write_report(records, destination, overwrite=False)
            self.assertEqual(destination.read_text(encoding="utf-8"), "preserve-me\n")

            audit.write_report(records, destination, overwrite=True)
            self.assertEqual(
                json.loads(destination.read_text(encoding="utf-8")), records[0]
            )
            self.assertNotIn(b"\r", destination.read_bytes())
            self.assertTrue(destination.read_bytes().endswith(b"\n"))
            self.assertEqual(list(destination.parent.glob(destination.name + ".*.tmp")), [])

            stdout = io.StringIO()
            with mock.patch.object(audit.sys, "stdout", stdout):
                audit.write_report(records, None, overwrite=False)
            self.assertEqual(
                stdout.getvalue(),
                json.dumps(records[0], sort_keys=True, separators=(",", ":")) + "\n",
            )


if __name__ == "__main__":
    unittest.main()
