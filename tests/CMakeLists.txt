# tests/CMakeLists.txt
# Test suite for the RS2V server emulator.
#
# Design constraints (do NOT "simplify" these away):
#   * Each tests/*Tests.cpp file provides its OWN entry point via the
#     RS2V_TEST_MAIN() macro. Therefore we build ONE executable per test file.
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

# Helper: add one test executable per source file, each with its own main().
# The native framework is header-only, so tests just need the tests/ directory
# on the include path (for "TestFramework.h" / "TestMock.h") plus rs2v_core.
function(rs2v_add_test test_name)
    add_executable(${test_name} ${test_name}.cpp)
    target_link_libraries(${test_name} PRIVATE rs2v_core Threads::Threads)
    target_include_directories(${test_name}
        PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME ${test_name} COMMAND ${test_name})
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
