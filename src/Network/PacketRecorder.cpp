// src/Network/PacketRecorder.cpp — see PacketRecorder.h

#include "Network/PacketRecorder.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>

#include "Utils/Logger.h"

namespace net {

namespace {

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

// Append the lower-case hex of [data, data+len) to `out`. ~2 chars/byte.
void AppendHex(std::string& out, const uint8_t* data, size_t len) {
    static const char* kHex = "0123456789abcdef";
    out.reserve(out.size() + len * 2);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(kHex[(data[i] >> 4) & 0x0F]);
        out.push_back(kHex[data[i] & 0x0F]);
    }
}

// Minimal JSON string escaping for the small set of fields we emit (peer/ctx/detail).
std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    static const char* kHex = "0123456789abcdef";
                    out += "\\u00";
                    out.push_back(kHex[(c >> 4) & 0x0F]);
                    out.push_back(kHex[c & 0x0F]);
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

} // namespace

PacketRecorder& PacketRecorder::Instance() {
    static PacketRecorder inst;
    return inst;
}

PacketRecorder::PacketRecorder() {
    // Default ON; disabled only if RS2V_PKTLOG is explicitly "0"/"false"/"off".
    const char* env = std::getenv("RS2V_PKTLOG");
    m_enabled = !(env && (std::string(env) == "0" || std::string(env) == "false" ||
                          std::string(env) == "off"));
}

PacketRecorder::~PacketRecorder() {
    std::lock_guard<std::mutex> lk(m_mu);
    if (m_pktOut.is_open())  m_pktOut.close();
    if (m_nullOut.is_open()) m_nullOut.close();
}

void PacketRecorder::EnsureOpen() {
    if (m_opened) return;
    m_opened = true;  // attempt once; if it fails we stay disabled (no retry storm)
    const uint64_t stamp = NowMs();
    try {
        std::filesystem::create_directories("packetlog");
    } catch (...) {
        // fall through; opening below will fail and disable recording
    }
    const std::string base = "packetlog/session_" + std::to_string(stamp);
    m_pktOut.open(base + ".jsonl", std::ios::out | std::ios::app);
    m_nullOut.open("packetlog/nulls_" + std::to_string(stamp) + ".jsonl",
                   std::ios::out | std::ios::app);
    if (!m_pktOut.is_open()) {
        m_enabled = false;
        Logger::Warn("[PacketRecorder] could not open packetlog/session_%llu.jsonl — recording disabled",
                     static_cast<unsigned long long>(stamp));
        return;
    }
    Logger::Info("[PacketRecorder] recording datagrams to packetlog/session_%llu.jsonl "
                 "(set RS2V_PKTLOG=0 to disable)", static_cast<unsigned long long>(stamp));
}

void PacketRecorder::RecordDatagram(PktDir dir, uint32_t clientId, const std::string& peer,
                                    const uint8_t* data, size_t len) {
    if (!m_enabled) return;
    std::lock_guard<std::mutex> lk(m_mu);
    if (!m_enabled) return;
    EnsureOpen();
    if (!m_enabled || !m_pktOut.is_open()) return;

    std::string line;
    line.reserve(len * 2 + 96);
    line += "{\"seq\":";
    line += std::to_string(m_seq++);
    line += ",\"t\":";
    line += std::to_string(NowMs());
    line += ",\"dir\":\"";
    line += (dir == PktDir::C2S) ? "C2S" : "S2C";
    line += "\",\"client\":";
    line += std::to_string(clientId);
    line += ",\"peer\":\"";
    line += JsonEscape(peer);
    line += "\",\"len\":";
    line += std::to_string(len);
    line += ",\"hex\":\"";
    if (data && len) AppendHex(line, data, len);
    line += "\"}\n";
    m_pktOut << line;
    m_pktOut.flush();   // durability for crash-time debugging; datagram rate is modest
}

void PacketRecorder::RecordNull(const char* context, const std::string& detail, uint32_t clientId) {
    // Always surface in the main log (greppable) regardless of file state.
    Logger::Warn("[NULLREC] client %u: null/missing at %s: %s",
                 clientId, context ? context : "?", detail.c_str());
    if (!m_enabled) return;
    std::lock_guard<std::mutex> lk(m_mu);
    if (!m_enabled) return;
    EnsureOpen();
    if (!m_enabled || !m_nullOut.is_open()) return;

    std::string line = "{\"t\":";
    line += std::to_string(NowMs());
    line += ",\"client\":";
    line += std::to_string(clientId);
    line += ",\"ctx\":\"";
    line += JsonEscape(context ? context : "?");
    line += "\",\"detail\":\"";
    line += JsonEscape(detail);
    line += "\"}\n";
    m_nullOut << line;
    m_nullOut.flush();
}

} // namespace net
