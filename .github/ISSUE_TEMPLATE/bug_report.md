---
name: Bug report
about: Create a report to help us improve
title: ''
labels: ''
assignees: ''

---

**Describe the bug**
A clear and concise description of what the bug is.

**To Reproduce**
Steps to reproduce the behavior:
1. Go to '...'
2. Click on '....'
3. Scroll down to '....'
4. See error

**Expected behavior**
A clear and concise description of what you expected to happen.

**Screenshots**
If applicable, add screenshots to help explain your problem.

**Desktop (please complete the following information):**
 - OS: [e.g. iOS]
 - Game version
 - Server version/Commit ID

**Additional context**
Add any other context about the problem here.

```markdown
---
name: “Server Bug Report”
about: “Report a reproducible issue in the RS2V Custom Server”
title: “[BUG] Short summary of the problem”
labels: bug
assignees: ''

---

**Description**  
Provide a clear and concise description of the issue you encountered in the RS2V Custom Server.

**Reproduction Steps**  
1. Start the RS2V server with this command:  
   ```
   rs2v_server --config configs/server.ini
   ```  
2. Connect a game client to `<server_ip>:<port>`.  
3. Perform action (e.g., “!changemap VTE-CuChi” in chat).  
4. Observe error or unexpected behavior in server log or client.

**Expected Behavior**  
Describe what you expected to happen instead (e.g., map changed, no crash, proper replication).

**Actual Behavior**  
Describe what actually happened (e.g., server crashed, network error, map not found).

**Server Environment**  
- **OS & Version:** (e.g., Ubuntu 22.04 LTS, Windows Server 2019)  
- **Build:** (e.g., v0.9.0-alpha, commit `abcdef1`)  
- **Tick Rate:** (e.g., 60 Hz)  
- **Telemetry Enabled:** (yes/no)  
- **Plugins Loaded:** (list C# and native plugin names)

**Logs & Diagnostics**  
Attach relevant excerpts from server.log, security.log, or telemetry snapshots. For example:  
```
[ERROR] [2025-07-12 20:45:33] Map 'VTE-CuChi' not found in Content/Maps.
```

**Configuration**  
Include any non-sensitive config overrides you used (omit passwords or API keys):  
```
[Server]
MaxPlayers=32
GameMode=Conquest

[Network]
CompressionEnabled=false
```

**Additional Context**  
Add any other context (e.g., recent commits, client version, network conditions, load-bot usage).

**Optional Attachments**  
- Screenshots of console or in-game errors  
- Packet capture snippet (`.pcap`) demonstrating malformed traffic  
- Core dump or crash stack trace  
```

This template guides reporters to include all essential server-specific details—environment, logs, config, and plugin context—ensuring maintainers can reproduce and resolve issues efficiently.
