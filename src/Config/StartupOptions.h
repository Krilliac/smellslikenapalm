#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct StartupOptions {
    std::string configFile = "config/server.ini";
    std::string mapName;  // empty means use Game.initial_map
    std::uint16_t port = 0;  // zero means use the configured port
    std::uint16_t eacPort = 0;  // zero means use configured EAC.listen_port
    bool help = false;
    bool version = false;
    bool valid = true;
    std::string error;
};

// Pure command-line parser. `arguments` excludes argv[0], which keeps this
// testable without linking the server executable's main translation unit.
StartupOptions ParseStartupOptions(const std::vector<std::string>& arguments);
