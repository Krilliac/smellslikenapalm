// src/Network/HandshakeState.h
//
// Per-connection UE3 / RS2 control-channel HANDSHAKE STATE MACHINE.
//
// This drives a connecting client through the control-channel handshake using
// the message codec in ControlChannel.{h,cpp} (the new, tested codec - this file
// only USES it, never re-implements it). One HandshakeState instance lives per
// ClientConnection and is fed inbound control-channel messages via
// HandleControlMessage(); it emits outbound control messages through a raw-send
// callback and notifies the Game layer (without a Network->Game compile
// dependency) through the ConnectionManager observer interface.
//
// State flow (happy path):
//
//   AwaitingHello --Hello--> ChallengeSent --Login--> WelcomeSent --Join--> Joined
//                                  ^  |
//                          Netspeed |  (Netspeed may arrive any time after Hello;
//                                   '--  it does not change the primary state)
//
// Terminal failure: any state --reject--> Rejected (NMT_Failure already sent).
//
// Steam auth (NMT_SteamAuth / NMT_SteamLogin) is STUBBED: accepted blindly with
// no ticket validation (see HandleSteamAuthStub).

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "Network/NetMessages.h"
#include "Network/URLOptions.h"

// Forward-declared so this header has no dependency on the Game layer.
class ConnectionManager;

// ===========================================================================
//  Game-facing event payloads.
//
//  These are emitted by the handshake (via ConnectionManager's observer
//  callbacks) so the Game layer (Stream B) can react WITHOUT the Network layer
//  taking a compile dependency on Game/. The Game layer subscribes by handing
//  ConnectionManager a std::function; Network never #includes anything from
//  Game/.
// ===========================================================================

// Fired when a client completes Login (after Welcome is sent). The Game layer
// uses this to create the player-controller/PlayerState placeholder.
struct ClientLoggedInEvent {
    uint32_t    clientId = 0;
    uint64_t    steamId = 0;        // stubbed: accepted blindly from Hello
    std::string leechSessionId;     // stubbed: accepted blindly from Hello
    URLOptions  options;            // parsed FURL options from the Login URL
};

// Fired when a client sends NMT_Join. The Game layer uses this to actually
// spawn the player pawn into the world.
struct ClientJoinedEvent {
    uint32_t clientId = 0;
};

// Observer callback signatures exposed by ConnectionManager.
using ClientLoggedInCallback = std::function<void(const ClientLoggedInEvent&)>;
using ClientJoinedCallback   = std::function<void(const ClientJoinedEvent&)>;

// The control-channel handshake states.
enum class HandshakePhase {
    AwaitingHello,   // initial: expecting NMT_Hello
    ChallengeSent,   // Hello accepted, Challenge emitted, expecting Login
    AwaitingLogin,   // alias-ish state retained for clarity (== ChallengeSent semantics)
    WelcomeSent,     // Login accepted, Welcome emitted, ClientLoggedIn fired, expecting Join
    Joined,          // Join received, ClientJoined fired - handshake complete
    Rejected         // terminal failure; a Failure was sent and the connection is dead
};

const char* HandshakePhaseName(HandshakePhase p);

class HandshakeState {
public:
    // RawSendFn sends a control-channel message payload (raw bytes from a
    // ControlChannel::Build* call) back to the client. The state machine never
    // touches a socket directly; the owner wires this.
    using RawSendFn = std::function<void(const std::vector<uint8_t>& payload)>;

    // clientId identifies the owning connection for callbacks/logging.
    // rawSend emits outbound control bytes (see RawSendFn).
    // onLoggedIn / onJoined are the Game-facing notifications; the owning
    // ConnectionManager wires these to its observer dispatch (FireClientLoggedIn
    // / FireClientJoined). They may be null (e.g. in unit tests) - in that case
    // the notification is skipped but every other behaviour is unchanged.
    HandshakeState(uint32_t clientId,
                   RawSendFn rawSend,
                   ClientLoggedInCallback onLoggedIn = nullptr,
                   ClientJoinedCallback onJoined = nullptr);

    // Feed ONE parsed/raw control-channel message PAYLOAD (a `<BYTE NMT><fields>`
    // buffer, exactly what ControlChannel::Build* produces and Parse* consumes).
    // Drives the state machine. Never throws; malformed input is logged + ignored.
    void HandleControlMessage(const uint8_t* data, size_t len);

    // Convenience overload.
    void HandleControlMessage(const std::vector<uint8_t>& payload) {
        HandleControlMessage(payload.data(), payload.size());
    }

    // ---- State accessors (also used by the unit test) ----
    HandshakePhase Phase() const { return m_phase; }
    bool IsRejected() const { return m_phase == HandshakePhase::Rejected; }
    bool IsJoined() const { return m_phase == HandshakePhase::Joined; }

    // The (stubbed, accepted-blindly) identity captured from Hello.
    uint64_t SteamId() const { return m_steamId; }
    const std::string& LeechSessionId() const { return m_leechSessionId; }
    int32_t Netspeed() const { return m_netspeed; }

    // The server nonce sent in the Challenge (useful for the test).
    const std::string& Challenge() const { return m_challenge; }

    // The parsed login options (valid once WelcomeSent has been reached).
    const URLOptions& LoginOptions() const { return m_loginOptions; }

    // Maximum netspeed we will accept (clamp). Public for the test.
    static constexpr int32_t kMaxAcceptedNetspeed = 100000;

private:
    // Per-message handlers.
    void OnHello(const uint8_t* data, size_t len);
    void OnNetspeed(const uint8_t* data, size_t len);
    void OnLogin(const uint8_t* data, size_t len);
    void OnJoin(const uint8_t* data, size_t len);
    void HandleSteamAuthStub(NMT type);

    // Emit a Failure(errorKey) and move to Rejected.
    void Reject(const std::string& errorKey, const char* reason);

    // Emit a payload through the raw-send callback (no-op if unset).
    void Emit(const std::vector<uint8_t>& payload);

    uint32_t          m_clientId;
    RawSendFn         m_rawSend;
    ClientLoggedInCallback m_onLoggedIn;  // may be null
    ClientJoinedCallback   m_onJoined;    // may be null

    HandshakePhase    m_phase = HandshakePhase::AwaitingHello;

    // Captured identity / params (Steam fields are stubbed - accepted blindly).
    uint64_t          m_steamId = 0;
    std::string       m_leechSessionId;
    int32_t           m_netspeed = kNetspeedInternet;

    std::string       m_challenge;     // server nonce we sent
    URLOptions        m_loginOptions;  // parsed from the Login URL
};
