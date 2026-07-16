#!/usr/bin/env python3
"""Build an exact, fail-closed installed Cu Chi role-reference report.

The script invokes the table-only ``role-exports`` mode of the existing cooked
package extractor, pins the current retail ROGame.u identity, joins those exact
UClass exports to Cu Chi's source/cooked role table, and emits JSONL.  It never
enables a gameplay role and never writes to the game install.
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


EXPECTED_BYTES = 40_989_134
EXPECTED_SHA256 = "AED4E60D406880D048EB579A082F4A44BE3D0B39CFEC47F9FCEF828A40C44961"
EXPECTED_GUID = "16A6CC8D446C4A9FD5B688B3210DCC82"
EXPECTED_PACKAGE_FLAGS = "0x20204001"
EXPECTED_GENERATIONS = 2
EXPECTED_EXPORTS = 64_476
EXPECTED_NAMES = 48_359
EXPECTED_NET_OBJECTS = 64_476
EXPECTED_ROLE_PAIRS = 94
EXPECTED_UELIB_BYTES = 366_592
EXPECTED_UELIB_SHA256 = "56823D074102BA959784B44DBE961B5DBE825EE2CF1C5D02BC53F5B0EB91BCAE"
EXPECTED_UELIB_ASSEMBLY_VERSION = "1.12.1.0"
EXPECTED_UELIB_PRODUCT_VERSION = "1.12.1+1af731ccb2ccfa61927fff5ffa9fdda64731ca0f"
EXPECTED_UNSAFE_NAME = "System.Runtime.CompilerServices.Unsafe.dll"
EXPECTED_UNSAFE_BYTES = 19_256
EXPECTED_UNSAFE_SHA256 = "08CBD7278B66F1E68425A82D4B97181A4130D93E3DD91831407ABA7212CCDACF"
EXPECTED_UNSAFE_ASSEMBLY_VERSION = "6.0.3.0"
EXPECTED_WRAPPER_BYTES = 14_303
EXPECTED_WRAPPER_SHA256 = "C18706D1B2E3E0A4462E5416623DC57E6764341E264206F5490D6C198EBA4B26"
EXPECTED_EXTRACTOR_SOURCE_NAME = "CookedMapMetadataExtractor.cs"
EXPECTED_EXTRACTOR_SOURCE_BYTES = 158_772
EXPECTED_EXTRACTOR_SOURCE_SHA256 = "56F59A70ED8FD61416E57CAD3122E01F4796AA6AC4A04EECCE3776C5AEEEB26D"
EXPECTED_EXTRACTOR_EXECUTABLE_BYTES = 125_952
EXPECTED_EXTRACTOR_EXECUTABLE_SHA256 = "087D939AAD765B3F73A32ADCD738BFCD757FF3D2371AC598BF71FE5230C988E2"
EXPECTED_MAP_BYTES = 283_465_712
EXPECTED_MAP_SHA256 = "F5D5E9DB687DC45889CC8C2C795904C467FF81285B06338E4F176A09C011687E"
EXPECTED_MAP_GUID = "1BE145E5457B54A941963282D62E012B"
EXPECTED_MAP_INFO_EXPORT = 10_022
EXPECTED_MAP_INFO_OFFSET = 36_844_352
EXPECTED_MAP_INFO_SIZE = 3_002
EXPECTED_SOURCE_FILES = {
    "ROMapInfo.uc": (65_594, "BAAA28A00163A6A2D18B37C5CD89A77109C8A0C83362E18676F81CC333B20481"),
    "RORoleInfo.uc": (27_300, "F96518DBE973B3737FE385BB96BC04D59F29BC8A8C4B709B012970DE7CDDB72F"),
    "RORoleInfoNorthernRifleman.uc": (1_486, "F7D91D87CA0C5A2B337501B5C53D70288946D5B8EA354342B2BD3C27A4508135"),
    "RORoleInfoNorthernScout.uc": (1_331, "01AD2C5CDA1FD92F93EE4D83227345CB8F7DD1A3DCF283623202ECFCB468E172"),
    "RORoleInfoNorthernMachineGunner.uc": (1_275, "950BBDED393AACA8354D09E2B1C59941B672D6ACA8D334064B7A57C2E97EA145"),
    "RORoleInfoNorthernSniper.uc": (1_183, "3FB4E2F28F5135DD2F005979B2F46E84455651C670C93D409C12C7AF95811B6A"),
    "RORoleInfoNorthernRPG.uc": (1_239, "4BA5BB8914C1BC385568C0F220E39C06A020DD79ED30E46FADE96C7FA2A5C975"),
    "RORoleInfoNorthernSapper.uc": (1_267, "229BB533AAADE20D2797D46F641CFFD2BA17BFA7FCF35B78C1B9B6E348F9CB83"),
    "RORoleInfoNorthernRadioman.uc": (1_459, "6EFDB8B792B8C1AC8F48D063161061EC7F3D04EB8E01883EB2B4E78315D6F27B"),
    "RORoleInfoSouthernRifleman.uc": (1_248, "8986B6780CB16E4C0A70E6B572CF9DAF8D6A859DD1D3FFEA6CAA341271935B1E"),
    "RORoleInfoSouthernPointman.uc": (1_522, "BC93946775B4F14928CBF6D3557EEC6B207951784DF44B689C417CEE51C289A1"),
    "RORoleInfoSouthernMachineGunner.uc": (1_116, "B7A6B60702FDE1A77DFE2F322DEC04DE2343975362E28AD5805BCCA31277F88E"),
    "RORoleInfoSouthernMarksman.uc": (1_031, "4AA2BF88FB1DFBBC3847F5611851AEF527587D6BB36F9768FFF7C579ED58FAB5"),
    "RORoleInfoSouthernGrenadier.uc": (1_860, "BA40286A858081EADEE5D6F197444B9D90B0B5CCC957EC511C89C813619DDA39"),
    "RORoleInfoSouthernEngineer.uc": (2_022, "6798FD562B7091E1BD66E20C5C9D630BFA6077DE10D8C103FBCA42B3BA18505E"),
    "RORoleInfoSouthernRadioman.uc": (1_325, "687D19B9A3FB03CC22764B1403377A4EBE3441E598970C2EC8DEB911609D9663"),
}
OBJECT_BASE = 39_478
KNOWN_CLASS_ZERO_REFS = {
    "RORoleInfoNorthernGuerilla": 87_399,
    "RORoleInfoSouthernGrunt": 87_491,
}
EXPECTED_EFFECTIVE_UCLASS = {
    "RORoleInfoNorthernGuerilla": (47_921, 87_399),
    "RORoleInfoNorthernMachineGunnerNLF": (47_929, 87_407),
    "RORoleInfoNorthernRadiomanNLF": (47_935, 87_413),
    "RORoleInfoNorthernRPGNLF": (47_945, 87_423),
    "RORoleInfoNorthernSapperNLF": (47_955, 87_433),
    "RORoleInfoNorthernScoutNLF": (47_963, 87_441),
    "RORoleInfoNorthernSniperNLF": (47_969, 87_447),
    "RORoleInfoSouthernEngineer": (47_985, 87_463),
    "RORoleInfoSouthernGrenadier": (47_999, 87_477),
    "RORoleInfoSouthernGrunt": (48_013, 87_491),
    "RORoleInfoSouthernMachineGunner": (48_019, 87_497),
    "RORoleInfoSouthernMarksman": (48_031, 87_509),
    "RORoleInfoSouthernPointman": (48_053, 87_531),
    "RORoleInfoSouthernRadioman": (48_065, 87_543),
}

REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_WRAPPER = REPO_ROOT / "tools" / "extract_cooked_map_metadata.ps1"
DEFAULT_UELIB = Path(r"D:\RE-Tools\UE-Explorer\Eliot.UELib.dll")
DEFAULT_PACKAGE = Path(
    os.environ.get(
        "RS2_ROGAME_PATH",
        r"D:\SteamLibrary\steamapps\common\Rising Storm 2\ROGame\BrewedPC\ROGame.u",
    )
)
DEFAULT_MAP = Path(
    os.environ.get(
        "RS2_CUCHI_MAP_PATH",
        r"D:\SteamLibrary\steamapps\common\Rising Storm 2\ROGame\BrewedPC\Maps\CuChi\VNTE-CuChi.roe",
    )
)
DEFAULT_SOURCE_ROOT = Path(
    os.environ.get("RS2_SOURCE_ROOT", r"D:\RE-Tools\rs2-source\ROGame")
)


class AuditError(ValueError):
    """Raised when package provenance or role linkage is not exact."""


@dataclass(frozen=True)
class EffectiveRole:
    team: str
    ordinal: int
    base_class: str
    effective_class: str
    class_index: int
    count: int
    reverse_count: int
    map_package_index: int


# Normal, non-reversed first-round Cu Chi. Force substitutions are selected by
# ClassIndex, so the RPG/Sapper and Grenadier/Engineer ordinal swaps are kept
# explicit rather than inferred from array position.
EFFECTIVE_ROLES = (
    EffectiveRole("north", 0, "RORoleInfoNorthernRifleman", "RORoleInfoNorthernGuerilla", 0, 255, 255, -92),
    EffectiveRole("north", 1, "RORoleInfoNorthernScout", "RORoleInfoNorthernScoutNLF", 1, 3, 5, -95),
    EffectiveRole("north", 2, "RORoleInfoNorthernMachineGunner", "RORoleInfoNorthernMachineGunnerNLF", 2, 4, 5, -90),
    EffectiveRole("north", 3, "RORoleInfoNorthernSniper", "RORoleInfoNorthernSniperNLF", 3, 2, 2, -96),
    EffectiveRole("north", 4, "RORoleInfoNorthernRPG", "RORoleInfoNorthernRPGNLF", 6, 2, 3, -93),
    EffectiveRole("north", 5, "RORoleInfoNorthernSapper", "RORoleInfoNorthernSapperNLF", 4, 3, 3, -94),
    EffectiveRole("north", 6, "RORoleInfoNorthernRadioman", "RORoleInfoNorthernRadiomanNLF", 7, 2, 2, -91),
    EffectiveRole("south", 0, "RORoleInfoSouthernRifleman", "RORoleInfoSouthernGrunt", 0, 255, 255, -104),
    EffectiveRole("south", 1, "RORoleInfoSouthernPointman", "RORoleInfoSouthernPointman", 1, 5, 3, -102),
    EffectiveRole("south", 2, "RORoleInfoSouthernMachineGunner", "RORoleInfoSouthernMachineGunner", 2, 5, 4, -100),
    EffectiveRole("south", 3, "RORoleInfoSouthernMarksman", "RORoleInfoSouthernMarksman", 3, 2, 2, -101),
    EffectiveRole("south", 4, "RORoleInfoSouthernGrenadier", "RORoleInfoSouthernGrenadier", 5, 3, 2, -99),
    EffectiveRole("south", 5, "RORoleInfoSouthernEngineer", "RORoleInfoSouthernEngineer", 4, 3, 3, -98),
    EffectiveRole("south", 6, "RORoleInfoSouthernRadioman", "RORoleInfoSouthernRadioman", 7, 2, 2, -103),
)


def _json(value: Mapping[str, object]) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"))


def load_jsonl(path: Path) -> tuple[dict[str, object], ...]:
    records: list[dict[str, object]] = []
    try:
        with path.open("r", encoding="utf-8") as source:
            for line_number, line in enumerate(source, 1):
                if line_number > 1_000:
                    raise AuditError("extractor JSONL exceeds 1000 records")
                if len(line) > 1_048_576:
                    raise AuditError(
                        f"extractor JSONL line {line_number} exceeds 1 MiB"
                    )
                if not line.strip():
                    raise AuditError(f"blank JSONL record at line {line_number}")
                value = json.loads(line)
                if not isinstance(value, dict):
                    raise AuditError(f"non-object JSONL record at line {line_number}")
                records.append(value)
    except (OSError, json.JSONDecodeError) as exc:
        raise AuditError(f"could not read extractor JSONL {path}: {exc}") from exc
    if not records:
        raise AuditError("extractor JSONL is empty")
    return tuple(records)


def _expect(record: Mapping[str, object], field: str, expected: object) -> None:
    actual = record.get(field)
    if actual != expected:
        raise AuditError(f"{field} drift: expected {expected!r}, found {actual!r}")


def file_identity(path: Path) -> tuple[int, str]:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as source:
            while chunk := source.read(1024 * 1024):
                digest.update(chunk)
        return path.stat().st_size, digest.hexdigest().upper()
    except OSError as exc:
        raise AuditError(f"could not hash {path}: {exc}") from exc


def validate_file_identity(
    path: Path, expected_bytes: int, expected_sha256: str, label: str
) -> None:
    actual_bytes, actual_sha256 = file_identity(path)
    if actual_bytes != expected_bytes or actual_sha256 != expected_sha256:
        raise AuditError(
            f"{label} identity drift: expected {expected_bytes} bytes/{expected_sha256}, "
            f"found {actual_bytes} bytes/{actual_sha256}"
        )


def _source_manifest_digest() -> str:
    digest = hashlib.sha256()
    for name, (size, sha256) in sorted(EXPECTED_SOURCE_FILES.items()):
        digest.update(f"{name}\0{size}\0{sha256}\n".encode("ascii"))
    return digest.hexdigest().upper()


def _parse_alt_role_tables(source: str) -> dict[tuple[str, int], tuple[str | None, ...]]:
    tables: dict[tuple[str, int], tuple[str | None, ...]] = {}
    pattern = re.compile(
        r"^\s*(North|South)AltRoleClasses\[(\d+)\]="
        r"\(AltRoleClassByArmy=\((.*?)\)\)\s*$",
        re.MULTILINE,
    )
    for match in pattern.finditer(source):
        side = match.group(1).lower()
        class_index = int(match.group(2))
        values: list[str | None] = []
        for raw_value in match.group(3).split(","):
            value = raw_value.strip()
            if value.casefold() == "none":
                values.append(None)
                continue
            class_match = re.fullmatch(r"Class'ROGame\.([A-Za-z0-9_]+)'", value)
            if class_match is None:
                raise AuditError(
                    f"unrecognized {side} alternate role source token {value!r}"
                )
            values.append(class_match.group(1))
        key = (side, class_index)
        if key in tables:
            raise AuditError(f"duplicate source alternate role table {key}")
        tables[key] = tuple(values)
    return tables


def _validate_root_class_index_contract(source: str) -> None:
    declarations = re.findall(
        r"^\s*var\s+byte\s+ClassIndex\s*;\s*$",
        source,
        re.MULTILINE | re.IGNORECASE,
    )
    if len(declarations) != 1:
        raise AuditError(
            "RORoleInfo ClassIndex declaration drift: expected one byte property"
        )
    assignments = re.findall(
        r"^\s*ClassIndex\s*=\s*[^;\r\n]+;?\s*$",
        source,
        re.MULTILINE | re.IGNORECASE,
    )
    if assignments:
        raise AuditError(
            "RORoleInfo ClassIndex default drift: the inherited byte default must remain zero"
        )


def validate_source_evidence(source_root: Path) -> str:
    source_text: dict[str, str] = {}
    for name, (expected_bytes, expected_sha256) in EXPECTED_SOURCE_FILES.items():
        path = source_root / name
        if not path.is_file():
            raise AuditError(f"pinned source file does not exist: {path}")
        validate_file_identity(path, expected_bytes, expected_sha256, name)
        try:
            source_text[name] = path.read_text(encoding="utf-8-sig")
        except (OSError, UnicodeDecodeError) as exc:
            raise AuditError(f"could not read pinned source {path}: {exc}") from exc

    map_source = source_text["ROMapInfo.uc"]
    required_source_fragments = (
        "SFOR_USArmy,                    // 0",
        "NFOR_NLF,                       // 1",
        "ClassIndex = int(NorthernRoles[I].RoleInfoClass.default.ClassIndex);",
        "NorthAltRoleClasses[ClassIndex].AltRoleClassByArmy[int(NorthernForce)]",
        "ClassIndex = int(SouthernRoles[I].RoleInfoClass.default.ClassIndex);",
        "SouthAltRoleClasses[ClassIndex].AltRoleClassByArmy[int(SouthernForce)]",
    )
    for fragment in required_source_fragments:
        if fragment not in map_source:
            raise AuditError(f"required ROMapInfo source contract is missing: {fragment}")

    _validate_root_class_index_contract(source_text["RORoleInfo.uc"])

    alt_tables = _parse_alt_role_tables(map_source)
    for role in EFFECTIVE_ROLES:
        army_index = 1 if role.team == "north" else 0
        values = alt_tables.get((role.team, role.class_index))
        if values is None or army_index >= len(values):
            raise AuditError(
                f"missing source force substitution for {role.team} class {role.class_index}"
            )
        if values[army_index] != role.effective_class:
            raise AuditError(
                f"source force substitution drift for {role.base_class}: "
                f"expected {role.effective_class}, found {values[army_index]}"
            )

        base_source = source_text[role.base_class + ".uc"]
        if not re.search(
            rf"^class\s+{re.escape(role.base_class)}\s+extends\s+",
            base_source,
            re.MULTILINE | re.IGNORECASE,
        ):
            raise AuditError(f"source class declaration drift for {role.base_class}")
        class_indices = [
            int(value)
            for value in re.findall(
                r"^\s*ClassIndex\s*=\s*(\d+)\s*$", base_source, re.MULTILINE
            )
        ]
        expected_indices = [] if role.class_index == 0 else [role.class_index]
        if class_indices != expected_indices:
            raise AuditError(
                f"source ClassIndex drift for {role.base_class}: "
                f"expected {expected_indices}, found {class_indices}"
            )
    return _source_manifest_digest()


def validate_map_evidence(
    role_records: Sequence[Mapping[str, object]],
    actor_records: Sequence[Mapping[str, object]],
) -> None:
    if len(role_records) != 16:
        raise AuditError(f"Cu Chi role-info record count drift: {len(role_records)}")
    role_header = role_records[0]
    role_summary = role_records[-1]
    _expect(role_header, "record", "header")
    _expect(role_header, "mode", "role-info")
    _expect(role_header, "inputBytes", EXPECTED_MAP_BYTES)
    _expect(role_header, "packageGuid", EXPECTED_MAP_GUID)
    _expect(role_header, "uelibAssemblyVersion", EXPECTED_UELIB_ASSEMBLY_VERSION)
    _expect(role_header, "uelibProductVersion", EXPECTED_UELIB_PRODUCT_VERSION)
    _expect(role_summary, "record", "summary")
    _expect(role_summary, "mode", "role-info")
    _expect(role_summary, "mapInfoExportIndex", EXPECTED_MAP_INFO_EXPORT)
    _expect(role_summary, "mapInfoSerialOffset", EXPECTED_MAP_INFO_OFFSET)
    _expect(role_summary, "mapInfoSerialSize", EXPECTED_MAP_INFO_SIZE)
    _expect(role_summary, "emittedRoles", len(EFFECTIVE_ROLES))
    _expect(role_summary, "roleArraysConsumedExactly", True)

    by_key: dict[tuple[str, int], Mapping[str, object]] = {}
    for record in role_records[1:-1]:
        _expect(record, "record", "role")
        key = (str(record.get("team")), int(record.get("ordinal", -1)))
        if key in by_key:
            raise AuditError(f"duplicate cooked role row {key}")
        by_key[key] = record
    for role in EFFECTIVE_ROLES:
        record = by_key.get((role.team, role.ordinal))
        if record is None:
            raise AuditError(f"missing cooked role row {role.team}/{role.ordinal}")
        _expect(record, "sourceProperty", "NorthernRoles" if role.team == "north" else "SouthernRoles")
        _expect(record, "roleInfoClassPackageIndex", role.map_package_index)
        _expect(record, "roleInfoClassPath", f"Class'ROGame.{role.base_class}'")
        _expect(record, "count", role.count)
        _expect(record, "reverseCount", role.reverse_count)
        _expect(record, "serializedBytes", 102)

    if len(actor_records) != 3:
        raise AuditError(f"Cu Chi ROMapInfo actor record count drift: {len(actor_records)}")
    actor_header, actor, actor_summary = actor_records
    _expect(actor_header, "record", "header")
    _expect(actor_header, "mode", "actors")
    _expect(actor_header, "inputBytes", EXPECTED_MAP_BYTES)
    _expect(actor_header, "packageGuid", EXPECTED_MAP_GUID)
    _expect(actor_header, "uelibAssemblyVersion", EXPECTED_UELIB_ASSEMBLY_VERSION)
    _expect(actor_header, "uelibProductVersion", EXPECTED_UELIB_PRODUCT_VERSION)
    _expect(actor, "record", "actor")
    _expect(actor, "class", "ROMapInfo")
    _expect(actor, "exportIndex", EXPECTED_MAP_INFO_EXPORT)
    _expect(actor, "serialOffset", EXPECTED_MAP_INFO_OFFSET)
    _expect(actor, "serialSize", EXPECTED_MAP_INFO_SIZE)
    _expect(actor, "propertiesTruncated", False)
    properties = actor.get("properties")
    if not isinstance(properties, list):
        raise AuditError("Cu Chi ROMapInfo properties are missing")
    property_values: dict[str, object] = {}
    for value in properties:
        if not isinstance(value, Mapping) or not isinstance(value.get("name"), str):
            raise AuditError("malformed Cu Chi ROMapInfo property")
        name = str(value["name"])
        if name in property_values:
            raise AuditError(f"duplicate Cu Chi ROMapInfo property {name}")
        property_values[name] = value.get("value")
    expected_properties = {
        "NorthernForce": "ENorthernForces.NFOR_NLF",
        "DefendingTeam": "EDefendingTeam.DT_North",
        "DefendingTeam16": "EDefendingTeam.DT_North",
        "DefendingTeam32": "EDefendingTeam.DT_North",
        "DefendingTeam64": "EDefendingTeam.DT_North",
        "bIgnoreReverseCountNorth": "false",
        "bIgnoreReverseCountSouth": "false",
    }
    for name, expected in expected_properties.items():
        if property_values.get(name) != expected:
            raise AuditError(
                f"Cu Chi ROMapInfo {name} drift: expected {expected}, "
                f"found {property_values.get(name)}"
            )
    if "SouthernForce" in property_values:
        raise AuditError("Cu Chi unexpectedly overrides source-default SouthernForce")
    _expect(actor_summary, "record", "summary")
    _expect(actor_summary, "mode", "actors")
    _expect(actor_summary, "emittedActors", 1)
    _expect(actor_summary, "errors", 0)


def build_report(
    extractor_records: Sequence[Mapping[str, object]],
    map_role_records: Sequence[Mapping[str, object]],
    map_actor_records: Sequence[Mapping[str, object]],
    source_manifest_sha256: str,
) -> tuple[dict[str, object], ...]:
    """Validate table-only extractor records and build the Cu Chi report."""
    validate_map_evidence(map_role_records, map_actor_records)
    if source_manifest_sha256 != _source_manifest_digest():
        raise AuditError("source manifest validation was not completed")
    if len(extractor_records) < 3:
        raise AuditError("role-export stream is truncated")
    header = extractor_records[0]
    summary = extractor_records[-1]
    _expect(header, "record", "header")
    _expect(header, "mode", "role-exports")
    _expect(header, "inputBytes", EXPECTED_BYTES)
    _expect(header, "inputSha256", EXPECTED_SHA256)
    _expect(header, "sha256MatchedExpectation", True)
    _expect(header, "sha256StableAcrossTableRead", True)
    _expect(header, "packageGuid", EXPECTED_GUID)
    _expect(header, "packageName", "ROGame")
    _expect(header, "uelibAssemblyVersion", EXPECTED_UELIB_ASSEMBLY_VERSION)
    _expect(header, "uelibProductVersion", EXPECTED_UELIB_PRODUCT_VERSION)
    _expect(header, "packageFlags", EXPECTED_PACKAGE_FLAGS)
    _expect(header, "generationCount", EXPECTED_GENERATIONS)
    _expect(header, "exports", EXPECTED_EXPORTS)
    _expect(header, "names", EXPECTED_NAMES)
    _expect(header, "finalGenerationExports", EXPECTED_EXPORTS)
    _expect(header, "finalGenerationNames", EXPECTED_NAMES)
    _expect(header, "finalGenerationNetObjects", EXPECTED_NET_OBJECTS)
    _expect(summary, "record", "summary")
    _expect(summary, "mode", "role-exports")
    _expect(summary, "pairedRoles", EXPECTED_ROLE_PAIRS)
    _expect(summary, "uclassExports", EXPECTED_ROLE_PAIRS)
    _expect(summary, "cdoExports", EXPECTED_ROLE_PAIRS)
    _expect(summary, "pairsValidatedExactly", True)
    _expect(summary, "tableOnly", True)
    _expect(summary, "objectBaseSupplied", True)
    _expect(summary, "runtimeRolesAuthorized", False)

    exports: dict[str, Mapping[str, object]] = {}
    for record in extractor_records[1:-1]:
        if record.get("record") != "roleExport":
            raise AuditError(f"unexpected extractor record {record.get('record')!r}")
        role_class = record.get("roleClass")
        if not isinstance(role_class, str) or not role_class:
            raise AuditError("role export is missing roleClass")
        if role_class in exports:
            raise AuditError(f"duplicate role export {role_class}")
        _expect(record, "schemaVersion", 1)
        _expect(record, "objectBase", OBJECT_BASE)
        _expect(record, "derivedOnly", True)
        _expect(record, "authorizedByExtractor", False)
        _expect(
            record,
            "referenceDerivation",
            "PackageMap ObjectBase + linker export index",
        )
        uclass_index = record.get("uclassLinkerIndex")
        cdo_index = record.get("cdoLinkerIndex")
        uclass_reference = record.get("uclassStaticReference")
        cdo_reference = record.get("cdoStaticReference")
        if not all(
            isinstance(value, int)
            for value in (uclass_index, cdo_index, uclass_reference, cdo_reference)
        ):
            raise AuditError(f"role export indices are malformed for {role_class}")
        if uclass_reference != OBJECT_BASE + uclass_index:
            raise AuditError(f"UClass reference arithmetic drift for {role_class}")
        if cdo_reference != OBJECT_BASE + cdo_index:
            raise AuditError(f"CDO reference arithmetic drift for {role_class}")
        if cdo_index != uclass_index + 1:
            raise AuditError(f"pinned UClass/CDO adjacency drift for {role_class}")
        exports[role_class] = record

    if len(exports) != EXPECTED_ROLE_PAIRS:
        raise AuditError(
            f"role-export record count drift: expected {EXPECTED_ROLE_PAIRS}, "
            f"found {len(exports)}"
        )

    report: list[dict[str, object]] = [
        {
            "record": "header",
            "schemaVersion": 1,
            "map": "VNTE-CuChi",
            "mode": "Territories",
            "round": "normal-non-reversed-first-round",
            "artifact": "ROGame.u",
            "artifactBytes": EXPECTED_BYTES,
            "artifactSha256": EXPECTED_SHA256,
            "packageGuid": EXPECTED_GUID,
            "packageFlags": EXPECTED_PACKAGE_FLAGS,
            "generationCount": EXPECTED_GENERATIONS,
            "finalGenerationExports": EXPECTED_EXPORTS,
            "finalGenerationNames": EXPECTED_NAMES,
            "finalGenerationNetObjects": EXPECTED_NET_OBJECTS,
            "objectBase": OBJECT_BASE,
            "mapArtifact": "VNTE-CuChi.roe",
            "mapArtifactBytes": EXPECTED_MAP_BYTES,
            "mapArtifactSha256": EXPECTED_MAP_SHA256,
            "mapPackageGuid": EXPECTED_MAP_GUID,
            "sourceManifestSha256": source_manifest_sha256,
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
                    "assemblyVersion": EXPECTED_UNSAFE_ASSEMBLY_VERSION,
                }
            ],
            "reportAuthorizesRuntimeRoles": False,
            "runtimeSupportedRoleCount": len(KNOWN_CLASS_ZERO_REFS),
        }
    ]

    seen_references: set[int] = set()
    for role in EFFECTIVE_ROLES:
        export = exports.get(role.effective_class)
        if export is None:
            raise AuditError(f"missing effective role export {role.effective_class}")
        linker_index = export.get("uclassLinkerIndex")
        static_reference = export.get("uclassStaticReference")
        if not isinstance(linker_index, int) or linker_index <= 0:
            raise AuditError(f"invalid UClass linker index for {role.effective_class}")
        if not isinstance(static_reference, int):
            raise AuditError(f"missing UClass static reference for {role.effective_class}")
        if static_reference != OBJECT_BASE + linker_index:
            raise AuditError(f"static-reference arithmetic drift for {role.effective_class}")
        if static_reference in seen_references:
            raise AuditError(f"duplicate effective static reference {static_reference}")
        seen_references.add(static_reference)
        expected_identity = EXPECTED_EFFECTIVE_UCLASS.get(role.effective_class)
        if expected_identity != (linker_index, static_reference):
            raise AuditError(
                f"pinned UClass identity drift for {role.effective_class}: "
                f"expected {expected_identity}, found {(linker_index, static_reference)}"
            )
        expected_known = KNOWN_CLASS_ZERO_REFS.get(role.effective_class)
        if expected_known is not None and static_reference != expected_known:
            raise AuditError(
                f"known class-0 reference drift for {role.effective_class}: "
                f"expected {expected_known}, found {static_reference}"
            )
        report.append(
            {
                "record": "role",
                "schemaVersion": 1,
                "team": role.team,
                "ordinal": role.ordinal,
                "baseClass": role.base_class,
                "effectiveClass": role.effective_class,
                "classIndex": role.class_index,
                "count": role.count,
                "reverseCount": role.reverse_count,
                "normalLimit": role.count,
                "mapRoleInfoClassPackageIndex": role.map_package_index,
                "uclassNetIndex": linker_index,
                "uclassStaticReference": static_reference,
                "authorizedByReport": False,
                "runtimeSupported": expected_known is not None,
                "runtimeBlockers": []
                if expected_known is not None
                else ["live-h175", "role-loadout-owning-pawn-graph"],
            }
        )

    report.append(
        {
            "record": "summary",
            "schemaVersion": 1,
            "roles": len(EFFECTIVE_ROLES),
            "teams": 2,
            "knownClassZeroReferencesCrossChecked": len(KNOWN_CLASS_ZERO_REFS),
            "uniqueStaticReferences": len(seen_references),
            "reportAuthorizesRuntimeRoles": False,
            "runtimeSupportedRoles": len(KNOWN_CLASS_ZERO_REFS),
            "runtimeUnsupportedRoles": len(EFFECTIVE_ROLES) - len(KNOWN_CLASS_ZERO_REFS),
            "vehiclePilotCommanderIncluded": False,
        }
    )
    return tuple(report)


def _run_extractor_command(
    command: Sequence[str], destination: Path, label: str
) -> None:
    try:
        completed = subprocess.run(
            command,
            check=False,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=180,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise AuditError(f"{label} extractor failed to run: {exc}") from exc
    if completed.returncode != 0:
        detail = completed.stderr.strip() or completed.stdout.strip()
        raise AuditError(
            f"{label} extractor exited {completed.returncode}: {detail[:2048]}"
        )
    if not destination.is_file():
        raise AuditError(f"{label} extractor did not create its JSONL output")


def run_role_export_extractor(
    package: Path, wrapper: Path, uelib: Path, destination: Path
) -> None:
    command = (
        "powershell",
        "-NoProfile",
        "-File",
        str(wrapper),
        str(package),
        "-ForceRebuild",
        "-ExpectedExecutableSha256",
        EXPECTED_EXTRACTOR_EXECUTABLE_SHA256,
        "-UELibPath",
        str(uelib),
        "-RoleExports",
        "-ExpectedPackageGuid",
        EXPECTED_GUID,
        "-ExpectedSha256",
        EXPECTED_SHA256,
        "-ObjectBase",
        str(OBJECT_BASE),
        "-MaxInputMiB",
        "64",
        "-OutputPath",
        str(destination),
    )
    _run_extractor_command(command, destination, "role-export")


def run_map_extractors(
    map_path: Path,
    wrapper: Path,
    uelib: Path,
    role_destination: Path,
    actor_destination: Path,
) -> None:
    common = (
        "powershell",
        "-NoProfile",
        "-File",
        str(wrapper),
        str(map_path),
        "-ForceRebuild",
        "-ExpectedExecutableSha256",
        EXPECTED_EXTRACTOR_EXECUTABLE_SHA256,
        "-UELibPath",
        str(uelib),
        "-MaxInputMiB",
        "300",
    )
    role_command = common + (
        "-RoleInfo",
        "-OutputPath",
        str(role_destination),
    )
    _run_extractor_command(role_command, role_destination, "Cu Chi role-info")
    actor_command = common + (
        "-ClassPattern",
        "ROMapInfo",
        "-AllProperties",
        "-MaxActors",
        "2",
        "-MaxProperties",
        "4096",
        "-MaxValueChars",
        "65536",
        "-OutputPath",
        str(actor_destination),
    )
    _run_extractor_command(actor_command, actor_destination, "Cu Chi ROMapInfo")


def write_jsonl(
    records: Iterable[Mapping[str, object]], destination: Path | None, overwrite: bool
) -> None:
    rendered = "".join(_json(record) + "\n" for record in records)
    if destination is None:
        sys.stdout.write(rendered)
        return
    destination = destination.resolve()
    if destination.exists() and not overwrite:
        raise AuditError(f"output exists; pass --overwrite: {destination}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary_path: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            newline="\n",
            dir=destination.parent,
            prefix=destination.name + ".",
            suffix=".tmp",
            delete=False,
        ) as output:
            temporary_path = Path(output.name)
            output.write(rendered)
            output.flush()
            os.fsync(output.fileno())
        if overwrite:
            os.replace(temporary_path, destination)
        else:
            os.rename(temporary_path, destination)
        temporary_path = None
    except OSError as exc:
        raise AuditError(f"could not write report {destination}: {exc}") from exc
    finally:
        if temporary_path is not None:
            try:
                temporary_path.unlink(missing_ok=True)
            except OSError:
                pass


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", type=Path, default=DEFAULT_PACKAGE)
    parser.add_argument("--map", type=Path, default=DEFAULT_MAP)
    parser.add_argument("--source-root", type=Path, default=DEFAULT_SOURCE_ROOT)
    parser.add_argument("--wrapper", type=Path, default=DEFAULT_WRAPPER)
    parser.add_argument("--uelib", type=Path, default=DEFAULT_UELIB)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--overwrite", action="store_true")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        extractor_source = args.wrapper.resolve().with_name(
            EXPECTED_EXTRACTOR_SOURCE_NAME
        )
        unsafe_dependency = args.uelib.resolve().with_name(EXPECTED_UNSAFE_NAME)
        for label, path in (
            ("ROGame.u", args.package),
            ("VNTE-CuChi.roe", args.map),
            ("extractor wrapper", args.wrapper),
            ("extractor source", extractor_source),
            ("Eliot.UELib.dll", args.uelib),
            (EXPECTED_UNSAFE_NAME, unsafe_dependency),
        ):
            if not path.is_file():
                raise AuditError(f"{label} does not exist: {path}")
        if args.output is not None:
            output = args.output.resolve()
            protected = {
                args.package.resolve(),
                args.map.resolve(),
                args.wrapper.resolve(),
                extractor_source,
                args.uelib.resolve(),
                unsafe_dependency,
            }
            protected.update(
                (args.source_root / name).resolve()
                for name in EXPECTED_SOURCE_FILES
            )
            if output in protected:
                raise AuditError("output must not overwrite an input")
            if output.exists() and any(
                source.exists() and os.path.samefile(output, source)
                for source in protected
            ):
                raise AuditError("output must not alias an input")
            try:
                output.relative_to(args.package.resolve().parent)
            except ValueError:
                pass
            else:
                raise AuditError("output must not be written inside the game package directory")
            try:
                output.relative_to(args.source_root.resolve())
            except ValueError:
                pass
            else:
                raise AuditError("output must not be written inside the pinned source tree")

        validate_file_identity(
            args.wrapper,
            EXPECTED_WRAPPER_BYTES,
            EXPECTED_WRAPPER_SHA256,
            "extractor wrapper",
        )
        validate_file_identity(
            extractor_source,
            EXPECTED_EXTRACTOR_SOURCE_BYTES,
            EXPECTED_EXTRACTOR_SOURCE_SHA256,
            "extractor source",
        )
        validate_file_identity(
            args.package, EXPECTED_BYTES, EXPECTED_SHA256, "ROGame.u"
        )
        validate_file_identity(
            args.map, EXPECTED_MAP_BYTES, EXPECTED_MAP_SHA256, "VNTE-CuChi.roe"
        )
        validate_file_identity(
            args.uelib,
            EXPECTED_UELIB_BYTES,
            EXPECTED_UELIB_SHA256,
            "Eliot.UELib.dll",
        )
        validate_file_identity(
            unsafe_dependency,
            EXPECTED_UNSAFE_BYTES,
            EXPECTED_UNSAFE_SHA256,
            EXPECTED_UNSAFE_NAME,
        )
        source_manifest_sha256 = validate_source_evidence(args.source_root)

        with tempfile.TemporaryDirectory(prefix="rsv2-role-refs-") as temporary:
            extractor_output = Path(temporary, "role-exports.jsonl")
            map_role_output = Path(temporary, "map-role-info.jsonl")
            map_actor_output = Path(temporary, "map-actor.jsonl")
            run_role_export_extractor(
                args.package.resolve(),
                args.wrapper.resolve(),
                args.uelib.resolve(),
                extractor_output,
            )
            run_map_extractors(
                args.map.resolve(),
                args.wrapper.resolve(),
                args.uelib.resolve(),
                map_role_output,
                map_actor_output,
            )
            validate_file_identity(
                args.wrapper,
                EXPECTED_WRAPPER_BYTES,
                EXPECTED_WRAPPER_SHA256,
                "extractor wrapper after audit",
            )
            validate_file_identity(
                extractor_source,
                EXPECTED_EXTRACTOR_SOURCE_BYTES,
                EXPECTED_EXTRACTOR_SOURCE_SHA256,
                "extractor source after audit",
            )
            validate_file_identity(
                args.package, EXPECTED_BYTES, EXPECTED_SHA256, "ROGame.u after audit"
            )
            validate_file_identity(
                args.map,
                EXPECTED_MAP_BYTES,
                EXPECTED_MAP_SHA256,
                "VNTE-CuChi.roe after audit",
            )
            validate_file_identity(
                args.uelib,
                EXPECTED_UELIB_BYTES,
                EXPECTED_UELIB_SHA256,
                "Eliot.UELib.dll after audit",
            )
            validate_file_identity(
                unsafe_dependency,
                EXPECTED_UNSAFE_BYTES,
                EXPECTED_UNSAFE_SHA256,
                EXPECTED_UNSAFE_NAME + " after audit",
            )
            if validate_source_evidence(args.source_root) != source_manifest_sha256:
                raise AuditError("pinned source changed during the audit")
            report = build_report(
                load_jsonl(extractor_output),
                load_jsonl(map_role_output),
                load_jsonl(map_actor_output),
                source_manifest_sha256,
            )
        write_jsonl(report, args.output, args.overwrite)
        return 0
    except AuditError as exc:
        print(_json({"record": "summary", "fatal": True, "error": str(exc)}), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
