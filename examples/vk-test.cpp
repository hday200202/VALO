/*
    Vulkan bring-up checks. Grows a section per step in VULKAN_SETUP.md, so a
    regression anywhere in the chain shows up as a named failure rather than as
    a blank window five steps later.

    Needs the vendored loader, ICD and layers:  source ext/vulkan/env.sh
    Exits non-zero on any failure, so it is usable as a build gate.
*/
#include <VALO.hpp>
#include <cstdio>

#ifdef VALO_HAS_VULKAN

#include <render/vulkan/VulkanSwapchain.hpp>
#include <cstring>
#include <vector>

using namespace valo;

namespace {

int fails = 0;

void check(const char* what, const bool ok, const char* detail = "") {
    std::printf("  %-42s %s %s\n", what, ok ? "ok  " : "FAIL", detail);
    if (!ok) ++fails;
}

const char* formatName(const VkFormat f) {
    switch (f) {
        case VK_FORMAT_B8G8R8A8_SRGB:  return "B8G8R8A8_SRGB";
        case VK_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
        case VK_FORMAT_R8G8B8A8_SRGB:  return "R8G8B8A8_SRGB";
        case VK_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
        default:                       return "other";
    }
}


/* ---- steps 1-3: instance, debug messenger, surface, device -------------- */

void testContext(Window& win) {
    std::printf("context (steps 1-3):\n");

    VulkanContext ctx;
    if (!ctx.init(win, true)) {
        // querying handles from a dead context aborts inside the loader, so
        // everything below would crash rather than report
        check("init", false, "(skipping the rest - did you source ext/vulkan/env.sh?)");
        return;
    }
    check("init", true);
    check("instance non-null",        ctx.instance()       != VK_NULL_HANDLE);
    check("surface non-null",         ctx.surface()        != VK_NULL_HANDLE);
    check("physical device non-null", ctx.physicalDevice() != VK_NULL_HANDLE);
    check("logical device non-null",  ctx.device()         != VK_NULL_HANDLE);
    check("graphics queue non-null",  ctx.graphicsQueue()  != VK_NULL_HANDLE);
    check("present queue non-null",   ctx.presentQueue()   != VK_NULL_HANDLE);

    // the chosen family has to actually support present, or the swapchain is
    // built on an assumption rather than a check
    VkBool32 canPresent = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(
        ctx.physicalDevice(), ctx.presentFamily(), ctx.surface(), &canPresent);
    check("present family supports present", canPresent == VK_TRUE);

    ctx.shutdown();
    ctx.shutdown();
    check("double shutdown survived", true);
}


/* ---- step 4: swapchain -------------------------------------------------- */

void testSwapchain(Window& win) {
    VulkanContext ctx;
    if (!ctx.init(win, true)) { check("context init for swapchain tests", false); return; }

    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx.physicalDevice(), ctx.surface(), &caps);

    char buf[160];

    std::printf("swapchain, vsync (step 4):\n");
    {
        VulkanSwapchain sc;
        if (!sc.init(ctx, {800, 600}, true)) {
            check("init", false);
        } else {
            check("handle non-null", sc.handle() != VK_NULL_HANDLE);

            // an _UNORM swapchain would look washed out next to the software
            // backend, and read as a shader bug rather than a format one
            std::snprintf(buf, sizeof buf, "(%s)", formatName(sc.format()));
            check("format is _SRGB", sc.format() == VK_FORMAT_B8G8R8A8_SRGB
                                  || sc.format() == VK_FORMAT_R8G8B8A8_SRGB, buf);

            std::snprintf(buf, sizeof buf, "(%ux%u, surface says %ux%u)",
                sc.extent().width, sc.extent().height,
                caps.currentExtent.width, caps.currentExtent.height);
            check("extent matches the surface", caps.currentExtent.width == UINT32_MAX
                || (sc.extent().width  == caps.currentExtent.width
                 && sc.extent().height == caps.currentExtent.height), buf);

            std::snprintf(buf, sizeof buf, "(%u, min %u max %u)",
                sc.imageCount(), caps.minImageCount, caps.maxImageCount);
            check("imageCount within surface limits",
                sc.imageCount() >= caps.minImageCount
                && (caps.maxImageCount == 0 || sc.imageCount() <= caps.maxImageCount), buf);

            // the driver may hand back more images than we asked for, so the
            // count has to come from the driver, not from the request
            uint32_t n = 0;
            vkGetSwapchainImagesKHR(ctx.device(), sc.handle(), &n, nullptr);
            std::vector<VkImage> images(n);
            vkGetSwapchainImagesKHR(ctx.device(), sc.handle(), &n, images.data());

            bool allLive = n > 0;
            for (VkImage i : images) if (i == VK_NULL_HANDLE) allLive = false;
            check("all images non-null", allLive);
            check("imageCount() agrees with the driver", n == sc.imageCount());

            sc.shutdown();
            sc.shutdown();
            check("double shutdown survived", true);
        }
    }

    std::printf("swapchain, no vsync:\n");
    {
        VulkanSwapchain sc;
        check("init", sc.init(ctx, {800, 600}, false));
        check("handle non-null", sc.handle() != VK_NULL_HANDLE);
        sc.shutdown();
    }

    std::printf("swapchain, repeated rebuild (step 9 dry run):\n");
    {
        const Vec2u sizes[] = {{1, 1}, {320, 240}, {1920, 1080}, {800, 600}, {17, 4099}};
        bool allOk = true;
        for (int i = 0; i < 20; ++i) {
            VulkanSwapchain sc;
            if (!sc.init(ctx, sizes[i % 5], true)) { allOk = false; break; }
            sc.shutdown();
        }
        check("20 create/destroy cycles", allOk);
    }

    // A surface holds exactly one swapchain. A second while the first is live
    // must be refused unless the first is passed as oldSwapchain - which is
    // what the resize path in step 9 has to do.
    std::printf("one swapchain per surface:\n");
    {
        VulkanSwapchain a, b;
        check("first created", a.init(ctx, {800, 600}, true));
        std::printf("    (the VK_ERROR_NATIVE_WINDOW_IN_USE_KHR below is expected)\n");
        check("second while first is live is refused", !b.init(ctx, {800, 600}, true));
        a.shutdown();
        check("creating again after shutdown works", b.init(ctx, {800, 600}, true));
        b.shutdown();
    }

    ctx.shutdown();
}

} // namespace

#endif // VALO_HAS_VULKAN


int main() {
#ifdef VALO_HAS_VULKAN
    Window win("vk-test", {800, 600}, WinFlags::Hidden | WinFlags::Vulkan);

    testContext(win);
    testSwapchain(win);

    std::printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "all passed",
        fails, fails == 1 ? "" : "s");
    return fails != 0;
#else
    std::printf("built without Vulkan (./build.sh no-vulkan) - nothing to test\n");
    return 0;
#endif
}
