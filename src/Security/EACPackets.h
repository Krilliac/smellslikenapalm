#pragma once

#include <cstdint>

#pragma pack(push,1)

/// EAC message types used between server and client
enum class EACMessageType : uint8_t
{
    DebugDrawCommand = 0x10,   // new: server → client
    MemoryRequest    = 0x11,   // new: server → client
    MemoryReply      = 0x12    // new: client → server
};

/// Operation codes for memory requests
enum class MemOp : uint8_t
{
    Read  = 0,  ///< Read len bytes from address
    Write = 1,  ///< Write len bytes to address (payload in data[])
    Alloc = 2   ///< Allocate len bytes with protection allocProtect
};

/// Packet sent from server to client to ask for a memory operation
struct MemoryRequestPacket
{
    uint8_t    type;            // = (uint8_t)EACMessageType::MemoryRequest
    MemOp      op;              // Read / Write / Alloc
    uint64_t   address;         // Address to read/write, or ignored for Alloc
    uint32_t   length;          // Number of bytes to read/write or allocate
    uint64_t   allocProtect;    // PAGE_* flags for Alloc (ignored on Read/Write)
    uint8_t    data[32];        // Payload for Write (up to 32 bytes; chunk if larger)
};

/// Packet sent from client back to server with memory operation result
struct MemoryReplyPacket
{
    uint8_t    type;            // = (uint8_t)EACMessageType::MemoryReply
    MemOp      op;              // Mirrors the request’s op
    uint64_t   address;         // Same address, or new base for Alloc
    uint32_t   length;          // Number of data bytes that follow
    // Immediately after this header, 'length' bytes of data follow in the UDP payload
    // uint8_t data[length];
};

#pragma pack(pop)