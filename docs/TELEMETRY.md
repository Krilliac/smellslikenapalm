# TELEMETRY.md — Telemetry System Guide

*Version 0.9.0-alpha · Last updated 2025-07-12*  
*Stability: 🚧 Under active development — APIs and file layouts may change.*

Comprehensive reference for the **Telemetry** subsystem: design, configuration, usage, and extension.  
For API details see [API.md](API.md). For deployment see [DEPLOYMENT.md](DEPLOYMENT.md).

## Contents

1. [Overview](#1-overview)  
2. [Architecture](#2-architecture)  
3. [Configuration](#3-configuration)  
4. [Initialization & Shutdown](#4-initialization--shutdown)  
5. [Sampling & Reporters](#5-sampling--reporters)  
6. [Custom Metrics](#6-custom-metrics)  
7. [Macro Helpers](#7-macro-helpers)  
8. [Extending Telemetry](#8-extending-telemetry)  
9. [Troubleshooting](#9-troubleshooting)  
10. [Future Roadmap](#10-future-roadmap)  

## 1. Overview

The Telemetry subsystem captures **system** (CPU, memory, network, disk) and **application** (players, packets, game events, security incidents) metrics at configurable intervals—persisting them to rotating JSON files, exposing a Prometheus endpoint, and/or storing in-memory for queries.

**Key Goals**  
- Low-overhead, thread-safe sampling  
- Pluggable reporters (File, Prometheus, CSV, Memory, Alerts)  
- Custom metric API for game logic and plugins  
- Real-time dashboards and alerting  

## 2. Architecture

```
┌──────────────────────────────────────────────────────┐
│                TelemetryManager (Singleton)         │
│ ┌───────────────────┐  ┌───────────────────────────┐ │
│ │  Sampling Thread  │  │  Custom Metrics Collector  │ │
│ └───────────────────┘  └───────────────────────────┘ │
│            │              │                          │
│            ▼              ▼                          │
│       CollectSnapshot()  CaptureCustomMetrics()     │
│            │              │                          │
│            └──────┬───────┘                          │
│                   ▼                                  │
│          ┌───────────────────┐                       │
│          │   Reporters[]     │                       │
│          │ ├─ FileReporter   │                       │
│          │ ├─ Prometheus     │                       │
│          │ ├─ MemoryReporter │                       │
│          │ ├─ CSVReporter    │                       │
│          │ └─ AlertReporter  │                       │
│          └───────────────────┘                       │
└──────────────────────────────────────────────────────┘
```

- **TelemetryManager**: core coordinator  
- **Sampling Thread**: wakes every `samplingInterval` to call `CollectSnapshot()`  
- **Custom Metrics Collector**: updated by game code each tick or event  
- **Reporters**: handle output/storage of each snapshot  

## 3. Configuration

All settings live in `configs/telemetry.ini` or under `[Telemetry]` in `server.ini`.

```ini
[Telemetry]
# Master enable switch
Enabled=true

# Sampling interval in milliseconds
SamplingInterval=1000

# Output directory for file reporter
MetricsDirectory=logs/telemetry

# FileReporter settings
EnableFileReporter=true
FilePrefix=metrics
MaxFileSizeBytes=10485760   ; 10 MB
MaxFiles=10
RotateIntervalMinutes=60
Compression=false

# PrometheusReporter settings
EnablePrometheusReporter=true
PrometheusPort=9100
MetricsEndpoint=/metrics
EnableTimestamps=false
ExcludeMetrics=

# MemoryReporter settings
EnableMemoryReporter=false
MaxSnapshotsInMemory=3600    ; 1 hour at 1s interval

# CSVReporter settings
EnableCSVReporter=false
CSVFilename=metrics.csv
IncludeHeaders=true
Delimiter=,

# AlertReporter settings
EnableAlertReporter=false
AlertCooldownSeconds=300     ; 5 minutes
```

## 4. Initialization & Shutdown

### 4.1 Initialize

```cpp
Telemetry::TelemetryManager& tm = Telemetry::TelemetryManager::Instance();
Telemetry::TelemetryConfig cfg;
cfg.enabled = true;
cfg.samplingInterval = std::chrono::milliseconds(1000);
cfg.metricsDirectory = "logs/telemetry";
cfg.enableFileReporter = true;
cfg.enablePrometheusReporter = true;
cfg.prometheusPort = 9100;

if (!tm.Initialize(cfg)) {
    Logger::Fatal("Telemetry initialization failed");
    return EXIT_FAILURE;
}

// Add standard reporters
auto reporters = Telemetry::ReporterFactory::CreateStandardReporters();
for (auto& r : reporters)
    tm.AddReporter(std::move(r));

// Start sampling loop
tm.StartSampling();
```

### 4.2 Shutdown

```cpp
// On server shutdown
Telemetry::TelemetryManager::Instance().Shutdown();
```

Stops sampling thread, flushes and shuts down all reporters.

## 5. Sampling & Reporters

### 5.1 Sampling

- **ForceSample()**: synchronous snapshot & report  
- **StartSampling()** / **StopSampling()**: control background thread  

### 5.2 Reporters

| Reporter                     | Output                                      | Config Key                     |
|------------------------------|---------------------------------------------|--------------------------------|
| **FileMetricsReporter**      | Rotating JSON files: `_.json` | `EnableFileReporter`  
| **PrometheusMetricsReporter**| HTTP endpoint at `/metrics`                 | `EnablePrometheusReporter`     |
| **MemoryMetricsReporter**    | In-memory ring buffer                       | `EnableMemoryReporter`         |
| **CSVMetricsReporter**       | CSV file rows                               | `EnableCSVReporter`            |
| **AlertMetricsReporter**     | Threshold-based alerts (webhook/email)      | `EnableAlertReporter`          |

**Factory Functions** in `ReporterFactory` simplify creation:

```cpp
auto fileRep = ReporterFactory::CreateFileReporter("metrics", 10*1024*1024, 10);
auto promRep = ReporterFactory::CreatePrometheusReporter(9100, "rs2v_server");
```

## 6. Custom Metrics

### 6.1 CustomMetrics API

```cpp
// Access the global instance:
auto& cm = Telemetry::TelemetryManager::Instance().GetCustomMetrics();

// Update counters
cm.network.totalPacketsProcessed++;
cm.gameplay.totalKills++;
cm.performance.frameTimeMs = measuredFrameTime;
cm.security.securityViolations++;
```

### 6.2 Snapshot Fields

`MetricsSnapshot` includes:

- **System**: cpuUsagePercent, memoryUsedBytes, networkBytesSent/Recv, diskRead/Write  
- **Network**: activeConnections, totalPacketsProcessed/Dropped, averageLatencyMs, packetLossRate  
- **Gameplay**: currentTick, activeMatches, totalKills/Deaths, objectivesCaptured, chatMessagesSent  
- **Performance**: frameTimeMs, physicsTimeMs, networkTimeMs, gameLogicTimeMs  
- **Security**: securityViolations, malformedPackets, speedHackDetections, kickedPlayers, bannedPlayers  

## 7. Macro Helpers

Simplify in-code metric updates:

```cpp
// Packet processing
TELEMETRY_INCREMENT_PACKETS_PROCESSED();
// Game events
TELEMETRY_INCREMENT_KILL();
// Latency update
TELEMETRY_UPDATE_LATENCY(latencyMs);
// Custom gauge
auto gauge = TELEMETRY_CREATE_GAUGE("custom_metric");
gauge.Increment();
```

Macros are defined in `TelemetryManager.h`.

## 8. Extending Telemetry

### 8.1 Custom Reporter

Implement `MetricsReporter`:

```cpp
class MyReporter : public Telemetry::MetricsReporter {
public:
  bool Initialize(const std::string& dir) override { /*…*/ }
  void Report(const Telemetry::MetricsSnapshot& s) override { /*…*/ }
  void Shutdown() override { /*…*/ }
  std::string GetReporterType() const override { return "MyReporter"; }
};
```

Add via:

```cpp
Telemetry::TelemetryManager::Instance().AddReporter(
    std::make_unique());
```

### 8.2 Custom Snapshot Processor

Subscribe to each snapshot:

```cpp
// In TelemetryManager after Initialize:
tm.SetSnapshotHandler([](const MetricsSnapshot& snap){
    // Custom processing, alerting, forwarding…
});
```

## 9. Troubleshooting

### 9.1 Telemetry Not Starting

- Check `Enabled=true` in config  
- Verify permissions on `MetricsDirectory`  
- Inspect logs for `TelemetryManager` errors  
- Ensure no port conflict on `PrometheusPort`  

### 9.2 No File Output

- Confirm `EnableFileReporter=true`  
- Check disk space and permissions  
- Examine `metrics_.json` creation times  

### 9.3 Prometheus 404

- Verify `EnablePrometheusReporter=true`  
- Ensure `StartSampling()` was called  
- Test with `curl http://localhost:9100/metrics`  
- Check firewall for port 9100  

## 10. Future Roadmap

| Feature                         | Status        | ETA        |
|---------------------------------|---------------|------------|
| In-memory time-series queries   | 🔲 Planned    | Q4 2025    |
| CSV export enhancements         | 🔄 In progress | Aug 2025   |
| Dynamic alert rule updates      | 🔲 Planned    | Q1 2026    |
| gRPC metrics streaming          | 🔲 Planned    | Q1 2026    |

For detailed tasks see [TODO.md](TODO.md).

*End of TELEMETRY.md*