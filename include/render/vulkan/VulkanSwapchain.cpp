#include "VulkanSwapchain.hpp"

#ifdef VALO_HAS_VULKAN

#include <algorithm>
#include <cstdio>

namespace valo {

namespace {

/*
    Mirrors RenderConfig::srgbBackbuffer, which on the software side decides
    whether writePixel encodes. An _SRGB swapchain makes the hardware encode on
    write: right for a shader that outputs linear, wrong for one that already
    gamma-encodes in its own tonemap - that one gets encoded twice and comes out
    washed out.
*/
VkSurfaceFormatKHR chooseFormat(VkPhysicalDevice phys, VkSurfaceKHR surface, const bool srgb) {
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &n, formats.data());

    const VkFormat want = srgb ? VK_FORMAT_B8G8R8A8_SRGB : VK_FORMAT_B8G8R8A8_UNORM;

    for (const auto& f : formats)
        if (f.format == want && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            return f;

    return formats[0];
}

VkPresentModeKHR choosePresentMode(VkPhysicalDevice phys, VkSurfaceKHR surface, const bool vsync) {
    if (vsync) return VK_PRESENT_MODE_FIFO_KHR;

    uint32_t n = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &n, nullptr);
    std::vector<VkPresentModeKHR> modes(n);
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &n, modes.data());

    for (auto m : modes)
        if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) return m;

    return VK_PRESENT_MODE_FIFO_KHR;
}

} // namespace

bool VulkanSwapchain::init(VulkanContext& ctx, const Vec2u& size, const bool vsync, const bool srgb) {
    m_ctx = &ctx;

    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx.physicalDevice(), ctx.surface(), &caps);

    const VkSurfaceFormatKHR surfaceFormat = chooseFormat(ctx.physicalDevice(), ctx.surface(), srgb);
    const VkPresentModeKHR presentMode = choosePresentMode(ctx.physicalDevice(), ctx.surface(), vsync);

    if (caps.currentExtent.width != UINT32_MAX)
        m_extent = caps.currentExtent;
    else {
        m_extent.width = std::clamp(size.x, caps.minImageExtent.width, caps.maxImageExtent.width);
        m_extent.height = std::clamp(size.y, caps.minImageExtent.height, caps.maxImageExtent.height);
    }

    if (m_extent.width == 0 || m_extent.height == 0) return false;

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0)
        imageCount = std::min(imageCount, caps.maxImageCount);

    const uint32_t families[2] = { ctx.graphicsFamily(), ctx.presentFamily() };
    const bool shared = families[0] != families[1];

    VkSwapchainCreateInfoKHR ci{
        .sType                  = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface                = ctx.surface(),
        .minImageCount          = imageCount,
        .imageFormat            = surfaceFormat.format,
        .imageColorSpace        = surfaceFormat.colorSpace,
        .imageExtent            = m_extent,
        .imageArrayLayers       = 1,
        .imageUsage             = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT 
                                | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode       = shared ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount  = shared ? 2u : 0u,
        .pQueueFamilyIndices    = shared ? families : nullptr,
        .preTransform           = caps.currentTransform,
        .compositeAlpha         = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode            = presentMode,
        .clipped                = VK_TRUE,
        .oldSwapchain           = VK_NULL_HANDLE
    };

    if (vkCreateSwapchainKHR(ctx.device(), &ci, nullptr, &m_swapchain) != VK_SUCCESS) {
        std::fprintf(stderr, "[vulkan] vkCreateSwapchainKHR failed\n");
        return false;
    }

    m_format = surfaceFormat.format;

    uint32_t n = 0;
    vkGetSwapchainImagesKHR(ctx.device(), m_swapchain, &n, nullptr);
    m_images.resize(n);
    vkGetSwapchainImagesKHR(ctx.device(), m_swapchain, &n, m_images.data());

    m_views.resize(m_images.size());

    for (size_t i = 0; i < m_images.size(); ++i) {
        const VkImageViewCreateInfo vi{
            .sType              = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image              = m_images[i],
            .viewType           = VK_IMAGE_VIEW_TYPE_2D,
            .format             = m_format,
            .subresourceRange   = {
                .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel   = 0,
                .levelCount     = 1,
                .baseArrayLayer = 0,
                .layerCount     = 1
            }
        };

        if (vkCreateImageView(ctx.device(), &vi, nullptr, &m_views[i]) != VK_SUCCESS) {
            std::fprintf(stderr, "[vulkan] vkCreateImageView failed for image %zu\n", i);
            return false;
        }
    }

    std::fprintf(
        stderr, "[vulkan] swapchain %ux%u, %u images, %s\n",
        m_extent.width, m_extent.height, n,
        presentMode == VK_PRESENT_MODE_FIFO_KHR ? "FIFO" : "IMMEDIATE"
    );
    
    return true;
}

void VulkanSwapchain::shutdown() {
    if (m_ctx) {
        for (auto v : m_views)
            if (v) vkDestroyImageView(m_ctx->device(), v, nullptr);
    }

    m_views.clear();

    if (m_swapchain && m_ctx) {
        vkDestroySwapchainKHR(m_ctx->device(), m_swapchain, nullptr);
        m_swapchain = VK_NULL_HANDLE;
    }

    m_images.clear();
    m_extent = {0, 0};
}

} // namespace valo

#endif // VALO_HAS_VULKAN