#pragma once

#ifdef VALO_HAS_VULKAN

#include <vulkan/vulkan.h>

#include "../Window.hpp"

namespace valo {

class VulkanContext {
public:
    VulkanContext() = default;
    ~VulkanContext() { shutdown(); }

    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    bool init(Window& window, const bool validation = true);
    void shutdown();

    VkInstance instance() const { return m_instance; }
    VkSurfaceKHR surface() const { return m_surface; }

private:
    VkInstance                  m_instance          = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT    m_debug             = VK_NULL_HANDLE;
    VkSurfaceKHR                m_surface           = VK_NULL_HANDLE;
    VkPhysicalDevice            m_physical          = VK_NULL_HANDLE;
    uint32_t                    m_presentFamily     = UINT32_MAX; 
    uint32_t                    m_graphicsFamily    = UINT32_MAX;
    VkDevice                    m_device            = VK_NULL_HANDLE;
    VkQueue                     m_graphicsQueue     = VK_NULL_HANDLE;
    VkQueue                     m_presentQueue      = VK_NULL_HANDLE;
    bool                        m_portabilitySubset = false;

    bool pickPhysicalDevice();
    bool createLogicalDevice();
};

} // namespace valo

#endif // VALO_HAS_VULKAN