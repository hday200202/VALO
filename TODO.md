# VALO TODO

Originally ordered by what it takes to bring the software renderer to parity
with the eventual Vulkan backend's dev-facing API. **That backend now exists and
is at state parity**, so sections 1-4 are largely a record of how it got there;
the open work is the handful of unchecked items in section 1 (parser and format
gaps that never gated the backend), the Vulkan known-gaps list at the end of
section 4, and the never-blocking sections 5 and 6.

## State (2026-10-05)

~9.1k lines of library across 40 files (3.4k of that Vulkan), 14 examples,
every feature behind a sanitizer-verified harness. The dev-facing API (Window ->
VALO -> create* -> RenderCommand -> submit, typed C++ shader callbacks) is
settled and Vulkan-shaped.

**The Vulkan backend is at state parity with the software backend.** `Renderer`
dispatches every public method through a 30-entry function-pointer table, both
backends fill all 30, and **no entry is a `todo()` stub**. Buffers, textures,
render targets, uniform buffers, depth and stencil all work;
`minimal.exe --vulkan` renders `sea_keep_lonely_watcher` textured through the
same `Engine`, camera and `RenderCommand` state as the software path.

**A C++ -> GLSL translator (`valoc`) exists** and converts **36 of the 38**
shader files in `examples/shaders/` into GLSL that `glslc` accepts. The two
holdouts are `gltf.frag` and `pbr.frag`, both for the same reason: a `uint`
field used directly as a boolean (`if (!u.detail)`, `x && y`), which C++ allows
and GLSL does not.

Examples: `main` (instanced cubes), `particles`, `sdf`, `clipstack`, `stencil`,
`pbr`, `gltf`, `minimal`, `showcase`, `rendertarget`, plus the four Vulkan
bring-up harnesses `vk-clear`, `vk-triangle`, `vk-cubes` and `vk-test`. Every
non-Vulkan example takes `--vulkan` and runs the same scene on either backend.

Landed 2026-10-05:

- **Stencil** - `VkStencilOpState` front and back, with `stencilTest`,
  `stencilOp` and `stencilRef` added to `packPipelineState`. VALO has one
  `StencilOp` where Vulkan has three, and `stencilApply()` in `DrawFuncs.cpp`
  only runs once a fragment has survived both tests - so it is `passOp`, and
  `failOp`/`depthFailOp` are both `KEEP`. `Incr`/`Decr` map to the `_AND_WRAP`
  variants, matching the `uint8_t` arithmetic the software path does; the
  `_CLAMP` variants would differ at 0 and 255, which is exactly where a
  two-pass outline lands.

  Three pieces of plumbing went with it: `stencilAttachmentFormat` on the
  pipeline, `pStencilAttachment` on the `VkRenderingInfo`, and widening the
  depth image views from `DEPTH` to `DEPTH | STENCIL` so one view can serve
  both attachments. `stencilTestEnable` stays off for an `Always`/`Keep`
  command, so ordinary draws neither pay for it nor split the pipeline cache.

Landed 2026-10-02/05:

- **Render targets**, via `bindTarget` and `m_currentTarget`. `clear` and
  `draw` honour their `target` argument, and a dynamic-rendering block is
  opened per distinct target rather than per draw.
- **`updateTexture` / `updateTextureRect`**, as staged copies into an existing
  image with the layout round trip either side.
- **The `valid*` predicates are backend-agnostic**, keyed on
  `slots(ResourceKind::...)` rather than reaching into `m_software`. They used
  to begin with `m_software &&` and so returned false for every ID on the
  Vulkan backend, which quietly disabled ID validation there.
- **Scalar block layout** (`VK_EXT_scalar_block_layout`), so a GLSL block
  mirrors a C++ struct field for field with no padding arithmetic. This is what
  `PLAN.md` phase 3 called the uniform-padding decision, and it is now made.

### Textures: bindless, with separate samplers (2026-10-02)

**Textures and render targets are one type.** `SoftwareRenderer::createTexture`
returns `pushTarget(...)`, `updateTexture` asserts `validTarget`, and
`TextureID`/`RenderTargetID` are both `uint32_t` indexing `m_targets`. The
Vulkan backend mirrors that: one `GPUTexture` vector, one free list
(`m_freeTargets`), and a render target is just one that also carries
`COLOR_ATTACHMENT_BIT`. Finding this *before* writing either is what stopped
them being written twice.

**Descriptor indexing, not per-draw bindings.** `main.cpp` and
`MinimalEngine` both put a `TextureID` *inside* the uniform struct and let the
shader do `texture(u.tex, uv)`. Conventional Vulkan cannot express that - a
`sampler2D` is a bound slot, not a value - so matching it needs one unsized
array with `TextureID` as the index. The alternative would have grown a
texture-binding API on `RenderCommand` that only one backend used.

**Separate `texture2D` and `sampler`, not `COMBINED_IMAGE_SAMPLER`**, because
`Sampling.hpp` passes a `Sampler` at *call* time. Binding 0 is an unsized
sampled-image array (`MAX_TEXTURES = 4096`); binding 1 is the 18 fixed
`Wrap x Wrap x Filter` combinations, written once at init. The shader recombines
them: `texture(sampler2D(uTextures[nonuniformEXT(id)], uSamplers[s]), uv)`.

Set 0 is push descriptors and set 1 is the bindless set - opposite mechanisms
for opposite lifetimes. Set 0 changes per draw and has no persistent state, so
inline writes win; set 1 is allocated once and lives for the renderer, so a pool
costs nothing.

Three binding flags, each load-bearing: `PARTIALLY_BOUND` (unwritten slots are
legal, so a 4096-entry array can start empty), `UPDATE_AFTER_BIND` (write a
descriptor while recorded command buffers still reference the set) and
`UPDATE_UNUSED_WHILE_PENDING` (which makes `createTexture` mid-frame safe rather
than merely legal). `UPDATE_AFTER_BIND_POOL` has to be on **both** the layout
and the pool; missing it on one fails at allocation and blames the pool.

Traps worth keeping written down:

- `nonuniformEXT` is required. Without it the compiler may assume a whole
  wavefront samples the same texture, and when different primitives do not, you
  get the wrong texture rather than an error. It often still works on AMD, which
  is how it survives into shipped code.
- `maxLod` defaults to `0.0`, clamping every sample to mip 0. Mipmaps upload
  correctly and are never used, which reads as a generation bug.
- the image **view** needs `levelCount = mipLevels`, for the same reason.
- the mip level count loop must stop when *both* dimensions reach zero. Using
  only the larger one gives a 1024x16 texture 5 levels instead of 11.
- pixel order was checked, not assumed: `Srgb.hpp:78` packs
  `(a << 24) | (b << 16) | (g << 8) | r`, so memory order is R,G,B,A - exactly
  `VK_FORMAT_R8G8B8A8_*`, no swizzle.

Mip generation is a `vkCmdBlitImage` chain, each level moving to
`SHADER_READ_ONLY_OPTIMAL` as soon as the next has been blitted from it, so only
the last level transitions after the loop.

`loadSpirv` passes `-I examples/shaders` to `glslc`, so shaders
`#include "valo.glsl"` for the bindless declarations and the sampler constants
rather than copying them. The include path being relative to the working
directory is crude for a library - it eventually belongs in `RenderConfig` or
beside the executable.

### The clip-space Y flip (2026-10-01)

**VALO's clip space is Y-up, and Vulkan's is Y-down.** The software backend
flips at the viewport transform (`DrawFuncs.cpp:79`, `(1 - ndc.y)`) and
`Mat4f::perspective` does *not* negate Y, so the convention is OpenGL's.
Vulkan's framebuffer Y points down and would not flip, which rendered
**vertically mirrored** and **reversed the sign of the signed area** - so the
same mesh culled the opposite way on each backend.

Fixed with a negative-height viewport in `draw` (`y += height, height = -height`;
`VK_KHR_maintenance1`, core since 1.1), *not* by flipping `frontFace`. Flipping
`frontFace` would have corrected the culling and left the image upside down,
hiding the real bug. One matrix now means one thing on both backends.

Two things worth remembering about how this was found:

- **The triangle could not have caught it.** With no projection matrix there is
  no Y-up clip space to mis-transform, so both backends agreed. It took closed
  geometry under perspective (`vk-cubes` rendering inside-out) to surface it.
- **The mirroring hid a second bug.** A vertically mirrored 3D scene is the same
  as viewing it from below, and orbiting from below reverses the apparent
  horizontal direction - so the mirror was cancelling a sign error in the orbit
  camera. `main.cpp` still has `yaw += d.x` and is inverted in software for the
  same reason; `vk-cubes` uses `-=`.

**The conventions, confirmed against the matrices rather than assumed:**
right-handed, **+X right, +Y up, -Z forward** (OpenGL / glTF). `perspective`
puts `-1` in the z column's w row, so `clip.w = -z` and only negative view-space
z is in front. Depth is reversed - near maps to 1, far to 0 - which is what makes
`DepthCompare::Greater` with clear depth 0 correct.

Verified state: `vk-test` covers context, swapchain, programs, uniform buffers
and the pipeline state tables; `vk-triangle` and `vk-clear` run clean with zero
validation output; VMA reports all allocations freed on close.

Landed 2026-09-26/27, in order: uniform buffers (slot-bound, `MAX_UNIFORM_SLOTS
= 8`); sRGB (converted at sample, not at load); the top-left fill rule;
`Renderer`/`VALO` function reorganisation; batched band dispatch (83.2 -> 30.9 ms
on the trees scene, per-draw overhead 24 us -> zero); reversed-Z as the sole
depth convention (smallest resolvable separation at 400 units 1.33 -> 0.000069);
wireframe 267 -> 72 ms; mipmaps with analytic per-triangle LOD (shimmer -70% near
to -100% far, and 0.9 ms *faster*); `examples/engines/`; hex `setSkyColor`;
viewport + scissor rects; and the three robustness items in section 2.

Benchmark (DamagedHelmet, 1280x960, lambert, M4): VALO 8.5 ms, SwiftShader
3.0 ms, MoltenVK GPU 0.7 ms. The 2.85x gap to SwiftShader is its LLVM-JIT'd
4-wide fragment shading; not something the Vulkan backend inherits.

---

## 1. API parity — things the Vulkan backend would force into the API

These are the gaps that would show up the moment the backend is written. Doing
them now means the API doesn't move later. Maybe a day of Harrison-hours.

- [✅] **GLSL built-ins** — shaders now write `valo_Position` / `valo_FragColor`
      (done, mirrors `gl_Position` / fragment `out`). Same thread-local pattern
      for the rest:
      - `valo_VertexIndex` (`gl_VertexIndex`) — set from the index value in the
        geometry loop. Trivial.
      - `valo_FragCoord` (`gl_FragCoord`) — `{x + 0.5, y + 0.5, z, 1/w}` set per
        pixel before `fsThunk`. Most-used after FragColor.
      - `valo_FrontFacing` (`gl_FrontFacing`) — `front` is already computed in
        `setupTriangle`; stash on `ScreenTri`, set before the shader. Makes the
        winding trap debuggable (color back faces red).
      - `valo_FragDepth` (`gl_FragDepth`) — read after the shader; if written,
        use it instead of interpolated z. Lets `sdf.cpp` depth-test against
        meshes.
      - `valo_PointCoord` (`gl_PointCoord`) — with AA points, later.
      - `dFdx` / `dFdy` / `fwidth` — **deferred, probably not for the software
        backend** (decided 2026-09-25). The general form needs 2x2 quad
        shading: shade fragments in blocks of four (including helper
        invocations outside the triangle) so a value computed *inside* the
        shader can be differenced against its neighbour. That changes how
        every pixel is shaded, and the payoff does not justify it here,
        because a renderer that owns its whole pipeline already knows the
        scale factor that `fwidth` exists to recover:
          - 2D pixel-space ortho (`clipstack.cpp`): `d` is already in screen
            pixels, so `fwidth(d) == 1`. `smoothstep(-1, 1, d)` antialiases
            those rounded rects today, no derivatives at all.
          - Raymarched SDFs (`sdf.cpp`): footprint is `t * pixelAngle` from
            FOV and resolution - ray differentials / cone tracing. Exact,
            rather than quantised to a 2x2 quad.
          - Procedural bump (`pbr.cpp`): keep the analytic finite difference,
            scale `eps` by a passed-in footprint instead of a constant.
        Revisit only if the 4-wide fragment shading in section 6 gets done for
        performance - the quad restructure is the same work, so derivatives
        would come along nearly free at that point.
      - **Analytic varying derivatives** — still worth doing, and independent
        of the above. For interpolated values the rasterizer already knows the
        gradient: the per-pixel edge steps `d1`/`d2`/`d3` in `rasterizeTriangle`
        *are* the barycentric gradients. With `A = dnum/dx` and `B = dinvW/dx`
        precomputed per triangle, `dinterp/dx = (A - interp * B) / invW` is one
        multiply-subtract-divide per varying. No shading contract change, and
        more accurate than quad differencing (exact at triangle edges). This is
        what mipmap LOD needs; do it with mipmaps.
- [✅] **`updateTexture(id, data)`** — the only missing dynamic path. UILO font
      atlases and streamed textures need it. Software: memcpy; Vulkan: staging
      copy. ~1 hour.
- [✅] **sRGB** — a `srgb` flag on `RenderTarget` (so it covers textures and
      targets alike, which are the same type here), `createTexture(..., srgb)`,
      `createTarget(..., srgb)` and `RenderConfig::srgbBackbuffer`. All default
      false, so nothing that existed before changed.

      Converted **on sample, not on load**: 8 bits of *linear* spends most of
      its codes above mid-grey and bands visibly in the darks, which is exactly
      why Vulkan `_SRGB` formats keep the bytes encoded and convert in the
      sampler. Tables live in `render/common/Srgb.hpp` (`inline`, header-only
      so `writePixel` can inline them): 256-entry decode, exact; 4097-entry
      encode (12 bits + 1 so linear 1.0 stays in bounds), ≤1 code of error in
      the darkest few values. Alpha is never converted.

      Three places had to change, and each is a real bug if skipped:
      `texture()` decodes the four bilinear taps *before* filtering (averaging
      encoded bytes averages the wrong quantity - a black/white checker filters
      to 0.21 instead of 0.5); `writePixel` decodes the destination, blends in
      linear and re-encodes (the classic muddy-alpha bug); `clearRows` encodes
      the clear colour.

      Harness `srgbtest` (scratchpad): byte-exact decode→encode round trip,
      sampling, linear-space bilinear, linear-space alpha blend, clear, and
      that a non-sRGB target is bit-identical to before. Clean under ASan/UBSan
      and TSan.

      Not done, deliberately: the examples still do their own gamma
      (`encodeGamma` / `toneMap`), so flipping `srgbBackbuffer` on them would
      double-correct. Converting them is its own change.
- [✅] **Mipmaps** (2026-09-27) — box-filtered chain built in
      `RenderTarget::generateMips`, called from `createTexture(..., srgb, mips)`
      (default on; render targets opt out). sRGB textures average in linear
      space, same reason bilinear does.

      LOD is analytic per triangle, not quad-based: `setupTriangle` stores the
      barycentric gradients (`dBdx`/`dBdy`, `dDenDx`/`dDenDy`) and
      `rasterizeTriangle` publishes them plus the pixel's interpolated varyings
      through `detail::varyingDerivs`. `texture()` infers which varying holds
      the uv from the address of its `const Vec2f&` argument, so **no API
      change and no shader change** - which matters because this scaffolding is
      software-only; Vulkan gets derivatives from hardware. A computed uv falls
      outside the varying array, is detected, and falls back to level 0.

      Measured (`miptest`, sub-pixel camera nudge on `trees/scene.gltf`):

      | cam dist | no mips | mips | reduction |
      |---|---|---|---|
      | 0.2x | 45890 | 13776 | 70% |
      | 1.5x | 751 | 40 | 95% |
      | 6.0x | 19 | 0 | 100% |

      The flat-shaded floor at 1.5x is 5 flips, so 40 is close to pure geometry.
      Frame time went 30.9 -> 29.8 ms: smaller levels sample better from cache.

      Still nearest-mip, not trilinear - a hard switch between levels can show
      as a visible band on a slowly receding surface. Blending the two adjacent
      levels is the follow-up.
- [✅] **Uniform buffer resource** — `createUniformBuffer` / `updateUniformBuffer`
      / `destroyUniformBuffer` on VALO and Renderer, `cmd.bindUniformBuffer(slot, id)`
      with `MAX_UNIFORM_SLOTS = 8`, and `uniformBuffer<T>(slot)` in the shader.
      Slot binding rather than a handle in the uniforms, because a GLSL uniform
      block's type *is* its layout, so handle-indexed UBOs would need one
      descriptor array per block type plus `shaderUniformBufferArrayNonUniform-
      Indexing`, which is optional even in Vulkan 1.2. Slots map 1:1 onto
      descriptor set bindings on any 1.0 device.

      `Renderer::draw` resolves the eight slots to pointers once per draw into
      a stack array and hands it down through `DrawContext` (which replaced the
      bare `targets` parameter on `forEachBand` and the three `draw*` funcs, so
      viewport/scissor/formats can ride along later without moving signatures
      again). `detail::boundUniforms` is set on the main thread for the vertex
      stage and per band for the fragment stage, the same two-place pattern
      `boundTargets` uses.

      Semantics worth remembering: `setUniforms` copies at submit, a uniform
      buffer is read when the queue executes at `endFrame` — so a UBO holds
      frame-constant data only, and updating one between two submits gives both
      draws the later value. Same as Vulkan, which is why push constants exist.

      Harness `ubotest` (scratchpad): vertex + fragment reads, visibility of
      `updateUniformBuffer`, every worker band bound, per-command binding,
      free-list reuse. Clean under ASan/UBSan and TSan.

      Landed in the examples: `ShowcaseEngine.hpp` and `pbr.cpp` moved lights
      (plus camPos/exposure) into a `SceneUniforms` buffer at slot 0. Showcase
      per-command uniforms: Uniforms 324→196 B, Floor 224→96, Orb 112→96,
      Sky 48→44; the 128 B scene block is now uploaded once a frame instead of
      memcpy'd into every command.
- [✅] **Viewport rect** on `RenderCommand` — draw into a sub-rect of the target.
      Vulkan dynamic viewport.
      `Viewport { x, y, width, height, minDepth, maxDepth }`; width/height of 0
      means the whole target, so every existing call is unaffected. Applied in
      the NDC→screen map of all three paths (`setupTriangle`, `rasterizePoint`,
      `rasterizeLine`), with `minDepth`/`maxDepth` remapping z.
- [✅] **Scissor rect** on `RenderCommand` — axis-aligned fast path (bbox clamp
      for triangles, pixel test for lines/points). Vulkan dynamic scissor. The
      stencil clip stack stays the general mechanism.
      `Scissor { x, y, width, height }`, zero extent = disabled. Both rects
      resolve through one helper, `resolveClipRect()`, which returns the
      inclusive `target ∩ viewport ∩ scissor` pixel box; triangles clamp their
      bbox to it, points test against it, and lines fold it into the band
      bounds so the DDA skips the scissored span instead of stepping it.
      Verified by `viewtest` (10 cases: viewport quadrants, overhang, scissor
      box, viewport∩scissor, disjoint → zero pixels, depth remap, wireframe,
      points), ASan + UBSan clean. `cliptest`/`filltest`/`covertest`/`miptest`
      unchanged.
      Not done: `clear()` ignores the scissor — it is a `Renderer` call, not a
      `RenderCommand`, so a scissored clear needs its own entry point.
- [✅] **glTF embedded images (`.glb`)** - `GLTF::images` holds a file path or
      the encoded PNG/JPEG bytes (from a `.glb` buffer view or a base64 `data:`
      URI), and material slots are `int` indices into it. URIs are
      percent-decoded. Verified by packing DamagedHelmet both ways: all three
      forms load the same 5 textures on both backends. `loadGLTFTexture()` is
      the public way to turn an index into a texture.
- [✅] **glTF alpha modes** - the parser carries `alphaMode`, `alphaCutoff`
      and `doubleSided`; `loadModel` sets `CullMode::None` for double-sided,
      `BlendMode::Alpha` for BLEND, and orders blended draws after opaque ones.
      MASK is the shader's job (`gltf.frag` discards below the cutoff).

      **Deliberately not done: depth write off for BLEND.** That is only right
      with a back-to-front sort, which VALO lacks, and exporters (Sketchfab
      especially) stamp BLEND on surfaces that are really opaque. Turning it off
      made the sea-keep island's rocks stop occluding the water - measured, not
      guessed. Revisit with the per-command sort key in section 5.

- [✅] **Tangents in `GLTFVertex`** - `{pos, normal, uv, tangent}`. The file's
      own tangents when it ships them (mech, trees, raptor, bathroom do), else
      computed from the UVs; missing normals are computed too, and non-indexed
      primitives get sequential indices instead of being dropped. The two
      copy-pasted generators in `gltf.cpp` and `ShowcaseEngine` are gone.
- [✅] **Storage buffers** - landed 2026-10-05 (`storage.cpp`, and the BVH
      raytracer in `showcase` is built on them). Three Vulkan bugs fixed
      2026-10-08, none visible without validation on:
      - uniforms and storage were two push-descriptor sets in one pipeline
        layout (VUID-VkPipelineLayoutCreateInfo-pSetLayouts-00293). Storage now
        lives in set 0 at binding `MAX_UNIFORM_SLOTS + slot`.
      - `.size()` read 0 on MoltenVK, so `trace.frag` bailed on an empty node
        list and **every traced shadow ray missed on Vulkan**. Each storage
        buffer now carries a 16-byte size header that `.size()` reads; GLSL's
        `.length()` is no longer used.
      - `storageBuffer<T>(NAMED_CONSTANT)` silently bound slot 0.

- [ ] **Texture formats** — at least R8 (font atlases, masks) and RGBA32F
      (HDR targets). `createTexture` gains a format; `texture()` unpacks per
      format. Vulkan has these for free; software needs them to exist so the
      API doesn't change. ~2 hours.

## Usability audit (2026-10-08)

Things that worked only if you already knew the trick. All fixed unless marked.

- [✅] **glTF needed a hand-written loader** - `loadModel` / `submitModel` /
      `destroyModel` in `utils/Model.hpp`, and `ModelMaterial` is laid out to
      drop straight into a shader's Uniforms. `gltf.cpp` and `MinimalEngine`
      use it; `ShowcaseEngine` and `stencil.cpp` stay on `parseGLTF` because
      they need the CPU-side triangles.
- [✅] **valoc translated these to GLSL that compiled and was wrong** - now each
      is either handled or refused with the line: `bool` in any GPU-read
      struct, integer varyings, `auto x = ...` (was deleted outright), enum
      constants (came out as bare names), unresolvable buffer slots, one
      uniform type bound to two slots.
- [✅] **Editing a shader did nothing on Vulkan** once it had been translated.
      valoc records its inputs as `// valoc-dep:` lines; a stale translation,
      or one older than valoc itself, is redone. Hand-written GLSL (no valoc
      marker) is never touched. Works for shaders outside the repo.
- [✅] **Mismatched shader halves** - `createProgram(path, path)` refuses a
      pair whose Uniforms or Varyings differ; every `static_assert` has a real
      message.
- [✅] **Oversized Uniforms truncated silently on Vulkan** - the draw is now
      skipped with one message per program (Vulkan only guarantees 128 bytes).
- [✅] **Uniforms vs uniform buffer vs storage buffer** - documented once,
      above `setUniforms` in RenderCommand.hpp; the shader contract is at the
      top of ShaderRegistry.hpp.
- [✅] **Smaller fixes found on the way** - VALO did not compile in Debug
      (`m_owner.validStorage` in an assert); `parseGLTF` leaked every load;
      software handed out a 0x0 texture ID for a missing image file.
- [✅] **`k`-prefixed constants** - all 51 renamed to SCREAMING_SNAKE_CASE
      (`PI`, `SCENE_SLOT`, `ENCODE_BITS`, ...). `kD` in the PBR shaders stays:
      it is the diffuse term k_d, a variable, not a prefixed constant.

## 2. Cleanup — housekeeping, never blocked the backend

- [ ] **Shared pixel path** — the three rasterizers duplicate the per-pixel
      sequence (depth test -> stencil test -> shade -> discard -> depth write ->
      stencil op -> blend). One `shadePixel()` helper so sRGB / MRT / formats
      land in one place. DrawFuncs is 146 (.hpp) + 514 (.cpp); add a stage-map
      comment at the top. The one section-2 item still open with real work in
      it.
- [ ] **Logging hook** — 9 `std::cout` / `std::cerr` sites across Renderer.cpp,
      RenderTarget.cpp, GLTFParser.hpp, OBJParser.hpp, Timestamp.hpp; one
      `valo::log(level, msg)` the dev can redirect.
- [ ] **Quat.hpp** is empty (0 lines) — needed for glTF animation / camera code.
- [✅] **Renderer.hpp -> Renderer.cpp** — done; 0 inline definitions left in the
      header. `DrawFuncs` got the same treatment.
- [✅] **Sentinel macros -> constexpr** — already `inline constexpr uint32_t` in
      RenderCommand.hpp; `EMPTY_TEXTURE` joined them.
- [✅] **Uniform limit** raised to 512 B (was 256). Note: an earlier revision of
      this file claimed 1024 — the code has only ever been 256 -> 512 (64e8cb5).
      A `me::Uniforms` with two Mat4f + material + light is ~232 B, so 512 is
      not roomy; this is what the uniform-buffer item in section 1 is for.

### Rasterizer correctness

- [~] **Fill rule — landed, but float precision caps it.** Two triangles
      sharing an edge both pass the edge test on the pixels along it, so every
      such pixel runs the fragment shader twice and, worse, *blends* twice.
      Reproduced by `filltest` (scratchpad): one alpha-blended quad (two
      triangles, shared diagonal) over white on a 4x4 target gives
      `127 127 127 63 / 127 127 63 127 / ...` - the four diagonal pixels
      composited twice and came out half as bright. Found while testing sRGB;
      it is not sRGB-specific and reproduces on a plain target.

      Visible today as a seam along every quad diagonal in anything alpha or
      additively blended - `clipstack.cpp`'s panels, the showcase floor, the
      light orbs. Opaque draws only pay the wasted shading.

      **Done (2026-09-26):** `ScreenTri::topLeft[3]` classified in
      `setupTriangle` from each edge's winding-normalised delta (left edge when
      delta.y < 0, top edge when delta.y == 0 && delta.x > 0), and
      `rasterizeTriangle` tests `e > 0 || (e == 0 && topLeft[i])`.

      Results (`covertest`, scratchpad): a fullscreen quad went from 64 doubled
      pixels to **0**. A 16-triangle fan went from 12 to 6 - halved, not fixed.

      **Why it stops there, and what the real fix is.** The rule needs `e == 0`
      exactly on a shared edge. With float edge functions that essentially
      never happens off axis: `edgeprobe` walks a fan's shared edge and finds 0
      exact zeros in 39 samples, `e_fwd != -e_rev` at all 39, and 5-6 points
      where *both* triangles compute a small positive value and both claim the
      pixel. Incremental stepping adds its own drift (2.4e-4 across 64 px), but
      it is not the root cause - evaluating directly does not agree either,
      because the two triangles build the edge from different vertices.

      The complete fix is what GPUs do: snap screen positions to a sub-pixel
      grid and compute edge functions in **fixed point** (24.8 is the usual
      choice). Then edge values are exact integers, `e_fwd == -e_rev` holds
      exactly, ties land exactly on zero, and the top-left rule becomes exact.
      Barycentrics come from the same integer edge values. That is a real
      rewrite of `setupTriangle` + `rasterizeTriangle`, half a day, and it is
      also the prerequisite for any future multisampling.

      Worth doing eventually; not urgent, since the common cases (quads, UI
      rects, anything axis-aligned) are now exact and the residue is a fraction
      of a percent of pixels on off-axis blended geometry.

- [✅] **Reversed-Z depth** (2026-09-26) — `RenderCommand::depthCompare`
      (`VkCompareOp` parity, defaults to `Less` so nothing existing changed),
      `Mat4f::perspectiveReverseZ`, per-layer clear depth
      (`addLayer(..., clearDepth)` / `setLayerClearDepth`), and the far-plane
      reject widened to a `[0,1]` range check so it is correct under both
      conventions. All depth tests in the three rasterizers go through
      `depthPasses()`.

      Measured (`depthtest`, scratchpad) — smallest separation at which the
      nearer surface reliably wins, near=0.1 far=500:

      | distance | standard Z | reversed Z | gain |
      |---|---|---|---|
      | 10 | 0.00295 | 0.0000026 | 1153x |
      | 100 | 0.0193 | 0.000043 | 450x |
      | 400 | 1.329 | 0.000069 | 19343x |

      One switch: `RenderConfig::reverseZ`. It drives the clip plane, the
      default depth compare (`DepthCompare::Auto` -> `Greater`) and the default
      layer clear depth; the caller only has to use `perspectiveReverseZ` for
      the projection to match. `MinimalEngine` runs it.

      **`clipNear` was the trap.** It clipped on `z >= 0`, which is the near
      plane under standard Z and the FAR plane under reversed - so enabling
      reversed-Z disabled near-plane clipping and geometry behind the camera
      rasterised as huge triangles. It now clips on `w - z >= 0` when reversed.
      `cliptest` (scratchpad) checks a quad straddling the near plane at seven
      tilts and requires identical coverage under both conventions.

      Not everything that shimmers is depth: `flicker` (scratchpad) showed
      98-100% of whole-scene shimmer in `trees/scene.gltf` is texture
      minification aliasing, unaffected by the depth convention. That one wants
      mipmaps. Reversed-Z fixes z-fighting between near-coplanar surfaces,
      which is a different artifact - measure which one you have before
      reaching for either.

      Note: `Renderer::clear` gained a clear-depth parameter **last**
      (`clear(color, target, depth)`). Putting it second silently broke existing
      `clear(color, target)` calls, since `RenderTargetID` converts to float
      without a warning.

### Robustness — unchecked indices that fail as segfaults

Not API gaps, but each one costs an hour of confusion when it bites, and the Vulkan
backend won't have the same failure modes to compare against.

Every one of these now has a debug `assert` naming the offending call plus a
release-build early return, since `./build.sh release` defines `NDEBUG` and the
asserts vanish. Verified by `guardtest` (27 bad-ID calls, built `-DNDEBUG`, none
crash, ASan + UBSan clean; the same source with asserts on aborts on the
matching assert, naming the call).

- [✅] `Renderer::draw` does `m_programs[cmd.program]` with no bounds check and
      no handling of `DEFAULT_PROGRAM` (= `UINT32_MAX`), so a command submitted
      without a program indexes ~4e9 past an empty vector. The crash surfaces
      later, inside `drawTriangles` at the `vsThunk` call, which points at the
      wrong file. Same for `m_vertexBuffers[cmd.verts]` / `m_indexBuffers`.
      `validTarget/Verts/Indices/Program/Uniform` predicates on `Renderer`;
      `draw` rejects all four plus a stale uniform slot. `clear`, `show` and
      both `updateTexture` overloads were the same shape — `show` subscripted
      before its null check, `updateTexture` bounds-checked but still
      dereferenced a destroyed target. `VALO` got `validLayer` on `submit`,
      `removeLayer`, `setLayerClearColor/Depth`, `pushClip`, `popClip`, and
      `setLayerOrder` now filters instead of storing bad IDs for `beginFrame`.
- [✅] `texture()` did `(*boundTargets)[tex]` unchecked (was Sampling.cpp:48, :84),
      so sampling `EMPTY_TEXTURE` is an out-of-bounds read rather than a
      diagnosable error. Every shader currently has to guard by hand.
      Returns magenta on a bad ID; `textureDepth()` returns 0 (reversed-Z far).
- [✅] `setupTriangle` clamps with `std::clamp<float>(lo.x, 0, w - 1)` where
      `w = int(target->width())`. At width 0 that is `clamp(x, 0, -1)` — hi < lo
      is UB. Minor since the loops then no-op, but a zero-sized target is
      reachable (a minimised window reports 0x0, and `renderScale` can round
      down to it).
      Fixed by the viewport work: `resolveClipRect` yields `x0 > x1` at width 0
      and `setupTriangle` returns before reaching the clamps.

## 3. Backend interface extraction — DONE

Historical. The split landed 2026-09-30 and the function-pointer table it
produced now carries 30 entries, both backends filling all 30. **See `PLAN.md`**
for the phased plan these items fed into, and for the shader-contract decision
that gated them.

### Semantics the software backend gets away with and Vulkan does not

Both of these are API-visible rules, not implementation details, so they have to
be decided before the backend rather than discovered during it. Neither costs
anything in software - the point is that the same dev code stays correct on both.

- [✅] **Deferred destruction** (2026-09-28) - `destroy*` enqueues `(id, m_frame)`;
      `destroyAllQueued()` drains from `VALO::beginFrame` once an entry has aged
      `framesInFlight` frames; `shutdown()` forces a full drain. `m_frame`
      increments in `show()`, so `RenderConfig::framesInFlight` finally does
      something. One `retireQueue` template covers all five resource types.

      The rule that matters: **the ID is deferred along with the object.**
      Recycling a handle while a queued draw still names it resolves that draw
      to a different resource - wrong pixels, no crash. See `PLAN.md` phase 0a
      for the two bugs this took on the way in.

- [~] **Mutation while queued** - decided 2026-09-28, wording deferred.
      `update*Buffer` is a memcpy, so writing one after `submit` and before
      `endFrame` silently changes what the queued draw reads. **Decision:
      document "only between frames"; no per-frame copies.** Rejected the
      copies because they would constrain the backend before it existed and can
      be added later without an API change.

      The comment lands in the full documentation pass, not before. What it has
      to say: `updateTexture` and `updateUniformBuffer` genuinely misbehave
      today (both are read at `flush()`, so a mid-frame write reaches a draw
      queued earlier in the *same* frame); `updateVertexBuffer` /
      `updateIndexBuffer` are safe as the software backend stands, and are
      covered only for Vulkan forward-compatibility. See `PLAN.md` phase 0b.

- [✅] ~~Split `Renderer` into an abstract interface + `SoftwareRenderer`.~~
      **Done 2026-09-30, but not as an abstract interface.** `Renderer` stays
      one concrete class and dispatches through a 28-entry function-pointer
      table installed by `init()`. A base class would have duplicated ~40
      declarations to express what the class already expresses; `#ifdef` gating
      would force one backend per binary, and the phase-6 conformance harness
      wants both in one process. Full reasoning in `PLAN.md`.

      Implementations live in `include/render/backends/SoftwareBackend.cpp` and
      `VulkanBackend.cpp` as `Renderer::software*` / `Renderer::vulkan*`
      members, so the bodies moved verbatim rather than being rewritten as free
      functions. Backend state is `VulkanRenderer* m_vulkan` - typed, not
      `void*`, because only one backend needs any.

      The non-obvious entries: five `release*` (the deferred-destroy *timing* is
      shared, the freeing is not - without these Vulkan leaks everything
      silently) and `initBackend` (`init` picks the table so it is not *in* the
      table, but the backend half of startup still has to dispatch).

- [✅] ~~Decide the shader contract for the GPU side.~~ **Decided 2026-09-28:
      two implementations, one spec.** `createProgram` is overloaded - C++
      callbacks for software, GLSL/SPIR-V for Vulkan - and they stay separate.
      The dual-compile header was rejected: a C++ function pointer cannot become
      SPIR-V and pretending otherwise drags the divergence into every uniform
      struct. **Endgame is a C++ -> GLSL translator**, for which each
      hand-written pair is an input/expected-output test case. The defence
      against drift is the conformance diff harness, pulled forward to phase 3.

## 4. Vulkan backend

Step-by-step bring-up notes: `VULKAN_SETUP.md`. Phase plan: `PLAN.md`.

- [✅] **Instance / device / queues / swapchain / frames-in-flight / sync**
      (2026-09-30). All nine steps of `VULKAN_SETUP.md`. `VulkanContext`,
      `VulkanSwapchain`, `VulkanFrames`, `VulkanRenderer`. Dynamic rendering
      rather than render passes, `synchronization2`, validation on from the
      first line. `renderFinished` is per swapchain *image* while
      `imageAvailable`/`inFlight` are per frame in flight - different counts,
      and conflating them is the classic bug.

- [✅] **A window clearing through `VALO`** (2026-09-30). `vk-clear` runs
      clean; `flush` is correctly a no-op, since it runs once per *layer* and
      anything that submits belongs in `show`.

- [✅] **One triangle** (2026-10-01). Shader modules, the pipeline, the pipeline
      cache, `draw`, push constants and depth. `vk-triangle` is a full `VALO`
      example: rotating, depth-tested, cull-mode-switchable, vsync-togglable,
      fps in the title.

- [✅] **Buffers** (2026-10-01). Vertex, index and uniform, all through one
      `uploadBuffer` staging path. Updates retire the old allocation onto a
      `(buffer, frame)` queue drained from `show()` after `framesInFlight`
      frames, so a frame still in flight never reads freed memory.

- [✅] **Uniform buffers / descriptor sets** (2026-10-01) - as push descriptors.
      See the State section for why that beat a pool.

- [✅] **Pipeline cache** keyed on `(program, packed state, vertex layout)`.

- [✅] **Textures** (2026-10-02) - bindless, with separate samplers. See the
      State section for the design and the traps. `createTexture`,
      `createTextureFromFile` and sampling in GLSL all work;
      `minimal --vulkan` renders the scene textured.

- [✅] **`updateTexture` / `updateTextureRect`** - `uploadImageRect` stages a
      `VkBufferImageCopy` with an offset and extent and does the layout round
      trip from `GPUTexture::layout` (not `UNDEFINED`, which would discard the
      rest of the image), rebuilding the mip chain afterwards since mip 0
      changed. `updateTexture` is a full replace, so it just calls
      `uploadImage`. Both go through `beginOneShot`/`endOneShot`, so a call
      mid-frame on a texture the in-flight frame samples is still a race -
      acceptable for load-time and between-frame updates, same exposure
      `createTexture` already has.

- [✅] **Texture destruction** - `releaseTarget` runs off `Renderer`'s retire
      queue, which only actually fired once `VulkanRenderer::show` started
      incrementing `m_owner->m_frame` (it never did, so nothing retired until
      shutdown and every destroyed resource leaked for the run). The bindless
      slot is repointed at a 1x1 white `m_nullTexture` by
      `clearTextureDescriptor` before the image dies, so a freed slot is never
      a dangling view rather than merely an unread one. The retire gate is
      `> framesInFlight`, not `>=`: `destroyAllQueued` runs at the top of
      `VALO::beginFrame`, *before* the backend's fence wait, so a resource last
      used in frame Q needs `N - Q >= framesInFlight + 1`.

- [✅] **Render targets** - `beginFrameIfNeeded` was split into the per-frame
      part and `bindTarget`, which opens one dynamic-rendering block per
      distinct target and reuses the open one when the target is unchanged and
      the load op is `LOAD`. `GPUTexture` tracks `layout`, and
      `endRenderingIfOpen` barriers `COLOR_ATTACHMENT_OPTIMAL` ->
      `SHADER_READ_ONLY_OPTIMAL` so the next layer can sample it. Targets carry
      their own `DepthBuffer` (the shared `m_depth` is swapchain-sized),
      `show(target)` blits a non-backbuffer target over the swapchain image,
      and `tracksWindow` targets are rebuilt in `recreateSwapchain`.

      Four things bit along the way, all now fixed: `m_textures` started empty
      so the first target got id 0 and aliased `Renderer::BACKBUFFER` (slot 0
      is now reserved, mirroring `SoftwareRenderer::init`); the pipeline cache
      baked in the swapchain's colour format, so `PipelineKey` gained
      `colorFormat`; `draw` took its default viewport from the swapchain rather
      than the bound target; and `Renderer::shutdown` force-drained the destroy
      queue *before* the backend went idle, so a `destroyTarget()` immediately
      followed by `shutdown()` freed images the GPU was still reading - the
      table gained a `waitIdle` entry.

      `examples/rendertarget.cpp` covers all of it: default mode samples a
      512x512 target, `--show-target` presents it directly, `--track` resizes
      it with the window, `--update` swaps it for a CPU-updated texture.

- [✅] **Stencil** (2026-10-05). `VkStencilOpState` on front and back, with
      `stencilTest`/`stencilOp`/`stencilRef` added to `packPipelineState` -
      `stencilRef` has to be in the key because it bakes into the pipeline
      rather than being dynamic state. VALO's single `StencilOp` is Vulkan's
      `passOp` with the other two `KEEP`, because `stencilApply()` only runs
      after a fragment survives both tests. `Incr`/`Decr` are the `_AND_WRAP`
      variants, matching `uint8_t(s + 1)`.

      The attachment side needed `stencilAttachmentFormat` on the pipeline,
      `pStencilAttachment` on the `VkRenderingInfo`, and the depth image views
      widened from `DEPTH` to `DEPTH | STENCIL` so one view serves both.

      *Verified:* `stencil.exe --vulkan` matches the software build - the
      outline pass would expose a wrong `passOp` and the circular clip a wrong
      `compareOp`.

- [~] **A glTF scene on both backends, switched by a runtime flag** - reached
      by `minimal --vulkan` (2026-10-01, textured 2026-10-02): same `Engine`,
      same camera, same `RenderCommand` state, resolved through one
      `createProgram(vsPath, fsPath)` call that the software backend answers
      from the shader registry and the Vulkan backend by translating. Every
      other example takes `--vulkan` too. **The conformance diff harness is
      still unwritten**, so nothing yet checks the two backends pixel against
      pixel - this is closed by eye, not by test.

### Known gaps, deliberately left

- ~~**Every `valid*` predicate is false on Vulkan**~~ - **fixed.** The bounds
  half now goes through a `slotCount(Renderer&, ResourceKind)` entry on the
  backend table, which each backend answers from its own storage, so the five
  predicates are backend-agnostic. The software-only liveness probe
  (`m_targets[id]` non-null) was dropped as redundant: a released slot is in
  the destroy queue until it retires and in the free list afterwards, so the
  bounds check plus those two is the whole test.
- ~~**`draw` opens a frame with `LOAD_OP_LOAD` but the barrier always uses
  `oldLayout = UNDEFINED`**~~ - **fixed.** `m_swapLayouts` tracks a layout per
  swapchain image; a CLEAR still names UNDEFINED (it overwrites anyway, and
  discarding is cheaper) while a LOAD names the layout the image is actually
  in. This was live, not theoretical: any frame that renders to a target and
  then back to the backbuffer reopens the backbuffer scope with LOAD, so the
  backbuffer's clear was being discarded.

  Residual difference, inherent rather than a bug: there are 3 swapchain images
  but one software backbuffer, so LOAD on Vulkan preserves *that image's* last
  contents - three frames back - where software gives the previous frame.
  Matching software exactly would mean rendering to an offscreen image and
  blitting, which is a bigger call than this gap justifies.
- ~~`recreateSwapchain()` errors out if the new swapchain reports a different
  image count~~ - **fixed.** It now tears down and re-inits `VulkanFrames` at
  the new image count and resets `m_frameIndex`, rather than failing. Still
  never observed on this hardware.
- ~~`uploadBuffer` has no zero-size guard~~ - already fixed; it opens with
  `if (bytes == 0) return false;`. This entry was stale.
- No validation-error counter. The debug messenger prints and returns
  `VK_FALSE`, so `vk-test`'s draw sections have no automatic oracle - clean
  stderr *is* the pass condition. A counter on `VulkanContext` would let the
  harness gate on it.
- ~~**Uniform block layout is still undecided**~~ - **resolved.**
  `scalarBlockLayout` is enabled in the feature chain (`VulkanContext.cpp`) and
  every generated shader declares `layout(scalar, push_constant)`, so GLSL
  follows C++ offsets field-for-field and the loose-`float` workaround is gone.
  One exception remains that scalar layout cannot fix: a C++ `bool` is 1 byte
  while a GLSL block `bool` is 32-bit, so uniform structs must use `uint32_t`
  for flags. `pbr.vert.hpp` hit this - `displace` was read 4 bytes past the end
  of the pushed range and was permanently false. `gltf.vert.hpp` has the same
  shape. valoc should reject `bool` in a uniform struct outright.
- **valoc does not translate a `uint` flag used as a boolean**, which is the
  only thing stopping full shader coverage: 36 of the 38 files in
  `examples/shaders/` translate and compile, and the two that do not are
  `pbr.frag` (`if (!u.detail || ...)`, line 11) and `gltf.frag`
  (`if (u.useNormalMap && ...)`, line 47). C++ converts `uint` to `bool`
  implicitly and GLSL does not, so the emitted `!u.detail` and `u.x && y` are
  type errors.

  This is the other half of the `bool` entry above: the flags became `uint32_t`
  to fix the push-constant layout, and the shader bodies kept reading them as
  booleans. Either valoc rewrites `!x` to `x == 0u` and `x &&` to `x != 0u &&`
  when the operand is an integer, or the shaders spell the comparison out -
  `pbr.vert.hpp:290` already does (`u.displace != 0 && u.detail != 0`).
- **No conformance diff harness.** Nothing compares the two backends pixel
  against pixel; `--vulkan` parity is checked by eye. `PLAN.md` phase 3 asked
  for this to start with textures and it never did, so every parity claim in
  this file rests on looking at two windows.

## 5. Nice-to-have (either backend, not blocking)

- [ ] **MRT** — fragment shader outputs N colors, command binds N targets
      (deferred shading).
- [ ] **Per-instance vertex buffer** (instance step rate) as an alternative to
      `valo_InstanceIndex` + uniforms.
- [ ] **Per-command sort key** within a layer (back-to-front for transparency,
      by program/texture for state coherence).
- [ ] **2D / 3D quick-config presets** — 2D = pixel-space ortho tracking
      resize, no depth, alpha blend, no cull, plus a sprite/quad helper for
      UI-heavy use (UILO). `clipstack.cpp` is the template.
- [ ] **Point coverage / anti-aliased points** — disc coverage per edge pixel,
      blended via alpha.

## 6. Software-renderer optimization (never blocking)

- [✅] Multithreaded rasterization — `ThreadPool` (generation + active-count,
      TSan-clean) + `forEachBand`; geometry phase on the main thread into a
      chunked buffer, workers rasterize disjoint row bands (`pool.size() * 8`).
      Triangles, lines, points, clear all banded.
- [✅] Triangle setup once per triangle (`setupTriangle` -> `ScreenTri`);
      barycentrics via `invArea` multiply; bilinear on packed ABGR32.
- [✅] **Batched band dispatch** (2026-09-26) — the big one. `drawTriangles`
  used to end every call with its own `forEachBand`, so every draw paid a full
  thread-pool barrier (~10 us median, 30 us p90, 80 bands) however few triangles
  it had. Triangles now accumulate in a `TriBatch` owned by `Renderer` and go
  out in chunks of 4096 regardless of which draw produced them; `ScreenTri`
  carries a `state` index into a parallel `DrawState` list holding that draw's
  target/program/command/uniforms.

  `DrawState` stores the command and program **by value**. Rasterising is
  deferred now, so a caller's `RenderCommand` can go out of scope between its
  `draw()` and the flush - which it does the moment anyone builds one in a
  block. ~700 B per draw against a ~10 us dispatch is free, and it makes the
  batch impossible to dangle. (ASan caught exactly this within a minute of the
  first version, which pointed at the pointers.)

  Flush points: buffer full, target change, `clear()`, `show()`, and
  `VALO::endFrame` per layer - that last one matters because the layer's command
  queue is cleared right after. Wireframe keeps the old per-draw path: it buffers
  raw `ClipVert`s with nowhere to hang a state index, and is never the
  many-small-draws case. Lines and points flush first, since they are not batched
  and must not jump ahead of queued triangles.

  Order is preserved: workers own disjoint row bands, and within a band the batch
  is walked front to back, so later draws still composite over earlier ones and
  still see their depth writes.

  **Callers driving `Renderer` directly must now call `flush()` before reading
  pixels back.** Through VALO nothing changes.

  Wireframe joined the batch later (its own `wires` list, since it keeps raw
  `ClipVert`s rather than `ScreenTri`s); a batch never holds both kinds, and
  switching flushes so order survives.

  Measured on `minimal.cpp` (trees/scene.gltf, 256,652 tris, 2712 draws, 800x600):

  | | before | after |
  |---|---|---|
  | frame | 83.2 ms (12 fps) | **30.9 ms (32 fps)** |
  | resolution-independent (G) | 67.5 ms | 12.9 ms |
  | fragment (F) | 15.6 ms | 18.4 ms |
  | split | 81% geometry / 19% fragment | 41% / 59% |

  Per-draw overhead across 1 -> 2712 draws went from 66 ms (24 us/draw) to
  **zero** - frame time is now flat in draw count. Merging primitives by
  material engine-side, which was the planned follow-up, is therefore not worth
  doing.

- Profile (25^3 cubes, 4K, BEFORE batching): geometry ~5 ms, raster ~31 ms of
  which ~20 ms is fragment shading + `writePixel`, ~10 ms per-triangle floor.
  Band scan is 0.03 ms — binning is NOT worth it. The cube lattice was a poor
  proxy: enormous fill, trivial unique geometry, few draws. `trees/scene.gltf`
  is the better target.
- What would close the SwiftShader gap, in order of payoff: (1) shade 4 pixels
  per fragment call so the surrounding loop vectorizes — changes the shader
  contract; (2) explicit SIMD for the fixed parts (edge stepping, depth
  compare, interpolation) — keeps the API, ~1.3x; (3) a JIT — out of scope.
- [✅] **Line band clamping** (2026-09-26) — `rasterizeLine` stepped the whole
      DDA in every band and discarded pixels outside its rows, so each line was
      walked once per band (80x on a default pool). It now solves the step range
      against the band's y-span first. Wireframe on `trees/scene.gltf`:
      267 ms -> 146 ms, and batching wireframe took it to **72 ms**. Helps
      `Topology::Lines` draws equally.
- [ ] Parallel geometry phase (fixed index range per worker, order preserved).
      Now attacks the 12.9 ms G, which is 41% of the frame - worth more than it
      was before batching, since that 12.9 ms is real per-triangle work rather
      than barrier overhead.
- [ ] Vertex cache: shade each unique index once per draw. The trees scene runs
      769,956 vsThunk calls for 378,154 unique vertices, so roughly half of them
      are redundant.
- [ ] Per-phase timing (clear / draw / show) printed with the fps. `Clock` /
      `Timestamp` and `VALO::getDeltaTime()` landed 2026-09-22, so this is now
      just instrumenting the three call sites.

## Done

- [✅] OBJ loading (tinyobjloader) · glTF loading (cgltf) · draw ranges ·
      dynamic vertex/index buffers · bilinear · sampler state (wrap/filter) ·
      instancing (`valo_InstanceIndex`) · depth as texture · depth-only passes
      (`colorWrite`, vs-only programs) · fragment discard · far-plane reject ·
      stencil buffer (test/op/ref) · clip stack (`pushClip`/`popClip`) ·
      Window (pixel-sized, input) · multithreaded raster.

## Examples

Every example below except the four Vulkan harnesses takes `--vulkan` and runs
the same scene on either backend; the shaders live in `examples/shaders/` as
`.vert.hpp` / `.frag.hpp` pairs, which the software backend resolves through the
shader registry and the Vulkan backend translates with `valoc`.


- `hello.cpp` — start here: one file, one spinning vertex-coloured triangle,
  nothing but window → init → program → buffers → loop. `--vulkan` works.
- `main.cpp` — instanced N^3 textured cube lattice, orbit camera, fps title.
- `particles.cpp` — galaxy point-particle shader.
- `sdf.cpp` — iq's raymarched primitives, fullscreen fragment shader, FPS camera.
- `pbr.cpp` — 62-material Cook-Torrance viewer with fbm bump/displacement and
  sky/sun environment (port of cool-pbr-test; 1680 -> 330 lines). Lights and
  camera live in a `SceneUniforms` uniform buffer at slot 0.
- `gltf.cpp` — DamagedHelmet / Box through a full glTF PBR shader (base color,
  packed metal-rough, normal map with load-time tangents, AO, emissive).
- `stencil.cpp` — helmet with a stencil outline (Replace + NotEqual extruded
  shell along smoothed normals) and an SDF circle clip (discard + Incr).
- `clipstack.cpp` — UI-style: pixel-space ortho, rounded-rect panels, a
  scrolling list with three nested `pushClip`s. `C` toggles clipping.
- `showcase.cpp` + `engines/ShowcaseEngine.hpp` — the big one: glTF models on a glossy
  mirrored floor under a procedural sky, four orbiting lights with visible
  emissive orbs, ACES tonemapping. Lights/camera/exposure in a `SceneUniforms`
  uniform buffer bound to slot 0 by every shader.
- `minimal.cpp` + `engines/MinimalEngine.hpp` — smallest embedder: an `Engine` owning
  window/VALO/camera/light and a `Model` that loads a glTF, owns its materials
  and program, and submits one command per primitive. Metallic-roughness
  Cook-Torrance with a hemisphere ambient standing in for IBL. Doubles as the
  test for what the API feels like from outside the repo.
  **`--vulkan` runs the same scene on the Vulkan backend**, with `Model`
  choosing the GLSL or the C++ shader half from `cfg.backend`. Untextured until
  `createTexture` exists.

- `rendertarget.cpp` — render to texture. Two layers: the first draws rounded
  rects into an offscreen 512x512 target, the second samples it through a quad
  on the backbuffer. `--show-target` hands the target straight to `endFrame()`
  so the backend presents it directly; `--track` makes the target
  window-tracking, so the backend has to rebuild it on every resize.

### Vulkan examples

- `vk-clear.cpp` — a window clearing to a cycling hue. Proves frame-index wrap,
  sustained running, resize absorption and clean teardown; nothing is drawn, the
  clear is the attachment's `loadOp`.
- `vk-triangle.cpp` — rotating triangle through `VALO`: GLSL program, push
  constants, depth, dynamic viewport. `F` cycles cullmode, `V` toggles vsync,
  fps in the title.
- `vk-cubes.cpp` — instanced cube grid, the Vulkan counterpart of `main.cpp`:
  vertex and index buffers, `gl_InstanceIndex`, perspective, depth occlusion,
  orbit camera, `W` for wireframe. The example that exposed the clip-space Y
  flip, because it was the first closed geometry under a projection.
- `vk-test.cpp` — the harness. Sections for context, swapchain, programs,
  uniform buffers and the pipeline state tables. The draw sections have no
  automatic oracle: clean validation output *is* the pass condition.
- `shaders/` — 38 `.vert.hpp` / `.frag.hpp` C++ shader files, one per shader,
  each registering its entry point with `VALO_VERTEX_SHADER` /
  `VALO_FRAGMENT_SHADER`. Alongside them, `valo.glsl` is the bindless prelude
  (`uTextures`, `uSamplers`, `valoTexture()`, the sampler-index constants),
  pulled in by every generated shader thanks to `glslc -I examples/shaders`.
  The `.vert` / `.frag` files are valoc output and the `.spv` beside them is
  glslc output; both regenerate when the source is newer.

- `bench/` — **untracked, not in git** (never committed; a fresh clone has no
  `examples/bench`). `vk_helmet.cpp` (headless Vulkan) and `valo_helmet.cpp`, same
  scene/shader/resolution. Run the Vulkan one on SwiftShader with
  `VK_ICD_FILENAMES=$PWD/ext/swiftshader/build/Darwin/vk_swiftshader_icd.json`.

## Deferred / probably not

- **MSAA** — expensive in software; `renderScale` already gives supersampling.
- **Compute** — out of scope; this is a graphics library.
