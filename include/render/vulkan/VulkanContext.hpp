#pragma once

#ifdef VALO_HAS_VULKAN

#include <vulkan/vulkan.h>

namespace valo {

class VulkanContext {
public:
    VulkanContext() = default;
    ~VulkanContext() { shutdown(); }

    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    bool init(const bool validation = true);
    void shutdown();

    VkInstance instance() const { return m_instance; }

private:
    VkInstance                  m_instance  = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT    m_debug     = VK_NULL_HANDLE;
};

} // namespace valo

#endif // VALO_HAS_VULKAN