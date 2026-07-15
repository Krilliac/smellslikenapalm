#!/usr/bin/env python3
"""Audit captured UE3 NMT_Uses identities against an installed RS2 package tree.

This is a stdlib-only drift detector. It parses the canonical
``replication_bootstrap.bin`` record stream, indexes only ``.u``, ``.upk`` and
``.roe`` files, reads at most 8 KiB from each matching package, and compares the
raw package GUID, package flags and package-summary generation metadata. Audit
mode is read-only. An explicit candidate mode can write a separate GUID-only
bootstrap plus provenance, but it never mutates game packages or the canonical
bootstrap.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Mapping, Sequence


UE3_PACKAGE_TAG = 0x9E2A83C1
NMT_USES = 0x07
PACKAGE_HEADER_READ_LIMIT = 8192
SUPPORTED_PACKAGE_VERSION = bytes.fromhex("FD020303")
SUPPORTED_EXTENSIONS = frozenset({"u", "upk", "roe"})
DEFAULT_INSTALL_ROOT = Path(
    os.environ.get(
        "RS2_INSTALL_ROOT",
        r"D:\SteamLibrary\steamapps\common\Rising Storm 2\ROGame\BrewedPC",
    )
)


class AuditError(ValueError):
    """Raised when captured or installed package metadata is malformed."""


@dataclass(frozen=True)
class UsesRecord:
    name: str
    extension: str
    guid: bytes
    package_flags: int
    generation: int
    outer_record: int
    payload_offset: int
    stream_guid_offset: int

    @property
    def key(self) -> tuple[str, str]:
        return self.name.casefold(), self.extension.casefold()


@dataclass(frozen=True)
class PackageSummary:
    path: Path
    guid: bytes
    package_flags: int
    generation_count: int
    generations: tuple[tuple[int, int, int], ...]
    package_version: bytes
    engine_version: int | None


def _u32(data: bytes, offset: int, field: str) -> int:
    if offset < 0 or offset + 4 > len(data):
        raise AuditError(f"truncated {field} at byte {offset}")
    return struct.unpack_from("<I", data, offset)[0]


def _i32(data: bytes, offset: int, field: str) -> int:
    value = _u32(data, offset, field)
    return value - 0x100000000 if value & 0x80000000 else value


def _fstring(data: bytes, offset: int, field: str) -> tuple[str, int]:
    count = _i32(data, offset, f"{field} length")
    cursor = offset + 4
    if count == 0:
        return "", cursor
    if count > 0:
        if count > len(data) - cursor:
            raise AuditError(f"truncated ANSI {field} at byte {offset}")
        raw = data[cursor : cursor + count]
        if not raw or raw[-1] != 0 or b"\0" in raw[:-1]:
            raise AuditError(f"invalid ANSI {field} terminator at byte {offset}")
        try:
            return raw[:-1].decode("ascii"), cursor + count
        except UnicodeDecodeError as exc:
            raise AuditError(f"non-ASCII ANSI {field} at byte {offset}") from exc

    code_units = -count
    byte_count = code_units * 2
    if code_units > (len(data) - cursor) // 2:
        raise AuditError(f"truncated UTF-16 {field} at byte {offset}")
    raw = data[cursor : cursor + byte_count]
    if len(raw) < 2 or raw[-2:] != b"\0\0":
        raise AuditError(f"invalid UTF-16 {field} terminator at byte {offset}")
    try:
        value = raw[:-2].decode("utf-16-le")
    except UnicodeDecodeError as exc:
        raise AuditError(f"invalid UTF-16 {field} at byte {offset}") from exc
    if "\0" in value:
        raise AuditError(f"invalid UTF-16 {field} terminator at byte {offset}")
    return value, cursor + byte_count


def display_guid(raw: bytes) -> str:
    """Render raw UE3 FGuid bytes as four displayed hexadecimal uint32 words."""
    if len(raw) != 16:
        raise AuditError(f"FGuid must be 16 bytes, got {len(raw)}")
    return "".join(raw[index : index + 4][::-1].hex().upper()
                   for index in range(0, 16, 4))


def _parse_uses(
    payload: bytes, offset: int, outer_record: int, payload_stream_offset: int
) -> tuple[UsesRecord, int]:
    begin = offset
    if offset >= len(payload) or payload[offset] != NMT_USES:
        raise AuditError(f"NMT_Uses tag missing at record {outer_record} byte {offset}")
    offset += 1
    if offset + 16 > len(payload):
        raise AuditError(f"truncated NMT_Uses GUID at record {outer_record} byte {begin}")
    guid_offset = offset
    guid = payload[offset : offset + 16]
    offset += 16
    name, offset = _fstring(payload, offset, "package name")
    extension, offset = _fstring(payload, offset, "package extension")
    if not name or not extension:
        raise AuditError(f"empty package identity at record {outer_record} byte {begin}")
    flags = _u32(payload, offset, "NMT_Uses package flags")
    generation = _u32(payload, offset + 4, "NMT_Uses generation")
    offset += 8
    download, offset = _fstring(payload, offset, "NMT_Uses download field")
    if download != "None":
        raise AuditError(
            f"unsupported NMT_Uses download field {download!r} for {name}.{extension}"
        )
    if offset + 8 > len(payload):
        raise AuditError(f"truncated NMT_Uses trailer for {name}.{extension}")
    trailer = payload[offset : offset + 8]
    if trailer != bytes(8):
        raise AuditError(f"nonzero NMT_Uses trailer for {name}.{extension}")
    offset += 8
    if guid == bytes(16):
        raise AuditError(f"zero captured GUID for {name}.{extension}")
    return UsesRecord(
        name,
        extension,
        guid,
        flags,
        generation,
        outer_record,
        begin,
        payload_stream_offset + guid_offset,
    ), offset


def parse_bootstrap(data: bytes) -> tuple[UsesRecord, ...]:
    """Parse every leading contiguous NMT_Uses record in the outer stream."""
    if not data:
        raise AuditError("bootstrap stream is empty")
    records: list[UsesRecord] = []
    seen: dict[tuple[str, str], UsesRecord] = {}
    cursor = 0
    outer_record = 0
    while cursor < len(data):
        length = _u32(data, cursor, "outer record length")
        cursor += 4
        if length == 0 or length > len(data) - cursor:
            raise AuditError(f"zero-length or truncated outer record {outer_record}")
        payload_stream_offset = cursor
        payload = data[cursor : cursor + length]
        cursor += length
        payload_offset = 0
        while payload_offset < len(payload) and payload[payload_offset] == NMT_USES:
            record, payload_offset = _parse_uses(
                payload, payload_offset, outer_record, payload_stream_offset
            )
            if record.key in seen:
                prior = seen[record.key]
                raise AuditError(
                    f"duplicate captured package {record.name}.{record.extension} "
                    f"in outer records {prior.outer_record} and {outer_record}"
                )
            seen[record.key] = record
            records.append(record)
        outer_record += 1
    if not records:
        raise AuditError("bootstrap contains no NMT_Uses records")
    return tuple(records)


def parse_package_summary(path: Path) -> PackageSummary:
    """Read bounded UE3 package-summary identity without loading package exports."""
    try:
        with path.open("rb") as package:
            package_size = os.fstat(package.fileno()).st_size
            data = package.read(PACKAGE_HEADER_READ_LIMIT)
    except OSError as exc:
        raise AuditError(f"could not read installed package {path}: {exc}") from exc
    if len(data) < 12:
        raise AuditError(f"installed package header is truncated: {path}")
    if _u32(data, 0, "package tag") != UE3_PACKAGE_TAG:
        raise AuditError(f"installed file is not a UE3 package: {path}")
    package_version = data[4:8]
    if package_version != SUPPORTED_PACKAGE_VERSION:
        raise AuditError(
            f"unsupported package-summary version {package_version.hex().upper()}: {path}"
        )
    header_size = _u32(data, 8, "package header size")
    if header_size == 0 or header_size > package_size:
        raise AuditError(
            f"invalid package header size {header_size} for {package_size}-byte file: {path}"
        )
    _, cursor = _fstring(data, 12, "package folder")
    package_flags = _u32(data, cursor, "package flags")
    cursor += 4
    # UE3 EngineVersion 7258 summary: eleven uint32 count/offset fields precede Guid.
    cursor += 11 * 4
    if cursor + 20 > len(data):
        raise AuditError(f"installed package identity is truncated: {path}")
    guid = data[cursor : cursor + 16]
    cursor += 16
    generation_count = _u32(data, cursor, "package generation count")
    cursor += 4
    if guid == bytes(16):
        raise AuditError(f"installed package has a zero GUID: {path}")
    if generation_count > 1024:
        raise AuditError(
            f"installed package generation count is implausible ({generation_count}): {path}"
        )
    generation_bytes = generation_count * 12
    if header_size < cursor + generation_bytes:
        raise AuditError(
            f"package header ends inside generation table: {path}"
        )
    generations = tuple(
        struct.unpack_from("<III", data, cursor + index * 12)
        for index in range(generation_count)
    )
    engine_version = None
    if cursor + generation_bytes + 4 <= len(data):
        engine_version = _u32(data, cursor + generation_bytes, "engine version")
    return PackageSummary(
        path,
        guid,
        package_flags,
        generation_count,
        generations,
        package_version,
        engine_version,
    )


def index_installed_packages(
    root: Path,
) -> tuple[dict[tuple[str, str], tuple[Path, ...]], tuple[str, ...]]:
    """Index exact stem/extension identities without following directory links."""
    if not root.is_dir():
        raise AuditError(f"installed package root is not a directory: {root}")
    mutable: dict[tuple[str, str], list[Path]] = {}
    scan_errors: list[str] = []

    def onerror(error: OSError) -> None:
        scan_errors.append(str(error))

    for directory, dirnames, filenames in os.walk(root, onerror=onerror, followlinks=False):
        # os.walk can include linked directories in dirnames even when it will not
        # recurse through them. Drop them explicitly so the audit never escapes root.
        dirnames[:] = [
            name for name in dirnames
            if not Path(directory, name).is_symlink()
        ]
        for filename in filenames:
            path = Path(directory, filename)
            if path.is_symlink():
                continue
            extension = path.suffix[1:].casefold()
            if extension not in SUPPORTED_EXTENSIONS:
                continue
            key = path.stem.casefold(), extension
            mutable.setdefault(key, []).append(path.absolute())
    index = {
        key: tuple(sorted(paths, key=lambda candidate: str(candidate).casefold()))
        for key, paths in mutable.items()
    }
    return index, tuple(scan_errors)


def audit_records(
    captured: Iterable[UsesRecord],
    installed: Mapping[tuple[str, str], Sequence[Path]],
) -> tuple[dict[str, object], ...]:
    results: list[dict[str, object]] = []
    for record in captured:
        base: dict[str, object] = {
            "record": "package",
            "package": record.name,
            "extension": record.extension,
            "captured": {
                "guid": display_guid(record.guid),
                "rawGuid": record.guid.hex().upper(),
                "packageFlags": f"0x{record.package_flags:08X}",
                "generation": record.generation,
                "outerRecord": record.outer_record,
                "payloadOffset": record.payload_offset,
            },
        }
        candidates = tuple(installed.get(record.key, ()))
        if not candidates:
            base.update(status="unresolved", candidates=[])
            results.append(base)
            continue
        if len(candidates) != 1:
            base.update(
                status="ambiguous",
                candidates=[str(path) for path in candidates],
            )
            results.append(base)
            continue
        path = candidates[0]
        try:
            summary = parse_package_summary(path)
        except AuditError as exc:
            base.update(status="error", path=str(path), error=str(exc))
            results.append(base)
            continue
        drift_fields: list[str] = []
        if record.guid != summary.guid:
            drift_fields.append("guid")
        if record.package_flags != summary.package_flags:
            drift_fields.append("packageFlags")
        if record.generation != summary.generation_count:
            drift_fields.append("generation")
        base.update(
            status="drift" if drift_fields else "matched",
            path=str(path),
            installed={
                "guid": display_guid(summary.guid),
                "rawGuid": summary.guid.hex().upper(),
                "packageFlags": f"0x{summary.package_flags:08X}",
                "generationCount": summary.generation_count,
                "generations": [
                    {
                        "exportCount": generation[0],
                        "nameCount": generation[1],
                        "netObjectCount": generation[2],
                    }
                    for generation in summary.generations
                ],
                "packageVersion": summary.package_version.hex().upper(),
                "engineVersion": summary.engine_version,
            },
            driftFields=drift_fields,
        )
        results.append(base)
    return tuple(results)


def summarize(
    results: Sequence[Mapping[str, object]], scan_errors: Sequence[str] = ()
) -> dict[str, object]:
    counts = {
        status: sum(item.get("status") == status for item in results)
        for status in ("matched", "drift", "unresolved", "ambiguous", "error")
    }
    failed = len(results) - counts["matched"] + len(scan_errors)
    return {
        "record": "summary",
        "packages": len(results),
        **counts,
        "scanErrors": len(scan_errors),
        "failed": failed,
        "exitCode": 0 if failed == 0 else 1,
    }


def compare_generation_rows(
    current: Sequence[Mapping[str, object]],
    baseline: Sequence[Mapping[str, object]],
) -> tuple[dict[str, object], ...]:
    """Return compact provenance records for package generation-table changes."""
    if len(current) != len(baseline):
        raise AuditError("current and baseline audits contain different package counts")
    changes: list[dict[str, object]] = []
    for current_item, baseline_item in zip(current, baseline):
        identity = (current_item.get("package"), current_item.get("extension"))
        if identity != (
            baseline_item.get("package"),
            baseline_item.get("extension"),
        ):
            raise AuditError("current and baseline audit package order differs")
        current_installed = current_item.get("installed")
        baseline_installed = baseline_item.get("installed")
        if not isinstance(current_installed, Mapping) or not isinstance(
            baseline_installed, Mapping
        ):
            raise AuditError(
                f"cannot compare generation rows for unresolved package "
                f"{identity[0]}.{identity[1]}"
            )
        current_rows = current_installed.get("generations")
        baseline_rows = baseline_installed.get("generations")
        if current_rows == baseline_rows:
            continue
        changes.append(
            {
                "record": "generation-row-drift",
                "package": identity[0],
                "extension": identity[1],
                "baselinePath": baseline_item.get("path"),
                "installedPath": current_item.get("path"),
                "baselineGenerations": baseline_rows,
                "installedGenerations": current_rows,
            }
        )
    return tuple(changes)


def _expected_keys(identities: Sequence[str]) -> frozenset[tuple[str, str]]:
    keys: set[tuple[str, str]] = set()
    for identity in identities:
        name, separator, extension = identity.rpartition(".")
        if not separator or not name or extension.casefold() not in SUPPORTED_EXTENSIONS:
            raise AuditError(
                f"invalid expected package identity {identity!r}; use Name.u, Name.upk or Name.roe"
            )
        key = name.casefold(), extension.casefold()
        if key in keys:
            raise AuditError(f"duplicate expected package identity {identity!r}")
        keys.add(key)
    if not keys:
        raise AuditError("candidate generation requires at least one --expect-guid-package")
    return frozenset(keys)


def build_guid_candidate(
    source: bytes,
    captured: Sequence[UsesRecord],
    results: Sequence[Mapping[str, object]],
    expected_identities: Sequence[str],
) -> tuple[bytes, tuple[dict[str, object], ...]]:
    """Patch only explicitly expected GUID fields and verify the rebuilt stream."""
    if len(captured) != len(results):
        raise AuditError("capture and audit result counts differ")
    expected = _expected_keys(expected_identities)
    actual: set[tuple[str, str]] = set()
    candidate = bytearray(source)
    replacements: list[dict[str, object]] = []

    for record, result in zip(captured, results):
        if (result.get("package"), result.get("extension")) != (
            record.name,
            record.extension,
        ):
            raise AuditError("capture and audit result package order differs")
        status = result.get("status")
        if status == "matched":
            continue
        if status != "drift":
            raise AuditError(
                f"candidate blocked by {status} package {record.name}.{record.extension}"
            )
        drift_fields = result.get("driftFields")
        if drift_fields != ["guid"]:
            raise AuditError(
                f"candidate supports GUID-only drift, got {drift_fields!r} for "
                f"{record.name}.{record.extension}"
            )
        installed = result.get("installed")
        if not isinstance(installed, Mapping):
            raise AuditError(f"missing installed identity for {record.name}.{record.extension}")
        raw_guid = installed.get("rawGuid")
        if not isinstance(raw_guid, str):
            raise AuditError(f"missing installed GUID for {record.name}.{record.extension}")
        try:
            installed_guid = bytes.fromhex(raw_guid)
        except ValueError as exc:
            raise AuditError(
                f"invalid installed GUID for {record.name}.{record.extension}"
            ) from exc
        if len(installed_guid) != 16 or installed_guid == bytes(16):
            raise AuditError(f"invalid installed GUID for {record.name}.{record.extension}")
        offset = record.stream_guid_offset
        if source[offset : offset + 16] != record.guid:
            raise AuditError(
                f"source GUID offset verification failed for {record.name}.{record.extension}"
            )
        candidate[offset : offset + 16] = installed_guid
        actual.add(record.key)
        replacements.append(
            {
                "package": record.name,
                "extension": record.extension,
                "streamOffset": offset,
                "capturedGuid": display_guid(record.guid),
                "installedGuid": display_guid(installed_guid),
                "rawCapturedGuid": record.guid.hex().upper(),
                "rawInstalledGuid": installed_guid.hex().upper(),
            }
        )

    if actual != expected:
        missing = sorted(f"{name}.{extension}" for name, extension in expected - actual)
        unexpected = sorted(f"{name}.{extension}" for name, extension in actual - expected)
        raise AuditError(
            f"GUID drift set differs from expectation; missing={missing}, unexpected={unexpected}"
        )

    rebuilt = bytes(candidate)
    verified = parse_bootstrap(rebuilt)
    if len(verified) != len(captured):
        raise AuditError("candidate package count changed during GUID replacement")
    replacement_by_key = {
        (item["package"].casefold(), item["extension"].casefold()): item
        for item in replacements
    }
    for original, updated in zip(captured, verified):
        if (
            original.key != updated.key
            or original.package_flags != updated.package_flags
            or original.generation != updated.generation
            or original.outer_record != updated.outer_record
            or original.payload_offset != updated.payload_offset
        ):
            raise AuditError("candidate changed non-GUID NMT_Uses metadata")
        replacement = replacement_by_key.get(original.key)
        expected_guid = (
            bytes.fromhex(str(replacement["rawInstalledGuid"]))
            if replacement is not None
            else original.guid
        )
        if updated.guid != expected_guid:
            raise AuditError(
                f"candidate GUID verification failed for {original.name}.{original.extension}"
            )
    return rebuilt, tuple(replacements)


def _json(value: Mapping[str, object]) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"))


def _is_within(path: Path, directory: Path) -> bool:
    try:
        path.relative_to(directory)
        return True
    except ValueError:
        return False


def _paths_alias(first: Path, second: Path) -> bool:
    first_resolved = first.resolve(strict=False)
    second_resolved = second.resolve(strict=False)
    if first_resolved == second_resolved:
        return True
    if not first_resolved.exists() or not second_resolved.exists():
        return False
    try:
        return os.path.samefile(first_resolved, second_resolved)
    except OSError as exc:
        raise AuditError(
            f"could not verify whether output paths alias: {first_resolved}, "
            f"{second_resolved}: {exc}"
        ) from exc


def write_jsonl(
    output: Path,
    results: Sequence[Mapping[str, object]],
    summary: Mapping[str, object],
    bootstrap: Path,
    install_root: Path,
    overwrite: bool,
    extra_records: Sequence[Mapping[str, object]] = (),
) -> None:
    resolved = output.resolve(strict=False)
    if _paths_alias(resolved, bootstrap):
        raise AuditError("refusing to overwrite the bootstrap input")
    if _is_within(resolved, install_root.resolve(strict=True)):
        raise AuditError("refusing to write audit output inside the game install")
    resolved.parent.mkdir(parents=True, exist_ok=True)
    mode = "w" if overwrite else "x"
    try:
        with resolved.open(mode, encoding="utf-8", newline="\n") as stream:
            for item in results:
                stream.write(_json(item) + "\n")
            for item in extra_records:
                stream.write(_json(item) + "\n")
            stream.write(_json(summary) + "\n")
    except OSError as exc:
        raise AuditError(f"could not write JSONL output {resolved}: {exc}") from exc


def write_candidate(
    output: Path,
    data: bytes,
    bootstrap: Path,
    install_root: Path,
    overwrite: bool,
) -> Path:
    resolved = output.resolve(strict=False)
    if _paths_alias(resolved, bootstrap):
        raise AuditError("refusing to overwrite the canonical bootstrap input")
    if _is_within(resolved, install_root.resolve(strict=True)):
        raise AuditError("refusing to write a candidate inside the game install")
    resolved.parent.mkdir(parents=True, exist_ok=True)
    mode = "wb" if overwrite else "xb"
    try:
        with resolved.open(mode) as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        written = resolved.read_bytes()
    except OSError as exc:
        raise AuditError(f"could not write candidate bootstrap {resolved}: {exc}") from exc
    if written != data:
        raise AuditError(f"candidate bootstrap readback verification failed: {resolved}")
    return resolved


def _parser(repo_root: Path) -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--bootstrap",
        type=Path,
        default=repo_root / "data" / "replication_bootstrap.bin",
    )
    parser.add_argument("--install-root", type=Path, default=DEFAULT_INSTALL_ROOT)
    parser.add_argument(
        "--baseline-root",
        type=Path,
        help="optional stale/capture-matching package root for generation-row provenance",
    )
    parser.add_argument("--output", type=Path)
    parser.add_argument(
        "--candidate-bootstrap",
        type=Path,
        help="write an experimental GUID-only candidate; never overwrites the source",
    )
    parser.add_argument(
        "--expect-guid-package",
        action="append",
        default=[],
        metavar="NAME.EXT",
        help="exact GUID-drift identity expected in candidate mode; repeat per package",
    )
    parser.add_argument("--overwrite", action="store_true")
    parser.add_argument(
        "--show-issues",
        type=int,
        default=12,
        help="maximum non-matching package lines printed before the JSON summary",
    )
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    repo_root = Path(__file__).resolve().parent.parent
    args = _parser(repo_root).parse_args(argv)
    try:
        if args.show_issues < 0 or args.show_issues > 100:
            raise AuditError("--show-issues must be between 0 and 100")
        bootstrap = args.bootstrap.resolve(strict=True)
        install_root = args.install_root.resolve(strict=True)
        data = bootstrap.read_bytes()
        captured = parse_bootstrap(data)
        installed, scan_errors = index_installed_packages(install_root)
        results = audit_records(captured, installed)
        summary = summarize(results, scan_errors)
        extra_records: list[Mapping[str, object]] = []

        if args.expect_guid_package and args.candidate_bootstrap is None:
            raise AuditError(
                "--expect-guid-package is only valid with --candidate-bootstrap"
            )

        generation_changes: tuple[dict[str, object], ...] = ()
        if args.baseline_root is not None:
            baseline_root = args.baseline_root.resolve(strict=True)
            baseline_index, baseline_scan_errors = index_installed_packages(
                baseline_root
            )
            baseline_results = audit_records(captured, baseline_index)
            baseline_summary = summarize(baseline_results, baseline_scan_errors)
            if baseline_summary["exitCode"] != 0:
                raise AuditError(
                    "baseline package tree does not exactly match the captured PackageMap: "
                    + _json(baseline_summary)
                )
            generation_changes = compare_generation_rows(results, baseline_results)
            extra_records.extend(generation_changes)
            summary["baselineAudit"] = {
                **baseline_summary,
                "root": str(baseline_root),
            }
            summary["generationRowDrifts"] = len(generation_changes)

        if args.candidate_bootstrap is not None:
            if args.baseline_root is None:
                raise AuditError(
                    "experimental candidate generation requires --baseline-root provenance"
                )
            if scan_errors:
                raise AuditError("candidate generation blocked by package scan errors")
            candidate_data, replacements = build_guid_candidate(
                data,
                captured,
                results,
                args.expect_guid_package,
            )
            candidate_records = parse_bootstrap(candidate_data)
            candidate_results = audit_records(candidate_records, installed)
            candidate_validation = summarize(candidate_results)
            if candidate_validation["exitCode"] != 0:
                raise AuditError(
                    "candidate does not exactly match installed package identities: "
                    + _json(candidate_validation)
                )
            if args.output is not None and _paths_alias(
                args.candidate_bootstrap, args.output
            ):
                raise AuditError("candidate bootstrap and JSONL report paths must differ")
            candidate_path = write_candidate(
                args.candidate_bootstrap,
                candidate_data,
                bootstrap,
                install_root,
                args.overwrite,
            )
            extra_records.extend(
                {"record": "guid-replacement", **replacement}
                for replacement in replacements
            )
            summary["candidate"] = {
                "status": "experimental-guid-only-rebase",
                "path": str(candidate_path),
                "bytes": len(candidate_data),
                "sha256": hashlib.sha256(candidate_data).hexdigest(),
                "sourceBytes": len(data),
                "sourceSha256": hashlib.sha256(data).hexdigest(),
                "guidReplacements": len(replacements),
                "validation": candidate_validation,
                "warning": (
                    "GUID identity validation passes, but generation-row drift means "
                    "downstream ObjectBase indices may still be stale."
                    if generation_changes
                    else "GUID identity validation passes; no generation-row drift detected."
                ),
            }

        issues = [item for item in results if item["status"] != "matched"]
        for item in issues[: args.show_issues]:
            detail = ",".join(item.get("driftFields", [])) or item["status"]
            print(
                f"{str(item['status']).upper():10} "
                f"{item['package']}.{item['extension']} {detail}"
            )
        if len(issues) > args.show_issues:
            print(f"... {len(issues) - args.show_issues} additional issue(s)")
        for error in scan_errors[:3]:
            print(f"SCAN_ERROR {error}")
        if len(scan_errors) > 3:
            print(f"... {len(scan_errors) - 3} additional scan error(s)")
        for change in generation_changes:
            baseline_rows = change["baselineGenerations"]
            installed_rows = change["installedGenerations"]
            fields: list[str] = []
            for index, (baseline_row, installed_row) in enumerate(
                zip(baseline_rows, installed_rows)
            ):
                for field in ("exportCount", "nameCount", "netObjectCount"):
                    before = baseline_row[field]
                    after = installed_row[field]
                    if before != after:
                        fields.append(
                            f"generation[{index}].{field}={before}->{after}"
                        )
            print(
                f"GENERATION_ROW_DRIFT {change['package']}.{change['extension']} "
                + ",".join(fields)
            )
        if args.output is not None:
            write_jsonl(
                args.output,
                results,
                summary,
                bootstrap,
                install_root,
                args.overwrite,
                extra_records,
            )
        print(_json(summary))
        return int(summary["exitCode"])
    except (AuditError, OSError) as exc:
        print(_json({
            "record": "summary",
            "fatal": True,
            "error": str(exc),
            "exitCode": 2,
        }))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
