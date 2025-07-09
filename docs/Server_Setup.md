# Server Setup Guide

This guide walks you through preparing, installing, configuring, and launching the RS2V Custom Server for production or local testing. Follow each step carefully to ensure a reliable deployment.

## 1. System Requirements

-  Operating System  
- Linux (Ubuntu 20.04+), Windows Server 2019+, or macOS 12+.  

-  Hardware  
- CPU: 4 cores minimum (8+ cores recommended for 60+ tick rate)  
- RAM: 8 GB minimum (16 GB+ recommended)  
- Disk: 10 GB free for binaries, logs, crash dumps  

-  Software  
- C++17 compiler (GCC 9+, Clang 10+, MSVC 2019+)  
- CMake ≥ 3.15  
- OpenSSL 1.1+  

## 2. Directory Layout

Assuming install path `/opt/rs2v_server` (Linux) or `C:\RS2VServer` (Windows):

```
/opt/rs2v_server/
├── bin/                   # Executable and DLLs
│   └── rs2v_server
├── config/                # All INI and TXT configuration files
│   ├── server.ini
│   ├── admin_commands.ini
│   ├── admin_list.txt
│   ├── ban_list.txt
│   ├── maps.ini
│   ├── game_modes.ini
│   ├── teams.ini
│   ├── weapons.ini
│   ├── motd.txt
│   ├── workshop_items.txt
│   └── auth_tokens.txt
├── data/                  # Game data: maps, assets, scripts
│   ├── maps/
│   ├── scripts/
│   └── players/
├── logs/                  # Runtime logs, rotating files
├── docs/                  # Documentation (API, guides)
└── scripts/               # Deployment & maintenance scripts
```

## 3. Installing the Server

1. Clone and build (development)  
   ```bash
   git clone https://github.com/Krilliac/smellslikenapalm.git
   cd smellslikenapalm
   mkdir build && cd build
   cmake .. -DCMAKE_BUILD_TYPE=Release
   cmake --build .
   ```

2. Deploy binaries (production)  
   ```bash
   # Linux example
   sudo mkdir -p /opt/rs2v_server/{bin,config,data,logs,scripts}
   sudo cp build/rs2v_server /opt/rs2v_server/bin/
   sudo cp -r config/* /opt/rs2v_server/config/
   sudo cp -r data/* /opt/rs2v_server/data/
   ```

3. Install dependencies  
   - Linux: `sudo apt-get install libssl-dev`  
   - Windows: Ensure OpenSSL DLLs adjacent to `rs2v_server.exe`  

## 4. Configuration Overview

1. **server.ini** (unified) controls all core settings: network, security, logging, performance, admin.  
2. **Dedicated INIs** (`maps.ini`, `game_modes.ini`, etc.) define game data.  
3. **Text files** (`admin_list.txt`, `ban_list.txt`, `motd.txt`) provide lists and messages.  

> Always validate your changes:  
> ```bash
> ./rs2v_server --config config/server.ini --validate-config
> ```

## 5. Running the Server

### 5.1 Basic Launch

```bash
/opt/rs2v_server/bin/rs2v_server --config /opt/rs2v_server/config/server.ini
```

### 5.2 Daemonize (Linux)

Use `systemd` unit:

```ini
# /etc/systemd/system/rs2v.service
[Unit]
Description=RS2V Custom Server
After=network.target

[Service]
Type=simple
WorkingDirectory=/opt/rs2v_server
ExecStart=/opt/rs2v_server/bin/rs2v_server --config config/server.ini
Restart=on-failure
User=rs2v
Group=rs2v

[Install]
WantedBy=multi-user.target
```
```bash
sudo systemctl enable rs2v
sudo systemctl start rs2v
sudo journalctl -u rs2v -f
```

### 5.3 Windows Service

Use `sc.exe` or NSSM to create a service pointing to `rs2v_server.exe` with `--config C:\RS2VServer\config\server.ini`.

## 6. Managing the Server

- **Configuration reload** (runtime):  
  Send SIGHUP (Linux) or `RCON reload` command to reload `server.ini` without restart.  

- **Admin commands**:  
  Use RCON port (default 27020) or in-chat commands if enabled (`/help`).

- **Log rotation**:  
  Logs rotate per `server.ini` settings (`log_max_size_mb`, `log_max_files`).

## 7. Security & Firewall

- Open TCP/UDP ports in firewall:  
  - Game port (default 7777)  
  - RCON port (default 27020)  

- Limit RCON access via IP allow-list or VPN.

## 8. Updating the Server

1. Pull new code or download release tarball.  
2. Build and stop the running service.  
3. Replace binaries in `bin/`, preserving `config/` and `data/`.  
4. Start the service and verify logs.

## 9. Troubleshooting

| Issue                                    | Resolution                                                   |
|------------------------------------------|--------------------------------------------------------------|
| Server crashes on start                  | Check `logs/server.log` for error stack; verify `server.ini`. |
| Players cannot connect                   | Confirm firewall opening for UDP/TCP port; check bind_address. |
| Admin commands not working               | Ensure `admin_list.txt` entries; verify `admin_rcon_only`.    |
| EAC errors or bans not applied           | Validate `Security.anti_cheat_mode` and `eac_scanner.json`.   |

## 10. Tips & Best Practices

- **Back up** your `config/` before major changes.  
- **Test** new configs on a local instance first.  
- **Monitor** performance with built-in profiler: set `Profiler.enable_profiling=true`.  
- **Automate** deployment via CI scripts in `scripts/` directory.

Follow this guide to ensure your RS2V Custom Server is installed, configured, and managed effectively. Enjoy hosting!