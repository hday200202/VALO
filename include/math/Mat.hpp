#pragma once

#include "Vec.hpp"

/* ========================================================================= */
/* ====================== ALL MATRICES ARE COL MAJOR ======================= */
/* ========================================================================= */

namespace valo {

/* ========================================================================= //
    Mat2
// ========================================================================= */
template <typename T> struct Mat2 {
    Vec2<T> cols[2] = { 
        {1, 0}, 
        {0, 1} 
    };

    static constexpr Mat2 identity() { return Mat2(); }
};

using Mat2f = Mat2<float>;
using Mat2d = Mat2<double>;
using Mat2i = Mat2<int32_t>;
using Mat2u = Mat2<uint32_t>;

static_assert(
    sizeof(Mat2f) == sizeof(float) * 4,
    "Mat2: Unexpected Padding"
);



/* ========================================================================= //
    Mat3
// ========================================================================= */
template <typename T> struct Mat3 {
    Vec3<T> cols[3] = { 
        {1, 0, 0}, 
        {0, 1, 0}, 
        {0, 0, 1} 
    };

    static constexpr Mat3 identity() { return Mat3(); }
};

using Mat3f = Mat3<float>;
using Mat3d = Mat3<double>;
using Mat3i = Mat3<int32_t>;
using Mat3u = Mat3<uint32_t>;

static_assert(
    sizeof(Mat3f) == sizeof(float) * 9,
    "Mat3: Unexpected Padding"
);



/* ========================================================================= //
    Mat4
// ========================================================================= */
template <typename T> struct Mat4 {
    Vec4<T> cols[4] = {
        {1, 0, 0, 0}, 
        {0, 1, 0, 0}, 
        {0, 0, 1, 0}, 
        {0, 0, 0, 1} 
    };

    static constexpr Mat4 identity() { return Mat4(); }


    static Mat4 lookAt(
        const Vec3<T>& eye, 
        const Vec3<T>& target, 
        const Vec3<T>& up
    ) {
        Vec3<T> f = (target - eye).normalized();
        Vec3<T> r = f.cross(up).normalized();
        Vec3<T> u = r.cross(f);

        return {{
            {        r.x,         u.x,        -f.x,   0},
            {        r.y,         u.y,        -f.y,   0},
            {        r.z,         u.z,        -f.z,   0},
            {-r.dot(eye), -u.dot(eye),  f.dot(eye),   1},
        }};
    }


    static constexpr Mat4 translation(const Vec3<T>& t) {
        return {{
            {  1,   0,   0,   0},
            {  0,   1,   0,   0},
            {  0,   0,   1,   0},
            {t.x, t.y, t.z,   1},
        }};
    }


    static constexpr Mat4 scale(const Vec3<T>& s) {
        return {{
            {s.x,   0,   0,   0},
            {  0, s.y,   0,   0},
            {  0,   0, s.z,   0},
            {  0,   0,   0,   1},
        }};
    }


    static constexpr Mat4 rotationX(const T rads) {
        T c = std::cos(rads);
        T s = std::sin(rads);

        return {{
            { 1,  0,  0,  0},
            { 0,  c,  s,  0},
            { 0, -s,  c,  0},
            { 0,  0,  0,  1},
        }};
    }


    static constexpr Mat4 rotationY(const T rads) {
        T c = std::cos(rads);
        T s = std::sin(rads);

        return {{
            { c,  0, -s,  0},
            { 0,  1,  0,  0},
            { s,  0,  c,  0},
            { 0,  0,  0,  1},
        }};
    }


    static constexpr Mat4 rotationZ(const T rads) {
        T c = std::cos(rads);
        T s = std::sin(rads);

        return {{
            { c,  s,  0,  0},
            {-s,  c,  0,  0},
            { 0,  0,  1,  0},
            { 0,  0,  0,  1},
        }};
    }


    static Mat4 perspective(
        const T fovY, const T aspect,
        const T zNear, const T zFar
    ) {
        T f = 1 / std::tan(fovY / 2);
        return {{
            {f / aspect, 0,                         0,  0},
            {         0, f,                         0,  0},
            {         0, 0,         zFar/(zNear-zFar), -1},
            {         0, 0, (zNear*zFar)/(zNear-zFar),  0},
        }};
    }


    static constexpr Mat4 orthographic(
        const T l, const T r,
        const T b, const T t,
        const T n, const T f
    ) {
        return {{
            {     2/(r-l),            0,       0, 0},
            {           0,      2/(t-b),       0, 0},
            {           0,            0, 1/(n-f), 0},
            {-(r+l)/(r-l), -(t+b)/(t-b), n/(n-f), 1}
        }};
    }


    static constexpr T det3(
        T a, T b, T c,
        T d, T e, T f,
        T g, T h, T i
    ) { return a * (e*i - f*h) - b * (d*i - f*g) + c * (d*h - e*g); }

    constexpr T cofactor(const int r, const int c) const {
        int rows[3], cs[3];
        for (int k = 0, i = 0; k < 4; ++k) if (k != r) rows[i++] = k;
        for (int k = 0, i = 0; k < 4; ++k) if (k != c) cs[i++]   = k;

        T d = det3(
            cols[cs[0]][rows[0]], cols[cs[1]][rows[0]], cols[cs[2]][rows[0]],
            cols[cs[0]][rows[1]], cols[cs[1]][rows[1]], cols[cs[2]][rows[1]],
            cols[cs[0]][rows[2]], cols[cs[1]][rows[2]], cols[cs[2]][rows[2]]
        );

        return ((r + c) % 2 == 0) ? d : -d;
    }

    constexpr T determinant() const {
        T det = 0;
        for (int c = 0; c < 4; ++c) det += cols[c][0] * cofactor(0, c);
        return det;
    }

    constexpr Mat4 inverse() const {
        Mat4 cof;
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                cof.cols[c][r] = cofactor(r, c);

        T det = 0;
        for (int c = 0; c < 4; ++c) det += cols[c][0] * cof.cols[c][0];
        if (std::abs(det) < T(1e-8)) return identity();

        Mat4 inv;
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                inv.cols[c][r] = cof.cols[r][c] / det;

        return inv;
    }


    Mat4 transpose() const {
        return {{
            {cols[0][0], cols[1][0], cols[2][0], cols[3][0]},
            {cols[0][1], cols[1][1], cols[2][1], cols[3][1]},
            {cols[0][2], cols[1][2], cols[2][2], cols[3][2]},
            {cols[0][3], cols[1][3], cols[2][3], cols[3][3]},
        }};
    }


    constexpr Vec4<T> operator*(const Vec4<T>& v) const {
        return v.x * cols[0] + v.y * cols[1] + v.z * cols[2] + v.w * cols[3];
    }


    constexpr Mat4 operator*(const Mat4& m) const {
        return {{
            (*this) * m.cols[0],
            (*this) * m.cols[1],
            (*this) * m.cols[2],
            (*this) * m.cols[3]
        }};
    }
};

using Mat4f = Mat4<float>;
using Mat4d = Mat4<double>;
using Mat4i = Mat4<int32_t>;
using Mat4u = Mat4<uint32_t>;

static_assert(
    sizeof(Mat4f) == sizeof(float) * 16,
    "Mat4: Unexpected Padding"
);

} // namespace valo