// tests/TestMain.cpp
//
// Single entry point for the consolidated test runner (RS2V_TESTS_UNIFIED).
//
// All test translation units are compiled into ONE executable (rs2v_tests).
// Each TEST/TEST_F self-registers into the global ::native::Registry at static
// init, so a single main() runs every registered test in one process — one
// console window instead of one-executable-per-suite. Supports the framework's
// CLI: `--list` to enumerate, `--filter=<substr>` to run a subset
// (e.g. rs2v_tests --filter=CompressionHandler).
//
// Under RS2V_TESTS_UNIFIED the per-file RS2V_TEST_MAIN() expands to nothing, so
// this is the only main() in the link.

#include "TestFramework.h"

int main(int argc, char** argv) {
    return ::native::RunAllTests(argc, argv);
}
