#include "RenderTarget.hpp"

#define STB_IMAGE_IMPLEMENTATION

#include <stb_image.h>
#include <cstring>
#include <string>
#include <iostream>

namespace valo {

uint8_t* getImageData(
    const std::string& filePath, 
    int* w, 
    int* h, 
    int* channels
);


// For Textures
RenderTarget RenderTarget::loadFromMemory(
    uint32_t w, 
    uint32_t h, 
    uint32_t* data
) {
    RenderTarget tex;
    tex.m_width = w;
    tex.m_height = h;
    tex.m_pixels.resize(w * h);
    std::memcpy(
        tex.m_pixels.data(), 
        data, 
        sizeof(uint32_t) * tex.width() * tex.height()
    );

    return tex;
    // don't initialize depth buffer.
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
    stbi_image_free(data);
    return tex;
}


uint8_t* getImageData(
    const std::string& filePath, 
    int* w, 
    int* h, 
    int* channels
) { return stbi_load(filePath.c_str(), w, h, channels, 4); }

} // namespace valo
