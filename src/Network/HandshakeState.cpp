// src/Network/HandshakeState.cpp
//
// Implementation of the per-connection UE3 / RS2 control-channel handshake
// state machine. See HandshakeState.h for the design / state diagram.

#include "Network/HandshakeState.h"

#include "Network/ControlChannel.h"
#include "Utils/Logger.h"

#include <random>

const char* HandshakePhaseName(HandshakePhase p) {
    switch (p) {
        case HandshakePhase::AwaitingHello: return "AwaitingHello";
        case HandshakePhase::ChallengeSent: return "ChallengeSent";
        case HandshakePhase::AwaitingLogin: return "AwaitingLogin";
        case HandshakePhase::WelcomeSent:   return "WelcomeSent";
        case HandshakePhase::Joined:        return "Joined";
        case HandshakePhase::Rejected:      return "Rejected";
    }
    return "Unknown";
}

namespace {
// Generate a short random server nonce for the Challenge. Not cryptographic -
// real challenge/response validation is intentionally out of scope (Steam auth
// is stubbed); this only needs to be a non-empty, per-connection string.
uint32_t MakeServerNonce(uint32_t clientId) {
    // RS2's NMT_Challenge body is a single 32-bit cookie (the on-wire Challenge is
    // a 40-bit bunch: NMT + DWORD). Return a 32-bit nonce.
    std::mt19937 rng(static_cast<uint32_t>(
        std::random_device{}() ^ (clientId * 2654435761u)));
    return rng();
}
} // namespace

HandshakeState::HandshakeState(uint32_t clientId,
                               RawSendFn rawSend,
                               ClientLoggedInCallback onLoggedIn,
                               ClientJoinedCallback onJoined)
    : m_clientId(clientId)
    , m_rawSend(std::move(rawSend))
    , m_onLoggedIn(std::move(onLoggedIn))
    , m_onJoined(std::move(onJoined))
{
    Logger::Debug("[HandshakeState] Created for client %u in state %s",
                  m_clientId, HandshakePhaseName(m_phase));
}

void HandshakeState::Emit(const std::vector<uint8_t>& payload) {
    if (m_rawSend) {
        m_rawSend(payload);
    } else {
        Logger::Trace("[HandshakeState] client %u: no raw-send callback set, dropping %zu byte payload",
                      m_clientId, payload.size());
    }
}

void HandshakeState::Reject(const std::string& errorKey, const char* reason) {
    Logger::Warn("[HandshakeState] client %u REJECTED (%s): sending Failure '%s'",
                 m_clientId, reason, errorKey.c_str());
    ControlChannel::FailureMessage fail;
    fail.errorKey = errorKey;
    Emit(ControlChannel::BuildFailure(fail));
    m_phase = HandshakePhase::Rejected;
}

void HandshakeState::HandleControlMessage(const uint8_t* data, size_t len) {
    if (m_phase == HandshakePhase::Rejected) {
        Logger::Debug("[HandshakeState] client %u: message after Rejected, ignoring", m_clientId);
        return;
    }
    if (!data || len == 0) {
        Logger::Debug("[HandshakeState] client %u: empty control message, ignoring", m_clientId);
        return;
    }

    // StatelessConnect handshake runs first. Until it completes, route by subtype.
    if (!m_controlHandshakeComplete) {
        HandleHandshakeMessage(data, len);
        return;
    }

    NMT type;
    if (!ControlChannel::PeekType(data, len, type)) {
        Logger::Debug("[HandshakeState] client %u: could not peek message type, ignoring", m_clientId);
        return;
    }

    Logger::Debug("[HandshakeState] client %u: control message type=0x%02X in state %s",
                  m_clientId, (unsigned)NMTByte(type), HandshakePhaseName(m_phase));

    switch (type) {
        case NMT::Hello:
            OnHello(data, len);
            break;
        case NMT::Netspeed:
            OnNetspeed(data, len);
            break;
        case NMT::Login:
            OnLogin(data, len);
            break;
        case NMT::Join:
        case NMT::JoinGuidRebind:
        case NMT::JoinSplit:
            OnJoin(data, len);
            break;
        case NMT::SteamAuth:
        case NMT::SteamLogin:
            HandleSteamAuthStub(type);
            break;
        default:
            // Unknown / unhandled control message: log and ignore gracefully.
            // NEVER crash on an unexpected type byte.
            Logger::Debug("[HandshakeState] client %u: unhandled control message type=0x%02X, ignoring",
                          m_clientId, (unsigned)NMTByte(type));
            break;
    }
}

void HandshakeState::HandleHandshakeMessage(const uint8_t* data, size_t len) {
    // Payload = [0x00 family][subtype][data]. Switch on the subtype byte (the 2nd
    // byte). We blind-accept: any nonce on the challenge, no validation of the
    // client's response. See ControlChannel::Handshake.
    if (len < 2) {
        Logger::Debug("[HandshakeState] client %u: short handshake msg (%zu bytes), ignoring", m_clientId, len);
        return;
    }
    const uint8_t subtype = data[1];
    switch (subtype) {
        case ControlChannel::Handshake::kStart: {  // 0x1d  C->S HandshakeStart
            m_handshakeNonce = MakeServerNonce(m_clientId);
            Emit(ControlChannel::BuildHandshakeChallenge(m_handshakeNonce));
            Logger::Info("[HandshakeState] client %u: HandshakeStart -> sent HandshakeChallenge (nonce=0x%06X)",
                         m_clientId, m_handshakeNonce & 0xFFFFFF);
            break;
        }
        case ControlChannel::Handshake::kResponse: {  // 0x1f  C->S HandshakeResponse
            // Accept blindly (no CRC32 validation - the server owns that choice).
            Emit(ControlChannel::BuildHandshakeComplete());
            m_controlHandshakeComplete = true;
            m_phase = HandshakePhase::AwaitingHello;
            Logger::Info("[HandshakeState] client %u: HandshakeResponse accepted -> sent HandshakeComplete; entering NMT phase",
                         m_clientId);
            break;
        }
        case ControlChannel::Handshake::kChallenge:   // 0x1e (server-sent; shouldn't arrive inbound)
        case ControlChannel::Handshake::kComplete:    // 0x20 (server-sent)
            Logger::Debug("[HandshakeState] client %u: ignoring inbound server-side handshake subtype 0x%02X",
                          m_clientId, subtype);
            break;
        default:
            Logger::Warn("[HandshakeState] client %u: unknown handshake subtype 0x%02X (len=%zu), ignoring",
                         m_clientId, subtype, len);
            break;
    }
}

void HandshakeState::OnHello(const uint8_t* data, size_t len) {
    if (m_phase != HandshakePhase::AwaitingHello) {
        Logger::Debug("[HandshakeState] client %u: duplicate/late Hello in state %s, ignoring",
                      m_clientId, HandshakePhaseName(m_phase));
        return;
    }

    // RS2's on-wire NMT_Hello is minimal (NMT + a single BYTE); the version /
    // SteamId / session fields the codec once assumed are NOT in this message
    // (they arrive later, e.g. in Login). So we do NOT require a full body parse
    // and do NOT gate on version here - the presence of NMT_Hello is enough to
    // issue the Challenge, which is what the real server does. Capture any
    // identity we CAN parse (best-effort); never reject a short Hello.
    ControlChannel::HelloMessage hello;
    if (ControlChannel::ParseHello(data, len, hello)) {
        m_steamId = hello.steamId;
        m_leechSessionId = hello.leechSessionId;
        Logger::Info("[HandshakeState] client %u: Hello { LE=%u MinVer=%d Ver=%d SteamId=%llu session='%s' }",
                     m_clientId, (unsigned)hello.bIsLittleEndian, hello.minVersion, hello.version,
                     (unsigned long long)hello.steamId, hello.leechSessionId.c_str());
    } else {
        Logger::Info("[HandshakeState] client %u: minimal Hello (%zu bytes), proceeding to Challenge",
                     m_clientId, len);
    }

    // Send Challenge (server nonce: a single 32-bit cookie).
    m_challenge = MakeServerNonce(m_clientId);
    ControlChannel::ChallengeMessage chal;
    chal.nonce = m_challenge;
    Emit(ControlChannel::BuildChallenge(chal));

    m_phase = HandshakePhase::ChallengeSent;
    Logger::Info("[HandshakeState] client %u: Hello accepted -> Challenge sent (nonce=0x%08X), state=%s",
                 m_clientId, m_challenge, HandshakePhaseName(m_phase));
}

void HandshakeState::OnNetspeed(const uint8_t* data, size_t len) {
    ControlChannel::NetspeedMessage ns;
    if (!ControlChannel::ParseNetspeed(data, len, ns)) {
        Logger::Warn("[HandshakeState] client %u: malformed Netspeed, ignoring", m_clientId);
        return;
    }
    int32_t requested = ns.netspeed;
    // Clamp to a sane range. Negative / zero falls back to the LAN default.
    if (requested <= 0) requested = kNetspeedLAN;
    if (requested > kMaxAcceptedNetspeed) requested = kMaxAcceptedNetspeed;
    m_netspeed = requested;
    Logger::Info("[HandshakeState] client %u: Netspeed requested=%d clamped=%d",
                 m_clientId, ns.netspeed, m_netspeed);
}

void HandshakeState::OnLogin(const uint8_t* data, size_t len) {
    if (m_phase != HandshakePhase::ChallengeSent &&
        m_phase != HandshakePhase::AwaitingLogin) {
        Logger::Debug("[HandshakeState] client %u: Login in unexpected state %s, ignoring",
                      m_clientId, HandshakePhaseName(m_phase));
        return;
    }

    ControlChannel::LoginMessage login;
    if (!ControlChannel::ParseLogin(data, len, login)) {
        Logger::Warn("[HandshakeState] client %u: malformed Login, ignoring", m_clientId);
        return;
    }

    // NOTE: ClientResponse (login.response) would be validated against the
    // Challenge nonce in a strict handshake. We accept blindly (stub) - the
    // point of this stream is the state machine + Game-facing event interface.
    m_loginOptions = URLOptions::Parse(login.url);

    Logger::Info("[HandshakeState] client %u: Login { url='%s' map='%s' name='%s' team=%d }",
                 m_clientId, login.url.c_str(), m_loginOptions.Map().c_str(),
                 m_loginOptions.PlayerName().c_str(), m_loginOptions.Team());

    CompleteLogin();
}

void HandshakeState::CompleteLogin() {
    if (m_phase != HandshakePhase::ChallengeSent &&
        m_phase != HandshakePhase::AwaitingLogin) {
        return; // already past login (e.g. a retransmitted Steam login)
    }
    // Send Welcome with placeholder map/gameclass (Stream B will make these real).
    ControlChannel::WelcomeMessage welcome;
    welcome.levelName = "VNTE-CuChi";                          // placeholder Map
    welcome.gameName  = "ROGame.ROGameInfoTerritories";        // placeholder GameClass
    welcome.flags = 0;                                         // spec §4 trailing QWORD
    Emit(ControlChannel::BuildWelcome(welcome));

    m_phase = HandshakePhase::WelcomeSent;
    Logger::Info("[HandshakeState] client %u: login complete -> Welcome sent, state=%s",
                 m_clientId, HandshakePhaseName(m_phase));

    // Fire the Game-facing ClientLoggedIn event (no direct Game/ dependency).
    if (m_onLoggedIn) {
        ClientLoggedInEvent ev;
        ev.clientId = m_clientId;
        ev.steamId = m_steamId;
        ev.leechSessionId = m_leechSessionId;
        ev.options = m_loginOptions;
        m_onLoggedIn(ev);
    }
}

void HandshakeState::OnJoin(const uint8_t* /*data*/, size_t /*len*/) {
    if (m_phase != HandshakePhase::WelcomeSent && m_phase != HandshakePhase::Joined) {
        Logger::Debug("[HandshakeState] client %u: Join in unexpected state %s, ignoring",
                      m_clientId, HandshakePhaseName(m_phase));
        return;
    }
    m_phase = HandshakePhase::Joined;
    Logger::Info("[HandshakeState] client %u: Join received -> Joined", m_clientId);

    if (m_onJoined) {
        ClientJoinedEvent ev;
        ev.clientId = m_clientId;
        m_onJoined(ev);
    }
}

void HandshakeState::HandleSteamAuthStub(NMT type) {
    // Steam auth (NMT_SteamAuth 0x12 / NMT_SteamLogin 0x10): accept blindly, no
    // ticket validation. In the RS2 EOS build this Steam-family message - not the
    // classic NMT_Login (0x05) - carries the client's login (the ~795B EOS auth
    // ticket). So when it arrives after the Challenge it IS the login: complete it
    // (send Welcome, fire ClientLoggedIn) exactly like OnLogin. The client
    // retransmits it; only the first (in ChallengeSent) advances.
    Logger::Info("[HandshakeState] client %u: Steam auth/login type=0x%02X accepted blindly (STUB)",
                 m_clientId, (unsigned)NMTByte(type));
    if (m_phase == HandshakePhase::ChallengeSent || m_phase == HandshakePhase::AwaitingLogin) {
        Logger::Info("[HandshakeState] client %u: treating Steam login as login completion -> Welcome", m_clientId);
        CompleteLogin();
    }
}
