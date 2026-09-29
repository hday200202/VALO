# Vulkan bring-up — step by step

Working doc for **PLAN.md phase 1**. End state: a window that clears to a colour,
validation-clean, surviving resize and shutdown. No drawing, no resources.

Everything lives in `include/render/vulkan/`, guarded on `VALO_HAS_VULKAN`, and
touches nothing in `Renderer`. The interface split (PLAN.md 0c) stays parked until
after this works.

**Before running anything:** `source ext/vulkan/env.sh`. The vendored loader,
MoltenVK ICD and validation layers are not in any system search path.

---

## What this machine actually reports

Probed 2026-09-28 on the M4, so the plan below is not guesswork:

```
Apple M4, api 1.3.357
queue families: 4, all GRAPHICS|COMPUTE|TRANSFER, count=1 each
dynamicRendering=1  synchronization2=1  fillModeNonSolid=1
VK_KHR_swapchain  VK_KHR_portability_subset  VK_EXT_scalar_block_layout
```

Everything the plan assumed is available. `portability_subset` being present is
the one macOS-specific consequence — see step 2.

---

## Step 1 — instance + debug messenger — DONE

`VulkanContext::init()` creates a `VkInstance` with `VK_LAYER_KHRONOS_validation`
and a `VkDebugUtilsMessengerEXT`. The messenger comes first so every later step
has diagnostics from the start.

Three things already in and worth not undoing:

- `VK_KHR_portability_enumeration` **and** the matching
  `VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR` flag. MoltenVK is a
  portability driver, so without both the instance reports **zero** physical
  devices and no error saying why.
- The messenger create-info chained into `VkInstanceCreateInfo::pNext`. A
  messenger created *after* the instance cannot report errors that happened
  *during* `vkCreateInstance`; chaining covers that window. Same struct, used
  twice, deliberately.
- Callback returns `VK_FALSE`. `VK_TRUE` aborts the offending call.

*Verified:* instance created, 1 device, the callback's own format string appears
(so it is our messenger, not the layer's default output), double `shutdown()`
is safe.

---

## Step 2 — physical device, logical device, graphics queue — DONE (2026-09-29)

`pickPhysicalDevice()` scores devices, rejecting any without a graphics queue,
a present-capable queue and `VK_KHR_swapchain`. `createLogicalDevice()` emits
one `VkDeviceQueueCreateInfo` per *unique* family and enables
`VK_KHR_portability_subset` when the device advertises it. On this machine:
`AMD Radeon(TM) 890M Graphics (graphics q=0, present q=0)`.

Two bugs this cost, both caught by validation rather than by reading:

- `(m_graphicsFamily = m_presentFamily)` — a single `=` in the dedupe test, so
  the unified case emitted two identical queue infos and tripped
  `VUID-VkDeviceCreateInfo-queueFamilyIndex-02802`. It also silently clobbered
  `m_graphicsFamily` on hardware where the families genuinely differ.
- The debug messenger was created *last* in `init()`, so that error printed as
  the layer's raw `Validation Error:` instead of through the callback. The
  messenger now comes straight after `vkCreateInstance`, which is the point of
  having it first.


Pick a device, find a graphics queue family, create the logical device, retrieve
the queue.

**Selection:** score by `deviceType` (discrete 3, integrated 2, other 1) and
require a graphics queue family. On this Mac there is one device and family 0
works; the scoring is for the Windows machine, where a discrete GPU sits
alongside an integrated one.

**Device extensions:** `VK_KHR_swapchain`, plus `"VK_KHR_portability_subset"`
**if the device advertises it**. When advertised, the spec *requires* enabling it
— skip it and `vkCreateDevice` trips validation. Use the string literal: the
`VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME` macro lives in `vulkan_beta.h` and
needs `VK_ENABLE_BETA_EXTENSIONS`.

**Features** go through `VkPhysicalDeviceFeatures2` chained into
`VkDeviceCreateInfo::pNext`:

```cpp
    VkPhysicalDeviceVulkan13Features f13{
        .sType            = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .synchronization2 = VK_TRUE,
        .dynamicRendering = VK_TRUE
    };

    VkPhysicalDeviceFeatures2 features{
        .sType    = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext    = &f13,
        .features = { .fillModeNonSolid = VK_TRUE }   // FillMode::Wireframe
    };
```

`pEnabledFeatures` must stay null — it and a `pNext` feature chain are mutually
exclusive, so once `dynamicRendering` is needed, *everything* including the old
`VkPhysicalDeviceFeatures` fields goes through `Features2`.

**Teardown** is strict reverse order: device before instance, and
`vkDeviceWaitIdle` before `vkDestroyDevice`. A no-op today, undefined behaviour
once there is a frame loop.

*Verify:* `[vulkan] using Apple M4 (queue family 0)`, zero validation output,
clean shutdown.

---

## Step 3 — surface — DONE (2026-09-29)

Instance extensions come from `SDL_Vulkan_GetInstanceExtensions()` rather than
being hardcoded, so the platform surface extension is right on all three OSes.

Two traps, both hit:

- The SDL extensions were appended to the vector *after* `VkInstanceCreateInfo`
  was constructed, so `enabledExtensionCount` was stale **and**
  `extensions.data()` dangled after the reallocation. It surfaced as
  `loader_validate_instance_extensions: ... string that is too long or is badly
  formed`, which does not sound like an ordering bug at all. Build the full
  list before the struct that points at it.
- SDL needs `SDL_WINDOW_VULKAN` at **creation** time; it cannot be added
  afterwards. `WinFlags::Vulkan = 0x10000000` now exists. Note `Window.cpp`
  recreates the window elsewhere — that path has to carry the flag too, or a
  resize silently drops Vulkan support.

Still open: `VK_KHR_portability_enumeration` is requested unconditionally at
instance level. It works on Windows only because the loader implements it;
an older loader would fail with `VK_ERROR_EXTENSION_NOT_PRESENT`. It wants the
same availability check the layer already gets. `vkCreateInstance`'s `VkResult`
is also discarded — it distinguishes LAYER_NOT_PRESENT from
EXTENSION_NOT_PRESENT from INCOMPATIBLE_DRIVER, which are three different
fixes on someone else's machine.


SDL owns the window, so it creates the surface:
`SDL_Vulkan_CreateSurface(window, instance, nullptr, &surface)`.

**The instance extension list has to change.** Surface extensions
(`VK_KHR_surface` plus a platform one) must be enabled at *instance* creation, so
query them with `SDL_Vulkan_GetInstanceExtensions` in step 1's code and append
before `vkCreateInstance`. This is the one step that reaches backwards.

**Present support is a separate query** from graphics support.
`vkGetPhysicalDeviceSurfaceSupportKHR(device, family, surface, &supported)` — a
family with `VK_QUEUE_GRAPHICS_BIT` is not automatically presentable. On desktop
they coincide in practice, so assert it rather than building a second queue path
that never runs.

Ordering note: strictly, device selection should verify present support, which
means the surface must exist before step 2's `pickPhysicalDevice`. Easiest fix is
to create the surface between instance and device rather than after both.

*Verify:* a non-null `VkSurfaceKHR`, and the chosen family reports present
support.

---

## Step 4 — swapchain

The rotating set of images the window presents from. Three queries feed it:

| Query | Choose |
|---|---|
| `vkGetPhysicalDeviceSurfaceCapabilitiesKHR` | image count, extent, transform |
| `vkGetPhysicalDeviceSurfaceFormatsKHR` | prefer `B8G8R8A8_SRGB` + `SRGB_NONLINEAR` |
| `vkGetPhysicalDeviceSurfacePresentModesKHR` | `FIFO` (always present, = vsync) |

**Pick the sRGB format deliberately.** VALO's backbuffer already encodes sRGB on
write (`RenderConfig::srgbBackbuffer`), so an `_SRGB` swapchain format is what
makes the two backends match. A `_UNORM` format would make everything look
washed out relative to the software renderer, and it would look like a shader bug.

**Image count:** `capabilities.minImageCount + 1`, clamped to `maxImageCount` when
that is non-zero (zero means no limit).

**Extent:** when `capabilities.currentExtent.width == UINT32_MAX` the surface is
telling you to pick; otherwise use what it says. On a HiDPI Mac this must come
from the *pixel* size, not the logical window size — `Window` already
distinguishes these and gets it right, so reuse it.

`usage` needs `VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT`. Add
`VK_IMAGE_USAGE_TRANSFER_DST_BIT` now if you ever want to blit into it.

*Verify:* swapchain created, image count printed, no validation output.

---

## Step 5 — image views

`vkGetSwapchainImagesKHR` gives the images (owned by the swapchain, never
destroyed by you). Each needs a `VkImageView` — a typed window onto the image —
and those you *do* own and destroy.

Small step, no traps, but it is the one that makes the resize path in step 9 have
something to tear down.

*Verify:* one view per image, all non-null.

---

## Step 6 — command pool and command buffers

One `VkCommandPool` on the graphics family, with
`VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT` so buffers can be re-recorded
individually each frame.

Allocate `framesInFlight` primary command buffers — `RenderConfig::framesInFlight`
already exists and defaults to 2, and step 7 is where it starts meaning something
on this side too.

**Per frame in flight, not per swapchain image.** These are different numbers and
conflating them is the classic bug: the CPU is allowed to run ahead by
`framesInFlight`, which has nothing to do with how many images the driver gave
you.

*Verify:* pool and buffers allocated, clean shutdown.

---

## Step 7 — synchronisation

Per frame in flight:

- `VkSemaphore imageAvailable` — acquire signals it, the submit waits on it
- `VkFence inFlight` — created **signalled**, so frame 0 does not deadlock

Per swapchain **image**:

- `VkSemaphore renderFinished` — submit signals it, present waits on it

**These two counts differ, and that matters.** `renderFinished` has to be
per-image rather than per-frame: present consumes it, and with fewer frames in
flight than images you can otherwise wait on a semaphore that is still pending.
This is a real, frequently-hit validation error, not a theoretical one.

The closest thing you already own is `ThreadPool`'s generation counter and
active-worker count — same idea, "has this work finished", just expressed as GPU
objects.

*Verify:* nothing visible yet; correctness shows up in step 8.

---

## Step 8 — the frame loop, clearing

Per frame:

1. `vkWaitForFences` on this frame's fence, then `vkResetFences`
2. `vkAcquireNextImageKHR` → image index, signals `imageAvailable`
3. `vkResetCommandBuffer`, `vkBeginCommandBuffer`
4. **Barrier:** `UNDEFINED` → `COLOR_ATTACHMENT_OPTIMAL` for that image
5. `vkCmdBeginRendering` with the image view as the colour attachment,
   `loadOp = CLEAR`, `storeOp = STORE`, and the clear colour
6. `vkCmdEndRendering`
7. **Barrier:** `COLOR_ATTACHMENT_OPTIMAL` → `PRESENT_SRC_KHR`
8. `vkEndCommandBuffer`, `vkQueueSubmit2` with the fence
9. `vkQueuePresentKHR` waiting on `renderFinished`

**Dynamic rendering means no `VkRenderPass` and no `VkFramebuffer`** — that is the
whole point of the choice in PLAN.md, and it deletes the two hardest objects from
the classic tutorial. `vkCmdBeginRendering` names its attachments inline.

**The two barriers are the price.** A render pass would have done these layout
transitions implicitly; with dynamic rendering they are explicit and yours. This
is the concept with no software analogue — a `RenderTarget` is just memory, always
readable, whereas a `VkImage` has a current layout that determines what the GPU
may legally do with it. Expect to get these wrong once; the validation message
names both the expected and actual layout, which makes it self-correcting.

Use `vkQueueSubmit2` and `VkImageMemoryBarrier2` (synchronization2, already
enabled in step 2) rather than the older forms — same concepts, less redundant
struct filling.

*Verify:* the window clears to a colour. Change the colour, confirm it changes.
Zero validation output.

---

## Step 9 — resize

`vkAcquireNextImageKHR` and `vkQueuePresentKHR` return
`VK_ERROR_OUT_OF_DATE_KHR` or `VK_SUBOPTIMAL_KHR` when the swapchain no longer
matches the window. Both are easy to ignore, because on some drivers it appears to
work anyway.

On either: `vkDeviceWaitIdle`, destroy the views and swapchain, rebuild steps 4–5.
Keep the surface, device and command pool.

`Window::resized()` already exists and `VALO::resize()` already routes through it,
so the hook is there.

**Handle this now.** Retrofitting it later means unpicking the frame loop, and a
zero-sized window (minimised) has to be skipped rather than producing a zero-extent
swapchain — the same 0×0 case the software backend already guards in
`resolveClipRect`.

*Verify:* resize the window fifty times, including dragging slowly across many
sizes, then minimise and restore. Zero validation output, no leak.

---

## Done when

- window clears to a colour, validation-clean
- resize and minimise/restore survive
- shutdown is clean, and clean under the validation layer's leak check

That is PLAN.md phase 1. Phase 2 is one triangle, and after that 0c — the
interface split — with Vulkan experience actually in hand.
