// src/Game/ParticleEffect.h
#pragma once

#include <string>
#include <cstdint>

struct ParticleEffect {
    std::string name;
    uint32_t    maxParticles = 100;
    float       lifetime     = 1.0f;
    float       speed        = 1.0f;
};
