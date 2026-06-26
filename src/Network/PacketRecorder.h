// src/Network/PacketRecorder.h
//
// Global wire/packet recorder — a WoW-emulator-style "sniff" for the RS2V server.
//
// PURPOSE: capture EVERY UDP datagram the server sends and receives (both
// directions), plus every notable NULL/missing-object occurrence on the net path,
// to dedicated structured logs that are separate from the verbose debug stream.
// This is the ground-truth record used to debug replication (e.g. the role-select
// state layer): you read exactly what crossed the wire instead of guessing.
//
// Output (created lazily under ./packetlog/ on first record):
//   packetlog/session_<unixms>.jsonl  — one JSON object per datagram:
//       {"seq":N,"t":<ms>,"dir":"C2S"|"S2C","client":N,"peer":"ip:port",
//        "len":N,"hex":"aabb.."}
//   packetlog/nulls_<unixms>.jsonl    — one JSON object per null occurrence:
//       {"t":<ms>,"ctx":"where","detail":"what","client":N}
//
// The raw hex is decodable offline by tools/pktlog_decode.py (reuses the
// mock_client bunch decoder), the same way WoW packet logs are parsed by WPP.
//
// OBSERVABILITY ONLY: never mutates a datagram, never blocks the wire on failure
// (a closed/failed log file silently disables recording), thread-safe via an
// internal mutex. Disable entirely by setting the env var RS2V_PKTLOG=0.

#pragma once

#include <cstdint>
#include <cstddef>
#include <fstream>
#include <mutex>
#include <string>

namespace net {

enum class PktDir { C2S, S2C };   // Client->Server (inbound), Server->Client (outbound)

class PacketRecorder {
public:
    // Process-wide singleton.
    static PacketRecorder& Instance();

    bool Enabled() const { return m_enabled; }

    // Record one raw datagram (either direction). `peer` is "ip:port". Safe to call
    // with len==0 / data==nullptr (recorded as an empty datagram). Never throws.
    void RecordDatagram(PktDir dir, uint32_t clientId, const std::string& peer,
                        const uint8_t* data, size_t len);

    // Record a NULL/missing-object occurrence on the net path. Also emits a
    // Logger::Warn with a "[NULLREC]" tag so it is greppable in the main log.
    void RecordNull(const char* context, const std::string& detail, uint32_t clientId = 0);

private:
    PacketRecorder();
    ~PacketRecorder();
    PacketRecorder(const PacketRecorder&) = delete;
    PacketRecorder& operator=(const PacketRecorder&) = delete;

    void EnsureOpen();   // lazily creates packetlog/ and opens the two files (caller holds m_mu)

    std::mutex    m_mu;
    std::ofstream m_pktOut;
    std::ofstream m_nullOut;
    bool          m_enabled = false;   // honours RS2V_PKTLOG env (default on)
    bool          m_opened  = false;
    uint64_t      m_seq     = 0;
};

// Convenience: record a null on the net path (no-op-cheap when recording is off).
inline void RecordNetNull(const char* context, const std::string& detail, uint32_t clientId = 0) {
    PacketRecorder::Instance().RecordNull(context, detail, clientId);
}

} // namespace net
