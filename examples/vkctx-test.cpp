#include <render/vulkan/VulkanContext.hpp>
#include <VALO.hpp>
#include <cstdio>
int main() {
#ifdef VALO_HAS_VULKAN
    valo::Window win("vkctx", {320,240}, valo::WinFlags::Hidden | valo::WinFlags::Vulkan);
    valo::VulkanContext ctx;
    if (!ctx.init(win, true)) { std::printf("RESULT: init returned false\n"); return 1; }
    std::printf("RESULT: init ok\n");
    ctx.shutdown();
    ctx.shutdown();
    std::printf("RESULT: shutdown x2 survived\n");
#endif
    return 0;
}
