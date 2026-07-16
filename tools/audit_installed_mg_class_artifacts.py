#!/usr/bin/env python3
"""Build the exact, sanitized M60/M61 cooked-class evidence report.

The auditor invokes the pinned ``class-artifacts`` mode of the cooked package
extractor against four exact Rising Storm 2 server packages.  It validates the
raw class/CDO/member records, derives the two zero-based replication handle
tables, and emits only sanitized package and class facts.  It never derives a
wire/static reference and never authorizes a runtime role or owning graph.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Mapping, Sequence


RAW_SCHEMA = "rs2.cooked-class-artifacts.raw.v1"
SCHEMA = "rs2.mg-class-artifacts.v1"
SCHEMA_VERSION = 1
MAX_RAW_RECORDS = 64
MAX_RAW_LINE_BYTES = 1_048_576
MAX_RAW_BYTES = 8 * 1_048_576
MAX_REPORT_BYTES = 2 * 1_048_576
REPORT_SPOOL_BYTES = 256 * 1024
EXTRACT_TIMEOUT_SECONDS = 240
BUILD_OUTPUT_BYTES = 8192

EXPECTED_WRAPPER_BYTES = 14_303
EXPECTED_WRAPPER_SHA256 = (
    "C18706D1B2E3E0A4462E5416623DC57E6764341E264206F5490D6C198EBA4B26"
)
EXPECTED_EXTRACTOR_SOURCE_NAME = "CookedMapMetadataExtractor.cs"
EXPECTED_EXTRACTOR_SOURCE_BYTES = 158_772
EXPECTED_EXTRACTOR_SOURCE_SHA256 = (
    "56F59A70ED8FD61416E57CAD3122E01F4796AA6AC4A04EECCE3776C5AEEEB26D"
)
EXPECTED_EXTRACTOR_EXECUTABLE_BYTES = 125_952
EXPECTED_EXTRACTOR_EXECUTABLE_SHA256 = (
    "087D939AAD765B3F73A32ADCD738BFCD757FF3D2371AC598BF71FE5230C988E2"
)
EXPECTED_EXTRACTOR_ASSEMBLY_IDENTITY = (
    "CookedMapMetadataExtractor, Version=0.0.0.0, "
    "Culture=neutral, PublicKeyToken=null"
)
EXPECTED_UELIB_BYTES = 366_592
EXPECTED_UELIB_SHA256 = (
    "56823D074102BA959784B44DBE961B5DBE825EE2CF1C5D02BC53F5B0EB91BCAE"
)
EXPECTED_UELIB_ASSEMBLY_IDENTITY = (
    "Eliot.UELib, Version=1.12.1.0, Culture=neutral, "
    "PublicKeyToken=d0d22cacd90fdb4a"
)
EXPECTED_UELIB_ASSEMBLY_VERSION = "1.12.1.0"
EXPECTED_UELIB_PRODUCT_VERSION = (
    "1.12.1+1af731ccb2ccfa61927fff5ffa9fdda64731ca0f"
)
EXPECTED_UNSAFE_NAME = "System.Runtime.CompilerServices.Unsafe.dll"
EXPECTED_UNSAFE_BYTES = 19_256
EXPECTED_UNSAFE_SHA256 = (
    "08CBD7278B66F1E68425A82D4B97181A4130D93E3DD91831407ABA7212CCDACF"
)
EXPECTED_UNSAFE_ASSEMBLY_IDENTITY = (
    "System.Runtime.CompilerServices.Unsafe, Version=6.0.3.0, "
    "Culture=neutral, PublicKeyToken=b03f5f7f11d50a3a"
)

REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_WRAPPER = REPO_ROOT / "tools" / "extract_cooked_map_metadata.ps1"
DEFAULT_UELIB = Path(r"D:\RE-Tools\UE-Explorer\Eliot.UELib.dll")
DEFAULT_PACKAGE_ROOT = Path(
    os.environ.get(
        "RS2_SERVER_PACKAGE_ROOT",
        r"D:\rs2dedicatedserver\ROGame\BrewedPCServer",
    )
)

FORBIDDEN_REFERENCE_KEYS = frozenset(
    {"objectbase", "staticreference", "wirereference"}
)
HASH_RE = re.compile(r"^[0-9A-F]{64}$")
GUID_RE = re.compile(r"^[0-9A-F]{32}$")
FLAG8_RE = re.compile(r"^0x[0-9A-F]{8}$")
FLAG16_RE = re.compile(r"^0x[0-9A-F]{16}$")
CLASS_PATH_RE = re.compile(r"^Class'([A-Za-z0-9_]+)\.([A-Za-z0-9_]+)'$")
MEMBER_NAME_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
ABSOLUTE_PATH_RE = re.compile(r"(?:^[A-Za-z]:[\\/]|^/|^\\\\)")

HEADER_KEYS = frozenset(
    {
        "record",
        "schema",
        "mode",
        "classPattern",
        "expectedClassCount",
        "artifactBytes",
        "artifactSha256",
        "extractorAssemblyIdentity",
        "extractorBytes",
        "extractorSha256",
        "uelibAssemblyIdentity",
        "uelibAssemblyVersion",
        "uelibProductVersion",
        "uelibBytes",
        "uelibSha256",
        "unsafeAssemblyIdentity",
        "unsafeBytes",
        "unsafeSha256",
        "wireStaticReferencesDerived",
        "reportAuthorizesRuntime",
    }
)
PACKAGE_KEYS = frozenset(
    {
        "record",
        "package",
        "artifactBytes",
        "artifactSha256",
        "rawGuid",
        "packageGuid",
        "packageFlags",
        "packageVersion",
        "licenseeVersion",
        "engineVersion",
        "generationCount",
        "generations",
    }
)
GENERATION_KEYS = frozenset({"ordinal", "exports", "names", "netObjects"})
CLASS_KEYS = frozenset(
    {
        "record",
        "classPath",
        "superClassPath",
        "superTableReference",
        "UClass",
        "CDO",
        "declaredNetworkMembers",
        "directBNetInitialRotationTags",
    }
)
UCLASS_KEYS = frozenset({"exportIndex", "uobjectNetIndex"})
CDO_KEYS = frozenset(
    {
        "path",
        "exportIndex",
        "uobjectNetIndex",
        "classTableReference",
        "classLinkVerified",
        "serialOffset",
        "serialSize",
        "objectFlags",
    }
)
MEMBER_KEYS = frozenset(
    {
        "name",
        "kind",
        "uobjectNetIndex",
        "propertyFlags",
        "functionFlags",
        "uelibType",
        "arrayDim",
        "functionSuperPresent",
        "specialDeclaration",
    }
)
TAG_KEYS = frozenset({"propertyPath", "value", "valueState", "sourceOffset"})
SUMMARY_KEYS = frozenset(
    {
        "record",
        "schema",
        "mode",
        "classesEmitted",
        "classLinksVerifiedDirectly",
        "wireStaticReferencesDerived",
        "reportAuthorizesRuntime",
    }
)
DENIAL_KEYS = frozenset(_runtime_key for _runtime_key in (
    "wireStaticReferencesDerived",
    "packageMapGrounded",
    "candidateRecordsGrounded",
    "owningGraphProven",
    "reportAuthorizesRuntime",
    "runtimeAuthorizes",
    "runtimeAuthorized",
    "complete",
))
FINAL_HEADER_KEYS = frozenset(
    {
        "record",
        "schema",
        "schemaVersion",
        "artifacts",
        "extractorWrapperBytes",
        "extractorWrapperSha256",
        "extractorSourceBytes",
        "extractorSourceSha256",
        "extractorExecutableBytes",
        "extractorExecutableSha256",
        "uelibBytes",
        "uelibSha256",
        "uelibAssemblyVersion",
        "uelibProductVersion",
        "uelibDependencies",
    }
) | DENIAL_KEYS
FINAL_ARTIFACT_KEYS = frozenset(
    {
        "artifact",
        "artifactBytes",
        "artifactSha256",
        "packageGuid",
        "rawGuid",
        "packageFlags",
        "packageVersion",
        "licenseeVersion",
        "engineVersion",
        "generations",
    }
)
FINAL_TARGET_KEYS = frozenset(
    {
        "record",
        "schema",
        "target",
        "terminalClassPath",
        "terminalUClassNetIndex",
        "terminalCDONetIndex",
        "handleIndexBase",
        "classChain",
        "handles",
        "maxHandle",
        "maxHandleExclusive",
        "spotHandles",
    }
) | DENIAL_KEYS
FINAL_CLASS_RANGE_KEYS = frozenset(
    {
        "classPath",
        "directNetworkMemberCount",
        "overriddenNetworkFunctions",
        "firstHandle",
        "lastHandle",
    }
)
FINAL_OVERRIDE_KEYS = frozenset({"name", "uobjectNetIndex"})
FINAL_HANDLE_KEYS = frozenset(
    {
        "handle",
        "declaringClassPath",
        "name",
        "kind",
        "uobjectNetIndex",
        "propertyFlags",
        "functionFlags",
        "arrayDim",
        "functionSuperPresent",
    }
)
FINAL_SPOT_KEYS = frozenset(
    {"handle", "declaringClassPath", "name", "uobjectNetIndex"}
)
FINAL_DEFAULT_KEYS = frozenset(
    {
        "record",
        "schema",
        "propertyPath",
        "declarationKind",
        "uelibType",
        "uobjectNetIndex",
        "propertyFlags",
        "arrayDim",
        "cpfNet",
        "specialDeclaration",
        "ue3ImplicitZeroDefaultRuleApplied",
        "classesCheckedForDirectOverride",
        "directSerializedOverrideCount",
        "effectiveValue",
        "effectiveFalseProven",
    }
) | DENIAL_KEYS
FINAL_SUMMARY_KEYS = frozenset(
    {
        "record",
        "schema",
        "schemaVersion",
        "artifacts",
        "uniqueClasses",
        "targets",
        "handleRows",
        "m60MaxHandle",
        "m61ContentSingleMaxHandle",
        "bNetInitialRotationEffective",
        "reportContainsLocalPaths",
    }
) | DENIAL_KEYS


class AuditError(ValueError):
    """Raised when an installed artifact or extracted fact is not exact."""


@dataclass(frozen=True)
class PackageSpec:
    name: str
    pattern: str
    class_names: tuple[str, ...]
    artifact_bytes: int
    artifact_sha256: str
    package_flags: str
    package_guid: str
    raw_guid: str
    generations: tuple[tuple[int, int, int], ...]

    @property
    def filename(self) -> str:
        return self.name + ".u"

    @property
    def expected_count(self) -> int:
        return len(self.class_names)


PACKAGE_SPECS = (
    PackageSpec(
        "Core",
        r"^(Object)$",
        ("Object",),
        226_734,
        "9F48070EEFF458478792677B6E3D3CCB6A94678A070EB602D6FE00B51EBE1E6A",
        "0x20204000",
        "4E98E1A84B17D0773382759988198719",
        "A8E1984E77D0174B9975823319871988",
        ((1535, 803, 1535), (1535, 803, 1535)),
    ),
    PackageSpec(
        "Engine",
        r"^(Actor|Inventory|Weapon)$",
        ("Actor", "Inventory", "Weapon"),
        196_406_917,
        "068946B520AA5DC81F22E0DBB6CE78B096F171E88A2E25608FB49D261E48D98E",
        "0x20204000",
        "7AE12CB344747678743E5ABD12E28DA7",
        "B32CE17A78767444BD5A3E74A78DE212",
        ((37942, 22296, 37942), (37943, 23123, 37943)),
    ),
    PackageSpec(
        "ROGame",
        (
            r"^(ROWeapon|ROProjectileWeapon|ROBipodWeapon|ROMGWeapon|"
            r"ROWeap_M60_GPMG|ROOneShotWeapon|ROExplosiveWeapon|"
            r"ROEggGrenadeWeapon|ROWeap_M61_Grenade)$"
        ),
        (
            "ROWeapon",
            "ROProjectileWeapon",
            "ROBipodWeapon",
            "ROMGWeapon",
            "ROWeap_M60_GPMG",
            "ROOneShotWeapon",
            "ROExplosiveWeapon",
            "ROEggGrenadeWeapon",
            "ROWeap_M61_Grenade",
        ),
        26_584_752,
        "06D63FF85F2C9BC740E50FC127AE5DF4DFF8AD2678DA615C44184EF4D9D71A02",
        "0x20204001",
        "33EE724F43F851351795FD975E8D5AC1",
        "4F72EE333551F84397FD9517C15A8D5E",
        ((64471, 48070, 64471), (64472, 48357, 64472)),
    ),
    PackageSpec(
        "ROGameContent",
        (
            r"^(ROWeap_M60_GPMG_Content|ROWeap_M61_Grenade_Content|"
            r"ROWeap_M61_Grenade_ContentSingle)$"
        ),
        (
            "ROWeap_M60_GPMG_Content",
            "ROWeap_M61_Grenade_Content",
            "ROWeap_M61_Grenade_ContentSingle",
        ),
        23_934_851,
        "2D6433144F00EB130D15193C414A4F401FCA40806AEFC9A6342B7670E376F992",
        "0x20204001",
        "FE4B4F2F4B3128C42FE5098ACAB560C8",
        "2F4F4BFEC428314B8A09E52FC860B5CA",
        ((2351, 3800, 2351), (2352, 4093, 2352)),
    ),
)

SUPER_CLASS_PATHS: Mapping[str, str | None] = {
    "Class'Core.Object'": None,
    "Class'Engine.Actor'": "Class'Core.Object'",
    "Class'Engine.Inventory'": "Class'Engine.Actor'",
    "Class'Engine.Weapon'": "Class'Engine.Inventory'",
    "Class'ROGame.ROWeapon'": "Class'Engine.Weapon'",
    "Class'ROGame.ROProjectileWeapon'": "Class'ROGame.ROWeapon'",
    "Class'ROGame.ROBipodWeapon'": "Class'ROGame.ROProjectileWeapon'",
    "Class'ROGame.ROMGWeapon'": "Class'ROGame.ROBipodWeapon'",
    "Class'ROGame.ROWeap_M60_GPMG'": "Class'ROGame.ROMGWeapon'",
    "Class'ROGame.ROOneShotWeapon'": "Class'ROGame.ROWeapon'",
    "Class'ROGame.ROExplosiveWeapon'": "Class'ROGame.ROOneShotWeapon'",
    "Class'ROGame.ROEggGrenadeWeapon'": "Class'ROGame.ROExplosiveWeapon'",
    "Class'ROGame.ROWeap_M61_Grenade'": "Class'ROGame.ROEggGrenadeWeapon'",
    "Class'ROGameContent.ROWeap_M60_GPMG_Content'": (
        "Class'ROGame.ROWeap_M60_GPMG'"
    ),
    "Class'ROGameContent.ROWeap_M61_Grenade_Content'": (
        "Class'ROGame.ROWeap_M61_Grenade'"
    ),
    "Class'ROGameContent.ROWeap_M61_Grenade_ContentSingle'": (
        "Class'ROGameContent.ROWeap_M61_Grenade_Content'"
    ),
}

M60_CHAIN = (
    "Class'Core.Object'",
    "Class'Engine.Actor'",
    "Class'Engine.Inventory'",
    "Class'Engine.Weapon'",
    "Class'ROGame.ROWeapon'",
    "Class'ROGame.ROProjectileWeapon'",
    "Class'ROGame.ROBipodWeapon'",
    "Class'ROGame.ROMGWeapon'",
    "Class'ROGame.ROWeap_M60_GPMG'",
    "Class'ROGameContent.ROWeap_M60_GPMG_Content'",
)
M60_COUNTS = (0, 23, 3, 5, 68, 0, 3, 10, 0, 0)
M61_CHAIN = (
    "Class'Core.Object'",
    "Class'Engine.Actor'",
    "Class'Engine.Inventory'",
    "Class'Engine.Weapon'",
    "Class'ROGame.ROWeapon'",
    "Class'ROGame.ROOneShotWeapon'",
    "Class'ROGame.ROExplosiveWeapon'",
    "Class'ROGame.ROEggGrenadeWeapon'",
    "Class'ROGame.ROWeap_M61_Grenade'",
    "Class'ROGameContent.ROWeap_M61_Grenade_Content'",
    "Class'ROGameContent.ROWeap_M61_Grenade_ContentSingle'",
)
M61_COUNTS = (0, 23, 3, 5, 68, 0, 2, 0, 0, 0, 0)

TARGETS = (
    (
        "M60",
        M60_CHAIN,
        M60_COUNTS,
        112,
        "Class'ROGameContent.ROWeap_M60_GPMG_Content'",
        512,
        513,
    ),
    (
        "M61-ContentSingle",
        M61_CHAIN,
        M61_COUNTS,
        101,
        "Class'ROGameContent.ROWeap_M61_Grenade_ContentSingle'",
        529,
        530,
    ),
)

SPOT_HANDLES: Mapping[int, tuple[str, str, int]] = {
    4: ("Class'Engine.Actor'", "Instigator", 2502),
    21: ("Class'Engine.Actor'", "bHidden", 2626),
    96: ("Class'ROGame.ROWeapon'", "bUserConfigApplied", 3062),
}
EXPECTED_OVERRIDDEN_NETWORK_FUNCTIONS: Mapping[
    str, tuple[tuple[str, int], ...]
] = {
    "Class'Engine.Weapon'": (("ClientGivenTo", 32874),),
    "Class'ROGame.ROWeapon'": (
        ("ServerStartFire", 424),
        ("ClientWeaponSet", 1272),
        ("ServerWarnPawnsAlongFireLine", 1479),
        ("ClientWeaponThrown", 1874),
    ),
    "Class'ROGame.ROBipodWeapon'": (
        ("ServerZoomIn", 26125),
        ("ServerZoomInModified", 26128),
    ),
    "Class'ROGame.ROExplosiveWeapon'": (
        ("ServerStartFire", 29204),
        ("ServerStopFire", 29220),
    ),
}


def _json(value: Mapping[str, object]) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"))


def _runtime_denials() -> dict[str, bool]:
    return {
        "wireStaticReferencesDerived": False,
        "packageMapGrounded": False,
        "candidateRecordsGrounded": False,
        "owningGraphProven": False,
        "reportAuthorizesRuntime": False,
        "runtimeAuthorizes": False,
        "runtimeAuthorized": False,
        "complete": False,
    }


def _reject_duplicate_pairs(pairs: Sequence[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise AuditError("extractor JSON contains a duplicate key")
        result[key] = value
    return result


def _reject_forbidden_keys(value: object) -> None:
    if isinstance(value, Mapping):
        for key, child in value.items():
            if not isinstance(key, str):
                raise AuditError("JSON object key is not a string")
            if key.casefold() in FORBIDDEN_REFERENCE_KEYS:
                raise AuditError("forbidden runtime-reference key in class evidence")
            _reject_forbidden_keys(child)
    elif isinstance(value, (list, tuple)):
        for child in value:
            _reject_forbidden_keys(child)


def load_jsonl(path: Path) -> tuple[dict[str, object], ...]:
    records: list[dict[str, object]] = []
    total_bytes = 0
    try:
        with path.open("rb") as source:
            for line_number, raw_line in enumerate(source, 1):
                total_bytes += len(raw_line)
                if total_bytes > MAX_RAW_BYTES:
                    raise AuditError("extractor JSONL exceeds the total byte bound")
                if line_number > MAX_RAW_RECORDS:
                    raise AuditError("extractor JSONL exceeds the record bound")
                if len(raw_line) > MAX_RAW_LINE_BYTES:
                    raise AuditError("extractor JSONL line exceeds the byte bound")
                if not raw_line.strip():
                    raise AuditError("extractor JSONL contains a blank record")
                try:
                    line = raw_line.decode("utf-8")
                    value = json.loads(line, object_pairs_hook=_reject_duplicate_pairs)
                except (UnicodeDecodeError, json.JSONDecodeError) as exc:
                    raise AuditError("extractor JSONL is not exact UTF-8 JSON") from exc
                if not isinstance(value, dict):
                    raise AuditError("extractor JSONL record is not an object")
                _reject_forbidden_keys(value)
                records.append(value)
    except OSError as exc:
        raise AuditError("could not read bounded extractor JSONL") from exc
    if not records:
        raise AuditError("extractor JSONL is empty")
    return tuple(records)


def file_identity(path: Path) -> tuple[int, str]:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as source:
            while chunk := source.read(1024 * 1024):
                digest.update(chunk)
        return path.stat().st_size, digest.hexdigest().upper()
    except OSError as exc:
        raise AuditError("could not hash a pinned input") from exc


def validate_file_identity(
    path: Path, expected_bytes: int, expected_sha256: str, label: str
) -> None:
    actual_bytes, actual_sha256 = file_identity(path)
    if actual_bytes != expected_bytes or actual_sha256 != expected_sha256:
        raise AuditError(f"{label} identity drift")


def _expect_keys(record: Mapping[str, object], expected: frozenset[str], label: str) -> None:
    if set(record) != expected:
        raise AuditError(f"{label} property set drift")


def _expect(record: Mapping[str, object], field: str, expected: object, label: str) -> None:
    if record.get(field) != expected or type(record.get(field)) is not type(expected):
        raise AuditError(f"{label} {field} drift")


def _exact_int(value: object, label: str, minimum: int = 0) -> int:
    if type(value) is not int or value < minimum:
        raise AuditError(f"{label} is not an exact bounded integer")
    return value


def _exact_optional_bool(value: object, label: str) -> bool | None:
    if value is None:
        return None
    if type(value) is not bool:
        raise AuditError(f"{label} is not a boolean or null")
    return value


def _exact_optional_int(value: object, label: str) -> int | None:
    if value is None:
        return None
    return _exact_int(value, label)


def _exact_optional_string(value: object, label: str) -> str | None:
    if value is None:
        return None
    if not isinstance(value, str):
        raise AuditError(f"{label} is not a string or null")
    return value


def _class_path(package: str, class_name: str) -> str:
    return f"Class'{package}.{class_name}'"


def _validate_member(
    raw: object, class_path: str, ordinal: int
) -> dict[str, object]:
    if not isinstance(raw, Mapping):
        raise AuditError("declared member is not an object")
    _expect_keys(raw, MEMBER_KEYS, "declared member")
    name = raw.get("name")
    kind = raw.get("kind")
    net_index = _exact_int(raw.get("uobjectNetIndex"), "member UObject.NetIndex")
    if not isinstance(name, str) or MEMBER_NAME_RE.fullmatch(name) is None:
        raise AuditError("declared member name drift")
    if kind not in ("property", "function"):
        raise AuditError("declared member kind drift")
    uelib_type = raw.get("uelibType")
    if not isinstance(uelib_type, str) or not uelib_type.startswith("UELib.Core.U"):
        raise AuditError("declared member UELib type drift")
    special = raw.get("specialDeclaration")
    if type(special) is not bool:
        raise AuditError("declared member special marker is not boolean")

    property_flags = _exact_optional_string(raw.get("propertyFlags"), "property flags")
    function_flags = _exact_optional_string(raw.get("functionFlags"), "function flags")
    array_dim = _exact_optional_int(raw.get("arrayDim"), "property array dimension")
    function_super = _exact_optional_bool(
        raw.get("functionSuperPresent"), "function super marker"
    )
    if kind == "property":
        if property_flags is None or FLAG16_RE.fullmatch(property_flags) is None:
            raise AuditError("property flags are not exact")
        if function_flags is not None or array_dim is None or array_dim < 1:
            raise AuditError("property nullable-field contract drift")
        if function_super is not None:
            raise AuditError("property function-super field must be null")
        flag_value = int(property_flags[2:], 16)
        if not special and (flag_value & 0x20) == 0:
            raise AuditError("non-special property is not CPF_Net")
    else:
        if function_flags is None or FLAG16_RE.fullmatch(function_flags) is None:
            raise AuditError("function flags are not exact")
        if property_flags is not None or array_dim is not None:
            raise AuditError("function nullable-field contract drift")
        if function_super is None or (int(function_flags[2:], 16) & 0x40) == 0:
            raise AuditError("network function contract drift")
        if special:
            raise AuditError("function cannot be a special declaration")

    expected_special = (
        class_path == "Class'Engine.Actor'" and name == "bNetInitialRotation"
    )
    if special != expected_special:
        raise AuditError("special declaration identity drift")
    if expected_special and (
        kind != "property"
        or property_flags != "0x0000000000000002"
        or uelib_type != "UELib.Core.UBoolProperty"
        or array_dim != 1
    ):
        raise AuditError("Actor.bNetInitialRotation declaration drift")

    return {
        "ordinal": ordinal,
        "name": name,
        "kind": kind,
        "uobjectNetIndex": net_index,
        "propertyFlags": property_flags,
        "functionFlags": function_flags,
        "uelibType": uelib_type,
        "arrayDim": array_dim,
        "functionSuperPresent": function_super,
        "specialDeclaration": special,
    }


def _validate_class_record(
    raw: Mapping[str, object], expected_path: str
) -> dict[str, object]:
    _expect_keys(raw, CLASS_KEYS, "class record")
    _expect(raw, "record", "class", "class record")
    _expect(raw, "classPath", expected_path, "class record")
    expected_super = SUPER_CLASS_PATHS[expected_path]
    if raw.get("superClassPath") != expected_super:
        raise AuditError("class super path drift")
    if raw.get("superClassPath") is not None and not isinstance(
        raw.get("superClassPath"), str
    ):
        raise AuditError("class super path type drift")
    super_reference = raw.get("superTableReference")
    if type(super_reference) is not int:
        raise AuditError("class super table reference type drift")
    if expected_super is None and super_reference != 0:
        raise AuditError("root class has a non-null super table reference")
    if expected_super is not None and super_reference == 0:
        raise AuditError("derived class has a null super table reference")

    uclass = raw.get("UClass")
    cdo = raw.get("CDO")
    if not isinstance(uclass, Mapping) or not isinstance(cdo, Mapping):
        raise AuditError("class identity objects are missing")
    _expect_keys(uclass, UCLASS_KEYS, "UClass identity")
    _expect_keys(cdo, CDO_KEYS, "CDO identity")
    uclass_export = _exact_int(uclass.get("exportIndex"), "UClass export index")
    uclass_net = _exact_int(uclass.get("uobjectNetIndex"), "UClass UObject.NetIndex")
    cdo_export = _exact_int(cdo.get("exportIndex"), "CDO export index")
    cdo_net = _exact_int(cdo.get("uobjectNetIndex"), "CDO UObject.NetIndex")
    class_reference = _exact_int(
        cdo.get("classTableReference"), "CDO class table reference", 1
    )
    if class_reference != uclass_export + 1:
        raise AuditError("CDO raw ClassTable reference drift")
    if cdo_export == uclass_export:
        raise AuditError("CDO and UClass exports alias")
    if cdo.get("classLinkVerified") is not True:
        raise AuditError("CDO direct UClass linkage was not verified")
    _exact_int(cdo.get("serialOffset"), "CDO serial offset")
    _exact_int(cdo.get("serialSize"), "CDO serial size")
    object_flags = cdo.get("objectFlags")
    if not isinstance(object_flags, str) or FLAG16_RE.fullmatch(object_flags) is None:
        raise AuditError("CDO object flags drift")

    match = CLASS_PATH_RE.fullmatch(expected_path)
    if match is None:
        raise AuditError("internal class path contract is invalid")
    expected_cdo_path = f"{match.group(2)}'{match.group(1)}.Default__{match.group(2)}'"
    if cdo.get("path") != expected_cdo_path:
        raise AuditError("CDO path drift")

    raw_members = raw.get("declaredNetworkMembers")
    if not isinstance(raw_members, list):
        raise AuditError("declared member list is missing")
    members = [
        _validate_member(member, expected_path, ordinal)
        for ordinal, member in enumerate(raw_members)
    ]
    ordering = [
        (member["uobjectNetIndex"], member["kind"], member["name"])
        for member in members
    ]
    if ordering != sorted(ordering) or len(ordering) != len(set(ordering)):
        raise AuditError("declared member order or identity is not exact")
    net_indices = [member["uobjectNetIndex"] for member in members]
    if len(net_indices) != len(set(net_indices)):
        raise AuditError("declared members reuse a UObject.NetIndex")
    raw_tags = raw.get("directBNetInitialRotationTags")
    if not isinstance(raw_tags, list):
        raise AuditError("direct bNetInitialRotation tag list is missing")
    for tag in raw_tags:
        if not isinstance(tag, Mapping):
            raise AuditError("direct bNetInitialRotation tag is not an object")
        _expect_keys(tag, TAG_KEYS, "direct bNetInitialRotation tag")
    if raw_tags:
        raise AuditError("a selected CDO serializes bNetInitialRotation directly")

    return {
        "classPath": expected_path,
        "superClassPath": expected_super,
        "UClass": {"exportIndex": uclass_export, "uobjectNetIndex": uclass_net},
        "CDO": {
            "exportIndex": cdo_export,
            "uobjectNetIndex": cdo_net,
            "classTableReference": class_reference,
        },
        "members": tuple(members),
    }


def validate_raw_package(
    spec: PackageSpec, records: Sequence[Mapping[str, object]]
) -> dict[str, dict[str, object]]:
    _reject_forbidden_keys(records)
    if len(records) != spec.expected_count + 3:
        raise AuditError(f"{spec.name} raw record count drift")
    header, package, *middle, summary = records

    _expect_keys(header, HEADER_KEYS, "raw header")
    for field, expected in (
        ("record", "header"),
        ("schema", RAW_SCHEMA),
        ("mode", "class-artifacts"),
        ("classPattern", spec.pattern),
        ("expectedClassCount", spec.expected_count),
        ("artifactBytes", spec.artifact_bytes),
        ("artifactSha256", spec.artifact_sha256),
        ("extractorAssemblyIdentity", EXPECTED_EXTRACTOR_ASSEMBLY_IDENTITY),
        ("extractorBytes", EXPECTED_EXTRACTOR_EXECUTABLE_BYTES),
        ("extractorSha256", EXPECTED_EXTRACTOR_EXECUTABLE_SHA256),
        ("uelibAssemblyIdentity", EXPECTED_UELIB_ASSEMBLY_IDENTITY),
        ("uelibAssemblyVersion", EXPECTED_UELIB_ASSEMBLY_VERSION),
        ("uelibProductVersion", EXPECTED_UELIB_PRODUCT_VERSION),
        ("uelibBytes", EXPECTED_UELIB_BYTES),
        ("uelibSha256", EXPECTED_UELIB_SHA256),
        ("unsafeAssemblyIdentity", EXPECTED_UNSAFE_ASSEMBLY_IDENTITY),
        ("unsafeBytes", EXPECTED_UNSAFE_BYTES),
        ("unsafeSha256", EXPECTED_UNSAFE_SHA256),
        ("wireStaticReferencesDerived", False),
        ("reportAuthorizesRuntime", False),
    ):
        _expect(header, field, expected, "raw header")

    _expect_keys(package, PACKAGE_KEYS, "raw package")
    for field, expected in (
        ("record", "package"),
        ("package", spec.name),
        ("artifactBytes", spec.artifact_bytes),
        ("artifactSha256", spec.artifact_sha256),
        ("rawGuid", spec.raw_guid),
        ("packageGuid", spec.package_guid),
        ("packageFlags", spec.package_flags),
        ("packageVersion", 765),
        ("licenseeVersion", 771),
        ("engineVersion", 7258),
        ("generationCount", len(spec.generations)),
    ):
        _expect(package, field, expected, "raw package")
    generations = package.get("generations")
    if not isinstance(generations, list) or len(generations) != len(spec.generations):
        raise AuditError("package generation list drift")
    for ordinal, (raw_generation, expected_counts) in enumerate(
        zip(generations, spec.generations, strict=True)
    ):
        if not isinstance(raw_generation, Mapping):
            raise AuditError("package generation is not an object")
        _expect_keys(raw_generation, GENERATION_KEYS, "package generation")
        expected_values = (ordinal, *expected_counts)
        actual_values = tuple(
            _exact_int(raw_generation.get(field), f"generation {field}")
            for field in ("ordinal", "exports", "names", "netObjects")
        )
        if actual_values != expected_values:
            raise AuditError("package generation tuple drift")

    expected_paths = {_class_path(spec.name, name) for name in spec.class_names}
    class_records: dict[str, Mapping[str, object]] = {}
    for raw_class in middle:
        if not isinstance(raw_class, Mapping):
            raise AuditError("raw class record is not an object")
        class_path = raw_class.get("classPath")
        if not isinstance(class_path, str) or class_path not in expected_paths:
            raise AuditError(f"{spec.name} selected class identity drift")
        if class_path in class_records:
            raise AuditError(f"{spec.name} contains a duplicate selected class")
        class_records[class_path] = raw_class
    if set(class_records) != expected_paths:
        raise AuditError(f"{spec.name} selected class set drift")
    validated = {
        path: _validate_class_record(class_records[path], path)
        for path in sorted(class_records)
    }

    _expect_keys(summary, SUMMARY_KEYS, "raw summary")
    for field, expected in (
        ("record", "summary"),
        ("schema", RAW_SCHEMA),
        ("mode", "class-artifacts"),
        ("classesEmitted", spec.expected_count),
        ("classLinksVerifiedDirectly", True),
        ("wireStaticReferencesDerived", False),
        ("reportAuthorizesRuntime", False),
    ):
        _expect(summary, field, expected, "raw summary")
    return validated


def _network_members(class_record: Mapping[str, object]) -> tuple[Mapping[str, object], ...]:
    members = class_record.get("members")
    if not isinstance(members, tuple):
        raise AuditError("validated class has no member tuple")
    # UE3 ClassNetCache gives a new handle to a newly declared net field.
    # A network UFunction with Super overrides an inherited field and reuses
    # that inherited identity, so counting it again shifts every later handle.
    result = tuple(
        member
        for member in members
        if isinstance(member, Mapping)
        and member.get("specialDeclaration") is False
        and not (
            member.get("kind") == "function"
            and member.get("functionSuperPresent") is True
        )
    )
    excluded = sum(
        1
        for member in members
        if isinstance(member, Mapping)
        and (
            member.get("specialDeclaration") is True
            or (
                member.get("kind") == "function"
                and member.get("functionSuperPresent") is True
            )
        )
    )
    if len(result) + excluded != len(members):
        raise AuditError("validated member special marker drift")
    return result


def _overridden_network_functions(
    class_record: Mapping[str, object],
) -> tuple[Mapping[str, object], ...]:
    members = class_record.get("members")
    if not isinstance(members, tuple):
        raise AuditError("validated class has no member tuple")
    return tuple(
        member
        for member in members
        if isinstance(member, Mapping)
        and member.get("kind") == "function"
        and member.get("functionSuperPresent") is True
    )


def _build_target(
    target: str,
    chain: Sequence[str],
    counts: Sequence[int],
    expected_max_handle: int,
    terminal_path: str,
    expected_uclass_net: int,
    expected_cdo_net: int,
    classes: Mapping[str, Mapping[str, object]],
) -> dict[str, object]:
    if len(chain) != len(counts) or chain[-1] != terminal_path:
        raise AuditError("internal target chain contract is invalid")
    handles: list[dict[str, object]] = []
    class_ranges: list[dict[str, object]] = []
    next_handle = 0
    for class_path, expected_count in zip(chain, counts, strict=True):
        record = classes.get(class_path)
        if record is None or record.get("superClassPath") != SUPER_CLASS_PATHS[class_path]:
            raise AuditError(f"{target} class chain drift")
        members = _network_members(record)
        overridden_functions = _overridden_network_functions(record)
        if len(members) != expected_count:
            raise AuditError(f"{target} direct network member count drift")
        for overridden in overridden_functions:
            inherited_matches = [
                handle
                for handle in handles
                if handle.get("kind") == "function"
                and handle.get("name") == overridden.get("name")
            ]
            if len(inherited_matches) != 1:
                raise AuditError(
                    f"{target} overridden network function has no unique inherited handle"
                )
        first_handle = next_handle if members else None
        for member in members:
            handles.append(
                {
                    "handle": next_handle,
                    "declaringClassPath": class_path,
                    "name": member["name"],
                    "kind": member["kind"],
                    "uobjectNetIndex": member["uobjectNetIndex"],
                    "propertyFlags": member["propertyFlags"],
                    "functionFlags": member["functionFlags"],
                    "arrayDim": member["arrayDim"],
                    "functionSuperPresent": member["functionSuperPresent"],
                }
            )
            next_handle += 1
        class_ranges.append(
            {
                "classPath": class_path,
                "directNetworkMemberCount": len(members),
                "overriddenNetworkFunctions": [
                    {
                        "name": member["name"],
                        "uobjectNetIndex": member["uobjectNetIndex"],
                    }
                    for member in overridden_functions
                ],
                "firstHandle": first_handle,
                "lastHandle": next_handle - 1 if members else None,
            }
        )
    if next_handle != expected_max_handle:
        raise AuditError(f"{target} maximum replication handle drift")
    if [row["handle"] for row in handles] != list(range(expected_max_handle)):
        raise AuditError(f"{target} handle table is not contiguous")

    spot_rows: list[dict[str, object]] = []
    for handle, expected in SPOT_HANDLES.items():
        if handle >= expected_max_handle:
            continue
        row = handles[handle]
        actual = (
            row["declaringClassPath"],
            row["name"],
            row["uobjectNetIndex"],
        )
        if actual != expected:
            raise AuditError(f"{target} spot handle h{handle} drift")
        spot_rows.append(
            {
                "handle": handle,
                "declaringClassPath": expected[0],
                "name": expected[1],
                "uobjectNetIndex": expected[2],
            }
        )

    terminal = classes[terminal_path]
    uclass = terminal.get("UClass")
    cdo = terminal.get("CDO")
    if not isinstance(uclass, Mapping) or not isinstance(cdo, Mapping):
        raise AuditError(f"{target} terminal identities are missing")
    if (
        uclass.get("uobjectNetIndex") != expected_uclass_net
        or cdo.get("uobjectNetIndex") != expected_cdo_net
    ):
        raise AuditError(f"{target} terminal UClass/CDO identity drift")

    return {
        "record": "target",
        "schema": SCHEMA,
        "target": target,
        "terminalClassPath": terminal_path,
        "terminalUClassNetIndex": expected_uclass_net,
        "terminalCDONetIndex": expected_cdo_net,
        "handleIndexBase": 0,
        "classChain": class_ranges,
        "handles": handles,
        "maxHandle": expected_max_handle,
        "maxHandleExclusive": expected_max_handle,
        "spotHandles": spot_rows,
        **_runtime_denials(),
    }


def _build_default_proof(
    classes: Mapping[str, Mapping[str, object]]
) -> dict[str, object]:
    actor = classes.get("Class'Engine.Actor'")
    if actor is None:
        raise AuditError("Actor class evidence is missing")
    members = actor.get("members")
    if not isinstance(members, tuple):
        raise AuditError("Actor member evidence is missing")
    declarations = [
        member
        for member in members
        if isinstance(member, Mapping)
        and member.get("name") == "bNetInitialRotation"
        and member.get("specialDeclaration") is True
    ]
    if len(declarations) != 1:
        raise AuditError("Actor.bNetInitialRotation declaration count drift")
    declaration = declarations[0]
    checked_classes = sorted(set(M60_CHAIN) | set(M61_CHAIN))
    if set(checked_classes) != set(SUPER_CLASS_PATHS):
        raise AuditError("bNetInitialRotation proof does not cover every selected class")
    return {
        "record": "default-proof",
        "schema": SCHEMA,
        "propertyPath": "Class'Engine.Actor'.bNetInitialRotation",
        "declarationKind": declaration["kind"],
        "uelibType": declaration["uelibType"],
        "uobjectNetIndex": declaration["uobjectNetIndex"],
        "propertyFlags": declaration["propertyFlags"],
        "arrayDim": declaration["arrayDim"],
        "cpfNet": False,
        "specialDeclaration": True,
        "ue3ImplicitZeroDefaultRuleApplied": True,
        "classesCheckedForDirectOverride": checked_classes,
        "directSerializedOverrideCount": 0,
        "effectiveValue": False,
        "effectiveFalseProven": True,
        **_runtime_denials(),
    }


def _package_identity_record(spec: PackageSpec) -> dict[str, object]:
    return {
        "artifact": spec.filename,
        "artifactBytes": spec.artifact_bytes,
        "artifactSha256": spec.artifact_sha256,
        "packageGuid": spec.package_guid,
        "rawGuid": spec.raw_guid,
        "packageFlags": spec.package_flags,
        "packageVersion": 765,
        "licenseeVersion": 771,
        "engineVersion": 7258,
        "generations": [
            {
                "ordinal": ordinal,
                "exports": row[0],
                "names": row[1],
                "netObjects": row[2],
            }
            for ordinal, row in enumerate(spec.generations)
        ],
    }


def build_report(
    raw_packages: Mapping[str, Sequence[Mapping[str, object]]]
) -> tuple[dict[str, object], ...]:
    if set(raw_packages) != {spec.name for spec in PACKAGE_SPECS}:
        raise AuditError("raw package input set drift")
    classes: dict[str, dict[str, object]] = {}
    for spec in PACKAGE_SPECS:
        validated = validate_raw_package(spec, raw_packages[spec.name])
        overlap = set(classes) & set(validated)
        if overlap:
            raise AuditError("selected class path appears in more than one package")
        classes.update(validated)
    if set(classes) != set(SUPER_CLASS_PATHS):
        raise AuditError("selected class closure drift")
    if any("ROWeap_M61_GrenadeSingle" in path for path in classes):
        raise AuditError("forbidden non-content M61 Single class entered the chain")

    header = {
        "record": "header",
        "schema": SCHEMA,
        "schemaVersion": SCHEMA_VERSION,
        "artifacts": [_package_identity_record(spec) for spec in PACKAGE_SPECS],
        "extractorWrapperBytes": EXPECTED_WRAPPER_BYTES,
        "extractorWrapperSha256": EXPECTED_WRAPPER_SHA256,
        "extractorSourceBytes": EXPECTED_EXTRACTOR_SOURCE_BYTES,
        "extractorSourceSha256": EXPECTED_EXTRACTOR_SOURCE_SHA256,
        "extractorExecutableBytes": EXPECTED_EXTRACTOR_EXECUTABLE_BYTES,
        "extractorExecutableSha256": EXPECTED_EXTRACTOR_EXECUTABLE_SHA256,
        "uelibBytes": EXPECTED_UELIB_BYTES,
        "uelibSha256": EXPECTED_UELIB_SHA256,
        "uelibAssemblyVersion": EXPECTED_UELIB_ASSEMBLY_VERSION,
        "uelibProductVersion": EXPECTED_UELIB_PRODUCT_VERSION,
        "uelibDependencies": [
            {
                "name": EXPECTED_UNSAFE_NAME,
                "bytes": EXPECTED_UNSAFE_BYTES,
                "sha256": EXPECTED_UNSAFE_SHA256,
            }
        ],
        **_runtime_denials(),
    }
    targets = tuple(
        _build_target(*target_spec, classes) for target_spec in TARGETS
    )
    default_proof = _build_default_proof(classes)
    summary = {
        "record": "summary",
        "schema": SCHEMA,
        "schemaVersion": SCHEMA_VERSION,
        "artifacts": len(PACKAGE_SPECS),
        "uniqueClasses": len(classes),
        "targets": len(targets),
        "handleRows": sum(len(target["handles"]) for target in targets),
        "m60MaxHandle": 112,
        "m61ContentSingleMaxHandle": 101,
        "bNetInitialRotationEffective": False,
        "reportContainsLocalPaths": False,
        **_runtime_denials(),
    }
    report = (header, *targets, default_proof, summary)
    validate_final_report(report)
    return report


def _walk(value: object) -> Iterable[object]:
    yield value
    if isinstance(value, Mapping):
        for child in value.values():
            yield from _walk(child)
    elif isinstance(value, (list, tuple)):
        for child in value:
            yield from _walk(child)


def validate_final_report(records: Sequence[Mapping[str, object]]) -> None:
    if len(records) != 5:
        raise AuditError("sanitized report record count drift")
    if [record.get("record") for record in records] != [
        "header",
        "target",
        "target",
        "default-proof",
        "summary",
    ]:
        raise AuditError("sanitized report record order drift")
    for record in records:
        if record.get("schema") != SCHEMA:
            raise AuditError("sanitized report schema drift")
        _reject_forbidden_keys(record)
        for field, expected in _runtime_denials().items():
            if record.get(field) is not expected:
                raise AuditError(f"sanitized report {field} must remain false")
    header, first_target, second_target, default_proof, summary = records
    _expect_keys(header, FINAL_HEADER_KEYS, "sanitized header")
    _expect_keys(first_target, FINAL_TARGET_KEYS, "sanitized target")
    _expect_keys(second_target, FINAL_TARGET_KEYS, "sanitized target")
    _expect_keys(default_proof, FINAL_DEFAULT_KEYS, "sanitized default proof")
    _expect_keys(summary, FINAL_SUMMARY_KEYS, "sanitized summary")

    artifacts = header.get("artifacts")
    if not isinstance(artifacts, list) or len(artifacts) != len(PACKAGE_SPECS):
        raise AuditError("sanitized artifact identity list drift")
    for artifact in artifacts:
        if not isinstance(artifact, Mapping):
            raise AuditError("sanitized artifact identity is not an object")
        _expect_keys(artifact, FINAL_ARTIFACT_KEYS, "sanitized artifact identity")
        generations = artifact.get("generations")
        if not isinstance(generations, list):
            raise AuditError("sanitized artifact generation list is missing")
        for generation in generations:
            if not isinstance(generation, Mapping):
                raise AuditError("sanitized artifact generation is not an object")
            _expect_keys(generation, GENERATION_KEYS, "sanitized generation")
    dependencies = header.get("uelibDependencies")
    if (
        not isinstance(dependencies, list)
        or len(dependencies) != 1
        or not isinstance(dependencies[0], Mapping)
        or set(dependencies[0]) != {"name", "bytes", "sha256"}
    ):
        raise AuditError("sanitized dependency identity drift")
    expected_header_scalars = {
        "record": "header",
        "schema": SCHEMA,
        "schemaVersion": SCHEMA_VERSION,
        "extractorWrapperBytes": EXPECTED_WRAPPER_BYTES,
        "extractorWrapperSha256": EXPECTED_WRAPPER_SHA256,
        "extractorSourceBytes": EXPECTED_EXTRACTOR_SOURCE_BYTES,
        "extractorSourceSha256": EXPECTED_EXTRACTOR_SOURCE_SHA256,
        "extractorExecutableBytes": EXPECTED_EXTRACTOR_EXECUTABLE_BYTES,
        "extractorExecutableSha256": EXPECTED_EXTRACTOR_EXECUTABLE_SHA256,
        "uelibBytes": EXPECTED_UELIB_BYTES,
        "uelibSha256": EXPECTED_UELIB_SHA256,
        "uelibAssemblyVersion": EXPECTED_UELIB_ASSEMBLY_VERSION,
        "uelibProductVersion": EXPECTED_UELIB_PRODUCT_VERSION,
    }
    for field, expected in expected_header_scalars.items():
        _expect(header, field, expected, "sanitized header")
    if artifacts != [_package_identity_record(spec) for spec in PACKAGE_SPECS]:
        raise AuditError("sanitized package identity facts drift")
    expected_dependency = {
        "name": EXPECTED_UNSAFE_NAME,
        "bytes": EXPECTED_UNSAFE_BYTES,
        "sha256": EXPECTED_UNSAFE_SHA256,
    }
    if dependencies != [expected_dependency]:
        raise AuditError("sanitized dependency facts drift")

    for value in _walk(records):
        if isinstance(value, str) and ABSOLUTE_PATH_RE.search(value):
            raise AuditError("sanitized report contains a local path")
    target_records = records[1:3]
    for target, target_spec in zip(target_records, TARGETS, strict=True):
        (
            expected_target,
            expected_chain,
            expected_counts,
            expected_max_handle,
            expected_terminal,
            expected_uclass_net,
            expected_cdo_net,
        ) = target_spec
        for field, expected in (
            ("record", "target"),
            ("schema", SCHEMA),
            ("target", expected_target),
            ("terminalClassPath", expected_terminal),
            ("terminalUClassNetIndex", expected_uclass_net),
            ("terminalCDONetIndex", expected_cdo_net),
            ("handleIndexBase", 0),
            ("maxHandle", expected_max_handle),
            ("maxHandleExclusive", expected_max_handle),
        ):
            _expect(target, field, expected, "sanitized target")
        handles = target.get("handles")
        max_handle = target.get("maxHandle")
        if not isinstance(handles, list) or type(max_handle) is not int:
            raise AuditError("sanitized target handle table is missing")
        class_chain = target.get("classChain")
        spots = target.get("spotHandles")
        if not isinstance(class_chain, list) or not isinstance(spots, list):
            raise AuditError("sanitized target chain or spots are missing")
        if len(class_chain) != len(expected_chain):
            raise AuditError("sanitized target chain length drift")
        for class_range in class_chain:
            if not isinstance(class_range, Mapping):
                raise AuditError("sanitized class range is not an object")
            _expect_keys(class_range, FINAL_CLASS_RANGE_KEYS, "sanitized class range")
            overrides = class_range.get("overriddenNetworkFunctions")
            if not isinstance(overrides, list):
                raise AuditError("sanitized overridden-function list is missing")
            for override in overrides:
                if not isinstance(override, Mapping):
                    raise AuditError("sanitized overridden function is not an object")
                _expect_keys(
                    override, FINAL_OVERRIDE_KEYS, "sanitized overridden function"
                )
        for row in handles:
            if not isinstance(row, Mapping):
                raise AuditError("sanitized handle is not an object")
            _expect_keys(row, FINAL_HANDLE_KEYS, "sanitized handle")
        for spot in spots:
            if not isinstance(spot, Mapping):
                raise AuditError("sanitized spot handle is not an object")
            _expect_keys(spot, FINAL_SPOT_KEYS, "sanitized spot handle")
        if [row.get("handle") for row in handles if isinstance(row, Mapping)] != list(
            range(max_handle)
        ):
            raise AuditError("sanitized target handle table is not contiguous")
        cursor = 0
        for class_range, expected_path, expected_count in zip(
            class_chain, expected_chain, expected_counts, strict=True
        ):
            first = cursor if expected_count else None
            last = cursor + expected_count - 1 if expected_count else None
            expected_range = {
                "classPath": expected_path,
                "directNetworkMemberCount": expected_count,
                "overriddenNetworkFunctions": [
                    {"name": name, "uobjectNetIndex": net_index}
                    for name, net_index in EXPECTED_OVERRIDDEN_NETWORK_FUNCTIONS.get(
                        expected_path, ()
                    )
                ],
                "firstHandle": first,
                "lastHandle": last,
            }
            if class_range != expected_range:
                raise AuditError("sanitized class handle range drift")
            for override in expected_range["overriddenNetworkFunctions"]:
                inherited_matches = [
                    row
                    for row in handles[:cursor]
                    if row.get("kind") == "function"
                    and row.get("name") == override["name"]
                ]
                if len(inherited_matches) != 1:
                    raise AuditError(
                        "sanitized overridden function has no unique inherited handle"
                    )
            if expected_count:
                for row in handles[cursor : cursor + expected_count]:
                    if row.get("declaringClassPath") != expected_path:
                        raise AuditError("sanitized handle declaring class drift")
                cursor += expected_count
        if cursor != expected_max_handle:
            raise AuditError("sanitized class ranges do not cover the handle table")
        for row in handles:
            name = row.get("name")
            kind = row.get("kind")
            if not isinstance(name, str) or MEMBER_NAME_RE.fullmatch(name) is None:
                raise AuditError("sanitized handle member name drift")
            _exact_int(row.get("handle"), "sanitized handle")
            _exact_int(row.get("uobjectNetIndex"), "sanitized UObject.NetIndex")
            property_flags = _exact_optional_string(
                row.get("propertyFlags"), "sanitized property flags"
            )
            function_flags = _exact_optional_string(
                row.get("functionFlags"), "sanitized function flags"
            )
            array_dim = _exact_optional_int(
                row.get("arrayDim"), "sanitized array dimension"
            )
            function_super = _exact_optional_bool(
                row.get("functionSuperPresent"), "sanitized function super marker"
            )
            if kind == "property":
                if (
                    property_flags is None
                    or FLAG16_RE.fullmatch(property_flags) is None
                    or int(property_flags[2:], 16) & 0x20 == 0
                    or function_flags is not None
                    or array_dim is None
                    or array_dim < 1
                    or function_super is not None
                ):
                    raise AuditError("sanitized network property drift")
            elif kind == "function":
                if (
                    function_flags is None
                    or FLAG16_RE.fullmatch(function_flags) is None
                    or int(function_flags[2:], 16) & 0x40 == 0
                    or property_flags is not None
                    or array_dim is not None
                    or function_super is not False
                ):
                    raise AuditError("sanitized network function drift")
            else:
                raise AuditError("sanitized handle member kind drift")
        expected_spots = [
            {
                "handle": handle,
                "declaringClassPath": identity[0],
                "name": identity[1],
                "uobjectNetIndex": identity[2],
            }
            for handle, identity in SPOT_HANDLES.items()
            if handle < expected_max_handle
        ]
        if spots != expected_spots:
            raise AuditError("sanitized spot handle facts drift")
        for spot in expected_spots:
            row = handles[spot["handle"]]
            actual = {
                "handle": row.get("handle"),
                "declaringClassPath": row.get("declaringClassPath"),
                "name": row.get("name"),
                "uobjectNetIndex": row.get("uobjectNetIndex"),
            }
            if actual != spot:
                raise AuditError("sanitized spot handle does not match handle table")

    expected_checked_classes = sorted(set(M60_CHAIN) | set(M61_CHAIN))
    for field, expected in (
        ("record", "default-proof"),
        ("schema", SCHEMA),
        ("propertyPath", "Class'Engine.Actor'.bNetInitialRotation"),
        ("declarationKind", "property"),
        ("uelibType", "UELib.Core.UBoolProperty"),
        ("propertyFlags", "0x0000000000000002"),
        ("arrayDim", 1),
        ("cpfNet", False),
        ("specialDeclaration", True),
        ("ue3ImplicitZeroDefaultRuleApplied", True),
        ("classesCheckedForDirectOverride", expected_checked_classes),
        ("directSerializedOverrideCount", 0),
        ("effectiveValue", False),
        ("effectiveFalseProven", True),
    ):
        _expect(default_proof, field, expected, "sanitized default proof")
    _exact_int(
        default_proof.get("uobjectNetIndex"),
        "bNetInitialRotation UObject.NetIndex",
    )
    for field, expected in (
        ("record", "summary"),
        ("schema", SCHEMA),
        ("schemaVersion", SCHEMA_VERSION),
        ("artifacts", 4),
        ("uniqueClasses", 16),
        ("targets", 2),
        ("handleRows", 213),
        ("m60MaxHandle", 112),
        ("m61ContentSingleMaxHandle", 101),
        ("bNetInitialRotationEffective", False),
        ("reportContainsLocalPaths", False),
    ):
        _expect(summary, field, expected, "sanitized summary")


def _run_command(
    command: Sequence[str], label: str, capture_stdout: bool = False
) -> str:
    try:
        completed = subprocess.run(
            command,
            check=False,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE if capture_stdout else subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            timeout=EXTRACT_TIMEOUT_SECONDS,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise AuditError(f"{label} command failed safely") from exc
    if completed.returncode != 0:
        raise AuditError(f"{label} command returned a nonzero status")
    if not capture_stdout:
        return ""
    stdout = completed.stdout or b""
    if len(stdout) > BUILD_OUTPUT_BYTES:
        raise AuditError("extractor build output exceeded its byte bound")
    try:
        return stdout.decode("utf-8").strip()
    except UnicodeDecodeError as exc:
        raise AuditError("extractor build output was not UTF-8") from exc


def build_pinned_extractor(wrapper: Path, uelib: Path) -> Path:
    output = _run_command(
        (
            "powershell",
            "-NoProfile",
            "-File",
            str(wrapper),
            "-UELibPath",
            str(uelib),
            "-BuildOnly",
            "-ForceRebuild",
            "-ExpectedExecutableSha256",
            EXPECTED_EXTRACTOR_EXECUTABLE_SHA256,
        ),
        "extractor build",
        capture_stdout=True,
    )
    lines = [line.strip() for line in output.splitlines() if line.strip()]
    if len(lines) != 1:
        raise AuditError("extractor build did not return one executable identity")
    executable = Path(lines[0])
    if not executable.is_file():
        raise AuditError("extractor build did not publish its executable")
    validate_file_identity(
        executable,
        EXPECTED_EXTRACTOR_EXECUTABLE_BYTES,
        EXPECTED_EXTRACTOR_EXECUTABLE_SHA256,
        "extractor executable",
    )
    return executable


def run_class_extractor(
    spec: PackageSpec,
    package: Path,
    wrapper: Path,
    uelib: Path,
    destination: Path,
) -> None:
    max_input_mib = max(1, (spec.artifact_bytes + 1_048_575) // 1_048_576)
    _run_command(
        (
            "powershell",
            "-NoProfile",
            "-File",
            str(wrapper),
            str(package),
            "-UELibPath",
            str(uelib),
            "-ClassArtifacts",
            "-ClassPattern",
            spec.pattern,
            "-ExpectedClassCount",
            str(spec.expected_count),
            "-ExpectedPackageGuid",
            spec.package_guid,
            "-ExpectedSha256",
            spec.artifact_sha256,
            "-ExpectedExecutableSha256",
            EXPECTED_EXTRACTOR_EXECUTABLE_SHA256,
            "-MaxInputMiB",
            str(max_input_mib),
            "-MaxExports",
            "100000",
            "-MaxClasses",
            "32",
            "-OutputPath",
            str(destination),
        ),
        spec.name + " class-artifact extraction",
    )
    if not destination.is_file() or destination.stat().st_size > MAX_RAW_BYTES:
        raise AuditError(f"{spec.name} extractor output is missing or oversized")


def _paths_alias(left: Path, right: Path) -> bool:
    try:
        left_resolved = left.resolve()
        right_resolved = right.resolve()
    except (OSError, RuntimeError) as exc:
        raise AuditError("could not validate an output alias safely") from exc
    if left_resolved == right_resolved:
        return True
    try:
        return left_resolved.exists() and right_resolved.exists() and os.path.samefile(
            left_resolved, right_resolved
        )
    except OSError:
        return False


def _inside(path: Path, root: Path) -> bool:
    try:
        path.resolve().relative_to(root.resolve())
        return True
    except ValueError:
        return False
    except (OSError, RuntimeError) as exc:
        raise AuditError("could not validate an output root safely") from exc


def validate_output_destination(
    destination: Path | None,
    protected_inputs: Sequence[Path],
    forbidden_roots: Sequence[Path],
) -> None:
    if destination is None:
        return
    if any(_paths_alias(source, destination) for source in protected_inputs):
        raise AuditError("output must not overwrite or alias a pinned input")
    if any(_inside(destination, root) for root in forbidden_roots):
        raise AuditError("output must not be written inside an installed input tree")


def _write_bounded_jsonl(
    records: Sequence[Mapping[str, object]], output: object
) -> None:
    total_bytes = 0
    for record in records:
        line = (_json(record) + "\n").encode("utf-8")
        total_bytes += len(line)
        if total_bytes > MAX_REPORT_BYTES:
            raise AuditError("sanitized report exceeds the output byte bound")
        output.write(line)  # type: ignore[attr-defined]


def _atomic_write_jsonl(
    records: Sequence[Mapping[str, object]],
    destination: Path | None,
    overwrite: bool,
) -> None:
    if destination is None:
        with tempfile.SpooledTemporaryFile(
            max_size=REPORT_SPOOL_BYTES, mode="w+b"
        ) as rendered:
            _write_bounded_jsonl(records, rendered)
            rendered.seek(0)
            while chunk := rendered.read(1024 * 1024):
                sys.stdout.write(chunk.decode("ascii"))
        return

    absolute_destination = destination.absolute()
    destination_name = absolute_destination.name
    if not destination_name or destination_name in (".", ".."):
        raise AuditError("report destination has no safe file name")
    parent = absolute_destination.parent.resolve()
    parent.mkdir(parents=True, exist_ok=True)
    anchored_destination = parent / destination_name
    if os.path.lexists(anchored_destination) and not overwrite:
        raise AuditError("report output exists; pass --overwrite")
    temporary_path: Path | None = None
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
            os.rename(temporary_path, anchored_destination)
        else:
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
    records: Sequence[Mapping[str, object]],
    destination: Path | None,
    overwrite: bool,
    protected_inputs: Sequence[Path] = (),
    forbidden_roots: Sequence[Path] = (),
) -> None:
    validate_output_destination(destination, protected_inputs, forbidden_roots)
    try:
        _atomic_write_jsonl(records, destination, overwrite)
    except AuditError:
        raise
    except (OSError, TypeError, ValueError, UnicodeError, RecursionError) as exc:
        raise AuditError("could not atomically write the sanitized report") from exc


def validate_installed_inputs(
    package_root: Path, wrapper: Path, uelib: Path
) -> tuple[Path, Path, dict[str, Path]]:
    source = wrapper.with_name(EXPECTED_EXTRACTOR_SOURCE_NAME)
    unsafe = uelib.with_name(EXPECTED_UNSAFE_NAME)
    for path, expected_bytes, expected_sha256, label in (
        (wrapper, EXPECTED_WRAPPER_BYTES, EXPECTED_WRAPPER_SHA256, "extractor wrapper"),
        (
            source,
            EXPECTED_EXTRACTOR_SOURCE_BYTES,
            EXPECTED_EXTRACTOR_SOURCE_SHA256,
            "extractor source",
        ),
        (uelib, EXPECTED_UELIB_BYTES, EXPECTED_UELIB_SHA256, "Eliot.UELib.dll"),
        (unsafe, EXPECTED_UNSAFE_BYTES, EXPECTED_UNSAFE_SHA256, EXPECTED_UNSAFE_NAME),
    ):
        if not path.is_file():
            raise AuditError(f"{label} is missing")
        validate_file_identity(path, expected_bytes, expected_sha256, label)
    packages: dict[str, Path] = {}
    for spec in PACKAGE_SPECS:
        package = package_root / spec.filename
        if not package.is_file():
            raise AuditError(f"{spec.filename} is missing")
        validate_file_identity(
            package, spec.artifact_bytes, spec.artifact_sha256, spec.filename
        )
        packages[spec.name] = package
    return source, unsafe, packages


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package-root", type=Path, default=DEFAULT_PACKAGE_ROOT)
    parser.add_argument("--wrapper", type=Path, default=DEFAULT_WRAPPER)
    parser.add_argument("--uelib", type=Path, default=DEFAULT_UELIB)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--overwrite", action="store_true")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        package_root = args.package_root.resolve()
        wrapper = args.wrapper.resolve()
        uelib = args.uelib.resolve()
        source, unsafe, packages = validate_installed_inputs(
            package_root, wrapper, uelib
        )
        protected: list[Path] = [wrapper, source, uelib, unsafe, *packages.values()]
        forbidden_roots = [package_root, uelib.parent]
        validate_output_destination(args.output, protected, forbidden_roots)

        executable = build_pinned_extractor(wrapper, uelib).resolve()
        protected.append(executable)
        forbidden_roots.append(executable.parent)
        validate_output_destination(args.output, protected, forbidden_roots)

        with tempfile.TemporaryDirectory(prefix="rsv2-mg-class-audit-") as temporary:
            temporary_root = Path(temporary)
            raw_packages: dict[str, tuple[dict[str, object], ...]] = {}
            for spec in PACKAGE_SPECS:
                destination = temporary_root / (spec.name + ".jsonl")
                run_class_extractor(
                    spec, packages[spec.name], wrapper, uelib, destination
                )
                raw_packages[spec.name] = load_jsonl(destination)

            source_after, unsafe_after, packages_after = validate_installed_inputs(
                package_root, wrapper, uelib
            )
            if source_after != source or unsafe_after != unsafe or packages_after != packages:
                raise AuditError("pinned input paths changed during the audit")
            validate_file_identity(
                executable,
                EXPECTED_EXTRACTOR_EXECUTABLE_BYTES,
                EXPECTED_EXTRACTOR_EXECUTABLE_SHA256,
                "extractor executable after audit",
            )
            report = build_report(raw_packages)

        write_report(
            report,
            args.output,
            args.overwrite,
            protected_inputs=protected,
            forbidden_roots=forbidden_roots,
        )
        return 0
    except AuditError as exc:
        error = str(exc)
    except (OSError, RuntimeError, TypeError, UnicodeError, RecursionError):
        error = "class-artifact audit failed safely"
    failure = {
        "record": "summary",
        "schema": SCHEMA,
        "fatal": True,
        "error": error,
        **_runtime_denials(),
    }
    print(_json(failure), file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
