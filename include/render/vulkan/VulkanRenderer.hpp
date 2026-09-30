#pragma once

#ifdef VALO_HAS_VULKAN

#include "VulkanContext.hpp"
#include "VulkanSwapchain.hpp"
#include "VulkanFrames.hpp"
#include "../common/Math.hpp"

namespace valo {

class VulkanRenderer {
public:
    VulkanRenderer() = default;
    ~VulkanRenderer() { shutdown(); }

    VulkanRenderer(const VulkanRenderer&) = delete;
    VulkanRenderer& operator=(const VulkanRenderer&) = delete;

    bool init(Window& window, const uint32_t framesInFlight = 2, const bool validation = true);
    void shutdown();

    bool drawFrame(const Color& clearColor = {0.f, 0.f, 0.f, 1.f});

private:
    bool recreateSwapchain();

    Window*         m_window = nullptr;
    VulkanContext   m_ctx;
    VulkanSwapchain m_swapchain;
    VulkanFrames    m_frames;
    uint32_t        m_frameIndex = 0;

    bool m_needsRecreate = false;
};

} // namespace valo

#endif // VALO_HAS_VULKAN