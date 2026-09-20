# VALO TODO

Software renderer feature work, roughly in order of payoff. Optimization comes
after all of this is done.

## State (2026-09-20)

~3.3k lines of library, 7 examples, 16/24 feature items done. Every feature has
a sanitizer-verified harness. The dev-facing API (Window -> VALO -> create* ->
RenderCommand -> submit, typed shader callbacks) is settled and Vulkan-shaped;
the backend should slot under it without dev code changing.

## Cleanup before the backend (from the 2026-09-20 review)

- [ ] **Shared pixel path** — the three rasterizers duplicate the per-pixel
      sequence (depth test -> stencil test -> shade -> discard -> depth write ->
      stencil op -> blend). Pull it into one `shadePixel()` helper so sRGB/MRT
      land in one place. DrawFuncs.hpp is 558 lines; a stage map comment at the
      top would help too.
- [ ] **Renderer.hpp -> Renderer.cpp** — 18 inline definitions still in the
      header (VALO, Window, ThreadPool, RenderTarget are done).
- [ ] **Sentinel macros -> constexpr** — `EMPTY_BUFFER`, `DEFAULT_PROGRAM`,
      `ALL_INDICES`, `MAX_INSTANCES` in RenderCommand.hpp.
- [ ] **Logging hook** — 8 `std::cout` sites; one `valo::log(level, msg)` the
      dev can redirect.
- [ ] **Uniform limit** — `MAX_UNIFORM_BYTES = 256`; gltf.cpp is at ~200. Bump
      to 512 or make it a template parameter before someone hits it.
- [ ] **Quat.hpp** is empty — needed for glTF animation / camera code.

## Features

- [x] **OBJ loading** — tinyobjloader vendored via `build.sh`; wrapper in
      `utils/OBJParser.hpp` gives positions/uvs/normals, face triples, materials,
      and per-face material ids. Mesh/vertex-buffer assembly is dev-side.
- [x] **glTF loading** — cgltf vendored (`ext/cgltf`, v1.15); wrapper in
      `utils/GLTFParser.hpp`: one interleaved vertex array + one index array,
      meshes → primitives as draw ranges, PBR materials with resolved texture
      paths, nodes with flattened world transforms. Verified on Khronos Box
      and DamagedHelmet.
- [x] **Draw ranges** — `firstIndex` / `indexCount` on `RenderCommand`
      (`ALL_INDICES` default). One vertex+index buffer, many materials.
- [x] **Bilinear filtering** — 2x2 tap in `texture()`, `Filter::Linear`.
- [ ] **Mipmaps** — generate the chain at `createTexture`; pick LOD from uv
      derivatives (per-triangle `du/dx`, `dv/dy` computed in the rasterizer).
      Fixes the crawling on far/oblique surfaces.
- [x] **Sampler state** — `Sampler{wrapU, wrapV, filter}` passed to
      `texture()`; Repeat / Clamp / Mirror, Nearest / Linear. Default is
      repeat + linear.
- [ ] **sRGB** — decode textures to linear on sample, encode on write to the
      backbuffer. Needed the moment lighting is involved.
- [x] **Instancing** — `instanceCount` on `RenderCommand` + `valo_InstanceIndex`
      built-in read by the vertex shader. (Per-instance vertex buffer with
      instance step rate is a possible later addition.)
- [x] **Dynamic buffers** — `updateVertexBuffer(id, data, count[, newLayout])` /
      `updateIndexBuffer(id, data, count)`.
- [ ] **MRT** — fragment shader outputs N colors, command binds N targets.
      Needed for deferred shading.
- [x] **Depth as texture** — `textureDepth()` samples a target's depth buffer.
- [x] **Depth-only passes** — `colorWrite` on `RenderCommand`: depth is
      tested/written, fragment shader and color write are skipped. Shadow maps
      are now entirely dev-side (target + layer + `textureDepth`).
- [x] **Fragment discard** — `discard;` in a fragment shader drops the pixel;
      discarded pixels write no depth/stencil. `colorWrite = false` still runs
      the shader (so SDF clips work); a program created with `createProgram(vs)`
      has no fragment stage at all (the cheap depth-only path).
- [x] **Far-plane reject** — fragments with `z > 1` are skipped. Note: geometry
      placed at exactly `z = 1` (sky quads) gets rounding-dropped; use `0.9999`.
- [ ] **Point coverage / anti-aliased points** — disc coverage per edge pixel,
      blended via alpha. Removes the last of the point-size stepping.
- [x] **Stencil buffer** — 8-bit per pixel; `stencilTest` (Always/Equal/
      NotEqual/Less/Greater) + `stencilOp` (Keep/Zero/Replace/Incr/Decr) +
      `stencilRef` on `RenderCommand`. Test is early, op applies only to pixels
      that survive depth/discard. Demo: `stencil.cpp` (outline + SDF circle clip).
- [ ] **Other GLSL built-ins** — `valo_FragCoord`, `valo_FrontFacing`,
      `valo_VertexID`, etc. Same pattern as `valo_PointSize`.

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

## API / ergonomics

- [x] **Clip stack** — `VALO::pushClip(shape)` / `popClip()` on the stencil
      buffer: any shape (SDF rounded rects, meshes), unbounded nesting, nested
      pushes intersect. Content submitted inside gets `Equal(depth)` + `Keep`.
      Stack is cleared at `beginFrame`. Demo: `clipstack.cpp` (nested UI panels).
- [ ] **Scissor rect** — optional fast path for axis-aligned clips (bbox clamp
      instead of a per-pixel stencil compare). Vulkan dynamic scissor.
- [ ] **2D / 3D quick-config presets** — a 2D preset = pixel-space orthographic
      projection tracking resize, no depth test/write, alpha blend, no cull;
      3D preset = current defaults. Plus a sprite/quad helper (shared unit quad
      + per-draw `{pos, size, uvRect, tint}` uniforms) for UI-heavy use (UILO).
- [ ] **Per-command sort key** within a layer (back-to-front for transparency,
      by program/texture for state coherence).
- [x] **Window** — `valo::Window` owns the SDL window; pixel-sized on Retina,
      `pollEvents`, `Key`/`MouseButton` enums (SDL scancode values),
      `isKeyDown` / `wasKeyPressed` / `mouseDelta` / `scrollDelta`.
- [ ] Move remaining header implementations into `.cpp` files once the
      interfaces settle (VALO done; Renderer, DrawFuncs, RenderTarget partial).

## Deferred / probably not

- **MSAA** — expensive in software; `renderScale` already gives supersampling.
- **Compute** — out of scope; this is a graphics library.
- **GLSL translation / dual-compile shaders** — revisit when the Vulkan
  backend exists and the real constraints are known.

## Later: Vulkan backend

- Abstract `Renderer` behind an interface once the software renderer is
  feature-complete; keep the dev-facing API (`createProgram`, `RenderCommand`,
  layers, IDs) identical. Only shader source changes.

## Optimization

- [x] **Multithreaded rasterization** — `ThreadPool` (generation +
      active-count, TSan-clean) + `forEachBand`: geometry phase on the main
      thread into a chunked buffer, workers rasterize disjoint row bands
      (`pool.size() * 8` bands). Triangles, lines, points, and clear all banded.
- [ ] Parallel geometry phase (triangles are independent; preserve output order
      by giving each worker a fixed index range).
- [ ] Per-phase timing (clear / draw / show) printed with the fps to see where
      frame time actually goes; `show()`'s `SDL_UpdateTexture` upload is the
      big serial chunk.
- [x] Triangle setup once per triangle (`setupTriangle` → `ScreenTri`), bands
      only band-clamp + pixel loop. Barycentrics via `invArea` multiply.
      Bilinear on packed ABGR32 (`lerpABGR32`, 0..256 weights, exact).
- Profile (25^3 cubes, 4K): geometry ~5 ms, raster ~31 ms of which ~20 ms is
  fragment shading + `writePixel`, ~10 ms per-triangle floor. Band scan is
  0.03 ms — binning is NOT worth it. Next real step is a sampling profiler
  (Instruments) on the fragment path, not more guessing.
- Incremental edge functions (step, don't recompute per pixel).
- Vertex cache: shade each unique index once per draw.
- SIMD inner loops (interpolation, blending, sampling).
- Depth-test before shading (early-z) is already the order; keep it.
- Profile with a real scene (Iron Man OBJ: 217k tris, 11 materials).
