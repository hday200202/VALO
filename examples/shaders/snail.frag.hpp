#pragma once

#include "snail.vert.hpp"

#include <cmath>
#include <algorithm>

/*
    "Snail" by Inigo Quilez (Shadertoy).
    License: Creative Commons Attribution-NonCommercial-ShareAlike 3.0 Unported.

    iChannel1/2 -> u.noise (text1 reads .w), iChannel3 -> u.grain, and the
    200-tap background blur is baked into u.sky on the CPU.
*/

namespace snail {

constexpr float PI      = 3.1415927f;
constexpr Vec3f SUN_DIR = { 0.8908708f, 0.4454354f, 0.0890871f };

// GLSL builtins - valoc skips these and the call binds to the real one
inline float fract(const float x)                                       { return x - std::floor(x); }
inline float sign(const float x)                                        { return x > 0.f ? 1.f : (x < 0.f ? -1.f : 0.f); }
inline float clamp(const float x, const float lo, const float hi)       { return std::min(std::max(x, lo), hi); }
inline Vec3f clamp(const Vec3f& v, const float lo, const float hi)      { return Vec3f(clamp(v.x, lo, hi), clamp(v.y, lo, hi), clamp(v.z, lo, hi)); }
inline float mix(const float a, const float b, const float t)           { return a + (b - a) * t; }
inline Vec2f mix(const Vec2f& a, const Vec2f& b, const float t)         { return a + (b - a) * t; }
inline Vec3f mix(const Vec3f& a, const Vec3f& b, const float t)         { return a + (b - a) * t; }
inline float smoothstep(const float a, const float b, const float x)    { const float t = clamp((x - a) / (b - a), 0.f, 1.f); return t * t * (3.f - 2.f * t); }
inline float dot(const Vec2f& a, const Vec2f& b)                        { return a.dot(b); }
inline float dot(const Vec3f& a, const Vec3f& b)                        { return a.dot(b); }
inline float length(const Vec2f& v)                                     { return v.length(); }
inline float length(const Vec3f& v)                                     { return v.length(); }
inline float distance(const Vec2f& a, const Vec2f& b)                   { return (a - b).length(); }
inline Vec3f normalize(const Vec3f& v)                                  { return v.normalized(); }
inline Vec3f cross(const Vec3f& a, const Vec3f& b)                      { return a.cross(b); }
inline Vec3f reflect(const Vec3f& i, const Vec3f& n)                    { return i - n * (2.f * n.dot(i)); }
inline Vec3f max(const Vec3f& v, const float s)                         { return Vec3f(std::max(v.x, s), std::max(v.y, s), std::max(v.z, s)); }
inline Vec3f sin(const Vec3f& v)                                        { return Vec3f(std::sin(v.x), std::sin(v.y), std::sin(v.z)); }
inline Vec3f pow(const Vec3f& v, const Vec3f& e)                        { return Vec3f(std::pow(v.x, e.x), std::pow(v.y, e.y), std::pow(v.z, e.z)); }
inline float atan(const float y, const float x)                         { return std::atan2(y, x); }

// GLSL's mat3(c0, c1, c2) * v
inline Vec3f mulCols(const Vec3f& c0, const Vec3f& c1, const Vec3f& c2, const Vec3f& v) {
    return c0 * v.x + c1 * v.y + c2 * v.z;
}

inline Vec3f leafSpace(const Vec3f& p) {
    return mulCols(Vec3f( 0.671212f, 0.366685f, -0.644218f),
                   Vec3f(-0.479426f, 0.877583f,  0.000000f),
                   Vec3f( 0.565354f, 0.308854f,  0.764842f), p);
}

inline float sdSphere(const Vec3f& p, const Vec4f& s) {
    return length(p - s.xyz) - s.w;
}

inline float sdEllipsoid(const Vec3f& p, const Vec3f& c, const Vec3f& r) {
    return (length((p - c) / r) - 1.f) * std::min(std::min(r.x, r.y), r.z);
}

inline float sdCircle(const Vec2f& p, const Vec2f& c, const float r) {
    return length(p - c) - r;
}

inline float sdTorus(const Vec3f& p, const Vec2f& t) {
    return length(Vec2f(length(Vec2f(p.x, p.z)) - t.x, p.y)) - t.y;
}

inline float sdCapsule(const Vec3f& p, const Vec3f& a, const Vec3f& b, const float r) {
    const Vec3f pa = p - a;
    const Vec3f ba = b - a;
    const float h  = clamp(dot(pa, ba) / dot(ba, ba), 0.f, 1.f);
    return length(pa - ba * h) - r;
}

inline Vec2f udSegment(const Vec3f& p, const Vec3f& a, const Vec3f& b) {
    const Vec3f pa = p - a;
    const Vec3f ba = b - a;
    const float h  = clamp(dot(pa, ba) / dot(ba, ba), 0.f, 1.f);
    return Vec2f(length(pa - ba * h), h);
}

inline float det(const Vec2f& a, const Vec2f& b) { return a.x * b.y - b.x * a.y; }

inline Vec3f getClosest(const Vec2f& b0, const Vec2f& b1, const Vec2f& b2) {
    const float a = det(b0, b2);
    const float b = 2.f * det(b1, b0);
    const float d = 2.f * det(b2, b1);
    const float f = b * d - a * a;

    const Vec2f d21 = b2 - b1;
    const Vec2f d10 = b1 - b0;
    const Vec2f d20 = b2 - b0;

    Vec2f gf = (d21 * b + d10 * d + d20 * a) * 2.f;
    gf = Vec2f(gf.y, -gf.x);

    const Vec2f pp  = gf * (-f / dot(gf, gf));
    const Vec2f d0p = b0 - pp;
    const float ap  = det(d0p, d20);
    const float bp  = 2.f * det(d10, d0p);
    const float t   = clamp((ap + bp) / (2.f * a + b + d), 0.f, 1.f);

    return Vec3f(mix(mix(b0, b1, t), mix(b1, b2, t), t), t);
}

inline Vec4f sdBezier(const Vec3f& a, const Vec3f& b, const Vec3f& c, const Vec3f& p) {
    const Vec3f ww = normalize(cross(c - b, a - b));
    const Vec3f uu = normalize(c - b);
    const Vec3f vv = cross(ww, uu);

    const Vec2f a2 = Vec2f(dot(a - b, uu), dot(a - b, vv));
    const Vec2f b2 = Vec2f(0.f, 0.f);
    const Vec2f c2 = Vec2f(dot(c - b, uu), dot(c - b, vv));
    const Vec3f p3 = Vec3f(dot(p - b, uu), dot(p - b, vv), dot(p - b, ww));

    const Vec3f cp = getClosest(a2 - p3.xy, b2 - p3.xy, c2 - p3.xy);

    return Vec4f(std::sqrt(dot(cp.xy, cp.xy) + p3.z * p3.z), cp.z, length(cp.xy), p3.z);
}

inline float smin(const float a, const float b, const float k) {
    const float h = std::max(k - std::abs(a - b), 0.f);
    return std::min(a, b) - h * h * 0.25f / k;
}

inline float smax(const float a, const float b, const float k) {
    const float h = std::max(k - std::abs(a - b), 0.f);
    return std::max(a, b) + h * h * 0.25f / k;
}

inline float hash1(const float n) {
    return fract(std::sin(n) * 43758.5453123f);
}

inline Vec3f forwardSF(const float i, const float n) {
    const float PHI      = 1.618033988749894848f;
    const float phi      = 2.f * PI * fract(i / PHI);
    const float zi       = 1.f - (2.f * i + 1.f) / n;
    const float sinTheta = std::sqrt(1.f - zi * zi);
    return Vec3f(std::cos(phi) * sinTheta, std::sin(phi) * sinTheta, zi);
}

//---------------------------------------------------------------------------

inline float mapShell(const SnailUniforms& u, Vec3f p, Vec4f& matInfo) {
    p -= Vec3f(0.05f, 0.12f, -0.09f);

    Vec3f q = mulCols(Vec3f(-0.6333234236f, -0.7332753384f, 0.2474039592f),
                      Vec3f( 0.7738444477f, -0.6034162289f, 0.1924931824f),
                      Vec3f( 0.0081370606f,  0.3133626215f, 0.9495986813f), p);

    const float b = 0.1759f;

    const float r = length(q.xy);
    const float t = atan(q.y, q.x);

    // https://swiftcoder.wordpress.com/2010/06/21/logarithmic-spiral-distance-field/
    const float np = (std::log(r)     / b - t) / (2.f * PI);
    const float nm = (std::log(0.11f) / b - t) / (2.f * PI);
    const float n  = std::min(np, nm);
    const float ni = std::floor(n);

    const float r1 = std::exp(b * (t + 2.f * PI * ni));
    const float r2 = r1 * 3.019863f;

    const float h1 = q.z + 1.5f * r1 - 0.5f; const float d1 = std::sqrt((r1 - r) * (r1 - r) + h1 * h1) - r1;
    const float h2 = q.z + 1.5f * r2 - 0.5f; const float d2 = std::sqrt((r2 - r) * (r2 - r) + h2 * h2) - r2;

    float d  = d2;
    float dx = r2 - r;
    float dy = h2;
    if (d1 < d2) { d = d1; dx = r1 - r; dy = h1; }

    const float di = texture(u.noise, Vec2f(t + r, 0.5f)).x;
    d += 0.002f * di;

    matInfo = Vec4f(dx, dy, r / 0.4f, t / PI);

    const Vec3f s = q;
    q = q - Vec3f(0.34f, -0.1f, 0.03f);
    q.xy = Vec2f(0.8f * q.x - 0.6f * q.y, 0.6f * q.x + 0.8f * q.y);
    d = smin(d,  sdTorus(q, Vec2f(0.28f, 0.05f)), 0.06f);
    d = smax(d, -sdEllipsoid(q, Vec3f(0.f, 0.f, 0.f), Vec3f(0.24f, 0.36f, 0.24f)), 0.03f);
    d = smax(d, -sdEllipsoid(s, Vec3f(0.52f, 0.f, 0.f), Vec3f(0.42f, 0.23f, 0.5f)), 0.05f);

    return d;
}

inline float text1(const SnailUniforms& u, const Vec2f& p) {
    return texture(u.noise, p).w;
}

inline Vec2f mapSnail(const SnailUniforms& u, const Vec3f& p, Vec4f& matInfo) {
    const Vec3f head = Vec3f(-0.76f, 0.6f, -0.3f);

    Vec3f q = p - head;

    // body
    Vec4f b1 = sdBezier(Vec3f(-0.13f, -0.65f, 0.f), Vec3f(0.24f, 1.f, 0.f), head + Vec3f(0.04f, 0.01f, 0.f), p);
    float d1 = b1.x;
    d1 -= smoothstep(0.f, 0.2f, b1.y) * (0.16f - 0.07f * smoothstep(0.5f, 1.f, b1.y));
    b1 = sdBezier(Vec3f(-0.085f, 0.f, 0.f), Vec3f(-0.1f, 0.85f, 0.f), head + Vec3f(0.06f, -0.08f, 0.f), p);
    float d2 = b1.x;
    d2 -= 0.1f - 0.06f * b1.y;
    d1 = smin(d1, d2, 0.03f);
    matInfo.xyz = Vec3f(b1.y, b1.z, b1.w);

    d2 = sdSphere(q, Vec4f(0.f, -0.06f, 0.f, 0.085f));
    d1 = smin(d1, d2, 0.03f);
    d1 = smin(d1, sdSphere(p, Vec4f(0.05f, 0.52f, 0.f, 0.13f)), 0.07f);

    q = Vec3f(0.8f * q.x - 0.6f * q.z, q.y, 0.6f * q.x + 0.8f * q.z);

    const Vec3f sq = Vec3f(q.x, q.y, std::abs(q.z));
    const float sz = sign(q.z);

    // top antennas
    const Vec3f af = sin(Vec3f(0.f, 1.f, 3.f) + Vec3f(2.f, 1.f, 0.f) * sz + Vec3f(1.f, 1.f, 1.f) * (0.5f * u.time)) * 0.05f;
    const Vec4f b2 = sdBezier(Vec3f(0.f, 0.f, 0.f), Vec3f(-0.1f, 0.2f, 0.2f), Vec3f(-0.3f, 0.2f, 0.3f) + af, sq);
    float d3 = b2.x;
    d3 -= 0.03f - 0.025f * b2.y;
    d1 = smin(d1, d3, 0.04f);
    d3 = sdSphere(sq, Vec4f(-0.3f, 0.2f, 0.3f, 0.016f) + Vec4f(af, 0.f));
    d1 = smin(d1, d3, 0.01f);

    // bottom antennas
    const Vec3f bf = sin(Vec3f(4.f, 1.f, 2.f) + Vec3f(3.f, 0.f, 1.f) * sz + Vec3f(1.f, 1.f, 1.f) * (0.3f * u.time)) * 0.02f;
    const Vec2f b3 = udSegment(sq, Vec3f(0.06f, -0.05f, 0.f), Vec3f(-0.04f, -0.2f, 0.18f) + bf);
    d3 = b3.x;
    d3 -= 0.025f - 0.02f * b3.y;
    d1 = smin(d1, d3, 0.06f);
    d3 = sdSphere(sq, Vec4f(-0.04f, -0.2f, 0.18f, 0.008f) + Vec4f(bf, 0.f));
    d1 = smin(d1, d3, 0.02f);

    // bottom
    Vec3f pp = p - Vec3f(-0.17f, 0.15f, 0.f);
    const float co = 0.988771078f;
    const float si = 0.149438132f;
    pp.xy = Vec2f(co * pp.x + si * pp.y, -si * pp.x + co * pp.y);
    d1 = smin(d1,  sdEllipsoid(pp, Vec3f(0.f, 0.f, 0.f), Vec3f(0.084f, 0.3f, 0.15f)), 0.05f);
    d1 = smax(d1, -sdEllipsoid(pp, Vec3f(-0.08f, 0.f, 0.f), Vec3f(0.06f, 0.55f, 0.1f)), 0.02f);

    // displacement
    const float dis = text1(u, p.xy * 5.f);
    const float dx  = 0.5f + 0.5f * (1.f - smoothstep(0.5f, 1.f, b1.y));
    d1 -= 0.005f * dis * dx * 0.5f;

    return Vec2f(d1, 1.f);
}

inline float mapDrop(Vec3f p) {
    p -= Vec3f(-0.26f, 0.25f, -0.02f);
    p.x -= 2.5f * p.y * p.y;
    return sdCapsule(p, Vec3f(0.f, -0.06f, 0.f), Vec3f(0.014f, 0.06f, 0.f), 0.037f);
}

inline float mapLeaf(Vec3f p) {
    p = leafSpace(p - Vec3f(-1.8f, 0.6f, -0.75f));

    p.y += 0.2f * std::exp2(-std::abs(2.9f * p.z));

    const float ph = 12.5f * p.x - 18.75f * std::abs(p.z);
    float rr = std::sin(ph);
    rr = rr * rr;
    rr = rr * rr;
    p.y += 0.005f * rr;

    float r = clamp((p.x + 2.f) / 4.f, 0.f, 1.f);
    r = 0.0001f + r * (1.f - r) * (1.f - r) * 6.f;

    rr = std::sin(ph * 2.f);
    rr = rr * rr;
    rr *= 0.5f + 0.5f * std::sin(p.x * 12.f);

    const float ri = 0.035f * rr;

    const float d  = sdEllipsoid(p, Vec3f(0.f, 0.f, 0.f), Vec3f(2.f, 0.25f * r, r + ri));
    const float d2 = p.y - 0.02f;

    return smax(d, -d2, 0.02f);
}

inline Vec2f mapOpaque(const SnailUniforms& u, const Vec3f& p, Vec4f& matInfo) {
    matInfo = Vec4f(0.f, 0.f, 0.f, 0.f);

    // leaf
    Vec2f res = Vec2f(mapLeaf(p), 4.f);

    // stem
    const Vec4f b3 = sdBezier(Vec3f(-0.15f, -1.5f, 0.f), Vec3f(-0.1f, 0.5f, 0.f), Vec3f(-0.6f, 1.5f, 0.f), p);
    const float d3 = b3.x - 0.04f + 0.02f * b3.y;
    if (d3 < res.x) res = Vec2f(d3, 3.f);

    // snail
    const float boundingVolume = sdCapsule(p, Vec3f(0.f, 0.f, 0.f), Vec3f(-0.6f, 0.7f, 0.f), 0.55f);
    if (boundingVolume < res.x) {
        const Vec2f tmp = mapSnail(u, p, matInfo);
        if (tmp.x < res.x) res = tmp;

        // shell
        const float bb = length(p - Vec3f(0.25f, 0.3f, -0.1f)) - 0.6f;
        if (bb < res.x) {
            Vec4f tmpMatInfo = Vec4f(0.f, 0.f, 0.f, 0.f);
            const float d4 = mapShell(u, p, tmpMatInfo);
            if (d4 < res.x) { res = Vec2f(d4, 2.f); matInfo = tmpMatInfo; }
        }
    }

    return res;
}

inline Vec3f calcNormalOpaque(const SnailUniforms& u, const Vec3f& pos, const float eps) {
    Vec4f kk = Vec4f(0.f, 0.f, 0.f, 0.f);
    Vec3f n  = Vec3f(0.f, 0.f, 0.f);
    for (int i = 0; i < 4; i++) {
        const Vec3f e = (Vec3f(float(((i + 3) >> 1) & 1), float((i >> 1) & 1), float(i & 1)) * 2.f - Vec3f(1.f, 1.f, 1.f)) * 0.5773f;
        n += e * mapOpaque(u, pos + e * eps, kk).x;
    }
    return normalize(n);
}

//=========================================================================

inline float mapLeafWaterDrops(Vec3f p) {
    p -= Vec3f(-1.8f, 0.6f, -0.75f);
    const Vec3f s = p;
    p = leafSpace(p);
    const Vec3f q = p;
    p.y += 0.2f * std::exp(-std::abs(2.f * p.z));

    const float d1 = sdEllipsoid(q, Vec3f( 0.5f,   0.f,   0.2f),  Vec3f(0.15f,  0.13f,  0.15f));
    const float d2 = sdEllipsoid(q, Vec3f( 0.8f,  -0.07f, -0.15f), Vec3f(0.075f, 0.065f, 0.075f));
    float       d3 = sdEllipsoid(s, Vec3f( 0.76f, -0.8f,  0.6f),  Vec3f(0.075f, 0.1f,   0.075f));
    const float d4 = sdEllipsoid(q, Vec3f(-0.5f,   0.09f, -0.2f),  Vec3f(0.04f,  0.03f,  0.04f));

    d3 = std::max(d3, p.y - 0.01f);

    return std::min(std::min(d1, d4), std::min(d2, d3));
}

inline float mapTransparent(const Vec3f& p) {
    return std::min(mapDrop(p), mapLeafWaterDrops(p));
}

inline Vec3f calcNormalTransparent(const Vec3f& pos, const float eps) {
    const float a = 0.5773f * eps;
    return normalize(Vec3f( a, -a, -a) * mapTransparent(pos + Vec3f( a, -a, -a))
                   + Vec3f(-a, -a,  a) * mapTransparent(pos + Vec3f(-a, -a,  a))
                   + Vec3f(-a,  a, -a) * mapTransparent(pos + Vec3f(-a,  a, -a))
                   + Vec3f( a,  a,  a) * mapTransparent(pos + Vec3f( a,  a,  a)));
}

//=========================================================================

inline float calcAO(const SnailUniforms& u, const Vec3f& pos, const Vec3f& nor) {
    Vec4f kk = Vec4f(0.f, 0.f, 0.f, 0.f);
    float ao = 0.f;
    for (int i = 0; i < 32; i++) {
        Vec3f ap = forwardSF(float(i), 32.f);
        const float h = hash1(float(i));
        ap *= sign(dot(ap, nor)) * h * 0.1f;
        ao += clamp(mapOpaque(u, pos + nor * 0.01f + ap, kk).x * 3.f, 0.f, 1.f);
    }
    ao /= 32.f;

    return clamp(ao * 6.f, 0.f, 1.f);
}

inline float calcSSS(const SnailUniforms& u, const Vec3f& pos, const Vec3f& nor) {
    Vec4f kk = Vec4f(0.f, 0.f, 0.f, 0.f);
    float occ = 0.f;
    for (int i = 0; i < 8; i++) {
        const float h = 0.002f + 0.11f * float(i) / 7.f;
        Vec3f dir = normalize(sin(Vec3f(0.f, 2.1f, 4.2f) + Vec3f(1.f, 1.f, 1.f) * (float(i) * 13.f)));
        dir *= sign(dot(dir, nor));
        occ += h - mapOpaque(u, pos - dir * h, kk).x;
    }
    occ = clamp(1.f - 11.f * occ / 8.f, 0.f, 1.f);
    return occ * occ;
}

inline float calcSoftShadow(const SnailUniforms& u, const Vec3f& ro, const Vec3f& rd, const float k) {
    Vec4f kk  = Vec4f(0.f, 0.f, 0.f, 0.f);
    float res = 1.f;
    float t   = 0.01f;
    for (int i = 0; i < 32; i++) {
        const float h = mapOpaque(u, ro + rd * t, kk).x;
        res = std::min(res, smoothstep(0.f, 1.f, k * h / t));
        t += clamp(h, 0.04f, 0.1f);
        if (res < 0.01f) break;
    }
    return clamp(res, 0.f, 1.f);
}

inline Vec3f shadeOpaque(const SnailUniforms& u, const Vec3f& ro, const Vec3f& rd, const float t, const float m, const Vec4f& matInfo) {
    const float eps = 0.002f;

    const Vec3f pos = ro + rd * t;
    Vec3f nor = calcNormalOpaque(u, pos, eps);

    Vec3f mateD = Vec3f(0.f, 0.f, 0.f);
    Vec3f mateS = Vec3f(0.f, 0.f, 0.f);
    Vec2f mateK = Vec2f(0.f, 0.f);
    Vec3f mateE = Vec3f(0.f, 0.f, 0.f);

    float focc = 1.f;
    float fsha = 1.f;

    if (m < 1.5f) { // snail body
        const float dis = text1(u, pos.xy * 5.f);

        float be = sdEllipsoid(pos, Vec3f(-0.3f, -0.5f, -0.1f), Vec3f(0.2f, 1.f, 0.5f));
        be = 1.f - smoothstep(-0.01f, 0.01f, be);

        const float ff = std::abs(matInfo.x - 0.2f);

        mateS = mix(Vec3f(2.f, 1.2f, 0.2f) * 0.7f, Vec3f(2.5f, 1.8f, 0.9f), ff) * 6.f;
        mateS += Vec3f(dis, dis, dis) * 2.f;
        mateS *= 1.5f;
        mateS *= 1.f + 0.5f * ff * ff;
        mateS *= 1.f - 0.5f * be;

        mateD = Vec3f(1.f, 0.8f, 0.4f) * (dis * 0.015f) + Vec3f(0.8f, 0.4f, 0.3f) * (0.15f * be);

        mateK = Vec2f(60.f, 0.7f + 2.f * dis);

        float f = clamp(dot(-rd, nor), 0.f, 1.f);
        f = 1.f - std::pow(f, 8.f);
        f = 1.f - (1.f - f) * (1.f - texture(u.noise, pos.xy * 0.3f).x);
        mateS *= Vec3f(0.5f, 0.1f, 0.f) + Vec3f(0.5f, 0.9f, 1.f) * f;

        focc = 0.2f + 0.8f * smoothstep(0.f, 0.15f, sdSphere(pos, Vec4f(0.05f, 0.52f, 0.f, 0.13f)));
    }
    else if (m < 2.5f) { // shell
        const float tip = 1.f - smoothstep(0.05f, 0.4f, length(pos - Vec3f(0.17f, 0.2f, 0.35f)));
        mateD = mix(Vec3f(0.2f, 0.21f, 0.22f) * 0.7f, Vec3f(0.15f, 0.1f, 0.f) * 0.2f, tip);

        const Vec2f uv = Vec2f(0.5f * atan(matInfo.x, matInfo.y) / 3.1416f, 1.5f * matInfo.w);

        const float ff  = 2.f * matInfo.w + matInfo.z;
        const float ral = 0.4f + 0.4f * (0.26f * std::sin(ff * 6.283185f *  1.f + 0.f)
                                       + 0.23f * std::sin(ff * 6.283185f *  3.f + 2.f)
                                       + 0.20f * std::sin(ff * 6.283185f *  8.f + 1.f)
                                       + 0.17f * std::sin(ff * 6.283185f * 17.f + 3.f)
                                       + 0.14f * std::sin(ff * 6.283185f * 25.f + 2.f));
        mateD *= 0.25f + 0.75f * ral;

        const float pa = smoothstep(-0.2f, 0.2f, 0.3f + std::sin(2.f + 40.f * uv.x + 3.f * std::sin(11.f * uv.x)));
        float bar = mix(pa, 1.f, smoothstep(0.7f, 1.f, tip));
        bar *= (matInfo.z < 0.6f) ? 1.f : smoothstep(0.17f, 0.21f, std::abs(matInfo.w));
        mateD *= Vec3f(0.06f, 0.03f, 0.f) + Vec3f(0.94f, 0.97f, 1.f) * bar;

        mateK = Vec2f(64.f, 0.2f);
        mateS = Vec3f(1.f, 0.65f, 0.6f) * (1.5f * (1.f - tip));
    }
    else if (m < 3.5f) { // plant
        mateD = Vec3f(0.05f, 0.1f, 0.f) * 0.2f;
        mateS = Vec3f(0.1f, 0.2f, 0.02f) * 25.f;
        mateK = Vec2f(5.f, 1.f);

        const float fre = clamp(1.f + dot(nor, rd), 0.f, 1.f);
        mateD += Vec3f(1.f, 0.5f, 0.1f) * (0.2f * fre);

        const Vec3f te = texture(u.noise, pos.xy * 0.2f).xyz;
        mateS *= Vec3f(0.5f, 0.5f, 0.5f) + te * 1.5f;
        mateE = Vec3f(0.1f, 0.1f, 0.03f) * (0.5f * (0.2f + 0.8f * te.x));
    }
    else { // leaf
        Vec3f p = leafSpace(pos - Vec3f(-1.8f, 0.6f, -0.75f));

        const Vec3f q = p;
        p.y += 0.2f * std::exp(-std::abs(2.f * p.z));

        const float v  = smoothstep(0.01f, 0.02f, std::abs(p.z));
        const float rr = std::sin(50.f * p.x - 75.f * std::abs(p.z));

        const Vec3f te = texture(u.noise, Vec2f(p.x, p.z) * 0.35f).xyz;

        float r = clamp((p.x + 2.f) / 4.f, 0.f, 1.f);
        r = r * (1.f - r) * (1.f - r) * 6.f;
        const float ff = length(Vec2f(p.x, p.z) / Vec2f(2.f, r));

        mateD = mix(Vec3f(0.07f, 0.1f, 0.f), Vec3f(0.05f, 0.2f, 0.01f) * 0.25f, v);
        mateD = mix(mateD, Vec3f(0.16f, 0.2f, 0.01f) * 0.25f, ff);
        mateD *= Vec3f(1.f, 1.f, 1.f) + te * 0.25f;
        mateD *= 0.8f;

        mateS = Vec3f(0.15f, 0.2f, 0.02f) * 0.8f;
        mateS *= 1.f + 0.2f * rr;
        mateS *= 0.8f;

        mateK = Vec2f(64.f, 0.25f);

        //---------------------

        const Vec2f nt = texture(u.grain, Vec2f(p.x, p.z)).xy;
        nor.x += v * 0.15f * (-1.f + 2.f * nt.x);
        nor.z += v * 0.15f * (-1.f + 2.f * nt.y);
        nor = normalize(nor);

        float d1 = sdEllipsoid(q, Vec3f( 0.43f,  0.f,   0.2f),  Vec3f(0.21f,  0.13f,  0.15f));
        float d2 = sdEllipsoid(q, Vec3f( 0.75f, -0.07f, -0.15f), Vec3f(0.0975f, 0.065f, 0.075f));
        float d4 = sdEllipsoid(q, Vec3f(-0.57f,  0.09f, -0.2f),  Vec3f(0.056f, 0.03f,  0.04f));
        float dd = std::min(d1, std::min(d2, d4));
        fsha = 0.05f + 0.95f * smoothstep(0.f, 0.05f, dd);

        const Vec2f qxz = Vec2f(q.x, q.z);
        d1 = std::abs(sdCircle(qxz, Vec2f( 0.5f,  0.2f),  0.15f));
        d2 = std::abs(sdCircle(qxz, Vec2f( 0.8f, -0.15f), 0.075f));
        d4 = std::abs(sdCircle(qxz, Vec2f(-0.5f, -0.2f),  0.04f));
        dd = std::min(d1, std::min(d2, d4));
        focc *= 0.55f + 0.45f * smoothstep(0.f, 0.08f, dd);

        d1 = distance(qxz, Vec2f(0.43f,  0.2f));
        d2 = distance(qxz, Vec2f(0.77f, -0.15f));
        fsha += (1.f - smoothstep(0.f, 0.1f,  d1)) * 1.5f;
        fsha += (1.f - smoothstep(0.f, 0.05f, d2)) * 1.5f;
    }

    const Vec3f hal = normalize(SUN_DIR - rd);
    const float fre = clamp(1.f + dot(nor, rd), 0.f, 1.f);
    const float occ = calcAO(u, pos, nor) * focc;
    float sss = calcSSS(u, pos, nor);
    sss = sss * occ + fre * occ + (0.5f + 0.5f * fre) * std::abs(matInfo.x - 0.2f) * occ;

    float dif1 = clamp(dot(nor, SUN_DIR), 0.f, 1.f);
    dif1 *= fsha;
    float sha = 1.f;
    if (dif1 > 0.0001f) sha = calcSoftShadow(u, pos, SUN_DIR, 20.f);
    dif1 *= sha;
    const float spe1 = clamp(dot(nor, hal), 0.f, 1.f);

    const float bou = clamp(0.3f - 0.7f * nor.y, 0.f, 1.f);

    // illumination
    Vec3f col = Vec3f(1.7f, 1.2f, 0.6f) * (14.f * dif1);              // sun
    col += Vec3f(0.2f, 1.2f, 1.6f) * (4.f * occ * (0.5f + 0.5f * nor.y)); // sky
    col += Vec3f(0.1f, 2.f, 0.1f) * (1.8f * bou * occ);                // bounce

    col *= mateD;

    col += (Vec3f(0.15f, 0.1f, 0.05f) + Vec3f(0.85f, 0.9f, 0.95f) * dif1) * mateS * (0.4f * sss * (0.05f + 0.95f * occ)); // sss
    col = pow(col, Vec3f(0.6f, 0.8f, 1.f));

    const float k = mateK.x * mateK.y;
    col += Vec3f(1.f, 1.f, 1.f) * (0.2f * std::pow(spe1, 1.f + mateK.x)       * dif1 * (0.04f + 0.96f * std::pow(fre, 4.f)) * k); // sun lobe1
    col += Vec3f(1.f, 1.f, 1.f) * (0.1f * std::pow(spe1, 1.f + mateK.x / 3.f) * dif1 * (0.1f  + 0.9f  * std::pow(fre, 4.f)) * k); // sun lobe2
    col += Vec3f(1.f, std::max(1.5f - 0.7f * col.y, 0.f), 2.f)
         * (0.1f * occ * occ * smoothstep(0.f, 0.3f, reflect(rd, nor).y) * k * (0.04f + 0.96f * std::pow(fre, 5.f)));        // sky

    col += mateE;

    return col;
}

inline Vec3f shadeTransparent(const SnailUniforms& u, const Vec3f& ro, const Vec3f& rd, const float t, Vec3f col, const float depth) {
    const Vec3f oriCol = col;

    const float dz   = depth - t;
    const float ao   = clamp(dz * 50.f, 0.f, 1.f);
    const Vec3f pos  = ro + rd * t;
    const Vec3f nor  = calcNormalTransparent(pos, 0.002f);
    const float fre  = clamp(1.f + dot(rd, nor), 0.f, 1.f);
    const Vec3f hal  = normalize(SUN_DIR - rd);
    const Vec3f ref  = reflect(-rd, nor);
    const float spe1 = clamp(dot(nor, hal), 0.f, 1.f);
    const float spe2 = clamp(dot(ref, SUN_DIR), 0.f, 1.f);

    const float ds = 1.6f - col.y;

    col *= mix(Vec3f(0.f, 0.f, 0.f), Vec3f(0.4f, 0.6f, 0.4f), ao);

    col += Vec3f(1.f, 0.9f, 0.8f) * (ds * 1.5f * std::pow(spe1, 80.f));
    col += Vec3f(0.9f, 1.f, 1.f)  * (ds * 0.2f * smoothstep(0.4f, 0.8f, fre));
    col += Vec3f(0.6f, 0.7f, 1.f) * (ds * 0.9f * smoothstep(-0.5f, 0.5f, -reflect(rd, nor).y) * smoothstep(0.2f, 0.4f, fre));
    col += Vec3f(1.f, 0.9f, 0.8f) * (ds * 0.5f * std::pow(spe2, 80.f));
    col += Vec3f(1.f, 0.9f, 0.8f) * (ds * 0.5f * std::pow(spe2, 16.f));
    col += Vec3f(0.8f, 1.f, 0.8f) * (0.5f * smoothstep(0.3f, 0.6f, text1(u, nor.xy * 0.8f)) * (0.1f + 0.9f * fre * fre));

    // hide aliasing a bit
    return mix(col, oriCol, smoothstep(0.6f, 1.f, fre));
}

//--------------------------------------------

inline Vec2f intersectOpaque(const SnailUniforms& u, const Vec3f& ro, const Vec3f& rd, const float mindist, const float maxdist, Vec4f& matInfo) {
    Vec2f res = Vec2f(-1.f, -1.f);

    float t = mindist;
    for (int i = 0; i < 128; i++) {
        const Vec3f p = ro + rd * t;
        const Vec2f h = mapOpaque(u, p, matInfo);
        res = Vec2f(t, h.y);

        if (h.x < 0.001f * t || t > maxdist) break;

        t += h.x * 0.9f;
    }
    return res;
}

inline Vec2f intersectTransparent(const Vec3f& ro, const Vec3f& rd, const float mindist, const float maxdist) {
    Vec2f res = Vec2f(-1.f, -1.f);

    float t = mindist;
    for (int i = 0; i < 64; i++) {
        const Vec3f p = ro + rd * t;
        const float h = mapTransparent(p);
        res = Vec2f(t, 4.f);

        if (h < 0.001f * t || t > maxdist) break;

        t += h;
    }
    return res;
}

inline Vec3f background(const SnailUniforms& u, const Vec3f& d) {
    // cheap cubemap
    const Vec3f n  = abs(d);
    const Vec2f uv = (n.x > n.y && n.x > n.z) ? Vec2f(d.y, d.z) / d.x
                   : (n.y > n.x && n.y > n.z) ? Vec2f(d.z, d.x) / d.y
                   :                            Vec2f(d.x, d.y) / d.z;

    const Vec3f col = texture(u.sky, uv * 0.25f).xyz;
    return pow(col, Vec3f(3.5f, 3.f, 6.f)) * 0.2f;
}

inline Vec3f render(const SnailUniforms& u, const Vec3f& ro, const Vec3f& rd, const Vec2f& q) {
    Vec3f col = Vec3f(0.f, 0.f, 0.f);

    const float mindist = 1.f;
    float       maxdist = 4.f;

    Vec4f matInfo = Vec4f(0.f, 0.f, 0.f, 0.f);
    Vec2f tm = intersectOpaque(u, ro, rd, mindist, maxdist, matInfo);
    if (tm.y > -0.5f && tm.x < maxdist) {
        col = shadeOpaque(u, ro, rd, tm.x, tm.y, matInfo);
        maxdist = tm.x;
    }
    else {
        col = background(u, rd);
    }

    tm = intersectTransparent(ro, rd, mindist, maxdist);
    if (tm.y > -0.5f && tm.x < maxdist) {
        col = shadeTransparent(u, ro, rd, tm.x, col, maxdist);
    }

    const float sun = clamp(dot(rd, SUN_DIR), 0.f, 1.f);
    col += Vec3f(1.5f, 0.8f, 0.7f) * std::pow(sun, 4.f);

    col = pow(col, Vec3f(0.45f, 0.45f, 0.45f));

    col = Vec3f(1.05f, 1.f, 1.f) * col * (Vec3f(0.7f, 0.7f, 0.7f) + col * max(Vec3f(3.f, 3.f, 3.f) - col * 2.f, 0.f) * 0.3f)
        + Vec3f(0.f, 0.f, 0.04f);

    col *= 0.3f + 0.7f * std::pow(16.f * q.x * q.y * (1.f - q.x) * (1.f - q.y), 0.1f);

    return clamp(col, 0.f, 1.f);
}

inline void snailFS(const SnailVaryings& in, const SnailUniforms& u) {
    const float minRes = std::min(u.width, u.height);
    const float an     = 1.87f - 0.04f * (1.f - std::cos(0.5f * u.time));

    const Vec3f ro = Vec3f(-0.4f, 0.2f, 0.f) + Vec3f(std::cos(an), 0.f, std::sin(an)) * 2.2f;
    const Vec3f ta = Vec3f(-0.6f, 0.2f, 0.f);

    // setCamera
    const Vec3f w  = normalize(ro - ta);
    const float m  = std::sqrt(1.f - w.y * w.y);
    const Vec3f c0 = Vec3f(w.z, 0.f, -w.x);
    const Vec3f c1 = Vec3f(0.f, m * m, -w.z * w.y);
    const Vec3f c2 = Vec3f(w.x * m, w.y * m, w.z * m);

    // aa x aa subpixel grid, centred on the pixel
    Vec3f col = Vec3f(0.f, 0.f, 0.f);
    for (int mi = 0; mi < u.aa; mi++)
    for (int ni = 0; ni < u.aa; ni++) {
        const Vec2f rr  = (Vec2f(float(mi), float(ni)) + Vec2f(0.5f, 0.5f)) / float(u.aa) - Vec2f(0.5f, 0.5f);
        const Vec2f ndc = Vec2f(in.ndc.x + 2.f * rr.x / u.width, in.ndc.y + 2.f * rr.y / u.height);

        const Vec2f p  = Vec2f(ndc.x * u.width, ndc.y * u.height) / minRes;
        const Vec2f q  = Vec2f(ndc.x * 0.5f + 0.5f, ndc.y * 0.5f + 0.5f);
        const Vec3f rd = normalize(mulCols(c0, c1, c2, Vec3f(p.x, p.y, -2.8f)));

        col += render(u, ro, rd, q);
    }
    col /= float(u.aa * u.aa);

    valo_FragColor = Color(col.x, col.y, col.z, 1.f);
}

VALO_FRAGMENT_SHADER(snailFS)

} // namespace snail
