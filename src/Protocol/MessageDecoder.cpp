// src/Protocol/MessageDecoder.cpp
#include "Protocol/MessageDecoder.h"
#include "Utils/Logger.h"
#include "Math/Vector3.h"
#include "Physics/InputValidator.h"

bool MessageDecoder::Decode(const Packet& pkt, std::string& outTag, std::vector<uint8_t>& outPayload) {
    Logger::Trace("[MessageDecoder::Decode] entry — packet tag='%s'", pkt.GetTag().c_str());
    outTag = pkt.GetTag();
    try {
        outPayload = pkt.RawData();
        Logger::Debug("[MessageDecoder::Decode] successfully decoded payload of %zu bytes for tag='%s'",
                      outPayload.size(), outTag.c_str());
        Logger::Info("[MessageDecoder::Decode] decoded packet '%s' (%zu bytes payload)", outTag.c_str(), outPayload.size());
        Logger::Trace("[MessageDecoder::Decode] exit — returning true");
        return true;
    } catch (...) {
        Logger::Error("MessageDecoder: failed to decode packet '%s'", pkt.GetTag().c_str());
        Logger::Trace("[MessageDecoder::Decode] exit — returning false due to exception");
        return false;
    }
}

bool MessageDecoder::DecodeChatMessage(const Packet& pkt, uint32_t& outClientId, std::string& outText) {
    Logger::Trace("[MessageDecoder::DecodeChatMessage] entry — packet tag='%s'", pkt.GetTag().c_str());
    if (pkt.GetTag() != "CHAT_MESSAGE") {
        Logger::Debug("[MessageDecoder::DecodeChatMessage] tag mismatch — expected 'CHAT_MESSAGE', got '%s'",
                      pkt.GetTag().c_str());
        Logger::Trace("[MessageDecoder::DecodeChatMessage] exit — returning false (tag mismatch)");
        return false;
    }
    Packet copy = pkt;
    outClientId = copy.ReadUInt();
    outText = copy.ReadString();
    Logger::Debug("[MessageDecoder::DecodeChatMessage] read clientId=%u, raw text='%s'", outClientId, outText.c_str());
    outText = InputValidator::SanitizeChatMessage(outText);
    Logger::Debug("[MessageDecoder::DecodeChatMessage] sanitized text='%s'", outText.c_str());
    if (!InputValidator::IsValidChatMessage(outText)) {
        Logger::Warn("MessageDecoder: invalid chat text from client %u", outClientId);
        Logger::Trace("[MessageDecoder::DecodeChatMessage] exit — returning false (invalid chat text)");
        return false;
    }
    Logger::Info("[MessageDecoder::DecodeChatMessage] successfully decoded chat message from client %u, text length=%zu",
                 outClientId, outText.size());
    Logger::Trace("[MessageDecoder::DecodeChatMessage] exit — returning true");
    return true;
}

bool MessageDecoder::DecodeMovement(const Packet& pkt, uint32_t& outClientId, float& outX, float& outY, float& outZ) {
    Logger::Trace("[MessageDecoder::DecodeMovement] entry — packet tag='%s'", pkt.GetTag().c_str());
    if (pkt.GetTag() != "MOVE") {
        Logger::Debug("[MessageDecoder::DecodeMovement] tag mismatch — expected 'MOVE', got '%s'",
                      pkt.GetTag().c_str());
        Logger::Trace("[MessageDecoder::DecodeMovement] exit — returning false (tag mismatch)");
        return false;
    }
    Packet copy = pkt;
    outClientId = copy.ReadUInt();
    Vector3 pos = copy.ReadVector3();
    outX = pos.x; outY = pos.y; outZ = pos.z;
    Logger::Debug("[MessageDecoder::DecodeMovement] read clientId=%u, position=(%.3f, %.3f, %.3f)",
                  outClientId, outX, outY, outZ);
    if (!InputValidator::IsValidPosition(outX, outY, outZ)) {
        Logger::Warn("MessageDecoder: invalid position from client %u (%.1f,%.1f,%.1f)",
                     outClientId, outX, outY, outZ);
        Logger::Trace("[MessageDecoder::DecodeMovement] exit — returning false (invalid position)");
        return false;
    }
    Logger::Info("[MessageDecoder::DecodeMovement] successfully decoded movement from client %u to (%.3f, %.3f, %.3f)",
                 outClientId, outX, outY, outZ);
    Logger::Trace("[MessageDecoder::DecodeMovement] exit — returning true");
    return true;
}

bool MessageDecoder::DecodeAction(const Packet& pkt, uint32_t& outClientId,
                                  std::string& outAction, std::vector<std::string>& outArgs) {
    Logger::Trace("[MessageDecoder::DecodeAction] entry — packet tag='%s'", pkt.GetTag().c_str());
    if (pkt.GetTag() != "PLAYER_ACTION") {
        Logger::Debug("[MessageDecoder::DecodeAction] tag mismatch — expected 'PLAYER_ACTION', got '%s'",
                      pkt.GetTag().c_str());
        Logger::Trace("[MessageDecoder::DecodeAction] exit — returning false (tag mismatch)");
        return false;
    }
    Packet copy = pkt;
    outClientId = copy.ReadUInt();
    outAction = copy.ReadString();
    uint32_t argCount = copy.ReadUInt();
    Logger::Debug("[MessageDecoder::DecodeAction] read clientId=%u, action='%s', argCount=%u",
                  outClientId, outAction.c_str(), argCount);
    // SECURITY: argCount is a fully attacker-controlled uint32. The per-string reads
    // below are bounds-safe (ReadString returns "" past the end), but the LOOP itself
    // is bounded only by argCount — a hostile value like 0xFFFFFFFF would push billions
    // of empty strings into outArgs, exhausting memory (DoS) before any read fails.
    // Each argument is a length-prefixed string costing >= 4 bytes on the wire, so a
    // legitimate argCount can never exceed remaining/4. Reject anything larger.
    const size_t maxArgs = copy.BytesRemaining() / 4;
    if (argCount > maxArgs) {
        Logger::Warn("MessageDecoder: argCount %u exceeds max %zu for client %u — rejecting action",
                     argCount, maxArgs, outClientId);
        Logger::Trace("[MessageDecoder::DecodeAction] exit — returning false (argCount out of range)");
        return false;
    }
    outArgs.clear();
    outArgs.reserve(argCount);
    for (uint32_t i = 0; i < argCount; ++i) {
        std::string arg = copy.ReadString();
        Logger::Trace("[MessageDecoder::DecodeAction] read arg[%u]='%s'", i, arg.c_str());
        outArgs.push_back(arg);
    }
    if (!InputValidator::IsValidAction(outAction) ||
        !InputValidator::IsValidCommand(outAction, outArgs)) {
        Logger::Warn("MessageDecoder: invalid action '%s' from client %u", outAction.c_str(), outClientId);
        Logger::Trace("[MessageDecoder::DecodeAction] exit — returning false (invalid action/command)");
        return false;
    }
    Logger::Info("[MessageDecoder::DecodeAction] successfully decoded action '%s' from client %u with %u args",
                 outAction.c_str(), outClientId, argCount);
    Logger::Trace("[MessageDecoder::DecodeAction] exit — returning true");
    return true;
}
