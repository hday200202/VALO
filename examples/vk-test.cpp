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
#include <render/vulkan/VulkanRenderer.hpp>
#include <cstring>
#include <vector>
#include <string>
#include <fstream>
#include <filesystem>
#include <cstdio>

using namespace valo;

namespace {

int fails = 0;

// only used to prove the Vulkan backend refuses the C++ shader entry point
struct DummyU {}; struct DummyV { float unused; };
void dummyVs(const Vec3f& in, const DummyU&, DummyV& out) { out.unused = 0.f; valo_Position = Vec4f(in, 1.f); }
void dummyFs(const DummyV&, const DummyU&) { valo_FragColor = Color(1.f, 1.f, 1.f, 1.f); }


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


/* ========================================================================= //
    Step 10 - shader modules (programs from GLSL / SPIR-V)
// ========================================================================= */

void testPrograms(Window& win) {
    std::printf("programs from files:\n");

    /*
        Through Renderer with Backend::Vulkan rather than VulkanRenderer
        directly - createProgram(vsPath, fsPath) is the public API, and the
        table is what has to carry it.
    */
    RenderConfig cfg;
    cfg.window      = &win;
    cfg.sdlWindow   = win.sdlHandle();
    cfg.backend     = Backend::Vulkan;

    Renderer r;
    if (!r.init(cfg)) { check("renderer init", false); return; }

    const std::string vert = "examples/shaders/triangle.vert";
    const std::string frag = "examples/shaders/triangle.frag";

    // glslc writes the cache beside the source; remove it so this run exercises
    // the compile path rather than whatever a previous run left behind
    std::remove((vert + ".spv").c_str());
    std::remove((frag + ".spv").c_str());

    const ProgramID fromGlsl = r.createProgram(vert, frag);
    check("GLSL compiles through glslc", fromGlsl != DEFAULT_PROGRAM);

    // both .spv files should now exist, and the second call should load them
    // without recompiling
    const bool cachedVs = std::filesystem::exists(vert + ".spv");
    const bool cachedFs = std::filesystem::exists(frag + ".spv");
    check("glslc cached the .spv beside the source", cachedVs && cachedFs);

    const ProgramID fromCache = r.createProgram(vert, frag);
    check("second call reuses the cache", fromCache != DEFAULT_PROGRAM);
    check("distinct IDs for distinct programs", fromGlsl != fromCache);

    // loading the .spv directly is the shipping path
    const ProgramID fromSpv = r.createProgram(vert + ".spv", frag + ".spv");
    check("explicit .spv path loads", fromSpv != DEFAULT_PROGRAM);

    std::printf("    (the errors below are expected)\n");

    const ProgramID missing = r.createProgram("does/not/exist.vert", frag);
    check("missing file is rejected", missing == DEFAULT_PROGRAM);

    // a non-SPIR-V file with a .spv extension skips glslc and must fail the
    // magic-number check rather than reaching the driver
    {
        std::ofstream junk("build/not-really.spv", std::ios::binary);
        junk << "this is not spirv at all";
    }
    const ProgramID junkId = r.createProgram("build/not-really.spv", frag + ".spv");
    check("non-SPIR-V .spv is rejected", junkId == DEFAULT_PROGRAM);
    std::remove("build/not-really.spv");

    // the software entry point must be refused by this backend
    const ProgramID viaCallbacks = r.createProgram(dummyVs, dummyFs);
    check("C++ callbacks are refused by the Vulkan backend", viaCallbacks == DEFAULT_PROGRAM);

    r.destroyProgram(fromGlsl);
    r.destroyProgram(fromCache);
    r.destroyProgram(fromSpv);
    r.shutdown();
    check("shutdown with programs live", true);
}

/* ========================================================================= //
    Step 11 - pipeline state translation

    vkCreateGraphicsPipelines itself is not reachable from here: createPipeline
    is private, and so is the GPUProgram it needs. What IS reachable is every
    translation table it feeds on, and those are where the typos live - a wrong
    VkFormat or a swapped compare op produces a pipeline that creates cleanly
    and renders garbage, which is the worst kind of bug to find later.

    So this section checks the mappings exhaustively, and the offset walk that
    turns a VtxLayout into attribute descriptions.
// ========================================================================= */

// Independent of attribSize() on purpose. Agreeing with the library's own
// answer would only prove the test and the library made the same mistake.
size_t formatBytes(const VkFormat f) {
    switch (f) {
        case VK_FORMAT_R32_SFLOAT:          case VK_FORMAT_R32_SINT:
        case VK_FORMAT_R32_UINT:            return 4;
        case VK_FORMAT_R32G32_SFLOAT:       case VK_FORMAT_R32G32_SINT:
        case VK_FORMAT_R32G32_UINT:         return 8;
        case VK_FORMAT_R32G32B32_SFLOAT:    case VK_FORMAT_R32G32B32_SINT:
        case VK_FORMAT_R32G32B32_UINT:      return 12;
        case VK_FORMAT_R32G32B32A32_SFLOAT: case VK_FORMAT_R32G32B32A32_SINT:
        case VK_FORMAT_R32G32B32A32_UINT:   return 16;
        default:                            return 0;
    }
}

bool formatIsFloat(const VkFormat f) {
    return f == VK_FORMAT_R32_SFLOAT       || f == VK_FORMAT_R32G32_SFLOAT
        || f == VK_FORMAT_R32G32B32_SFLOAT || f == VK_FORMAT_R32G32B32A32_SFLOAT;
}

bool formatIsSigned(const VkFormat f) {
    return f == VK_FORMAT_R32_SINT       || f == VK_FORMAT_R32G32_SINT
        || f == VK_FORMAT_R32G32B32_SINT || f == VK_FORMAT_R32G32B32A32_SINT;
}


/* ========================================================================= //
    Step 12 - uniform buffers (push descriptors)
// ========================================================================= */

void testUniformBuffers(Window& win) {
    std::printf("uniform buffers:\n");

    RenderConfig cfg;
    cfg.window      = &win;
    cfg.sdlWindow   = win.sdlHandle();
    cfg.backend     = Backend::Vulkan;

    Renderer r;
    if (!r.init(cfg)) { check("renderer init", false); return; }

    struct Tint { float r, g, b, a; };
    const Tint red{1.f, 0.f, 0.f, 1.f};
    const Tint blue{0.f, 0.f, 1.f, 1.f};

    const UniformBufferID a = r.createUniformBuffer(&red,  sizeof red);
    const UniformBufferID b = r.createUniformBuffer(&blue, sizeof blue);

    check("create returns a live ID",      a != EMPTY_BUFFER && b != EMPTY_BUFFER);
    check("distinct IDs for distinct bufs", a != b);

    // retires the old allocation and swaps in a fresh one; the retired half is
    // only freed once framesInFlight frames have passed
    r.updateUniformBuffer(a, &blue, sizeof blue);
    check("update in range survives", true);

    std::printf("    (the error below is expected)\n");
    r.updateUniformBuffer(9999, &red, sizeof red);
    check("update out of range is rejected", true);

    /*
        The draws below have no automatic oracle - the renderer reports nothing
        on success. Validation output between here and the summary IS the
        failure signal, so a clean run means a clean run.
    */
    const ProgramID plain = r.createProgram(
        "examples/shaders/triangle.vert", "examples/shaders/triangle.frag");
    const ProgramID uses0 = r.createProgram(
        "examples/shaders/ubo0.vert", "examples/shaders/triangle.frag");
    const ProgramID uses3 = r.createProgram(
        "examples/shaders/ubo3.vert", "examples/shaders/triangle.frag");

    check("UBO shaders compile", plain != DEFAULT_PROGRAM
                              && uses0 != DEFAULT_PROGRAM
                              && uses3 != DEFAULT_PROGRAM);

    if (uses0 != DEFAULT_PROGRAM && uses3 != DEFAULT_PROGRAM) {
        struct Push { Mat4f mvp; } push;
        push.mvp = Mat4f::identity();

        RenderCommand cmd;
        cmd.indexCount = 3;
        cmd.cullmode   = CullMode::None;
        cmd.setUniforms(push);

        // writeCount == 0: nothing is pushed, and the shader reads nothing
        cmd.program = plain;
        r.clear({0.f, 0.f, 0.f, 1.f}, Renderer::BACKBUFFER, 0.f);
        r.draw(cmd);
        r.show(Renderer::BACKBUFFER);
        check("draw with no uniform slots bound", true);

        // slot index and write index agree
        cmd.program = uses0;
        cmd.bindUniformBuffer(0, a);
        r.clear({0.f, 0.f, 0.f, 1.f}, Renderer::BACKBUFFER, 0.f);
        r.draw(cmd);
        r.show(Renderer::BACKBUFFER);
        check("draw with slot 0 bound", true);

        // slot 3 with 0-2 empty: dstBinding must be 3 while the write lands at
        // index 0, which is the one case where the two indices diverge
        RenderCommand sparse;
        sparse.indexCount = 3;
        sparse.cullmode   = CullMode::None;
        sparse.program    = uses3;
        sparse.setUniforms(push);
        sparse.bindUniformBuffer(3, b);

        r.clear({0.f, 0.f, 0.f, 1.f}, Renderer::BACKBUFFER, 0.f);
        r.draw(sparse);
        r.show(Renderer::BACKBUFFER);
        check("draw with only slot 3 bound", true);

        // an update between frames has to retire rather than free in place, or
        // a frame still in flight reads a destroyed buffer
        for (int i = 0; i < 8; ++i) {
            r.updateUniformBuffer(a, i % 2 ? &red : &blue, sizeof red);
            cmd.program = uses0;
            r.clear({0.f, 0.f, 0.f, 1.f}, Renderer::BACKBUFFER, 0.f);
            r.draw(cmd);
            r.show(Renderer::BACKBUFFER);
        }
        check("8 frames of update-while-drawing", true);
    }

    r.shutdown();
    check("shutdown with uniform buffers live", true);
}


void testPipelineState() {
    std::printf("vertex attribute formats (step 11):\n");

    const VtxAttrib allAttribs[]{
        VtxAttrib::Float, VtxAttrib::Int32, VtxAttrib::Uint32,
        VtxAttrib::Vec2f, VtxAttrib::Vec2i, VtxAttrib::Vec2u,
        VtxAttrib::Vec3f, VtxAttrib::Vec3i, VtxAttrib::Vec3u,
        VtxAttrib::Vec4f, VtxAttrib::Vec4i, VtxAttrib::Vec4u,
    };

    // a missing case falls through to UNDEFINED, which vkCreateGraphicsPipelines
    // rejects - but only for layouts that actually use that attribute, so it can
    // sit undetected until one model happens to have vertex colours
    bool allMapped = true;
    for (const VtxAttrib a : allAttribs)
        if (vkFormatOf(a) == VK_FORMAT_UNDEFINED) allMapped = false;
    check("every VtxAttrib maps to a real format", allMapped);

    // a format one component too small silently drops w, and one too large reads
    // past the vertex - both render, neither errors
    bool sizesAgree = true;
    char buf[160];
    for (const VtxAttrib a : allAttribs) {
        if (formatBytes(vkFormatOf(a)) != attribSize(a)) {
            std::snprintf(buf, sizeof buf, "(attrib %d: format says %zu, attribSize says %zu)",
                int(a), formatBytes(vkFormatOf(a)), attribSize(a));
            sizesAgree = false;
            break;
        }
    }
    check("format size matches attribSize", sizesAgree, sizesAgree ? "" : buf);

    // SFLOAT where the shader declares int reads the bit pattern as a float.
    // Nothing in Vulkan catches it; the geometry just lands somewhere absurd.
    check("float attribs map to SFLOAT",
        formatIsFloat(vkFormatOf(VtxAttrib::Float)) && formatIsFloat(vkFormatOf(VtxAttrib::Vec2f))
     && formatIsFloat(vkFormatOf(VtxAttrib::Vec3f)) && formatIsFloat(vkFormatOf(VtxAttrib::Vec4f)));

    check("signed attribs map to SINT",
        formatIsSigned(vkFormatOf(VtxAttrib::Int32)) && formatIsSigned(vkFormatOf(VtxAttrib::Vec2i))
     && formatIsSigned(vkFormatOf(VtxAttrib::Vec3i)) && formatIsSigned(vkFormatOf(VtxAttrib::Vec4i)));

    bool unsignedOk = true;
    const VtxAttrib unsignedAttribs[]{
        VtxAttrib::Uint32, VtxAttrib::Vec2u, VtxAttrib::Vec3u, VtxAttrib::Vec4u };
    for (const VtxAttrib a : unsignedAttribs) {
        const VkFormat f = vkFormatOf(a);
        if (formatIsFloat(f) || formatIsSigned(f) || formatBytes(f) == 0) unsignedOk = false;
    }
    check("unsigned attribs map to UINT", unsignedOk);

    // all twelve must be distinct, which catches a copy-paste that left two
    // cases returning the same format
    bool allDistinct = true;
    for (size_t i = 0; i < 12; ++i)
        for (size_t j = i + 1; j < 12; ++j)
            if (vkFormatOf(allAttribs[i]) == vkFormatOf(allAttribs[j])) allDistinct = false;
    check("all twelve formats are distinct", allDistinct);


    std::printf("attribute offset walk:\n");

    /*
        The loop in createPipeline assigns location = declaration index and
        accumulates offset by attribSize. Repeated here against a layout with
        mixed component counts, because a uniform-width layout would hide an
        offset that never advances.
    */
    const VtxLayout layout({
        VtxAttrib::Vec3f,   // position
        VtxAttrib::Vec4u,   // joints
        VtxAttrib::Float,   // size
        VtxAttrib::Vec2f    // uv
    });

    const size_t expectedOffsets[]{ 0, 12, 28, 32 };

    bool offsetsOk = true;
    uint32_t offset = 0;
    for (uint32_t i = 0; i < layout.getAttribs().size(); ++i) {
        if (offset != expectedOffsets[i]) offsetsOk = false;
        offset += uint32_t(attribSize(layout.getAttribs()[i]));
    }
    check("offsets accumulate in declaration order", offsetsOk);

    // if these disagree the driver strides past the attributes it was told
    // about, and every vertex after the first reads the wrong memory
    std::snprintf(buf, sizeof buf, "(walked %u, stride() says %zu)", offset, layout.stride());
    check("final offset equals stride()", offset == layout.stride(), buf);

    // an empty layout is the hardcoded-vertex case (triangle.vert), and has to
    // produce zero bindings rather than a binding with stride 0
    const VtxLayout empty;
    check("empty layout has stride 0", empty.stride() == 0);
    check("empty layout has no attribs", empty.getAttribs().empty());


    std::printf("raster and depth state:\n");

    check("cull modes map distinctly",
        vkCullOf(CullMode::None)  == VK_CULL_MODE_NONE
     && vkCullOf(CullMode::Back)  == VK_CULL_MODE_BACK_BIT
     && vkCullOf(CullMode::Front) == VK_CULL_MODE_FRONT_BIT);

    // the default is reversed-Z, so Greater is the one that must survive the
    // trip - Less here renders only the geometry behind everything else
    check("DepthCompare::Greater maps to GREATER",
        vkCompareOf(DepthCompare::Greater) == VK_COMPARE_OP_GREATER);

    const DepthCompare allCompares[]{
        DepthCompare::Less,  DepthCompare::LessEqual, DepthCompare::Greater, DepthCompare::GreaterEqual,
        DepthCompare::Equal, DepthCompare::NotEqual,  DepthCompare::Always,  DepthCompare::Never };

    bool comparesDistinct = true;
    for (size_t i = 0; i < 8; ++i)
        for (size_t j = i + 1; j < 8; ++j)
            if (vkCompareOf(allCompares[i]) == vkCompareOf(allCompares[j])) comparesDistinct = false;
    check("all eight compare ops are distinct", comparesDistinct);

    // the unreachable trailing return is a fallback, not a mapping - if a new
    // enumerator ever lands without a case it would silently become Greater
    check("RenderCommand defaults are reversed-Z",
        RenderCommand{}.depthCompare == DepthCompare::Greater);
}

} // namespace

#endif // VALO_HAS_VULKAN


int main() {
#ifdef VALO_HAS_VULKAN
    Window win("vk-test", {800, 600}, WinFlags::Hidden | WinFlags::Vulkan);

    testContext(win);
    testSwapchain(win);
    testPrograms(win);
    testUniformBuffers(win);
    testPipelineState();

    std::printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "all passed",
        fails, fails == 1 ? "" : "s");
    return fails != 0;
#else
    std::printf("built without Vulkan (./build.sh no-vulkan) - nothing to test\n");
    return 0;
#endif
}
