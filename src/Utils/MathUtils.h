#pragma once

#include <cmath>
#include <vector>
#include <optional>
#include <random>
#include <utility>

namespace MathUtils {

    // Clamp value to [min, max]
    template<typename T>
    inline T Clamp(T value, T minVal, T maxVal) {
        return (value < minVal) ? minVal : ((value > maxVal) ? maxVal : value);
    }

    // Linear interpolation: a + (b - a) * t
    template<typename T, typename U>
    inline T Lerp(const T& a, const T& b, U t) {
        return static_cast<T>(a + (b - a) * t);
    }

    // Smoothstep interpolation in [0,1]
    inline double SmoothStep(double edge0, double edge1, double x) {
        x = Clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
        return x * x * (3 - 2 * x);
    }

    // Map a value from one range to another
    template<typename T>
    inline T Remap(T value, T inMin, T inMax, T outMin, T outMax) {
        if (inMax == inMin) return outMin;
        return outMin + (value - inMin) * (outMax - outMin) / (inMax - inMin);
    }

    // Degrees ↔ Radians
    constexpr double Pi = 3.14159265358979323846;
    inline double ToRadians(double degrees) { return degrees * (Pi / 180.0); }
    inline double ToDegrees(double radians) { return radians * (180.0 / Pi); }

    // Wrap angle to [−π, +π]
    inline double WrapRadians(double theta) {
        theta = std::fmod(theta + Pi, 2 * Pi);
        if (theta < 0) theta += 2 * Pi;
        return theta - Pi;
    }

    // Compute factorial (slow; use for small n)
    inline unsigned long long Factorial(unsigned n) {
        unsigned long long result = 1;
        for (unsigned i = 2; i <= n; ++i) result *= i;
        return result;
    }

    // Compute binomial coefficient n choose k
    inline unsigned long long Binomial(unsigned n, unsigned k) {
        if (k > n) return 0;
        if (k > n - k) k = n - k;
        unsigned long long result = 1;
        for (unsigned i = 1; i <= k; ++i) {
            result = result * (n - k + i) / i;
        }
        return result;
    }

    // Generate uniform random integer in [min, max]
    inline int RandomInt(int min, int max) {
        static std::mt19937 rng{ std::random_device{}() };
        std::uniform_int_distribution<int> dist(min, max);
        return dist(rng);
    }

    // Generate uniform random real in [0,1)
    inline double Random01() {
        static std::mt19937_64 rng{ std::random_device{}() };
        std::uniform_real_distribution<double> dist(0.0, 1.0);
        return dist(rng);
    }

    // Solve quadratic ax² + bx + c = 0; returns zero, one, or two real roots
    inline std::vector<double> SolveQuadratic(double a, double b, double c) {
        std::vector<double> roots;
        if (std::abs(a) < 1e-12) {
            if (std::abs(b) < 1e-12) return roots;
            roots.push_back(-c / b);
            return roots;
        }
        double disc = b*b - 4*a*c;
        if (disc < 0) return roots;
        double sq = std::sqrt(disc);
        roots.push_back((-b - sq) / (2*a));
        if (disc > 0) roots.push_back((-b + sq) / (2*a));
        return roots;
    }

    // Solve cubic x³ + ax² + bx + c = 0; returns real roots
    std::vector<double> SolveCubic(double a, double b, double c);

    // Greatest common divisor
    inline long long GCD(long long x, long long y) {
        while (y != 0) {
            long long t = y;
            y = x % y;
            x = t;
        }
        return x < 0 ? -x : x;
    }

    // Least common multiple
    inline long long LCM(long long x, long long y) {
        if (x == 0 || y == 0) return 0;
        return (x / GCD(x,y)) * y;
    }

}