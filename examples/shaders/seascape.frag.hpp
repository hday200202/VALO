#pragma once

#include "seascape.vert.hpp"

#include <cmath>
#include <algorithm>

/*
    "Seascape" by Alexander Alekseev aka TDM - 2014
    https://www.shadertoy.com/view/Ms2SD1
    License: Creative Commons Attribution-NonCommercial-ShareAlike 3.0 Unported.
*/

namespace sea {

constexpr int   NUM_STEPS       = 32;
constexpr float PI              = 3.141592f;
constexpr float EPSILON         = 1e-3f;
constexpr float TAU             = 6.2831853f;

constexpr int   ITER_GEOMETRY   = 3;
constexpr int   ITER_FRAGMENT   = 5;
constexpr float SEA_HEIGHT      = 0.6f;
constexpr float SEA_CHOPPY      = 4.0f;
constexpr float SEA_SPEED       = 0.8f;
constexpr float SEA_FREQ        = 0.16f;
constexpr Vec3f SEA_BASE        = { 0.0f, 0.09f, 0.18f };
constexpr Vec3f SEA_WATER_COLOR = { 0.48f, 0.54f, 0.36f };

inline float seaFract(const float x)                              { return x - std::floor(x); }
inline float seaMix(const float a, const float b, const float t)  { return a + (b - a) * t; }
inline Vec3f seaMix3(const Vec3f& a, const Vec3f& b, const float t) { return a + (b - a) * t; }

// AMD's hardware sin/cos are only accurate within about 256 turns; the upper
// octaves go past that far out to sea. Reducing to one turn first keeps both backends alike.
inline float seaSin(const float x) { return std::sin(x - TAU * std::floor(x / TAU)); }
inline float seaCos(const float x) { return std::cos(x - TAU * std::floor(x / TAU)); }

inline Vec3f seaReflect(const Vec3f& i, const Vec3f& n) { return i - n * (2.f * n.dot(i)); }

// the original's `dir * fromEuler(ang)`: a row vector times the matrix, so each
// component is the dot product with one column
inline Vec3f seaRotate(const Vec3f& d, const Vec3f& ang) {
    const float s1 = std::sin(ang.x), c1 = std::cos(ang.x);
    const float s2 = std::sin(ang.y), c2 = std::cos(ang.y);
    const float s3 = std::sin(ang.z), c3 = std::cos(ang.z);

    const Vec3f m0 = Vec3f(c1 * c3 + s1 * s2 * s3, c1 * s2 * s3 + c3 * s1, -c2 * s3);
    const Vec3f m1 = Vec3f(-c2 * s1, c1 * c2, s2);
    const Vec3f m2 = Vec3f(c3 * s1 * s2 + c1 * s3, s1 * s3 - c1 * c3 * s2, c2 * c3);

    return Vec3f(d.dot(m0), d.dot(m1), d.dot(m2));
}

// The original hashes with fract(sin(h) * 43758.5); h reaches the millions here,
// where float can't hold it and the GPU computes it differently per call site, so
// neighbouring normal samples disagree. The lattice point is an exact integer, so
// an integer hash is bit-identical on both backends.
inline float seaHash(const Vec2f& p) {
    uint32_t n = uint32_t(int32_t(p.x)) * 1597334677u ^ uint32_t(int32_t(p.y)) * 3812015801u;
    n = n * 0x27d4eb2du;
    n = n ^ (n >> 15u);
    return float(n >> 8u) * (1.f / 16777216.f);
}

inline float seaNoise(const Vec2f& p) {
    const Vec2f i = Vec2f(std::floor(p.x), std::floor(p.y));
    const Vec2f f = Vec2f(p.x - i.x, p.y - i.y);
    const Vec2f u = Vec2f(f.x * f.x * (3.f - 2.f * f.x), f.y * f.y * (3.f - 2.f * f.y));

    return -1.f + 2.f * seaMix(
        seaMix(seaHash(i + Vec2f(0.f, 0.f)), seaHash(i + Vec2f(1.f, 0.f)), u.x),
        seaMix(seaHash(i + Vec2f(0.f, 1.f)), seaHash(i + Vec2f(1.f, 1.f)), u.x),
        u.y);
}

inline float seaDiffuse(const Vec3f& n, const Vec3f& l, const float p) {
    return std::pow(n.dot(l) * 0.4f + 0.6f, p);
}

inline float seaSpecular(const Vec3f& n, const Vec3f& l, const Vec3f& e, const float s) {
    const float nrm = (s + 8.f) / (PI * 8.f);
    return std::pow(std::max(seaReflect(e, n).dot(l), 0.f), s) * nrm;
}

inline Vec3f seaSky(const Vec3f& e) {
    const float y = (std::max(e.y, 0.f) * 0.8f + 0.2f) * 0.8f;
    return Vec3f((1.f - y) * (1.f - y), 1.f - y, 0.6f + (1.f - y) * 0.4f) * 1.1f;
}

inline float seaOctave(const Vec2f& p, const float choppy) {
    const float n = seaNoise(p);
    const Vec2f uv = Vec2f(p.x + n, p.y + n);

    Vec2f wv = Vec2f(1.f - std::abs(seaSin(uv.x)), 1.f - std::abs(seaSin(uv.y)));
    const Vec2f swv = Vec2f(std::abs(seaCos(uv.x)), std::abs(seaCos(uv.y)));
    wv = Vec2f(seaMix(wv.x, swv.x, wv.x), seaMix(wv.y, swv.y, wv.y));

    return std::pow(1.f - std::pow(wv.x * wv.y, 0.65f), choppy);
}

// the original's map (3 octaves, for tracing) and map_detailed (5, for normals)
inline float seaMap(const Vec3f& p, const float seaTime, const int iterations) {
    float freq   = SEA_FREQ;
    float amp    = SEA_HEIGHT;
    float choppy = SEA_CHOPPY;
    Vec2f uv     = Vec2f(p.x * 0.75f, p.z);

    float h = 0.f;
    for (int i = 0; i < iterations; ++i) {
        float d = seaOctave(Vec2f(uv.x + seaTime, uv.y + seaTime) * freq, choppy);
        d = d + seaOctave(Vec2f(uv.x - seaTime, uv.y - seaTime) * freq, choppy);
        h = h + d * amp;

        uv     = Vec2f(1.6f * uv.x + 1.2f * uv.y, -1.2f * uv.x + 1.6f * uv.y);
        freq   = freq * 1.9f;
        amp    = amp * 0.22f;
        choppy = seaMix(choppy, 1.f, 0.2f);
    }
    return p.y - h;
}

inline Vec3f seaColor(const Vec3f& p, const Vec3f& n, const Vec3f& l, const Vec3f& eye, const Vec3f& dist) {
    float fresnel = std::clamp(1.f - n.dot(eye * -1.f), 0.f, 1.f);
    fresnel = std::min(fresnel * fresnel * fresnel, 0.5f);

    const Vec3f reflected = seaSky(seaReflect(eye, n));
    const Vec3f refracted = SEA_BASE + SEA_WATER_COLOR * (seaDiffuse(n, l, 80.f) * 0.12f);

    Vec3f color = seaMix3(refracted, reflected, fresnel);

    const float d2    = dist.dot(dist);
    const float atten = std::max(1.f - d2 * 0.001f, 0.f);
    color = color + SEA_WATER_COLOR * ((p.y - SEA_HEIGHT) * 0.18f * atten);

    const float spec = seaSpecular(n, l, eye, 600.f / std::sqrt(d2));
    return color + Vec3f(spec, spec, spec);
}

inline Vec3f seaNormal(const Vec3f& p, const float eps, const float seaTime) {
    const float h = seaMap(p, seaTime, ITER_FRAGMENT);
    const float x = seaMap(Vec3f(p.x + eps, p.y, p.z), seaTime, ITER_FRAGMENT) - h;
    const float z = seaMap(Vec3f(p.x, p.y, p.z + eps), seaTime, ITER_FRAGMENT) - h;
    return Vec3f(x, eps, z).normalized();
}

// bisection between a point above the surface and one below it
inline Vec3f seaTrace(const Vec3f& ori, const Vec3f& dir, const float seaTime) {
    float tm = 0.f;
    float tx = 1000.f;
    float hx = seaMap(ori + dir * tx, seaTime, ITER_GEOMETRY);
    if (hx > 0.f) return ori + dir * tx;

    float hm = seaMap(ori, seaTime, ITER_GEOMETRY);
    Vec3f p  = ori;

    for (int i = 0; i < NUM_STEPS; ++i) {
        const float tmid = seaMix(tm, tx, hm / (hm - hx));
        p = ori + dir * tmid;
        const float hmid = seaMap(p, seaTime, ITER_GEOMETRY);

        if (hmid < 0.f) { tx = tmid; hx = hmid; }
        else            { tm = tmid; hm = hmid; }

        if (std::abs(hmid) < EPSILON) break;
    }
    return p;
}

inline void seaFS(const SeaVaryings& in, const SeaUniforms& u) {
    const float time    = u.time * 0.3f;
    const float seaTime = 1.f + u.time * SEA_SPEED;

    const Vec2f uv = Vec2f(in.ndc.x * u.aspect, in.ndc.y);

    const Vec3f ang = Vec3f(std::sin(time * 3.f) * 0.1f, std::sin(time) * 0.2f + 0.3f, time);
    const Vec3f ori = Vec3f(0.f, 3.5f, time * 5.f);

    Vec3f dir = Vec3f(uv.x, uv.y, -2.f).normalized();
    dir.z = dir.z + std::sqrt(uv.x * uv.x + uv.y * uv.y) * 0.14f;
    dir = seaRotate(dir.normalized(), ang);

    const Vec3f p     = seaTrace(ori, dir, seaTime);
    const Vec3f dist  = p - ori;
    const Vec3f n     = seaNormal(p, dist.dot(dist) * (0.1f / u.width), seaTime);
    const Vec3f light = Vec3f(0.f, 1.f, 0.8f).normalized();

    const float t   = std::clamp(dir.y / -0.02f, 0.f, 1.f);
    const float sea = std::pow(t * t * (3.f - 2.f * t), 0.2f);
    const Vec3f color = seaMix3(seaSky(dir), seaColor(p, n, light, dir, dist), sea);

    valo_FragColor = Color(std::pow(color.x, 0.65f), std::pow(color.y, 0.65f), std::pow(color.z, 0.65f), 1.f);
}

VALO_FRAGMENT_SHADER(seaFS)

} // namespace sea
