# VALO work needed before UILO can use it

UILO ([the parent repo](../../)) is moving its rendering, window and input off
bgfx + SDL and onto VALO. This file lists everything on VALO's side that blocks
that move, in the order it should be done. Each item has why UILO needs it,
where the code is now, what to change, and how to know it's done.

UILO's side of the migration is tracked separately in `../../RENDERER_MIGRATION.md`.
Don't change UILO from here.

## Ground rules

- **The public API is settled. Changes are additive only.** New methods,
  new fields with defaults, new overloads: fine. Renaming, removing or changing
  the meaning of an existing call: not fine. The one deliberate exception is
  item 5 (blend alpha), which changes software output only in a target's alpha
  channel.
- **Both backends, every time.** Anything that exists on one backend works the
  same on the other, or is explicitly a no-op there (and says so in a comment).
- **Match the existing code.** Comment density, the `/* ---- section ---- */`
  banners, `SCREAMING_SNAKE_CASE` constants, assert + release-build early return
  on bad input, and the "why, not what" comment style. Read `TODO.md` and
  `PLAN.md` first; they explain decisions you shouldn't undo.
- **Verify the way VALO already does:** a small harness (see `TODO.md` for the
  `*test` pattern), ASan/UBSan and TSan clean, Vulkan validation layers clean
  (`RenderConfig::validation = true`), and every existing example still runs with
  and without `--vulkan`.
- **When an item lands, tick it here and add a short entry to `TODO.md`**, in
  the same voice as the existing entries.

---

## 1. Vulkan: per-frame updates must not stall the GPU

**Why UILO needs it.** UILO rebuilds one vertex/index arena every frame and
uploads it once before `endFrame`. It also updates glyph atlases with
`updateTextureRect` whenever new characters appear. Today each of those calls
drains the GPU queue, so every UI frame waits for the previous one to finish
completely and frames in flight do nothing.

**Where it is now.**
- `VulkanContext::endOneShot` ([VulkanContext.cpp:372](include/render/vulkan/VulkanContext.cpp#L372)) submits, then `vkQueueWaitIdle`.
- `VulkanRenderer::uploadBuffer` ([VulkanRenderer.cpp:267](include/render/vulkan/VulkanRenderer.cpp#L267)) allocates a staging buffer *and* a new device buffer on every call, then copies through a one-shot.
- `updateVertexBuffer` / `updateIndexBuffer` / `updateUniformBuffer` / `updateStorageBuffer` ([VulkanRenderer.cpp:2127](include/render/vulkan/VulkanRenderer.cpp#L2127), 2166, 2194, 2242) all go through `uploadBuffer` and `retireBuffer` the old one, so every update is two allocations plus a queue drain.
- `uploadImage` / `uploadImageRect` (332, 421) use the same one-shot path, and `uploadImageRect` rebuilds the full mip chain afterwards.

**What to change.** Keep the one-shot path for *creation*, which happens at load time and where a stall is fine. Give *updates* a per-frame path:

- [x] **Dynamic buffers.** On a buffer's first `update*`, switch it to `framesInFlight` persistently mapped, host-visible copies (VMA `HOST_ACCESS_SEQUENTIAL_WRITE | MAPPED`, `AUTO`). An update `memcpy`s into the copy for the current frame slot, growing it if needed (retire the old allocation through the existing `retireBuffer`). `draw` binds the current slot's copy. UI-sized data (KB to a few MB) reads fine from host-visible memory, and there's no copy command at all.
- [x] **Frame-slot ownership.** Writing slot *k* is only safe after slot *k*'s fence has signalled. Split the fence wait out of `beginFrameIfNeeded` ([VulkanRenderer.cpp:28](include/render/vulkan/VulkanRenderer.cpp#L28)) into an idempotent "wait for the current slot" step. Call it from every update and from `beginFrameIfNeeded`. Updates can arrive before `VALO::beginFrame` (load time), during the frame, or between frames.
- [x] **Texture updates.** Copy the pixels into a per-slot, persistently mapped staging ring. Record `transition → vkCmdCopyBufferToImage → transition` (and the mip blit chain, only when `mipLevels > 1`) into a **per-slot upload command buffer**. `vkCmdCopyBufferToImage` can't be recorded into the frame's main command buffer, because by then `VALO::beginFrame` → `clear` → `bindTarget` has already opened dynamic rendering on it.
  - [x] In `show`, end the upload command buffer and submit it **ahead of** the main one in the same `vkQueueSubmit2`. The barrier at its end (transfer write → fragment shader read) orders it against both the earlier frame still sampling the image and the draws after it. Queue submission order makes that a complete dependency, so no extra semaphore is needed.
  - [x] If no frame is open when a texture update arrives, either open the slot (fence wait only) or fall back to the existing one-shot. Load-time updates may stall.
- [x] Zero-byte updates must leave the buffer valid and empty, not fail. `uploadBuffer` currently returns false on `bytes == 0`.
- [x] Software backend: no change needed (updates are already `memcpy`). Re-read the "Mutation while queued" entry in `TODO.md` §3 and keep its documented semantics true on both backends: a buffer is read when the queue runs at `endFrame`.

**Done when:** a harness updates a vertex buffer, index buffer, uniform buffer, storage buffer and a texture sub-rect every frame for 1000 frames with `framesInFlight = 2`. It renders correctly, is validation-clean (including synchronization validation, `VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT`), never calls `vkQueueWaitIdle` / `vkDeviceWaitIdle` after the first frame, and allocates nothing per frame once sizes have settled. Measure the frame time before and after on `clipstack --vulkan` with per-frame vertex updates, and put the numbers in `TODO.md`.

---

## 2. Vulkan: shaders must work without VALO's repo or a toolchain at runtime

**Why UILO needs it.** UILO is a library. Apps built on it run from any working directory, on machines with no Vulkan SDK, no `glslc` and no `bin/valoc`. Today a UILO app on the Vulkan backend couldn't create a single program.

**Where it is now.** `VulkanRenderer::loadSpirv` ([VulkanRenderer.cpp:798](include/render/vulkan/VulkanRenderer.cpp#L798)):
- shells out to `bin\valoc.exe` relative to the **current working directory** (lines 742–745, 812–830)
- passes include paths hard-coded to VALO's own layout: `-I include -I examples -I ext/stb …`
- runs `glslc -I examples/shaders` from `PATH` (843)
- then reads the `.spv` from disk

The bindless prelude every generated shader includes lives at `examples/shaders/valo.glsl`, which isn't part of the library.

**What to change.** Keep `createProgram(vsPath, fsPath)` exactly as it is. It's already the one call that works on both backends: software resolves it through the shader registry (`ShaderRegistry.hpp`, which reads no files). Give Vulkan the same no-files property:

- [x] **Move `valo.glsl` into the library** (e.g. `include/render/vulkan/valo.glsl`). Point valoc's output and every `glslc` invocation at that location. Keep the examples working.
- [x] **An embedded SPIR-V registry**, the twin of `shaderRegistry()`: `registerSpirv(const char* path, const uint32_t* words, size_t count)` plus a registration macro. Look it up with the same suffix match `findShader` uses.
- [x] **`createProgramFromFiles` on Vulkan checks the embedded registry first**, and only falls back to disk and runtime translation when a path isn't embedded.
- [x] **A CMake helper**, e.g. `valo_embed_shaders(<target> SHADERS a.vert.hpp a.frag.hpp …)`. At build time it runs `glslc` (from the vendored SDK, `${VALO_VULKAN_ROOT}`) on the committed `.vert`/`.frag` GLSL beside each `.hpp`, turns each `.spv` into a generated `.cpp` holding a `uint32_t` array plus registration, and adds those sources to `<target>`.
  - It runs valoc first only when valoc was built and the GLSL is stale, using the same `needsTranslation` rules. The GLSL is committed, as VALO already does, so consumers don't need libclang.
  - It must work when VALO is a subproject (see item 4).
- [x] **Make the runtime fallback independent of the working directory.** Bake `VALO_SOURCE_DIR` (or similar) in with a compile definition and build the valoc path, include paths and `glslc` path from it, instead of `bin/…`, `include`, `examples/…`. The fallback is a developer convenience; keep it, but it shouldn't depend on where the app was launched from.
- [x] Convert at least one example (`clipstack` is the closest to UILO) to embedded shaders, so the path is exercised.

**Done when:** `clipstack --vulkan`, built with embedded shaders, runs correctly when launched from an unrelated directory with `glslc` and `bin/valoc` both removed from reach, and prints no `[vulkan] translating` / `compiling` lines. The software build of the same example is unchanged.

---

## 3. Window: the input and platform surface a UI toolkit needs

**Why UILO needs it.** `valo::Window` replaces SDL for UILO completely; UILO's public API must not expose SDL. `pollEvents()` drains the SDL queue itself and keeps only per-frame bitsets, so UILO can't get anything that isn't exposed here. Textbox and Terminal can't take typed text, holding Backspace does nothing after the first press, and copy/paste, cursor shapes and live-resize redraw are all missing.

**Where it is now.** [Window.hpp](include/render/Window.hpp) / [Window.cpp](include/render/Window.cpp). `pollEvents()` (Window.cpp ~161) ignores `e.key.repeat`, text input, keycodes, event order, timestamps and press positions.

**What to add.** Additive only: the existing bitset queries stay exactly as they are. Names below are suggestions; fit them to VALO's style. Everything here is a thin SDL wrapper, so both backends get it for free.

- [x] **An ordered per-frame event list**, filled by `pollEvents()` and cleared at its start: `const std::vector<WindowEvent>& events() const`. Each event has:
  - a type: key down, key up, text, mouse down, mouse up, mouse move, wheel, focus gained/lost, close requested
  - `Key key` (scancode, as now), `bool repeat`
  - `char32_t symbol`: the layout-mapped character for that key (`SDL_GetKeyFromScancode` with the current modifiers), 0 for non-character keys. This is what lets UILO bind Ctrl+Z / C / V / A by *letter* on AZERTY and Dvorak.
  - modifiers (see below), the mouse position in pixels at the time of the event, the mouse button, the wheel delta as `Vec2f` plus whether it's precise (`SDL_MouseWheelEvent::integer_x/y` vs float), and a nanosecond timestamp
  - for text events, the UTF-8 text (a small fixed buffer is fine; SDL's is 32 bytes)
- [x] **Text input control:** `startTextInput()`, `stopTextInput()`, `isTextInputActive()`. UILO toggles these as focus moves between text fields.
- [x] **Modifiers:** a `KeyMod` bit-flag enum (Shift, Ctrl, Alt, Gui, plus L/R if cheap) and `KeyMod modState() const`. These could be derived from `isKeyDown`, but the event needs them anyway.
- [x] **Clipboard:** `std::string getClipboardText() const`, `void setClipboardText(const std::string&)`.
- [x] **System cursors:** an enum covering at least Arrow, Hand/Pointer, Text/IBeam, ResizeEW, ResizeNS, Crosshair, and `setCursor(CursorShape)`. Cache the `SDL_Cursor*`s and free them in `cleanup()`.
- [x] **Live-resize redraw:** `setResizeCallback(std::function<void()>)`, invoked from an `SDL_AddEventWatch` on `WINDOW_RESIZED` / `WINDOW_PIXEL_SIZE_CHANGED` / `WINDOW_EXPOSED`. Windows and macOS block the main loop inside a modal resize loop, and this is how the app keeps drawing during it. Remove the watch in `cleanup()`.
- [x] **Native handle:** `void* nativeHandle() const`, returning HWND on Windows, `NSWindow*` on macOS, the X11 `Window` (cast) or `wl_surface*` on Linux, read from `SDL_GetWindowProperties`. UILO's macOS trackpad scroll/zoom monitors and live-resize configuration need the `NSWindow`.
- [x] **Display and system queries:**
  - `float getDisplayScale() const` (`SDL_GetWindowDisplayScale`, the UI content scale, which is not the same thing as `getPixelDensity`)
  - `float getRefreshRate() const`
  - `static SystemTheme getSystemTheme()` (Light / Dark / Unknown)
  - `static std::string basePath()` and `static std::string prefPath(org, app)` (free SDL's allocation)
- [x] Keep `wasKeyPressed` ignoring repeats (that's what games want). The event list is where repeats show up.

**Done when:** a small example (or `clipstack` with a debug overlay) shows:
- typed text including non-ASCII (é, ü, CJK through an IME if available)
- held Backspace repeating
- Ctrl+Z reported with `symbol == 'z'` under a non-QWERTY layout
- Shift+wheel and trackpad horizontal scroll
- copy/paste round-tripping through the OS clipboard
- the cursor changing over regions
- the window continuing to redraw during a drag-resize on Windows

---

## 4. CMake: usable as a subproject

**Why UILO needs it.** UILO will `add_subdirectory(ext/VALO)`. Today that resolves every VALO path against UILO's root.

**Where it is now.** [CMakeLists.txt](CMakeLists.txt): 19 uses of `${CMAKE_SOURCE_DIR}`. SDL3 always comes from `ext/SDL3-install` through `find_package`, library outputs are forced into the source tree's `lib/`, and examples are on by default.

**What to change.**
- [x] `${CMAKE_SOURCE_DIR}` → `${CMAKE_CURRENT_SOURCE_DIR}` (or `${PROJECT_SOURCE_DIR}`) everywhere, including the valoc block.
- [x] Default `VALO_BUILD_EXAMPLES` (and building valoc) to `PROJECT_IS_TOP_LEVEL`.
- [x] Only force `ARCHIVE/LIBRARY/RUNTIME_OUTPUT_DIRECTORY` into `lib/` when top-level.
- [x] SDL3: skip `find_package` when the parent already defines `SDL3::SDL3-static` (`if(NOT TARGET SDL3::SDL3-static)`), so there's exactly one SDL in the process.
- [x] MSVC: UILO builds with MSVC (Visual Studio 2022), not just MinGW. VALO must compile and link warning-clean there. Don't hard-code a runtime library; leave `CMAKE_MSVC_RUNTIME_LIBRARY` to the parent so both use the same `/MT` or `/MD`. Check the Vulkan loader lookup on MSVC too (it needs `vulkan-1.lib`, not the `.dll` fallback MinGW uses).
- [x] Document in a comment that consumers must **not** define `STB_IMAGE_IMPLEMENTATION` themselves; VALO's `software/RenderTarget.cpp` already does.

**Done when:** a scratch top-level project that does `add_subdirectory(path/to/VALO)` and links `VALO::VALO` configures and builds with MSVC and with the existing toolchain, from a build directory outside VALO's tree, with examples off by default.

---

## 5. Blend: the same alpha on both backends

**Why UILO needs it.** UILO may render subtrees into targets and composite them later (the `Renderer` framebuffer API it already exposes). Today the two backends write different destination alpha for the same draw, so that output would differ between backends.

**Where it is now.**
- Software `writePixel`, `BlendMode::Alpha` ([DrawFuncs.cpp:179](include/render/software/DrawFuncs.cpp#L179) and ~208, the sRGB and plain paths): forces `out.w = 1`.
- Vulkan ([VulkanRenderer.cpp:1006](include/render/vulkan/VulkanRenderer.cpp#L1006)): `srcAlphaBlendFactor = ONE, dstAlphaBlendFactor = ZERO`, so it writes the source alpha.
- `Additive` has the same mismatch.

**What to change.** Use standard "over" alpha on both:
- [x] **Alpha:** `outA = srcA + dstA·(1 − srcA)`. On Vulkan that's `srcAlpha ONE, dstAlpha ONE_MINUS_SRC_ALPHA`.
- [x] **Additive:** `outA = dstA`. On Vulkan that's `srcAlpha ZERO, dstAlpha ONE`.
- [x] Software present: set the streaming texture to `SDL_BLENDMODE_NONE` in [SoftwareRenderer.cpp](include/render/software/SoftwareRenderer.cpp) (`SDL_CreateTexture` ~line 11), so a backbuffer alpha below 1 can never show through on present.
- [x] Nothing visible changes for an opaque-cleared backbuffer (its alpha stays 1). Check that every example looks the same as before.

**Done when:** the readback test in the harness (software target pixels; Vulkan where readback exists) gives identical alpha for Alpha and Additive draws over a half-transparent target, and every example is visually unchanged.

---

## 6. Vulkan: stencil reference as dynamic state

**Why UILO needs it.** Every clip UILO pushes is a stencil draw at the current depth, and every draw inside it tests against that depth. With the reference baked into the pipeline, each new nesting depth compiles new pipelines the first time it's used, which shows up as a hitch when a UI opens a deeper panel. It also multiplies the pipeline cache by the maximum depth.

**Where it is now.** `packPipelineState` ([VulkanRenderer.hpp:54](include/render/vulkan/VulkanRenderer.hpp#L54)) packs `stencilRef` into the key, `createPipeline` sets `.reference = cmd.stencilRef` (~983), and the dynamic states are only `VIEWPORT, SCISSOR` (1022).

**What to change.**
- [x] Add `VK_DYNAMIC_STATE_STENCIL_REFERENCE` (core 1.0).
- [x] Remove `stencilRef` from `packPipelineState`.
- [x] In `draw`, call `vkCmdSetStencilReference(buf, VK_STENCIL_FACE_FRONT_AND_BACK, cmd.stencilRef)` whenever the pipeline uses stencil.

**Done when:** `clipstack --vulkan` with nesting pushed to depth 20 creates the same number of pipelines as at depth 3, and `stencil --vulkan` still matches software.

---

## Not blocking (don't do these unless the above are finished)

- **R8 / RGBA32F texture formats** (`TODO.md` §1). UILO will store glyph atlases as RGBA8 for now.
- **`VALO::submit` stamps the clip test onto every layer**, whatever target the layer draws to. UILO uses one layer, so it's harmless for now.
- **`#define discard` is global** (`software/Sampling.hpp:92`). UILO only includes VALO's render headers in its own .cpp files.
- **`mips` defaults to true** on `createTexture`. UILO will pass `false` for anything it updates. Don't change the default.
- **A conformance diff harness** (`TODO.md` §4). Very valuable for UILO later (a UI shows 1px differences), but it doesn't block integration.
- **Per-draw redundant binds on Vulkan.** Pipeline, push descriptors and the bindless set are rebound on every draw, and `pipelineFor` is a linear search. UILO draws a handful of commands per frame, so this doesn't matter yet.
