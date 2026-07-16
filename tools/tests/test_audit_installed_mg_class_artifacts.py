from __future__ import annotations

import contextlib
import copy
import hashlib
import io
import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from tools import audit_installed_mg_class_artifacts as audit


DIRECT_COUNTS = dict(zip(audit.M60_CHAIN, audit.M60_COUNTS, strict=True))
for _path, _count in zip(audit.M61_CHAIN, audit.M61_COUNTS, strict=True):
    if _path in DIRECT_COUNTS:
        assert DIRECT_COUNTS[_path] == _count
    DIRECT_COUNTS[_path] = _count

OVERRIDDEN_NETWORK_FUNCTIONS = {
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


def _network_property(name: str, net_index: int) -> dict[str, object]:
    return {
        "name": name,
        "kind": "property",
        "uobjectNetIndex": net_index,
        "propertyFlags": "0x0000000000000020",
        "functionFlags": None,
        "uelibType": "UELib.Core.UIntProperty",
        "arrayDim": 1,
        "functionSuperPresent": None,
        "specialDeclaration": False,
    }


def _network_function(
    name: str, net_index: int, *, function_super: bool = False
) -> dict[str, object]:
    return {
        "name": name,
        "kind": "function",
        "uobjectNetIndex": net_index,
        "propertyFlags": None,
        "functionFlags": "0x0000000000000040",
        "uelibType": "UELib.Core.UFunction",
        "arrayDim": None,
        "functionSuperPresent": function_super,
        "specialDeclaration": False,
    }


def _members(class_path: str) -> list[dict[str, object]]:
    count = DIRECT_COUNTS[class_path]
    if class_path == "Class'Engine.Actor'":
        net_indices = (
            495,
            2481,
            2482,
            2488,
            2502,
            2515,
            2516,
            2518,
            2520,
            2521,
            2522,
            2528,
            2529,
            2530,
            2546,
            2560,
            2561,
            2563,
            2564,
            2583,
            2598,
            2626,
            2628,
        )
        assert len(net_indices) == 23
        names = [f"ActorMember{index:02d}" for index in range(1, 24)]
        names[4] = "Instigator"
        names[21] = "bHidden"
        members = [
            _network_property(name, net_index)
            for name, net_index in zip(names, net_indices, strict=True)
        ]
        members.append(
            {
                "name": "bNetInitialRotation",
                "kind": "property",
                "uobjectNetIndex": 2495,
                "propertyFlags": "0x0000000000000002",
                "functionFlags": None,
                "uelibType": "UELib.Core.UBoolProperty",
                "arrayDim": 1,
                "functionSuperPresent": None,
                "specialDeclaration": True,
            }
        )
        members = sorted(
            members,
            key=lambda member: (
                member["uobjectNetIndex"], member["kind"], member["name"]
            ),
        )
    elif class_path == "Class'ROGame.ROWeapon'":
        net_indices = [2800 + 4 * index for index in range(1, 65)]
        net_indices.extend((3060, 3062, 3066, 3070))
        names = [f"ROWeaponMember{index:02d}" for index in range(1, 69)]
        names[65] = "bUserConfigApplied"
        members = [
            _network_property(name, net_index)
            for name, net_index in zip(names, net_indices, strict=True)
        ]
        members[0] = _network_function("ServerZoomIn", net_indices[0])
        members[1] = _network_function("ServerZoomInModified", net_indices[1])
    else:
        class_name = class_path.split(".", 1)[1][:-1]
        ordinal = list(audit.SUPER_CLASS_PATHS).index(class_path)
        members = []
        for index in range(1, count + 1):
            name = f"{class_name}Member{index:02d}"
            net_index = 4000 + ordinal * 100 + index
            if class_path == "Class'Engine.Weapon'" and index == count:
                members.append(_network_function(name, net_index))
            else:
                members.append(_network_property(name, net_index))
        if class_path == "Class'Engine.Inventory'":
            members[0] = _network_function(
                "ClientGivenTo", members[0]["uobjectNetIndex"]
            )
        elif class_path == "Class'Engine.Weapon'":
            inherited_names = (
                "ServerStartFire",
                "ServerStopFire",
                "ClientWeaponSet",
                "ServerWarnPawnsAlongFireLine",
                "ClientWeaponThrown",
            )
            members = [
                _network_function(name, member["uobjectNetIndex"])
                for name, member in zip(inherited_names, members, strict=True)
            ]
    members.extend(
        _network_function(name, net_index, function_super=True)
        for name, net_index in OVERRIDDEN_NETWORK_FUNCTIONS.get(class_path, ())
    )
    return sorted(
        members,
        key=lambda member: (
            member["uobjectNetIndex"], member["kind"], member["name"]
        ),
    )


def _class_record(class_path: str) -> dict[str, object]:
    match = audit.CLASS_PATH_RE.fullmatch(class_path)
    assert match is not None
    package, class_name = match.groups()
    ordinal = list(audit.SUPER_CLASS_PATHS).index(class_path)
    uclass_export = 0 if class_path == "Class'Core.Object'" else 100 + ordinal * 20
    uclass_net = 1000 + ordinal * 2
    cdo_net = uclass_net + 1
    if class_path == "Class'ROGameContent.ROWeap_M60_GPMG_Content'":
        uclass_net, cdo_net = 512, 513
    elif class_path == "Class'ROGameContent.ROWeap_M61_Grenade_ContentSingle'":
        uclass_net, cdo_net = 529, 530
    return {
        "record": "class",
        "classPath": class_path,
        "superClassPath": audit.SUPER_CLASS_PATHS[class_path],
        "superTableReference": 0
        if audit.SUPER_CLASS_PATHS[class_path] is None
        else -1,
        "UClass": {
            "exportIndex": uclass_export,
            "uobjectNetIndex": uclass_net,
        },
        "CDO": {
            "path": f"{class_name}'{package}.Default__{class_name}'",
            "exportIndex": uclass_export + 7,
            "uobjectNetIndex": cdo_net,
            "classTableReference": uclass_export + 1,
            "classLinkVerified": True,
            "serialOffset": 4096 + ordinal * 128,
            "serialSize": 64,
            "objectFlags": "0x0000000000000003",
        },
        "declaredNetworkMembers": _members(class_path),
        "directBNetInitialRotationTags": [],
    }


def _raw_records(spec: audit.PackageSpec) -> list[dict[str, object]]:
    header: dict[str, object] = {
        "record": "header",
        "schema": audit.RAW_SCHEMA,
        "mode": "class-artifacts",
        "classPattern": spec.pattern,
        "expectedClassCount": spec.expected_count,
        "artifactBytes": spec.artifact_bytes,
        "artifactSha256": spec.artifact_sha256,
        "extractorAssemblyIdentity": audit.EXPECTED_EXTRACTOR_ASSEMBLY_IDENTITY,
        "extractorBytes": audit.EXPECTED_EXTRACTOR_EXECUTABLE_BYTES,
        "extractorSha256": audit.EXPECTED_EXTRACTOR_EXECUTABLE_SHA256,
        "uelibAssemblyIdentity": audit.EXPECTED_UELIB_ASSEMBLY_IDENTITY,
        "uelibAssemblyVersion": audit.EXPECTED_UELIB_ASSEMBLY_VERSION,
        "uelibProductVersion": audit.EXPECTED_UELIB_PRODUCT_VERSION,
        "uelibBytes": audit.EXPECTED_UELIB_BYTES,
        "uelibSha256": audit.EXPECTED_UELIB_SHA256,
        "unsafeAssemblyIdentity": audit.EXPECTED_UNSAFE_ASSEMBLY_IDENTITY,
        "unsafeBytes": audit.EXPECTED_UNSAFE_BYTES,
        "unsafeSha256": audit.EXPECTED_UNSAFE_SHA256,
        "wireStaticReferencesDerived": False,
        "reportAuthorizesRuntime": False,
    }
    package: dict[str, object] = {
        "record": "package",
        "package": spec.name,
        "artifactBytes": spec.artifact_bytes,
        "artifactSha256": spec.artifact_sha256,
        "rawGuid": spec.raw_guid,
        "packageGuid": spec.package_guid,
        "packageFlags": spec.package_flags,
        "packageVersion": 765,
        "licenseeVersion": 771,
        "engineVersion": 7258,
        "generationCount": len(spec.generations),
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
    classes = [
        _class_record(f"Class'{spec.name}.{name}'") for name in spec.class_names
    ]
    summary: dict[str, object] = {
        "record": "summary",
        "schema": audit.RAW_SCHEMA,
        "mode": "class-artifacts",
        "classesEmitted": spec.expected_count,
        "classLinksVerifiedDirectly": True,
        "wireStaticReferencesDerived": False,
        "reportAuthorizesRuntime": False,
    }
    return [header, package, *classes, summary]


def _raw_packages() -> dict[str, list[dict[str, object]]]:
    return {spec.name: _raw_records(spec) for spec in audit.PACKAGE_SPECS}


def _find_class(
    packages: dict[str, list[dict[str, object]]], class_path: str
) -> dict[str, object]:
    package_name = audit.CLASS_PATH_RE.fullmatch(class_path).group(1)  # type: ignore[union-attr]
    for record in packages[package_name]:
        if record.get("classPath") == class_path:
            return record
    raise AssertionError(class_path)


class BuildReportTests(unittest.TestCase):
    def test_checked_report_matches_the_exact_sanitized_contract(self) -> None:
        repo_root = Path(__file__).resolve().parents[2]
        report_path = repo_root / "data" / "installed_mg_class_artifacts.jsonl"
        payload = report_path.read_bytes()

        self.assertEqual(len(payload), 59_182)
        self.assertEqual(
            hashlib.sha256(payload).hexdigest().upper(),
            "D4F6BB2CCEB3E8B0A544CD23AE57A960662343B3D01EE5E7FA3F99BBE24ED0B0",
        )
        self.assertNotIn(b"\r", payload)
        self.assertTrue(payload.endswith(b"\n"))
        records = audit.load_jsonl(report_path)
        audit.validate_final_report(records)

    def test_builds_complete_exact_handle_tables_and_default_proof(self) -> None:
        report = audit.build_report(_raw_packages())

        self.assertEqual([record["record"] for record in report], [
            "header", "target", "target", "default-proof", "summary"
        ])
        m60, m61 = report[1], report[2]
        self.assertEqual((m60["target"], m60["maxHandle"]), ("M60", 112))
        self.assertEqual(
            (m61["target"], m61["maxHandle"]),
            ("M61-ContentSingle", 101),
        )
        self.assertEqual(
            [row["handle"] for row in m60["handles"]], list(range(112))
        )
        self.assertEqual(
            [row["handle"] for row in m61["handles"]], list(range(101))
        )
        self.assertEqual(
            (m60["handleIndexBase"], m60["maxHandleExclusive"]), (0, 112)
        )
        self.assertEqual(
            (m61["handleIndexBase"], m61["maxHandleExclusive"]), (0, 101)
        )
        for target in (m60, m61):
            spots = {row["handle"]: row for row in target["spotHandles"]}
            self.assertEqual(spots[4]["name"], "Instigator")
            self.assertEqual(spots[21]["name"], "bHidden")
            self.assertEqual(spots[96]["name"], "bUserConfigApplied")
            self.assertEqual(spots[96]["uobjectNetIndex"], 3062)
        self.assertEqual(
            [row["directNetworkMemberCount"] for row in m60["classChain"]],
            list(audit.M60_COUNTS),
        )
        self.assertEqual(
            [row["directNetworkMemberCount"] for row in m61["classChain"]],
            list(audit.M61_COUNTS),
        )
        for target, chain in ((m60, audit.M60_CHAIN), (m61, audit.M61_CHAIN)):
            actual_overrides = {
                row["classPath"]: tuple(
                    (item["name"], item["uobjectNetIndex"])
                    for item in row["overriddenNetworkFunctions"]
                )
                for row in target["classChain"]
                if row["overriddenNetworkFunctions"]
            }
            self.assertEqual(
                actual_overrides,
                {
                    path: OVERRIDDEN_NETWORK_FUNCTIONS[path]
                    for path in chain
                    if path in OVERRIDDEN_NETWORK_FUNCTIONS
                },
            )
        self.assertTrue(
            any(row["kind"] == "function" for row in m60["handles"])
        )
        proof = report[3]
        self.assertEqual(proof["propertyFlags"], "0x0000000000000002")
        self.assertFalse(proof["cpfNet"])
        self.assertFalse(proof["effectiveValue"])
        self.assertTrue(proof["effectiveFalseProven"])
        self.assertEqual(proof["directSerializedOverrideCount"], 0)
        self.assertEqual(report[-1]["handleRows"], 213)
        audit.validate_final_report(report)

    def test_every_record_retains_hard_false_runtime_denials(self) -> None:
        report = audit.build_report(_raw_packages())
        for record in report:
            for field in audit.DENIAL_KEYS:
                self.assertIn(field, record)
                self.assertIs(record[field], False)
        rendered = "\n".join(audit._json(record) for record in report)
        self.assertNotIn("ROWeap_M61_GrenadeSingle", rendered)
        self.assertNotRegex(rendered, r"[A-Za-z]:[\\/]")

    def test_direct_cdo_link_does_not_assume_adjacent_exports(self) -> None:
        packages = _raw_packages()
        actor = _find_class(packages, "Class'Engine.Actor'")
        self.assertNotEqual(
            actor["CDO"]["exportIndex"], actor["UClass"]["exportIndex"] + 1
        )
        audit.build_report(packages)

    def test_rejects_identity_schema_generation_and_scalar_type_drift(self) -> None:
        mutations = (
            ("header identity", lambda data: data["Core"][0].__setitem__(
                "artifactSha256", "0" * 64
            )),
            ("unknown header key", lambda data: data["Core"][0].__setitem__(
                "extra", False
            )),
            ("licensee", lambda data: data["Engine"][1].__setitem__(
                "licenseeVersion", 770
            )),
            ("bool ordinal", lambda data: data["ROGame"][1]["generations"][0].__setitem__(
                "ordinal", True
            )),
            ("bool export", lambda data: _find_class(
                data, "Class'Core.Object'"
            )["UClass"].__setitem__("exportIndex", False)),
        )
        for label, mutate in mutations:
            packages = _raw_packages()
            mutate(packages)
            with self.subTest(label=label), self.assertRaises(audit.AuditError):
                audit.build_report(packages)

    def test_rejects_chain_class_count_order_and_member_identity_drift(self) -> None:
        def wrong_super(data: dict[str, list[dict[str, object]]]) -> None:
            _find_class(data, "Class'ROGame.ROMGWeapon'")["superClassPath"] = (
                "Class'ROGame.ROWeapon'"
            )

        def missing_member(data: dict[str, list[dict[str, object]]]) -> None:
            actor = _find_class(data, "Class'Engine.Actor'")
            actor["declaredNetworkMembers"] = actor["declaredNetworkMembers"][:-1]

        def reversed_members(data: dict[str, list[dict[str, object]]]) -> None:
            actor = _find_class(data, "Class'Engine.Actor'")
            actor["declaredNetworkMembers"][0:2] = reversed(
                actor["declaredNetworkMembers"][0:2]
            )

        def duplicate_net_index(data: dict[str, list[dict[str, object]]]) -> None:
            inventory = _find_class(data, "Class'Engine.Inventory'")
            inventory["declaredNetworkMembers"][1]["uobjectNetIndex"] = (
                inventory["declaredNetworkMembers"][0]["uobjectNetIndex"]
            )

        def missing_inherited_override(
            data: dict[str, list[dict[str, object]]]
        ) -> None:
            inventory = _find_class(data, "Class'Engine.Inventory'")
            inherited = next(
                member
                for member in inventory["declaredNetworkMembers"]
                if member["name"] == "ClientGivenTo"
            )
            inherited["name"] = "DifferentInheritedFunction"

        def wrong_selected_class(data: dict[str, list[dict[str, object]]]) -> None:
            selected = _find_class(
                data,
                "Class'ROGameContent.ROWeap_M61_Grenade_ContentSingle'",
            )
            selected["classPath"] = "Class'ROGame.ROWeap_M61_GrenadeSingle'"

        def promote_overridden_function(
            data: dict[str, list[dict[str, object]]]
        ) -> None:
            weapon = _find_class(data, "Class'Engine.Weapon'")
            overridden = next(
                member
                for member in weapon["declaredNetworkMembers"]
                if member["name"] == "ClientGivenTo"
            )
            overridden["functionSuperPresent"] = False

        for label, mutate in (
            ("super", wrong_super),
            ("member count", missing_member),
            ("member order", reversed_members),
            ("duplicate net index", duplicate_net_index),
            ("missing inherited override", missing_inherited_override),
            ("forbidden M61 class", wrong_selected_class),
            ("overridden function promoted to a new handle", promote_overridden_function),
        ):
            packages = _raw_packages()
            mutate(packages)
            with self.subTest(label=label), self.assertRaises(audit.AuditError):
                audit.build_report(packages)

    def test_rejects_special_declaration_and_any_direct_override(self) -> None:
        def special(data: dict[str, list[dict[str, object]]]) -> dict[str, object]:
            actor = _find_class(data, "Class'Engine.Actor'")
            return next(
                member
                for member in actor["declaredNetworkMembers"]
                if member["name"] == "bNetInitialRotation"
            )

        packages = _raw_packages()
        special(packages)["propertyFlags"] = "0x0000000000000022"
        with self.assertRaises(audit.AuditError):
            audit.build_report(packages)

        packages = _raw_packages()
        special(packages)["specialDeclaration"] = False
        with self.assertRaises(audit.AuditError):
            audit.build_report(packages)

        for value, state in ((False, "explicit"), (True, "explicit"), (None, "unknown")):
            packages = _raw_packages()
            actor = _find_class(packages, "Class'Engine.Actor'")
            actor["directBNetInitialRotationTags"] = [{
                "propertyPath": "Actor'Engine.Default__Actor'.bNetInitialRotation",
                "value": value,
                "valueState": state,
                "sourceOffset": 64,
            }]
            with self.subTest(value=value, state=state), self.assertRaises(
                audit.AuditError
            ):
                audit.build_report(packages)

    def test_rejects_spot_handle_and_terminal_identity_drift(self) -> None:
        packages = _raw_packages()
        actor = _find_class(packages, "Class'Engine.Actor'")
        instigator = next(
            member
            for member in actor["declaredNetworkMembers"]
            if member["name"] == "Instigator"
        )
        instigator["uobjectNetIndex"] = 2503
        with self.assertRaises(audit.AuditError):
            audit.build_report(packages)

        packages = _raw_packages()
        terminal = _find_class(
            packages, "Class'ROGameContent.ROWeap_M60_GPMG_Content'"
        )
        terminal["UClass"]["uobjectNetIndex"] = 511
        with self.assertRaises(audit.AuditError):
            audit.build_report(packages)

    def test_rejects_recursive_forbidden_reference_keys(self) -> None:
        packages = _raw_packages()
        actor = _find_class(packages, "Class'Engine.Actor'")
        actor["CDO"]["StaticReference"] = 99
        with self.assertRaises(audit.AuditError):
            audit.build_report(packages)


class JsonBoundaryTests(unittest.TestCase):
    def test_load_jsonl_rejects_duplicate_blank_invalid_and_forbidden_data(self) -> None:
        fixtures = {
            "duplicate": b'{"record":"x","record":"y"}\n',
            "blank": b"\n",
            "invalid utf8": b'{"record":"\xff"}\n',
            "forbidden": b'{"record":"x","nested":{"WireReference":1}}\n',
        }
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for label, payload in fixtures.items():
                path = root / (label + ".jsonl")
                path.write_bytes(payload)
                with self.subTest(label=label), self.assertRaises(audit.AuditError):
                    audit.load_jsonl(path)

    def test_final_report_rejects_unknown_local_runtime_and_handle_drift(self) -> None:
        base = audit.build_report(_raw_packages())
        mutations = []

        unknown = copy.deepcopy(base)
        unknown[0]["unexpected"] = False
        mutations.append(("unknown", unknown))

        local_path = copy.deepcopy(base)
        local_path[1]["handles"][0]["name"] = r"C:\secret\member"
        mutations.append(("local path", local_path))

        runtime = copy.deepcopy(base)
        runtime[-1]["runtimeAuthorized"] = True
        mutations.append(("runtime", runtime))

        gap = copy.deepcopy(base)
        gap[1]["handles"][0]["handle"] = 2
        mutations.append(("gap", gap))

        spot_mismatch = copy.deepcopy(base)
        spot_mismatch[1]["handles"][4]["name"] = "NotInstigator"
        mutations.append(("spot handle mismatch", spot_mismatch))

        missing_inherited_override = copy.deepcopy(base)
        inherited = next(
            row
            for row in missing_inherited_override[1]["handles"]
            if row["kind"] == "function" and row["name"] == "ClientGivenTo"
        )
        inherited["name"] = "ClientGivenToChanged"
        mutations.append(("missing inherited override", missing_inherited_override))

        forbidden = copy.deepcopy(base)
        forbidden[1]["handles"][0]["wireReference"] = 1
        mutations.append(("forbidden", forbidden))

        short_chain = copy.deepcopy(base)
        short_chain[1]["classChain"].pop()
        mutations.append(("short chain", short_chain))

        overridden_handle = copy.deepcopy(base)
        function_row = next(
            row for row in overridden_handle[1]["handles"] if row["kind"] == "function"
        )
        function_row["functionSuperPresent"] = True
        mutations.append(("overridden function received a new handle", overridden_handle))

        for label, report in mutations:
            with self.subTest(label=label), self.assertRaises(audit.AuditError):
                audit.validate_final_report(report)


class OutputBoundaryTests(unittest.TestCase):
    def setUp(self) -> None:
        self.report = audit.build_report(_raw_packages())

    def test_report_write_is_atomic_and_requires_explicit_overwrite(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            destination = Path(temporary, "report.jsonl")
            audit.write_report(self.report, destination, False)
            original = destination.read_bytes()
            self.assertTrue(original.endswith(b"\n"))
            self.assertEqual(len(original.splitlines()), 5)
            with self.assertRaises(audit.AuditError):
                audit.write_report(self.report, destination, False)
            audit.write_report(self.report, destination, True)
            self.assertEqual(destination.read_bytes(), original)
            self.assertEqual(list(destination.parent.glob("report.jsonl.*.tmp")), [])

    def test_refuses_direct_and_hardlink_aliases_to_protected_inputs(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            protected = root / "Core.u"
            protected.write_bytes(b"pinned package")
            alias = root / "alias.jsonl"
            os.link(protected, alias)
            with self.assertRaises(audit.AuditError):
                audit.write_report(
                    self.report, protected, True, protected_inputs=(protected,)
                )
            with self.assertRaises(audit.AuditError):
                audit.write_report(
                    self.report, alias, True, protected_inputs=(protected,)
                )
            self.assertEqual(protected.read_bytes(), b"pinned package")

    def test_raced_final_hardlink_is_replaced_without_mutating_input(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            protected = root / "tshark-or-package.bin"
            protected.write_bytes(b"pinned bytes")
            destination = root / "report.jsonl"
            original_writer = audit._write_bounded_jsonl

            def raced_writer(records: object, output: object) -> None:
                original_writer(records, output)
                if not os.path.lexists(destination):
                    os.link(protected, destination)

            with mock.patch.object(
                audit, "_write_bounded_jsonl", side_effect=raced_writer
            ):
                audit.write_report(
                    self.report,
                    destination,
                    True,
                    protected_inputs=(protected,),
                )
            self.assertEqual(protected.read_bytes(), b"pinned bytes")
            self.assertFalse(os.path.samefile(protected, destination))
            self.assertEqual(len(destination.read_text(encoding="utf-8").splitlines()), 5)

    def test_rejects_output_inside_installed_tree(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with self.assertRaises(audit.AuditError):
                audit.validate_output_destination(
                    root / "report.jsonl", (), (root,)
                )


class CommandBoundaryTests(unittest.TestCase):
    def test_commands_use_no_profile_without_execution_policy_bypass(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            wrapper = root / "wrapper.ps1"
            uelib = root / "Eliot.UELib.dll"
            executable = root / "extractor.exe"
            for path in (wrapper, uelib, executable):
                path.write_bytes(b"fixture")
            with mock.patch.object(
                audit, "_run_command", return_value=str(executable)
            ) as run, mock.patch.object(audit, "validate_file_identity"):
                self.assertEqual(
                    audit.build_pinned_extractor(wrapper, uelib), executable
                )
            command = run.call_args.args[0]
            self.assertIn("-NoProfile", command)
            self.assertIn("-BuildOnly", command)
            self.assertNotIn("-ExecutionPolicy", command)
            self.assertNotIn("Bypass", command)

            destination = root / "raw.jsonl"
            destination.write_text("{}\n", encoding="utf-8")
            with mock.patch.object(audit, "_run_command") as extract:
                audit.run_class_extractor(
                    audit.PACKAGE_SPECS[0], root / "Core.u", wrapper, uelib,
                    destination,
                )
            extract_command = extract.call_args.args[0]
            self.assertIn("-NoProfile", extract_command)
            self.assertIn("-ClassArtifacts", extract_command)
            self.assertNotIn("-ExecutionPolicy", extract_command)
            self.assertNotIn("Bypass", extract_command)

    def test_fatal_error_does_not_echo_user_paths(self) -> None:
        stderr = io.StringIO()
        with mock.patch.object(
            audit,
            "validate_installed_inputs",
            side_effect=audit.AuditError("pinned package input is missing"),
        ), contextlib.redirect_stderr(stderr):
            exit_code = audit.main(
                (
                    "--package-root", r"D:\private\game",
                    "--wrapper", r"D:\private\repo\wrapper.ps1",
                    "--uelib", r"D:\private\tools\Eliot.UELib.dll",
                )
            )
        self.assertEqual(exit_code, 2)
        self.assertNotIn("private", stderr.getvalue().casefold())
        failure = json.loads(stderr.getvalue())
        self.assertFalse(failure["runtimeAuthorized"])
        self.assertFalse(failure["complete"])


if __name__ == "__main__":
    unittest.main()
