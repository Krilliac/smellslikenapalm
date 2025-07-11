[![Ask DeepWiki](https://deepwiki.com/badge.svg)](https://deepwiki.com/Krilliac/smellslikenapalm)

RS2V Custom Server
A highly modular, extensible dedicated server for Rising Storm 2: Vietnam, featuring:

	* Cross-platform networking (TCP/UDP) with configurable timeouts, non-blocking modes, and protocol framing
	 
	* Comprehensive physics and collision subsystems

	* Robust anti-cheat: Easy Anti-Cheat emulation, memory scanning, executable validation, and enhanced cheat detection
	 
	* Secure authentication, Steam ticket validation, and configurable ban management
	 
	* Flexible replication pipeline (actor & property replication, RPCs, compression)
	 
	* Utility libraries: logging, file I/O, crypto, math, threading, profiling, timing, and more

Features:

Networking

	*Network/TCPSocket & UDPSocket wrappers

	*ProtocolHandler, MessageEncoder/Decoder, packet types & framing

	*UE3-style “bunch” protocol (UE3Protocol)

	*Ping/latency management, time synchronization

Game Systems

	*PhysicsEngine, CollisionDetection, Vector3 math

	*Input, movement, and command validation

Security & Anti-Cheat

	*Authentication, SteamAuth, BanManager, NetworkBlocker

	*Easy Anti-Cheat emulator, detector, memory scanner, enhanced EAC pipeline

	*FlexibleEACServer for runtime configurable cheat emulation

Utilities

	*Logger, FileUtils, CryptoUtils, MathUtils, StringUtils

	*ThreadPool, Timer, MemoryPool, PerformanceProfiler, RandomGenerator

	*Time management: GameClock, TickManager, TimeSync, TimeUtils

Build & Run
	1. Install dependencies

	*C++17 compiler (GCC/Clang/MSVC)

	*CMake ≥ 3.15

	*OpenSSL

	*On Windows: Winsock2 (built-in)
	
	2. Configure & Generate
	mkdir build && cd build
	cmake .. -DCMAKE_BUILD_TYPE=Release
	
	3. Build
	cmake --build . --config Release
	
	4. Run
	./rs2v_server \
	--config ../config.json \
	--port 7777
	
Configuration
	Edit config.json
	{
  "Server": {
    "Name": "RS2V Custom Server",
    "Port": 7777,
    "MaxPlayers": 64,
    "TickRate": 60,
    "LogFile": "server.log",
    "LogLevel": "INFO",
    "SecurityConfigFile": "security.json"
  }
}

And security.json to adjust anti-cheat, ban durations, and EAC settings.

Usage
	*Launch server with -h/--help or -v/--version flags.

	*In-game, connect via connect <server_ip>:7777.

	*Admin commands via chat or RCON (configured in Config/AdminConfig).

License
This project is licensed under the MIT License. See LICENSE for details.
