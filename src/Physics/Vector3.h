// src/Math/Vector3.h

#pragma once

#include <cmath>

class Vector3 {
public:
    float x, y, z;

    // Constructors
    Vector3() : x(0.0f), y(0.0f), z(0.0f) {}
    Vector3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

    // Basic arithmetic operations
    Vector3 operator+(const Vector3& other) const {
        return Vector3(x + other.x, y + other.y, z + other.z);
    }

    Vector3 operator-(const Vector3& other) const {
        return Vector3(x - other.x, y - other.y, z - other.z);
    }

    Vector3 operator*(float scalar) const {
        return Vector3(x * scalar, y * scalar, z * scalar);
    }

    Vector3 operator/(float scalar) const {
        return Vector3(x / scalar, y / scalar, z / scalar);
    }

    // Compound assignment operators
    Vector3& operator+=(const Vector3& other) {
        x += other.x; y += other.y; z += other.z;
        return *this;
    }

    Vector3& operator-=(const Vector3& other) {
        x -= other.x; y -= other.y; z -= other.z;
        return *this;
    }

    Vector3& operator*=(float scalar) {
        x *= scalar; y *= scalar; z *= scalar;
        return *this;
    }

    Vector3& operator/=(float scalar) {
        x /= scalar; y /= scalar; z /= scalar;
        return *this;
    }

    // Comparison operators
    bool operator==(const Vector3& other) const {
        return std::abs(x - other.x) < 1e-6f &&
               std::abs(y - other.y) < 1e-6f &&
               std::abs(z - other.z) < 1e-6f;
    }

    bool operator!=(const Vector3& other) const {
        return !(*this == other);
    }

    // Vector operations
    float Dot(const Vector3& other) const {
        return x * other.x + y * other.y + z * other.z;
    }

    Vector3 Cross(const Vector3& other) const {
        return Vector3(
            y * other.z - z * other.y,
            z * other.x - x * other.z,
            x * other.y - y * other.x
        );
    }

    float Length() const {
        return std::sqrt(x * x + y * y + z * z);
    }

    float LengthSquared() const {
        return x * x + y * y + z * z;
    }

    Vector3 Normalized() const {
        float len = Length();
        if (len < 1e-6f) return Vector3(0, 0, 0);
        return *this / len;
    }

    void Normalize() {
        float len = Length();
        if (len > 1e-6f) {
            x /= len; y /= len; z /= len;
        }
    }

    float Distance(const Vector3& other) const {
        return (*this - other).Length();
    }

    float DistanceSquared(const Vector3& other) const {
        return (*this - other).LengthSquared();
    }

    // Linear interpolation
    Vector3 Lerp(const Vector3& target, float t) const {
        return *this + (target - *this) * t;
    }

    // Utility methods
    bool IsZero() const {
        return std::abs(x) < 1e-6f && std::abs(y) < 1e-6f && std::abs(z) < 1e-6f;
    }

    void Set(float x_, float y_, float z_) {
        x = x_; y = y_; z = z_;
    }

    // Static utility vectors
    static Vector3 Zero() { return Vector3(0, 0, 0); }
    static Vector3 One() { return Vector3(1, 1, 1); }
    static Vector3 Up() { return Vector3(0, 0, 1); }
    static Vector3 Down() { return Vector3(0, 0, -1); }
    static Vector3 Forward() { return Vector3(0, 1, 0); }
    static Vector3 Backward() { return Vector3(0, -1, 0); }
    static Vector3 Right() { return Vector3(1, 0, 0); }
    static Vector3 Left() { return Vector3(-1, 0, 0); }
};

// Global operators for scalar * Vector3
inline Vector3 operator*(float scalar, const Vector3& vec) {
    return vec * scalar;
}