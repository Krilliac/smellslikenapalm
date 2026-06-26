# Static analysis (cppcheck 2.20.0) — iteration 8

Ran `cppcheck --enable=warning,performance,portability --inconclusive --std=c++17` over
src/Network, src/Protocol, src/Security, src/Game, src/Config, src/Utils.

Only 11 findings total (the manual hardening fleets + clean codebase). Dispositions:

| Finding | File | Disposition |
|---|---|---|
| uninitMemberVar Sha256::m_buf | Security/PasswordHasher.cpp:30 | FIXED — zero-init m_buf/m_len/m_bufLen (defense-in-depth; was written-before-read, not exploitable) |
| dangerousTypeCast (old-style C cast) ×3 | Network/NetworkInterface.cpp:136, SocketFactory.cpp:86, TCPSocket.cpp:75 | NOT-A-BUG — standard sockaddr_in*/sockaddr* casts in BSD socket code; correct + idiomatic |
| invalidPointerCast float*→char* ×6 | Game/MapManager.cpp:116-121 | NOT-A-BUG — intentional float→bytes serialization for map config; correct on the target (x86/x64 LE). Left as-is to avoid touching working save/load |
| returnByReference GetConfig() | telemetry/TelemetryManager.h:196 | minor perf, non-security; left as-is |

No memory-safety, OOB, overflow, leak, or use-after-free findings — consistent with the
fuzz results (0 crashes over ~1.3M malformed inputs).
