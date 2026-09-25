# VALO TODO

Ordered by what it takes to bring the software renderer to parity with the
eventual Vulkan backend's dev-facing API. Work top to bottom; each section is
a gate for the next.

## State (2026-09-23)

~4.2k lines of library, 8 examples, 20/28 feature items done, every feature
behind a sanitizer-verified harness. The dev-facing API (Window -> VALO ->
create* -> RenderCommand -> submit, typed C++ shader callbacks) is settled and
Vulkan-shaped.

Landed since 2026-09-20: `Renderer.hpp` -> `Renderer.cpp` and `DrawFuncs`
split (both section-2 items, now checked off); sentinels were already
`inline constexpr`; `Clock` / `Timestamp` utils + `VALO::getDeltaTime()`;
`Sampling.cpp` split out; `EMPTY_TEXTURE` sentinel; `minimal.cpp` +
`MinimalEngine.hpp`. Benchmark (DamagedHelmet, 1280x960, lambert, M4): VALO 8.5 ms,
SwiftShader 3.0 ms, MoltenVK GPU 0.7 ms. The 2.85x gap to SwiftShader is its
LLVM-JIT'd 4-wide fragment shading; not something the Vulkan backend inherits.

---

## 1. API parity — things the Vulkan backend would force into the API

These are the gaps that would show up the moment the backend is written. Doing
them now means the API doesn't move later. ~3-5 days.

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
      copy. Half a day.
- [ ] **sRGB** — `linear` flag on `createTexture` (decode to linear on load),
      encode on write to the backbuffer. Vulkan expresses this as `_SRGB` image
      formats, so it has to be a texture/target property, not shader code.
      Every PBR demo currently does `pow(2.2)` by hand. Half a day; land it in
      the shared pixel path (section 2).
- [ ] **Mipmaps** — generate the chain at `createTexture`; LOD from per-triangle
      uv derivatives (`du/dx`, `dv/dy` in `setupTriangle`), `Sampler` gains a
      mip filter. Vulkan samplers assume a chain; without one GPU textures
      shimmer exactly like CPU ones do now. 1-2 days; the one substantive
      rasterizer item left.
- [ ] **Uniform buffer resource** — `createUniformBuffer` / `updateUniformBuffer`
      + a `UniformBufferID` the shader can read from. The 1 KB inline
      `setUniforms` maps to Vulkan push constants badly (128 B guaranteed); bone
      arrays and light lists want a real buffer. Half a day of API; software
      side is trivial.
- [ ] **Viewport rect** on `RenderCommand` — draw into a sub-rect of the target.
      Vulkan dynamic viewport. Half a day, alongside the scissor below.
- [ ] **Scissor rect** on `RenderCommand` — axis-aligned fast path (bbox clamp
      for triangles, pixel test for lines/points). Vulkan dynamic scissor. The
      stencil clip stack stays the general mechanism. Half a day.
- [ ] **glTF embedded images (`.glb`)** — `GLTFParser::texPath()` returns `""`
      unless `image->uri` is set, so every `.glb` loads fully untextured and
      silently: verified on a 29-material car, 15/15 images live in
      `buffer_view`, 0/29 materials got a texture. Same for `.gltf` files with
      `data:` URIs, which currently build a nonsense path out of the base64.
      `cgltf_buffer_view_data()` (cgltf.h:871) + `stbi_load_from_memory` is the
      decode; the structural part is that `GLTFMaterial` stores `std::string`
      paths, which can't name an embedded image. Suggest a
      `std::vector<GLTFImage>` on `GLTF` (decoded pixels + w/h) with materials
      holding `int` indices, so file-backed and embedded land in one place.
      Half a day.
- [ ] **Tangents in `GLTFVertex`** — it is `{pos, normal, uv}`, so nothing
      using the shared vertex type can do normal mapping. `gltf.cpp` works
      around it by declaring its own `Vertex` with a `Vec4f tangent` and
      computing the basis at load. That belongs in the parser (MikkTSpace-ish
      per-triangle accumulate) so every consumer gets it. 1 day.
- [ ] **Texture formats** — at least R8 (font atlases, masks) and RGBA32F
      (HDR targets). `createTexture` gains a format; `texture()` unpacks per
      format. Vulkan has these for free; software needs them to exist so the
      API doesn't change. 1 day.

## 2. Cleanup — before the backend, because the backend duplicates the shape

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

### Robustness — unchecked indices that fail as segfaults

Not API gaps, but each one costs an afternoon when it bites, and the Vulkan
backend won't have the same failure modes to compare against.

- [ ] `Renderer::draw` does `m_programs[cmd.program]` with no bounds check and
      no handling of `DEFAULT_PROGRAM` (= `UINT32_MAX`), so a command submitted
      without a program indexes ~4e9 past an empty vector. The crash surfaces
      later, inside `drawTriangles` at the `vsThunk` call, which points at the
      wrong file. Same for `m_vertexBuffers[cmd.verts]` / `m_indexBuffers`.
- [ ] `texture()` does `(*boundTargets)[tex]` unchecked (Sampling.cpp:48, :84),
      so sampling `EMPTY_TEXTURE` is an out-of-bounds read rather than a
      diagnosable error. Every shader currently has to guard by hand.
- [ ] `setupTriangle` clamps with `std::clamp<float>(lo.x, 0, w - 1)` where
      `w = int(target->width())`. At width 0 that is `clamp(x, 0, -1)` — hi < lo
      is UB. Minor since the loops then no-op, but a zero-sized target is
      reachable (a minimised window reports 0x0, and `renderScale` can round
      down to it).

## 3. Backend interface extraction

- [ ] Split `Renderer` into an abstract interface + `SoftwareRenderer`.
      Everything above `Renderer` (VALO, layers, clip stack, IDs, RenderCommand)
      stays identical. `RenderTarget` becomes backend-owned.
- [ ] Decide the shader contract for the GPU side: GLSL/SPIR-V passed to a
      `createProgram` overload. Optionally the dual-compile header (same source
      builds as C++ and GLSL) to shrink the rewrite — revisit once the backend
      exists and the real constraints are known.

## 4. Vulkan backend

- [ ] Instance / device / queues / swapchain / frames-in-flight / sync.
- [ ] Resources: buffers (staging upload), images + samplers, uniform buffers.
- [ ] Pipeline cache keyed on `(program, RenderCommand state, vertex layout)`.
- [ ] Layers -> render passes; targets -> framebuffers; `textureDepth` ->
      depth-image sampling; stencil ops verbatim; dynamic viewport/scissor.
- [ ] `gltf.cpp` running unchanged on both backends is the finish line.
      Estimate: 3-4 weeks.

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
- Profile (25^3 cubes, 4K): geometry ~5 ms, raster ~31 ms of which ~20 ms is
  fragment shading + `writePixel`, ~10 ms per-triangle floor. Band scan is
  0.03 ms — binning is NOT worth it.
- What would close the SwiftShader gap, in order of payoff: (1) shade 4 pixels
  per fragment call so the surrounding loop vectorizes — changes the shader
  contract; (2) explicit SIMD for the fixed parts (edge stepping, depth
  compare, interpolation) — keeps the API, ~1.3x; (3) a JIT — out of scope.
- [ ] Parallel geometry phase (fixed index range per worker, order preserved).
- [ ] Vertex cache: shade each unique index once per draw.
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

- `main.cpp` — instanced N^3 textured cube lattice, orbit camera, fps title.
- `particles.cpp` — galaxy point-particle shader.
- `sdf.cpp` — iq's raymarched primitives, fullscreen fragment shader, FPS camera.
- `pbr.cpp` — 62-material Cook-Torrance viewer with fbm bump/displacement and
  sky/sun environment (port of cool-pbr-test; 1680 -> 330 lines).
- `gltf.cpp` — DamagedHelmet / Box through a full glTF PBR shader (base color,
  packed metal-rough, normal map with load-time tangents, AO, emissive).
- `stencil.cpp` — helmet with a stencil outline (Replace + NotEqual extruded
  shell along smoothed normals) and an SDF circle clip (discard + Incr).
- `clipstack.cpp` — UI-style: pixel-space ortho, rounded-rect panels, a
  scrolling list with three nested `pushClip`s. `C` toggles clipping.
- `minimal.cpp` + `MinimalEngine.hpp` — smallest embedder: an `Engine` owning
  window/VALO/camera/light and a `Model` that loads a glTF, owns its materials
  and program, and submits one command per primitive. Metallic-roughness
  Cook-Torrance with a hemisphere ambient standing in for IBL. Doubles as the
  test for what the API feels like from outside the repo.
- `bench/` — **untracked, not in git** (never committed; a fresh clone has no
  `examples/bench`). `vk_helmet.cpp` (headless Vulkan) and `valo_helmet.cpp`, same
  scene/shader/resolution. Run the Vulkan one on SwiftShader with
  `VK_ICD_FILENAMES=$PWD/ext/swiftshader/build/Darwin/vk_swiftshader_icd.json`.

## Deferred / probably not

- **MSAA** — expensive in software; `renderScale` already gives supersampling.
- **Compute** — out of scope; this is a graphics library.
