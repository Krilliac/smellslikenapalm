// src/Generated/CodeGen.cpp

#include <fstream>
#include <sstream>
#include <filesystem>
#include "Protocol/PacketTypes.h"
#include "Protocol/ProtocolUtils.h"
#include "Utils/Logger.h"

int main(int argc, char** argv) {
    Logger::Initialize("");
    Logger::SetLevel(LogLevel::Trace);
    Logger::Trace("[CodeGen::main] Entering code generator main with argc=%d", argc);

    if (argc < 2) {
        Logger::Error("[CodeGen::main] Missing required argument: output directory. Usage: CodeGen <outDir>");
        return 1;
    }

    namespace fs = std::filesystem;
    fs::path outDir = fs::path(argv[1]); // e.g., "../src/Generated/Handlers"
    Logger::Info("[CodeGen::main] Output directory specified: '%s'", outDir.string().c_str());

    Logger::Debug("[CodeGen::main] Creating output directories if they don't exist...");
    fs::create_directories(outDir);
    Logger::Debug("[CodeGen::main] Output directory ensured: '%s'", outDir.string().c_str());

    int startType = static_cast<int>(PacketType::PT_HEARTBEAT);
    int maxType = static_cast<int>(PacketType::PT_MAX);
    Logger::Info("[CodeGen::main] Generating handler stubs for PacketType range [%d, %d)", startType, maxType);

    int generatedCount = 0;
    int skippedCount = 0;

    // Map PacketType enum to tag strings via ProtocolUtils
    for (int i = startType; i < maxType; ++i) {
        PacketType type = static_cast<PacketType>(i);
        std::string tag = ProtocolUtils::TypeToTag(type);
        Logger::Trace("[CodeGen::main] Processing PacketType %d -> tag='%s'", i, tag.c_str());

        if (tag.empty() || tag.rfind("CUSTOM_", 0) == 0) {
            Logger::Debug("[CodeGen::main] Skipping PacketType %d: tag is empty or starts with CUSTOM_ (tag='%s')", i, tag.c_str());
            skippedCount++;
            continue;
        }

        std::string fileBase = "Handler_" + tag;
        fs::path header = outDir / (fileBase + ".h");
        fs::path impl   = outDir / (fileBase + ".cpp");
        Logger::Debug("[CodeGen::main] Generating files: header='%s', impl='%s'", header.string().c_str(), impl.string().c_str());

        // Header
        Logger::Trace("[CodeGen::main] Writing header file for tag '%s'...", tag.c_str());
        std::ofstream h(header);
        if (!h.is_open()) {
            Logger::Error("[CodeGen::main] Failed to open header file for writing: '%s'", header.string().c_str());
            continue;
        }
        h << "// Auto-generated. Do not edit.\n"
             "#pragma once\n"
             "#include \"Utils/PacketAnalysis.h\"\n\n"
             "void Handle_" << tag << "(const PacketAnalysisResult& res);\n";
        h.close();
        Logger::Trace("[CodeGen::main] Header file written successfully: '%s'", header.string().c_str());

        // Implementation
        Logger::Trace("[CodeGen::main] Writing implementation file for tag '%s'...", tag.c_str());
        std::ofstream c(impl);
        if (!c.is_open()) {
            Logger::Error("[CodeGen::main] Failed to open implementation file for writing: '%s'", impl.string().c_str());
            continue;
        }
        c << "// Auto-generated. Do not edit.\n"
             "#include \"" << header.filename().string() << "\"\n\n"
             "void Handle_" << tag << "(const PacketAnalysisResult& res) {\n"
             "    // TODO: add handling logic for " << tag << "\n"
             "}\n";
        c.close();
        Logger::Trace("[CodeGen::main] Implementation file written successfully: '%s'", impl.string().c_str());

        generatedCount++;
    }

    Logger::Info("[CodeGen::main] Code generation complete: generated=%d handler stubs, skipped=%d, output_dir='%s'",
                 generatedCount, skippedCount, outDir.string().c_str());
    Logger::Trace("[CodeGen::main] Exiting code generator main with return code 0");
    Logger::Shutdown();
    return 0;
}
