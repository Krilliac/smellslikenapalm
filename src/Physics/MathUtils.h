// src/Math/MathUtils.h
#pragma once

#include <vector>

namespace MathUtils {

    // Solve the depressed cubic x^3 + ax^2 + bx + c = 0
    // Returns 1-3 real roots
    std::vector<double> SolveCubic(double a, double b, double c);

}
