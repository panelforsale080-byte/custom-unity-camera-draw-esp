#ifndef MLBB_MATRIX4X4_HPP
#define MLBB_MATRIX4X4_HPP

#include <cmath>

// EspSource / EXTERNAL-MLBB Unity column layout + MultiplyPoint (world -> clip / NDC).
struct Vector3 {
    float X = 0.f;
    float Y = 0.f;
    float Z = 0.f;

    Vector3() = default;
    Vector3(float x, float y, float z) : X(x), Y(y), Z(z) {}
};

struct Matrix4x4 {
    union {
        struct {
            float m00;
            float m10;
            float m20;
            float m30;
            float m01;
            float m11;
            float m21;
            float m31;
            float m02;
            float m12;
            float m22;
            float m32;
            float m03;
            float m13;
            float m23;
            float m33;
        };
        float m[16];
    };

    inline Matrix4x4();

    inline Matrix4x4 operator*(const Matrix4x4 &m2) const;

    inline Matrix4x4(float entry00, float entry01, float entry02, float entry03, float entry10,
                     float entry11, float entry12, float entry13, float entry20, float entry21,
                     float entry22, float entry23, float entry30, float entry31, float entry32,
                     float entry33);

    inline Vector3 MultiplyPoint(Vector3 point) const;
};

inline Matrix4x4::Matrix4x4()
    : m00(1.0f), m10(0.0f), m20(0.0f), m30(0.0f), m01(0.0f), m11(1.0f), m21(0.0f), m31(0.0f),
      m02(0.0f), m12(0.0f), m22(1.0f), m32(0.0f), m03(0.0f), m13(0.0f), m23(0.0f), m33(1.0f) {}

inline Matrix4x4::Matrix4x4(float entry00, float entry01, float entry02, float entry03,
                            float entry10, float entry11, float entry12, float entry13,
                            float entry20, float entry21, float entry22, float entry23,
                            float entry30, float entry31, float entry32, float entry33)
    : m{entry00, entry01, entry02, entry03, entry10, entry11, entry12, entry13,
        entry20, entry21, entry22, entry23, entry30, entry31, entry32, entry33} {}

inline Matrix4x4 Matrix4x4::operator*(const Matrix4x4 &rhs) const {
    Matrix4x4 res;
    res.m00 = m00 * rhs.m00 + m01 * rhs.m10 + m02 * rhs.m20 + m03 * rhs.m30;
    res.m01 = m00 * rhs.m01 + m01 * rhs.m11 + m02 * rhs.m21 + m03 * rhs.m31;
    res.m02 = m00 * rhs.m02 + m01 * rhs.m12 + m02 * rhs.m22 + m03 * rhs.m32;
    res.m03 = m00 * rhs.m03 + m01 * rhs.m13 + m02 * rhs.m23 + m03 * rhs.m33;

    res.m10 = m10 * rhs.m00 + m11 * rhs.m10 + m12 * rhs.m20 + m13 * rhs.m30;
    res.m11 = m10 * rhs.m01 + m11 * rhs.m11 + m12 * rhs.m21 + m13 * rhs.m31;
    res.m12 = m10 * rhs.m02 + m11 * rhs.m12 + m12 * rhs.m22 + m13 * rhs.m32;
    res.m13 = m10 * rhs.m03 + m11 * rhs.m13 + m12 * rhs.m23 + m13 * rhs.m33;

    res.m20 = m20 * rhs.m00 + m21 * rhs.m10 + m22 * rhs.m20 + m23 * rhs.m30;
    res.m21 = m20 * rhs.m01 + m21 * rhs.m11 + m22 * rhs.m21 + m23 * rhs.m31;
    res.m22 = m20 * rhs.m02 + m21 * rhs.m12 + m22 * rhs.m22 + m23 * rhs.m32;
    res.m23 = m20 * rhs.m03 + m21 * rhs.m13 + m22 * rhs.m23 + m23 * rhs.m33;

    res.m30 = m30 * rhs.m00 + m31 * rhs.m10 + m32 * rhs.m20 + m33 * rhs.m30;
    res.m31 = m30 * rhs.m01 + m31 * rhs.m11 + m32 * rhs.m21 + m33 * rhs.m31;
    res.m32 = m30 * rhs.m02 + m31 * rhs.m12 + m32 * rhs.m22 + m33 * rhs.m32;
    res.m33 = m30 * rhs.m03 + m31 * rhs.m13 + m32 * rhs.m23 + m33 * rhs.m33;

    return res;
}

inline Vector3 Matrix4x4::MultiplyPoint(Vector3 point) const {
    Vector3 res;
    float w = m30 * point.X + m31 * point.Y + m32 * point.Z + m33;
    res.X = m00 * point.X + m01 * point.Y + m02 * point.Z + m03;
    res.Y = m10 * point.X + m11 * point.Y + m12 * point.Z + m13;
    res.Z = m20 * point.X + m21 * point.Y + m22 * point.Z + m23;
    /* Signed perspective divide (behind camera: negative w). */
    if (std::fabs(w) > 1e-5f) {
        const float inv = 1.0f / w;
        res.X *= inv;
        res.Y *= inv;
        res.Z *= inv;
    }
    return res;
}

#endif
