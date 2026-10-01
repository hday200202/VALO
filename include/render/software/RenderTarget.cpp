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
    assert(xy.x < m_width && xy.y < m_height);
    m_depth[(xy.y * m_width) + xy.x] = value;
}


void RenderTarget::setDepth(const uint32_t x,const uint32_t y, const float value) {
    assert(x < m_width && y < m_height);
    m_depth[(y * m_width) + x] = value;
}


float RenderTarget::getDepth(const Vec2u& xy) const {
    assert(xy.x < m_width && xy.y < m_height);
    return m_depth[(xy.y * m_width) + xy.x];
}


float RenderTarget::getDepth(const uint32_t x, const uint32_t y) const {
    assert(x < m_width && y < m_height);
    return m_depth[(y * m_width) + x];
}


void RenderTarget::setStencil(const Vec2u& xy, const uint8_t value) {
    assert(xy.x < m_width && xy.y < m_height);
    m_stencil[(xy.y * m_width) + xy.x] = value;
}


void RenderTarget::setStencil(const uint32_t x, const uint32_t y, const uint8_t value) {
    assert(x < m_width && y < m_height);
    m_stencil[(y * m_width) + x] = value;
}


uint8_t RenderTarget::getStencil(const Vec2u& xy) const {
    assert(xy.x < m_width && xy.y < m_height);
    return m_stencil[(xy.y * m_width) + xy.x];
}


uint8_t RenderTarget::getStencil(const uint32_t x, const uint32_t y) const {
    assert(x < m_width && y < m_height);
    return m_stencil[(y * m_width) + x];
}


void RenderTarget::resize(const uint32_t width,const uint32_t height) { 
    m_width     = width;
    m_height    = height;
    m_pixels.resize(width * height);
    m_depth.resize(width * height);
    m_stencil.resize(width * height);
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
}


void RenderTarget::update(const uint32_t* data) {
    std::memcpy(
        m_pixels.data(), 
        data, 
        sizeof(uint32_t) * m_width * m_height
    );
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
