# tests/CMakeLists.txt
# Test suite for the RS2V server emulator.
#
# Design constraints (do NOT "simplify" these away):
#   * By default all test sources are linked into ONE consolidated executable
#     (rs2v_tests) with a single main() in TestMain.cpp — one process / one
#     console for the whole suite, not one executable per file. Each source's
#     RS2V_TEST_MAIN() expands to nothing under RS2V_TESTS_UNIFIED; tests
#     self-register into a global registry so the single main() runs them all.
#     (-DRS2V_TESTS_INDIVIDUAL=ON restores one-executable-per-file for isolating
#     a single suite while debugging.) See the target-creation block at the end.
#   * The suite uses the RS2V NATIVE test framework (tests/TestFramework.h +
#     tests/TestMock.h) — a self-contained, dependency-free reimplementation of
#     the GoogleTest/GoogleMock subset these tests use. There is NO external
#     test dependency and NO network access at configure or build time.
#
#     Why: GoogleTest was previously pulled via CMake FetchContent, which clones
#     github.com/google/googletest at configure time. In offline / sandboxed /
#     restricted-CI environments that clone fails and the whole suite becomes
#     un-buildable. The native framework removes that dependency entirely while
#     keeping the test sources unchanged (they just include the native headers).
#
#   * Tests link the rs2v_core static library defined in the root CMakeLists.txt.
#
# This is an explicit ALLOW-LIST, not a glob: only files known to compile (after
# include-path / API reconciliation) are wired up. Files that depend on headers
# that do not exist in src/, or whose tests were written against an API that was
# never implemented, are listed in the EXCLUDED block at the bottom so the gap is
# tracked rather than silently dropped.
#
# Background: most tests/*Tests.cpp files were authored against an aspirational
# API surface that diverged from (or was never implemented in) src/. Where the
# gap is a wrong include path, a dead mock, or a renamed method, the test was
# reconciled. Where the test assumes whole classes/structs/methods that do not
# exist, it is EXCLUDED rather than rewritten (rewriting would mean inventing
# new test behavior, which is out of scope).

# Test layout (consolidated single-binary runner, the default):
#
#   Every test source self-registers its TEST/TEST_F cases into a global
#   ::native::Registry at static-init time, so they can all be linked into ONE
#   executable (rs2v_tests) driven by a single main() in TestMain.cpp. That is
#   one process / one console window for the whole suite, instead of one
#   executable (and on Windows one console) per test file. Run a subset with
#   `rs2v_tests --filter=<substr>`; list everything with `rs2v_tests --list`.
#
#   Under this build each per-file RS2V_TEST_MAIN() expands to nothing (gated by
#   the RS2V_TESTS_UNIFIED compile definition) so there is exactly one main().
#
#   Set -DRS2V_TESTS_INDIVIDUAL=ON to instead build one standalone executable per
#   test file (each with its own main via RS2V_TEST_MAIN()) — useful for running
#   or debugging a single suite in isolation.
#
# rs2v_add_test(<name>) just records <name> in the allow-list below; the targets
# are created once, after the list is complete.
set(RS2V_TEST_NAMES "")
macro(rs2v_add_test test_name)
    list(APPEND RS2V_TEST_NAMES ${test_name})
endmacro()

# Settings shared by every test target (the consolidated binary or, in
# individual mode, each per-file executable).
function(rs2v_apply_test_settings tgt)
    target_link_libraries(${tgt} PRIVATE rs2v_core Threads::Threads)
    target_include_directories(${tgt}
        PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
    # TestMock.h's MOCK_METHOD relies on Gustedt's ISEMPTY trick to detect a
    # zero-argument arg-list. That requires a conformant preprocessor; MSVC's
    # legacy preprocessor miscounts () as one arg (emits a stray `a0`). Opt the
    # test TUs into the conformant preprocessor so 0-arg mocks compile.
    if(MSVC)
        target_compile_options(${tgt} PRIVATE /Zc:preprocessor)
    endif()
endfunction()

# -----------------------------------------------------------------------------
# ALLOW-LIST — building + linking against rs2v_core
# -----------------------------------------------------------------------------
rs2v_add_test(StackTraceTests)   # crash-handler stack capture: frames non-empty, addresses non-zero, symbol name when available
rs2v_add_test(Vector3Tests)      # reconciled: global Vector3, LengthSquared/DistanceSquared, member Lerp, inline clamp
rs2v_add_test(ThreadPoolTests)   # reconciled: future-based waits (no WaitAll/GetThisThreadIndex)
rs2v_add_test(INIParserTests)    # reconciled: self-contained TestINIParser; dead mocks + INISection clash removed
rs2v_add_test(AuthenticationTests) # implemented: Security/{EACProxy,AuthManager,PasswordHasher,TokenManager}
rs2v_add_test(PacketFramingTests)  # UE3 packet/bunch framing codec; fixtures = real handshake capture
rs2v_add_test(ControlChannelTests) # UE3 control-channel message-body codec; round-trips per spec §4
rs2v_add_test(PacketAssemblerTests) # UE3 outbound control-channel packet assembler (sequencing/fragmentation)
rs2v_add_test(ControlReassemblerTests) # UE3 inbound control-channel reassembler (ordering/dedup/delimiting)
rs2v_add_test(ControlReassemblerFuzzTests) # fuzz/hostile-input: out-of-order/dup/huge-seq/oversized bunches -> bounded buffering, no crash/over-read/hang
rs2v_add_test(HandshakeStateFuzzTests)     # fuzz/hostile-input: malformed/truncated/oversized NMT + bad type bytes -> never throws/crashes, phase stays valid
rs2v_add_test(HandshakeIntegrationTests) # end-to-end client<->server handshake over the real wire codec
rs2v_add_test(ActorReplicationTests)     # actor-channel NetGUID/SerializeNewActor codec (per-session replication)
rs2v_add_test(BitReaderFuzzTests)        # fuzz + malformed-input hardening for BitReader/BitWriter (overflow safety, no OOB/hang, round-trip)
rs2v_add_test(PacketCodecFuzzTests)      # fuzz/hostile-input: random+mutated+boundary bytes into Decode/Encode (no crash/OOB/hang), valid round-trip
rs2v_add_test(NetworkPacketSafetyTests)  # malformed-input bounds checks for legacy Packet readers (ReadString/DecodeString/ReadUInt/ReadFloat/ReadBytes)

# -----------------------------------------------------------------------------
# Unit coverage for self-contained subsystem APIs (added with the native test
# framework). Each targets the REAL src/ API surface and asserts published
# vectors / round-trips / invariants.
# -----------------------------------------------------------------------------
rs2v_add_test(StringUtilsTests)        # Utils/StringUtils: trim/split/join/case/parse
rs2v_add_test(MathUtilsTests)          # Utils/MathUtils: clamp/lerp/remap/angles/gcd/quadratic
rs2v_add_test(CryptoUtilsTests)        # Utils/CryptoUtils: SHA-256/HMAC/Base64 vectors + RNG
rs2v_add_test(ProtocolUtilsTests)      # Protocol/ProtocolUtils: hex/base64/checksum/tag<->type
rs2v_add_test(NetworkUtilsTests)       # Network/NetworkUtils: IPv4 validate/normalize, ports
rs2v_add_test(BoundsTests)             # Game/Bounds: AABB contains/center/size
rs2v_add_test(RandomGeneratorTests)    # Utils/RandomGenerator: seed determinism, range bounds
rs2v_add_test(CompressionHandlerTests) # Protocol/CompressionHandler: NONE + zlib round-trips
rs2v_add_test(CommandManagerTests)     # Game/CommandManager + Network/RemoteAdminServer: tokenise/alias/permission-gate/help-visibility + defensive SOAP XML parse
rs2v_add_test(CrashHandlerTests)       # Utils/CrashHandler: non-fatal Guard swallows+counts exceptions, returns completion status

# Scripting is OFF by default (deprecated .NET COM hosting). The scripting test
# is gated on ENABLE_SCRIPTING, but it ALSO references Scripting/ScriptEngine.h
# which does not exist (src has CSharpEngine / ScriptManager), so it stays off.
if(ENABLE_SCRIPTING)
    # rs2v_add_test(ScriptingTests)  # blocked: missing Scripting/ScriptEngine.h
endif()

# -----------------------------------------------------------------------------
# EXCLUDED — reason tracked per file.
#
# (A) Reference headers that do NOT exist in src/ (cannot compile; do not
#     fabricate src headers):
#   EACTests              -> Security/EACProxy.h
#   IntegrationTests      -> Security/EACProxy.h
#   SecurityTests         -> Security/AuthManager.h, Utils/PasswordHasher.h,
#                            Utils/TokenManager.h, Security/EACProxy.h
#   CollisionTests        -> Physics/BoundingVolume.h, Math/Matrix4.h,
#                            Math/Quaternion.h
#   PhysicsTests          -> Physics/BoundingVolume.h
#   ReplicationTests      -> Network/ReplicationSnapshot.h
#   ScriptingTests        -> Scripting/ScriptEngine.h (also gated ENABLE_SCRIPTING)
#   VehicleTests          -> Game/VehicleManager.h
#   WeaponTests           -> Game/WeaponManager.h
#   SteamQueryTests       -> Steamworks SDK types (ISteamAPI, SteamLeaderboard_t,
#                            etc.) that are not part of this repo
#
# (B) Written against an API that was never implemented in src/ (would require
#     inventing new test behavior, not a modest reconciliation):
#   MemoryPoolTests       -> MemoryPool is a compile-time TEMPLATE
#                            MemoryPool<BlockSize,BlocksPerChunk>; test assumes a
#                            runtime ctor + OutstandingAllocations()/ArenaCount().
#   CompressionTests      -> CompressionHandler is static bool Compress(in,out,
#                            algo,level); test assumes instance Compress(buf)->buf,
#                            GetCompressionRatio/ShouldCompress/throws.
#   ConfigValidationTests -> Config wrappers take ConfigManager in ctor; test
#                            assumes Initialize(path)/Initialize(&mgr),
#                            GetLogLevel/GetCompressionThreshold/Reload/etc.
#   ConfigTests           -> mocks derive from FileUtils/ConfigManager (FileUtils
#                            is a NAMESPACE; ConfigManager methods non-virtual).
#   SecurityConfigTests   -> SecurityConfig has no default ctor / no Initialize().
#   NetworkTests          -> mocks override non-virtual NetworkConfig methods
#                            (GetMaxBandwidthMbps/GetPacketTimeout/... not present).
#   BandwidthTests        -> same: mocks over non-virtual NetworkConfig
#                            (GetMTU/GetTickRate/...).
#   MapTests              -> mocks override non-virtual ConfigManager methods.
#   MovementValidationTests -> mocks override non-virtual PlayerManager methods
#                            (GetPlayerPosition/IsValidPlayerPosition/...).
#   InputValidationTests  -> mocks over non-virtual ConfigManager/ChatManager.
#   PlayerTests           -> PlayerManager has no AddNewPlayer/IsValidPlayer.
#   TeamTests             -> TeamManager has no CreateTeam; no TeamID enum.
#   ObjectiveTests        -> GameModeDefinition lacks modeName/matchDuration/maxScore.
#   RoundTests            -> same GameModeDefinition field mismatch.
#   GameTests             -> redefines Player/PlayerState/GameModeDefinition
#                            (clashes with real types).
#   ProtocolTests         -> ProtocolUtils/PacketSerializer have a different API
#                            (TypeToTag(PacketType)->string vs ->uint16_t; no
#                            TagToName/NameToTag; PacketSerializer is static R/W).
#   UtilsTests            -> StringUtils/FileUtils/PathUtils/TimeUtils are
#                            NAMESPACES; test instantiates them as classes.
#   PacketFlowTests       -> instance PacketSerializer::Serialize(Packet),
#                            PacketAnalyzer::Analyze(...), ProtocolUtils::TagToName,
#                            HandlerLibraryManager::GetHandler — none exist as used.
#   PacketAnalysisTests   -> PacketAnalyzer has no Analyze/GenerateHexDump;
#                            PacketSerializer has no instance Serialize.
#   HandlerGenerationTests-> mocks derive from FileUtils namespace; uses
#                            std::chrono::file_time_type (filesystem type).
#   ClientSimulatorTests  -> references undefined ReceivedPacket and a client
#                            simulation API not present in src/.
#   PerformanceTests      -> PacketAnalyzer::Analyze and related API not present.
#   LoadTests             -> COMPILES (after atomic<double> fix + adding main),
#                            but its self-contained traffic simulation asserts
#                            throughput >= 10 Mbps using a CUMULATIVE byte count
#                            (not per-second) that is unsatisfiable at low load,
#                            so all 4 cases fail by construction. Passing it would
#                            require rewriting the thresholds (inventing behavior).
#                            Also slow (~75-90s). Excluded; build verified.
# -----------------------------------------------------------------------------

# -----------------------------------------------------------------------------
# Create the test target(s) from the allow-list accumulated above.
# -----------------------------------------------------------------------------
option(RS2V_TESTS_INDIVIDUAL
       "Build one standalone executable per test file instead of the consolidated rs2v_tests runner"
       OFF)

if(RS2V_TESTS_INDIVIDUAL)
    # One executable per test file, each with its own RS2V_TEST_MAIN() entry.
    foreach(_t IN LISTS RS2V_TEST_NAMES)
        add_executable(${_t} ${_t}.cpp)
        rs2v_apply_test_settings(${_t})
        add_test(NAME ${_t} COMMAND ${_t})
    endforeach()
else()
    # Consolidated single-binary runner: every test source + one shared main().
    set(_rs2v_test_srcs TestMain.cpp)
    foreach(_t IN LISTS RS2V_TEST_NAMES)
        list(APPEND _rs2v_test_srcs ${_t}.cpp)
    endforeach()
    add_executable(rs2v_tests ${_rs2v_test_srcs})
    rs2v_apply_test_settings(rs2v_tests)
    target_compile_definitions(rs2v_tests PRIVATE RS2V_TESTS_UNIFIED)
    # A single CTest entry runs the whole suite in one process. The binary still
    # prints per-test [ OK ]/[ FAILED ] lines, and --output-on-failure surfaces
    # them in CI. Use `ctest -R rs2v_tests` or run the exe directly with
    # --filter=<substr> to scope to a subset.
    add_test(NAME rs2v_tests COMMAND rs2v_tests)
endif()
