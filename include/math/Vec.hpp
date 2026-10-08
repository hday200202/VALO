#pragma once

#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <algorithm>

// Anonymous structs inside a union are a compiler extension (every compiler we
// target supports them, ISO C++ does not). The swizzle unions below are wrapped
// so -Wpedantic / /W4 stay clean for anyone including this header.
#if defined(__clang__) || defined(__GNUC__)
    #define VALO_SWIZZLE_BEGIN         _Pragma("GCC diagnostic push")         _Pragma("GCC diagnostic ignored \"-Wpedantic\"")
    #define VALO_SWIZZLE_END _Pragma("GCC diagnostic pop")
#elif defined(_MSC_VER)
    #define VALO_SWIZZLE_BEGIN __pragma(warning(push)) __pragma(warning(disable : 4201))
    #define VALO_SWIZZLE_END   __pragma(warning(pop))
#else
    #define VALO_SWIZZLE_BEGIN
    #define VALO_SWIZZLE_END
#endif

namespace valo {

/* ========================================================================= //
    Vec2
// ========================================================================= */
template <typename T> struct Vec2 {
    T x = T(0), y = T(0);

    constexpr Vec2() {}
    constexpr Vec2(const T x, const T y) : x(x), y(y) {}

    T* data() { return &x; }
    const T* data() const { return &x; }

    constexpr T lengthSq() const { return x*x + y*y; }
    T length() const { return std::sqrt(lengthSq()); }
    
    constexpr T dot(const Vec2& v) const { return x*v.x + y*v.y; }
    constexpr T cross(const Vec2& v) const { return x*v.y - y*v.x; }

    Vec2 normalized() const { return *this / length(); }
    constexpr Vec2 perpendicular() const { return {-y, x}; }

    constexpr Vec2 operator+(const Vec2& v) const { return {x+v.x, y+v.y}; }
    constexpr Vec2 operator-(const Vec2& v) const { return {x-v.x, y-v.y}; }
    constexpr Vec2 operator*(const T s) const { return {x*s, y*s}; }
    constexpr Vec2 operator*(const Vec2& v) const { return {x*v.x, y*v.y}; }
    constexpr Vec2 operator/(const T s) const { return {x/s, y/s}; }
    constexpr Vec2 operator/(const Vec2& v) const { return {x/v.x, y/v.y}; }
    constexpr Vec2 operator-() const { return {-x, -y}; }

    constexpr Vec2& operator+=(const Vec2& v) { x += v.x; y += v.y; return *this; }
    constexpr Vec2& operator-=(const Vec2& v) { x -= v.x; y -= v.y; return *this; }
    constexpr Vec2& operator*=(const T s) { x *= s; y *= s; return *this; }
    constexpr Vec2& operator*=(const Vec2& v) { x *= v.x; y *= v.y; return *this; }
    constexpr Vec2& operator/=(const T s) { x /= s; y /= s; return *this; }
    constexpr Vec2& operator/=(const Vec2& v) { x /= v.x; y /= v.y; return *this; }

    T& operator[](const std::size_t i) { return data()[i]; }
    const T& operator[](const std::size_t i) const { return data()[i]; }

    constexpr bool operator==(const Vec2& v) const { return (x == v.x) && (y == v.y); }
    constexpr bool operator!=(const Vec2& v) const { return !(*this == v); }
};

using Vec2f = Vec2<float>;
using Vec2d = Vec2<double>;
using Vec2i = Vec2<int32_t>;
using Vec2u = Vec2<uint32_t>;

static_assert(
    sizeof(Vec2f) == sizeof(float) * 2, 
    "Vec2: unexpected padding"
);

template <typename T> Vec2<T> operator*(T s, Vec2<T> v) 
{ return {s * v.x, s * v.y}; }

// Vec2 Free Functions ===================================================== //
template <typename T> Vec2<T> min(const Vec2<T>& a, const Vec2<T>& b) 
{ return {std::min(a.x, b.x), std::min(a.y, b.y)}; }

template <typename T> Vec2<T> max(const Vec2<T>& a, const Vec2<T>& b)
{ return {std::max(a.x, b.x), std::max(a.y, b.y)}; }

template <typename T> Vec2<T> abs(const Vec2<T>& v)
{ return {std::abs(v.x), std::abs(v.y)}; }

template <typename T> Vec2<T> floor(const Vec2<T>& v)
{ return {std::floor(v.x), std::floor(v.y)}; }

template <typename T> Vec2<T> ceil(const Vec2<T>& v)
{ return {std::ceil(v.x), std::ceil(v.y)}; }

template <typename T> Vec2<T> lerp(const Vec2<T>& a, const Vec2<T>& b, const T t)
{ return a + ((b - a) * t); }



/* ========================================================================= //
    Vec3
// ========================================================================= */
template <typename T> struct Vec3 {
    VALO_SWIZZLE_BEGIN
    union {
        struct { T x, y, z; };
        Vec2<T> xy;
        struct { T _sx; Vec2<T> yz; };
    };
    VALO_SWIZZLE_END

    // MSVC rejects default member initializers inside an anonymous struct in a union
    constexpr Vec3() : x(T(0)), y(T(0)), z(T(0)) {}
    constexpr Vec3(const T x, const T y, const T z) : x(x), y(y), z(z) {}
    constexpr Vec3(const Vec2<T>& xy, const T z) : x(xy.x), y(xy.y), z(z) {}
    constexpr Vec3(const T x, const Vec2<T>& yz) : x(x), y(yz.x), z(yz.y) {}

    T* data() { return &x; }
    const T* data() const { return &x; }

    constexpr T lengthSq() const { return x*x + y*y + z*z; }
    T length() const { return std::sqrt(lengthSq()); }

    constexpr T dot(const Vec3& v) const { return x*v.x + y*v.y + z*v.z; }
    constexpr Vec3 cross(const Vec3& v) const { return {y*v.z - z*v.y, -(x*v.z - z*v.x), x*v.y - y*v.x}; }

    Vec3 normalized() const { return *this / length(); }

    constexpr Vec3 operator+(const Vec3& v) const { return {x+v.x, y+v.y, z+v.z}; }
    constexpr Vec3 operator-(const Vec3& v) const { return {x-v.x, y-v.y, z-v.z}; }
    constexpr Vec3 operator*(const T s) const { return {x*s, y*s, z*s}; }
    constexpr Vec3 operator*(const Vec3& v) const { return {x*v.x, y*v.y, z*v.z}; }
    constexpr Vec3 operator/(const T s) const { return {x/s, y/s, z/s}; }
    constexpr Vec3 operator/(const Vec3& v) const { return {x/v.x, y/v.y, z/v.z}; }
    constexpr Vec3 operator-() const { return {-x, -y, -z}; }

    constexpr Vec3& operator+=(const Vec3& v) { x += v.x; y += v.y; z += v.z; return *this; }
    constexpr Vec3& operator-=(const Vec3& v) { x -= v.x; y -= v.y; z -= v.z; return *this; }
    constexpr Vec3& operator*=(const T s) { x *= s; y *= s; z *= s; return *this; }
    constexpr Vec3& operator*=(const Vec3& v) { x *= v.x; y *= v.y; z *= v.z; return *this; }
    constexpr Vec3& operator/=(const T s) { x /= s; y /= s; z /= s; return *this; }
    constexpr Vec3& operator/=(const Vec3& v) { x /= v.x; y /= v.y; z /= v.z; return *this; }

    T& operator[](const std::size_t i) { return data()[i]; }
    const T& operator[](const std::size_t i) const { return data()[i]; }

    constexpr bool operator==(const Vec3& v) const { return (x == v.x) && (y == v.y) && (z == v.z); }
    constexpr bool operator!=(const Vec3& v) const { return !(*this == v); }
};

using Vec3f = Vec3<float>;
using Vec3d = Vec3<double>;
using Vec3i = Vec3<int32_t>;
using Vec3u = Vec3<uint32_t>;

static_assert(
    sizeof(Vec3f) == sizeof(float) * 3, 
    "Vec3: unexpected padding"
);

template <typename T> Vec3<T> operator*(T s, Vec3<T> v) 
{ return {s * v.x, s * v.y, s * v.z}; }

// Vec3 Free Functions ===================================================== //
template <typename T> Vec3<T> min(const Vec3<T>& a, const Vec3<T>& b) 
{ return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)}; }

template <typename T> Vec3<T> max(const Vec3<T>& a, const Vec3<T>& b)
{ return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)}; }

template <typename T> Vec3<T> abs(const Vec3<T>& v)
{ return {std::abs(v.x), std::abs(v.y), std::abs(v.z)}; }

template <typename T> Vec3<T> floor(const Vec3<T>& v)
{ return {std::floor(v.x), std::floor(v.y), std::floor(v.z)}; }

template <typename T> Vec3<T> ceil(const Vec3<T>& v)
{ return {std::ceil(v.x), std::ceil(v.y), std::ceil(v.z)}; }

template <typename T> Vec3<T> lerp(const Vec3<T>& a, const Vec3<T>& b, const T t)
{ return a + ((b - a) * t); }



/* ========================================================================= //
    Vec4
// ========================================================================= */
template <typename T> struct Vec4 {
    VALO_SWIZZLE_BEGIN
    union {
        struct { T x, y, z, w; };
        struct { T r, g, b, a; };
        Vec2<T> xy;
        Vec3<T> xyz;
        struct { T _sx; Vec2<T> yz; T _sz; };
        struct { T _ssx; T _ssy; Vec2<T> zw; };
        struct { T _sssx; Vec3<T> yzw; };
    };
    VALO_SWIZZLE_END

    constexpr Vec4() : x(T(0)), y(T(0)), z(T(0)), w(T(0)) {}
    constexpr Vec4(const T x, const T y, const T z, const T w) : x(x), y(y), z(z), w(w) {}
    constexpr Vec4(const Vec2<T>& xy, const T z, const T w) : x(xy.x), y(xy.y), z(z), w(w) {}
    constexpr Vec4(const T x, const Vec2<T>& yz, const T w) : x(x), y(yz.x), z(yz.y), w(w) {}
    constexpr Vec4(const T x, const T y, const Vec2<T>& zw) : x(x), y(y), z(zw.x), w(zw.y) {}
    constexpr Vec4(const Vec2<T>& xy, const Vec2<T>& zw) : x(xy.x), y(xy.y), z(zw.x), w(zw.y) {}
    constexpr Vec4(const Vec3<T>& xyz, const T w) : x(xyz.x), y(xyz.y), z(xyz.z), w(w) {}
    constexpr Vec4(const T x, const Vec3<T>& yzw) : x(x), y(yzw.x), z(yzw.y), w(yzw.z) {}

    T* data() { return &x; }
    const T* data() const { return &x; }

    constexpr T lengthSq() const { return x*x + y*y + z*z + w*w; }
    T length() const { return std::sqrt(lengthSq()); }

    constexpr T dot(const Vec4& v) const { return x*v.x + y*v.y + z*v.z + w*v.w; }
    constexpr Vec4 cross(const Vec4& v) const { return {y*v.z - z*v.y, -(x*v.z - z*v.x), x*v.y - y*v.x, 0}; }

    Vec4 normalized() const { return *this / length(); }

    constexpr Vec4 operator+(const Vec4& v) const { return {x+v.x, y+v.y, z+v.z, w+v.w}; }
    constexpr Vec4 operator-(const Vec4& v) const { return {x-v.x, y-v.y, z-v.z, w-v.w}; }
    constexpr Vec4 operator*(const T s) const { return {x*s, y*s, z*s, w*s}; }
    constexpr Vec4 operator*(const Vec4& v) const { return {x*v.x, y*v.y, z*v.z, w*v.w}; }
    constexpr Vec4 operator/(const T s) const { return {x/s, y/s, z/s, w/s}; }
    constexpr Vec4 operator/(const Vec4& v) const { return {x/v.x, y/v.y, z/v.z, w/v.w}; }
    constexpr Vec4 operator-() const { return {-x, -y, -z, -w}; }

    constexpr Vec4& operator+=(const Vec4& v) { x += v.x; y += v.y; z += v.z; w += v.w; return *this; }
    constexpr Vec4& operator-=(const Vec4& v) { x -= v.x; y -= v.y; z -= v.z; w -= v.w; return *this; }
    constexpr Vec4& operator*=(const T s) { x *= s; y *= s; z *= s; w *= s; return *this; }
    constexpr Vec4& operator*=(const Vec4& v) { x *= v.x; y *= v.y; z *= v.z; w *= v.w; return *this; }
    constexpr Vec4& operator/=(const T s) { x /= s; y /= s; z /= s; w /= s; return *this; }
    constexpr Vec4& operator/=(const Vec4& v) { x /= v.x; y /= v.y; z /= v.z; w /= v.w; return *this; }

    T& operator[](const std::size_t i) { return data()[i]; }
    const T& operator[](const std::size_t i) const { return data()[i]; }

    constexpr bool operator==(const Vec4& v) const { return (x == v.x) && (y == v.y) && (z == v.z) && (w == v.w); }
    constexpr bool operator!=(const Vec4& v) const { return !(*this == v); }
};

using Vec4f = Vec4<float>;
using Vec4d = Vec4<double>;
using Vec4i = Vec4<int32_t>;
using Vec4u = Vec4<uint32_t>;
using Color = Vec4f;

static_assert(
    sizeof(Vec4f) == sizeof(float) * 4, 
    "Vec4: unexpected padding"
);

template <typename T> Vec4<T> operator*(T s, Vec4<T> v) 
{ return {s * v.x, s * v.y, s * v.z, s * v.w}; }

// Vec4 Free Functions ===================================================== //
template <typename T> Vec4<T> min(const Vec4<T>& a, const Vec4<T>& b) 
{ return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z), std::min(a.w, b.w)}; }

template <typename T> Vec4<T> max(const Vec4<T>& a, const Vec4<T>& b)
{ return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z), std::max(a.w, b.w)}; }

template <typename T> Vec4<T> abs(const Vec4<T>& v)
{ return {std::abs(v.x), std::abs(v.y), std::abs(v.z), std::abs(v.w)}; }

template <typename T> Vec4<T> floor(const Vec4<T>& v)
{ return {std::floor(v.x), std::floor(v.y), std::floor(v.z), std::floor(v.w)}; }

template <typename T> Vec4<T> ceil(const Vec4<T>& v)
{ return {std::ceil(v.x), std::ceil(v.y), std::ceil(v.z), std::ceil(v.w)}; }

template <typename T> Vec4<T> clamp(const Vec4<T>& v, const T lo, const T hi) {
    return {
        std::clamp<T>(v.x, lo, hi), 
        std::clamp<T>(v.y, lo, hi), 
        std::clamp<T>(v.z, lo, hi), 
        std::clamp<T>(v.w, lo, hi)
    };
}

template <typename T> Vec4<T> lerp(const Vec4<T>& a, const Vec4<T>& b, const T t)
{ return a + ((b - a) * t); }

inline uint32_t colorToRGBA32(const Color& c) {
    auto col = clamp(c, 0.f, 1.f);
    uint8_t r = uint8_t(255 * col.x), g = uint8_t(255 * col.y), b = uint8_t(255 * col.z), a = uint8_t(255 * col.w);
    return (r << 24) | (g << 16) | (b << 8) | a;
}

inline Color RGBA32ToColor(const uint32_t c) {
    uint32_t r = static_cast<uint8_t>(c >> 24);
    uint32_t g = static_cast<uint8_t>(c >> 16);
    uint32_t b = static_cast<uint8_t>(c >> 8);
    uint32_t a = static_cast<uint8_t>(c);

    return Vec4f{r / 255.f, g / 255.f, b / 255.f, a / 255.f};
}

inline uint32_t colorToABGR32(const Color& c) {
    auto col = clamp(c, 0.f, 1.f);
    uint8_t r = uint8_t(255 * col.x), g = uint8_t(255 * col.y), b = uint8_t(255 * col.z), a = uint8_t(255 * col.w);
    return (a << 24) | (b << 16) | (g << 8) | r;
}

inline Color ABGR32ToColor(const uint32_t c) {
    uint32_t a = static_cast<uint8_t>(c >> 24);
    uint32_t b = static_cast<uint8_t>(c >> 16);
    uint32_t g = static_cast<uint8_t>(c >> 8);
    uint32_t r = static_cast<uint8_t>(c);

    return Vec4f{r / 255.f, g / 255.f, b / 255.f, a / 255.f};
}

template <typename T> T lerp(const T& a, const T& b, const T t)
{ return a + ((b - a) * t); }

} // namespace valo