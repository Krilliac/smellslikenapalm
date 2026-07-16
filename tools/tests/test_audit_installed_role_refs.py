from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from tools import audit_installed_role_refs as audit


def _extractor_records() -> list[dict[str, object]]:
    header: dict[str, object] = {
        "record": "header",
        "mode": "role-exports",
        "inputBytes": audit.EXPECTED_BYTES,
        "inputSha256": audit.EXPECTED_SHA256,
        "sha256MatchedExpectation": True,
        "sha256StableAcrossTableRead": True,
        "packageGuid": audit.EXPECTED_GUID,
        "packageName": "ROGame",
        "uelibAssemblyVersion": audit.EXPECTED_UELIB_ASSEMBLY_VERSION,
        "uelibProductVersion": audit.EXPECTED_UELIB_PRODUCT_VERSION,
        "packageFlags": audit.EXPECTED_PACKAGE_FLAGS,
        "generationCount": audit.EXPECTED_GENERATIONS,
        "exports": audit.EXPECTED_EXPORTS,
        "names": audit.EXPECTED_NAMES,
        "finalGenerationExports": audit.EXPECTED_EXPORTS,
        "finalGenerationNames": audit.EXPECTED_NAMES,
        "finalGenerationNetObjects": audit.EXPECTED_NET_OBJECTS,
    }
    records = [header]
    for role in audit.EFFECTIVE_ROLES:
        linker_index, static_reference = audit.EXPECTED_EFFECTIVE_UCLASS[
            role.effective_class
        ]
        records.append(
            {
                "record": "roleExport",
                "schemaVersion": 1,
                "roleClass": role.effective_class,
                "uclassLinkerIndex": linker_index,
                "cdoLinkerIndex": linker_index + 1,
                "objectBase": audit.OBJECT_BASE,
                "uclassStaticReference": static_reference,
                "cdoStaticReference": audit.OBJECT_BASE + linker_index + 1,
                "referenceDerivation": "PackageMap ObjectBase + linker export index",
                "derivedOnly": True,
                "authorizedByExtractor": False,
            }
        )
    while len(records) - 1 < audit.EXPECTED_ROLE_PAIRS:
        ordinal = len(records) - 1
        linker_index = 50_000 + ordinal * 2
        records.append(
            {
                "record": "roleExport",
                "schemaVersion": 1,
                "roleClass": f"RORoleInfoSynthetic{ordinal}",
                "uclassLinkerIndex": linker_index,
                "cdoLinkerIndex": linker_index + 1,
                "objectBase": audit.OBJECT_BASE,
                "uclassStaticReference": audit.OBJECT_BASE + linker_index,
                "cdoStaticReference": audit.OBJECT_BASE + linker_index + 1,
                "referenceDerivation": "PackageMap ObjectBase + linker export index",
                "derivedOnly": True,
                "authorizedByExtractor": False,
            }
        )
    records.append(
        {
            "record": "summary",
            "mode": "role-exports",
            "pairedRoles": audit.EXPECTED_ROLE_PAIRS,
            "uclassExports": audit.EXPECTED_ROLE_PAIRS,
            "cdoExports": audit.EXPECTED_ROLE_PAIRS,
            "pairsValidatedExactly": True,
            "tableOnly": True,
            "objectBaseSupplied": True,
            "runtimeRolesAuthorized": False,
        }
    )
    return records


def _map_role_records() -> list[dict[str, object]]:
    records: list[dict[str, object]] = [
        {
            "record": "header",
            "mode": "role-info",
            "inputBytes": audit.EXPECTED_MAP_BYTES,
            "packageGuid": audit.EXPECTED_MAP_GUID,
            "uelibAssemblyVersion": audit.EXPECTED_UELIB_ASSEMBLY_VERSION,
            "uelibProductVersion": audit.EXPECTED_UELIB_PRODUCT_VERSION,
        }
    ]
    for role in audit.EFFECTIVE_ROLES:
        records.append(
            {
                "record": "role",
                "team": role.team,
                "sourceProperty": "NorthernRoles"
                if role.team == "north"
                else "SouthernRoles",
                "ordinal": role.ordinal,
                "roleInfoClassPackageIndex": role.map_package_index,
                "roleInfoClassPath": f"Class'ROGame.{role.base_class}'",
                "count": role.count,
                "reverseCount": role.reverse_count,
                "serializedBytes": 102,
            }
        )
    records.append(
        {
            "record": "summary",
            "mode": "role-info",
            "mapInfoExportIndex": audit.EXPECTED_MAP_INFO_EXPORT,
            "mapInfoSerialOffset": audit.EXPECTED_MAP_INFO_OFFSET,
            "mapInfoSerialSize": audit.EXPECTED_MAP_INFO_SIZE,
            "emittedRoles": len(audit.EFFECTIVE_ROLES),
            "roleArraysConsumedExactly": True,
        }
    )
    return records


def _map_actor_records() -> list[dict[str, object]]:
    properties = [
        {"name": "NorthernForce", "value": "ENorthernForces.NFOR_NLF"},
        {"name": "DefendingTeam", "value": "EDefendingTeam.DT_North"},
        {"name": "DefendingTeam16", "value": "EDefendingTeam.DT_North"},
        {"name": "DefendingTeam32", "value": "EDefendingTeam.DT_North"},
        {"name": "DefendingTeam64", "value": "EDefendingTeam.DT_North"},
        {"name": "bIgnoreReverseCountNorth", "value": "false"},
        {"name": "bIgnoreReverseCountSouth", "value": "false"},
    ]
    return [
        {
            "record": "header",
            "mode": "actors",
            "inputBytes": audit.EXPECTED_MAP_BYTES,
            "packageGuid": audit.EXPECTED_MAP_GUID,
            "uelibAssemblyVersion": audit.EXPECTED_UELIB_ASSEMBLY_VERSION,
            "uelibProductVersion": audit.EXPECTED_UELIB_PRODUCT_VERSION,
        },
        {
            "record": "actor",
            "class": "ROMapInfo",
            "exportIndex": audit.EXPECTED_MAP_INFO_EXPORT,
            "serialOffset": audit.EXPECTED_MAP_INFO_OFFSET,
            "serialSize": audit.EXPECTED_MAP_INFO_SIZE,
            "propertiesTruncated": False,
            "properties": properties,
        },
        {"record": "summary", "mode": "actors", "emittedActors": 1, "errors": 0},
    ]


def _build_report(
    extractor_records: list[dict[str, object]] | None = None,
    map_role_records: list[dict[str, object]] | None = None,
    map_actor_records: list[dict[str, object]] | None = None,
) -> tuple[dict[str, object], ...]:
    return audit.build_report(
        _extractor_records() if extractor_records is None else extractor_records,
        _map_role_records() if map_role_records is None else map_role_records,
        _map_actor_records() if map_actor_records is None else map_actor_records,
        audit._source_manifest_digest(),
    )


class ReportTests(unittest.TestCase):
    def test_builds_exact_fail_closed_cuchi_role_report(self) -> None:
        report = _build_report()

        roles = [record for record in report if record["record"] == "role"]
        self.assertEqual(len(roles), 14)
        self.assertEqual(roles[0]["effectiveClass"], "RORoleInfoNorthernGuerilla")
        self.assertEqual(roles[0]["uclassStaticReference"], 87399)
        self.assertEqual(roles[4]["classIndex"], 6)
        self.assertEqual(roles[5]["classIndex"], 4)
        self.assertEqual(roles[11]["classIndex"], 5)
        self.assertEqual(roles[12]["classIndex"], 4)
        self.assertTrue(all(not record["authorizedByReport"] for record in roles))
        self.assertEqual(sum(record["runtimeSupported"] for record in roles), 2)
        self.assertEqual(roles[0]["runtimeBlockers"], [])
        self.assertIn("role-loadout-owning-pawn-graph", roles[2]["runtimeBlockers"])
        self.assertEqual(
            report[0]["uelibDependencies"],
            [
                {
                    "name": audit.EXPECTED_UNSAFE_NAME,
                    "bytes": audit.EXPECTED_UNSAFE_BYTES,
                    "sha256": audit.EXPECTED_UNSAFE_SHA256,
                    "assemblyVersion": audit.EXPECTED_UNSAFE_ASSEMBLY_VERSION,
                }
            ],
        )
        self.assertEqual(
            report[0]["extractorWrapperSha256"], audit.EXPECTED_WRAPPER_SHA256
        )
        self.assertEqual(
            report[0]["extractorSourceSha256"],
            audit.EXPECTED_EXTRACTOR_SOURCE_SHA256,
        )
        self.assertEqual(
            report[0]["extractorExecutableSha256"],
            audit.EXPECTED_EXTRACTOR_EXECUTABLE_SHA256,
        )
        self.assertFalse(report[-1]["reportAuthorizesRuntimeRoles"])

    def test_rejects_artifact_and_reference_drift(self) -> None:
        cases = {}
        wrong_hash = _extractor_records()
        wrong_hash[0] = {**wrong_hash[0], "inputSha256": "F" * 64}
        cases["hash"] = wrong_hash

        bad_known_ref = _extractor_records()
        bad_known_ref[1] = {
            **bad_known_ref[1],
            "uclassStaticReference": 87398,
        }
        cases["known class zero"] = bad_known_ref

        bad_arithmetic = _extractor_records()
        bad_arithmetic[2] = {
            **bad_arithmetic[2],
            "uclassStaticReference": 1,
        }
        cases["arithmetic"] = bad_arithmetic

        missing = _extractor_records()
        del missing[3]
        cases["missing role"] = missing

        for label, records in cases.items():
            with self.subTest(label=label), self.assertRaises(audit.AuditError):
                _build_report(extractor_records=records)

    def test_rejects_duplicate_exports_and_summary_count_drift(self) -> None:
        duplicate = _extractor_records()
        duplicate.insert(2, dict(duplicate[1]))
        with self.assertRaises(audit.AuditError):
            _build_report(extractor_records=duplicate)

        count_drift = _extractor_records()
        count_drift[-1] = {**count_drift[-1], "pairedRoles": 999}
        with self.assertRaises(audit.AuditError):
            _build_report(extractor_records=count_drift)

    def test_rejects_extractor_schema_and_cdo_contract_drift(self) -> None:
        cases = []
        missing_schema = _extractor_records()
        missing_schema[1] = dict(missing_schema[1])
        del missing_schema[1]["schemaVersion"]
        cases.append(missing_schema)
        bad_cdo = _extractor_records()
        bad_cdo[1] = {**bad_cdo[1], "cdoStaticReference": 1}
        cases.append(bad_cdo)
        bad_derivation = _extractor_records()
        bad_derivation[1] = {**bad_derivation[1], "referenceDerivation": "guessed"}
        cases.append(bad_derivation)
        for records in cases:
            with self.assertRaises(audit.AuditError):
                _build_report(extractor_records=records)

    def test_rejects_cooked_map_and_force_drift(self) -> None:
        roles = _map_role_records()
        roles[1] = {**roles[1], "count": 254}
        with self.assertRaises(audit.AuditError):
            _build_report(map_role_records=roles)

        actors = _map_actor_records()
        actors[1] = dict(actors[1])
        actors[1]["properties"] = [
            {**value, "value": "ENorthernForces.NFOR_NVA"}
            if value["name"] == "NorthernForce"
            else value
            for value in actors[1]["properties"]
        ]
        with self.assertRaises(audit.AuditError):
            _build_report(map_actor_records=actors)

    def test_source_alt_table_parser_preserves_class_index_swaps(self) -> None:
        source = """
        NorthAltRoleClasses[4]=(AltRoleClassByArmy=(Class'ROGame.NorthSapper',Class'ROGame.NorthSapperNLF'))
        NorthAltRoleClasses[6]=(AltRoleClassByArmy=(Class'ROGame.NorthRPG',Class'ROGame.NorthRPGNLF'))
        SouthAltRoleClasses[4]=(AltRoleClassByArmy=(Class'ROGame.SouthEngineer'))
        SouthAltRoleClasses[5]=(AltRoleClassByArmy=(Class'ROGame.SouthGrenadier'))
        """
        parsed = audit._parse_alt_role_tables(source)
        self.assertEqual(parsed[("north", 4)][1], "NorthSapperNLF")
        self.assertEqual(parsed[("north", 6)][1], "NorthRPGNLF")
        self.assertEqual(parsed[("south", 4)][0], "SouthEngineer")
        self.assertEqual(parsed[("south", 5)][0], "SouthGrenadier")

    def test_root_class_index_contract_requires_zero_inherited_default(self) -> None:
        audit._validate_root_class_index_contract(
            "class RORoleInfo extends Object;\nvar byte ClassIndex;\ndefaultproperties\n{\n}\n"
        )
        for source in (
            "class RORoleInfo extends Object;\ndefaultproperties\n{\n}\n",
            "class RORoleInfo extends Object;\nvar int ClassIndex;\ndefaultproperties\n{\n}\n",
            "class RORoleInfo extends Object;\nvar byte ClassIndex;\ndefaultproperties\n{\n ClassIndex=1\n}\n",
        ):
            with self.subTest(source=source), self.assertRaises(audit.AuditError):
                audit._validate_root_class_index_contract(source)

    def test_checked_in_report_matches_the_exact_contract(self) -> None:
        repo_root = Path(__file__).resolve().parents[2]
        report_path = repo_root / "data" / "installed_cuchi_role_refs.jsonl"
        self.assertEqual(audit.load_jsonl(report_path), _build_report())
        self.assertNotIn(b"\r\n", report_path.read_bytes())

        wrapper = repo_root / "tools" / "extract_cooked_map_metadata.ps1"
        source = repo_root / "tools" / audit.EXPECTED_EXTRACTOR_SOURCE_NAME
        audit.validate_file_identity(
            wrapper,
            audit.EXPECTED_WRAPPER_BYTES,
            audit.EXPECTED_WRAPPER_SHA256,
            "checked wrapper",
        )
        audit.validate_file_identity(
            source,
            audit.EXPECTED_EXTRACTOR_SOURCE_BYTES,
            audit.EXPECTED_EXTRACTOR_SOURCE_SHA256,
            "checked extractor source",
        )

    def test_every_audit_extraction_forces_a_fresh_content_build(self) -> None:
        with mock.patch.object(audit, "_run_extractor_command") as runner:
            audit.run_role_export_extractor(
                Path("ROGame.u"), Path("wrapper.ps1"), Path("UELib.dll"), Path("roles.jsonl")
            )
            audit.run_map_extractors(
                Path("CuChi.roe"),
                Path("wrapper.ps1"),
                Path("UELib.dll"),
                Path("map-roles.jsonl"),
                Path("map-actor.jsonl"),
            )
        self.assertEqual(runner.call_count, 3)
        for invocation in runner.call_args_list:
            command = invocation.args[0]
            self.assertIn("-ForceRebuild", command)
            hash_index = command.index("-ExpectedExecutableSha256")
            self.assertEqual(
                command[hash_index + 1], audit.EXPECTED_EXTRACTOR_EXECUTABLE_SHA256
            )


class JsonlTests(unittest.TestCase):
    def test_load_and_write_jsonl_are_strict(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = root / "report.jsonl"
            records = ({"record": "header"}, {"record": "summary"})
            audit.write_jsonl(records, output, False)
            loaded = audit.load_jsonl(output)
            self.assertEqual(loaded, records)
            self.assertEqual(
                json.loads(output.read_text(encoding="utf-8").splitlines()[0]),
                records[0],
            )
            with self.assertRaises(audit.AuditError):
                audit.write_jsonl(records, output, False)

            malformed = root / "malformed.jsonl"
            malformed.write_text('{"record":"header"}\n\n', encoding="utf-8")
            with self.assertRaises(audit.AuditError):
                audit.load_jsonl(malformed)


if __name__ == "__main__":
    unittest.main()
