#include "Math/MathUtils.h"
#include <cmath>

namespace MathUtils {

    std::vector<double> SolveCubic(double a, double b, double c) {
        // Depressed cubic: x³ + px + q = 0
        double p = b - (a*a)/3.0;
        double q = (2.0*a*a*a)/27.0 - (a*b)/3.0 + c;
        double discr = (q*q)/4.0 + (p*p*p)/27.0;

        std::vector<double> roots;
        if (discr > 0) {
            double sqrt_disc = std::sqrt(discr);
            double u = std::cbrt(-q/2.0 + sqrt_disc);
            double v = std::cbrt(-q/2.0 - sqrt_disc);
            roots.push_back(u + v - a/3.0);
        }
        else if (std::abs(discr) < 1e-12) {
            double u = std::cbrt(-q/2.0);
            roots.push_back(2*u - a/3.0);
            roots.push_back(-u - a/3.0);
        }
        else {
            double r = std::sqrt(-p*p*p/27.0);
            double phi = std::acos(-q/(2*r));
            double m = 2 * std::cbrt(r);
            roots.push_back(m * std::cos(phi/3.0)     - a/3.0);
            roots.push_back(m * std::cos((phi+2*M_PI)/3.0) - a/3.0);
            roots.push_back(m * std::cos((phi+4*M_PI)/3.0) - a/3.0);
        }
        return roots;
    }

}