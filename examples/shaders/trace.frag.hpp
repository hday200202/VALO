#pragma once

#include "trace.vert.hpp"

#include <cmath>
#include <algorithm>

namespace sc {

/* ---- sky ---------------------------------------------------------------- */

inline Vec3f traceSky(const Vec3f& dir) {
    const float t = std::max(dir.y, 0.f);

    const Vec3f horizon = {0.52f, 0.60f, 0.72f};
    const Vec3f zenith  = {0.10f, 0.20f, 0.46f};
    Vec3f sky = horizon + (zenith - horizon) * std::pow(t, 0.55f);

    // the ground half is a dim bounce rather than black, so anything facing
    // down is lit by something
    if (dir.y < 0.f) sky = Vec3f{0.16f, 0.14f, 0.13f};

    const Vec3f sun = Vec3f{-0.42f, 0.46f, 0.62f}.normalized();
    const float d   = std::max(dir.dot(sun), 0.f);

    return sky + Vec3f{1.0f, 0.92f, 0.78f} * (std::pow(d, 220.f) * 9.f + std::pow(d, 18.f) * 0.22f);
}


/* ---- intersection ------------------------------------------------------- */

// slab test; returns the near hit distance, or -1 when the ray misses
inline float hitBox(const Vec3f& lo, const Vec3f& hi, const Vec3f& o, const Vec3f& invD, const float best) {
    const float tx0 = (lo.x - o.x) * invD.x, tx1 = (hi.x - o.x) * invD.x;
    float tmin = std::min(tx0, tx1), tmax = std::max(tx0, tx1);

    const float ty0 = (lo.y - o.y) * invD.y, ty1 = (hi.y - o.y) * invD.y;
    tmin = std::max(tmin, std::min(ty0, ty1));
    tmax = std::min(tmax, std::max(ty0, ty1));

    const float tz0 = (lo.z - o.z) * invD.z, tz1 = (hi.z - o.z) * invD.z;
    tmin = std::max(tmin, std::min(tz0, tz1));
    tmax = std::min(tmax, std::max(tz0, tz1));

    if (tmax < std::max(tmin, 0.f) || tmin > best) return -1.f;
    return std::max(tmin, 0.f);
}


// Moller-Trumbore. u and v come back so the normal can be interpolated.
inline float hitTri(const TriPos& tp, const Vec3f& o, const Vec3f& d, float& u, float& v) {
    const Vec3f pv  = d.cross(tp.e2);
    const float det = tp.e1.dot(pv);

    if (det > -1e-8f && det < 1e-8f) return -1.f;        // ray parallel to the face

    const float inv = 1.f / det;
    const Vec3f tv  = o - tp.p0;

    u = tv.dot(pv) * inv;
    if (u < 0.f || u > 1.f) return -1.f;

    const Vec3f qv = tv.cross(tp.e1);
    v = d.dot(qv) * inv;
    if (v < 0.f || u + v > 1.f) return -1.f;

    return tp.e2.dot(qv) * inv;
}


struct Hit {
    float    t        = -1.f;
    uint32_t tri      = 0u;
    float    u        = 0.f;
    float    v        = 0.f;
    float    transmit = 1.f;
};

/*
    Children are tested when their parent is visited and pushed far-first, so
    the nearer one pops next and a closest-hit ray tightens hit.t before the
    farther box is considered. Each entry carries its entry distance, so a box
    that turned out to be behind the current hit is dropped without being
    loaded again.

    anyHit stops at the first opaque intersection inside maxDist, which is
    all a shadow ray needs to know - and shadow rays are most of the rays
    here. Glass on the way only dims transmit, so light still reaches through
    a window.

    An explicit stack, because GLSL has no recursion. A push past the end
    would drop geometry silently, so it is guarded.
*/
inline Hit traceScene(const Vec3f& o, const Vec3f& d, const float maxDist, const bool anyHit) {
    const StorageView<BVHNode> nodes = storageBuffer<BVHNode>(0);
    const StorageView<TriPos>  pos   = storageBuffer<TriPos>(3);

    Hit hit;
    hit.t = maxDist;

    if (nodes.size() == 0u) { hit.t = -1.f; return hit; }

    const Vec3f invD = {1.f / d.x, 1.f / d.y, 1.f / d.z};

    uint32_t stack[32];
    float    stackT[32];
    int      sp = 0;

    const BVHNode root = nodes[0];
    const float rootT = hitBox(root.lo, root.hi, o, invD, hit.t);
    if (rootT >= 0.f) { stack[0] = 0u; stackT[0] = rootT; sp = 1; }

    while (sp > 0) {
        --sp;
        if (stackT[sp] >= hit.t) continue;

        const BVHNode n = nodes[stack[sp]];

        if (n.count > 0u) {
            for (uint32_t i = 0u; i < n.count; ++i) {
                const uint32_t ti = n.leftFirst + i;

                const TriPos tp = pos[ti];

                float u = 0.f, v = 0.f;
                const float t = hitTri(tp, o, d, u, v);

                if (t > 1e-4f && t < hit.t) {
                    if (anyHit) {
                        if (tp.opacity < 0.995f) {
                            hit.transmit = hit.transmit * (1.f - tp.opacity);
                            if (hit.transmit > 0.01f) continue;
                        }
                        hit.t = t; hit.tri = ti; hit.transmit = 0.f;
                        return hit;
                    }
                    hit.t = t; hit.tri = ti; hit.u = u; hit.v = v;
                }
            }
            continue;
        }

        const uint32_t li = n.leftFirst;
        const uint32_t ri = n.leftFirst + 1u;
        const BVHNode  L  = nodes[li];
        const BVHNode  R  = nodes[ri];

        const float tl = hitBox(L.lo, L.hi, o, invD, hit.t);
        const float tr = hitBox(R.lo, R.hi, o, invD, hit.t);

        if (sp > 29) continue;

        if (tl >= 0.f && tr >= 0.f) {
            if (tl <= tr) {
                stack[sp] = ri; stackT[sp] = tr; ++sp;
                stack[sp] = li; stackT[sp] = tl; ++sp;
            } else {
                stack[sp] = li; stackT[sp] = tl; ++sp;
                stack[sp] = ri; stackT[sp] = tr; ++sp;
            }
        } else if (tl >= 0.f) {
            stack[sp] = li; stackT[sp] = tl; ++sp;
        } else if (tr >= 0.f) {
            stack[sp] = ri; stackT[sp] = tr; ++sp;
        }
    }

    if (hit.t >= maxDist) hit.t = -1.f;
    return hit;
}


// the fraction of light that gets through: 1 unblocked, 0 behind anything opaque
inline float traceShadow(const Vec3f& o, const Vec3f& d, const float maxDist) {
    return traceScene(o, d, maxDist, true).transmit;
}


/* ---- shading ------------------------------------------------------------ */

inline float traceGGX(const float nDotH, const float r) {
    const float a = r * r;
    const float a2 = a * a;
    const float f = nDotH * nDotH * (a2 - 1.f) + 1.f;
    return a2 / std::max(3.14159265f * f * f, 1e-7f);
}

inline float traceSchlick(const float nDotX, const float r) {
    const float k = (r + 1.f) * (r + 1.f) / 8.f;
    return nDotX / (nDotX * (1.f - k) + k);
}

inline Vec3f traceFresnel(const float cosTheta, const Vec3f& f0) {
    const float f = std::pow(1.f - cosTheta, 5.f);
    return f0 + (Vec3f{1.f, 1.f, 1.f} - f0) * f;
}


inline Vec3f traceShade(
    const TraceUniforms& u,
    const Vec3f& pos,
    const Vec3f& n,
    const Vec3f& view,
    const Vec3f& albedo,
    const float metallic,
    const float roughness
) {
    const Vec3f f0 = Vec3f{0.04f, 0.04f, 0.04f} * (1.f - metallic) + albedo * metallic;
    Vec3f radiance = {0.f, 0.f, 0.f};

    for (uint32_t i = 0u; i < u.lightCount; ++i) {
        const TraceLight L = u.lights[i];

        Vec3f toLight = L.pos - pos;
        const float dist = toLight.length();
        toLight = toLight * (1.f / std::max(dist, 1e-4f));

        const float nDotL = n.dot(toLight);
        if (nDotL <= 0.f) continue;

        // offset along the normal, or the surface shadows itself at grazing
        // angles and the model goes blotchy
        const float visible = traceShadow(pos + n * 1e-3f, toLight, dist - 1e-3f);
        if (visible <= 0.f) continue;

        const Vec3f h = (toLight + view).normalized();
        const float nDotV = std::max(n.dot(view), 1e-4f);
        const float nDotH = std::max(n.dot(h), 0.f);

        const float D = traceGGX(nDotH, roughness);
        const float G = traceSchlick(nDotV, roughness) * traceSchlick(nDotL, roughness);
        const Vec3f F = traceFresnel(std::max(h.dot(view), 0.f), f0);

        const Vec3f spec = F * (D * G / std::max(4.f * nDotV * nDotL, 1e-5f));
        const Vec3f kD   = (Vec3f{1.f, 1.f, 1.f} - F) * (1.f - metallic);

        const float falloff = L.intensity / std::max(dist * dist, 1e-4f);
        const Vec3f light   = L.colour * (falloff * nDotL * visible);

        radiance = radiance + (kD * albedo * (1.f / 3.14159265f) + spec) * light;
    }

    // hemisphere ambient standing in for IBL, which would need the sky
    // prefiltered and there is nowhere to put it
    const Vec3f ambient = traceSky(n) * albedo * 0.18f;
    return radiance + ambient;
}


inline Vec3f traceTonemap(const Vec3f& x, const float exposure) {
    const Vec3f c = x * exposure;

    auto aces = [](const float v) {
        const float a = v * (2.51f * v + 0.03f);
        const float b = v * (2.43f * v + 0.59f) + 0.14f;
        return std::clamp(a / std::max(b, 1e-6f), 0.f, 1.f);
    };

    const Vec3f m = {aces(c.x), aces(c.y), aces(c.z)};
    return {std::pow(m.x, 1.f / 2.2f), std::pow(m.y, 1.f / 2.2f), std::pow(m.z, 1.f / 2.2f)};
}


/* ---- the trace ---------------------------------------------------------- */

inline void traceFS(const TraceVaryings& in, const TraceUniforms& u) {
    const StorageView<TraceTri>      tris = storageBuffer<TraceTri>(1);
    const StorageView<TraceMaterial> mats = storageBuffer<TraceMaterial>(2);

    const Vec3f dir = (u.camFwd
                     + u.camRight * (in.ndc.x * u.aspect * u.tanHalf)
                     + u.camUp    * (in.ndc.y * u.tanHalf)).normalized();

    Vec3f colour = {0.f, 0.f, 0.f};
    Vec3f origin = u.camPos;
    Vec3f ray    = dir;
    Vec3f weight = {1.f, 1.f, 1.f};

    /*
        Each segment ends at the floor, the sky, an opaque surface, or glass.
        The floor mirrors the ray once; glass adds its own share and lets the
        ray carry on straight through, so a window costs a segment per pane
        rather than a recursive refraction.
    */
    uint32_t mirrored = 0u;
    for (uint32_t segment = 0u; segment < 6u; ++segment) {
        const Hit hit = traceScene(origin, ray, 1e30f, false);

        float floorT = -1.f;
        if (u.showFloor != 0u && ray.y < -1e-5f)
            floorT = (u.floorY - origin.y) / ray.y;

        const bool hitFloor = floorT > 1e-4f && (hit.t < 0.f || floorT < hit.t);

        if (hitFloor) {
            const Vec3f p = origin + ray * floorT;

            // a checker, dimmed with distance so the horizon does not alias
            // c - 2 floor(c / 2) rather than fmod: fmod keeps the sign of a
            // negative c and GLSL's mod does not, so the two backends drew
            // different floors for every negative coordinate
            const float cell = std::floor(p.x * 0.5f) + std::floor(p.z * 0.5f);
            const float checker = (cell - 2.f * std::floor(cell * 0.5f) < 0.5f)
                                ? 0.16f : 0.09f;
            const float fade = 1.f / (1.f + floorT * floorT * 0.0016f);

            Vec3f base = Vec3f{checker, checker, checker * 1.12f} * fade;
            base = traceShade(u, p, {0.f, 1.f, 0.f}, ray * -1.f, base, 0.f, 0.42f);

            colour = colour + weight * base;

            if (u.reflections == 0u || mirrored != 0u) break;

            // mirror the ray and go round once more
            mirrored = 1u;
            origin = p + Vec3f{0.f, 1e-3f, 0.f};
            ray    = Vec3f(ray.x, -ray.y, ray.z);
            weight = weight * 0.34f;
            continue;
        }

        if (hit.t < 0.f) {
            colour = colour + weight * traceSky(ray);
            break;
        }

        const TraceTri t = tris[hit.tri];
        const Vec3f pos  = origin + ray * hit.t;

        const float w = 1.f - hit.u - hit.v;
        Vec3f n = (t.n0 * w + t.n1 * hit.u + t.n2 * hit.v).normalized();
        if (n.dot(ray) > 0.f) n = n * -1.f;      // two-sided

        const Vec2f uv = t.uv0 * w + t.uv1 * hit.u + t.uv2 * hit.v;
        const Sampler samp = Sampler(Wrap::Repeat, Wrap::Repeat, Filter::Linear);

        const TraceMaterial m = mats[t.material];

        /*
            The normal map is where the raster path gets its smoothness from.
            Interpolated vertex normals alone leave a traced hit reading as
            faceted however well the normals are welded - the surface detail
            lives in this texture, not in the geometry.
        */
        if (m.normalTex != EMPTY_TEXTURE) {
            const Vec4f tan4 = t.t0 * w + t.t1 * hit.u + t.t2 * hit.v;
            Vec3f tangent = Vec3f(tan4.x, tan4.y, tan4.z);

            if (tangent.length() > 1e-4f) {
                tangent = (tangent - n * n.dot(tangent)).normalized();   // Gram-Schmidt
                const Vec3f bitangent = n.cross(tangent) * tan4.w;

                const Color s = texture(m.normalTex, uv, samp);
                const Vec3f tn = Vec3f(s.r * 2.f - 1.f, s.g * 2.f - 1.f, s.b * 2.f - 1.f);

                n = (tangent * tn.x + bitangent * tn.y + n * tn.z).normalized();
            }
        }

        // only a material whose own alpha is below one is treated as glass;
        // texture alpha alone stays opaque, the same as the raster path
        float alpha = m.baseColor.w < 0.995f ? m.baseColor.w : 1.f;

        Vec3f albedo = Vec3f(m.baseColor.x, m.baseColor.y, m.baseColor.z);
        if (m.baseColorTex != EMPTY_TEXTURE) {
            const Color s = texture(m.baseColorTex, uv, samp);
            albedo = Vec3f(albedo.x * s.r, albedo.y * s.g, albedo.z * s.b);
            if (alpha < 1.f) alpha = alpha * s.a;
        }

        float metallic  = m.metallic;
        float roughness = m.roughness;
        if (m.metallicRoughnessTex != EMPTY_TEXTURE) {
            // glTF packs roughness in green and metallic in blue
            const Color s = texture(m.metallicRoughnessTex, uv, samp);
            roughness = roughness * s.g;
            metallic  = metallic  * s.b;
        }

        Vec3f lit = traceShade(u, pos, n, ray * -1.f, albedo, metallic, roughness);

        if (m.occlusionTex != EMPTY_TEXTURE)
            lit = lit * texture(m.occlusionTex, uv, samp).r;

        Vec3f emissive = m.emissive;
        if (m.emissiveTex != EMPTY_TEXTURE) {
            const Color s = texture(m.emissiveTex, uv, samp);
            emissive = Vec3f(emissive.x * s.r, emissive.y * s.g, emissive.z * s.b);
        }

        lit = lit + emissive;

        if (alpha >= 0.995f) {
            colour = colour + weight * lit;
            break;
        }

        /*
            Glass: alpha of the surface's own shading, plus a Fresnel
            reflection of the sky, which is most of what makes a window read
            as glass at a glancing angle. What is neither reflected nor
            covered carries on through to whatever is behind.
        */
        const float cosV    = std::max(n.dot(ray * -1.f), 0.f);
        const float fresnel = 0.04f + 0.96f * std::pow(1.f - cosV, 5.f);
        const Vec3f refl    = ray - n * (2.f * n.dot(ray));

        colour = colour + weight * (lit * alpha + traceSky(refl) * (fresnel * (1.f - roughness)));
        weight = weight * ((1.f - alpha) * (1.f - fresnel));

        if (std::max(weight.x, std::max(weight.y, weight.z)) < 0.01f) break;

        origin = pos + ray * 1e-3f;
    }

    valo_FragColor = Color(traceTonemap(colour, u.exposure), 1.f);
}

VALO_FRAGMENT_SHADER(traceFS)

} // namespace sc
