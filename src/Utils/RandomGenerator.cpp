// src/Utils/RandomGenerator.cpp
#include "Utils/RandomGenerator.h"
#include "Utils/Logger.h"
#include <algorithm>
#include <cmath>

namespace Utils {

RandomGenerator& RandomGenerator::Instance() {
    Logger::Trace("[RandomGenerator::Instance] Entry: accessing singleton instance");
    static RandomGenerator inst;
    Logger::Trace("[RandomGenerator::Instance] Exit: returning singleton reference");
    return inst;
}

RandomGenerator::RandomGenerator() {
    Logger::Trace("[RandomGenerator::RandomGenerator] Entry: constructor called");
    std::random_device rd;
    auto seedVal = rd();
    m_engine.seed(seedVal);
    Logger::Debug("[RandomGenerator::RandomGenerator] Engine seeded with random_device value");
    Logger::Info("[RandomGenerator::RandomGenerator] RandomGenerator constructed and seeded successfully");
    Logger::Trace("[RandomGenerator::RandomGenerator] Exit: constructor complete");
}

void RandomGenerator::Seed(uint64_t seed) {
    Logger::Trace("[RandomGenerator::Seed] Entry: seed=%llu", (unsigned long long)seed);
    std::lock_guard<std::mutex> lock(m_mutex);
    Logger::Debug("[RandomGenerator::Seed] Mutex acquired, re-seeding engine with user-provided value");
    m_engine.seed(seed);
    Logger::Info("[RandomGenerator::Seed] Engine re-seeded with value %llu", (unsigned long long)seed);
    Logger::Trace("[RandomGenerator::Seed] Exit");
}

int32_t RandomGenerator::Int(int32_t min, int32_t max) {
    Logger::Trace("[RandomGenerator::Int] Entry: min=%d, max=%d", min, max);
    std::lock_guard<std::mutex> lock(m_mutex);
    // Hardening: std::uniform_int_distribution has undefined behavior when min > max.
    // Normalize the range (swap) rather than crash; the resulting value set is identical.
    if (min > max) {
        Logger::Warn("[RandomGenerator::Int] Inverted range min=%d > max=%d; swapping bounds", min, max);
        std::swap(min, max);
    }
    std::uniform_int_distribution<int32_t> dist(min, max);
    int32_t result = dist(m_engine);
    Logger::Debug("[RandomGenerator::Int] Generated random int: %d from uniform_int_distribution[%d, %d]", result, min, max);
    Logger::Trace("[RandomGenerator::Int] Exit: returning %d", result);
    return result;
}

double RandomGenerator::Real(double min, double max) {
    Logger::Trace("[RandomGenerator::Real] Entry: min=%f, max=%f", min, max);
    std::lock_guard<std::mutex> lock(m_mutex);
    // Hardening: non-finite bounds or min > max are UB for uniform_real_distribution.
    if (!std::isfinite(min) || !std::isfinite(max)) {
        Logger::Warn("[RandomGenerator::Real] Non-finite bounds (min=%f, max=%f); defaulting to [0,1)", min, max);
        min = 0.0;
        max = 1.0;
    }
    if (min > max) {
        Logger::Warn("[RandomGenerator::Real] Inverted range min=%f > max=%f; swapping bounds", min, max);
        std::swap(min, max);
    }
    std::uniform_real_distribution<double> dist(min, max);
    double result = dist(m_engine);
    Logger::Debug("[RandomGenerator::Real] Generated random real: %f from uniform_real_distribution[%f, %f]", result, min, max);
    Logger::Trace("[RandomGenerator::Real] Exit: returning %f", result);
    return result;
}

double RandomGenerator::Normal(double mean, double stddev) {
    Logger::Trace("[RandomGenerator::Normal] Entry: mean=%f, stddev=%f", mean, stddev);
    std::lock_guard<std::mutex> lock(m_mutex);
    // Hardening: normal_distribution requires a finite mean and a non-negative, finite stddev.
    if (!std::isfinite(mean)) {
        Logger::Warn("[RandomGenerator::Normal] Non-finite mean=%f; defaulting to 0.0", mean);
        mean = 0.0;
    }
    if (!std::isfinite(stddev) || stddev < 0.0) {
        Logger::Warn("[RandomGenerator::Normal] Invalid stddev=%f; clamping to 0.0", stddev);
        stddev = 0.0;
    }
    std::normal_distribution<double> dist(mean, stddev);
    double result = dist(m_engine);
    Logger::Debug("[RandomGenerator::Normal] Generated normal variate: %f (mean=%f, stddev=%f)", result, mean, stddev);
    Logger::Trace("[RandomGenerator::Normal] Exit: returning %f", result);
    return result;
}

int32_t RandomGenerator::Poisson(double mean) {
    Logger::Trace("[RandomGenerator::Poisson] Entry: mean=%f", mean);
    std::lock_guard<std::mutex> lock(m_mutex);
    // Hardening: poisson_distribution requires mean > 0 (and finite); otherwise UB.
    if (!std::isfinite(mean) || mean <= 0.0) {
        Logger::Warn("[RandomGenerator::Poisson] Invalid mean=%f (must be finite and > 0); returning 0", mean);
        Logger::Trace("[RandomGenerator::Poisson] Exit: returning 0 (invalid mean)");
        return 0;
    }
    std::poisson_distribution<int32_t> dist(mean);
    int32_t result = dist(m_engine);
    Logger::Debug("[RandomGenerator::Poisson] Generated Poisson variate: %d (mean=%f)", result, mean);
    Logger::Trace("[RandomGenerator::Poisson] Exit: returning %d", result);
    return result;
}

double RandomGenerator::Exponential(double lambda) {
    Logger::Trace("[RandomGenerator::Exponential] Entry: lambda=%f", lambda);
    std::lock_guard<std::mutex> lock(m_mutex);
    // Hardening: exponential_distribution requires lambda > 0 (and finite); otherwise UB.
    if (!std::isfinite(lambda) || lambda <= 0.0) {
        Logger::Warn("[RandomGenerator::Exponential] Invalid lambda=%f (must be finite and > 0); returning 0.0", lambda);
        Logger::Trace("[RandomGenerator::Exponential] Exit: returning 0.0 (invalid lambda)");
        return 0.0;
    }
    std::exponential_distribution<double> dist(lambda);
    double result = dist(m_engine);
    Logger::Debug("[RandomGenerator::Exponential] Generated exponential variate: %f (lambda=%f)", result, lambda);
    Logger::Trace("[RandomGenerator::Exponential] Exit: returning %f", result);
    return result;
}

template<typename It>
void RandomGenerator::Shuffle(It first, It last) {
    Logger::Trace("[RandomGenerator::Shuffle] Entry: shuffling iterator range");
    std::lock_guard<std::mutex> lock(m_mutex);
    Logger::Debug("[RandomGenerator::Shuffle] Performing Fisher-Yates shuffle via std::shuffle with mt19937_64 engine");
    std::shuffle(first, last, m_engine);
    Logger::Debug("[RandomGenerator::Shuffle] Shuffle complete");
    Logger::Trace("[RandomGenerator::Shuffle] Exit");
}

// Explicit instantiation for common iterator types
template void RandomGenerator::Shuffle<std::vector<int>::iterator>(
    std::vector<int>::iterator, std::vector<int>::iterator);

} // namespace Utils
