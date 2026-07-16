#!/usr/bin/env python3
"""Ground configured RS2 map GUIDs with sequential, read-only extraction.

The helper intentionally does not edit config/maps.ini or C++ sources. It finds
configured .roe packages absent from RetailBootstrap's constexpr lookup table,
runs the existing PowerShell extractor in ClassesOnly/MaxClasses=1 mode, and
records SHA-256 before and after every invocation.
"""

from __future__ import annotations

import argparse
import configparser
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from typing import Callable, Dict, List, Mapping, Optional, Sequence, TextIO, Tuple


GUID_RE = re.compile(r"^[0-9A-Fa-f]{32}$")
MAP_ID_RE = re.compile(r"^[A-Za-z0-9_-]+$")
TABLE_BEGIN = "// BEGIN SOURCE-GROUNDED MAP GUIDS"
TABLE_END = "// END SOURCE-GROUNDED MAP GUIDS"
TABLE_DECL_RE = re.compile(
    r"constexpr\s+std::array<MapGuidEntry,\s*(\d+)>\s+kMapPackageGuids"
)
TABLE_ENTRY_RE = re.compile(
    r'\{\s*"([^"]+)"\s*,\s*PackageGuid\("([^"]+)"\)\s*\}'
)


class ValidationError(RuntimeError):
    """Input, output, or grounded-data validation failed."""


class MutationError(ValidationError):
    """A supposedly read-only extractor changed its source package."""


@dataclass(frozen=True)
class MapSpec:
    map_id: str
    source: Path


@dataclass(frozen=True)
class GroundedMap:
    map_id: str
    source: Path
    size: int
    sha256_before: str
    sha256_after: str
    package_guid: str


@dataclass(frozen=True)
class GroundingResult:
    attempted: int
    grounded: Tuple[GroundedMap, ...]
    failures: Tuple[Mapping[str, object], ...]


def _json(record: Mapping[str, object]) -> str:
    return json.dumps(record, ensure_ascii=True, separators=(",", ":"), sort_keys=True)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest().upper()


def display_guid_to_wire_bytes(package_guid: str) -> bytes:
    if not GUID_RE.fullmatch(package_guid):
        raise ValidationError(f"malformed package GUID: {package_guid!r}")
    package_guid = package_guid.upper()
    output = bytearray()
    for word_start in range(0, 32, 8):
        word = bytes.fromhex(package_guid[word_start : word_start + 8])
        output.extend(reversed(word))
    return bytes(output)


def read_configured_maps(config_path: Path) -> List[MapSpec]:
    parser = configparser.RawConfigParser(interpolation=None, strict=True)
    parser.optionxform = str.lower
    try:
        with config_path.open("r", encoding="utf-8-sig") as stream:
            parser.read_file(stream)
    except (OSError, configparser.Error) as exc:
        raise ValidationError(f"could not parse map config {config_path}: {exc}") from exc

    maps: List[MapSpec] = []
    seen_ids: Dict[str, str] = {}
    seen_paths: Dict[str, str] = {}
    for section in parser.sections():
        folded = section.casefold()
        if folded in seen_ids:
            raise ValidationError(
                f"duplicate map id (case-insensitive): {seen_ids[folded]!r} and {section!r}"
            )
        if not MAP_ID_RE.fullmatch(section):
            raise ValidationError(f"invalid map id in config: {section!r}")
        raw_source = parser.get(section, "file", fallback="").strip()
        if not raw_source:
            raise ValidationError(f"map {section!r} has no file entry")
        expanded = os.path.expandvars(os.path.expanduser(raw_source))
        source = Path(expanded)
        if not source.is_absolute():
            source = config_path.parent / source
        source = source.resolve(strict=False)
        if source.suffix.casefold() != ".roe":
            raise ValidationError(f"map {section!r} does not reference a .roe package: {source}")
        path_key = os.path.normcase(str(source))
        if path_key in seen_paths:
            raise ValidationError(
                f"duplicate configured package path for {seen_paths[path_key]!r} and {section!r}: {source}"
            )
        seen_ids[folded] = section
        seen_paths[path_key] = section
        maps.append(MapSpec(section, source))
    if not maps:
        raise ValidationError(f"map config contains no sections: {config_path}")
    return maps


def read_resolver_table(source_path: Path) -> Dict[str, Tuple[str, str]]:
    try:
        source = source_path.read_text(encoding="utf-8-sig")
    except OSError as exc:
        raise ValidationError(f"could not read resolver source {source_path}: {exc}") from exc
    begin = source.find(TABLE_BEGIN)
    end = source.find(TABLE_END, begin + len(TABLE_BEGIN))
    if begin < 0 or end < 0 or end <= begin:
        raise ValidationError("resolver source is missing the grounded GUID table markers")
    table = source[begin:end]
    declaration = TABLE_DECL_RE.search(table)
    if declaration is None:
        raise ValidationError("resolver source is missing the kMapPackageGuids declaration")
    entries = TABLE_ENTRY_RE.findall(table)
    declared_count = int(declaration.group(1))
    if declared_count != len(entries):
        raise ValidationError(
            f"resolver table declares {declared_count} entries but contains {len(entries)}"
        )

    resolved: Dict[str, Tuple[str, str]] = {}
    guid_owner: Dict[str, str] = {}
    for map_id, package_guid in entries:
        folded = map_id.casefold()
        if not MAP_ID_RE.fullmatch(map_id):
            raise ValidationError(f"invalid resolver map id: {map_id!r}")
        if folded in resolved:
            raise ValidationError(
                f"duplicate resolver map id: {resolved[folded][0]!r} and {map_id!r}"
            )
        if not GUID_RE.fullmatch(package_guid):
            raise ValidationError(f"malformed resolver GUID for {map_id}: {package_guid!r}")
        package_guid = package_guid.upper()
        if int(package_guid, 16) == 0:
            raise ValidationError(f"zero resolver GUID for {map_id}")
        if package_guid in guid_owner:
            raise ValidationError(
                f"duplicate resolver GUID {package_guid} for {guid_owner[package_guid]!r} and {map_id!r}"
            )
        display_guid_to_wire_bytes(package_guid)
        resolved[folded] = (map_id, package_guid)
        guid_owner[package_guid] = map_id
    return resolved


def parse_extractor_output(output_path: Path, expected_source: Path) -> str:
    try:
        records = [
            json.loads(line)
            for line in output_path.read_text(encoding="utf-8-sig").splitlines()
            if line.strip()
        ]
    except (OSError, json.JSONDecodeError) as exc:
        raise ValidationError(f"invalid extractor JSONL {output_path}: {exc}") from exc
    if len(records) < 2 or not isinstance(records[0], dict):
        raise ValidationError("extractor output is missing header/summary records")
    header = records[0]
    summary = records[-1]
    if header.get("record") != "header" or header.get("mode") != "classes":
        raise ValidationError("extractor output did not use bounded classes mode")
    if not isinstance(summary, dict) or summary.get("record") != "summary":
        raise ValidationError("extractor output is missing its final summary record")
    errors = summary.get("errors", 0)
    if not isinstance(errors, int) or isinstance(errors, bool):
        raise ValidationError(f"extractor summary has an invalid errors count: {errors!r}")
    if errors != 0:
        raise ValidationError(f"extractor reported {errors} errors")
    header_input = Path(str(header.get("input", ""))).resolve(strict=False)
    if os.path.normcase(str(header_input)) != os.path.normcase(str(expected_source.resolve(strict=False))):
        raise ValidationError(
            f"extractor header input mismatch: expected {expected_source}, got {header_input}"
        )
    package_guid = str(header.get("packageGuid", "")).upper()
    display_guid_to_wire_bytes(package_guid)
    if int(package_guid, 16) == 0:
        raise ValidationError(f"extractor returned a zero GUID for {expected_source}")
    return package_guid


def invoke_extractor(
    wrapper: Path,
    source: Path,
    output: Path,
    powershell: str,
    max_input_mib: int,
    uelib: Optional[Path],
) -> str:
    command = [
        powershell,
        "-NoProfile",
        "-File",
        str(wrapper),
        "-InputPath",
        str(source),
        "-OutputPath",
        str(output),
        "-ClassesOnly",
        "-MaxClasses",
        "1",
        "-MaxInputMiB",
        str(max_input_mib),
    ]
    if uelib is not None:
        command.extend(["-UELibPath", str(uelib)])
    completed = subprocess.run(
        command,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
    )
    if completed.returncode != 0:
        detail = (completed.stderr or completed.stdout).strip().replace("\r", " ").replace("\n", " ")
        if len(detail) > 800:
            detail = detail[-800:]
        raise ValidationError(
            f"extractor exited {completed.returncode} for {source}: {detail or 'no diagnostic'}"
        )
    return parse_extractor_output(output, source)


Extractor = Callable[[Path], str]


def ground_missing_maps(
    configured: Sequence[MapSpec],
    resolver: Mapping[str, Tuple[str, str]],
    extract: Extractor,
    emit: Callable[[Mapping[str, object]], None],
) -> GroundingResult:
    known_guids = {package_guid: map_id for map_id, package_guid in resolver.values()}
    grounded: List[GroundedMap] = []
    failures: List[Mapping[str, object]] = []
    attempted = 0
    for spec in configured:
        if spec.map_id.casefold() in resolver:
            continue
        attempted += 1
        if not spec.source.is_file():
            failure = {
                "record": "error",
                "map": spec.map_id,
                "source": str(spec.source),
                "error": "configured .roe package does not exist",
            }
            failures.append(failure)
            emit(failure)
            continue

        before = sha256_file(spec.source)
        size = spec.source.stat().st_size
        try:
            package_guid = extract(spec.source)
        except Exception as exc:
            after = sha256_file(spec.source)
            if before != after:
                raise MutationError(
                    f"source package mutated during failed extraction: {spec.source}; "
                    f"before={before} after={after}"
                ) from exc
            failure = {
                "record": "error",
                "map": spec.map_id,
                "source": str(spec.source),
                "sha256Before": before,
                "sha256After": after,
                "error": str(exc),
            }
            failures.append(failure)
            emit(failure)
            continue

        after = sha256_file(spec.source)
        if before != after:
            raise MutationError(
                f"source package mutated: {spec.source}; before={before} after={after}"
            )
        package_guid = package_guid.upper()
        wire = display_guid_to_wire_bytes(package_guid)
        if int(package_guid, 16) == 0:
            raise ValidationError(f"zero package GUID for {spec.map_id}")
        if package_guid in known_guids:
            raise ValidationError(
                f"duplicate package GUID {package_guid} for {known_guids[package_guid]!r} and {spec.map_id!r}"
            )
        known_guids[package_guid] = spec.map_id
        item = GroundedMap(
            spec.map_id, spec.source, size, before, after, package_guid
        )
        grounded.append(item)
        emit(
            {
                "record": "mapGuid",
                "map": spec.map_id,
                "source": str(spec.source),
                "inputBytes": size,
                "sha256Before": before,
                "sha256After": after,
                "packageGuid": package_guid,
                "wireGuid": wire.hex().upper(),
            }
        )
    return GroundingResult(attempted, tuple(grounded), tuple(failures))


def _parser(repo_root: Path) -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, default=repo_root / "config" / "maps.ini")
    parser.add_argument(
        "--resolver-source",
        type=Path,
        default=repo_root / "src" / "Network" / "RetailBootstrap.cpp",
    )
    parser.add_argument(
        "--wrapper",
        type=Path,
        default=repo_root / "tools" / "extract_cooked_map_metadata.ps1",
    )
    parser.add_argument("--powershell", default="powershell")
    parser.add_argument(
        "--uelib",
        type=Path,
        default=Path(r"D:\RE-Tools\UE-Explorer\Eliot.UELib.dll"),
    )
    parser.add_argument("--max-input-mib", type=int, default=1536)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--overwrite", action="store_true")
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    repo_root = Path(__file__).resolve().parent.parent
    args = _parser(repo_root).parse_args(argv)
    output_stream: Optional[TextIO] = None
    summary: Mapping[str, object]
    try:
        if args.max_input_mib < 1 or args.max_input_mib > 8192:
            raise ValidationError("--max-input-mib must be between 1 and 8192")
        config_path = args.config.resolve(strict=True)
        resolver_source = args.resolver_source.resolve(strict=True)
        wrapper = args.wrapper.resolve(strict=True)
        uelib = args.uelib.resolve(strict=True)
        configured = read_configured_maps(config_path)
        resolver = read_resolver_table(resolver_source)

        if args.output is not None:
            output = args.output.resolve(strict=False)
            protected = {
                os.path.normcase(str(config_path)),
                os.path.normcase(str(resolver_source)),
                os.path.normcase(str(wrapper)),
                os.path.normcase(str(uelib)),
                *(os.path.normcase(str(item.source)) for item in configured),
            }
            if os.path.normcase(str(output)) in protected:
                raise ValidationError(f"refusing to overwrite an input/protected path: {output}")
            output.parent.mkdir(parents=True, exist_ok=True)
            mode = "w" if args.overwrite else "x"
            output_stream = output.open(mode, encoding="utf-8", newline="\n")

        def emit(record: Mapping[str, object]) -> None:
            line = _json(record)
            if output_stream is None:
                print(line)
            else:
                output_stream.write(line + "\n")
                output_stream.flush()

        with tempfile.TemporaryDirectory(prefix="rs2-map-guids-") as temp_dir:
            temporary = Path(temp_dir)
            counter = 0

            def extract(source: Path) -> str:
                nonlocal counter
                counter += 1
                extractor_output = temporary / f"{counter:03d}.jsonl"
                return invoke_extractor(
                    wrapper,
                    source,
                    extractor_output,
                    args.powershell,
                    args.max_input_mib,
                    uelib,
                )

            result = ground_missing_maps(configured, resolver, extract, emit)
        exit_code = 1 if result.failures else 0
        summary = {
            "record": "summary",
            "configuredMaps": len(configured),
            "resolverMaps": len(resolver),
            "attempted": result.attempted,
            "grounded": len(result.grounded),
            "failed": len(result.failures),
            "exitCode": exit_code,
        }
        emit(summary)
        if output_stream is not None:
            print(_json(summary))
        return exit_code
    except (OSError, ValidationError, ValueError) as exc:
        summary = {"record": "summary", "failed": 1, "fatal": True, "error": str(exc), "exitCode": 2}
        line = _json(summary)
        if output_stream is None:
            print(line)
        else:
            output_stream.write(line + "\n")
            output_stream.flush()
            print(line)
        return 2
    finally:
        if output_stream is not None:
            output_stream.close()


if __name__ == "__main__":
    raise SystemExit(main())
