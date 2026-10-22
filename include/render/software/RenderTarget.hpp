#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <cassert>
#include <algorithm>
#include "../common/Math.hpp"
#include "Srgb.hpp"

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

    void generateMips();

    /*
        Multisampling, for the backbuffer (RenderConfig::msaa). Colour, depth and
        stencil are kept per sample; the image itself - data(), what is shown and
        sampled - is the resolve, made by resolve().

        Colour is compressed per pixel the way GPU colour compression is: while
        every sample of a pixel holds the same colour it lives once, in the
        image, and only a pixel an edge has split keeps all of its samples. So a
        fully covered pixel costs what it does without multisampling, and only
        edges pay per sample.

        The pixel-level depth and stencil accessors read sample 0 and write every
        sample, which is what a line or point - drawn with full coverage - wants.
    */
    void     setSamples(const uint32_t samples);
    uint32_t samples() const { return m_samples; }

    // false once an edge has split the pixel's samples
    bool     pixelUniform(const uint32_t x, const uint32_t y) const {
        return m_uniform[size_t(y) * m_width + x] != 0;
    }
    uint32_t* sampleColors(const uint32_t x, const uint32_t y) {
        return m_sampleColor.data() + (size_t(y) * m_width + x) * m_samples;
    }
    // copies the pixel's colour into each of its samples, so they can diverge
    void     splitPixel(const uint32_t x, const uint32_t y);
    // every sample holds `abgr` again, so the pixel is stored once
    void     joinPixel(const uint32_t x, const uint32_t y, const uint32_t abgr);

    float   getDepthSample(const uint32_t x, const uint32_t y, const uint32_t s) const {
        return m_sampleDepth[(size_t(y) * m_width + x) * m_samples + s];
    }
    void    setDepthSample(const uint32_t x, const uint32_t y, const uint32_t s, const float value) {
        m_sampleDepth[(size_t(y) * m_width + x) * m_samples + s] = value;
    }
    uint8_t getStencilSample(const uint32_t x, const uint32_t y, const uint32_t s) const {
        return m_sampleStencil[(size_t(y) * m_width + x) * m_samples + s];
    }
    void    setStencilSample(const uint32_t x, const uint32_t y, const uint32_t s, const uint8_t value) {
        m_sampleStencil[(size_t(y) * m_width + x) * m_samples + s] = value;
    }

    // averages every split pixel's samples into the image
    void     resolveRows(const uint32_t y0, const uint32_t y1);

    uint32_t mipCount() const { return uint32_t(m_mips.size()) + 1; }

    const uint32_t* mipData(const uint32_t level) const {
        return level == 0 ? m_pixels.data() : m_mips[level - 1].data();
    }

    Vec2u mipSize(const uint32_t level) const {
        return level == 0 ? Vec2u{m_width, m_height} : m_mipSizes[level - 1];
    }

private:
    std::vector<uint32_t>                   m_pixels        = {};
    std::vector<std::vector<uint32_t>>      m_mips          = {};
    std::vector<Vec2u>                      m_mipSizes      = {};
    std::vector<float>                      m_depth         = {};
    std::vector<uint8_t>                    m_stencil       = {};
    uint32_t                                m_width         = 0;
    uint32_t                                m_height        = 0;
    
    // should this target be resized with window?
    bool                                    m_trackWindow   = true;

    // are m_pixels sRGB-encoded? see isSrgb()
    bool                                    m_srgb          = false;

    // multisampling - see setSamples()
    uint32_t                                m_samples       = 1;
    std::vector<uint8_t>                    m_uniform       = {};
    std::vector<uint32_t>                   m_sampleColor   = {};
    std::vector<float>                      m_sampleDepth   = {};
    std::vector<uint8_t>                    m_sampleStencil = {};
};

} // namespace valo
