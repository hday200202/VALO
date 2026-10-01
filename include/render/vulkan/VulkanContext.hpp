#pragma once

#ifdef VALO_HAS_VULKAN

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

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
    VkPhysicalDevice physicalDevice() const { return m_physical; }
    VkDevice device() const { return m_device; }
    VkQueue graphicsQueue() const { return m_graphicsQueue; }
    VkQueue presentQueue() const { return m_presentQueue; }
    uint32_t graphicsFamily() const { return m_graphicsFamily; }
    uint32_t presentFamily() const { return m_presentFamily; }
    VmaAllocator allocator() const { return m_allocator; }
    VkCommandBuffer beginOneShot();
    bool endOneShot(VkCommandBuffer cmd);

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
    VmaAllocator                m_allocator         = VK_NULL_HANDLE;
    VkCommandPool               m_oneShotPool       = VK_NULL_HANDLE;
    
    bool                        m_portabilitySubset = false;

    bool pickPhysicalDevice();
    bool createLogicalDevice();
    bool createAllocator();
};

} // namespace valo

#endif // VALO_HAS_VULKAN