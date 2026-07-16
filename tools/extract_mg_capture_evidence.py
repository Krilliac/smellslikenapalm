#!/usr/bin/env python3
"""Create a sanitized structural/candidate audit of an RS2 deployment pcap.

The auditor validates capture provenance and transport structure, preserves the
full direction-local NMT_Uses identity streams, and records opaque candidates
for the constructed h175 request and post-Uses actor opens.  It deliberately
does not assign role, weapon, actor, or owning-graph semantics: the capture does
not contain the complete pinned PackageMap generation rows and ObjectBase/CDO
NetIndex resolver required to make those assignments.  C2S and S2C package
streams are never merged, and every report remains runtime-authorizing false.
"""

from __future__ import annotations

import argparse
import hashlib
import ipaddress
import json
import os
import queue
import subprocess
import sys
import tempfile
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Iterable, Iterator, Mapping, Optional, Sequence


TOOLS_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS_DIR))
import audit_installed_packagemap as package_audit  # noqa: E402
from audit_installed_role_refs import (  # noqa: E402
    AuditError as AtomicWriteError,
    file_identity,
    validate_file_identity,
)
import mock_client as mc  # noqa: E402


SCHEMA = "rs2.owning-mg-capture-evidence.v1"
NMT_USES = 0x07
C2S_BUNCH_BITS = 10240
S2C_BUNCH_BITS = 12000
STATIC_OBJECT_MAX = 0x80000000
CHANNEL_MAX = 1024
PACKET_ID_MAX = 16384
MAX_RELEVANT_EVENTS = 20000
TSHARK_STREAM_TIMEOUT_SECONDS = 120.0
TSHARK_PROCESS_WAIT_SECONDS = 2.0
MAX_REPORT_BYTES = 256 * 1024 * 1024
REPORT_SPOOL_BYTES = 8 * 1024 * 1024
PINNED_TSHARK_BYTES = 623784
PINNED_TSHARK_SHA256 = "908A3B04DA69EE45BE9BD54627A722741D895262B4CE0B39F6D79A03DAA24087"
PINNED_TSHARK_VERSION = "TShark (Wireshark) 4.6.6 (v4.6.6-0-g3a22c3ef473d)."

ROPC_CHANNEL = 2
ROPC_MAX_HANDLE = 531
PAWN_MAX_HANDLE = 168
PRI_MAX_HANDLE = 98

SELECT_ROLE_HANDLE = 175
CHANGED_ROLE_HANDLE = 210
SET_SPAWN_HANDLE = 261
READY_HANDLE = 434
AUTO_SELECT_SQUAD_HANDLE = 451

SOURCE_CANDIDATE_ROLE_REF = 87497
TARGET_CLASS_INDEX = 2
CONSTRUCTED_H175_BITS = 57
CONSTRUCTED_H175_HEX = "af965c150080c301"

# The h175 reference above is retained only because the constructed payload is
# an explicit input candidate. Actor-open numbers are not assigned any semantic
# names until a full pinned PackageMap resolver exists.


class EvidenceError(ValueError):
    """The capture or a target wire structure failed a bounded validation."""


@dataclass(frozen=True)
class CaptureManifest:
    capture_bytes: int
    capture_sha256: str
    dropped_packets: int
    captured_packets: int
    server_address: str
    server_port: int


@dataclass(frozen=True)
class PackageSpec:
    name: str
    extension: str
    raw_guid_hex: str
    flags: int
    generation: int

    @property
    def key(self) -> tuple[str, str]:
        return self.name.casefold(), self.extension.casefold()


PACKAGE_ORDER = (
    PackageSpec(
        "ROGame", "u", "8DCCA6169F4A6C44B388B6D582CC0D21", 0x20204001, 2
    ),
    PackageSpec(
        "ROGameContent", "u", "0DEC063695F9EF49B1D7E7E07B70FC15",
        0x20204001, 2,
    ),
    PackageSpec(
        "VNTE-CuChi", "roe", "E545E11BA9547B45823296412B012ED6",
        0x20024001, 2,
    ),
)
PACKAGE_BY_KEY = {spec.key: spec for spec in PACKAGE_ORDER}


@dataclass(frozen=True)
class CaptureFrame:
    frame: int
    time_seconds: float
    source: str
    source_port: int
    destination: str
    destination_port: int
    datagram: bytes
    drop_count: int = 0


@dataclass(frozen=True)
class ActorIdentity:
    connection: tuple[str, int, str, int]
    connection_epoch: int
    map_epoch: int
    channel: int
    generation: int
    class_ref: int
    kind: str
    open_frame: int


@dataclass(frozen=True)
class PacketContext:
    frame: int
    time_seconds: float
    direction: str
    packet_id: int
    channel: int
    channel_generation: int
    channel_sequence: int
    payload_bits: int
    payload_sha256: str


@dataclass(frozen=True)
class DynamicRef:
    bit_offset: int
    channel: int
    target: Optional[ActorIdentity]
    special: Optional[str] = None


@dataclass
class ActorGraphState:
    fields: dict[str, object] = field(default_factory=dict)
    contexts: dict[str, PacketContext] = field(default_factory=dict)
    refs: dict[str, DynamicRef] = field(default_factory=dict)
    graph_block_complete: bool = False


@dataclass
class Transaction:
    map_epoch: int
    selection: dict[str, object]
    selection_context: PacketContext
    spawn_context: Optional[PacketContext] = None
    ready_context: Optional[PacketContext] = None
    changed_role_context: Optional[PacketContext] = None
    completed_graph: Optional[dict[str, object]] = None


@dataclass
class DirectionalArtifacts:
    direction: str
    ordinal: int = 0
    map_epoch: int = 0
    next_required: int = 0
    current_records: list[dict[str, object]] = field(default_factory=list)
    ready_epochs: dict[int, tuple[dict[str, object], ...]] = field(default_factory=dict)
    incompatible: bool = False
    contradictions: int = 0
    stream_digest: object = field(default_factory=hashlib.sha256)

    def observe(self, record: package_audit.UsesRecord, frame: int) -> Optional[int]:
        self.ordinal += 1
        digest_line = (
            f"{self.ordinal}\0{record.name}\0{record.extension}\0"
            f"{record.guid.hex().upper()}\0{record.package_flags:08X}\0"
            f"{record.generation}\n"
        ).encode("utf-8")
        self.stream_digest.update(digest_line)  # type: ignore[attr-defined]
        key = record.key
        spec = PACKAGE_BY_KEY.get(key)
        if spec is None:
            return None
        if self.incompatible:
            # Preserve the complete identity stream, but never recover an epoch
            # after a target-package contradiction inside the same connection.
            return None

        actual = {
            "package": spec.name,
            "extension": spec.extension,
            "rawGuid": record.guid.hex().upper(),
            "packageFlags": f"0x{record.package_flags:08X}",
            "generation": record.generation,
            "usesOrdinal": self.ordinal,
            "frame": frame,
        }
        expected = PACKAGE_ORDER[self.next_required] if self.next_required < len(PACKAGE_ORDER) else None
        exact = (
            expected is not None and key == expected.key and
            actual["rawGuid"] == expected.raw_guid_hex and
            record.package_flags == expected.flags and
            record.generation == expected.generation
        )
        if not exact:
            # A new exact ROGame begins a fresh map epoch candidate.  Any other
            # target-package mismatch is a hard provenance barrier.
            first = PACKAGE_ORDER[0]
            if (
                key == first.key and actual["rawGuid"] == first.raw_guid_hex and
                record.package_flags == first.flags and
                record.generation == first.generation
            ):
                self.next_required = 1
                self.current_records = [actual]
                return None
            self.incompatible = True
            self.contradictions += 1
            self.next_required = 0
            self.current_records.clear()
            return None

        self.current_records.append(actual)
        self.next_required += 1
        if self.next_required != len(PACKAGE_ORDER):
            return None
        self.map_epoch += 1
        self.ready_epochs[self.map_epoch] = tuple(self.current_records)
        self.next_required = 0
        self.current_records = []
        return self.map_epoch

    def ready(self, epoch: int) -> bool:
        return epoch in self.ready_epochs and not self.incompatible

    def digest_hex(self) -> str:
        return self.stream_digest.copy().hexdigest().upper()  # type: ignore[attr-defined]


@dataclass
class Session:
    connection: tuple[str, int, str, int]
    connection_epoch: int
    session_ordinal: int
    emit_event: Callable[[dict[str, object]], None]
    artifacts: dict[str, DirectionalArtifacts] = field(default_factory=dict)
    channel_generations: dict[tuple[str, int], int] = field(default_factory=dict)
    active_lifetimes: set[tuple[str, int]] = field(default_factory=set)
    actors: dict[int, ActorIdentity] = field(default_factory=dict)
    graph: dict[ActorIdentity, ActorGraphState] = field(default_factory=dict)
    last_packet: dict[str, tuple[int, str]] = field(default_factory=dict)
    reliable_seen: dict[
        tuple[str, int, int, int], tuple[object, ...]
    ] = field(default_factory=dict)
    reliable_last: dict[tuple[str, int, int], int] = field(default_factory=dict)
    invalid_reasons: list[str] = field(default_factory=list)
    transaction: Optional[Transaction] = None
    target_selection_count: int = 0
    retransmissions: int = 0
    partial_payloads: int = 0
    preartifact_refs: int = 0
    candidate_invalid_reasons: list[str] = field(default_factory=list)
    candidate_diagnostics: list[str] = field(default_factory=list)
    candidate_identity_incomplete_directions: set[str] = field(default_factory=set)
    candidate_application_incomplete: bool = False

    def __post_init__(self) -> None:
        self.artifacts = {
            "C2S": DirectionalArtifacts("C2S"),
            "S2C": DirectionalArtifacts("S2C"),
        }

    def invalidate(self, reason: str, frame: int) -> None:
        rendered = f"frame {frame}: {reason}"
        if rendered not in self.invalid_reasons:
            self.invalid_reasons.append(rendered)
            self.emit_event({
                "record": "identityBarrier",
                "session": _connection_json(self.session_ordinal),
                "connectionEpoch": self.connection_epoch,
                "frame": frame,
                "reason": reason,
                "invalidatedActorCount": len(self.actors),
                "runtimeAuthorized": False,
            })
        self.actors.clear()
        self.active_lifetimes.clear()

    def artifact_epoch_for(self, direction: str) -> int:
        if direction in self.candidate_identity_incomplete_directions:
            return 0
        tracker = self.artifacts[direction]
        return tracker.map_epoch if tracker.ready(tracker.map_epoch) else 0

    def record_candidate_diagnostic(
        self, scope: str, direction: str, reason: str, frame: int,
        *, retire_association: bool = False,
    ) -> None:
        """Record application uncertainty without damaging transport evidence."""
        if scope not in ("identity", "application"):
            raise ValueError("candidate diagnostic scope must be identity or application")
        if scope == "identity" and direction == "C2S":
            retire_association = True
        association_retired = retire_association and self.transaction is not None
        rendered = f"frame {frame}: {scope}: {reason}"
        if rendered not in self.candidate_diagnostics:
            self.candidate_diagnostics.append(rendered)
            self.emit_event({
                "record": "candidateDiagnostic",
                "session": _connection_json(self.session_ordinal),
                "connectionEpoch": self.connection_epoch,
                "frame": frame,
                "direction": direction,
                "scope": scope,
                "reason": reason,
                "candidateAssociationRetired": association_retired,
                "captureTransportInvalidated": False,
                "runtimeAuthorized": False,
            })
        if scope == "identity":
            self.candidate_identity_incomplete_directions.add(direction)
        else:
            self.candidate_application_incomplete = True
        if retire_association:
            self.transaction = None

    def invalidate_candidate_association(self, direction: str, frame: int) -> None:
        """Retire candidate state without treating valid transport as malformed."""
        if direction != "C2S" or self.transaction is None:
            return
        reason = "target package identity contradicted the selected candidate epoch"
        rendered = f"frame {frame}: {reason}"
        if rendered not in self.candidate_invalid_reasons:
            self.candidate_invalid_reasons.append(rendered)
            self.emit_event({
                "record": "candidateAssociationInvalidated",
                "session": _connection_json(self.session_ordinal),
                "connectionEpoch": self.connection_epoch,
                "frame": frame,
                "direction": direction,
                "usesOrdinal": self.artifacts[direction].ordinal,
                "reason": reason,
                "captureTransportInvalidated": False,
                "runtimeAuthorized": False,
            })
        self.transaction = None
        self.candidate_application_incomplete = True


def _connection_json(session_ordinal: int) -> dict[str, object]:
    """Return a capture-local identifier; never serialize the tracked 4-tuple."""
    return {"sessionOrdinal": session_ordinal, "fourTupleTrackedInternally": True}


def _actor_json(actor: ActorIdentity) -> dict[str, object]:
    return {
        "channel": actor.channel,
        "channelGeneration": actor.generation,
        "staticReference": actor.class_ref,
        "referenceSemanticsResolved": False,
        "kind": actor.kind,
        "openFrame": actor.open_frame,
        "mapEpoch": actor.map_epoch,
    }


def _context_json(context: PacketContext) -> dict[str, object]:
    return {
        "frame": context.frame,
        "timeSeconds": context.time_seconds,
        "direction": context.direction,
        "packetId": context.packet_id,
        "channel": context.channel,
        "channelGeneration": context.channel_generation,
        "channelSequence": context.channel_sequence,
        "payloadBits": context.payload_bits,
        "payloadSha256": context.payload_sha256,
    }


def _ref_json(ref: DynamicRef) -> dict[str, object]:
    result: dict[str, object] = {
        "kind": "dynamic",
        "bitOffset": ref.bit_offset,
        "channel": ref.channel,
    }
    if ref.target is not None:
        result["targetChannelGeneration"] = ref.target.generation
        result["targetArchetypeCdoStaticReference"] = ref.target.class_ref
    if ref.special is not None:
        result["specialIdentity"] = ref.special
    return result


def _payload_sha256(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest().upper()


def _capture_string_sha256(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8")).hexdigest().upper()


def _uses_public_identity(
    record: package_audit.UsesRecord,
) -> dict[str, object]:
    """Return canonical known names or irreversible labels for unknown names."""
    spec = PACKAGE_BY_KEY.get(record.key)
    if spec is not None:
        return {
            "packageIdentityKnown": True,
            "package": spec.name,
            "extension": spec.extension,
        }
    return {
        "packageIdentityKnown": False,
        "packageNameSha256": _capture_string_sha256(record.name),
        "extensionSha256": _capture_string_sha256(record.extension),
    }


def _bunch_payload_and_fingerprint(
    bunch: Mapping[str, object],
) -> tuple[bytes, tuple[object, ...]]:
    try:
        payload = bytes.fromhex(str(bunch.get("payloadHex", "")))
    except ValueError as exc:
        raise EvidenceError("bunch has invalid payload hex") from exc
    payload_bits = int(bunch.get("bits", 0))
    if payload_bits < 0 or payload_bits > len(payload) * 8:
        raise EvidenceError("bunch payload bit range is outside its buffer")
    return payload, (
        bool(bunch.get("bControl")), bool(bunch.get("bOpen")),
        bool(bunch.get("bClose")), bool(bunch.get("bReliable")),
        int(bunch.get("chType", 0)), payload_bits, _payload_sha256(payload),
    )


def validate_tshark_identity(tshark: Path) -> None:
    try:
        validate_file_identity(
            tshark, PINNED_TSHARK_BYTES, PINNED_TSHARK_SHA256, "tshark"
        )
        completed = subprocess.run(
            (str(tshark), "--version"), check=False, capture_output=True,
            text=True, encoding="utf-8", errors="replace", timeout=10,
        )
    except (AtomicWriteError, OSError, subprocess.TimeoutExpired) as exc:
        raise EvidenceError("pinned tshark identity validation failed") from exc
    first_line = (completed.stdout or completed.stderr).splitlines()
    if (completed.returncode != 0 or not first_line or
            first_line[0].strip() != PINNED_TSHARK_VERSION):
        raise EvidenceError("pinned tshark version validation failed")


def load_capture_manifest(
    path: Path, pcap: Optional[Path] = None, server_port: Optional[int] = None,
) -> CaptureManifest:
    """Load the bounded privacy-sensitive sidecar without returning local data."""
    if not path.is_file() or path.is_symlink():
        raise EvidenceError("capture manifest is missing, not regular, or is a symlink")
    try:
        size = path.stat().st_size
        if size <= 0 or size > 65536:
            raise EvidenceError("capture manifest size is outside (0,65536]")

        def no_duplicates(pairs: list[tuple[str, object]]) -> dict[str, object]:
            result: dict[str, object] = {}
            for key, value in pairs:
                if key in result:
                    raise EvidenceError("capture manifest contains a duplicate key")
                result[key] = value
            return result

        raw = path.read_text(encoding="utf-8")
        value = json.loads(raw, object_pairs_hook=no_duplicates)
    except EvidenceError:
        raise
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise EvidenceError("capture manifest is unreadable or malformed") from exc
    if (not isinstance(value, Mapping) or
            value.get("schema") != "rs2v.realserver.capture-manifest.v1"):
        raise EvidenceError(
            "capture manifest schema must be rs2v.realserver.capture-manifest.v1"
        )
    scenario = value.get("scenario")
    if not isinstance(scenario, str) or not all(
        token in scenario.casefold() for token in ("south", "machine", "gunner", "deploy")
    ):
        raise EvidenceError("capture manifest scenario is not the target South machine-gunner deployment")
    capture = value.get("capture")
    packets = value.get("packet_statistics")
    dumpcap = value.get("dumpcap")
    structural = value.get("structural_validation")
    endpoint = value.get("endpoint")
    if not all(isinstance(item, Mapping) for item in (
        capture, packets, dumpcap, structural, endpoint
    )):
        raise EvidenceError("capture manifest is missing required provenance objects")
    assert isinstance(capture, Mapping)
    assert isinstance(packets, Mapping)
    assert isinstance(dumpcap, Mapping)
    assert isinstance(structural, Mapping)
    assert isinstance(endpoint, Mapping)
    server_address = endpoint.get("server_address")
    manifest_port = endpoint.get("server_port")
    capture_filter = endpoint.get("capture_filter")
    if (not isinstance(server_address, str) or not server_address or
            any(ord(ch) < 0x20 or ord(ch) == 0x7F for ch in server_address)):
        raise EvidenceError("capture manifest server address is malformed")
    try:
        canonical_server_address = str(ipaddress.IPv4Address(server_address))
    except ipaddress.AddressValueError as exc:
        raise EvidenceError("capture manifest server address is not canonical IPv4") from exc
    if canonical_server_address != server_address:
        raise EvidenceError("capture manifest server IPv4 address is not canonical")
    if (isinstance(manifest_port, bool) or not isinstance(manifest_port, int) or
            not 1 <= manifest_port <= 65535):
        raise EvidenceError("capture manifest server port is malformed")
    if capture_filter != f"udp and host {server_address} and port {manifest_port}":
        raise EvidenceError("capture manifest BPF is not the exact endpoint-specific filter")
    route = value.get("route_validation")
    limits = value.get("limits")
    if (not isinstance(route, Mapping) or route.get("validated") is not True or
            route.get("destination") != server_address):
        raise EvidenceError("capture manifest route validation is not exact")
    if (not isinstance(limits, Mapping) or
            limits.get("promiscuous_mode") is not False):
        raise EvidenceError("capture manifest must prove nonpromiscuous capture")
    duration = limits.get("duration_seconds")
    filesize = limits.get("filesize_kib")
    if (isinstance(duration, bool) or not isinstance(duration, int) or
            not 1 <= duration <= 3600 or isinstance(filesize, bool) or
            not isinstance(filesize, int) or not 64 <= filesize <= 2097152):
        raise EvidenceError("capture manifest duration/filesize bounds are invalid")
    capture_bytes = capture.get("byte_count")
    capture_sha = capture.get("sha256")
    if (isinstance(capture_bytes, bool) or not isinstance(capture_bytes, int) or
            capture_bytes <= 0):
        raise EvidenceError("capture manifest captureBytes must be a positive integer")
    if (not isinstance(capture_sha, str) or len(capture_sha) != 64 or
            any(ch not in "0123456789abcdefABCDEF" for ch in capture_sha)):
        raise EvidenceError("capture manifest captureSha256 must be 64 hexadecimal digits")
    captured = packets.get("captured")
    dropped = packets.get("dropped")
    received_missing = object()
    received = packets.get("received", received_missing)
    if packets.get("parse_complete") is not True:
        raise EvidenceError("capture manifest packet/drop statistics are incomplete")
    if isinstance(captured, bool) or not isinstance(captured, int) or captured <= 0:
        raise EvidenceError("capture manifest captured packet count must be positive")
    if isinstance(dropped, bool) or not isinstance(dropped, int) or dropped < 0:
        raise EvidenceError("capture manifest droppedPackets must be a nonnegative integer")
    if received is not received_missing:
        if (isinstance(received, bool) or not isinstance(received, int) or
                received < 0):
            raise EvidenceError(
                "capture manifest received packet count must be a nonnegative integer"
            )
        if received != captured + dropped:
            raise EvidenceError(
                "capture manifest received packet count is inconsistent with captured+dropped"
            )
    if dropped != 0:
        raise EvidenceError(f"capture manifest reports {dropped} dropped packet(s)")
    if (structural.get("tool") != "capinfos" or
            structural.get("structurally_valid") is not True or
            structural.get("hash_stable") is not True or
            structural.get("exit_code") != 0):
        raise EvidenceError("capture manifest capinfos structural validation is not clean")
    for label, tool, after_key in (
        ("dumpcap", dumpcap, "sha256_after_capture"),
        ("capinfos", structural, "sha256_after_validation"),
    ):
        before = tool.get("sha256")
        after = tool.get(after_key)
        if (tool.get("hash_stable") is not True or not isinstance(before, str) or
                not isinstance(after, str) or before.upper() != after.upper() or
                len(before) != 64 or
                any(ch not in "0123456789abcdefABCDEF" for ch in before + after)):
            raise EvidenceError(f"capture manifest {label} executable hash is not stable")
    if dumpcap.get("exit_code") != 0:
        raise EvidenceError("capture manifest dumpcap exit code is not zero")
    if server_port is not None and manifest_port != server_port:
        raise EvidenceError("capture manifest server port does not match extraction port")
    capture_path = capture.get("path")
    if pcap is not None:
        if not isinstance(capture_path, str) or not _paths_alias(pcap, Path(capture_path)):
            raise EvidenceError("capture manifest path does not identify the input pcap")
        try:
            actual_bytes, actual_sha256 = file_identity(pcap)
        except AtomicWriteError as exc:
            raise EvidenceError("input pcap identity could not be validated") from exc
        if actual_bytes != capture_bytes or actual_sha256 != capture_sha.upper():
            raise EvidenceError("capture manifest identity does not match the input pcap")
    return CaptureManifest(
        capture_bytes, capture_sha.upper(), dropped, captured,
        canonical_server_address, manifest_port,
    )


def _decode_object_ref(
    reader: mc.BitReader, session: Session, map_epoch: int,
) -> tuple[bool, int, int, Optional[ActorIdentity], Optional[str]]:
    offset = reader.p
    dynamic = bool(reader.bit())
    index = reader.rint(CHANNEL_MAX if dynamic else STATIC_OBJECT_MAX)
    if reader.error:
        raise EvidenceError("truncated object reference")
    if not dynamic:
        return False, index, offset, None, None
    if index == ROPC_CHANNEL:
        return True, index, offset, None, "owningPlayerController"
    target = session.actors.get(index)
    if target is not None and target.map_epoch != map_epoch:
        target = None
    return True, index, offset, target, None


def _decode_compressed_vector(reader: mc.BitReader) -> tuple[int, int, int]:
    bits = reader.rint(20)
    if reader.error or bits >= 20:
        raise EvidenceError("invalid compressed vector magnitude")
    bias = 1 << (bits + 1)
    maximum = 1 << (bits + 2)
    value = tuple(reader.rint(maximum) - bias for _ in range(3))
    if reader.error:
        raise EvidenceError("truncated compressed vector")
    return value  # type: ignore[return-value]


def decode_actor_open_prefix(payload: bytes, payload_bits: int) -> tuple[int, int]:
    if payload_bits <= 0 or payload_bits > len(payload) * 8:
        raise EvidenceError("actor open bit range is outside its buffer")
    reader = mc.BitReader(payload, payload_bits)
    dynamic = bool(reader.bit())
    class_ref = reader.rint(CHANNEL_MAX if dynamic else STATIC_OBJECT_MAX)
    if reader.error or dynamic or class_ref == 0:
        raise EvidenceError("actor open does not begin with a nonzero static reference")
    _decode_compressed_vector(reader)
    return class_ref, reader.p


def decode_h175(payload: bytes, payload_bits: int) -> dict[str, object]:
    """Decode one complete final h175+h451 payload without grounding by inference."""
    if payload_bits <= 0 or payload_bits > len(payload) * 8:
        raise EvidenceError("h175 bit range is outside its buffer")
    reader = mc.BitReader(payload, payload_bits)
    start = reader.p
    if reader.rint(ROPC_MAX_HANDLE) != SELECT_ROLE_HANDLE or reader.error:
        raise EvidenceError("payload does not begin with h175")
    south = bool(reader.bit())
    if not reader.bit():
        raise EvidenceError("h175 omits RoleInfoClass")
    ref_offset = reader.p
    dynamic = bool(reader.bit())
    role_ref = reader.rint(CHANNEL_MAX if dynamic else STATIC_OBJECT_MAX)
    if reader.error or dynamic or role_ref == 0:
        raise EvidenceError("h175 RoleInfoClass is not a nonzero static reference")
    weapon_present = bool(reader.bit())
    weapon: Optional[dict[str, int]] = None
    if weapon_present:
        weapon = {
            "primaryWeaponIndex": reader.ru(8),
            "primaryWeaponLevel": reader.ru(8),
            "primaryWeaponAmmo": reader.ru(8),
            "secondaryWeaponIndex": reader.ru(8),
            "secondaryWeaponLevel": reader.ru(8),
        }
    tank_present = bool(reader.bit())
    if tank_present:
        _decode_object_ref_unscoped(reader)
    allow_team_tank = bool(reader.bit())
    desired_context = bool(reader.bit())
    close_menu = bool(reader.bit())
    consumed_h175 = reader.p - start
    following = None
    if reader.p < reader.n:
        following_handle = reader.rint(ROPC_MAX_HANDLE)
        if reader.error or following_handle != AUTO_SELECT_SQUAD_HANDLE:
            raise EvidenceError("h175 has an unsupported following RPC layout")
        following = "h451.ServerAutoSelectSquad"
    if reader.error or reader.p != reader.n:
        raise EvidenceError("h175 payload has truncation or trailing bits")
    structural_candidate = (
        south and role_ref == SOURCE_CANDIDATE_ROLE_REF and not weapon_present and
        not tank_present and not allow_team_tank and not desired_context and
        close_menu and following == "h451.ServerAutoSelectSquad"
    )
    payload_hex = payload.hex()
    return {
        "southDesired": south,
        "roleInfoStaticReference": {
            "kind": "static", "index": role_ref, "bitOffset": ref_offset,
        },
        "weaponSelectionPresent": weapon_present,
        "weaponSelection": weapon,
        "tankSelectionPresent": tank_present,
        "allowTeamTank": allow_team_tank,
        "desiredContext": desired_context,
        "closeMenu": close_menu,
        "h175ConsumedBits": consumed_h175,
        "followingRpc": following,
        "wholePayloadConsumed": True,
        "sourceCandidateStaticReferenceMatch": role_ref == SOURCE_CANDIDATE_ROLE_REF,
        "sourceCandidateRequestShapeMatch": structural_candidate,
        "roleSemanticsResolved": False,
        "packageMapObjectBaseResolved": False,
        "matchesConstructedCandidate": (
            payload_bits == CONSTRUCTED_H175_BITS and
            payload_hex.lower() == CONSTRUCTED_H175_HEX
        ),
        "candidateProvenance": "sourceConstructed-not-live-until-observed",
    }


def _decode_object_ref_unscoped(reader: mc.BitReader) -> tuple[bool, int, int]:
    offset = reader.p
    dynamic = bool(reader.bit())
    index = reader.rint(CHANNEL_MAX if dynamic else STATIC_OBJECT_MAX)
    if reader.error:
        raise EvidenceError("truncated object reference")
    return dynamic, index, offset


def _decode_single_rpc(payload: bytes, payload_bits: int) -> Optional[dict[str, object]]:
    reader = mc.BitReader(payload, payload_bits)
    handle = reader.rint(ROPC_MAX_HANDLE)
    if reader.error:
        raise EvidenceError("truncated PlayerController handle")
    result: dict[str, object] = {"handle": handle}
    if handle == SET_SPAWN_HANDLE:
        present = bool(reader.bit())
        result.update(kind="h261.ServerSetSpawnSelect", value=reader.ru(8) if present else 0)
    elif handle == READY_HANDLE:
        present = bool(reader.bit())
        result.update(kind="h434.ServerSetReadyToSpawn", value=reader.ru(2) if present else 0)
    elif handle == CHANGED_ROLE_HANDLE:
        squad_present = bool(reader.bit())
        squad = reader.ru(8) if squad_present else 0
        class_present = bool(reader.bit())
        class_index = reader.ru(8) if class_present else 0
        show_lobby = bool(reader.bit())
        show_spawn = bool(reader.bit())
        result.update(
            kind="h210.ChangedRole", squadIndex=squad, classIndex=class_index,
            showLobby=show_lobby, showSpawnSelect=show_spawn,
        )
    else:
        return None
    if reader.error:
        raise EvidenceError("truncated recognized PlayerController RPC")
    result.update(
        consumedBits=reader.p,
        payloadBits=payload_bits,
        decodeState="complete" if reader.p == reader.n else "partial-unknown-tail",
        unknownTailBits=reader.n - reader.p,
    )
    return result


def _decode_actor_delta(
    session: Session, actor: ActorIdentity, payload: bytes, payload_bits: int,
) -> tuple[dict[str, object], dict[str, object], dict[str, DynamicRef]]:
    if actor.kind == "pawn":
        max_handle = PAWN_MAX_HANDLE
        definitions = {27: ("inventoryManager", "object"),
                       32: ("playerReplicationInfo", "object"),
                       52: ("controller", "object"),
                       57: ("clientPossessed", "none")}
    elif actor.kind == "pri":
        max_handle = PRI_MAX_HANDLE
        definitions = {31: ("waitingPlayer", "bool"),
                       32: ("onlySpectator", "bool"),
                       33: ("isSpectator", "bool"),
                       79: ("classIndex", "byte"),
                       80: ("roleIndex", "byte"),
                       81: ("squadIndex", "byte")}
    else:
        if actor.kind == "inventoryManager":
            return ({
                "consumedBits": 0, "payloadBits": payload_bits,
                "decodeState": "partial-ungrounded-inventory-manager-schema",
                "unknownTailBits": payload_bits,
            }, {}, {})
        return _decode_weapon_delta(session, actor, payload, payload_bits)

    reader = mc.BitReader(payload, payload_bits)
    fields: dict[str, object] = {}
    refs: dict[str, DynamicRef] = {}
    while reader.p < reader.n and not reader.error:
        field_offset = reader.p
        handle = reader.rint(max_handle)
        definition = definitions.get(handle)
        if definition is None:
            reader.p = field_offset
            break
        name, value_type = definition
        if value_type == "object":
            dynamic, index, offset, target, special = _decode_object_ref(
                reader, session, actor.map_epoch
            )
            fields[name] = {"kind": "dynamic" if dynamic else "static", "index": index}
            if dynamic:
                refs[name] = DynamicRef(offset, index, target, special)
        elif value_type == "byte":
            fields[name] = reader.ru(8)
        elif value_type == "bool":
            fields[name] = bool(reader.bit())
        else:
            fields[name] = True
        if reader.error:
            raise EvidenceError(f"truncated {actor.kind} field {name}")
    detail = {
        "consumedBits": reader.p,
        "payloadBits": payload_bits,
        "decodeState": "complete" if reader.p == reader.n else "partial-unknown-tail",
        "unknownTailBits": reader.n - reader.p,
    }
    return detail, fields, refs


def _decode_weapon_delta(
    session: Session, actor: ActorIdentity, payload: bytes, payload_bits: int,
) -> tuple[dict[str, object], dict[str, object], dict[str, DynamicRef]]:
    return ({
        "consumedBits": 0, "payloadBits": payload_bits,
        "decodeState": "partial-unresolved-packagemap-and-class-schema",
        "unknownTailBits": payload_bits, "owningGraphEligible": False,
    }, {}, {})


def _actor_kind(class_ref: int) -> str:
    # ObjectBase/CDO NetIndex linkage is intentionally unresolved in this slice.
    return "unknownActor"


def _parse_uses_payload(payload: bytes, payload_bits: int) -> tuple[package_audit.UsesRecord, ...]:
    if payload_bits == 0 or payload_bits % 8 != 0 or payload_bits != len(payload) * 8:
        raise EvidenceError("NMT_Uses bunch is not an exact byte-aligned payload")
    records: list[package_audit.UsesRecord] = []
    cursor = 0
    while cursor < len(payload):
        if payload[cursor] != NMT_USES:
            raise EvidenceError("NMT_Uses bunch has an unknown trailing control layout")
        try:
            record, cursor = package_audit._parse_uses(
                payload, cursor, 0, 0
            )
        except package_audit.AuditError as exc:
            raise EvidenceError("malformed NMT_Uses record") from exc
        records.append(record)
    if not records:
        raise EvidenceError("empty NMT_Uses bunch")
    return tuple(records)


def _new_context(
    frame: CaptureFrame, direction: str, packet_id: int, bunch: Mapping[str, object],
    generation: int, payload: bytes,
) -> PacketContext:
    return PacketContext(
        frame.frame, frame.time_seconds, direction, packet_id,
        int(bunch.get("chIndex", 0)), generation,
        int(bunch.get("chSeq", 0)), int(bunch.get("bits", 0)),
        _payload_sha256(payload),
    )


def _dynamic_ref_is(ref: Optional[DynamicRef], actor: ActorIdentity) -> bool:
    return ref is not None and ref.target == actor


def _evaluate_graph(session: Session) -> Optional[dict[str, object]]:
    # GUID/order-only Uses observations cannot resolve ObjectBase or CDO
    # NetIndex. A later vertical must supply a pinned full PackageMap resolver
    # (including generation object-count rows) before any dynamic graph can be
    # semantically typed or completed.
    return None


class CaptureAnalyzer:
    def __init__(
        self, server_port: int, max_events: int = MAX_RELEVANT_EVENTS,
        server_address: str = "10.0.0.1",
    ) -> None:
        self.server_port = server_port
        self.server_address = str(ipaddress.IPv4Address(server_address))
        self.max_events = max_events
        self.events: list[dict[str, object]] = []
        self.sessions: list[Session] = []
        self.current: dict[tuple[str, int, str, int], Session] = {}
        self.connection_epochs: dict[tuple[str, int, str, int], int] = {}
        self.packet_errors = 0
        self.dumpcap_drops = 0
        self.frames_observed = 0

    def _emit(self, record: dict[str, object]) -> None:
        if len(self.events) >= self.max_events:
            raise EvidenceError("relevant evidence exceeds --max-events")
        self.events.append(record)

    def _coordinates(self, frame: CaptureFrame) -> tuple[str, tuple[str, int, str, int]]:
        if (frame.source == self.server_address and
                frame.source_port == self.server_port and
                frame.destination_port != self.server_port):
            return "S2C", (
                frame.destination, frame.destination_port, frame.source, frame.source_port,
            )
        if (frame.destination == self.server_address and
                frame.destination_port == self.server_port and
                frame.source_port != self.server_port):
            return "C2S", (
                frame.source, frame.source_port, frame.destination, frame.destination_port,
            )
        raise EvidenceError("UDP row does not have exactly one configured server-port endpoint")

    def _session(
        self, connection: tuple[str, int, str, int], new_connection: bool = False,
    ) -> Session:
        current = self.current.get(connection)
        if current is None or new_connection:
            epoch = self.connection_epochs.get(connection, 0) + 1
            self.connection_epochs[connection] = epoch
            current = Session(connection, epoch, len(self.sessions) + 1, self._emit)
            self.sessions.append(current)
            self.current[connection] = current
        return current

    def observe(self, frame: CaptureFrame) -> None:
        self.frames_observed += 1
        direction, connection = self._coordinates(frame)
        bd_max = C2S_BUNCH_BITS if direction == "C2S" else S2C_BUNCH_BITS
        decoded = mc.decode_packet(frame.datagram, bd_max=bd_max)
        session = self._session(connection)
        endpos = decoded.get("endpos")
        termbit = decoded.get("termbit")
        exactly_consumed = (
            isinstance(endpos, int) and not isinstance(endpos, bool) and
            isinstance(termbit, int) and not isinstance(termbit, bool) and
            0 <= termbit < len(frame.datagram) * 8 and endpos == termbit
        )
        if not decoded.get("ok") or not exactly_consumed:
            if frame.drop_count:
                self.dumpcap_drops += frame.drop_count
                session.invalidate(
                    f"capture metadata reports {frame.drop_count} dropped frame(s)",
                    frame.frame,
                )
            self.packet_errors += 1
            session.invalidate(
                f"{direction} packet was not consumed exactly to its terminator",
                frame.frame,
            )
            return

        bunches = list(decoded.get("bunches", []))
        pid = int(decoded.get("pid", 0))
        datagram_sha = _payload_sha256(frame.datagram)
        prior_packet = session.last_packet.get(direction)
        # Exact packet duplicates are retired before a control-open can create
        # a new epoch. This is the capture-file duplicate case, not merely a
        # reliable bunch retransmission in a fresh packet.
        if (prior_packet is not None and pid == prior_packet[0] and
                datagram_sha == prior_packet[1]):
            if frame.drop_count:
                self.dumpcap_drops += frame.drop_count
                session.invalidate(
                    f"capture metadata reports {frame.drop_count} dropped frame(s)",
                    frame.frame,
                )
            return

        connection_opens = [
            bunch for bunch in bunches
            if (
            direction == "C2S" and bunch.get("bOpen") and
            int(bunch.get("chIndex", -1)) == 0
            )
        ]
        starts_connection = bool(connection_opens)
        has_prior_epoch_state = (
            session.transaction is not None or session.artifacts["C2S"].ordinal or
            session.artifacts["S2C"].ordinal or session.actors or session.last_packet
        )
        connection_open_is_retransmission = starts_connection and all(
            self._is_current_open_retransmission(session, direction, bunch)
            for bunch in connection_opens
        )
        if (starts_connection and has_prior_epoch_state and
                not connection_open_is_retransmission):
            session = self._session(connection, new_connection=True)
            prior_packet = None
        if frame.drop_count:
            self.dumpcap_drops += frame.drop_count
            session.invalidate(
                f"capture metadata reports {frame.drop_count} dropped frame(s)",
                frame.frame,
            )

        if prior_packet is not None:
            prior_pid, prior_sha = prior_packet
            if pid != (prior_pid + 1) % PACKET_ID_MAX:
                session.invalidate(
                    f"{direction} PacketId gap {prior_pid}->{pid}", frame.frame
                )
                session.last_packet[direction] = (pid, datagram_sha)
                return
        session.last_packet[direction] = (pid, datagram_sha)

        for bunch in bunches:
            self._observe_bunch(session, frame, direction, pid, bunch)
        _evaluate_graph(session)

    @staticmethod
    def _is_current_open_retransmission(
        session: Session, direction: str, bunch: Mapping[str, object],
    ) -> bool:
        if not bunch.get("bReliable"):
            return False
        channel = int(bunch.get("chIndex", -1))
        lifetime_key = (direction, channel)
        generation = session.channel_generations.get(lifetime_key, 0)
        if generation <= 0:
            return False
        sequence = int(bunch.get("chSeq", 0))
        try:
            _, fingerprint = _bunch_payload_and_fingerprint(bunch)
        except (EvidenceError, TypeError, ValueError):
            return False
        return session.reliable_seen.get(
            (direction, channel, generation, sequence)
        ) == fingerprint

    def _observe_bunch(
        self, session: Session, frame: CaptureFrame, direction: str,
        packet_id: int, bunch: Mapping[str, object],
    ) -> None:
        channel = int(bunch.get("chIndex", 0))
        payload_bits = int(bunch.get("bits", 0))
        try:
            payload, fingerprint = _bunch_payload_and_fingerprint(bunch)
        except (EvidenceError, TypeError, ValueError):
            session.invalidate(f"{direction} bunch has invalid payload encoding", frame.frame)
            return
        lifetime_key = (direction, channel)
        current_generation = session.channel_generations.get(lifetime_key, 0)
        is_open = bool(bunch.get("bOpen"))
        lifetime_active = lifetime_key in session.active_lifetimes

        # An exact reliable open may be retransmitted after its original bunch
        # also closed the channel. Match that just-retired generation before an
        # inactive open is assigned a fresh lifetime. A differing open remains
        # eligible to create the next generation because channel sequence values
        # may restart on legitimate reuse.
        if bunch.get("bReliable") and is_open and not lifetime_active and current_generation:
            sequence = int(bunch.get("chSeq", 0))
            completed_key = (direction, channel, current_generation, sequence)
            if session.reliable_seen.get(completed_key) == fingerprint:
                session.retransmissions += 1
                return

        lifetime_generation = (
            current_generation
            if is_open and lifetime_active
            else current_generation + 1
            if is_open
            else current_generation
        )
        if bunch.get("bReliable"):
            sequence = int(bunch.get("chSeq", 0))
            key = (direction, channel, lifetime_generation, sequence)
            prior = session.reliable_seen.get(key)
            sequence_key = (direction, channel, lifetime_generation)
            previous_sequence = session.reliable_last.get(sequence_key)
            expected_advance = (
                previous_sequence is not None and
                sequence == (previous_sequence + 1) % CHANNEL_MAX
            )
            if prior is not None and not expected_advance:
                if prior == fingerprint:
                    session.retransmissions += 1
                    return
                session.invalidate(
                    f"conflicting reliable retransmission ch{channel} seq{sequence}",
                    frame.frame,
                )
                return
            if (previous_sequence is not None and
                    not expected_advance):
                session.invalidate(
                    f"{direction} reliable sequence gap ch{channel} "
                    f"{previous_sequence}->{sequence}", frame.frame,
                )
                return
            session.reliable_seen[key] = fingerprint
            session.reliable_last[sequence_key] = sequence

        if is_open:
            if lifetime_key in session.active_lifetimes:
                session.invalidate(
                    f"{direction} non-retransmitted open on active ch{channel}",
                    frame.frame,
                )
                return
            session.channel_generations[lifetime_key] = lifetime_generation
            session.active_lifetimes.add(lifetime_key)

        # Retire lifetimes even before PackageMap candidates exist. Otherwise a
        # pre-artifact close poisons a legitimate reuse of the same channel.
        if bunch.get("bClose"):
            if direction == "S2C":
                session.actors.pop(channel, None)
            session.active_lifetimes.discard(lifetime_key)

        # NMT_Uses is direction- and epoch-scoped. It is parsed before any ref.
        if channel == 0 and payload and payload[0] == NMT_USES:
            if (not bunch.get("bReliable") or int(bunch.get("chType", 0)) != 1 or
                    bunch.get("bClose")):
                session.invalidate(
                    f"{direction} NMT_Uses lacks exact reliable ch0 control semantics",
                    frame.frame,
                )
                return
            try:
                records = _parse_uses_payload(payload, payload_bits)
            except EvidenceError as exc:
                session.record_candidate_diagnostic(
                    "identity", direction, str(exc), frame.frame,
                    retire_association=True,
                )
                return
            tracker = session.artifacts[direction]
            for record in records:
                was_incompatible = tracker.incompatible
                map_epoch = tracker.observe(record, frame.frame)
                if not was_incompatible and tracker.incompatible:
                    session.invalidate_candidate_association(direction, frame.frame)
                spec = PACKAGE_BY_KEY.get(record.key)
                exact = (
                        spec is not None and
                        record.guid.hex().upper() == spec.raw_guid_hex and
                        record.package_flags == spec.flags and
                        record.generation == spec.generation
                )
                public_identity = _uses_public_identity(record)
                self._emit({
                    "record": "nmtUsesIdentity",
                    "session": _connection_json(session.session_ordinal),
                    "connectionEpoch": session.connection_epoch,
                    "direction": direction,
                    "frame": frame.frame,
                    **public_identity,
                    "rawGuid": record.guid.hex().upper(),
                    "packageFlags": f"0x{record.package_flags:08X}",
                    "generation": record.generation,
                    "usesOrdinal": tracker.ordinal,
                    "sourceCandidateIdentityMatch": exact,
                    "generationObjectCountsPresent": False,
                    "objectBaseResolved": False,
                    "runtimeAuthorized": False,
                })
                if map_epoch is not None:
                    self._emit({
                        "record": "packageIdentityCandidateEpoch",
                        "connection": _connection_json(session.session_ordinal),
                        "connectionEpoch": session.connection_epoch,
                        "direction": direction,
                        "mapEpoch": map_epoch,
                        "mapIdentityCandidate": "VNTE-CuChi",
                        "modeCandidate": "Territories",
                        "mapModeSemanticsGrounded": False,
                        "requiredUsesOrder": [spec.name for spec in PACKAGE_ORDER],
                        "uses": list(tracker.ready_epochs[map_epoch]),
                        "fullPackageMapOrderGrounded": False,
                        "generationObjectCountsGrounded": False,
                        "objectBaseResolved": False,
                        "runtimeAuthorized": False,
                    })
            return

        map_epoch = session.artifact_epoch_for(direction)
        if map_epoch == 0:
            if (direction == "C2S" and channel == ROPC_CHANNEL) or bunch.get("bOpen"):
                session.preartifact_refs += 1
            return

        generation = session.channel_generations.get((direction, channel), 0)
        actor: Optional[ActorIdentity] = session.actors.get(channel)
        if direction == "S2C" and bunch.get("bOpen") and int(bunch.get("chType", 0)) == 2:
            try:
                class_ref, prefix_bits = decode_actor_open_prefix(payload, payload_bits)
            except EvidenceError as exc:
                session.record_candidate_diagnostic(
                    "application", direction,
                    f"actor open ch{channel} could not be decoded: {exc}",
                    frame.frame,
                )
                return
            actor = ActorIdentity(
                session.connection, session.connection_epoch, map_epoch,
                channel, generation, class_ref, _actor_kind(class_ref), frame.frame,
            )
            session.actors[channel] = actor
            session.graph[actor] = ActorGraphState()
            context = _new_context(frame, direction, packet_id, bunch, generation, payload)
            session.graph[actor].contexts["open"] = context
            self._emit({
                "record": "opaqueActorOpen",
                "session": _connection_json(session.session_ordinal),
                "connectionEpoch": session.connection_epoch,
                "directionLocalMapCandidateEpoch": map_epoch,
                "actor": _actor_json(actor),
                "evidence": _context_json(context),
                "staticObjectReferencePrefixBits": prefix_bits,
                "objectBaseResolved": False,
                "actorSemanticsResolved": False,
                "remainingOpenBitsState": "opaque",
                "runtimeAuthorized": False,
            })

            # A single reliable bunch may both create and close an actor
            # channel. The generic close above retired any prior actor; retire
            # this newly decoded actor as well so later deltas cannot bind to a
            # lifetime that ended in its opening bunch.
            if bunch.get("bClose"):
                session.actors.pop(channel, None)

        context = _new_context(frame, direction, packet_id, bunch, generation, payload)
        if direction == "C2S" and channel == ROPC_CHANNEL and not bunch.get("bOpen"):
            self._observe_c2s_rpc(session, context, payload, payload_bits)
        elif direction == "S2C" and channel == ROPC_CHANNEL and not bunch.get("bOpen"):
            self._observe_s2c_pc(session, context, payload, payload_bits)
        elif direction == "S2C" and actor is not None and not bunch.get("bOpen"):
            self._observe_actor_delta(session, actor, context, payload, payload_bits)

    def _observe_c2s_rpc(
        self, session: Session, context: PacketContext, payload: bytes,
        payload_bits: int,
    ) -> None:
        select_role_attempt = False
        try:
            leading = mc.BitReader(payload, payload_bits)
            handle = leading.rint(ROPC_MAX_HANDLE)
            if leading.error:
                raise EvidenceError("truncated C2S PlayerController handle")
            if handle == SELECT_ROLE_HANDLE:
                select_role_attempt = True
                decoded = decode_h175(payload, payload_bits)
                if not decoded["sourceCandidateRequestShapeMatch"]:
                    return
                session.target_selection_count += 1
                record = {
                    "record": "sourceCandidateRoleSelection",
                    "connection": _connection_json(session.session_ordinal),
                    "connectionEpoch": session.connection_epoch,
                    "mapEpoch": session.artifact_epoch_for("C2S"),
                    "evidence": _context_json(context),
                    "selection": decoded,
                    "captureObserved": True,
                    "semanticRoleResolved": False,
                    "owningGraphEligible": False,
                    "runtimeAuthorized": False,
                }
                self._emit(record)
                if session.transaction is not None:
                    session.record_candidate_diagnostic(
                        "application", context.direction,
                        "multiple non-retransmitted target h175 selections",
                        context.frame, retire_association=True,
                    )
                    return
                session.transaction = Transaction(
                    session.artifact_epoch_for("C2S"), decoded, context
                )
                return
            decoded_rpc = _decode_single_rpc(payload, payload_bits)
        except EvidenceError as exc:
            session.partial_payloads += 1
            session.record_candidate_diagnostic(
                "application", context.direction, str(exc), context.frame,
                retire_association=select_role_attempt,
            )
            return
        if decoded_rpc is None or session.transaction is None:
            return
        exact = decoded_rpc["decodeState"] == "complete"
        if not exact:
            session.partial_payloads += 1
            session.record_candidate_diagnostic(
                "application", context.direction,
                "C2S PlayerController RPC has an unsupported or unknown tail",
                context.frame,
            )
        if decoded_rpc["handle"] == SET_SPAWN_HANDLE:
            if exact and 128 <= int(decoded_rpc.get("value", -1)) <= 255:
                session.transaction.spawn_context = context
        elif decoded_rpc["handle"] == READY_HANDLE:
            if exact and int(decoded_rpc.get("value", -1)) == 0:
                session.transaction.ready_context = context
        else:
            return
        self._emit({
            "record": "deploymentRequest",
            "connection": _connection_json(session.session_ordinal),
            "connectionEpoch": session.connection_epoch,
            "mapEpoch": session.transaction.map_epoch,
            "evidence": _context_json(context),
            "decoded": decoded_rpc,
            "runtimeAuthorized": False,
        })

    def _observe_s2c_pc(
        self, session: Session, context: PacketContext, payload: bytes,
        payload_bits: int,
    ) -> None:
        if session.transaction is None:
            return
        try:
            decoded = _decode_single_rpc(payload, payload_bits)
        except EvidenceError as exc:
            session.partial_payloads += 1
            session.record_candidate_diagnostic(
                "application", context.direction, str(exc), context.frame,
            )
            return
        if decoded is None or decoded.get("handle") != CHANGED_ROLE_HANDLE:
            return
        exact = decoded["decodeState"] == "complete"
        if not exact:
            session.partial_payloads += 1
            session.record_candidate_diagnostic(
                "application", context.direction,
                "S2C ChangedRole RPC has an unsupported or unknown tail",
                context.frame,
            )
        if (exact and decoded.get("classIndex") == TARGET_CLASS_INDEX and
                decoded.get("showLobby") is False and
                decoded.get("showSpawnSelect") is True):
            session.transaction.changed_role_context = context
        self._emit({
            "record": "changedRole",
            "connection": _connection_json(session.session_ordinal),
            "connectionEpoch": session.connection_epoch,
            "mapEpoch": session.transaction.map_epoch,
            "evidence": _context_json(context),
            "decoded": decoded,
            "runtimeAuthorized": False,
        })

    def _observe_actor_delta(
        self, session: Session, actor: ActorIdentity, context: PacketContext,
        payload: bytes, payload_bits: int,
    ) -> None:
        try:
            detail, fields, refs = _decode_actor_delta(
                session, actor, payload, payload_bits
            )
        except EvidenceError as exc:
            session.partial_payloads += 1
            session.record_candidate_diagnostic(
                "application", context.direction,
                f"typed actor delta ch{actor.channel} could not be decoded: {exc}",
                context.frame,
            )
            return
        if not fields:
            session.partial_payloads += 1
            session.record_candidate_diagnostic(
                "application", context.direction,
                f"typed actor delta ch{actor.channel} has no grounded fields",
                context.frame,
            )
            return
        exact = detail["decodeState"] == "complete"
        if not exact:
            session.partial_payloads += 1
            session.record_candidate_diagnostic(
                "application", context.direction,
                f"typed actor delta ch{actor.channel} has an unsupported or unknown tail",
                context.frame,
            )
        if not exact:
            self._emit({
                "record": "typedGraphDelta",
                "session": _connection_json(session.session_ordinal),
                "connectionEpoch": session.connection_epoch,
                "mapEpoch": actor.map_epoch,
                "actor": _actor_json(actor),
                "evidence": _context_json(context),
                "decode": detail,
                "fields": fields,
                "dynamicReferences": {
                    name: _ref_json(ref) for name, ref in refs.items()
                },
                "fieldsAppliedToGraph": False,
                "runtimeAuthorized": False,
            })
            return
        state = session.graph[actor]
        state.fields.update(fields)
        state.refs.update(refs)
        for name in fields:
            state.contexts[name] = context
        # The two historical weapon schemas do not cover the full owning MG
        # loadout, h6 Owner/h4 Instigator, manager h6/h4/h23, pawn attachment
        # state, or possession edges. Never promote one local prefix into a
        # complete graph.
        state.graph_block_complete = False
        self._emit({
            "record": "typedGraphDelta",
            "connection": _connection_json(session.session_ordinal),
            "connectionEpoch": session.connection_epoch,
            "mapEpoch": actor.map_epoch,
            "actor": _actor_json(actor),
            "evidence": _context_json(context),
            "decode": detail,
            "fields": fields,
            "dynamicReferences": {name: _ref_json(ref) for name, ref in refs.items()},
            "fieldsAppliedToGraph": True,
            "owningGraphComplete": False,
            "runtimeAuthorized": False,
        })

    def finish(self) -> tuple[dict[str, object], ...]:
        identity_sessions: list[tuple[Session, int]] = []
        for candidate_session in self.sessions:
            epoch = candidate_session.artifact_epoch_for("C2S")
            if epoch:
                identity_sessions.append((candidate_session, epoch))
        candidate_identity_complete = len(identity_sessions) == 1
        target_sessions = [
            candidate_session
            for candidate_session, epoch in identity_sessions
            if (
                candidate_session.transaction is not None and
                candidate_session.transaction.map_epoch == epoch
            )
        ]
        candidate_application_complete = (
            candidate_identity_complete and len(target_sessions) == 1 and
            not target_sessions[0].candidate_application_incomplete
        )
        ambiguous = len(target_sessions) != 1
        session = target_sessions[0] if len(target_sessions) == 1 else None
        identity_barriers = sum(len(item.invalid_reasons) for item in self.sessions)
        structural_complete = (
            self.frames_observed > 0 and self.packet_errors == 0 and
            self.dumpcap_drops == 0 and identity_barriers == 0
        )
        summary = {
        "record": "summary",
        "schema": SCHEMA,
        "sourceCandidate": {
                "mapIdentityCandidate": "VNTE-CuChi",
                "modeCandidate": "Territories",
                "mapModeSemanticsGrounded": False,
                "southDesired": True,
                "roleInfoStaticReference": SOURCE_CANDIDATE_ROLE_REF,
                "constructedPayloadBits": CONSTRUCTED_H175_BITS,
                "constructedPayloadSha256": _payload_sha256(
                    bytes.fromhex(CONSTRUCTED_H175_HEX)
                ),
                "semanticRoleResolved": False,
            },
            "sourceConstructedCandidateGroundedByCapture": False,
            "historicalRemoteActorProximityGroundsOwningGraph": False,
            "sessions": len(self.sessions),
            "sourceCandidateSessions": len(target_sessions),
            "ambiguousSourceCandidateSessions": ambiguous,
            "candidateIdentityComplete": candidate_identity_complete,
            "candidateApplicationComplete": candidate_application_complete,
            "candidateAssociationInvalidations": sum(
                len(item.candidate_invalid_reasons) for item in self.sessions
            ),
            "candidateDiagnostics": sum(
                len(item.candidate_diagnostics) for item in self.sessions
            ),
            "packetDecodeErrors": self.packet_errors,
            "captureReportedDrops": self.dumpcap_drops,
            "captureFramesAnalyzed": self.frames_observed,
            "identityBarriers": identity_barriers,
            "reliableRetransmissionsDeduplicated": sum(item.retransmissions for item in self.sessions),
            "partialTypedPayloads": sum(item.partial_payloads for item in self.sessions),
            "preArtifactReferenceBunchesIgnored": sum(item.preartifact_refs for item in self.sessions),
            "captureStructurallyComplete": structural_complete,
            "packageMapGrounded": False,
            "candidateRecordsGrounded": False,
            "owningGraphProven": False,
            "runtimeAuthorizes": False,
            "complete": False,
            "completeMeaning": (
                "owning acceptance intentionally unavailable; "
                "see captureStructurallyComplete"
            ),
            "acceptanceBlockers": [
                "full direction-specific PackageMap order and generation object-count rows",
                "artifact-pinned ObjectBase and serialized CDO NetIndex resolver",
                "complete installed candidate-loadout CDO ClassNetCache tables",
                "weapon h6 Owner and h4 Instigator joins",
                "inventory-manager h6 Owner h4 Instigator h23 InventoryChain",
                "pawn h167 attachments h147 current attachment h57 ClientPossessed",
                "PlayerController possession edges",
            ],
            "runtimeAuthorized": False,
        }
        if session is not None:
            summary["selectedConnection"] = _connection_json(session.session_ordinal)
            summary["selectedConnectionEpoch"] = session.connection_epoch
            summary["selectedMapEpoch"] = session.transaction.map_epoch
            summary["invalidReasons"] = list(session.invalid_reasons)
            summary["candidateDiagnosticReasons"] = list(session.candidate_diagnostics)
            summary["directionalNmtUsesStreams"] = {
                direction: {
                    "records": tracker.ordinal,
                    "orderedIdentitySha256": tracker.digest_hex(),
                    "generationObjectCountsGrounded": False,
                    "objectBaseResolved": False,
                }
                for direction, tracker in session.artifacts.items()
            }
        return tuple(self.events + [summary])


def analyze_frames(
    frames: Iterable[CaptureFrame], server_port: int = 7777,
    max_events: int = MAX_RELEVANT_EVENTS, server_address: str = "10.0.0.1",
) -> tuple[dict[str, object], ...]:
    analyzer = CaptureAnalyzer(server_port, max_events, server_address)
    iterator = iter(frames)
    try:
        for frame in iterator:
            analyzer.observe(frame)
    finally:
        close = getattr(iterator, "close", None)
        if callable(close):
            close()
    return analyzer.finish()


def _bounded_reap_process(
    process: subprocess.Popen[str], *, terminate_first: bool,
) -> Optional[int]:
    """Reap a child without an unbounded wait; never expose child diagnostics."""
    try:
        return_code = process.poll()
    except OSError:
        return_code = None
    if return_code is not None:
        return return_code

    if terminate_first:
        try:
            process.terminate()
        except OSError:
            pass
    try:
        return process.wait(timeout=TSHARK_PROCESS_WAIT_SECONDS)
    except (OSError, subprocess.TimeoutExpired):
        pass

    if not terminate_first:
        try:
            process.terminate()
        except OSError:
            pass
        try:
            return process.wait(timeout=TSHARK_PROCESS_WAIT_SECONDS)
        except (OSError, subprocess.TimeoutExpired):
            pass

    try:
        process.kill()
    except OSError:
        pass
    try:
        return process.wait(timeout=TSHARK_PROCESS_WAIT_SECONDS)
    except (OSError, subprocess.TimeoutExpired):
        try:
            return process.poll()
        except OSError:
            return None


def iter_tshark_frames(
    tshark: Path, pcap: Path, server_address: str, server_port: int,
) -> Iterator[CaptureFrame]:
    command = [
        str(tshark), "-r", str(pcap), "-Y",
        f"ip.addr=={server_address} && udp.port=={server_port}",
        "-T", "fields", "-E", "separator=\t", "-E", "occurrence=f",
        "-e", "frame.number", "-e", "frame.time_relative",
        "-e", "ip.src", "-e", "udp.srcport", "-e", "ip.dst",
        "-e", "udp.dstport", "-e", "udp.payload", "-e", "frame.drop_count",
    ]
    try:
        process = subprocess.Popen(
            command, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True,
            encoding="utf-8", errors="replace",
        )
    except OSError as exc:
        raise EvidenceError("pinned tshark could not be started") from exc
    assert process.stdout is not None
    cleanup_lock = threading.Lock()
    cleanup_complete = False
    cleanup_return_code: Optional[int] = None

    def cleanup(terminate_first: bool) -> Optional[int]:
        nonlocal cleanup_complete, cleanup_return_code
        with cleanup_lock:
            if not cleanup_complete:
                cleanup_return_code = _bounded_reap_process(
                    process, terminate_first=terminate_first
                )
                cleanup_complete = True
            return cleanup_return_code

    output: queue.Queue[tuple[str, object]] = queue.Queue(maxsize=256)
    stop_reader = threading.Event()

    def enqueue(kind: str, value: object) -> bool:
        while not stop_reader.is_set():
            try:
                output.put((kind, value), timeout=0.05)
                return True
            except queue.Full:
                continue
        return False

    def read_stdout() -> None:
        try:
            for line in process.stdout:
                if not enqueue("line", line):
                    return
        except BaseException:
            # Child/pipe diagnostics may contain local paths. Preserve only the
            # fact that the streaming read failed.
            enqueue("error", None)
        finally:
            enqueue("eof", None)

    reader = threading.Thread(target=read_stdout, name="tshark-stdout", daemon=True)
    reader.start()
    deadline = time.monotonic() + TSHARK_STREAM_TIMEOUT_SECONDS
    reached_stdout_eof = False
    row_ordinal = 0
    try:
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise EvidenceError("pinned tshark streaming extraction timed out")
            try:
                kind, value = output.get(timeout=min(0.1, remaining))
            except queue.Empty:
                continue
            if kind == "eof":
                reached_stdout_eof = True
                break
            if kind == "error":
                raise EvidenceError("pinned tshark output stream could not be read")
            line = str(value)
            row_ordinal += 1
            columns = line.rstrip("\r\n").split("\t")
            if len(columns) != 8:
                raise EvidenceError(
                    f"tshark output row {row_ordinal} has an unexpected field count"
                )
            try:
                yield CaptureFrame(
                    int(columns[0]), float(columns[1]), columns[2],
                    int(columns[3]), columns[4], int(columns[5]),
                    bytes.fromhex(columns[6]) if columns[6] else b"",
                    int(columns[7] or "0"),
                )
            except ValueError as exc:
                raise EvidenceError(
                    f"tshark output row {row_ordinal} is malformed"
                ) from exc
    finally:
        stop_reader.set()
        if not reached_stdout_eof:
            cleanup(True)
        return_code = cleanup(not reached_stdout_eof)
        reader.join(timeout=TSHARK_PROCESS_WAIT_SECONDS * 2)
        if not reader.is_alive() and process.stdout is not None:
            process.stdout.close()
    if return_code != 0:
        raise EvidenceError("pinned tshark exited nonzero")


def extract_capture(
    pcap: Path, manifest_path: Path, tshark: Path,
    expected_bytes: int, expected_sha256: str,
    server_port: int, max_capture_mib: int, max_events: int,
) -> tuple[dict[str, object], ...]:
    if pcap.is_symlink() or manifest_path.is_symlink() or tshark.is_symlink():
        raise EvidenceError("pcap, manifest, and tshark must not be symlinks")
    pcap = pcap.resolve()
    manifest_path = manifest_path.absolute()
    tshark = tshark.resolve()
    if not pcap.is_file():
        raise EvidenceError("pcap does not exist or is not a regular file")
    if not tshark.is_file():
        raise EvidenceError("pinned tshark does not exist or is not a regular file")
    validate_tshark_identity(tshark)
    if expected_bytes <= 0 or expected_bytes > max_capture_mib * 1024 * 1024:
        raise EvidenceError("expected pcap size is outside the configured bound")
    expected_sha256 = expected_sha256.upper()
    if len(expected_sha256) != 64 or any(ch not in "0123456789ABCDEF" for ch in expected_sha256):
        raise EvidenceError("expected SHA-256 must be exactly 64 hexadecimal digits")
    try:
        manifest_identity = file_identity(manifest_path)
    except AtomicWriteError as exc:
        raise EvidenceError("capture manifest identity validation failed") from exc
    manifest = load_capture_manifest(manifest_path, pcap, server_port)
    if (manifest.capture_bytes != expected_bytes or
            manifest.capture_sha256 != expected_sha256):
        raise EvidenceError("CLI pcap identity does not exactly match the capture manifest")
    try:
        validate_file_identity(pcap, expected_bytes, expected_sha256, "pcap before extraction")
    except AtomicWriteError as exc:
        raise EvidenceError("pcap identity validation failed before extraction") from exc

    records = analyze_frames(
        iter_tshark_frames(
            tshark, pcap, manifest.server_address, manifest.server_port
        ),
        server_port, max_events, manifest.server_address,
    )
    if int(records[-1].get("captureFramesAnalyzed", -1)) != manifest.captured_packets:
        raise EvidenceError(
            "tshark frame count does not match the manifest captured-packet count"
        )
    try:
        validate_file_identity(pcap, expected_bytes, expected_sha256, "pcap after extraction")
        validate_file_identity(
            manifest_path, manifest_identity[0], manifest_identity[1],
            "capture manifest after extraction",
        )
        validate_tshark_identity(tshark)
    except AtomicWriteError as exc:
        raise EvidenceError("pcap/manifest identity changed during extraction") from exc
    header = {
        "record": "header",
        "schema": SCHEMA,
        "captureBytes": expected_bytes,
        "captureSha256": expected_sha256,
        "captureIdentityStablePrePost": True,
        "captureManifestValidated": True,
        "captureManifestIdentityStablePrePost": True,
        "captureDropsKnown": True,
        "captureDroppedPackets": 0,
        "directionalDecodeBounds": {"C2S": C2S_BUNCH_BITS, "S2C": S2C_BUNCH_BITS},
        "packageStreamsMergedAcrossDirections": False,
        "packageMapGrounded": False,
        "candidateRecordsGrounded": False,
        "owningGraphProven": False,
        "rawPcapEmbedded": False,
        "runtimeAuthorizes": False,
        "runtimeAuthorized": False,
    }
    return (header, *records)


def _paths_alias(left: Path, right: Path) -> bool:
    left_resolved = left.resolve()
    right_resolved = right.resolve()
    if left_resolved == right_resolved:
        return True
    try:
        return left_resolved.exists() and right_resolved.exists() and os.path.samefile(
            left_resolved, right_resolved
        )
    except OSError:
        return False


def _validate_report_destination(
    destination: Optional[Path], protected_inputs: Sequence[Path],
) -> None:
    if destination is not None and any(
        _paths_alias(source, destination) for source in protected_inputs
    ):
        raise EvidenceError(
            "output must not overwrite or alias the pcap/manifest/tshark inputs"
        )


def _write_bounded_jsonl(
    records: Sequence[Mapping[str, object]], output: object,
) -> None:
    total_bytes = 0
    for record in records:
        line = (
            json.dumps(record, sort_keys=True, separators=(",", ":")) + "\n"
        ).encode("utf-8")
        total_bytes += len(line)
        if total_bytes > MAX_REPORT_BYTES:
            raise EvidenceError("evidence report exceeds the bounded output size")
        output.write(line)  # type: ignore[attr-defined]


def _atomic_write_jsonl(
    records: Sequence[Mapping[str, object]], destination: Optional[Path],
    overwrite: bool,
) -> None:
    if destination is None:
        with tempfile.SpooledTemporaryFile(
            max_size=REPORT_SPOOL_BYTES, mode="w+b"
        ) as rendered:
            _write_bounded_jsonl(records, rendered)
            rendered.seek(0)
            while True:
                chunk = rendered.read(1024 * 1024)
                if not chunk:
                    break
                # json.dumps ensure_ascii=True keeps every serialized byte ASCII.
                sys.stdout.write(chunk.decode("ascii"))
        return

    # Resolve the parent once, but never resolve or open the final component.
    # Publishing a sibling temporary with replace/rename changes only that
    # directory entry even if an attacker races in a symlink or hardlink.
    absolute_destination = destination.absolute()
    destination_name = absolute_destination.name
    if not destination_name or destination_name in (".", ".."):
        raise EvidenceError("evidence report destination has no safe file name")
    parent = absolute_destination.parent.resolve()
    parent.mkdir(parents=True, exist_ok=True)
    anchored_destination = parent / destination_name
    if os.path.lexists(anchored_destination) and not overwrite:
        raise EvidenceError("evidence report output exists; pass --overwrite")

    temporary_path: Optional[Path] = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w+b",
            dir=parent,
            prefix=destination_name + ".",
            suffix=".tmp",
            delete=False,
        ) as output:
            temporary_path = Path(output.name)
            _write_bounded_jsonl(records, output)
            output.flush()
            os.fsync(output.fileno())
        if overwrite:
            os.replace(temporary_path, anchored_destination)
        elif os.name == "nt":
            # Windows rename is fail-if-exists, preserving no-overwrite races.
            os.rename(temporary_path, anchored_destination)
        else:
            # link is an atomic fail-if-exists publication on the same volume.
            os.link(temporary_path, anchored_destination)
            temporary_path.unlink()
        temporary_path = None
    finally:
        if temporary_path is not None:
            try:
                temporary_path.unlink(missing_ok=True)
            except OSError:
                pass


def write_report(
    records: Sequence[Mapping[str, object]], destination: Optional[Path],
    overwrite: bool, protected_inputs: Sequence[Path] = (),
) -> None:
    _validate_report_destination(destination, protected_inputs)
    try:
        _atomic_write_jsonl(records, destination, overwrite)
    except EvidenceError:
        raise
    except (OSError, TypeError, ValueError, UnicodeError, RecursionError) as exc:
        raise EvidenceError("could not atomically write the evidence report") from exc


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pcap", type=Path, required=True)
    parser.add_argument("--expected-bytes", type=int, required=True)
    parser.add_argument("--expected-sha256", required=True)
    parser.add_argument(
        "--manifest", type=Path,
        help="capture sidecar; defaults to <pcap>.manifest.json",
    )
    parser.add_argument("--tshark", type=Path,
                        default=Path(r"C:\Program Files\Wireshark\tshark.exe"))
    parser.add_argument("--server-port", type=int, default=7777)
    parser.add_argument("--max-capture-mib", type=int, default=512)
    parser.add_argument("--max-events", type=int, default=MAX_RELEVANT_EVENTS)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--overwrite", action="store_true")
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if not 1 <= args.server_port <= 65535:
        parser.error("--server-port must be in [1,65535]")
    if args.max_capture_mib <= 0 or args.max_events <= 0:
        parser.error("size/event bounds must be positive")
    try:
        pcap = args.pcap.absolute()
        manifest = (
            args.manifest.absolute() if args.manifest is not None
            else Path(str(pcap) + ".manifest.json").absolute()
        )
        protected_inputs = (pcap, manifest, args.tshark.absolute())
        _validate_report_destination(args.output, protected_inputs)
        records = extract_capture(
            pcap, manifest, args.tshark, args.expected_bytes, args.expected_sha256,
            args.server_port, args.max_capture_mib, args.max_events,
        )
        # Recheck at the final write boundary in case the output path was
        # replaced with a hardlink/symlink while extraction was running.
        _validate_report_destination(args.output, protected_inputs)
        write_report(records, args.output, args.overwrite, protected_inputs)
        return 0 if bool(records[-1].get("complete")) else 67
    except EvidenceError as exc:
        print(json.dumps({
            "record": "summary", "schema": SCHEMA, "fatal": True,
            "error": str(exc),
            "captureStructurallyComplete": False,
            "candidateIdentityComplete": False,
            "candidateApplicationComplete": False,
            "packageMapGrounded": False,
            "candidateRecordsGrounded": False,
            "owningGraphProven": False,
            "runtimeAuthorizes": False,
            "complete": False,
            "runtimeAuthorized": False,
        }, separators=(",", ":")), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
