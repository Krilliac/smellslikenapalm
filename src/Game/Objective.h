// src/Game/Objective.h – Header for Objective

#pragma once

#include <cstdint>
#include <string>
#include "Math/Vector3.h"

enum class ObjectiveType {
    CapturePoint,
    DestroyableAsset,
    HoldArea
};

struct ObjectiveState {
    uint32_t          id;
    ObjectiveType     type;
    Vector3           position;
    float             radius;           // for HoldArea
    uint32_t          controllingTeam;  // 0 = neutral
    float             captureProgress;  // 0.0–1.0
    bool              active;
};

class Objective {
public:
    Objective(uint32_t id, ObjectiveType type, const Vector3& pos, float radius = 0.0f);
    ~Objective();

    uint32_t          GetId() const;
    ObjectiveType     GetType() const;
    Vector3           GetPosition() const;
    float             GetRadius() const;

    uint32_t          GetControllingTeam() const;
    float             GetCaptureProgress() const;
    bool              IsActive() const;

    // Called each tick to advance capture/decay
    void              Update(float deltaSeconds);

    // Player attempts to capture or contest
    // returns true if state changed
    bool              Contest(uint32_t teamId, float deltaSeconds);

    // Force completion or reset
    void              SetControl(uint32_t teamId);
    void              Reset();

private:
    uint32_t          m_id;
    ObjectiveType     m_type;
    Vector3           m_position;
    float             m_radius;

    uint32_t          m_controllingTeam;
    float             m_captureProgress;
    bool              m_active;

    // Configuration
    float             m_captureRatePerSecond;
    float             m_decayRatePerSecond;
};