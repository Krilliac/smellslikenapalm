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
std::string MakeServerNonce(uint32_t clientId) {
    static const char kHex[] = "0123456789ABCDEF";
    std::mt19937 rng(static_cast<uint32_t>(
        std::random_device{}() ^ (clientId * 2654435761u)));
    std::string s;
    s.reserve(16);
    for (int i = 0; i < 16; ++i) s.push_back(kHex[rng() & 0xF]);
    return s;
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

void HandshakeState::OnHello(const uint8_t* data, size_t len) {
    if (m_phase != HandshakePhase::AwaitingHello) {
        Logger::Debug("[HandshakeState] client %u: duplicate/late Hello in state %s, ignoring",
                      m_clientId, HandshakePhaseName(m_phase));
        return;
    }

    ControlChannel::HelloMessage hello;
    if (!ControlChannel::ParseHello(data, len, hello)) {
        Logger::Warn("[HandshakeState] client %u: malformed Hello, ignoring", m_clientId);
        return;
    }

    Logger::Info("[HandshakeState] client %u: Hello { LE=%u MinVer=%d Ver=%d SteamId=%llu session='%s' }",
                 m_clientId, (unsigned)hello.bIsLittleEndian, hello.minVersion, hello.version,
                 (unsigned long long)hello.steamId, hello.leechSessionId.c_str());

    // Version gate: reject clients older than our minimum net version. We also
    // send an Upgrade carrying our minimum so a compliant client can react.
    if (hello.version < kMinNetVersion) {
        Logger::Warn("[HandshakeState] client %u: version %d < kMinNetVersion %d, upgrading/rejecting",
                     m_clientId, hello.version, kMinNetVersion);
        ControlChannel::UpgradeMessage upg;
        upg.remoteMinVer = kMinNetVersion;
        Emit(ControlChannel::BuildUpgrade(upg));
        Reject(NetFailureKeys::SteamClientRequired, "client version too old");
        return;
    }

    // Accept identity BLINDLY (stub). No Steam ticket validation here.
    m_steamId = hello.steamId;
    m_leechSessionId = hello.leechSessionId;

    // Send Challenge (server nonce string).
    m_challenge = MakeServerNonce(m_clientId);
    ControlChannel::ChallengeMessage chal;
    chal.challenge = m_challenge;
    Emit(ControlChannel::BuildChallenge(chal));

    m_phase = HandshakePhase::ChallengeSent;
    Logger::Info("[HandshakeState] client %u: Hello accepted -> Challenge sent (nonce='%s'), state=%s",
                 m_clientId, m_challenge.c_str(), HandshakePhaseName(m_phase));
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

    // Send Welcome with placeholder map/gameclass (Stream B will make these real).
    ControlChannel::WelcomeMessage welcome;
    welcome.levelName = "VNTE-CuChi";                          // placeholder Map
    welcome.gameName  = "ROGame.ROGameInfoTerritories";        // placeholder GameClass
    welcome.redirectUrl = "";
    Emit(ControlChannel::BuildWelcome(welcome));

    m_phase = HandshakePhase::WelcomeSent;
    Logger::Info("[HandshakeState] client %u: Login accepted -> Welcome sent, state=%s",
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
    // Steam auth (NMT_SteamAuth / NMT_SteamLogin): accept blindly. No ticket
    // validation is performed - this is a deliberate stub for Stream A.
    Logger::Info("[HandshakeState] client %u: Steam auth message type=0x%02X accepted blindly (STUB, no validation)",
                 m_clientId, (unsigned)NMTByte(type));
}
