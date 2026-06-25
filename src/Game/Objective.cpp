// src/Game/Objective.cpp – Implementation for Objective

#include "Game/Objective.h"
#include <algorithm>
#include "Utils/Logger.h"

Objective::Objective(uint32_t id, ObjectiveType type, const Vector3& pos, float radius)
    : m_id(id)
    , m_type(type)
    , m_position(pos)
    , m_radius(radius)
    , m_controllingTeam(0)
    , m_captureProgress(0.0f)
    , m_active(true)
    , m_captureRatePerSecond(0.25f)  // 4 seconds to capture
    , m_decayRatePerSecond(0.10f)    // 10% per second decay
{
    Logger::Trace("[Objective::Objective] Entry: id=%u, type=%d, pos=(%.1f,%.1f,%.1f), radius=%.1f",
                  id, static_cast<int>(type), pos.x, pos.y, pos.z, radius);
    Logger::Info("Objective %u created (type=%d) at (%.1f,%.1f,%.1f) radius=%.1f",
                 m_id, static_cast<int>(m_type),
                 pos.x,pos.y,pos.z, radius);
    Logger::Debug("[Objective::Objective] Initial state: controllingTeam=%u, captureProgress=%.2f, active=%d, captureRate=%.2f, decayRate=%.2f",
                  m_controllingTeam, m_captureProgress, m_active, m_captureRatePerSecond, m_decayRatePerSecond);
    Logger::Trace("[Objective::Objective] Exit");
}

Objective::~Objective() {
    Logger::Trace("[Objective::~Objective] Entry: id=%u", m_id);
    Logger::Trace("[Objective::~Objective] Exit");
}

uint32_t Objective::GetId() const {
    Logger::Trace("[Objective::GetId] Entry: returning id=%u", m_id);
    return m_id;
}

ObjectiveType Objective::GetType() const {
    Logger::Trace("[Objective::GetType] Entry: returning type=%d", static_cast<int>(m_type));
    return m_type;
}

Vector3 Objective::GetPosition() const {
    Logger::Trace("[Objective::GetPosition] Entry: returning pos=(%.1f,%.1f,%.1f)", m_position.x, m_position.y, m_position.z);
    return m_position;
}

float Objective::GetRadius() const {
    Logger::Trace("[Objective::GetRadius] Entry: returning radius=%.1f", m_radius);
    return m_radius;
}

uint32_t Objective::GetControllingTeam() const {
    Logger::Trace("[Objective::GetControllingTeam] Entry: returning controllingTeam=%u", m_controllingTeam);
    return m_controllingTeam;
}

float Objective::GetCaptureProgress() const {
    Logger::Trace("[Objective::GetCaptureProgress] Entry: returning captureProgress=%.2f", m_captureProgress);
    return m_captureProgress;
}

bool Objective::IsActive() const {
    Logger::Trace("[Objective::IsActive] Entry: returning active=%d", m_active);
    return m_active;
}

void Objective::Update(float deltaSeconds) {
    Logger::Trace("[Objective::Update] Entry: id=%u, deltaSeconds=%.4f", m_id, deltaSeconds);
    if (!m_active) {
        Logger::Debug("[Objective::Update] Objective %u is inactive, skipping update", m_id);
        Logger::Trace("[Objective::Update] Exit (inactive)");
        return;
    }
    // Passive decay toward neutral when contested by no one
    if (m_controllingTeam == 0 && m_captureProgress > 0.0f) {
        float oldProgress = m_captureProgress;
        m_captureProgress = std::max(0.0f, m_captureProgress - m_decayRatePerSecond * deltaSeconds);
        Logger::Debug("[Objective::Update] Objective %u decaying: progress %.4f -> %.4f (no controlling team)", m_id, oldProgress, m_captureProgress);
        if (m_captureProgress == 0.0f) {
            Logger::Debug("Objective %u decayed to neutral", m_id);
        }
    } else {
        Logger::Trace("[Objective::Update] Objective %u no decay needed: controllingTeam=%u, captureProgress=%.2f", m_id, m_controllingTeam, m_captureProgress);
    }
    Logger::Trace("[Objective::Update] Exit");
}

bool Objective::Contest(uint32_t teamId, float deltaSeconds) {
    Logger::Trace("[Objective::Contest] Entry: id=%u, teamId=%u, deltaSeconds=%.4f", m_id, teamId, deltaSeconds);
    if (!m_active) {
        Logger::Debug("[Objective::Contest] Objective %u is inactive, returning false", m_id);
        Logger::Trace("[Objective::Contest] Exit: return false (inactive)");
        return false;
    }

    if (teamId == m_controllingTeam) {
        // Already controlled by this team; nothing to do
        Logger::Debug("[Objective::Contest] Objective %u already controlled by team %u, no contest", m_id, teamId);
        Logger::Trace("[Objective::Contest] Exit: return false (already controlled)");
        return false;
    }

    // Progress capture toward teamId
    float oldProgress = m_captureProgress;
    m_captureProgress += m_captureRatePerSecond * deltaSeconds;
    m_captureProgress = std::min(m_captureProgress, 1.0f);
    Logger::Debug("[Objective::Contest] Objective %u capture progress: %.4f -> %.4f (team %u contesting vs team %u)",
                  m_id, oldProgress, m_captureProgress, teamId, m_controllingTeam);

    if (m_captureProgress >= 1.0f) {
        Logger::Info("Objective %u captured by team %u", m_id, teamId);
        m_controllingTeam = teamId;
        m_captureProgress = 1.0f;
        Logger::Trace("[Objective::Contest] Exit: return true (captured)");
        return true;
    }

    Logger::Trace("[Objective::Contest] Exit: return false (in progress)");
    return false;
}

void Objective::SetControl(uint32_t teamId) {
    Logger::Trace("[Objective::SetControl] Entry: id=%u, teamId=%u", m_id, teamId);
    Logger::Debug("[Objective::SetControl] Objective %u forced from team %u to team %u", m_id, m_controllingTeam, teamId);
    m_controllingTeam = teamId;
    m_captureProgress = 1.0f;
    Logger::Info("Objective %u forcibly set to team %u", m_id, teamId);
    Logger::Trace("[Objective::SetControl] Exit");
}

void Objective::Reset() {
    Logger::Trace("[Objective::Reset] Entry: id=%u", m_id);
    Logger::Debug("[Objective::Reset] Objective %u resetting from team %u, progress=%.2f", m_id, m_controllingTeam, m_captureProgress);
    m_controllingTeam = 0;
    m_captureProgress = 0.0f;
    m_active = true;
    Logger::Info("Objective %u reset to neutral", m_id);
    Logger::Trace("[Objective::Reset] Exit");
}
