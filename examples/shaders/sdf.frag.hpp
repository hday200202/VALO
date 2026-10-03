#pragma once

#include "sdf.vert.hpp"

// ---------------------------------------------------------------------------
// GLSL-style helpers VALO's math lib doesn't have (yet)
// ---------------------------------------------------------------------------
static inline float sign(float x)                     { return x > 0.f ? 1.f : (x < 0.f ? -1.f : 0.f); }
static inline float fract(float x)                    { return x - std::floor(x); }
static inline float clampf(float x, float lo, float hi){ return std::min(std::max(x, lo), hi); }
static inline float smoothstep(float a, float b, float x) { float t = clampf((x - a) / (b - a), 0.f, 1.f); return t * t * (3.f - 2.f * t); }

static inline Vec2f max(const Vec2f& v, float s)      { return {std::max(v.x, s), std::max(v.y, s)}; }
static inline Vec3f max(const Vec3f& v, float s)      { return {std::max(v.x, s), std::max(v.y, s), std::max(v.z, s)}; }
static inline Vec2f fract(const Vec2f& v)             { return {fract(v.x), fract(v.y)}; }
static inline Vec3f sinv(const Vec3f& v)              { return {std::sin(v.x), std::sin(v.y), std::sin(v.z)}; }
static inline Vec3f powv(const Vec3f& v, float e)     { return {std::pow(v.x, e), std::pow(v.y, e), std::pow(v.z, e)}; }
static inline Vec3f clampv(const Vec3f& v, float lo, float hi) { return {clampf(v.x, lo, hi), clampf(v.y, lo, hi), clampf(v.z, lo, hi)}; }
static inline Vec3f mix(const Vec3f& a, const Vec3f& b, float t) { return a + (b - a) * t; }
static inline Vec3f reflect(const Vec3f& i, const Vec3f& n) { return i - n * (2.f * n.dot(i)); }
static inline Vec3f normalize(const Vec3f& v)         { return v.normalized(); }
static inline float length(const Vec3f& v)            { return v.length(); }
static inline float length(const Vec2f& v)            { return v.length(); }
static inline float dot(const Vec3f& a, const Vec3f& b){ return a.dot(b); }
static inline float dot(const Vec2f& a, const Vec2f& b){ return a.dot(b); }
static inline float dot2(const Vec2f& v)              { return v.dot(v); }
static inline float dot2(const Vec3f& v)              { return v.dot(v); }
static inline float ndot(const Vec2f& a, const Vec2f& b){ return a.x * b.x - a.y * b.y; }

// non-contiguous swizzles
static inline Vec2f xz (const Vec3f& v) { return {v.x, v.z}; }
static inline Vec3f xzy(const Vec3f& v) { return {v.x, v.z, v.y}; }
static inline Vec3f yzx(const Vec3f& v) { return {v.y, v.z, v.x}; }
static inline Vec3f zxy(const Vec3f& v) { return {v.z, v.x, v.y}; }

// ---------------------------------------------------------------------------
// distance functions (iq)
// ---------------------------------------------------------------------------
static float sdSphere(const Vec3f& p, float s) { return length(p) - s; }

static float sdBox(const Vec3f& p, const Vec3f& b) {
    Vec3f d = valo::abs(p) - b;
    return std::min(std::max(d.x, std::max(d.y, d.z)), 0.f) + length(max(d, 0.f));
}

static float sdBoxFrame(Vec3f p, const Vec3f& b, float e) {
    p = valo::abs(p) - b;
    Vec3f q = valo::abs(p + Vec3f{e, e, e}) - Vec3f{e, e, e};
    return std::min(std::min(
        length(max(Vec3f{p.x, q.y, q.z}, 0.f)) + std::min(std::max(p.x, std::max(q.y, q.z)), 0.f),
        length(max(Vec3f{q.x, p.y, q.z}, 0.f)) + std::min(std::max(q.x, std::max(p.y, q.z)), 0.f)),
        length(max(Vec3f{q.x, q.y, p.z}, 0.f)) + std::min(std::max(q.x, std::max(q.y, p.z)), 0.f));
}

static float sdEllipsoid(const Vec3f& p, const Vec3f& r) {
    float k0 = length(p / r);
    float k1 = length(p / (r * r));
    return k0 * (k0 - 1.f) / k1;
}

static float sdTorus(const Vec3f& p, const Vec2f& t) {
    return length(Vec2f{length(xz(p)) - t.x, p.y}) - t.y;
}

static float sdCappedTorus(Vec3f p, const Vec2f& sc, float ra, float rb) {
    p.x = std::abs(p.x);
    float k = (sc.y * p.x > sc.x * p.y) ? dot(p.xy, sc) : length(p.xy);
    return std::sqrt(dot(p, p) + ra * ra - 2.f * ra * k) - rb;
}

static float sdHexPrism(Vec3f p, const Vec2f& h) {
    const Vec3f k = {-0.8660254f, 0.5f, 0.57735f};
    p = valo::abs(p);
    p.xy -= k.xy * (2.f * std::min(dot(k.xy, p.xy), 0.f));
    Vec2f d = {
        length(p.xy - Vec2f{clampf(p.x, -k.z * h.x, k.z * h.x), h.x}) * sign(p.y - h.x),
        p.z - h.y };
    return std::min(std::max(d.x, d.y), 0.f) + length(max(d, 0.f));
}

static float sdOctogonPrism(Vec3f p, float r, float h) {
    const Vec3f k = {-0.9238795325f, 0.3826834323f, 0.4142135623f};
    p = valo::abs(p);
    p.xy -= Vec2f{ k.x, k.y} * (2.f * std::min(dot(Vec2f{ k.x, k.y}, p.xy), 0.f));
    p.xy -= Vec2f{-k.x, k.y} * (2.f * std::min(dot(Vec2f{-k.x, k.y}, p.xy), 0.f));
    p.xy -= Vec2f{clampf(p.x, -k.z * r, k.z * r), r};
    Vec2f d = {length(p.xy) * sign(p.y), p.z - h};
    return std::min(std::max(d.x, d.y), 0.f) + length(max(d, 0.f));
}

static float sdCapsule(const Vec3f& p, const Vec3f& a, const Vec3f& b, float r) {
    Vec3f pa = p - a, ba = b - a;
    float h = clampf(dot(pa, ba) / dot(ba, ba), 0.f, 1.f);
    return length(pa - ba * h) - r;
}

static float sdRoundCone(const Vec3f& p, float r1, float r2, float h) {
    Vec2f q = {length(xz(p)), p.y};
    float b = (r1 - r2) / h;
    float a = std::sqrt(1.f - b * b);
    float k = dot(q, Vec2f{-b, a});
    if (k < 0.f)    return length(q) - r1;
    if (k > a * h)  return length(q - Vec2f{0.f, h}) - r2;
    return dot(q, Vec2f{a, b}) - r1;
}

static float sdRoundCone(const Vec3f& p, const Vec3f& a, const Vec3f& b, float r1, float r2) {
    Vec3f ba = b - a;
    float l2 = dot(ba, ba);
    float rr = r1 - r2;
    float a2 = l2 - rr * rr;
    float il2 = 1.f / l2;
    Vec3f pa = p - a;
    float y = dot(pa, ba);
    float z = y - l2;
    float x2 = dot2(pa * l2 - ba * y);
    float y2 = y * y * l2;
    float z2 = z * z * l2;
    float k = sign(rr) * rr * rr * x2;
    if (sign(z) * a2 * z2 > k) return  std::sqrt(x2 + z2) * il2 - r2;
    if (sign(y) * a2 * y2 < k) return  std::sqrt(x2 + y2) * il2 - r1;
    return (std::sqrt(x2 * a2 * il2) + y * rr) * il2 - r1;
}

static float sdTriPrism(Vec3f p, Vec2f h) {
    const float k = std::sqrt(3.f);
    h.x *= 0.5f * k;
    p.xy /= h.x;
    p.x = std::abs(p.x) - 1.f;
    p.y = p.y + 1.f / k;
    if (p.x + k * p.y > 0.f) p.xy = Vec2f{p.x - k * p.y, -k * p.x - p.y} / 2.f;
    p.x -= clampf(p.x, -2.f, 0.f);
    float d1 = length(p.xy) * sign(-p.y) * h.x;
    float d2 = std::abs(p.z) - h.y;
    return length(max(Vec2f{d1, d2}, 0.f)) + std::min(std::max(d1, d2), 0.f);
}

static float sdCylinder(const Vec3f& p, const Vec2f& h) {
    Vec2f d = valo::abs(Vec2f{length(xz(p)), p.y}) - h;
    return std::min(std::max(d.x, d.y), 0.f) + length(max(d, 0.f));
}

static float sdCylinder(const Vec3f& p, const Vec3f& a, const Vec3f& b, float r) {
    Vec3f pa = p - a, ba = b - a;
    float baba = dot(ba, ba), paba = dot(pa, ba);
    float x = length(pa * baba - ba * paba) - r * baba;
    float y = std::abs(paba - baba * 0.5f) - baba * 0.5f;
    float x2 = x * x, y2 = y * y * baba;
    float d = (std::max(x, y) < 0.f) ? -std::min(x2, y2) : (((x > 0.f) ? x2 : 0.f) + ((y > 0.f) ? y2 : 0.f));
    return sign(d) * std::sqrt(std::abs(d)) / baba;
}

static float sdCone(const Vec3f& p, const Vec2f& c, float h) {
    Vec2f q = Vec2f{c.x, -c.y} * (h / c.y);
    Vec2f w = {length(xz(p)), p.y};
    Vec2f a = w - q * clampf(dot(w, q) / dot(q, q), 0.f, 1.f);
    Vec2f b = w - q * Vec2f{clampf(w.x / q.x, 0.f, 1.f), 1.f};
    float k = sign(q.y);
    float d = std::min(dot(a, a), dot(b, b));
    float s = std::max(k * (w.x * q.y - w.y * q.x), k * (w.y - q.y));
    return std::sqrt(d) * sign(s);
}

static float sdCappedCone(const Vec3f& p, float h, float r1, float r2) {
    Vec2f q = {length(xz(p)), p.y};
    Vec2f k1 = {r2, h};
    Vec2f k2 = {r2 - r1, 2.f * h};
    Vec2f ca = {q.x - std::min(q.x, (q.y < 0.f) ? r1 : r2), std::abs(q.y) - h};
    Vec2f cb = q - k1 + k2 * clampf(dot(k1 - q, k2) / dot2(k2), 0.f, 1.f);
    float s = (cb.x < 0.f && ca.y < 0.f) ? -1.f : 1.f;
    return s * std::sqrt(std::min(dot2(ca), dot2(cb)));
}

static float sdCappedCone(const Vec3f& p, const Vec3f& a, const Vec3f& b, float ra, float rb) {
    float rba  = rb - ra;
    float baba = dot(b - a, b - a);
    float papa = dot(p - a, p - a);
    float paba = dot(p - a, b - a) / baba;
    float x = std::sqrt(std::max(papa - paba * paba * baba, 0.f));
    float cax = std::max(0.f, x - ((paba < 0.5f) ? ra : rb));
    float cay = std::abs(paba - 0.5f) - 0.5f;
    float k = rba * rba + baba;
    float f = clampf((rba * (x - ra) + paba * baba) / k, 0.f, 1.f);
    float cbx = x - ra - f * rba;
    float cby = paba - f;
    float s = (cbx < 0.f && cay < 0.f) ? -1.f : 1.f;
    return s * std::sqrt(std::min(cax * cax + cay * cay * baba, cbx * cbx + cby * cby * baba));
}

static float sdSolidAngle(const Vec3f& pos, const Vec2f& c, float ra) {
    Vec2f p = {length(xz(pos)), pos.y};
    float l = length(p) - ra;
    float m = length(p - c * clampf(dot(p, c), 0.f, ra));
    return std::max(l, m * sign(c.y * p.x - c.x * p.y));
}

static float sdOctahedron(Vec3f p, float s) {
    p = valo::abs(p);
    float m = p.x + p.y + p.z - s;
    Vec3f q;
    if      (3.f * p.x < m) q = p;
    else if (3.f * p.y < m) q = yzx(p);
    else if (3.f * p.z < m) q = zxy(p);
    else return m * 0.57735027f;
    float k = clampf(0.5f * (q.z - q.y + s), 0.f, s);
    return length(Vec3f{q.x, q.y - s + k, q.z - k});
}

static float sdPyramid(Vec3f p, float h) {
    float m2 = h * h + 0.25f;
    p.x = std::abs(p.x); p.z = std::abs(p.z);
    if (p.z > p.x) { const float t = p.x; p.x = p.z; p.z = t; }
    p.x -= 0.5f; p.z -= 0.5f;
    Vec3f q = {p.z, h * p.y - 0.5f * p.x, h * p.x + 0.5f * p.y};
    float s = std::max(-q.x, 0.f);
    float t = clampf((q.y - 0.5f * p.z) / (m2 + 0.25f), 0.f, 1.f);
    float a = m2 * (q.x + s) * (q.x + s) + q.y * q.y;
    float b = m2 * (q.x + 0.5f * t) * (q.x + 0.5f * t) + (q.y - m2 * t) * (q.y - m2 * t);
    float d2 = std::min(q.y, -q.x * m2 - q.y * 0.5f) > 0.f ? 0.f : std::min(a, b);
    return std::sqrt((d2 + q.z * q.z) / m2) * sign(std::max(q.z, -p.y));
}

static float sdRhombus(Vec3f p, float la, float lb, float h, float ra) {
    p = valo::abs(p);
    Vec2f b = {la, lb};
    float f = clampf(ndot(b, b - xz(p) * 2.f) / dot(b, b), -1.f, 1.f);
    Vec2f q = {
        length(xz(p) - b * 0.5f * Vec2f{1.f - f, 1.f + f}) * sign(p.x * b.y + p.z * b.x - b.x * b.y) - ra,
        p.y - h };
    return std::min(std::max(q.x, q.y), 0.f) + length(max(q, 0.f));
}

static float sdHorseshoe(Vec3f p, const Vec2f& c, float r, float le, const Vec2f& w) {
    p.x = std::abs(p.x);
    float l = length(p.xy);
    p.xy = Vec2f{-c.x * p.x + c.y * p.y, c.y * p.x + c.x * p.y};   // mat2(-c.x, c.y, c.y, c.x) * p.xy (column-major)
    p.xy = Vec2f{(p.y > 0.f || p.x > 0.f) ? p.x : l * sign(-c.x), (p.x > 0.f) ? p.y : l};
    p.xy = Vec2f{p.x, std::abs(p.y - r)} - Vec2f{le, 0.f};
    Vec2f q = {length(max(p.xy, 0.f)) + std::min(0.f, std::max(p.x, p.y)), p.z};
    Vec2f d = valo::abs(q) - w;
    return std::min(std::max(d.x, d.y), 0.f) + length(max(d, 0.f));
}

// ---------------------------------------------------------------------------
// scene
// ---------------------------------------------------------------------------
static inline Vec2f opU(const Vec2f& d1, const Vec2f& d2) { return (d1.x < d2.x) ? d1 : d2; }

static Vec2f map(const Vec3f& pos) {
    Vec2f res = Vec2f{pos.y, 0.f};

    if (sdBox(pos - Vec3f{-2.f, 0.3f, 0.25f}, {0.3f, 0.3f, 1.f}) < res.x) {
        res = opU(res, {sdSphere (pos - Vec3f{-2.f, 0.25f, 0.f}, 0.25f), 26.9f});
        res = opU(res, {sdRhombus(xzy(pos - Vec3f{-2.f, 0.25f, 1.f}), 0.15f, 0.25f, 0.04f, 0.08f), 17.f});
    }
    if (sdBox(pos - Vec3f{0.f, 0.3f, -1.f}, {0.35f, 0.3f, 2.5f}) < res.x) {
        res = opU(res, {sdCappedTorus((pos - Vec3f{0.f, 0.30f, 1.f}) * Vec3f{1, -1, 1}, {0.866025f, -0.5f}, 0.25f, 0.05f), 25.f});
        res = opU(res, {sdBoxFrame  (pos - Vec3f{0.f, 0.25f, 0.f}, {0.3f, 0.25f, 0.2f}, 0.025f), 16.9f});
        res = opU(res, {sdCone      (pos - Vec3f{0.f, 0.45f, -1.f}, {0.6f, 0.8f}, 0.45f), 55.f});
        res = opU(res, {sdCappedCone(pos - Vec3f{0.f, 0.25f, -2.f}, 0.25f, 0.25f, 0.1f), 13.67f});
        res = opU(res, {sdSolidAngle(pos - Vec3f{0.f, 0.00f, -3.f}, Vec2f{3.f, 4.f} / 5.f, 0.4f), 49.13f});
    }
    if (sdBox(pos - Vec3f{1.f, 0.3f, -1.f}, {0.35f, 0.3f, 2.5f}) < res.x) {
        res = opU(res, {sdTorus   (xzy(pos - Vec3f{1.f, 0.30f, 1.f}), {0.25f, 0.05f}), 7.1f});
        res = opU(res, {sdBox     (pos - Vec3f{1.f, 0.25f, 0.f}, {0.3f, 0.25f, 0.1f}), 3.f});
        res = opU(res, {sdCapsule (pos - Vec3f{1.f, 0.00f, -1.f}, {-0.1f, 0.1f, -0.1f}, {0.2f, 0.4f, 0.2f}, 0.1f), 31.9f});
        res = opU(res, {sdCylinder(pos - Vec3f{1.f, 0.25f, -2.f}, {0.15f, 0.25f}), 8.f});
        res = opU(res, {sdHexPrism(pos - Vec3f{1.f, 0.2f, -3.f}, {0.2f, 0.05f}), 18.4f});
    }
    if (sdBox(pos - Vec3f{-1.f, 0.35f, -1.f}, {0.35f, 0.35f, 2.5f}) < res.x) {
        res = opU(res, {sdPyramid   (pos - Vec3f{-1.f, -0.6f, -3.f}, 1.f), 13.56f});
        res = opU(res, {sdOctahedron(pos - Vec3f{-1.f, 0.15f, -2.f}, 0.35f), 23.56f});
        res = opU(res, {sdTriPrism  (pos - Vec3f{-1.f, 0.15f, -1.f}, {0.3f, 0.05f}), 43.5f});
        res = opU(res, {sdEllipsoid (pos - Vec3f{-1.f, 0.25f, 0.f}, {0.2f, 0.25f, 0.05f}), 43.17f});
        res = opU(res, {sdHorseshoe (pos - Vec3f{-1.f, 0.25f, 1.f}, {std::cos(1.3f), std::sin(1.3f)}, 0.2f, 0.3f, {0.03f, 0.08f}), 11.5f});
    }
    if (sdBox(pos - Vec3f{2.f, 0.3f, -1.f}, {0.35f, 0.3f, 2.5f}) < res.x) {
        res = opU(res, {sdOctogonPrism(pos - Vec3f{2.f, 0.2f, -3.f}, 0.2f, 0.05f), 51.8f});
        res = opU(res, {sdCylinder    (pos - Vec3f{2.f, 0.14f, -2.f}, {0.1f, -0.1f, 0.f}, {-0.2f, 0.35f, 0.1f}, 0.08f), 31.2f});
        res = opU(res, {sdCappedCone  (pos - Vec3f{2.f, 0.09f, -1.f}, {0.1f, 0.f, 0.f}, {-0.2f, 0.40f, 0.1f}, 0.15f, 0.05f), 46.1f});
        res = opU(res, {sdRoundCone   (pos - Vec3f{2.f, 0.15f, 0.f}, {0.1f, 0.f, 0.f}, {-0.1f, 0.35f, 0.1f}, 0.15f, 0.05f), 51.7f});
        res = opU(res, {sdRoundCone   (pos - Vec3f{2.f, 0.20f, 1.f}, 0.2f, 0.1f, 0.3f), 37.f});
    }
    return res;
}

static Vec2f iBox(const Vec3f& ro, const Vec3f& rd, const Vec3f& rad) {
    Vec3f m = Vec3f{1.f, 1.f, 1.f} / rd;
    Vec3f n = m * ro;
    Vec3f k = valo::abs(m) * rad;
    Vec3f t1 = -n - k;
    Vec3f t2 = -n + k;
    return {std::max(std::max(t1.x, t1.y), t1.z), std::min(std::min(t2.x, t2.y), t2.z)};
}

static Vec2f raycast(const Vec3f& ro, const Vec3f& rd) {
    Vec2f res = Vec2f{-1.f, -1.f};
    float tmin = 1.f, tmax = 20.f;

    float tp1 = (0.f - ro.y) / rd.y;
    if (tp1 > 0.f) { tmax = std::min(tmax, tp1); res = Vec2f{tp1, 1.f}; }

    Vec2f tb = iBox(ro - Vec3f{0.f, 0.4f, -0.5f}, rd, {2.5f, 0.41f, 3.f});
    if (tb.x < tb.y && tb.y > 0.f && tb.x < tmax) {
        tmin = std::max(tb.x, tmin);
        tmax = std::min(tb.y, tmax);
        float t = tmin;
        for (int i = 0; i < 70 && t < tmax; i++) {
            Vec2f h = map(ro + rd * t);
            if (std::abs(h.x) < (0.0001f * t)) { res = Vec2f{t, h.y}; break; }
            t += h.x;
        }
    }
    return res;
}

static float calcSoftshadow(const Vec3f& ro, const Vec3f& rd, float mint, float tmax) {
    float tp = (0.8f - ro.y) / rd.y; if (tp > 0.f) tmax = std::min(tmax, tp);
    float res = 1.f, t = mint;
    for (int i = 0; i < 24; i++) {
        float h = map(ro + rd * t).x;
        float s = clampf(8.f * h / t, 0.f, 1.f);
        res = std::min(res, s);
        t += clampf(h, 0.01f, 0.2f);
        if (res < 0.004f || t > tmax) break;
    }
    res = clampf(res, 0.f, 1.f);
    return res * res * (3.f - 2.f * res);
}

static Vec3f calcNormal(const Vec3f& pos) {
    Vec3f n = {0.f, 0.f, 0.f};
    for (int i = 0; i < 4; i++) {
        Vec3f e = Vec3f{float(((i + 3) >> 1) & 1), float((i >> 1) & 1), float(i & 1)} * 2.f - Vec3f{1.f, 1.f, 1.f};
        e = e * 0.5773f;
        n += e * map(pos + e * 0.0005f).x;
    }
    return normalize(n);
}

static float calcAO(const Vec3f& pos, const Vec3f& nor) {
    float occ = 0.f, sca = 1.f;
    for (int i = 0; i < 5; i++) {
        float h = 0.01f + 0.12f * float(i) / 4.f;
        float d = map(pos + nor * h).x;
        occ += (h - d) * sca;
        sca *= 0.95f;
        if (occ > 0.35f) break;
    }
    return clampf(1.f - 3.f * occ, 0.f, 1.f) * (0.5f + 0.5f * nor.y);
}

static float checkersGradBox(const Vec2f& p, const Vec2f& dpdx, const Vec2f& dpdy) {
    Vec2f w = valo::abs(dpdx) + valo::abs(dpdy) + Vec2f{0.001f, 0.001f};
    Vec2f i = (valo::abs(fract((p - w * 0.5f) * 0.5f) - Vec2f{0.5f, 0.5f})
             - valo::abs(fract((p + w * 0.5f) * 0.5f) - Vec2f{0.5f, 0.5f})) * 2.f / w;
    return 0.5f - 0.5f * i.x * i.y;
}

static Vec3f render(const Vec3f& ro, const Vec3f& rd, const Vec3f& rdx, const Vec3f& rdy) {
    Vec3f col = Vec3f{0.7f, 0.7f, 0.9f} - Vec3f{0.3f, 0.3f, 0.3f} * std::max(rd.y, 0.f);

    Vec2f res = raycast(ro, rd);
    float t = res.x, m = res.y;
    if (m > -0.5f) {
        Vec3f pos = ro + rd * t;
        Vec3f nor = (m < 1.5f) ? Vec3f{0.f, 1.f, 0.f} : calcNormal(pos);
        Vec3f ref = reflect(rd, nor);

        col = Vec3f{0.2f, 0.2f, 0.2f} + sinv(Vec3f{0.f, 1.f, 2.f} + Vec3f{m * 2.f, m * 2.f, m * 2.f}) * 0.2f;
        float ks = 1.f;

        if (m < 1.5f) {
            Vec3f dpdx = (rd / rd.y - rdx / rdx.y) * ro.y;
            Vec3f dpdy = (rd / rd.y - rdy / rdy.y) * ro.y;
            float f = checkersGradBox(xz(pos) * 3.f, xz(dpdx) * 3.f, xz(dpdy) * 3.f);
            col = Vec3f{0.15f, 0.15f, 0.15f} + Vec3f{0.05f, 0.05f, 0.05f} * f;
            ks = 0.4f;
        }

        float occ = calcAO(pos, nor);
        Vec3f lin = {0.f, 0.f, 0.f};

        { // sun
            Vec3f lig = normalize(Vec3f{-0.5f, 0.4f, -0.6f});
            Vec3f hal = normalize(lig - rd);
            float dif = clampf(dot(nor, lig), 0.f, 1.f);
            dif *= calcSoftshadow(pos, lig, 0.02f, 2.5f);
            float spe = std::pow(clampf(dot(nor, hal), 0.f, 1.f), 16.f);
            spe *= dif;
            spe *= 0.04f + 0.96f * std::pow(clampf(1.f - dot(hal, lig), 0.f, 1.f), 5.f);
            lin += col * Vec3f{1.30f, 1.00f, 0.70f} * (2.20f * dif);
            lin += Vec3f{1.30f, 1.00f, 0.70f} * (5.00f * spe * ks);
        }
        { // sky
            float dif = std::sqrt(clampf(0.5f + 0.5f * nor.y, 0.f, 1.f));
            dif *= occ;
            float spe = smoothstep(-0.2f, 0.2f, ref.y);
            spe *= dif;
            spe *= 0.04f + 0.96f * std::pow(clampf(1.f + dot(nor, rd), 0.f, 1.f), 5.f);
            spe *= calcSoftshadow(pos, ref, 0.02f, 2.5f);
            lin += col * Vec3f{0.40f, 0.60f, 1.15f} * (0.60f * dif);
            lin += Vec3f{0.40f, 0.60f, 1.30f} * (2.00f * spe * ks);
        }
        { // back
            float dif = clampf(dot(nor, normalize(Vec3f{0.5f, 0.f, 0.6f})), 0.f, 1.f) * clampf(1.f - pos.y, 0.f, 1.f);
            dif *= occ;
            lin += col * Vec3f{0.25f, 0.25f, 0.25f} * (0.55f * dif);
        }
        { // sss
            float dif = std::pow(clampf(1.f + dot(nor, rd), 0.f, 1.f), 2.f);
            dif *= occ;
            lin += col * (0.25f * dif);
        }
        col = lin;
        col = mix(col, Vec3f{0.7f, 0.7f, 0.9f}, 1.f - std::exp(-0.0001f * t * t * t));
    }
    return clampv(col, 0.f, 1.f);
}

// ---------------------------------------------------------------------------
// VALO shaders: one fullscreen triangle, all the work in the fragment shader
// ---------------------------------------------------------------------------
// focal length. File scope rather than a local, because a lambda that captures
// it is hoisted to a free function when translated to GLSL and the capture does
// not come with it.
static constexpr float kFocal = 2.5f;

inline void sdfFS(const Varyings& in, const Uniforms& u) {
    // p in [-aspect, aspect] x [-1, 1], like (2*fragCoord - res) / res.y
    auto rayDir = [&](Vec2f p) {
        Vec3f d = normalize(Vec3f{p.x, p.y, kFocal});
        return u.cu * d.x + u.cv * d.y + u.cw * d.z;
    };
    Vec2f p = {in.ndc.x * u.aspect, in.ndc.y};
    // neighbouring-pixel rays for the checkerboard filter; 1/540 ~ one pixel at the default scale
    const float px = 2.f / 540.f;
    Vec3f rd  = rayDir(p);
    Vec3f rdx = rayDir(p + Vec2f{px, 0.f});
    Vec3f rdy = rayDir(p + Vec2f{0.f, px});

    Vec3f col = render(u.ro, rd, rdx, rdy);
    col = powv(col, 0.4545f);
    valo::valo_FragColor = {col, 1.f};
}

VALO_FRAGMENT_SHADER(sdfFS)
