// src/Utils/RandomGenerator.cpp
#include "Utils/RandomGenerator.h"
#include <algorithm>

namespace Utils {

RandomGenerator& RandomGenerator::Instance() {
    static RandomGenerator inst;
    return inst;
}

RandomGenerator::RandomGenerator() {
    std::random_device rd;
    m_engine.seed(rd());
}

void RandomGenerator::Seed(uint64_t seed) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_engine.seed(seed);
}

int32_t RandomGenerator::Int(int32_t min, int32_t max) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::uniform_int_distribution<int32_t> dist(min, max);
    return dist(m_engine);
}

double RandomGenerator::Real(double min, double max) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::uniform_real_distribution<double> dist(min, max);
    return dist(m_engine);
}

double RandomGenerator::Normal(double mean, double stddev) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::normal_distribution<double> dist(mean, stddev);
    return dist(m_engine);
}

int32_t RandomGenerator::Poisson(double mean) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::poisson_distribution<int32_t> dist(mean);
    return dist(m_engine);
}

double RandomGenerator::Exponential(double lambda) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::exponential_distribution<double> dist(lambda);
    return dist(m_engine);
}

template<typename It>
void RandomGenerator::Shuffle(It first, It last) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::shuffle(first, last, m_engine);
}

// Explicit instantiation for common iterator types
template void RandomGenerator::Shuffle<std::vector<int>::iterator>(
    std::vector<int>::iterator, std::vector<int>::iterator);

} // namespace Utils