// src/Protocol/ReverseEngineering/NetFieldTable.cpp

#include "Protocol/ReverseEngineering/NetFieldTable.h"
#include "Utils/Logger.h"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <filesystem>

namespace {

// Trim leading/trailing ASCII whitespace.
std::string Trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// Strip a leading UTF-8 BOM if present.
void StripBom(std::string& s) {
    if (s.size() >= 3 &&
        static_cast<unsigned char>(s[0]) == 0xEF &&
        static_cast<unsigned char>(s[1]) == 0xBB &&
        static_cast<unsigned char>(s[2]) == 0xBF) {
        s.erase(0, 3);
    }
}

std::vector<std::string> SplitWS(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream iss(s);
    std::string tok;
    while (iss >> tok) out.push_back(tok);
    return out;
}

bool ParseUInt(const std::string& s, uint32_t& out) {
    if (s.empty()) return false;
    for (char c : s) if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    out = static_cast<uint32_t>(std::strtoul(s.c_str(), nullptr, 10));
    return true;
}

bool ContainsCI(const std::string& hay, const char* needle) {
    std::string h = hay;
    std::transform(h.begin(), h.end(), h.begin(),
                   [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    return h.find(needle) != std::string::npos;
}

} // namespace

NetValueType NetFieldTable::ClassifyType(const std::string& rawTypeIn, NetPropKind kind) {
    std::string t = Trim(rawTypeIn);
    if (kind == NetPropKind::Func) return NetValueType::Unknown; // RPC params not handled
    if (t.empty() || t == "(rpc)") return NetValueType::Unknown;

    // Struct dispatch (order matters: check specific structs before generic).
    if (ContainsCI(t, "struct")) {
        if (ContainsCI(t, "vector"))      return NetValueType::StructVector;
        if (ContainsCI(t, "rotator"))     return NetValueType::StructRotator;
        if (ContainsCI(t, "quat"))        return NetValueType::StructQuat;
        if (ContainsCI(t, "plane"))       return NetValueType::StructPlane;
        if (ContainsCI(t, "uniquenetid")) return NetValueType::StructUniqueNetId;
        return NetValueType::StructOther;
    }
    if (ContainsCI(t, "array"))  return NetValueType::DynArray;
    if (ContainsCI(t, "class"))  return NetValueType::Class;   // class<...> / class
    if (ContainsCI(t, "obj<") || ContainsCI(t, "object")) return NetValueType::Object;
    if (ContainsCI(t, "byte")) {
        return ContainsCI(t, "enum") ? NetValueType::EnumByte : NetValueType::Byte;
    }
    if (ContainsCI(t, "bool"))   return NetValueType::Bool;
    if (ContainsCI(t, "float"))  return NetValueType::Float;
    if (ContainsCI(t, "string") || t == "str") return NetValueType::String;
    if (ContainsCI(t, "int"))    return NetValueType::Int;     // int, after the above
    return NetValueType::Unknown;
}

const NetField* NetFieldTable::GetField(uint32_t handle) const {
    auto it = m_fields.find(handle);
    return it == m_fields.end() ? nullptr : &it->second;
}

bool NetFieldTable::LoadFromFile(const std::string& path, const std::string& classNameHint) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        Logger::Warn("NetFieldTable: cannot open '%s'", path.c_str());
        return false;
    }

    m_fields.clear();
    m_className = classNameHint;
    m_maxIndex = 0;
    m_hasValueTypes = false;

    uint32_t headerMax = 0;
    bool haveHeaderMax = false;
    uint32_t maxHandleSeen = 0;

    std::string line;
    bool firstLine = true;
    while (std::getline(file, line)) {
        if (firstLine) { StripBom(line); firstLine = false; }
        // Drop a stray trailing CR (CRLF files).
        if (!line.empty() && line.back() == '\r') line.pop_back();

        std::string trimmed = Trim(line);
        if (trimmed.empty()) continue;

        // Header line: "# <Class>  maxHandle=N"
        if (trimmed[0] == '#') {
            auto toks = SplitWS(trimmed);
            if (toks.size() >= 2 && m_className.empty()) m_className = toks[1];
            for (const auto& tk : toks) {
                auto eq = tk.find("maxHandle=");
                if (eq != std::string::npos) {
                    uint32_t v;
                    if (ParseUInt(tk.substr(eq + 10), v)) { headerMax = v; haveHeaderMax = true; }
                }
            }
            continue;
        }

        auto toks = SplitWS(trimmed);
        if (toks.size() < 4) continue;

        // Data row begins with an integer handle; the column-header row ("h
        // class kind name ...") starts with "h" and is skipped by this check.
        uint32_t handle;
        if (!ParseUInt(toks[0], handle)) continue;

        NetField f;
        f.handle = handle;
        f.ownerClass = toks[1];
        if (toks[2] == "prop")      f.kind = NetPropKind::Prop;
        else if (toks[2] == "func") f.kind = NetPropKind::Func;
        else                        f.kind = NetPropKind::Unknown;
        f.name = toks[3];

        // Locate the trailing "ni=NNN" token; the type (if any) is everything
        // between the name and ni=.
        size_t niTok = toks.size();
        for (size_t i = 4; i < toks.size(); ++i) {
            if (toks[i].rfind("ni=", 0) == 0) {
                niTok = i;
                uint32_t v;
                if (ParseUInt(toks[i].substr(3), v)) f.netIndex = static_cast<int32_t>(v);
                break;
            }
        }
        if (niTok > 4) {
            std::string type;
            for (size_t i = 4; i < niTok; ++i) {
                if (!type.empty()) type += ' ';
                type += toks[i];
            }
            f.rawType = type;
        }

        // A fixed C-array type is written as "<base>[N]" (e.g. "byte[3]",
        // "int[11]", "obj<X>[16]"). Parse N -> arrayDim and classify the BASE
        // type; the wire carries one element per record (handle + element byte +
        // value), per ue3_property_value_codec.md §1.
        std::string classifyType = f.rawType;
        if (!classifyType.empty() && classifyType.back() == ']') {
            size_t lb = classifyType.rfind('[');
            if (lb != std::string::npos) {
                uint32_t dim = 0;
                if (ParseUInt(classifyType.substr(lb + 1, classifyType.size() - lb - 2), dim)
                    && dim > 0) {
                    f.arrayDim = dim;
                    classifyType = Trim(classifyType.substr(0, lb));
                }
            }
        }
        f.valueType = ClassifyType(classifyType, f.kind);
        if (f.valueType != NetValueType::Unknown) m_hasValueTypes = true;

        maxHandleSeen = std::max(maxHandleSeen, handle);
        m_fields[handle] = std::move(f);
    }

    if (m_fields.empty()) return false;

    m_maxIndex = haveHeaderMax ? headerMax : (maxHandleSeen + 1);
    // Guard: the handle read bound must cover every handle we catalogued.
    if (m_maxIndex <= maxHandleSeen) m_maxIndex = maxHandleSeen + 1;

    if (m_className.empty()) m_className = "Unknown";

    Logger::Info("NetFieldTable: loaded class '%s' (%zu fields, maxIndex=%u, types=%s)",
                 m_className.c_str(), m_fields.size(), m_maxIndex,
                 m_hasValueTypes ? "yes" : "no");
    return true;
}

size_t NetFieldRegistry::LoadDirectory(const std::string& dir) {
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) {
        Logger::Info("NetFieldRegistry: netfields dir '%s' not present — "
                     "property decoding disabled", dir.c_str());
        return 0;
    }

    size_t loaded = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        const std::string fname = entry.path().filename().string();
        // Expect netfields_u_<Class>.txt
        const std::string prefix = "netfields_u_";
        if (fname.rfind(prefix, 0) != 0) continue;
        if (entry.path().extension() != ".txt") continue;

        std::string cls = fname.substr(prefix.size());
        if (cls.size() > 4 && cls.substr(cls.size() - 4) == ".txt")
            cls = cls.substr(0, cls.size() - 4);

        NetFieldTable table;
        if (table.LoadFromFile(entry.path().string(), cls)) {
            m_classes[table.ClassName()] = std::move(table);
            ++loaded;
        }
    }

    Logger::Info("NetFieldRegistry: loaded %zu class tables from '%s'", loaded, dir.c_str());
    return loaded;
}

const NetFieldTable* NetFieldRegistry::GetClass(const std::string& className) const {
    auto it = m_classes.find(className);
    return it == m_classes.end() ? nullptr : &it->second;
}

std::vector<const NetFieldTable*> NetFieldRegistry::AllClasses() const {
    std::vector<const NetFieldTable*> out;
    out.reserve(m_classes.size());
    for (const auto& [name, table] : m_classes) out.push_back(&table);
    return out;
}
