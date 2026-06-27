// src/Protocol/ReverseEngineering/NetFieldTable.h
//
// Loader for the per-class UE3 net-field handle tables produced by the offline
// RE toolchain (tools/netfields_u_<Class>.txt, deployed to data/re/netfields/).
//
// Each table maps a replication HANDLE (the FieldNetIndex read off the wire with
// Bunch.ReadInt(GetMaxIndex())) to the property/function it identifies, plus the
// property's wire VALUE type. This turns the runtime decoder from "guess a
// byte layout" into "look up the known UE3 property for this handle and decode
// its value with the documented codec" (see docs/re/ue3_property_value_codec.md).
//
// Two on-disk formats are tolerated:
//   * RICH  — has a "# <Class>  maxHandle=N" header and a per-row type column
//             (e.g. ROTeamInfo, ROPlayerController, ROGameReplicationInfo).
//             These are the only classes whose VALUES can be decoded.
//   * LEAN  — handle/class/kind/name only, no type column (e.g. ROPawn). Handles
//             still resolve to NAMES, but values are undecodable (type Unknown).

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// Wire value encoding of a replicated property, derived from its declared type.
// Drives BunchPropertyDecoder. See ue3_property_value_codec.md §2-§4.
enum class NetValueType : uint8_t {
    Unknown = 0,        // type column absent or unrecognised -> value undecodable
    Bool,               // 1 bit
    Byte,               // 8 bits
    EnumByte,           // ceil(log2(NumEnums-1)) bits -- width unknown here
    Int,                // 32 bits LE
    Float,              // 32 bits IEEE-754 LE
    String,             // int32 len (incl NUL) + |len| ansi/ucs2 chars
    StructVector,       // FVector::SerializeCompressed
    StructRotator,      // FRotator::SerializeCompressed
    StructQuat,         // 3x float32
    StructPlane,        // 4x int16
    StructUniqueNetId,  // 1x uint64
    StructOther,        // members in field order -- not decodable without struct layout
    Object,             // UPackageMapLevel::SerializeObject
    Class,              // ditto (static class ref)
    DynArray            // NetSerializeItem no-op (0 value bits)
};

enum class NetPropKind : uint8_t { Unknown = 0, Prop, Func };

struct NetField {
    uint32_t     handle = 0;
    std::string  ownerClass;                 // declaring class, e.g. "Actor", "Pawn"
    NetPropKind  kind = NetPropKind::Unknown;
    std::string  name;                       // e.g. "Health"
    std::string  rawType;                    // e.g. "struct Vector", "byte(enum ENetRole)"
    NetValueType valueType = NetValueType::Unknown;
    int32_t      netIndex = -1;              // ni= value, informational
};

// One class's handle table.
class NetFieldTable {
public:
    // Parse a netfields_u_<Class>.txt file. classNameHint is used when the file
    // has no "# <Class>" header (LEAN format). Returns true if >=1 field parsed.
    bool LoadFromFile(const std::string& path, const std::string& classNameHint = "");

    const std::string& ClassName() const { return m_className; }

    // GetMaxIndex() — the ValueMax passed to Bunch.ReadInt() when reading a
    // handle (UnChan.cpp:1477). Header maxHandle when present, else maxSeen+1.
    uint32_t MaxIndex() const { return m_maxIndex; }

    bool            HasField(uint32_t handle) const { return m_fields.count(handle) != 0; }
    const NetField* GetField(uint32_t handle) const;
    size_t          Size() const { return m_fields.size(); }

    // True if any field carries a decodable value type (RICH file).
    bool HasValueTypes() const { return m_hasValueTypes; }

    // Classify a raw type string (the type column) into a NetValueType. Public +
    // static so it can be unit-tested directly.
    static NetValueType ClassifyType(const std::string& rawType, NetPropKind kind);

private:
    std::string m_className;
    uint32_t    m_maxIndex = 0;
    bool        m_hasValueTypes = false;
    std::unordered_map<uint32_t, NetField> m_fields;
};

// Registry of class tables loaded from a directory.
class NetFieldRegistry {
public:
    // Load every netfields_u_<Class>.txt in dir. Returns number of classes loaded.
    size_t LoadDirectory(const std::string& dir);

    const NetFieldTable* GetClass(const std::string& className) const;

    // All loaded class tables, in no particular order.
    std::vector<const NetFieldTable*> AllClasses() const;

    size_t ClassCount() const { return m_classes.size(); }

private:
    std::unordered_map<std::string, NetFieldTable> m_classes;
};
