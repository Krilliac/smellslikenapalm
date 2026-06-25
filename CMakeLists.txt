cmake_minimum_required(VERSION 3.15)
project(RS2VServer VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build options
option(ENABLE_TELEMETRY   "Enable telemetry system"       ON)
# NOTE: the C# scripting engine uses deprecated .NET COM hosting
# (mscorlib.tlb / ICorRuntimeHost) and does not currently build. Disabled by
# default until reworked (e.g. onto a supported hosting API). See src/Scripting.
option(ENABLE_SCRIPTING   "Enable C# scripting support"   OFF)
option(ENABLE_COMPRESSION "Enable packet compression"     ON)
option(BUILD_TESTS        "Build test suite"              OFF)

# Compiler flags
if(MSVC)
    add_compile_options(/W4 /permissive-)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_compile_options(-g -O0)
    else()
        add_compile_options(-O2)
    endif()
endif()

# Include paths — src/ holds both headers and sources
include_directories(${CMAKE_SOURCE_DIR}/src)

# Find dependencies
find_package(OpenSSL QUIET)
find_package(Threads REQUIRED)
find_package(ZLIB QUIET)

# Collect all source files
file(GLOB_RECURSE CORE_SOURCES
    src/Config/*.cpp
    src/Game/*.cpp
    src/Math/*.cpp
    src/Network/*.cpp
    src/Physics/*.cpp
    src/Protocol/*.cpp
    src/Security/*.cpp
    src/Time/*.cpp
    src/Utils/*.cpp
)

# Exclude CodeGen.cpp (standalone code generation tool with its own main)
list(FILTER CORE_SOURCES EXCLUDE REGEX "CodeGen\\.cpp$")

# Scripting sources (optional)
if(ENABLE_SCRIPTING)
    file(GLOB_RECURSE SCRIPTING_SOURCES src/Scripting/*.cpp)
    list(APPEND CORE_SOURCES ${SCRIPTING_SOURCES})
    add_compile_definitions(RS2V_SCRIPTING_ENABLED)
endif()

# Telemetry sources (optional)
if(ENABLE_TELEMETRY)
    file(GLOB_RECURSE TELEMETRY_SOURCES telemetry/*.cpp)
    list(APPEND CORE_SOURCES ${TELEMETRY_SOURCES})
    include_directories(${CMAKE_SOURCE_DIR}/telemetry)
    add_compile_definitions(RS2V_TELEMETRY_ENABLED)
endif()

# Core library — all sources the server uses MINUS src/main.cpp.
# Built once as a STATIC library so both rs2v_server and the test suite can
# link against it without recompiling every translation unit.
add_library(rs2v_core STATIC ${CORE_SOURCES})

# src/ is on the include path (set globally above via include_directories),
# but expose it transitively so test targets that link rs2v_core inherit it.
target_include_directories(rs2v_core PUBLIC ${CMAKE_SOURCE_DIR}/src)
if(ENABLE_TELEMETRY)
    target_include_directories(rs2v_core PUBLIC ${CMAKE_SOURCE_DIR}/telemetry)
endif()

target_link_libraries(rs2v_core PUBLIC Threads::Threads)

# Platform-specific link libraries (PUBLIC so consumers inherit them)
if(WIN32)
    target_link_libraries(rs2v_core PUBLIC ws2_32 iphlpapi pdh psapi)
else()
    target_link_libraries(rs2v_core PUBLIC dl)
endif()

# OpenSSL (optional — ProtocolUtils uses it for Base64)
if(OpenSSL_FOUND)
    target_link_libraries(rs2v_core PUBLIC OpenSSL::SSL OpenSSL::Crypto)
    target_compile_definitions(rs2v_core PUBLIC RS2V_HAS_OPENSSL)
else()
    message(STATUS "OpenSSL not found — Base64 will use built-in implementation")
endif()

# zlib (optional — CompressionHandler uses it)
if(ZLIB_FOUND)
    target_link_libraries(rs2v_core PUBLIC ZLIB::ZLIB)
    target_compile_definitions(rs2v_core PUBLIC RS2V_HAS_ZLIB)
else()
    message(STATUS "zlib not found — compression will use built-in stub")
endif()

# Filesystem support (GCC < 9 needs explicit linkage)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.0)
    target_link_libraries(rs2v_core PUBLIC stdc++fs)
endif()

# Main server executable — just main.cpp + the core library
add_executable(rs2v_server src/main.cpp)
target_link_libraries(rs2v_server PRIVATE rs2v_core)

# Install target
install(TARGETS rs2v_server RUNTIME DESTINATION bin)
install(DIRECTORY config/ DESTINATION etc/rs2v)
install(DIRECTORY data/ DESTINATION share/rs2v/data)

message(STATUS "")
message(STATUS "RS2V Server v${PROJECT_VERSION}")
message(STATUS "  Build type:    ${CMAKE_BUILD_TYPE}")
message(STATUS "  Telemetry:     ${ENABLE_TELEMETRY}")
message(STATUS "  Scripting:     ${ENABLE_SCRIPTING}")
message(STATUS "  Compression:   ${ENABLE_COMPRESSION}")
message(STATUS "  OpenSSL:       ${OpenSSL_FOUND}")
message(STATUS "  zlib:          ${ZLIB_FOUND}")
message(STATUS "")

# Test suite (opt-in via -DBUILD_TESTS=ON)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
