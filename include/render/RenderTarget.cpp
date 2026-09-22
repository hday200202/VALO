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
    uint32_t* data
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


void RenderTarget::clear(const Color& color) { 
    clearRows(color, 0, m_height);
}


void RenderTarget::clearRows(const Color& color, const uint32_t y0, const uint32_t y1) {
    const uint32_t col = colorToABGR32(color);
    std::fill(m_pixels.begin() + y0 * m_width, m_pixels.begin() + y1 * m_width, col);
    std::fill(m_depth.begin() + y0 * m_width, m_depth.begin() + y1 * m_width, 1.f);
    std::fill(m_stencil.begin() + y0 * m_width, m_stencil.begin() + y1 * m_width, uint8_t(0));
}

} // namespace valo
