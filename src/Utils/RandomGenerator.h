// src/Utils/RandomGenerator.h
#pragma once

#include <random>
#include <mutex>
#include <vector>
#include <cstdint>

namespace Utils {

class RandomGenerator {
public:
    // Singleton access
    static RandomGenerator& Instance();

    // Seed the generator (thread-safe)
    void Seed(uint64_t seed);

    // Generate uniform integer in [min, max]
    int32_t Int(int32_t min, int32_t max);

    // Generate uniform real in [min, max)
    double Real(double min = 0.0, double max = 1.0);

    // Generate normally distributed real with mean and stddev
    double Normal(double mean = 0.0, double stddev = 1.0);

    // Generate Poisson-distributed integer with given mean
    int32_t Poisson(double mean);

    // Generate exponential distribution with given lambda
    double Exponential(double lambda);

    // Shuffle a container (by reference)
    template<typename It>
    void Shuffle(It first, It last);

private:
    RandomGenerator();
    ~RandomGenerator() = default;

    // Non-copyable
    RandomGenerator(const RandomGenerator&) = delete;
    RandomGenerator& operator=(const RandomGenerator&) = delete;

    std::mt19937_64                     m_engine;
    std::mutex                          m_mutex;
};

} // namespace Utils