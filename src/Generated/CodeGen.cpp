// src/Generated/CodeGen.cpp

#include <fstream>
#include <sstream>
#include <filesystem>
#include "Protocol/PacketTypes.h"
#include "Utils/Logger.h"

int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    fs::path outDir = fs::path(argv[1]); // e.g., "../src/Generated/Handlers"
    fs::create_directories(outDir);

    // Map PacketType enum to tag strings via ProtocolUtils
    for (int i = static_cast<int>(PacketType::PT_HEARTBEAT);
             i < static_cast<int>(PacketType::PT_MAX); ++i) {
        PacketType type = static_cast<PacketType>(i);
        std::string tag = ProtocolUtils::TypeToTag(type);
        if (tag.empty() || tag.rfind("CUSTOM_", 0) == 0) continue;

        std::string fileBase = "Handler_" + tag;
        fs::path header = outDir / (fileBase + ".h");
        fs::path impl   = outDir / (fileBase + ".cpp");

        // Header
        std::ofstream h(header);
        h << "// Auto-generated. Do not edit.\n"
             "#pragma once\n"
             "#include \"Utils/PacketAnalysis.h\"\n\n"
             "void Handle_" << tag << "(const PacketAnalysisResult& res);\n";
        h.close();

        // Implementation
        std::ofstream c(impl);
        c << "// Auto-generated. Do not edit.\n"
             "#include \"" << header.filename().string() << "\"\n\n"
             "void Handle_" << tag << "(const PacketAnalysisResult& res) {\n"
             "    // TODO: add handling logic for " << tag << "\n"
             "}\n";
        c.close();
    }

    Logger::Info("Generated %d handler stubs in %s", 
                 static_cast<int>(PacketType::PT_MAX) - static_cast<int>(PacketType::PT_HEARTBEAT),
                 outDir.string().c_str());
    return 0;
}