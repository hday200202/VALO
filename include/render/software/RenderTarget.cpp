#include "RenderTarget.hpp"

#define STB_IMAGE_IMPLEMENTATION
#define TINYOBJLOADER_IMPLEMENTATION
#define CGLTF_IMPLEMENTATION

#include <stb_image.h>
#include <tiny_obj_loader.h>
#include <cgltf.h>
#include <cstring>
#include <string>
#include <iostream>

namespace valo {

uint8_t* getImageData(
    const std::string& filePath, 
    int* w, 
    int* h, 
    int* channels
) { return stbi_load(filePath.c_str(), w, h, channels, 4); }


// For Textures
RenderTarget RenderTarget::loadFromMemory(
    uint32_t w, 
    uint32_t h, 
    const uint32_t* data
) {
    RenderTarget tex;
    tex.m_width = w;
    tex.m_height = h;
    tex.resize(w, h);
    tex.m_trackWindow = false;
    std::memcpy(
        tex.m_pixels.data(), 
        data, 
        sizeof(uint32_t) * tex.width() * tex.height()
    );

    return tex;
}


RenderTarget RenderTarget::loadImageFromPath(
    const std::string& path
) {
    int w, h, c;
    uint8_t* data = getImageData(path, &w, &h, &c);

    if (!data) {
        std::cout <<  "RenderTarget: " << stbi_failure_reason() << std::endl;
        return {};
    }

    RenderTarget tex = loadFromMemory(
        static_cast<uint32_t>(w),
        static_cast<uint32_t>(h),
        reinterpret_cast<uint32_t*>(data)
    );

    tex.m_trackWindow = false;
    stbi_image_free(data);
    return tex;
}


void RenderTarget::setPixel(const Vec2u& xy, const Color& color) { 
    assert(xy.x < m_width && xy.y < m_height);
    m_pixels[(xy.y * m_width) + xy.x] = colorToABGR32(color);
}


void RenderTarget::setPixel(const uint32_t x, const uint32_t y, const Color& color) { 
    assert(x < m_width && y < m_height);
    m_pixels[(y * m_width) + x] = colorToABGR32(color); 
}


Color RenderTarget::getPixel(const Vec2u& xy) const {
    assert(xy.x < m_width && xy.y < m_height);
    uint32_t pixel = m_pixels[(xy.y * m_width) + xy.x];
    return ABGR32ToColor(pixel);
}


Color RenderTarget::getPixel(const uint32_t x, const uint32_t y) const {
    assert(x < m_width && y < m_height);
    uint32_t pixel = m_pixels[(y * m_width) + x];
    return ABGR32ToColor(pixel);
}


void RenderTarget::setDepth(const Vec2u& xy, const float value) {
    setDepth(xy.x, xy.y, value);
}


// multisampled: every sample, since a pixel-level write covers the pixel
void RenderTarget::setDepth(const uint32_t x,const uint32_t y, const float value) {
    assert(x < m_width && y < m_height);
    if (m_samples > 1) {
        float* d = m_sampleDepth.data() + (size_t(y) * m_width + x) * m_samples;
        std::fill(d, d + m_samples, value);
        return;
    }
    m_depth[(y * m_width) + x] = value;
}


float RenderTarget::getDepth(const Vec2u& xy) const {
    return getDepth(xy.x, xy.y);
}


// multisampled: sample 0 stands for the pixel
float RenderTarget::getDepth(const uint32_t x, const uint32_t y) const {
    assert(x < m_width && y < m_height);
    if (m_samples > 1) return m_sampleDepth[(size_t(y) * m_width + x) * m_samples];
    return m_depth[(y * m_width) + x];
}


void RenderTarget::setStencil(const Vec2u& xy, const uint8_t value) {
    setStencil(xy.x, xy.y, value);
}


void RenderTarget::setStencil(const uint32_t x, const uint32_t y, const uint8_t value) {
    assert(x < m_width && y < m_height);
    if (m_samples > 1) {
        uint8_t* s = m_sampleStencil.data() + (size_t(y) * m_width + x) * m_samples;
        std::fill(s, s + m_samples, value);
        return;
    }
    m_stencil[(y * m_width) + x] = value;
}


uint8_t RenderTarget::getStencil(const Vec2u& xy) const {
    return getStencil(xy.x, xy.y);
}


uint8_t RenderTarget::getStencil(const uint32_t x, const uint32_t y) const {
    assert(x < m_width && y < m_height);
    if (m_samples > 1) return m_sampleStencil[(size_t(y) * m_width + x) * m_samples];
    return m_stencil[(y * m_width) + x];
}


void RenderTarget::resize(const uint32_t width,const uint32_t height) {
    m_width     = width;
    m_height    = height;
    m_pixels.resize(width * height);
    m_depth.resize(width * height);
    m_stencil.resize(width * height);

    if (m_samples > 1) {
        const size_t n = size_t(width) * height;
        m_uniform.assign(n, 1);
        m_sampleColor.assign(n * m_samples, 0u);
        m_sampleDepth.assign(n * m_samples, 0.f);
        m_sampleStencil.assign(n * m_samples, 0);
    }
}


void RenderTarget::setSamples(const uint32_t samples) {
    m_samples = std::max(1u, samples);
    if (m_samples == 1) {
        m_uniform.clear();       m_uniform.shrink_to_fit();
        m_sampleColor.clear();   m_sampleColor.shrink_to_fit();
        m_sampleDepth.clear();   m_sampleDepth.shrink_to_fit();
        m_sampleStencil.clear(); m_sampleStencil.shrink_to_fit();
        return;
    }
    resize(m_width, m_height);
}


void RenderTarget::splitPixel(const uint32_t x, const uint32_t y) {
    const size_t i = size_t(y) * m_width + x;
    if (!m_uniform[i]) return;
    uint32_t* c = m_sampleColor.data() + i * m_samples;
    std::fill(c, c + m_samples, m_pixels[i]);
    m_uniform[i] = 0;
}


void RenderTarget::joinPixel(const uint32_t x, const uint32_t y, const uint32_t abgr) {
    const size_t i = size_t(y) * m_width + x;
    m_pixels[i]  = abgr;
    m_uniform[i] = 1;
}


/*
    The box resolve a GPU does: each channel the mean of the samples, rounded.
    An sRGB image averages in linear light, as a resolve of an _SRGB format
    does, so an edge does not darken.
*/
void RenderTarget::resolveRows(const uint32_t y0, const uint32_t y1) {
    if (m_samples <= 1) return;

    const uint32_t n = m_samples;
    for (uint32_t y = y0; y < y1; ++y) {
        for (uint32_t x = 0; x < m_width; ++x) {
            const size_t i = size_t(y) * m_width + x;
            if (m_uniform[i]) continue;
            const uint32_t* c = m_sampleColor.data() + i * n;

            if (m_srgb) {
                Color sum{0.f, 0.f, 0.f, 0.f};
                for (uint32_t s = 0; s < n; ++s) sum = sum + abgr32ToLinear(c[s]);
                m_pixels[i] = linearToABGR32Srgb(sum * (1.f / float(n)));
                continue;
            }

            uint32_t r = 0, g = 0, b = 0, a = 0;
            for (uint32_t s = 0; s < n; ++s) {
                r +=  c[s]        & 0xFF;
                g += (c[s] >>  8) & 0xFF;
                b += (c[s] >> 16) & 0xFF;
                a += (c[s] >> 24) & 0xFF;
            }
            const uint32_t h = n / 2;
            m_pixels[i] = ((a + h) / n << 24) | ((b + h) / n << 16) | ((g + h) / n << 8) | ((r + h) / n);
        }
    }
}


void RenderTarget::clear(const Color& color, const float depth) { 
    clearRows(color, depth, 0, m_height);
}


void RenderTarget::clearRows(const Color& color, const float depth, const uint32_t y0, const uint32_t y1) {
    // the clear colour is linear like every other colour a shader sees, so an
    // sRGB target has to encode it rather than quantise it straight
    const uint32_t col = m_srgb ? linearToABGR32Srgb(color) : colorToABGR32(color);
    std::fill(m_pixels.begin() + y0 * m_width, m_pixels.begin() + y1 * m_width, col);
    std::fill(m_depth.begin() + y0 * m_width, m_depth.begin() + y1 * m_width, depth);
    std::fill(m_stencil.begin() + y0 * m_width, m_stencil.begin() + y1 * m_width, uint8_t(0));

    if (m_samples > 1) {
        const size_t r0 = size_t(y0) * m_width, r1 = size_t(y1) * m_width;
        std::fill(m_uniform.begin() + r0, m_uniform.begin() + r1, uint8_t(1));
        std::fill(m_sampleDepth.begin() + r0 * m_samples, m_sampleDepth.begin() + r1 * m_samples, depth);
        std::fill(m_sampleStencil.begin() + r0 * m_samples, m_sampleStencil.begin() + r1 * m_samples, uint8_t(0));
    }
}


void RenderTarget::update(const uint32_t* data) {
    std::memcpy(
        m_pixels.data(), 
        data, 
        sizeof(uint32_t) * m_width * m_height
    );

    if (!m_mips.empty()) generateMips();
}


void RenderTarget::update(
    const uint32_t x, const uint32_t y, 
    const uint32_t w, const uint32_t h, 
    const uint32_t* data
) {
    assert(
        x <= m_width && w <= m_width - x &&
        y <= m_height && h <= m_height - y && 
        "update: sub-rect out of bounds"
    );

    for (uint32_t row = 0; row < h; ++row) {
        std::memcpy(
            m_pixels.data() + (y + row) * m_width + x,
            data + row * w, 
            sizeof(uint32_t) * w
        );
    }

    // mip 0 changed, so the rest of the chain is stale - the same rebuild Vulkan does
    if (!m_mips.empty()) generateMips();
}


void RenderTarget::generateMips() {
    m_mips.clear();
    m_mipSizes.clear();
    uint32_t w = m_width, h = m_height;
    const uint32_t* src = m_pixels.data();
    uint32_t sw = w;

    while (w > 1 || h > 1) {
        const uint32_t dw = std::max(1u, w / 2);
        const uint32_t dh = std::max(1u, h / 2);

        std::vector<uint32_t> level(size_t(dw) * dh);

        for (uint32_t y = 0; y < dh; ++y) {
            for (uint32_t x = 0; x < dw; ++x) {
                const uint32_t x0 = std::min(x * 2,     w - 1);
                const uint32_t x1 = std::min(x * 2 + 1, w - 1);
                const uint32_t y0 = std::min(y * 2,     h - 1);
                const uint32_t y1 = std::min(y * 2 + 1, h - 1);

                const uint32_t a = src[y0 * sw + x0], b = src[y0 * sw + x1];
                const uint32_t c = src[y1 * sw + x0], d = src[y1 * sw + x1];

                uint32_t out;
                if (m_srgb) {
                    const Color ca = abgr32ToLinear(a), cb = abgr32ToLinear(b);
                    const Color cc = abgr32ToLinear(c), cd = abgr32ToLinear(d);
                    out = linearToABGR32Srgb((ca + cb + cc + cd) * 0.25f);
                } else out = lerpABGR32(lerpABGR32(a, b, 128), lerpABGR32(c, d, 128), 128);
                
                level[y * dw + x] = out;
            }
        }

        m_mips.push_back(std::move(level));
        m_mipSizes.push_back({dw, dh});

        src = m_mips.back().data();
        sw  = dw;
        w   = dw;
        h   = dh;
    }
}

} // namespace valo
