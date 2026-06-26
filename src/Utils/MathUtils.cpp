#define _USE_MATH_DEFINES
#include "Utils/MathUtils.h"
#include "Utils/Logger.h"
#include <cmath>

namespace MathUtils {

    std::vector<double> SolveCubic(double a, double b, double c) {
        Logger::Trace("[MathUtils::SolveCubic] Entry: a=%.6f, b=%.6f, c=%.6f", a, b, c);

        // Depressed cubic: x^3 + px + q = 0
        double p = b - (a*a)/3.0;
        double q = (2.0*a*a*a)/27.0 - (a*b)/3.0 + c;
        double discr = (q*q)/4.0 + (p*p*p)/27.0;
        Logger::Debug("[MathUtils::SolveCubic] Computed depressed cubic coefficients: p=%.6f, q=%.6f, discriminant=%.6f", p, q, discr);

        std::vector<double> roots;
        if (discr > 0) {
            Logger::Debug("[MathUtils::SolveCubic] Discriminant > 0: one real root");
            double sqrt_disc = std::sqrt(discr);
            double u = std::cbrt(-q/2.0 + sqrt_disc);
            double v = std::cbrt(-q/2.0 - sqrt_disc);
            roots.push_back(u + v - a/3.0);
            Logger::Debug("[MathUtils::SolveCubic] Single root: %.6f", roots[0]);
        }
        else if (std::abs(discr) < 1e-12) {
            Logger::Debug("[MathUtils::SolveCubic] Discriminant ~= 0: two real roots (one repeated)");
            double u = std::cbrt(-q/2.0);
            roots.push_back(2*u - a/3.0);
            roots.push_back(-u - a/3.0);
            Logger::Debug("[MathUtils::SolveCubic] Roots: %.6f, %.6f", roots[0], roots[1]);
        }
        else {
            Logger::Debug("[MathUtils::SolveCubic] Discriminant < 0: three real roots");
            double r = std::sqrt(-p*p*p/27.0);
            double phi = std::acos(-q/(2*r));
            double m = 2 * std::cbrt(r);
            roots.push_back(m * std::cos(phi/3.0)     - a/3.0);
            roots.push_back(m * std::cos((phi+2*M_PI)/3.0) - a/3.0);
            roots.push_back(m * std::cos((phi+4*M_PI)/3.0) - a/3.0);
            Logger::Debug("[MathUtils::SolveCubic] Roots: %.6f, %.6f, %.6f", roots[0], roots[1], roots[2]);
        }

        Logger::Trace("[MathUtils::SolveCubic] Exit: returning %zu roots", roots.size());
        return roots;
    }

}
