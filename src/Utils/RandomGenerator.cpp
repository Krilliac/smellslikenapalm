// src/Utils/RandomGenerator.cpp
#include "Utils/RandomGenerator.h"
#include "Utils/Logger.h"
#include <algorithm>

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
    std::uniform_int_distribution<int32_t> dist(min, max);
    int32_t result = dist(m_engine);
    Logger::Debug("[RandomGenerator::Int] Generated random int: %d from uniform_int_distribution[%d, %d]", result, min, max);
    Logger::Trace("[RandomGenerator::Int] Exit: returning %d", result);
    return result;
}

double RandomGenerator::Real(double min, double max) {
    Logger::Trace("[RandomGenerator::Real] Entry: min=%f, max=%f", min, max);
    std::lock_guard<std::mutex> lock(m_mutex);
    std::uniform_real_distribution<double> dist(min, max);
    double result = dist(m_engine);
    Logger::Debug("[RandomGenerator::Real] Generated random real: %f from uniform_real_distribution[%f, %f]", result, min, max);
    Logger::Trace("[RandomGenerator::Real] Exit: returning %f", result);
    return result;
}

double RandomGenerator::Normal(double mean, double stddev) {
    Logger::Trace("[RandomGenerator::Normal] Entry: mean=%f, stddev=%f", mean, stddev);
    std::lock_guard<std::mutex> lock(m_mutex);
    std::normal_distribution<double> dist(mean, stddev);
    double result = dist(m_engine);
    Logger::Debug("[RandomGenerator::Normal] Generated normal variate: %f (mean=%f, stddev=%f)", result, mean, stddev);
    Logger::Trace("[RandomGenerator::Normal] Exit: returning %f", result);
    return result;
}

int32_t RandomGenerator::Poisson(double mean) {
    Logger::Trace("[RandomGenerator::Poisson] Entry: mean=%f", mean);
    std::lock_guard<std::mutex> lock(m_mutex);
    std::poisson_distribution<int32_t> dist(mean);
    int32_t result = dist(m_engine);
    Logger::Debug("[RandomGenerator::Poisson] Generated Poisson variate: %d (mean=%f)", result, mean);
    Logger::Trace("[RandomGenerator::Poisson] Exit: returning %d", result);
    return result;
}

double RandomGenerator::Exponential(double lambda) {
    Logger::Trace("[RandomGenerator::Exponential] Entry: lambda=%f", lambda);
    std::lock_guard<std::mutex> lock(m_mutex);
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
