// src/Game/Objective.cpp – Implementation for Objective

#include "Game/Objective.h"
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
    Logger::Info("Objective %u created (type=%d) at (%.1f,%.1f,%.1f) radius=%.1f",
                 m_id, static_cast<int>(m_type),
                 pos.x,pos.y,pos.z, radius);
}

Objective::~Objective() = default;

uint32_t Objective::GetId() const { return m_id; }
ObjectiveType Objective::GetType() const { return m_type; }
Vector3 Objective::GetPosition() const { return m_position; }
float Objective::GetRadius() const { return m_radius; }

uint32_t Objective::GetControllingTeam() const { return m_controllingTeam; }
float Objective::GetCaptureProgress() const { return m_captureProgress; }
bool Objective::IsActive() const { return m_active; }

void Objective::Update(float deltaSeconds) {
    if (!m_active) return;
    // Passive decay toward neutral when contested by no one
    if (m_controllingTeam == 0 && m_captureProgress > 0.0f) {
        m_captureProgress = std::max(0.0f, m_captureProgress - m_decayRatePerSecond * deltaSeconds);
        if (m_captureProgress == 0.0f) {
            Logger::Debug("Objective %u decayed to neutral", m_id);
        }
    }
}

bool Objective::Contest(uint32_t teamId, float deltaSeconds) {
    if (!m_active) return false;

    if (teamId == m_controllingTeam) {
        // Already controlled by this team; nothing to do
        return false;
    }

    // Progress capture toward teamId
    m_captureProgress += m_captureRatePerSecond * deltaSeconds;
    m_captureProgress = std::min(m_captureProgress, 1.0f);

    if (m_captureProgress >= 1.0f) {
        Logger::Info("Objective %u captured by team %u", m_id, teamId);
        m_controllingTeam = teamId;
        m_captureProgress = 1.0f;
        return true;
    }

    return false;
}

void Objective::SetControl(uint32_t teamId) {
    m_controllingTeam = teamId;
    m_captureProgress = 1.0f;
    Logger::Info("Objective %u forcibly set to team %u", m_id, teamId);
}

void Objective::Reset() {
    m_controllingTeam = 0;
    m_captureProgress = 0.0f;
    m_active = true;
    Logger::Info("Objective %u reset to neutral", m_id);
}