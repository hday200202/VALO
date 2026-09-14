#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <cassert>
#include <algorithm>
#include "common/Math.hpp"

namespace valo {

class RenderTarget {
public:
    RenderTarget() = default;
    RenderTarget(
        const uint32_t width, 
        const uint32_t height, 
        bool trackWindow = true
    ) : m_width(width), m_height(height), m_trackWindow(trackWindow)
    { m_pixels.resize(width * height); m_depth.resize(width * height); }

    // For Textures
    static RenderTarget loadFromMemory(
        uint32_t w, 
        uint32_t h, 
        uint32_t* data
    );

    static RenderTarget loadImageFromPath(const std::string& path);


    void setPixel(
        const Vec2u& xy, 
        const Color& color
    ) { 
        assert(xy.x < m_width && xy.y < m_height);
        m_pixels[(xy.y * m_width) + xy.x] = colorToABGR32(color);
    }


    void setPixel(
        const uint32_t x, 
        const uint32_t y, 
        const Color& color
    ) { 
        assert(x < m_width && y < m_height);
        m_pixels[(y * m_width) + x] = colorToABGR32(color); 
    }

    
    Color getPixel(
        const Vec2u& xy
    ) const {
        assert(xy.x < m_width && xy.y < m_height);
        uint32_t pixel = m_pixels[(xy.y * m_width) + xy.x];
        return ABGR32ToColor(pixel);
    }


    Color getPixel(
        const uint32_t x,
        const uint32_t y
    ) const {
        assert(x < m_width && y < m_height);
        uint32_t pixel = m_pixels[(y * m_width) + x];
        return ABGR32ToColor(pixel);
    }


    void setDepth(
        const Vec2u& xy,
        const float value
    ) {
        assert(xy.x < m_width && xy.y < m_height);
        m_depth[(xy.y * m_width) + xy.x] = value;
    }


    void setDepth(
        const uint32_t x,
        const uint32_t y,
        const float value
    ) {
        assert(x < m_width && y < m_height);
        m_depth[(y * m_width) + x] = value;
    }


    float getDepth(
        const Vec2u& xy
    ) const {
        assert(xy.x < m_width && xy.y < m_height);
        return m_depth[(xy.y * m_width) + xy.x];
    }


    float getDepth(
        const uint32_t x,
        const uint32_t y
    ) const {
        assert(x < m_width && y < m_height);
        return m_depth[(y * m_width) + x];
    }


    void resize(
        const uint32_t width,
        const uint32_t height
    ) { 
        m_width     = width;
        m_height    = height;
        m_pixels.resize(width * height);
        m_depth.resize(width * height);
    }


    void clear(
        const Color& color = {0.f, 0.f, 0.f, 1.f}
    ) { 
        uint32_t col = colorToABGR32(color);
        std::fill(m_pixels.begin(), m_pixels.end(), col); 
        std::fill(m_depth.begin(), m_depth.end(), 1.f);
    }


    uint32_t width() const { return m_width; }
    uint32_t height() const { return m_height; }
    uint32_t* data() { return m_pixels.data(); }
    float* depthData() { return m_depth.data(); }
    const uint32_t* data() const { return m_pixels.data(); }
    const float* depthData() const { return m_depth.data(); }
    bool tracksWindow() const { return m_trackWindow; }
    void setTrackWindow(const bool trackWindow) { m_trackWindow = trackWindow; }


private:
    std::vector<uint32_t>   m_pixels        = {};
    std::vector<float>      m_depth         = {};
    uint32_t                m_width         = 0;
    uint32_t                m_height        = 0;
    
    // should this target be resized with window?
    bool                    m_trackWindow   = true;
};




} // namespace valo
