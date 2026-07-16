#!/usr/bin/env python3
"""Extract source-grounded RS2 helicopter/crew evidence from a pcap.

The tool intentionally decodes only the prefix that is proven for every
transient UE3 actor open:

    static PackageMap object ref + compressed FVector Location

Rotation and the helicopter replicated-property/RPC tail remain opaque.  The
tool additionally decodes a deliberately small set of *whole* property bunches
on channels whose actor class has already been grounded by its open bunch:
ROVehicleFactory h23/h24, ROTeamInfo h48/h49/h50/h72, and ROPRI h35/h63/h64.
If every bit cannot be typed, the bunch stays opaque; bit-pattern searches are
never promoted to semantic evidence.

Class identities are pinned by installed package export tables.  Vehicle and
factory refs use ROGameContent ObjectBase 285944.  TeamInfo/PRI refs are the
existing capture-grounded ROGame PackageMap indices.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Optional, TextIO


TOOLS_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS_DIR))
import mock_client as mc  # noqa: E402


SCHEMA = "rs2.vehicle-transaction-evidence.v2"
KNOWN_CAPTURE_SHA256 = (
    "08B9128409B8269100AA21B3DD2D596DE1F6BE50AABE53A3C7B53BE9E48A411F"
)
ROGAMECONTENT_PACKAGE_GUID = "FE4B4F2F4B3128C42FE5098ACAB560C8"
ROGAMECONTENT_OBJECT_BASE = 285944
STATIC_OBJECT_MAX = 0x80000000
MAX_BUNCH_BITS = 12000
TEAMINFO_STATIC_INDEX = 90245
PRI_STATIC_INDEX = 86701
TEAMINFO_MAX_HANDLE = 78
PRI_MAX_HANDLE = 98
FACTORY_MAX_HANDLE = 25
MAX_STATIC_ARRAY_INDEX = 10
MAX_FSTRING_UNITS = 1024


class EvidenceError(ValueError):
    """The capture contained a recognized vehicle open with an invalid prefix."""


@dataclass(frozen=True)
class ArchetypeSpec:
    static_index: int
    package_export_index: int
    class_path: str
    max_handle: int


ARCHETYPES = {
    285994: ArchetypeSpec(
        285994, 50, "ROGameContent.ROHeli_AH1G_Content", 130
    ),
    285996: ArchetypeSpec(
        285996, 52, "ROGameContent.ROHeli_OH6_Content", 129
    ),
    286038: ArchetypeSpec(
        286038, 94, "ROGameContent.ROHeli_UH1H_Content", 150
    ),
}

# Exact compiled ROGameContent.u class exports.  Placed map actors may instead
# open through their map-package object ref; those are intentionally not guessed.
FACTORY_ARCHETYPES = {
    286244: ArchetypeSpec(
        286244, 300, "ROGameContent.ROVehicleFactory_AH1G", FACTORY_MAX_HANDLE
    ),
    286252: ArchetypeSpec(
        286252, 308, "ROGameContent.ROVehicleFactory_OH6", FACTORY_MAX_HANDLE
    ),
    286259: ArchetypeSpec(
        286259, 315, "ROGameContent.ROVehicleFactory_UH1H", FACTORY_MAX_HANDLE
    ),
}


@dataclass(frozen=True)
class ObjectRef:
    kind: str
    index: int


@dataclass(frozen=True)
class TypedProperty:
    handle: int
    name: str
    bit_offset: int
    bits_consumed: int
    array_index: Optional[int]
    value: object


@dataclass(frozen=True)
class ActorIdentity:
    endpoint: str
    channel: int
    generation: int
    kind: str
    class_path: str
    open_frame: int


@dataclass(frozen=True)
class EvidenceContext:
    frame: int
    time_seconds: float
    packet_id: Optional[int]
    payload_bits: int
    payload_sha256: str


@dataclass
class PriState:
    team: Optional[ActorIdentity] = None
    team_evidence: Optional[EvidenceContext] = None
    array_index: Optional[int] = None
    array_evidence: Optional[EvidenceContext] = None
    seat_index: Optional[int] = None
    seat_evidence: Optional[EvidenceContext] = None
    last_emitted: Optional[tuple] = None


@dataclass(frozen=True)
class VehicleOpenPrefix:
    archetype: ArchetypeSpec
    location: tuple[int, int, int]
    bits_consumed: int


def _decode_object_ref(reader: mc.BitReader) -> ObjectRef:
    dynamic = bool(reader.bit())
    index = reader.rint(1024 if dynamic else STATIC_OBJECT_MAX)
    if reader.error:
        raise EvidenceError("truncated object reference")
    return ObjectRef("dynamic" if dynamic else "static", index)


def _decode_compressed_vector(reader: mc.BitReader) -> tuple[int, int, int]:
    magnitude_bits = reader.rint(20)
    if reader.error or magnitude_bits >= 20:
        raise EvidenceError("invalid compressed vector magnitude")
    bias = 1 << (magnitude_bits + 1)
    component_max = 1 << (magnitude_bits + 2)
    value = tuple(reader.rint(component_max) - bias for _ in range(3))
    if reader.error:
        raise EvidenceError("truncated compressed vector")
    if any(abs(component) > 1_048_576 for component in value):
        raise EvidenceError("compressed vector exceeds UE3 bounds")
    return value


def _decode_fstring(reader: mc.BitReader) -> str:
    length = reader.ru(32)
    if reader.error:
        raise EvidenceError("truncated FString length")
    if length & 0x80000000:
        length -= 1 << 32
    if length == 0:
        return ""
    units = abs(length)
    width = 2 if length < 0 else 1
    if units > MAX_FSTRING_UNITS or reader.p + units * width * 8 > reader.n:
        raise EvidenceError("FString exceeds its bounded property payload")
    raw = bytes(reader.ru(8) for _ in range(units * width))
    if reader.error:
        raise EvidenceError("truncated FString data")
    terminator = b"\x00\x00" if width == 2 else b"\x00"
    if not raw.endswith(terminator):
        raise EvidenceError("FString is missing its serialized terminator")
    try:
        return raw[:-width].decode("utf-16-le" if width == 2 else "latin-1")
    except UnicodeDecodeError as error:
        raise EvidenceError("FString contains invalid UTF-16") from error


def _property_definition(actor_kind: str, handle: int) -> Optional[tuple[str, str, bool]]:
    if actor_kind == "factory":
        return {
            23: ("bHasLockedVehicle", "bool", False),
            24: ("ChildVehicle", "object", False),
        }.get(handle)
    if actor_kind == "teamInfo":
        return {
            48: ("TeamHelicopterPilotNames", "string", True),
            49: ("TeamHelicopterLocationArray", "vector", True),
            50: ("TeamHelicopterArray", "object", True),
            72: ("TeamHelicopterRep", "byte", True),
        }.get(handle)
    if actor_kind == "pri":
        return {
            35: ("Team", "object", False),
            63: ("TeamHelicopterSeatIndex", "byte", False),
            64: ("TeamHelicopterArrayIndex", "byte", False),
        }.get(handle)
    raise ValueError("unsupported actor kind: {}".format(actor_kind))


def decode_target_property_block(
    payload: bytes, payload_bits: int, actor_kind: str, start_bit: int = 0,
) -> Optional[tuple[TypedProperty, ...]]:
    """Decode only fully typed target-field bunches.

    ``None`` means the bunch contains another handle and therefore remains
    opaque.  Once a target handle is recognized, malformed/truncated target
    data raises ``EvidenceError`` rather than being reported as an absence.
    """

    if not isinstance(payload, (bytes, bytearray)):
        raise TypeError("payload must be bytes")
    if (payload_bits < 0 or payload_bits > len(payload) * 8 or
            payload_bits > MAX_BUNCH_BITS or start_bit < 0 or start_bit > payload_bits):
        raise EvidenceError("property block bit range is outside its bounded byte buffer")
    if start_bit == payload_bits:
        return tuple()

    max_handle = {
        "factory": FACTORY_MAX_HANDLE,
        "teamInfo": TEAMINFO_MAX_HANDLE,
        "pri": PRI_MAX_HANDLE,
    }.get(actor_kind)
    if max_handle is None:
        raise ValueError("unsupported actor kind: {}".format(actor_kind))

    reader = mc.BitReader(bytes(payload), payload_bits)
    reader.p = start_bit
    properties = []
    while reader.p < payload_bits:
        field_start = reader.p
        handle = reader.rint(max_handle)
        if reader.error:
            raise EvidenceError("truncated property handle")
        definition = _property_definition(actor_kind, handle)
        if definition is None:
            return None
        name, value_type, is_array = definition
        array_index = reader.ru(8) if is_array else None
        if reader.error:
            raise EvidenceError("truncated static-array index")
        if array_index is not None and not 0 <= array_index < MAX_STATIC_ARRAY_INDEX:
            raise EvidenceError("static-array index is outside [0,10)")

        if value_type == "bool":
            value = bool(reader.bit())
        elif value_type == "byte":
            value = reader.ru(8)
        elif value_type == "object":
            value = _decode_object_ref(reader)
        elif value_type == "vector":
            value = _decode_compressed_vector(reader)
        elif value_type == "string":
            value = _decode_fstring(reader)
        else:  # pragma: no cover - definitions above are deliberately closed.
            raise AssertionError("unhandled target property type")
        if reader.error:
            raise EvidenceError("truncated {} property value".format(name))
        properties.append(TypedProperty(
            handle=handle,
            name=name,
            bit_offset=field_start,
            bits_consumed=reader.p - field_start,
            array_index=array_index,
            value=value,
        ))

    if reader.p != payload_bits:
        raise EvidenceError("property decoder did not consume the exact bunch bit count")
    return tuple(properties)


def decode_vehicle_open(payload: bytes, payload_bits: int) -> Optional[VehicleOpenPrefix]:
    """Return a recognized vehicle prefix, or None for a non-vehicle actor open.

    Once the static ref names a known vehicle class, malformed/truncated location
    data is an error rather than a silent non-match.  This distinction prevents a
    damaged capture from looking like an authoritative absence of vehicles.
    """

    if not isinstance(payload, (bytes, bytearray)):
        raise TypeError("payload must be bytes")
    if payload_bits < 0 or payload_bits > len(payload) * 8 or payload_bits > MAX_BUNCH_BITS:
        raise EvidenceError("payload bit count is outside its bounded byte buffer")
    if payload_bits < 32:
        return None

    reader = mc.BitReader(bytes(payload), payload_bits)
    is_dynamic = bool(reader.bit())
    static_index = reader.rint(1024 if is_dynamic else STATIC_OBJECT_MAX)
    if reader.error:
        raise EvidenceError("truncated actor object reference")
    if is_dynamic:
        return None

    archetype = ARCHETYPES.get(static_index)
    if archetype is None:
        return None

    location = _decode_compressed_vector(reader)

    return VehicleOpenPrefix(archetype, location, reader.p)


def _value_json(value: object) -> object:
    if isinstance(value, ObjectRef):
        return {"kind": value.kind, "index": value.index}
    if isinstance(value, tuple) and len(value) == 3:
        return {"x": value[0], "y": value[1], "z": value[2]}
    return value


def _property_json(prop: TypedProperty) -> dict:
    result = {
        "handle": prop.handle,
        "name": prop.name,
        "bitOffset": prop.bit_offset,
        "bitsConsumed": prop.bits_consumed,
        "value": _value_json(prop.value),
    }
    if prop.array_index is not None:
        result["arrayIndex"] = prop.array_index
    return result


def _actor_json(actor: ActorIdentity) -> dict:
    return {
        "kind": actor.kind,
        "classPath": actor.class_path,
        "channel": actor.channel,
        "channelGeneration": actor.generation,
        "openFrame": actor.open_frame,
    }


def _context_json(context: EvidenceContext) -> dict:
    return {
        "frame": context.frame,
        "timeSeconds": context.time_seconds,
        "packetId": context.packet_id,
        "payloadBits": context.payload_bits,
        "payloadSha256": context.payload_sha256,
    }


class EvidenceCorrelator:
    """Resolve typed refs without allowing channel reuse to rewrite history."""

    def __init__(self) -> None:
        self._generations = {}
        self._current = {}
        self._vehicle_details = {}
        self._team_slots = {}
        self._team_aux_last = {}
        self._factory_children = {}
        self._pri_states = {}
        self.unresolved_vehicle_refs = 0

    def open_actor(
        self, endpoint: str, channel: int, kind: str, class_path: str,
        frame: int, vehicle_details: Optional[dict] = None,
    ) -> ActorIdentity:
        key = (endpoint, channel)
        generation = self._generations.get(key, 0) + 1
        self._generations[key] = generation
        actor = ActorIdentity(endpoint, channel, generation, kind, class_path, frame)
        self._current[key] = actor
        if vehicle_details is not None:
            self._vehicle_details[actor] = vehicle_details
        return actor

    def close_actor(self, endpoint: str, channel: int) -> None:
        self._current.pop((endpoint, channel), None)

    def invalidate_endpoint(self, endpoint: str) -> tuple[ActorIdentity, ...]:
        """Drop all live identities after an undecodable packet.

        A failed frame could contain a close/reopen.  Keeping any channel alive
        would let a later dynamic ref bind across an unobserved generation.
        """
        actors = tuple(sorted(
            (actor for key, actor in self._current.items() if key[0] == endpoint),
            key=lambda actor: (actor.channel, actor.generation),
        ))
        for actor in actors:
            self._current.pop((endpoint, actor.channel), None)
        return actors

    def current_actor(self, endpoint: str, channel: int) -> Optional[ActorIdentity]:
        return self._current.get((endpoint, channel))

    def _active(self, actor: Optional[ActorIdentity]) -> bool:
        return actor is not None and self.current_actor(actor.endpoint, actor.channel) == actor

    def _resolve(self, endpoint: str, ref: ObjectRef, kind: str) -> Optional[ActorIdentity]:
        if ref.kind != "dynamic" or ref.index == 0:
            return None
        actor = self.current_actor(endpoint, ref.index)
        return actor if actor is not None and actor.kind == kind else None

    def _direct_record(
        self, kind: str, source: ActorIdentity, vehicle: ActorIdentity,
        context: EvidenceContext, prop: TypedProperty,
    ) -> dict:
        return {
            "record": "vehicleCorrelation",
            "correlation": kind,
            "clientEndpoint": source.endpoint,
            "sourceActor": _actor_json(source),
            "vehicleActor": _actor_json(vehicle),
            "vehicle": self._vehicle_details[vehicle],
            "evidence": {
                **_context_json(context),
                "property": _property_json(prop),
                "wholeBunchTyped": True,
            },
        }

    def _seat_record(
        self, pri: ActorIdentity, state: PriState, vehicle: ActorIdentity,
    ) -> dict:
        assert state.team is not None
        assert state.array_index is not None and state.seat_index is not None
        return {
            "record": "vehicleCorrelation",
            "correlation": "pri.helicopterSeat",
            "clientEndpoint": pri.endpoint,
            "sourceActor": _actor_json(pri),
            "teamInfoActor": _actor_json(state.team),
            "vehicleActor": _actor_json(vehicle),
            "vehicle": self._vehicle_details[vehicle],
            "arrayIndex": state.array_index,
            "seatIndex": state.seat_index,
            "supportingEvidence": {
                "team": _context_json(state.team_evidence),
                "arrayIndex": _context_json(state.array_evidence),
                "seatIndex": _context_json(state.seat_evidence),
            },
            "wholeBunchTyped": True,
        }

    def _maybe_seat(self, pri: ActorIdentity) -> list[dict]:
        state = self._pri_states.get(pri)
        if (state is None or state.team is None or not self._active(state.team) or
                state.array_index is None or state.array_index == 255 or
                state.seat_index is None or state.seat_index == 255):
            return []
        slot = self._team_slots.get((state.team, state.array_index))
        if slot is None:
            return []
        vehicle = slot[0]
        if not self._active(vehicle):
            return []
        fingerprint = (
            state.team, state.array_index, state.seat_index, vehicle,
        )
        if state.last_emitted == fingerprint:
            return []
        state.last_emitted = fingerprint
        return [self._seat_record(pri, state, vehicle)]

    def observe_block(
        self, actor: ActorIdentity, properties: tuple[TypedProperty, ...],
        context: EvidenceContext,
    ) -> list[dict]:
        # Callers must never be able to re-use a source identity that was
        # closed or invalidated by a packet-decode gap.
        if not self._active(actor):
            return []
        records = []
        for prop in properties:
            if actor.kind == "factory" and prop.handle == 24:
                assert isinstance(prop.value, ObjectRef)
                vehicle = self._resolve(actor.endpoint, prop.value, "vehicle")
                previous = self._factory_children.get(actor)
                self._factory_children[actor] = (vehicle, context, prop)
                if vehicle is not None and (previous is None or previous[0] != vehicle):
                    records.append(self._direct_record(
                        "factory.ChildVehicle", actor, vehicle, context, prop
                    ))
                elif prop.value.kind == "dynamic" and prop.value.index and vehicle is None:
                    self.unresolved_vehicle_refs += 1

            elif actor.kind == "teamInfo" and prop.handle == 50:
                assert prop.array_index is not None and isinstance(prop.value, ObjectRef)
                vehicle = self._resolve(actor.endpoint, prop.value, "vehicle")
                slot_key = (actor, prop.array_index)
                previous = self._team_slots.get(slot_key)
                self._team_slots[slot_key] = (vehicle, context, prop)
                if vehicle is not None and (previous is None or previous[0] != vehicle):
                    records.append(self._direct_record(
                        "teamInfo.TeamHelicopterArray", actor, vehicle, context, prop
                    ))
                elif prop.value.kind == "dynamic" and prop.value.index and vehicle is None:
                    self.unresolved_vehicle_refs += 1

            elif (actor.kind == "teamInfo" and prop.handle in (48, 49, 72) and
                  prop.array_index is not None):
                slot = self._team_slots.get((actor, prop.array_index))
                vehicle = slot[0] if slot is not None else None
                if self._active(vehicle):
                    fingerprint = (vehicle, prop.value)
                    key = (actor, prop.name, prop.array_index)
                    if self._team_aux_last.get(key) != fingerprint:
                        self._team_aux_last[key] = fingerprint
                        records.append(self._direct_record(
                            "teamInfo." + prop.name, actor, vehicle, context, prop
                        ))

            elif actor.kind == "pri":
                state = self._pri_states.setdefault(actor, PriState())
                if prop.handle == 35:
                    assert isinstance(prop.value, ObjectRef)
                    state.team = self._resolve(actor.endpoint, prop.value, "teamInfo")
                    state.team_evidence = context
                elif prop.handle == 64:
                    state.array_index = int(prop.value)
                    state.array_evidence = context
                elif prop.handle == 63:
                    state.seat_index = int(prop.value)
                    state.seat_evidence = context
                records.extend(self._maybe_seat(actor))
        return records


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest().upper()


def _write_record(output: TextIO, record: dict) -> None:
    output.write(json.dumps(record, separators=(",", ":"), sort_keys=False))
    output.write("\n")


def _open_output(path: Optional[Path], overwrite: bool) -> tuple[TextIO, bool]:
    if path is None:
        return sys.stdout, False
    path = path.resolve()
    path.parent.mkdir(parents=True, exist_ok=True)
    mode = "w" if overwrite else "x"
    return path.open(mode, encoding="utf-8", newline="\n"), True


def _iter_tshark_rows(
    tshark: Path, pcap: Path, server_port: int, frame_start: Optional[int],
    frame_end: Optional[int],
) -> tuple[subprocess.Popen, Iterable[str]]:
    display_filter = "udp.srcport=={}".format(server_port)
    if frame_start is not None:
        display_filter += " && frame.number>={}".format(frame_start)
    if frame_end is not None:
        display_filter += " && frame.number<={}".format(frame_end)
    command = [
        str(tshark), "-r", str(pcap), "-Y", display_filter, "-T", "fields",
        "-e", "frame.number", "-e", "frame.time_relative",
        "-e", "ip.dst", "-e", "udp.dstport", "-e", "data.data",
    ]
    process = subprocess.Popen(
        command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        encoding="utf-8", errors="replace",
    )
    assert process.stdout is not None
    return process, process.stdout


def extract(args: argparse.Namespace, output: TextIO) -> int:
    pcap = Path(args.pcap).resolve()
    tshark = Path(args.tshark).resolve()
    if not pcap.is_file():
        raise EvidenceError("pcap does not exist or is not a regular file")
    if not tshark.is_file():
        raise EvidenceError("tshark does not exist or is not a regular file")
    size = pcap.stat().st_size
    if size <= 0 or size > args.max_capture_mib * 1024 * 1024:
        raise EvidenceError("pcap size exceeds the configured bound")

    expected_sha = args.expected_sha256.upper()
    if len(expected_sha) != 64 or any(ch not in "0123456789ABCDEF" for ch in expected_sha):
        raise EvidenceError("expected SHA-256 must be exactly 64 hexadecimal characters")
    actual_sha = _sha256_file(pcap)
    if actual_sha != expected_sha:
        raise EvidenceError(
            "pcap SHA-256 mismatch: expected {}, got {}".format(expected_sha, actual_sha)
        )

    _write_record(output, {
        "record": "header",
        "schema": SCHEMA,
        "captureName": pcap.name,
        "captureBytes": size,
        "captureSha256": actual_sha,
        "serverPort": args.server_port,
        "package": "ROGameContent",
        "packageGuid": ROGAMECONTENT_PACKAGE_GUID,
        "packageMapObjectBase": ROGAMECONTENT_OBJECT_BASE,
        "classResolution": "ObjectBase + compiled package export index",
        "decodedVehiclePrefix": "staticObjectRef+compressedLocation",
        "vehicleRotationAndPropertyTail": "opaque",
        "wholeBunchTypedFields": {
            "ROVehicleFactory": [23, 24],
            "ROTeamInfo": [48, 49, 50, 72],
            "ROPlayerReplicationInfo": [35, 63, 64],
        },
        "partialPropertyMatches": "opaque",
    })

    process, rows = _iter_tshark_rows(
        tshark, pcap, args.server_port, args.frame_start, args.frame_end
    )
    decoded_packets = 0
    packet_errors = 0
    lifecycle_invalidations = 0
    invalidated_live_actors = 0
    post_gap_discarded_bunches = 0
    vehicle_errors = 0
    typed_errors = 0
    emitted_records = 0
    vehicle_opens = 0
    typed_blocks = 0
    opaque_known_actor_bunches = 0
    grounded_factory_opens = 0
    factory_child_observations = 0
    team_vehicle_observations = 0
    pri_seat_observations = 0
    correlation_counts = {}
    property_counts = {
        "bHasLockedVehicle": 0,
        "ChildVehicle": 0,
        "TeamHelicopterPilotNames": 0,
        "TeamHelicopterLocationArray": 0,
        "TeamHelicopterArray": 0,
        "TeamHelicopterRep": 0,
        "Team": 0,
        "TeamHelicopterSeatIndex": 0,
        "TeamHelicopterArrayIndex": 0,
    }
    counts = {spec.class_path: 0 for spec in ARCHETYPES.values()}
    correlator = EvidenceCorrelator()
    post_gap_channels = {}
    post_gap_discard_ranges = {}

    def emit(record: dict) -> None:
        nonlocal emitted_records
        if emitted_records >= args.max_events:
            process.kill()
            process.wait()
            raise EvidenceError("evidence record count exceeds --max-events")
        _write_record(output, record)
        emitted_records += 1

    def invalidate_for_packet(
        endpoint: str, frame: int, timestamp: float, datagram: bytes,
        packet: dict,
    ) -> None:
        """Record and enforce an endpoint-wide channel-identity barrier."""
        nonlocal lifecycle_invalidations, invalidated_live_actors
        invalidated = correlator.invalidate_endpoint(endpoint)
        lifecycle_invalidations += 1
        invalidated_live_actors += len(invalidated)
        for actor in invalidated:
            post_gap_channels[(endpoint, actor.channel)] = {
                "actor": actor,
                "invalidationFrame": frame,
                "invalidationPacketId": packet.get("pid"),
            }
        emit({
            "record": "channelLifecycleInvalidation",
            "frame": frame,
            "timeSeconds": timestamp,
            "clientEndpoint": endpoint,
            "packetId": packet.get("pid"),
            "datagramBytes": len(datagram),
            "datagramBits": len(datagram) * 8,
            "datagramSha256": hashlib.sha256(datagram).hexdigest().upper(),
            "decoderReason": packet.get("reason", "bitReaderOverflow"),
            "decoderEndBit": packet.get("endpos"),
            "datagramTerminatorBit": packet.get("termbit"),
            "partiallyDecodedBunchesDiscarded": len(packet.get("bunches", [])),
            "invalidatedActorCount": len(invalidated),
            "invalidatedActors": [_actor_json(actor) for actor in invalidated],
            "disposition": "allLiveTypedChannelIdentitiesDropped",
        })

    for line in rows:
        columns = line.rstrip("\r\n").split("\t")
        if len(columns) < 5 or not columns[4]:
            continue
        try:
            frame = int(columns[0])
            timestamp = float(columns[1])
            destination = columns[2]
            destination_port = int(columns[3])
            if not destination:
                raise ValueError("missing destination address")
            endpoint = "{}:{}".format(destination, destination_port)
            datagram = bytes.fromhex(columns[4])
        except ValueError:
            packet_errors += 1
            continue

        packet = mc.decode_packet(datagram, bd_max=MAX_BUNCH_BITS)
        if not packet.get("ok"):
            packet_errors += 1
            invalidate_for_packet(endpoint, frame, timestamp, datagram, packet)
            continue
        decoded_packets += 1
        for bunch in packet.get("bunches", []):
            channel = int(bunch.get("chIndex", 0))
            payload_bits = int(bunch.get("bits", 0))
            try:
                payload = bytes.fromhex(bunch.get("payloadHex", ""))
            except ValueError:
                packet_errors += 1
                continue
            payload_digest = hashlib.sha256(payload).hexdigest().upper()

            gap_key = (endpoint, channel)
            gap = post_gap_channels.get(gap_key)
            if bunch.get("bOpen"):
                # A decoded open is an explicit lifetime boundary.  It may be
                # unsupported, but it can no longer be mistaken for the actor
                # that was live before the decode gap.
                post_gap_channels.pop(gap_key, None)
            elif gap is not None:
                post_gap_discarded_bunches += 1
                range_key = (
                    endpoint, channel, gap["actor"].generation,
                    gap["invalidationFrame"],
                )
                discard = post_gap_discard_ranges.get(range_key)
                if discard is None:
                    discard = {
                        "clientEndpoint": endpoint,
                        "channel": channel,
                        "discardedActor": _actor_json(gap["actor"]),
                        "invalidationFrame": gap["invalidationFrame"],
                        "invalidationPacketId": gap["invalidationPacketId"],
                        "firstDiscardFrame": frame,
                        "lastDiscardFrame": frame,
                        "firstDiscardPacketId": packet.get("pid"),
                        "lastDiscardPacketId": packet.get("pid"),
                        "discardedBunches": 0,
                    }
                    post_gap_discard_ranges[range_key] = discard
                discard["lastDiscardFrame"] = frame
                discard["lastDiscardPacketId"] = packet.get("pid")
                discard["discardedBunches"] += 1
                if bunch.get("bClose"):
                    post_gap_channels.pop(gap_key, None)

            actor = correlator.current_actor(endpoint, channel)
            property_start = 0
            if bunch.get("bOpen") and bunch.get("chType") == 2:
                # An open always starts a new channel lifetime.  Unknown opens
                # deliberately evict an old typed identity too.
                correlator.close_actor(endpoint, channel)
                actor = None
                static_index = None
                if payload_bits >= 32:
                    ref_reader = mc.BitReader(payload, payload_bits)
                    is_dynamic = bool(ref_reader.bit())
                    candidate_index = ref_reader.rint(
                        1024 if is_dynamic else STATIC_OBJECT_MAX
                    )
                    if not ref_reader.error and not is_dynamic:
                        static_index = candidate_index

                try:
                    prefix = decode_vehicle_open(payload, payload_bits)
                except EvidenceError:
                    if static_index in ARCHETYPES:
                        vehicle_errors += 1
                    prefix = None

                if prefix is not None:
                    location = {
                        "x": prefix.location[0], "y": prefix.location[1],
                        "z": prefix.location[2],
                    }
                    vehicle_details = {
                        "classStaticIndex": prefix.archetype.static_index,
                        "classPath": prefix.archetype.class_path,
                        "location": location,
                    }
                    actor = correlator.open_actor(
                        endpoint, channel, "vehicle", prefix.archetype.class_path,
                        frame, vehicle_details,
                    )
                    record = {
                        "record": "vehicleOpen",
                        "frame": frame,
                        "timeSeconds": timestamp,
                        "clientEndpoint": endpoint,
                        "packetId": packet.get("pid"),
                        "channel": channel,
                        "channelGeneration": actor.generation,
                        "channelSequence": int(bunch.get("chSeq", 0)),
                        "reliable": bool(bunch.get("bReliable")),
                        "open": True,
                        "close": bool(bunch.get("bClose")),
                        "payloadBits": payload_bits,
                        "payloadSha256": payload_digest,
                        "classStaticIndex": prefix.archetype.static_index,
                        "classPath": prefix.archetype.class_path,
                        "classPackageExportIndex": prefix.archetype.package_export_index,
                        "classMaxHandle": prefix.archetype.max_handle,
                        "actorIdentity": {
                            "kind": "dynamicChannel", "channel": channel,
                            "generation": actor.generation,
                        },
                        "location": location,
                        "locationPrefixBits": prefix.bits_consumed,
                        "remainingBitsState": "opaque",
                        "remainingBits": payload_bits - prefix.bits_consumed,
                    }
                    if args.include_payload:
                        record["payloadHex"] = payload.hex()
                    emit(record)
                    vehicle_opens += 1
                    counts[prefix.archetype.class_path] += 1

                elif static_index == TEAMINFO_STATIC_INDEX:
                    actor = correlator.open_actor(
                        endpoint, channel, "teamInfo", "ROGame.ROTeamInfo", frame
                    )
                    try:
                        prefix_reader = mc.BitReader(payload, payload_bits)
                        _decode_object_ref(prefix_reader)
                        _decode_compressed_vector(prefix_reader)
                        property_start = prefix_reader.p
                    except EvidenceError:
                        typed_errors += 1
                        property_start = payload_bits

                elif static_index == PRI_STATIC_INDEX:
                    actor = correlator.open_actor(
                        endpoint, channel, "pri",
                        "ROGame.ROPlayerReplicationInfo", frame,
                    )
                    try:
                        prefix_reader = mc.BitReader(payload, payload_bits)
                        _decode_object_ref(prefix_reader)
                        _decode_compressed_vector(prefix_reader)
                        property_start = prefix_reader.p
                    except EvidenceError:
                        typed_errors += 1
                        property_start = payload_bits

                elif static_index in FACTORY_ARCHETYPES:
                    factory_spec = FACTORY_ARCHETYPES[static_index]
                    actor = correlator.open_actor(
                        endpoint, channel, "factory", factory_spec.class_path, frame
                    )
                    grounded_factory_opens += 1
                    # Factory initial rotation/property split is not yet pinned.
                    property_start = payload_bits

            if (actor is not None and actor.kind in ("factory", "teamInfo", "pri") and
                    payload_bits > property_start and
                    (not bunch.get("bOpen") or actor.kind in ("teamInfo", "pri"))):
                try:
                    properties = decode_target_property_block(
                        payload, payload_bits, actor.kind, property_start
                    )
                except EvidenceError:
                    typed_errors += 1
                    properties = None
                if properties is None:
                    opaque_known_actor_bunches += 1
                elif properties:
                    context = EvidenceContext(
                        frame, timestamp, packet.get("pid"), payload_bits, payload_digest
                    )
                    typed_record = {
                        "record": "typedPropertyBlock",
                        "frame": frame,
                        "timeSeconds": timestamp,
                        "clientEndpoint": endpoint,
                        "packetId": packet.get("pid"),
                        "sourceActor": _actor_json(actor),
                        "channelSequence": int(bunch.get("chSeq", 0)),
                        "reliable": bool(bunch.get("bReliable")),
                        "open": bool(bunch.get("bOpen")),
                        "close": bool(bunch.get("bClose")),
                        "payloadBits": payload_bits,
                        "payloadSha256": payload_digest,
                        "propertyStartBit": property_start,
                        "wholeBunchTyped": True,
                        "properties": [_property_json(prop) for prop in properties],
                    }
                    if args.include_payload:
                        typed_record["payloadHex"] = payload.hex()
                    emit(typed_record)
                    typed_blocks += 1
                    for prop in properties:
                        property_counts[prop.name] += 1
                        if actor.kind == "factory" and prop.handle == 24:
                            factory_child_observations += 1
                        elif actor.kind == "teamInfo" and prop.handle == 50:
                            team_vehicle_observations += 1
                        elif actor.kind == "pri" and prop.handle in (63, 64):
                            pri_seat_observations += 1
                    for correlation in correlator.observe_block(actor, properties, context):
                        emit(correlation)
                        kind = correlation["correlation"]
                        correlation_counts[kind] = correlation_counts.get(kind, 0) + 1

            if bunch.get("bClose"):
                correlator.close_actor(endpoint, channel)

    stderr = process.stderr.read() if process.stderr is not None else ""
    return_code = process.wait()
    if return_code != 0:
        raise EvidenceError("tshark failed: {}".format(stderr.strip() or return_code))

    for key in sorted(post_gap_discard_ranges):
        emit({
            "record": "postGapDiscardRange",
            **post_gap_discard_ranges[key],
            "disposition": "notTypedOrCorrelated",
        })

    _write_record(output, {
        "record": "summary",
        "decodedPackets": decoded_packets,
        "packetErrors": packet_errors,
        "channelLifecycleInvalidations": lifecycle_invalidations,
        "invalidatedLiveActors": invalidated_live_actors,
        "postGapDiscardedChannelBunches": post_gap_discarded_bunches,
        "postGapDiscardedChannelLifetimes": len(post_gap_discard_ranges),
        "vehiclePrefixErrors": vehicle_errors,
        "typedEvidenceErrors": typed_errors,
        "vehicleOpens": vehicle_opens,
        "typedPropertyBlocks": typed_blocks,
        "opaqueKnownActorBunches": opaque_known_actor_bunches,
        "groundedFactoryOpens": grounded_factory_opens,
        "factoryChildVehicleObservations": factory_child_observations,
        "teamHelicopterObjectObservations": team_vehicle_observations,
        "priSeatIndexObservations": pri_seat_observations,
        "unresolvedVehicleObjectRefs": correlator.unresolved_vehicle_refs,
        "vehicleCorrelations": correlation_counts,
        "decodableTransactions": {
            "typedPropertyBlocks": typed_blocks,
            "vehicleCorrelations": sum(correlation_counts.values()),
        },
        "typedPropertyObservations": property_counts,
        "factoryEvidenceState": (
            "wholeBunchChildVehicleObserved" if factory_child_observations else
            "groundedFactoryChannelObservedNoWholeBunchChildVehicle" if grounded_factory_opens else
            "noGroundedFactoryChannelObserved"
        ),
        "evidenceRecords": emitted_records,
        "counts": counts,
        "complete": packet_errors == 0 and vehicle_errors == 0 and typed_errors == 0,
    })
    return 0 if packet_errors == 0 and vehicle_errors == 0 and typed_errors == 0 else 67


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pcap", required=True)
    parser.add_argument(
        "--tshark", default=r"C:\Program Files\Wireshark\tshark.exe"
    )
    parser.add_argument("--server-port", type=int, default=7777)
    parser.add_argument("--frame-start", type=int)
    parser.add_argument("--frame-end", type=int)
    parser.add_argument("--max-capture-mib", type=int, default=512)
    parser.add_argument("--max-events", type=int, default=10000)
    parser.add_argument("--expected-sha256", default=KNOWN_CAPTURE_SHA256)
    parser.add_argument("--include-payload", action="store_true")
    parser.add_argument("--output")
    parser.add_argument("--overwrite", action="store_true")
    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    if not 1 <= args.server_port <= 65535:
        parser.error("--server-port must be in [1,65535]")
    if args.max_capture_mib <= 0 or args.max_events <= 0:
        parser.error("size/event bounds must be positive")
    if args.frame_start is not None and args.frame_start <= 0:
        parser.error("--frame-start must be positive")
    if args.frame_end is not None and args.frame_end <= 0:
        parser.error("--frame-end must be positive")
    if (args.frame_start is not None and args.frame_end is not None and
            args.frame_start > args.frame_end):
        parser.error("--frame-start must not exceed --frame-end")

    output = None
    owns_output = False
    try:
        output, owns_output = _open_output(
            Path(args.output) if args.output else None, args.overwrite
        )
        return extract(args, output)
    except (EvidenceError, OSError) as error:
        print("vehicle_capture_evidence: {}".format(error), file=sys.stderr)
        return 65
    finally:
        if output is not None:
            output.flush()
            if owns_output:
                output.close()


if __name__ == "__main__":
    raise SystemExit(main())
