# TROUBLESHOOTING.md — Common Issues & Solutions

This guide provides **diagnostic procedures**, **common issue resolutions**, and **troubleshooting workflows** for the RS2V Custom Server.  
For deployment procedures, see **DEPLOYMENT.md**. For security issues, see **SECURITY.md**.

## 1 · Quick Diagnostic Overview

### 1.1 Health Check Commands

```bash
# Server status
sudo systemctl status rs2v-server

# Process check
ps aux | grep rs2v_server

# Port status
netstat -tulpn | grep 7777
ss -ulpn | grep 7777

# Metrics endpoint
curl -s http://localhost:9100/metrics | head -20

# Disk space
df -h /opt/rs2v

# Memory usage
free -h
```

### 1.2 Log File Locations

| Log Type | Location | Purpose |
|----------|----------|---------|
| **Main Server** | `/opt/rs2v/logs/server.log` | General server events |
| **Security** | `/opt/rs2v/logs/security.log` | Authentication, violations |
| **Performance** | `/opt/rs2v/logs/performance.log` | Timing, bottlenecks |
| **Telemetry** | `/opt/rs2v/logs/telemetry/` | Metrics snapshots |
| **System (journald)** | `journalctl -u rs2v-server` | Service management |
| **EAC** | `/opt/rs2v/logs/eac.log` | Anti-cheat events |

## 2 · Server Won't Start

### 2.1 Common Startup Failures

| Error Pattern | Cause | Solution |
|---------------|-------|----------|
| `Permission denied` | File permissions | `sudo chown -R rs2v:rs2v /opt/rs2v` |
| `Port already in use` | Another process on 7777 | `sudo lsof -i :7777` and kill conflicting process |
| `Config file not found` | Missing configuration | Verify `/opt/rs2v/configs/server.ini` exists |
| `Steam API initialization failed` | Invalid Steam key | Check `STEAM_API_KEY` in config |
| `EAC initialization failed` | EAC service down | `sudo systemctl status eac` |

### 2.2 Detailed Diagnosis

#### 2.2.1 Configuration Issues

```bash
# Validate configuration syntax
/opt/rs2v/bin/rs2v_server --validate-config --config /opt/rs2v/configs/server.ini

# Check for missing required keys
grep -E "(Port|Name|MaxPlayers)" /opt/rs2v/configs/server.ini

# Verify file permissions
ls -la /opt/rs2v/configs/
```

#### 2.2.2 Dependency Issues

```bash
# Check library dependencies
ldd /opt/rs2v/bin/rs2v_server

# Verify .NET runtime (for scripting)
dotnet --version

# Check OpenSSL version
openssl version

# Verify zlib (for compression)
ldconfig -p | grep zlib
```

#### 2.2.3 Network Binding Issues

```bash
# Check what's using the port
sudo netstat -tulpn | grep :7777
sudo ss -tulpn | grep :7777

# Check if firewall is blocking
sudo ufw status
sudo iptables -L

# Test port binding manually
nc -ul 7777  # Should bind successfully
```

### 2.3 Startup Troubleshooting Script

```bash
#!/bin/bash
# scripts/diagnose_startup.sh

echo "=== RS2V Server Startup Diagnostics ==="

# Check if binary exists and is executable
if [[ ! -x /opt/rs2v/bin/rs2v_server ]]; then
    echo "❌ Binary missing or not executable"
    exit 1
fi

# Check configuration
if [[ ! -f /opt/rs2v/configs/server.ini ]]; then
    echo "❌ Configuration file missing"
    exit 1
fi

# Check port availability
if netstat -tulpn | grep -q ":7777"; then
    echo "❌ Port 7777 already in use:"
    netstat -tulpn | grep ":7777"
    exit 1
fi

# Check disk space
DISK_FREE=$(df /opt/rs2v | tail -1 | awk '{print $4}')
if [[ $DISK_FREE -lt 1048576 ]]; then  # Less than 1GB
    echo "⚠️  Low disk space: ${DISK_FREE}KB available"
fi

# Check memory
MEM_FREE=$(free | grep "Mem:" | awk '{print $7}')
if [[ $MEM_FREE -lt 1048576 ]]; then  # Less than 1GB
    echo "⚠️  Low memory: ${MEM_FREE}KB available"
fi

# Test configuration validation
if ! /opt/rs2v/bin/rs2v_server --validate-config --config /opt/rs2v/configs/server.ini 2>/dev/null; then
    echo "❌ Configuration validation failed"
    exit 1
fi

echo "✅ All startup checks passed"
```

## 3 · Performance Issues

### 3.1 High CPU Usage

#### 3.1.1 Symptoms
- CPU usage consistently >80%
- High server tick times
- Player lag and timeouts

#### 3.1.2 Diagnosis

```bash
# Monitor CPU usage by thread
top -H -p $(pgrep rs2v_server)

# Check tick timing
grep "Frame time" /opt/rs2v/logs/performance.log | tail -50

# Profile CPU usage
perf top -p $(pgrep rs2v_server)

# Check for runaway scripts
grep "Script execution" /opt/rs2v/logs/server.log
```

#### 3.1.3 Solutions

| Cause | Solution |
|-------|----------|
| **High player count** | Reduce `MaxPlayers` in config |
| **High tick rate** | Lower `TickRate` from 60 to 30 Hz |
| **Physics complexity** | Reduce physics quality settings |
| **Script loops** | Review and optimize C# scripts |
| **Memory pressure** | Increase system RAM |

```ini
# Performance tuning config
[Server]
TickRate=30
MaxPlayers=32

[Physics]
QualityLevel=Medium
UpdateFrequency=30

[Telemetry]
SamplingInterval=2000  # Less frequent sampling
```

### 3.2 Memory Issues

#### 3.2.1 Memory Leaks

```bash
# Monitor memory growth over time
while true; do
    ps -p $(pgrep rs2v_server) -o pid,vsz,rss,pcpu,etime
    sleep 60
done

# Check for memory leaks in logs
grep -i "memory" /opt/rs2v/logs/server.log

# Force garbage collection (if using scripts)
kill -USR1 $(pgrep rs2v_server)  # Send signal to trigger GC
```

#### 3.2.2 Out of Memory

```bash
# Check system memory
free -h
cat /proc/meminfo

# Check swap usage
swapon --show

# Monitor memory allocation
valgrind --tool=massif /opt/rs2v/bin/rs2v_server --config configs/server.ini
```

#### 3.2.3 Solutions

```bash
# Increase swap (temporary solution)
sudo fallocate -l 4G /swapfile
sudo chmod 600 /swapfile
sudo mkswap /swapfile
sudo swapon /swapfile

# Tune memory limits for scripts
echo 'vm.overcommit_memory=2' >> /etc/sysctl.conf
echo 'vm.overcommit_ratio=80' >> /etc/sysctl.conf
```

### 3.3 Network Performance

#### 3.3.1 High Latency

```bash
# Check network interface statistics
cat /proc/net/dev

# Monitor packet loss
ping -c 100 

# Check buffer sizes
sysctl net.core.rmem_max
sysctl net.core.wmem_max

# Monitor network congestion
ss -i

# Check for dropped packets
netstat -su | grep -i drop
```

#### 3.3.2 Packet Loss Solutions

```bash
# Increase network buffers
echo 'net.core.rmem_max = 134217728' >> /etc/sysctl.conf
echo 'net.core.wmem_max = 134217728' >> /etc/sysctl.conf
echo 'net.ipv4.udp_rmem_min = 8192' >> /etc/sysctl.conf
echo 'net.ipv4.udp_wmem_min = 8192' >> /etc/sysctl.conf

# Apply changes
sysctl -p

# Optimize network interface
ethtool -K eth0 gro off
ethtool -K eth0 tso off
```

## 4 · Connection Issues

### 4.1 Players Can't Connect

#### 4.1.1 Diagnosis Checklist

```bash
# 1. Check if server is running
systemctl is-active rs2v-server

# 2. Verify port is listening
netstat -ulpn | grep 7777

# 3. Test external connectivity
nmap -sU -p 7777 

# 4. Check firewall rules
ufw status numbered
iptables -L -n

# 5. Verify Steam authentication
curl -s "https://api.steampowered.com/ISteamWebAPIUtil/GetServerInfo/v1/" | jq .

# 6. Check server browser listing
# Test with game client or browser query tools
```

#### 4.1.2 Common Connection Failures

| Error Message | Cause | Solution |
|---------------|-------|----------|
| "Connection timeout" | Firewall blocking | Open UDP port 7777 |
| "Authentication failed" | Steam API issues | Check Steam API key |
| "Server full" | Max players reached | Increase `MaxPlayers` or wait |
| "Banned" | IP/Steam ban | Check ban list |
| "Version mismatch" | Client outdated | Update client |

### 4.2 Authentication Problems

#### 4.2.1 Steam Authentication Failures

```bash
# Check Steam API connectivity
curl -s "https://api.steampowered.com/ISteamUser/GetPlayerSummaries/v0002/?key=YOUR_KEY&steamids=76561197960435530"

# Verify API key format
grep "SteamAPIKey" /opt/rs2v/configs/server.ini

# Check authentication logs
grep "Authentication" /opt/rs2v/logs/security.log

# Test with known good Steam ID
/opt/rs2v/bin/rs2v_server --test-auth --steam-id 76561197960435530
```

#### 4.2.2 EAC Issues

```bash
# Check EAC service status
systemctl status eac

# Verify EAC configuration
cat /opt/rs2v/configs/eac.ini

# Check EAC logs
tail -f /opt/rs2v/logs/eac.log

# Test EAC connectivity
telnet eac-cdn.s3.amazonaws.com 443
```

### 4.3 Bandwidth Issues

```bash
# Monitor bandwidth usage
iftop -i eth0

# Check current connections
ss -tuln | grep 7777

# Monitor per-client bandwidth
tcpdump -i eth0 port 7777 -c 100

# Check bandwidth limits
cat /opt/rs2v/configs/server.ini | grep Bandwidth
```

## 5 · Security & Anti-Cheat Issues

### 5.1 False Positive Detections

#### 5.1.1 Movement Validation Issues

```bash
# Check movement violation logs
grep "Movement violation" /opt/rs2v/logs/security.log

# Review movement thresholds
grep -A 5 "\[Movement\]" /opt/rs2v/configs/security.ini

# Analyze specific player movement
grep "Player ID: 12345" /opt/rs2v/logs/security.log
```

#### 5.1.2 Tuning Anti-Cheat Sensitivity

```ini
# configs/security.ini - Relaxed settings
[Movement]
MaxSpeed=650.0              # Increased from 600.0
MaxAcceleration=1300.0      # Increased from 1200.0
TeleportThreshold=75.0      # Increased from 50.0
ValidationStrict=false      # Disabled strict mode

[Behavior]
SuspicionThreshold=9.0      # Increased from 8.5
RequiredViolations=5        # Increased from 3
WindowMinutes=10            # Increased window
```

### 5.2 EAC Problems

#### 5.2.1 EAC Service Issues

```bash
# Restart EAC service
sudo systemctl restart eac

# Check EAC configuration
/opt/eac/eac_server -validate

# Update EAC definitions
/opt/eac/eac_server -update

# Check EAC connectivity
telnet eac-api.easyanticheat.net 443
```

#### 5.2.2 EAC Integration Problems

```bash
# Verify EAC library
ldd /opt/rs2v/bin/rs2v_server | grep eac

# Check EAC initialization
grep "EAC" /opt/rs2v/logs/server.log

# Test EAC functionality
/opt/rs2v/bin/rs2v_server --test-eac
```

## 6 · Script and Plugin Issues

### 6.1 C# Script Problems

#### 6.1.1 Script Compilation Errors

```bash
# Check script compilation logs
grep "Script compilation" /opt/rs2v/logs/server.log

# Validate script syntax
/opt/rs2v/bin/rs2v_server --validate-scripts

# Check individual script
csc /opt/rs2v/plugins/myscript.csx /target:library
```

#### 6.1.2 Script Runtime Errors

```bash
# Monitor script execution
tail -f /opt/rs2v/logs/server.log | grep "Script"

# Check script memory usage
grep "Script memory" /opt/rs2v/logs/performance.log

# Test script manually
/opt/rs2v/bin/rs2v_server --test-script myscript.csx
```

#### 6.1.3 Script Performance Issues

```csharp
// Add performance monitoring to scripts
using System.Diagnostics;

void OnTick(float dt)
{
    var sw = Stopwatch.StartNew();
    
    // Your script logic here
    
    sw.Stop();
    if (sw.ElapsedMilliseconds > 5) // Log if >5ms
    {
        Log(LogLevel.WARN, $"Slow script execution: {sw.ElapsedMilliseconds}ms");
    }
}
```

### 6.2 Native Plugin Issues

```bash
# Check plugin loading
grep "Plugin" /opt/rs2v/logs/server.log

# Verify plugin dependencies
ldd /opt/rs2v/plugins/myplugin.so

# Test plugin manually
dlopen_test /opt/rs2v/plugins/myplugin.so
```

## 7 · Database and Data Issues

### 7.1 Configuration Corruption

```bash
# Backup current config
cp /opt/rs2v/configs/server.ini /opt/rs2v/configs/server.ini.backup

# Validate configuration syntax
/opt/rs2v/bin/rs2v_server --validate-config

# Reset to defaults
/opt/rs2v/bin/rs2v_server --generate-default-config

# Compare configs
diff /opt/rs2v/configs/server.ini.backup /opt/rs2v/configs/server.ini
```

### 7.2 Log File Issues

```bash
# Check log file permissions
ls -la /opt/rs2v/logs/

# Fix permissions
sudo chown -R rs2v:rs2v /opt/rs2v/logs/
sudo chmod 644 /opt/rs2v/logs/*.log

# Rotate large log files
logrotate -f /etc/logrotate.d/rs2v

# Clear old logs (be careful!)
find /opt/rs2v/logs/ -name "*.log" -mtime +30 -delete
```

## 8 · System-Level Issues

### 8.1 Resource Exhaustion

#### 8.1.1 File Descriptor Limits

```bash
# Check current limits
ulimit -n
cat /proc/$(pgrep rs2v_server)/limits

# Check usage
lsof -p $(pgrep rs2v_server) | wc -l

# Increase limits permanently
echo "rs2v soft nofile 65536" >> /etc/security/limits.conf
echo "rs2v hard nofile 65536" >> /etc/security/limits.conf
```

#### 8.1.2 Process Limits

```bash
# Check process count
ps --no-headers -eLf | grep rs2v | wc -l

# Check thread limits
cat /proc/sys/kernel/threads-max
cat /proc/sys/vm/max_map_count
```

### 8.2 Disk Space Issues

```bash
# Check disk usage
df -h /opt/rs2v
du -sh /opt/rs2v/logs/

# Clean up old files
find /opt/rs2v/logs/ -name "*.log.*" -mtime +7 -delete
find /opt/rs2v/logs/telemetry/ -name "*.json" -mtime +30 -delete

# Compress old logs
gzip /opt/rs2v/logs/*.log.*
```

### 8.3 Time Synchronization

```bash
# Check system time
timedatectl status

# Sync time
sudo ntpdate -s time.nist.gov

# Check for time drift in logs
grep "Time sync" /opt/rs2v/logs/server.log
```

## 9 · Recovery Procedures

### 9.1 Emergency Recovery

```bash
#!/bin/bash
# scripts/emergency_recovery.sh

echo "=== Emergency Recovery Procedure ==="

# 1. Stop the server
sudo systemctl stop rs2v-server

# 2. Kill any remaining processes
sudo pkill -f rs2v_server

# 3. Check for core dumps
ls -la /opt/rs2v/core.*

# 4. Backup current state
tar -czf /tmp/rs2v_emergency_$(date +%s).tar.gz /opt/rs2v/logs/ /opt/rs2v/configs/

# 5. Reset to known good configuration
cp /opt/rs2v/configs/server.ini.default /opt/rs2v/configs/server.ini

# 6. Clear problematic plugins
mv /opt/rs2v/plugins/ /opt/rs2v/plugins.disabled/

# 7. Start with minimal configuration
sudo systemctl start rs2v-server

# 8. Verify startup
sleep 10
if systemctl is-active --quiet rs2v-server; then
    echo "✅ Recovery successful"
else
    echo "❌ Recovery failed - manual intervention required"
fi
```

### 9.2 Configuration Rollback

```bash
#!/bin/bash
# scripts/config_rollback.sh

BACKUP_DIR="/opt/rs2v/configs/backups"
LATEST_BACKUP=$(ls -t $BACKUP_DIR/ | head -1)

if [[ -z "$LATEST_BACKUP" ]]; then
    echo "No backup found"
    exit 1
fi

echo "Rolling back to: $LATEST_BACKUP"

# Stop server
sudo systemctl stop rs2v-server

# Backup current config
cp /opt/rs2v/configs/server.ini /opt/rs2v/configs/server.ini.pre-rollback

# Restore backup
cp "$BACKUP_DIR/$LATEST_BACKUP" /opt/rs2v/configs/server.ini

# Restart server
sudo systemctl start rs2v-server

echo "Rollback complete"
```

## 10 · Monitoring and Alerting

### 10.1 Health Check Script

```bash
#!/bin/bash
# scripts/health_check.sh

ERRORS=0

# Check if server process is running
if ! pgrep -f rs2v_server > /dev/null; then
    echo "❌ Server process not running"
    ((ERRORS++))
fi

# Check if port is listening
if ! netstat -ulpn | grep -q ":7777"; then
    echo "❌ Game port not listening"
    ((ERRORS++))
fi

# Check metrics endpoint
if ! curl -sf http://localhost:9100/metrics > /dev/null 2>&1; then
    echo "❌ Metrics endpoint not responding"
    ((ERRORS++))
fi

# Check recent errors in logs
ERROR_COUNT=$(grep -c "ERROR" /opt/rs2v/logs/server.log | tail -100)
if [[ $ERROR_COUNT -gt 10 ]]; then
    echo "⚠️  High error count in recent logs: $ERROR_COUNT"
fi

# Check disk space
DISK_USAGE=$(df /opt/rs2v | tail -1 | awk '{print $5}' | sed 's/%//')
if [[ $DISK_USAGE -gt 90 ]]; then
    echo "⚠️  High disk usage: ${DISK_USAGE}%"
fi

# Check memory usage
MEM_USAGE=$(free | grep Mem | awk '{printf "%.1f", $3/$2 * 100.0}')
if (( $(echo "$MEM_USAGE > 90" | bc -l) )); then
    echo "⚠️  High memory usage: ${MEM_USAGE}%"
fi

if [[ $ERRORS -eq 0 ]]; then
    echo "✅ All health checks passed"
    exit 0
else
    echo "❌ $ERRORS critical issues found"
    exit 1
fi
```

### 10.2 Automated Monitoring

```bash
# Add to crontab for automated monitoring
# crontab -e
*/5 * * * * /opt/rs2v/scripts/health_check.sh >> /var/log/rs2v-health.log 2>&1

# Send alerts on failure
*/5 * * * * /opt/rs2v/scripts/health_check.sh || echo "RS2V Health Check Failed" | mail -s "RS2V Alert" admin@example.com
```

### 10.3 Log Analysis Tools

```bash
# Real-time log monitoring
tail -f /opt/rs2v/logs/server.log | grep --color=always -E "(ERROR|WARN|FATAL)"

# Performance analysis
awk '/Frame time/ {sum+=$4; count++} END {print "Average frame time:", sum/count "ms"}' /opt/rs2v/logs/performance.log

# Connection analysis
grep "Client connected" /opt/rs2v/logs/server.log | wc -l
grep "Client disconnected" /opt/rs2v/logs/server.log | wc -l

# Security event summary
grep "SECURITY" /opt/rs2v/logs/security.log | awk '{print $3}' | sort | uniq -c
```

## 11 · Getting Help

### 11.1 Information to Collect

When reporting issues, include:

```bash
# System information
uname -a
cat /etc/os-release
free -h
df -h

# Server information
/opt/rs2v/bin/rs2v_server --version
systemctl status rs2v-server

# Configuration
cat /opt/rs2v/configs/server.ini

# Recent logs (last 100 lines)
tail -100 /opt/rs2v/logs/server.log
tail -50 /opt/rs2v/logs/security.log

# Network status
netstat -tulpn | grep rs2v
ss -tulpn | grep 7777

# Process information
ps aux | grep rs2v
top -p $(pgrep rs2v_server) -n 1
```

### 11.2 Support Channels

| Issue Type | Contact Method | Response Time |
|------------|----------------|---------------|
| **Critical (server down)** | Discord #emergency | 15 minutes |
| **High (performance issues)** | GitHub Issues | 2-4 hours |
| **Medium (configuration help)** | Discord #help-and-questions | 24 hours |
| **Low (feature requests)** | GitHub Discussions | 1-2 weeks |

### 11.3 Debug Information Script

```bash
#!/bin/bash
# scripts/collect_debug_info.sh

DEBUG_DIR="/tmp/rs2v_debug_$(date +%s)"
mkdir -p "$DEBUG_DIR"

echo "Collecting debug information..."

# System info
uname -a > "$DEBUG_DIR/system_info.txt"
cat /etc/os-release >> "$DEBUG_DIR/system_info.txt"
free -h > "$DEBUG_DIR/memory_info.txt"
df -h > "$DEBUG_DIR/disk_info.txt"

# Server info
/opt/rs2v/bin/rs2v_server --version > "$DEBUG_DIR/server_version.txt" 2>&1
systemctl status rs2v-server > "$DEBUG_DIR/service_status.txt"

# Configuration (sanitized)
cp /opt/rs2v/configs/server.ini "$DEBUG_DIR/"
sed -i 's/Password=.*/Password=***REDACTED***/g' "$DEBUG_DIR/server.ini"
sed -i 's/APIKey=.*/APIKey=***REDACTED***/g' "$DEBUG_DIR/server.ini"

# Logs (recent only)
tail -1000 /opt/rs2v/logs/server.log > "$DEBUG_DIR/server.log"
tail -500 /opt/rs2v/logs/security.log > "$DEBUG_DIR/security.log"
journalctl -u rs2v-server --no-pager -n 500 > "$DEBUG_DIR/systemd.log"

# Network info
netstat -tulpn > "$DEBUG_DIR/network.txt"
ss -tulpn >> "$DEBUG_DIR/network.txt"

# Process info
ps aux | grep rs2v > "$DEBUG_DIR/processes.txt"
lsof -p $(pgrep rs2v_server) > "$DEBUG_DIR/open_files.txt" 2>/dev/null

# Create archive
tar -czf "/tmp/rs2v_debug_$(date +%s).tar.gz" -C /tmp "$DEBUG_DIR"

echo "Debug information collected: /tmp/rs2v_debug_*.tar.gz"
echo "Please attach this file to your support request."
```

**End of TROUBLESHOOTING.md**  
For additional help, join our Discord server or open an issue on GitHub with the debug information collected using the script above.