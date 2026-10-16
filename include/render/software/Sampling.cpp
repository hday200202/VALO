#include "Sampling.hpp"

namespace valo {

namespace {

float wrap(float t, Wrap mode) {
    switch (mode) {
        case Wrap::Clamp:   return std::clamp(t, 0.f, 1.f);
        case Wrap::Repeat:  return t - std::floor(t);
        case Wrap::Mirror:  {
            const float tile    = std::floor(t);
            const float f       = t - tile;
            const bool  odd     = (int(tile) & 1) != 0;
            return odd ? 1.f - f : f;
        }
    }
    return 0.f;
}


constexpr int wrapPixel(int x, int size, Wrap mode) {
    switch (mode) {
        case Wrap::Clamp: return std::clamp(x, 0, size - 1);
        case Wrap::Repeat: return ((x % size) + size) % size;
        case Wrap::Mirror: {
            const int period = 2 * size;
            const int m = ((x % period) + period) % period;
            return m < size ? m : period - 1 - m;
        }
    }
    return 0;
}

}

Color texture(const TextureID tex, const Vec2f& uv, const Sampler& sampler) {
    assert(detail::boundTargets && "texture() called outside of a draw");
    assert(detail::boundTargets && tex < detail::boundTargets->size()
        && (*detail::boundTargets)[tex] && "texture(): unbound or destroyed texture");

    // magenta rather than an out-of-bounds read, so a missing bind is visible
    if (!detail::boundTargets || tex >= detail::boundTargets->size()
        || !(*detail::boundTargets)[tex]) return {1.f, 0.f, 1.f, 1.f};

    const RenderTarget& t = *(*detail::boundTargets)[tex];

    // a window-tracking target is 0x0 while minimized; nothing to read, and the wrap would divide by zero
    if (t.width() == 0 || t.height() == 0) return {0.f, 0.f, 0.f, 0.f};

    uint32_t level = 0;
    const auto& dv = detail::varyingDerivs;

    if (dv.valid && dv.interp && t.mipCount() > 1) {
        const ptrdiff_t k = &uv.x - dv.interp;

        if (k >= 0 && k + 1 < ptrdiff_t(MAX_VARYINGS)) {
            auto ddx = [&](ptrdiff_t i) {
                const float dnum = 
                    dv.dBdx[0] * dv.vary[0][i] * dv.w[0] + 
                    dv.dBdx[1] * dv.vary[1][i] * dv.w[1] + 
                    dv.dBdx[2] * dv.vary[2][i] * dv.w[2];
                return (dnum - dv.interp[i] * dv.dDenDx) / dv.invW;
            };
            auto ddy = [&](ptrdiff_t i) {
                const float dnum = 
                    dv.dBdy[0] * dv.vary[0][i] * dv.w[0] + 
                    dv.dBdy[1] * dv.vary[1][i] * dv.w[1] + 
                    dv.dBdy[2] * dv.vary[2][i] * dv.w[2];
                return (dnum - dv.interp[i] * dv.dDenDy) / dv.invW;
            };

            const float tw = float(t.width()), th = float(t.height());
            const float ux = ddx(k) * tw, vx = ddx(k + 1) * th;
            const float uy = ddy(k) * tw, vy = ddy(k + 1) * th;

            const float rho = std::max(std::sqrt(ux*ux + vx*vx), std::sqrt(uy*uy + vy*vy));
            if (rho > 1.f) level = std::min(uint32_t(std::log2(rho) + 0.5f), t.mipCount() - 1);
        }
    }

    const Vec2u     ms = t.mipSize(level);
    const uint32_t* px = t.mipData(level);

    float u = wrap(uv.x, sampler.wrapU) * ms.x;
    float v = wrap(uv.y, sampler.wrapV) * ms.y;

    if (sampler.filter == Filter::Nearest) {
        const uint32_t x = std::min(uint32_t(u), ms.x - 1);
        const uint32_t y = std::min(uint32_t(v), ms.y - 1);
        const uint32_t p = px[y * ms.x + x];
        return t.isSrgb() ? abgr32ToLinear(p) : ABGR32ToColor(p);
    }

    const float fx = u - 0.5f, fy = v - 0.5f;
    const float x0f = std::floor(fx), y0f = std::floor(fy);
    const float tx = fx - x0f, ty = fy - y0f;

    const int w = int(ms.x), h = int(ms.y);
    const int x0 = wrapPixel(int(x0f),      w, sampler.wrapU);
    const int x1 = wrapPixel(int(x0f) + 1,  w, sampler.wrapU);
    const int y0 = wrapPixel(int(y0f),      h, sampler.wrapV);
    const int y1 = wrapPixel(int(y0f) + 1,  h, sampler.wrapV);

    const uint32_t  W  = ms.x;
    const uint32_t p00 = px[y0 * W + x0], p10 = px[y0 * W + x1];
    const uint32_t p01 = px[y1 * W + x0], p11 = px[y1 * W + x1];

    if (t.isSrgb()) {
        const Color c00 = abgr32ToLinear(p00), c10 = abgr32ToLinear(p10);
        const Color c01 = abgr32ToLinear(p01), c11 = abgr32ToLinear(p11);
        const Color top    = c00 + (c10 - c00) * tx;
        const Color bottom = c01 + (c11 - c01) * tx;
        return top + (bottom - top) * ty;
    }

    const uint32_t wx = uint32_t(tx * 256.f + 0.5f);
    const uint32_t wy = uint32_t(ty * 256.f + 0.5f);

    const uint32_t top    = lerpABGR32(p00, p10, wx);
    const uint32_t bottom = lerpABGR32(p01, p11, wx);
    return ABGR32ToColor(lerpABGR32(top, bottom, wy));
}

float textureDepth(const TextureID tex, const Vec2f& uv) {
    assert(detail::boundTargets && "textureDepth() called outside of a draw");
    assert(detail::boundTargets && tex < detail::boundTargets->size()
        && (*detail::boundTargets)[tex] && "textureDepth(): unbound or destroyed texture");

    if (!detail::boundTargets || tex >= detail::boundTargets->size()
        || !(*detail::boundTargets)[tex]) return 0.f;   // reversed-Z: far plane

    const RenderTarget& t = *(*detail::boundTargets)[tex];
    if (t.width() == 0 || t.height() == 0) return 0.f;
    float u = std::clamp<float>(uv.x, 0, 1) * (t.width() - 1);
    float v = std::clamp<float>(uv.y, 0, 1) * (t.height() - 1);

    uint32_t x = static_cast<uint32_t>(u);
    uint32_t y = static_cast<uint32_t>(v);

    return t.getDepth(x, y);
}

};