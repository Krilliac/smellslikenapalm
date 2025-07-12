# DEPLOYMENT.md — Production Deployment Guide

This guide covers **production deployment**, **security hardening**, **monitoring setup**, and **operational maintenance** for the RS2V Custom Server.  
For development workflow and API details, see **DEVELOPMENT.md** and **API.md**.

## 1 · Overview

The RS2V server is designed for **bare-metal**, **containerized**, or **cloud** deployment with enterprise-grade monitoring and security features.

| Deployment Type | Best For | Complexity |
|-----------------|----------|------------|
| **Docker Container** | Development, small-scale production | Low |
| **Systemd Service** | Dedicated Linux servers | Medium |
| **Kubernetes** | Auto-scaling, high availability | High |
| **Cloud Platforms** | AWS/GCP/Azure with managed services | Medium |

## 2 · System Requirements

### 2.1 Minimum Hardware

| Component | Specification | Notes |
|-----------|---------------|-------|
| **CPU** | 2 cores, 2.5 GHz | Intel/AMD x64 or ARM64 |
| **Memory** | 4 GB RAM | Base + 50 MB per concurrent player |
| **Storage** | 20 GB SSD | Logs, telemetry, and game data |
| **Network** | 100 Mbps | Symmetric upload/download |

### 2.2 Recommended Production

| Component | Specification | Rationale |
|-----------|---------------|-----------|
| **CPU** | 8+ cores, 3.0+ GHz | Physics simulation, concurrent players |
| **Memory** | 16+ GB RAM | Telemetry buffers, script caching |
| **Storage** | 100+ GB NVMe SSD | Fast I/O for logs and metrics |
| **Network** | 1+ Gbps | Low latency, high throughput |

### 2.3 Operating System Support

| OS | Version | Status | Notes |
|----|---------|--------|-------|
| **Ubuntu** | 20.04, 22.04 LTS | ✅ Recommended | Primary development platform |
| **CentOS/RHEL** | 8, 9 | ✅ Supported | Enterprise environments |
| **Rocky Linux** | 8, 9 | ✅ Supported | CentOS alternative |
| **Debian** | 11, 12 | ✅ Supported | Minimal installations |
| **Windows Server** | 2019, 2022 | 🔄 Beta | Limited testing |
| **macOS** | 12+ | 🔧 Development only | Not recommended for production |

## 3 · Installation Methods

### 3.1 Docker Deployment (Recommended)

#### 3.1.1 Quick Start

```bash
# Pull the latest image
docker pull ghcr.io/krilliac/rs2v-server:latest

# Run with basic configuration
docker run -d \
  --name rs2v-server \
  -p 7777:7777/udp \
  -p 9100:9100/tcp \
  -v $(pwd)/configs:/app/configs \
  -v $(pwd)/logs:/app/logs \
  -v $(pwd)/data:/app/data \
  ghcr.io/krilliac/rs2v-server:latest
```

#### 3.1.2 Production Docker Compose

```yaml
# docker-compose.yml
version: '3.8'

services:
  rs2v-server:
    image: ghcr.io/krilliac/rs2v-server:latest
    container_name: rs2v-server
    restart: unless-stopped
    ports:
      - "7777:7777/udp"
      - "9100:9100/tcp"
    volumes:
      - ./configs:/app/configs:ro
      - ./logs:/app/logs
      - ./data:/app/data
      - ./plugins:/app/plugins
    environment:
      - RUST_LOG=info
      - RS2V_CONFIG_PATH=/app/configs/server.ini
    healthcheck:
      test: ["CMD", "curl", "-f", "http://localhost:9100/metrics"]
      interval: 30s
      timeout: 10s
      retries: 3
    security_opt:
      - no-new-privileges:true
    cap_drop:
      - ALL
    cap_add:
      - NET_BIND_SERVICE
    networks:
      - rs2v-network

  prometheus:
    image: prom/prometheus:latest
    container_name: rs2v-prometheus
    restart: unless-stopped
    ports:
      - "9090:9090"
    volumes:
      - ./monitoring/prometheus.yml:/etc/prometheus/prometheus.yml:ro
      - prometheus_data:/prometheus
    command:
      - '--config.file=/etc/prometheus/prometheus.yml'
      - '--storage.tsdb.path=/prometheus'
      - '--web.console.libraries=/etc/prometheus/console_libraries'
      - '--web.console.templates=/etc/prometheus/consoles'
      - '--storage.tsdb.retention.time=200h'
      - '--web.enable-lifecycle'
    networks:
      - rs2v-network

  grafana:
    image: grafana/grafana:latest
    container_name: rs2v-grafana
    restart: unless-stopped
    ports:
      - "3000:3000"
    volumes:
      - grafana_data:/var/lib/grafana
      - ./monitoring/grafana/provisioning:/etc/grafana/provisioning
    environment:
      - GF_SECURITY_ADMIN_PASSWORD=your_secure_password
      - GF_USERS_ALLOW_SIGN_UP=false
    networks:
      - rs2v-network

volumes:
  prometheus_data:
  grafana_data:

networks:
  rs2v-network:
    driver: bridge
```

#### 3.1.3 Container Security

```dockerfile
# Dockerfile.production
FROM ubuntu:22.04 AS base
RUN apt-get update && apt-get install -y \
    ca-certificates \
    curl \
    && rm -rf /var/lib/apt/lists/*

# Create non-root user
RUN useradd -r -s /bin/false -m -d /app rs2v
USER rs2v
WORKDIR /app

COPY --chown=rs2v:rs2v ./bin/rs2v_server /app/
COPY --chown=rs2v:rs2v ./configs/ /app/configs/

EXPOSE 7777/udp 9100/tcp

HEALTHCHECK --interval=30s --timeout=10s --start-period=5s --retries=3 \
  CMD curl -f http://localhost:9100/metrics || exit 1

ENTRYPOINT ["./rs2v_server"]
CMD ["--config", "configs/server.ini"]
```

### 3.2 Systemd Service

#### 3.2.1 Service Installation

```bash
# 1. Create dedicated user
sudo useradd -r -s /bin/false -m -d /opt/rs2v rs2v

# 2. Install binary and configs
sudo mkdir -p /opt/rs2v/{bin,configs,logs,data,plugins}
sudo cp rs2v_server /opt/rs2v/bin/
sudo cp -r configs/* /opt/rs2v/configs/
sudo chown -R rs2v:rs2v /opt/rs2v

# 3. Create systemd service
sudo tee /etc/systemd/system/rs2v-server.service > /dev/null  /dev/null  80
        for: 5m
        labels:
          severity: warning
        annotations:
          summary: "High CPU usage detected"
          description: "CPU usage is above 80% for more than 5 minutes."

      - alert: HighMemoryUsage
        expr: rs2v_server_memory_usage_percent > 90
        for: 5m
        labels:
          severity: critical
        annotations:
          summary: "High memory usage detected"
          description: "Memory usage is above 90% for more than 5 minutes."

      - alert: SecurityViolations
        expr: increase(rs2v_server_security_violations_total[5m]) > 0
        for: 0m
        labels:
          severity: warning
        annotations:
          summary: "Security violations detected"
          description: "{{ $value }} security violations in the last 5 minutes."
```

## 7 · Backup and Recovery

### 7.1 Backup Strategy

```bash
#!/bin/bash
# scripts/backup.sh

BACKUP_DIR="/backup/rs2v"
DATE=$(date +%Y%m%d_%H%M%S)
RETENTION_DAYS=30

# Create backup directory
mkdir -p "$BACKUP_DIR"

# Backup configurations
tar -czf "$BACKUP_DIR/configs_$DATE.tar.gz" -C /opt/rs2v configs/

# Backup data (player stats, etc.)
tar -czf "$BACKUP_DIR/data_$DATE.tar.gz" -C /opt/rs2v data/

# Backup recent logs (last 7 days)
find /opt/rs2v/logs -type f -mtime -7 | tar -czf "$BACKUP_DIR/logs_$DATE.tar.gz" -T -

# Upload to cloud storage (example: AWS S3)
aws s3 sync "$BACKUP_DIR" s3://your-backup-bucket/rs2v/ --delete

# Cleanup old local backups
find "$BACKUP_DIR" -name "*.tar.gz" -mtime +$RETENTION_DAYS -delete

echo "Backup completed: $DATE"
```

### 7.2 Recovery Procedures

```bash
#!/bin/bash
# scripts/restore.sh

BACKUP_DATE=${1:-$(date +%Y%m%d)}
BACKUP_DIR="/backup/rs2v"

# Stop server
sudo systemctl stop rs2v-server

# Restore configurations
tar -xzf "$BACKUP_DIR/configs_$BACKUP_DATE.tar.gz" -C /opt/rs2v/

# Restore data
tar -xzf "$BACKUP_DIR/data_$BACKUP_DATE.tar.gz" -C /opt/rs2v/

# Fix permissions
sudo chown -R rs2v:rs2v /opt/rs2v

# Start server
sudo systemctl start rs2v-server

echo "Restore completed from backup: $BACKUP_DATE"
```

## 8 · Performance Tuning

### 8.1 System Optimization

```bash
# CPU performance governor
echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor

# Disable CPU mitigations for performance (security trade-off)
# Add to GRUB_CMDLINE_LINUX: mitigations=off

# Network buffer sizes
sudo sysctl -w net.core.rmem_max=134217728
sudo sysctl -w net.core.wmem_max=134217728

# Disable swap for consistent performance
sudo swapoff -a
```

### 8.2 Application Tuning

```ini
# configs/performance.ini
[Server]
TickRate=128  # Higher tick rate for competitive play

[Network]
MaxBandwidthMbps=1000.0
MaxPacketsPerTick=500
CompressionThreshold=256

[Telemetry]
SamplingInterval=500  # More frequent sampling
MaxSamplesInMemory=7200  # 1 hour at 500ms
```

## 9 · Troubleshooting

### 9.1 Common Issues

| Problem | Symptom | Solution |
|---------|---------|----------|
| **High CPU usage** | >80% constant | Check tick rate, reduce physics quality |
| **Memory leak** | RAM increases over time | Enable memory profiling, check script plugins |
| **Network packet loss** | High latency, disconnects | Check network buffers, firewall rules |
| **EAC auth failures** | Players can't connect | Verify EAC key, check EAC service status |

### 9.2 Log Analysis

```bash
# Real-time log monitoring
sudo journalctl -u rs2v-server -f

# Search for specific errors
sudo journalctl -u rs2v-server | grep -i error

# Performance metrics from logs
grep "Frame time" /opt/rs2v/logs/server.log | tail -100
```

### 9.3 Health Checks

```bash
#!/bin/bash
# scripts/health-check.sh

# Check if server is running
if ! systemctl is-active --quiet rs2v-server; then
    echo "ERROR: RS2V server is not running"
    exit 1
fi

# Check metrics endpoint
if ! curl -s http://localhost:9100/metrics > /dev/null; then
    echo "ERROR: Metrics endpoint not responding"
    exit 1
fi

# Check memory usage
MEM_USAGE=$(free | grep Mem | awk '{printf "%.1f", $3/$2 * 100.0}')
if (( $(echo "$MEM_USAGE > 90" | bc -l) )); then
    echo "WARNING: High memory usage: ${MEM_USAGE}%"
fi

# Check disk space
DISK_USAGE=$(df /opt/rs2v | tail -1 | awk '{print $5}' | sed 's/%//')
if [ "$DISK_USAGE" -gt 85 ]; then
    echo "WARNING: High disk usage: ${DISK_USAGE}%"
fi

echo "Health check passed"
```

## 10 · Maintenance Procedures

### 10.1 Regular Maintenance Tasks

| Task | Frequency | Command |
|------|-----------|---------|
| **Log rotation** | Daily | `logrotate /etc/logrotate.d/rs2v` |
| **Backup** | Daily | `./scripts/backup.sh` |
| **Security updates** | Weekly | `sudo apt update && sudo apt upgrade` |
| **Performance review** | Weekly | Check Grafana dashboards |
| **Config validation** | Monthly | `./rs2v_server --validate-config` |

### 10.2 Update Procedures

```bash
#!/bin/bash
# scripts/update.sh

# Create backup
./scripts/backup.sh

# Download new version
wget https://github.com/Krilliac/smellslikenapalm/releases/latest/download/rs2v_server -O /tmp/rs2v_server

# Stop server
sudo systemctl stop rs2v-server

# Replace binary
sudo cp /tmp/rs2v_server /opt/rs2v/bin/
sudo chmod +x /opt/rs2v/bin/rs2v_server
sudo chown rs2v:rs2v /opt/rs2v/bin/rs2v_server

# Validate configuration
/opt/rs2v/bin/rs2v_server --validate-config --config /opt/rs2v/configs/server.ini

# Start server
sudo systemctl start rs2v-server

# Verify startup
sleep 10
sudo systemctl status rs2v-server
```

## 11 · Cloud Platform Deployment

### 11.1 AWS Deployment

```yaml
# aws/cloudformation.yml
AWSTemplateFormatVersion: '2010-09-09'
Resources:
  RS2VSecurityGroup:
    Type: AWS::EC2::SecurityGroup
    Properties:
      GroupDescription: RS2V Server Security Group
      SecurityGroupIngress:
        - IpProtocol: udp
          FromPort: 7777
          ToPort: 7777
          CidrIp: 0.0.0.0/0
        - IpProtocol: tcp
          FromPort: 22
          ToPort: 22
          CidrIp: 0.0.0.0/0

  RS2VInstance:
    Type: AWS::EC2::Instance
    Properties:
      ImageId: ami-0c02fb55956c7d316  # Ubuntu 22.04 LTS
      InstanceType: c5.2xlarge
      SecurityGroupIds:
        - !Ref RS2VSecurityGroup
      UserData:
        Fn::Base64: !Sub |
          #!/bin/bash
          apt-get update
          apt-get install -y docker.io
          docker run -d --name rs2v-server \
            -p 7777:7777/udp \
            -p 9100:9100/tcp \
            ghcr.io/krilliac/rs2v-server:latest
```

### 11.2 Google Cloud Platform

```yaml
# gcp/deployment.yaml
apiVersion: apps/v1
kind: Deployment
metadata:
  name: rs2v-server
spec:
  replicas: 1
  selector:
    matchLabels:
      app: rs2v-server
  template:
    metadata:
      labels:
        app: rs2v-server
    spec:
      containers:
      - name: rs2v-server
        image: gcr.io/your-project/rs2v-server:latest
        ports:
        - containerPort: 7777
          protocol: UDP
        - containerPort: 9100
          protocol: TCP
        resources:
          requests:
            memory: "4Gi"
            cpu: "2000m"
          limits:
            memory: "8Gi"
            cpu: "4000m"
```

This comprehensive deployment guide covers all aspects of running the RS2V Custom Server in production environments, from basic Docker deployments to enterprise Kubernetes clusters with full monitoring and security hardening.