// Server/telemetry/AlertMetricsReporter.cpp
// Implementation of alert-based metrics reporter for threshold monitoring
// and factory functions for alert reporter and standard reporter sets

#include "MetricsReporter.h"
#include "TelemetryManager.h"
#include "Utils/Logger.h"

#include <algorithm>
#include <sstream>
#include <iomanip>
#include <cmath>

namespace Telemetry {

AlertMetricsReporter::AlertMetricsReporter(const AlertReporterConfig& config)
    : m_config(config), m_reportsGenerated(0) {
    Logger::Trace("[AlertMetricsReporter::AlertMetricsReporter] Entry: rules=%zu, emailAlerts=%s",
                  config.rules.size(), config.enableEmailAlerts ? "true" : "false");

    Logger::Info("AlertMetricsReporter created with %zu alert rules", m_config.rules.size());
    Logger::Trace("[AlertMetricsReporter::AlertMetricsReporter] Exit");
}

bool AlertMetricsReporter::Initialize(const std::string& outputDirectory) {
    Logger::Trace("[AlertMetricsReporter::Initialize] Entry: outputDirectory='%s'", outputDirectory.c_str());
    std::lock_guard<std::mutex> lock(m_alertMutex);

    // Log all configured rules
    for (const auto& rule : m_config.rules) {
        const char* opStr = "UNKNOWN";
        switch (rule.op) {
            case AlertReporterConfig::AlertRule::GREATER_THAN: opStr = "GREATER_THAN"; break;
            case AlertReporterConfig::AlertRule::LESS_THAN:    opStr = "LESS_THAN"; break;
            case AlertReporterConfig::AlertRule::EQUAL:        opStr = "EQUAL"; break;
            case AlertReporterConfig::AlertRule::NOT_EQUAL:    opStr = "NOT_EQUAL"; break;
        }
        Logger::Info("[AlertMetricsReporter::Initialize] Rule '%s': metric='%s', op=%s, threshold=%.4f, cooldown=%llds",
                     rule.name.c_str(), rule.metricPath.c_str(), opStr, rule.threshold,
                     (long long)rule.cooldownPeriod.count());
    }

    // Clear any previous state
    m_lastAlertTimes.clear();
    m_activeAlerts.clear();

    Logger::Info("AlertMetricsReporter initialized with %zu rules", m_config.rules.size());
    Logger::Trace("[AlertMetricsReporter::Initialize] Exit: returning true");
    return true;
}

void AlertMetricsReporter::Shutdown() {
    Logger::Trace("[AlertMetricsReporter::Shutdown] Entry");
    std::lock_guard<std::mutex> lock(m_alertMutex);

    size_t activeCount = m_activeAlerts.size();
    size_t cooldownCount = m_lastAlertTimes.size();

    m_lastAlertTimes.clear();
    m_activeAlerts.clear();

    Logger::Info("AlertMetricsReporter shutdown complete. Generated %llu reports, cleared %zu active alerts and %zu cooldown entries.",
                m_reportsGenerated.load(), activeCount, cooldownCount);
    Logger::Trace("[AlertMetricsReporter::Shutdown] Exit");
}

void AlertMetricsReporter::Report(const MetricsSnapshot& snapshot) {
    Logger::Trace("[AlertMetricsReporter::Report] Entry");

    EvaluateRules(snapshot);
    m_reportsGenerated.fetch_add(1, std::memory_order_relaxed);

    Logger::Debug("[AlertMetricsReporter::Report] Report #%llu evaluated against %zu rules",
                  m_reportsGenerated.load(), m_config.rules.size());
    Logger::Trace("[AlertMetricsReporter::Report] Exit");
}

void AlertMetricsReporter::AddAlertRule(const AlertReporterConfig::AlertRule& rule) {
    Logger::Trace("[AlertMetricsReporter::AddAlertRule] Entry: name='%s'", rule.name.c_str());
    std::lock_guard<std::mutex> lock(m_alertMutex);

    // Check for duplicate rule name and remove if found
    auto removeIt = std::remove_if(m_config.rules.begin(), m_config.rules.end(),
        [&rule](const AlertReporterConfig::AlertRule& existing) {
            return existing.name == rule.name;
        });
    if (removeIt != m_config.rules.end()) {
        Logger::Warn("[AlertMetricsReporter::AddAlertRule] Rule with name '%s' already exists, replacing",
                     rule.name.c_str());
        m_config.rules.erase(removeIt, m_config.rules.end());
        m_lastAlertTimes.erase(rule.name);
        auto activeIt = std::find(m_activeAlerts.begin(), m_activeAlerts.end(), rule.name);
        if (activeIt != m_activeAlerts.end()) {
            m_activeAlerts.erase(activeIt);
        }
    }

    m_config.rules.push_back(rule);

    const char* opStr = "UNKNOWN";
    switch (rule.op) {
        case AlertReporterConfig::AlertRule::GREATER_THAN: opStr = "GREATER_THAN"; break;
        case AlertReporterConfig::AlertRule::LESS_THAN:    opStr = "LESS_THAN"; break;
        case AlertReporterConfig::AlertRule::EQUAL:        opStr = "EQUAL"; break;
        case AlertReporterConfig::AlertRule::NOT_EQUAL:    opStr = "NOT_EQUAL"; break;
    }
    Logger::Info("[AlertMetricsReporter::AddAlertRule] Added rule '%s': metric='%s', op=%s, threshold=%.4f",
                 rule.name.c_str(), rule.metricPath.c_str(), opStr, rule.threshold);
    Logger::Trace("[AlertMetricsReporter::AddAlertRule] Exit");
}

void AlertMetricsReporter::RemoveAlertRule(const std::string& name) {
    Logger::Trace("[AlertMetricsReporter::RemoveAlertRule] Entry: name='%s'", name.c_str());
    std::lock_guard<std::mutex> lock(m_alertMutex);

    auto it = std::remove_if(m_config.rules.begin(), m_config.rules.end(),
        [&name](const AlertReporterConfig::AlertRule& rule) {
            return rule.name == name;
        });

    if (it != m_config.rules.end()) {
        m_config.rules.erase(it, m_config.rules.end());
        Logger::Info("[AlertMetricsReporter::RemoveAlertRule] Removed rule '%s'", name.c_str());

        // Also remove from cooldown tracking
        m_lastAlertTimes.erase(name);

        // Remove from active alerts if present
        auto activeIt = std::find(m_activeAlerts.begin(), m_activeAlerts.end(), name);
        if (activeIt != m_activeAlerts.end()) {
            m_activeAlerts.erase(activeIt);
            Logger::Debug("[AlertMetricsReporter::RemoveAlertRule] Also removed '%s' from active alerts", name.c_str());
        }
    } else {
        Logger::Warn("[AlertMetricsReporter::RemoveAlertRule] Rule '%s' not found", name.c_str());
    }

    Logger::Trace("[AlertMetricsReporter::RemoveAlertRule] Exit");
}

std::vector<std::string> AlertMetricsReporter::GetActiveAlerts() const {
    Logger::Trace("[AlertMetricsReporter::GetActiveAlerts] Entry");
    std::lock_guard<std::mutex> lock(m_alertMutex);
    Logger::Debug("[AlertMetricsReporter::GetActiveAlerts] Returning %zu active alerts", m_activeAlerts.size());
    Logger::Trace("[AlertMetricsReporter::GetActiveAlerts] Exit: returning %zu alerts", m_activeAlerts.size());
    return m_activeAlerts;
}

void AlertMetricsReporter::ClearActiveAlerts() {
    Logger::Trace("[AlertMetricsReporter::ClearActiveAlerts] Entry");
    std::lock_guard<std::mutex> lock(m_alertMutex);
    size_t count = m_activeAlerts.size();
    m_activeAlerts.clear();
    Logger::Info("[AlertMetricsReporter::ClearActiveAlerts] Cleared %zu active alerts", count);
    Logger::Trace("[AlertMetricsReporter::ClearActiveAlerts] Exit");
}

double AlertMetricsReporter::ExtractMetricValue(const MetricsSnapshot& snapshot, const std::string& metricPath) const {
    Logger::Trace("[AlertMetricsReporter::ExtractMetricValue] Entry: metricPath='%s'", metricPath.c_str());

    double value = 0.0;

    // System metrics
    if (metricPath == "cpuUsagePercent") {
        value = snapshot.cpuUsagePercent;
    } else if (metricPath == "memoryUsedBytes") {
        value = static_cast<double>(snapshot.memoryUsedBytes);
    } else if (metricPath == "memoryTotalBytes") {
        value = static_cast<double>(snapshot.memoryTotalBytes);
    } else if (metricPath == "networkBytesSent") {
        value = static_cast<double>(snapshot.networkBytesSent);
    } else if (metricPath == "networkBytesReceived") {
        value = static_cast<double>(snapshot.networkBytesReceived);
    } else if (metricPath == "diskReadBytes") {
        value = static_cast<double>(snapshot.diskReadBytes);
    } else if (metricPath == "diskWriteBytes") {
        value = static_cast<double>(snapshot.diskWriteBytes);
    }
    // Application metrics - Game Server
    else if (metricPath == "activeConnections") {
        value = static_cast<double>(snapshot.activeConnections);
    } else if (metricPath == "authenticatedPlayers") {
        value = static_cast<double>(snapshot.authenticatedPlayers);
    } else if (metricPath == "totalPacketsProcessed") {
        value = static_cast<double>(snapshot.totalPacketsProcessed);
    } else if (metricPath == "totalPacketsDropped") {
        value = static_cast<double>(snapshot.totalPacketsDropped);
    } else if (metricPath == "currentTick") {
        value = static_cast<double>(snapshot.currentTick);
    } else if (metricPath == "averageLatencyMs") {
        value = snapshot.averageLatencyMs;
    } else if (metricPath == "packetLossRate") {
        value = snapshot.packetLossRate;
    }
    // Application metrics - Game Logic
    else if (metricPath == "activeMatches") {
        value = static_cast<double>(snapshot.activeMatches);
    } else if (metricPath == "totalKills") {
        value = static_cast<double>(snapshot.totalKills);
    } else if (metricPath == "totalDeaths") {
        value = static_cast<double>(snapshot.totalDeaths);
    } else if (metricPath == "objectivesCaptured") {
        value = static_cast<double>(snapshot.objectivesCaptured);
    } else if (metricPath == "chatMessagesSent") {
        value = static_cast<double>(snapshot.chatMessagesSent);
    }
    // Performance metrics
    else if (metricPath == "frameTimeMs") {
        value = snapshot.frameTimeMs;
    } else if (metricPath == "physicsTimeMs") {
        value = snapshot.physicsTimeMs;
    } else if (metricPath == "networkTimeMs") {
        value = snapshot.networkTimeMs;
    } else if (metricPath == "gameLogicTimeMs") {
        value = snapshot.gameLogicTimeMs;
    }
    // Security metrics
    else if (metricPath == "securityViolations") {
        value = static_cast<double>(snapshot.securityViolations);
    } else if (metricPath == "malformedPackets") {
        value = static_cast<double>(snapshot.malformedPackets);
    } else if (metricPath == "speedHackDetections") {
        value = static_cast<double>(snapshot.speedHackDetections);
    } else if (metricPath == "kickedPlayers") {
        value = static_cast<double>(snapshot.kickedPlayers);
    } else if (metricPath == "bannedPlayers") {
        value = static_cast<double>(snapshot.bannedPlayers);
    }
    // Unknown metric path
    else {
        Logger::Warn("[AlertMetricsReporter::ExtractMetricValue] Unknown metric path: '%s', returning 0.0",
                     metricPath.c_str());
    }

    Logger::Debug("[AlertMetricsReporter::ExtractMetricValue] metricPath='%s' -> value=%.4f",
                  metricPath.c_str(), value);
    Logger::Trace("[AlertMetricsReporter::ExtractMetricValue] Exit: returning %.4f", value);
    return value;
}

void AlertMetricsReporter::EvaluateRules(const MetricsSnapshot& snapshot) {
    Logger::Trace("[AlertMetricsReporter::EvaluateRules] Entry: evaluating %zu rules", m_config.rules.size());
    std::lock_guard<std::mutex> lock(m_alertMutex);

    for (const auto& rule : m_config.rules) {
        Logger::Debug("[AlertMetricsReporter::EvaluateRules] Evaluating rule '%s' (metric='%s')",
                      rule.name.c_str(), rule.metricPath.c_str());

        double metricValue = ExtractMetricValue(snapshot, rule.metricPath);
        bool triggered = false;

        switch (rule.op) {
            case AlertReporterConfig::AlertRule::GREATER_THAN:
                triggered = metricValue > rule.threshold;
                break;
            case AlertReporterConfig::AlertRule::LESS_THAN:
                triggered = metricValue < rule.threshold;
                break;
            case AlertReporterConfig::AlertRule::EQUAL:
                triggered = std::fabs(metricValue - rule.threshold) < 1e-9;
                break;
            case AlertReporterConfig::AlertRule::NOT_EQUAL:
                triggered = std::fabs(metricValue - rule.threshold) >= 1e-9;
                break;
        }

        if (triggered) {
            Logger::Debug("[AlertMetricsReporter::EvaluateRules] Rule '%s' triggered: value=%.4f, threshold=%.4f",
                          rule.name.c_str(), metricValue, rule.threshold);

            if (!IsInCooldown(rule.name)) {
                TriggerAlert(rule.name, snapshot);

                // Invoke callback if provided
                if (rule.callback) {
                    try {
                        Logger::Debug("[AlertMetricsReporter::EvaluateRules] Invoking callback for rule '%s'",
                                      rule.name.c_str());
                        rule.callback(rule.name, snapshot);
                    } catch (const std::exception& ex) {
                        Logger::Error("[AlertMetricsReporter::EvaluateRules] Exception in callback for rule '%s': %s",
                                      rule.name.c_str(), ex.what());
                    }
                }
            } else {
                Logger::Debug("[AlertMetricsReporter::EvaluateRules] Rule '%s' is in cooldown, skipping alert",
                              rule.name.c_str());
            }
        } else {
            Logger::Debug("[AlertMetricsReporter::EvaluateRules] Rule '%s' not triggered: value=%.4f, threshold=%.4f",
                          rule.name.c_str(), metricValue, rule.threshold);
        }
    }

    Logger::Trace("[AlertMetricsReporter::EvaluateRules] Exit");
}

void AlertMetricsReporter::TriggerAlert(const std::string& ruleName, const MetricsSnapshot& snapshot) {
    Logger::Trace("[AlertMetricsReporter::TriggerAlert] Entry: ruleName='%s'", ruleName.c_str());

    // Record the alert time for cooldown tracking
    m_lastAlertTimes[ruleName] = std::chrono::steady_clock::now();

    // Add to active alerts if not already present
    auto it = std::find(m_activeAlerts.begin(), m_activeAlerts.end(), ruleName);
    if (it == m_activeAlerts.end()) {
        m_activeAlerts.push_back(ruleName);
        Logger::Debug("[AlertMetricsReporter::TriggerAlert] Added '%s' to active alerts (total=%zu)",
                      ruleName.c_str(), m_activeAlerts.size());
    }

    // Format timestamp for logging
    auto ts = std::chrono::duration_cast<std::chrono::seconds>(
        snapshot.timestamp.time_since_epoch()).count();

    Logger::Warn("[AlertMetricsReporter::TriggerAlert] ALERT TRIGGERED: rule='%s', snapshotTime=%lld, activeAlerts=%zu",
                 ruleName.c_str(), (long long)ts, m_activeAlerts.size());

    Logger::Trace("[AlertMetricsReporter::TriggerAlert] Exit");
}

bool AlertMetricsReporter::IsInCooldown(const std::string& ruleName) const {
    Logger::Trace("[AlertMetricsReporter::IsInCooldown] Entry: ruleName='%s'", ruleName.c_str());

    auto it = m_lastAlertTimes.find(ruleName);
    if (it == m_lastAlertTimes.end()) {
        Logger::Debug("[AlertMetricsReporter::IsInCooldown] No previous alert for rule '%s', not in cooldown",
                      ruleName.c_str());
        Logger::Trace("[AlertMetricsReporter::IsInCooldown] Exit: returning false");
        return false;
    }

    // Find the rule to get its cooldown period
    std::chrono::seconds cooldown{300}; // Default 5 minutes
    for (const auto& rule : m_config.rules) {
        if (rule.name == ruleName) {
            cooldown = rule.cooldownPeriod;
            break;
        }
    }

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - it->second);
    bool inCooldown = elapsed < cooldown;

    Logger::Debug("[AlertMetricsReporter::IsInCooldown] Rule '%s': elapsed=%llds, cooldown=%llds, inCooldown=%s",
                  ruleName.c_str(), (long long)elapsed.count(), (long long)cooldown.count(),
                  inCooldown ? "true" : "false");
    Logger::Trace("[AlertMetricsReporter::IsInCooldown] Exit: returning %s", inCooldown ? "true" : "false");
    return inCooldown;
}

// Factory function implementations
namespace ReporterFactory {

std::unique_ptr<MetricsReporter> CreateAlertReporter(
    const std::vector<AlertReporterConfig::AlertRule>& rules) {

    Logger::Trace("[ReporterFactory::CreateAlertReporter] Entry: rules=%zu", rules.size());

    AlertReporterConfig config;
    config.rules = rules;
    config.enableEmailAlerts = false;

    auto reporter = std::make_unique<AlertMetricsReporter>(config);
    Logger::Info("[ReporterFactory::CreateAlertReporter] Created AlertMetricsReporter with %zu rules", rules.size());
    Logger::Trace("[ReporterFactory::CreateAlertReporter] Exit: returning AlertMetricsReporter");
    return reporter;
}

std::vector<std::unique_ptr<MetricsReporter>> CreateStandardReporters() {
    Logger::Trace("[ReporterFactory::CreateStandardReporters] Entry");

    std::vector<std::unique_ptr<MetricsReporter>> reporters;

    // File reporter: JSON output with rotation
    Logger::Debug("[ReporterFactory::CreateStandardReporters] Creating FileMetricsReporter");
    reporters.push_back(CreateFileReporter("metrics", 10 * 1024 * 1024, 10));

    // Memory reporter: in-memory circular buffer for real-time queries
    Logger::Debug("[ReporterFactory::CreateStandardReporters] Creating MemoryMetricsReporter");
    reporters.push_back(CreateMemoryReporter(3600));

    // CSV reporter: CSV export for analysis tools
    Logger::Debug("[ReporterFactory::CreateStandardReporters] Creating CSVMetricsReporter");
    reporters.push_back(CreateCSVReporter("metrics.csv"));

    Logger::Info("[ReporterFactory::CreateStandardReporters] Created standard reporter set: File + Memory + CSV (%zu reporters)",
                reporters.size());
    Logger::Trace("[ReporterFactory::CreateStandardReporters] Exit: returning %zu reporters", reporters.size());
    return reporters;
}

} // namespace ReporterFactory

} // namespace Telemetry
