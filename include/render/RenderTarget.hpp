#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <cassert>
#include <algorithm>
#include "common/Math.hpp"
#include "common/Srgb.hpp"

namespace valo {

class RenderTarget {
public:
    RenderTarget() = default;
    RenderTarget(const uint32_t width, const uint32_t height, bool trackWindow = true) 
    : m_width(width), m_height(height), m_trackWindow(trackWindow) { resize(width, height); }

    // For Textures
    static RenderTarget loadFromMemory(uint32_t w, uint32_t h, const uint32_t* data);
    static RenderTarget loadImageFromPath(const std::string& path);

    void setPixel(const Vec2u& xy, const Color& color);
    void setPixel(const uint32_t x, const uint32_t y, const Color& color);
    Color getPixel(const Vec2u& xy) const;
    Color getPixel(const uint32_t x, const uint32_t y) const;

    void setDepth(const Vec2u& xy, const float value);
    void setDepth(const uint32_t x,const uint32_t y, const float value);
    float getDepth(const Vec2u& xy) const;
    float getDepth(const uint32_t x, const uint32_t y) const;

    void setStencil(const Vec2u& xy, const uint8_t value);
    void setStencil(const uint32_t x, const uint32_t y, const uint8_t value);
    uint8_t getStencil(const Vec2u& xy) const;
    uint8_t getStencil(const uint32_t x, const uint32_t y) const;

    void resize(const uint32_t width,const uint32_t height);

    void clear(const Color& color = {0.f, 0.f, 0.f, 1.f}, const float depth = 0.f);    
    void clearRows(const Color& color, const float depth, const uint32_t y0, const uint32_t y1);

    void update(const uint32_t* data);
    void update(const uint32_t x, const uint32_t y, const uint32_t w, const uint32_t h, const uint32_t* data);


    uint32_t width() const { return m_width; }
    uint32_t height() const { return m_height; }
    uint32_t* data() { return m_pixels.data(); }
    float* depthData() { return m_depth.data(); }
    const uint32_t* data() const { return m_pixels.data(); }
    const float* depthData() const { return m_depth.data(); }
    bool tracksWindow() const { return m_trackWindow; }
    void setTrackWindow(const bool trackWindow) { m_trackWindow = trackWindow; }

    /*
        sRGB is a property of the image, not of the shader - the same thing a
        Vulkan _SRGB format expresses. The pixels stay sRGB-encoded bytes;
        texture() decodes them to linear on read and writePixel encodes on
        write, so shaders only ever see linear values.
    */
    bool isSrgb() const { return m_srgb; }
    void setSrgb(const bool srgb) { m_srgb = srgb; }
    bool isSRGB() const { return m_srgb; }
    void setSRGB(const bool srgb) { m_srgb = srgb; }


private:
    std::vector<uint32_t>   m_pixels        = {};
    std::vector<float>      m_depth         = {};
    std::vector<uint8_t>    m_stencil       = {};
    uint32_t                m_width         = 0;
    uint32_t                m_height        = 0;
    
    // should this target be resized with window?
    bool                    m_trackWindow   = true;

    // are m_pixels sRGB-encoded? see isSrgb()
    bool                    m_srgb          = false;
};

} // namespace valo
