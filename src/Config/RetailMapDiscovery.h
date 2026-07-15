// src/Config/RetailMapDiscovery.h

#pragma once

#include <filesystem>
#include <string>
#include <vector>

// One installed retail map package that the emulator can activate without
// persisting machine-local Steam paths into config/maps.ini.
struct RetailMapPackage {
    std::string           mapName;
    std::string           displayName;
    std::string           defaultMode;
    std::filesystem::path packagePath;
};

namespace RetailMapDiscovery {

// Discover Steam itself from the platform's standard locations, then resolve
// the active Rising Storm 2 (app 418460) installation through Steam's library
// and app manifests. All returned paths are canonical, existing regular files
// contained by that installation's cooked Maps directory.
std::vector<RetailMapPackage> Discover();

// Deterministic seam for tests and embedding. When roots are supplied, only
// those Steam roots (and libraries declared by them) are inspected; platform
// registry/default probing is deliberately bypassed.
std::vector<RetailMapPackage> DiscoverFromSteamRoots(
    const std::vector<std::filesystem::path>& steamRoots);

} // namespace RetailMapDiscovery
