import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "ground_cooked_map_guids.py"
SPEC = importlib.util.spec_from_file_location("ground_cooked_map_guids", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


def resolver_source(entries, declared_count=None):
    count = len(entries) if declared_count is None else declared_count
    rows = "\n".join(
        f'    {{"{map_id}", PackageGuid("{guid}")}},'
        for map_id, guid in entries
    )
    return (
        "// BEGIN SOURCE-GROUNDED MAP GUIDS\n"
        f"constexpr std::array<MapGuidEntry, {count}> kMapPackageGuids = {{{{\n"
        f"{rows}\n"
        "}};\n"
        "// END SOURCE-GROUNDED MAP GUIDS\n"
    )


class GuidEncodingTests(unittest.TestCase):
    def test_ue3_display_words_become_little_endian_wire_words(self):
        wire = MODULE.display_guid_to_wire_bytes(
            "28867F2947A19DC5A5C5A0801B37FEE5"
        )
        self.assertEqual(
            wire.hex().upper(),
            "297F8628C59DA14780A0C5A5E5FE371B",
        )

    def test_malformed_guid_is_rejected(self):
        for value in ("", "0" * 31, "Z" * 32, "0" * 33):
            with self.subTest(value=value):
                with self.assertRaises(MODULE.ValidationError):
                    MODULE.display_guid_to_wire_bytes(value)


class ResolverParsingTests(unittest.TestCase):
    def write_source(self, root, entries, declared_count=None):
        path = Path(root) / "RetailBootstrap.cpp"
        path.write_text(
            resolver_source(entries, declared_count), encoding="utf-8"
        )
        return path

    def test_valid_table_is_parsed_case_insensitively(self):
        with tempfile.TemporaryDirectory() as root:
            path = self.write_source(
                root,
                [
                    ("VNTE-Resort", "C75E786345B77AA5243259ABAF16C294"),
                    ("VNSK-Firebase", "BA1BDD124DC9ECCF3B48B79C2F86CECE"),
                ],
            )
            parsed = MODULE.read_resolver_table(path)
            self.assertEqual(parsed["vnte-resort"][0], "VNTE-Resort")
            self.assertEqual(len(parsed), 2)

    def test_declared_count_must_match_rows(self):
        with tempfile.TemporaryDirectory() as root:
            path = self.write_source(
                root,
                [("VNTE-Resort", "C75E786345B77AA5243259ABAF16C294")],
                declared_count=2,
            )
            with self.assertRaises(MODULE.ValidationError):
                MODULE.read_resolver_table(path)

    def test_duplicate_map_or_guid_is_rejected(self):
        resort = "C75E786345B77AA5243259ABAF16C294"
        cases = [
            [("VNTE-Resort", resort), ("vnte-resort", "1" * 32)],
            [("VNTE-Resort", resort), ("VNTE-Other", resort)],
        ]
        for entries in cases:
            with self.subTest(entries=entries), tempfile.TemporaryDirectory() as root:
                path = self.write_source(root, entries)
                with self.assertRaises(MODULE.ValidationError):
                    MODULE.read_resolver_table(path)


class ConfigParsingTests(unittest.TestCase):
    def test_configured_paths_and_ids_must_be_unique(self):
        with tempfile.TemporaryDirectory() as root:
            root_path = Path(root)
            first = root_path / "one.roe"
            second = root_path / "two.roe"
            config = root_path / "maps.ini"
            config.write_text(
                f"[VNTE-Test]\nfile={first}\n\n"
                f"[vnte-test]\nfile={second}\n",
                encoding="utf-8",
            )
            with self.assertRaises(MODULE.ValidationError):
                MODULE.read_configured_maps(config)


class SequentialGroundingTests(unittest.TestCase):
    def test_known_maps_are_skipped_and_missing_maps_keep_config_order(self):
        with tempfile.TemporaryDirectory() as root:
            root_path = Path(root)
            known_source = root_path / "known.roe"
            first_source = root_path / "first.roe"
            second_source = root_path / "second.roe"
            for path, content in (
                (known_source, b"known"),
                (first_source, b"first"),
                (second_source, b"second"),
            ):
                path.write_bytes(content)
            configured = [
                MODULE.MapSpec("VNTE-Known", known_source),
                MODULE.MapSpec("VNTE-First", first_source),
                MODULE.MapSpec("VNTE-Second", second_source),
            ]
            resolver = {
                "vnte-known": (
                    "VNTE-Known",
                    "C75E786345B77AA5243259ABAF16C294",
                )
            }
            calls = []
            guids = iter(("1" * 32, "2" * 32))

            def extract(source):
                calls.append(source)
                return next(guids)

            records = []
            result = MODULE.ground_missing_maps(
                configured, resolver, extract, records.append
            )
            self.assertEqual(calls, [first_source, second_source])
            self.assertEqual(result.attempted, 2)
            self.assertEqual([item.map_id for item in result.grounded], [
                "VNTE-First",
                "VNTE-Second",
            ])
            self.assertEqual([item["record"] for item in records], [
                "mapGuid",
                "mapGuid",
            ])
            self.assertTrue(all(
                item.sha256_before == item.sha256_after
                for item in result.grounded
            ))

    def test_source_mutation_is_fatal_even_when_extractor_returns_guid(self):
        with tempfile.TemporaryDirectory() as root:
            source = Path(root) / "map.roe"
            source.write_bytes(b"before")

            def mutate(path):
                path.write_bytes(b"after")
                return "1" * 32

            with self.assertRaises(MODULE.MutationError):
                MODULE.ground_missing_maps(
                    [MODULE.MapSpec("VNTE-Test", source)],
                    {},
                    mutate,
                    lambda _record: None,
                )

    def test_duplicate_new_guid_is_fatal(self):
        with tempfile.TemporaryDirectory() as root:
            sources = [Path(root) / "one.roe", Path(root) / "two.roe"]
            for source in sources:
                source.write_bytes(b"unchanged")

            with self.assertRaises(MODULE.ValidationError):
                MODULE.ground_missing_maps(
                    [
                        MODULE.MapSpec("VNTE-One", sources[0]),
                        MODULE.MapSpec("VNTE-Two", sources[1]),
                    ],
                    {},
                    lambda _source: "1" * 32,
                    lambda _record: None,
                )


if __name__ == "__main__":
    unittest.main()
