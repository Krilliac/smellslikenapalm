// src/Protocol/MessageDecoder.cpp
#include "Protocol/MessageDecoder.h"
#include "Utils/Logger.h"
#include "Math/Vector3.h"
#include "Input/InputValidator.h"

bool MessageDecoder::Decode(const Packet& pkt, std::string& outTag, std::vector<uint8_t>& outPayload) {
    outTag = pkt.GetTag();
    try {
        outPayload = pkt.RawData();
        return true;
    } catch (...) {
        Logger::Error("MessageDecoder: failed to decode packet '%s'", pkt.GetTag().c_str());
        return false;
    }
}

bool MessageDecoder::DecodeChatMessage(const Packet& pkt, uint32_t& outClientId, std::string& outText) {
    if (pkt.GetTag() != "CHAT_MESSAGE") return false;
    Packet copy = pkt;
    outClientId = copy.ReadUInt();
    outText = copy.ReadString();
    outText = InputValidator::SanitizeChatMessage(outText);
    if (!InputValidator::IsValidChatMessage(outText)) {
        Logger::Warn("MessageDecoder: invalid chat text from client %u", outClientId);
        return false;
    }
    return true;
}

bool MessageDecoder::DecodeMovement(const Packet& pkt, uint32_t& outClientId, float& outX, float& outY, float& outZ) {
    if (pkt.GetTag() != "MOVE") return false;
    Packet copy = pkt;
    outClientId = copy.ReadUInt();
    Vector3 pos = copy.ReadVector3();
    outX = pos.x; outY = pos.y; outZ = pos.z;
    if (!InputValidator::IsValidPosition(outX, outY, outZ)) {
        Logger::Warn("MessageDecoder: invalid position from client %u (%.1f,%.1f,%.1f)",
                     outClientId, outX, outY, outZ);
        return false;
    }
    return true;
}

bool MessageDecoder::DecodeAction(const Packet& pkt, uint32_t& outClientId,
                                  std::string& outAction, std::vector<std::string>& outArgs) {
    if (pkt.GetTag() != "PLAYER_ACTION") return false;
    Packet copy = pkt;
    outClientId = copy.ReadUInt();
    outAction = copy.ReadString();
    uint32_t argCount = copy.ReadUInt();
    outArgs.clear();
    for (uint32_t i = 0; i < argCount; ++i) {
        outArgs.push_back(copy.ReadString());
    }
    if (!InputValidator::IsValidAction(outAction) ||
        !InputValidator::IsValidCommand(outAction, outArgs)) {
        Logger::Warn("MessageDecoder: invalid action '%s' from client %u", outAction.c_str(), outClientId);
        return false;
    }
    return true;
}