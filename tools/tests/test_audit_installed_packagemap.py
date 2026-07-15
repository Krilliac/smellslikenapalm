from __future__ import annotations

import contextlib
import hashlib
import io
import json
import os
import struct
import tempfile
import unittest
from pathlib import Path

from tools import audit_installed_packagemap as audit


GUID_A = bytes.fromhex("00112233445566778899AABBCCDDEEFF")
GUID_B = bytes.fromhex("FFEEDDCCBBAA99887766554433221100")


def _ansi(value: str) -> bytes:
    encoded = value.encode("ascii") + b"\0"
    return struct.pack("<i", len(encoded)) + encoded


def _utf16(value: str) -> bytes:
    encoded = value.encode("utf-16-le") + b"\0\0"
    return struct.pack("<i", -(len(value) + 1)) + encoded


def _uses(
    name: str,
    extension: str,
    guid: bytes = GUID_A,
    flags: int = 0x20024001,
    generation: int = 2,
) -> bytes:
    return b"".join(
        (
            bytes((audit.NMT_USES,)),
            guid,
            _ansi(name),
            _ansi(extension),
            struct.pack("<II", flags, generation),
            _ansi("None"),
            bytes(8),
        )
    )


def _outer(payload: bytes) -> bytes:
    return struct.pack("<I", len(payload)) + payload


def _package_bytes(
    guid: bytes = GUID_A,
    flags: int = 0x20024001,
    generations: int = 2,
    *,
    package_version: bytes = audit.SUPPORTED_PACKAGE_VERSION,
    tag: int = audit.UE3_PACKAGE_TAG,
    header_size: int | None = None,
    generation_rows: tuple[tuple[int, int, int], ...] | None = None,
) -> bytes:
    if generation_rows is not None:
        generations = len(generation_rows)
    else:
        generation_rows = tuple((0, 0, 0) for _ in range(generations))
    data = bytearray()
    data.extend(struct.pack("<I", tag))
    data.extend(package_version)
    data.extend(struct.pack("<I", 0))
    data.extend(_ansi("None"))
    data.extend(struct.pack("<I", flags))
    data.extend(bytes(11 * 4))
    data.extend(guid)
    data.extend(struct.pack("<I", generations))
    for row in generation_rows:
        data.extend(struct.pack("<III", *row))
    data.extend(struct.pack("<I", 7258))
    struct.pack_into("<I", data, 8, len(data) if header_size is None else header_size)
    return bytes(data)


def _record(
    name: str,
    extension: str,
    guid: bytes = GUID_A,
    flags: int = 0x20024001,
    generation: int = 2,
) -> audit.UsesRecord:
    return audit.UsesRecord(name, extension, guid, flags, generation, 0, 0, 0)


class BootstrapParserTests(unittest.TestCase):
    def test_parses_contiguous_uses_and_ignores_other_message_payload(self) -> None:
        stream = _outer(
            _uses("Core", "u") + _uses("VNSU-HueCity", "roe", GUID_B) + b"\x42tail"
        ) + _outer(b"\x08not-a-package-record")

        records = audit.parse_bootstrap(stream)

        self.assertEqual([record.name for record in records], ["Core", "VNSU-HueCity"])
        self.assertEqual(records[1].guid, GUID_B)
        self.assertEqual(
            audit.display_guid(GUID_A), "3322110077665544BBAA9988FFEEDDCC"
        )

    def test_rejects_structurally_unsafe_capture_streams(self) -> None:
        cases = {
            "empty": b"",
            "zero outer length": struct.pack("<I", 0),
            "truncated outer record": struct.pack("<I", 9) + b"short",
            "no uses": _outer(b"\x08other"),
            "zero guid": _outer(_uses("Core", "u", bytes(16))),
            "duplicate identity": _outer(_uses("Core", "u") + _uses("core", "U")),
            "nonzero uses trailer": _outer(_uses("Core", "u")[:-1] + b"\x01"),
        }
        for label, stream in cases.items():
            with self.subTest(label=label), self.assertRaises(audit.AuditError):
                audit.parse_bootstrap(stream)

    def test_fstring_accepts_utf16_and_rejects_embedded_null(self) -> None:
        value, end = audit._fstring(_utf16("Hue"), 0, "test")
        self.assertEqual(value, "Hue")
        self.assertEqual(end, len(_utf16("Hue")))
        malformed = struct.pack("<i", -4) + "A\0B".encode("utf-16-le") + b"\0\0"
        with self.assertRaises(audit.AuditError):
            audit._fstring(malformed, 0, "test")


class InstalledPackageTests(unittest.TestCase):
    def test_reads_summary_identity_from_a_bounded_header(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary, "VNSU-HueCity.roe")
            path.write_bytes(_package_bytes(GUID_B, generations=3) + bytes(9000))

            summary = audit.parse_package_summary(path)

        self.assertEqual(summary.guid, GUID_B)
        self.assertEqual(summary.package_flags, 0x20024001)
        self.assertEqual(summary.generation_count, 3)
        self.assertEqual(summary.generations, ((0, 0, 0),) * 3)
        self.assertEqual(summary.engine_version, 7258)

    def test_rejects_unsupported_or_malformed_package_headers(self) -> None:
        fixtures = {
            "bad tag": _package_bytes(tag=0),
            "unsupported version": _package_bytes(package_version=b"\x01\x02\x03\x04"),
            "zero guid": _package_bytes(guid=bytes(16)),
            "header past file": _package_bytes(header_size=999999),
            "generation table past header": _package_bytes(generations=2, header_size=80),
        }
        with tempfile.TemporaryDirectory() as temporary:
            for label, contents in fixtures.items():
                path = Path(temporary, f"{label}.u")
                path.write_bytes(contents)
                with self.subTest(label=label), self.assertRaises(audit.AuditError):
                    audit.parse_package_summary(path)


class AuditTests(unittest.TestCase):
    def test_classifies_match_drift_unresolved_ambiguous_and_error(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            exact = root / "Exact.u"
            drift = root / "Drift.upk"
            duplicate_a = root / "a" / "Duplicate.roe"
            duplicate_b = root / "b" / "Duplicate.roe"
            broken = root / "Broken.u"
            for path in (duplicate_a, duplicate_b):
                path.parent.mkdir()
                path.write_bytes(_package_bytes())
            exact.write_bytes(_package_bytes())
            drift.write_bytes(_package_bytes(GUID_B, flags=0x10, generations=4))
            broken.write_bytes(b"not a package")
            installed, scan_errors = audit.index_installed_packages(root)

            results = audit.audit_records(
                (
                    _record("Exact", "u"),
                    _record("Drift", "upk"),
                    _record("Missing", "u"),
                    _record("Duplicate", "roe"),
                    _record("Broken", "u"),
                ),
                installed,
            )

        self.assertEqual(scan_errors, ())
        self.assertEqual(
            [result["status"] for result in results],
            ["matched", "drift", "unresolved", "ambiguous", "error"],
        )
        self.assertEqual(
            results[1]["driftFields"], ["guid", "packageFlags", "generation"]
        )
        summary = audit.summarize(results)
        self.assertEqual(summary["failed"], 4)
        self.assertEqual(summary["exitCode"], 1)

    def test_main_writes_jsonl_and_returns_nonzero_for_drift(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            install = root / "install"
            install.mkdir()
            (install / "Core.u").write_bytes(_package_bytes(GUID_B))
            bootstrap = root / "bootstrap.bin"
            bootstrap.write_bytes(_outer(_uses("Core", "u", GUID_A)))
            output = root / "audit.jsonl"
            stdout = io.StringIO()

            with contextlib.redirect_stdout(stdout):
                exit_code = audit.main(
                    (
                        "--bootstrap",
                        str(bootstrap),
                        "--install-root",
                        str(install),
                        "--output",
                        str(output),
                        "--show-issues",
                        "1",
                    )
                )

            lines = [json.loads(line) for line in output.read_text().splitlines()]

        self.assertEqual(exit_code, 1)
        self.assertEqual(lines[0]["status"], "drift")
        self.assertEqual(lines[-1]["exitCode"], 1)
        self.assertIn('"exitCode":1', stdout.getvalue())

    def test_builds_only_the_explicit_guid_candidate(self) -> None:
        source = _outer(_uses("Core", "u", GUID_A))
        captured = audit.parse_bootstrap(source)
        with tempfile.TemporaryDirectory() as temporary:
            package = Path(temporary, "Core.u")
            package.write_bytes(_package_bytes(GUID_B))
            results = audit.audit_records(captured, {("core", "u"): (package,)})

            candidate, replacements = audit.build_guid_candidate(
                source, captured, results, ("Core.u",)
            )

        guid_offset = captured[0].stream_guid_offset
        self.assertEqual(candidate[:guid_offset], source[:guid_offset])
        self.assertEqual(candidate[guid_offset : guid_offset + 16], GUID_B)
        self.assertEqual(candidate[guid_offset + 16 :], source[guid_offset + 16 :])
        self.assertEqual(len(replacements), 1)
        self.assertEqual(audit.parse_bootstrap(candidate)[0].guid, GUID_B)
        with self.assertRaises(audit.AuditError):
            audit.build_guid_candidate(source, captured, results, ("Engine.u",))

    def test_candidate_cli_records_baseline_generation_drift_and_hash(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            install = root / "install"
            baseline = root / "baseline"
            install.mkdir()
            baseline.mkdir()
            old_rows = ((10, 20, 30), (11, 21, 31))
            new_rows = ((10, 20, 30), (11, 21, 35))
            (baseline / "Core.u").write_bytes(
                _package_bytes(GUID_A, generation_rows=old_rows)
            )
            (install / "Core.u").write_bytes(
                _package_bytes(GUID_B, generation_rows=new_rows)
            )
            bootstrap = root / "bootstrap.bin"
            bootstrap.write_bytes(_outer(_uses("Core", "u", GUID_A)))
            candidate = root / "candidate.bin"
            report = root / "report.jsonl"

            with contextlib.redirect_stdout(io.StringIO()):
                exit_code = audit.main(
                    (
                        "--bootstrap",
                        str(bootstrap),
                        "--install-root",
                        str(install),
                        "--baseline-root",
                        str(baseline),
                        "--candidate-bootstrap",
                        str(candidate),
                        "--expect-guid-package",
                        "Core.u",
                        "--output",
                        str(report),
                    )
                )

            candidate_data = candidate.read_bytes()
            report_records = [
                json.loads(line) for line in report.read_text().splitlines()
            ]
            summary = report_records[-1]

        self.assertEqual(exit_code, 1)
        self.assertEqual(audit.parse_bootstrap(candidate_data)[0].guid, GUID_B)
        self.assertEqual(
            summary["candidate"]["sha256"], hashlib.sha256(candidate_data).hexdigest()
        )
        self.assertEqual(summary["candidate"]["validation"]["matched"], 1)
        self.assertEqual(summary["generationRowDrifts"], 1)
        generation_record = next(
            item for item in report_records if item["record"] == "generation-row-drift"
        )
        self.assertEqual(
            generation_record["baselineGenerations"][1]["netObjectCount"], 31
        )
        self.assertEqual(
            generation_record["installedGenerations"][1]["netObjectCount"], 35
        )

    def test_refuses_to_write_output_inside_game_install(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            install = root / "install"
            install.mkdir()
            bootstrap = root / "bootstrap.bin"
            bootstrap.write_bytes(b"capture")
            with self.assertRaises(audit.AuditError):
                audit.write_jsonl(
                    install / "audit.jsonl",
                    (),
                    {"record": "summary"},
                    bootstrap,
                    install,
                    False,
                )

    def test_refuses_hardlink_aliases_to_canonical_bootstrap(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            install = root / "install"
            install.mkdir()
            bootstrap = root / "bootstrap.bin"
            bootstrap.write_bytes(b"canonical capture")
            candidate_alias = root / "candidate.bin"
            report_alias = root / "report.jsonl"
            os.link(bootstrap, candidate_alias)
            os.link(bootstrap, report_alias)

            with self.assertRaises(audit.AuditError):
                audit.write_candidate(
                    candidate_alias, b"replacement", bootstrap, install, True
                )
            with self.assertRaises(audit.AuditError):
                audit.write_jsonl(
                    report_alias,
                    (),
                    {"record": "summary"},
                    bootstrap,
                    install,
                    True,
                )

            self.assertEqual(bootstrap.read_bytes(), b"canonical capture")

    def test_same_candidate_and_report_path_fails_before_write(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            install = root / "install"
            baseline = root / "baseline"
            install.mkdir()
            baseline.mkdir()
            (install / "Core.u").write_bytes(_package_bytes(GUID_B))
            (baseline / "Core.u").write_bytes(_package_bytes(GUID_A))
            bootstrap = root / "bootstrap.bin"
            bootstrap.write_bytes(_outer(_uses("Core", "u", GUID_A)))
            collision = root / "candidate-and-report.bin"

            with contextlib.redirect_stdout(io.StringIO()):
                exit_code = audit.main(
                    (
                        "--bootstrap",
                        str(bootstrap),
                        "--install-root",
                        str(install),
                        "--baseline-root",
                        str(baseline),
                        "--candidate-bootstrap",
                        str(collision),
                        "--expect-guid-package",
                        "Core.u",
                        "--output",
                        str(collision),
                    )
                )

            self.assertEqual(exit_code, 2)
            self.assertFalse(collision.exists())


if __name__ == "__main__":
    unittest.main()
