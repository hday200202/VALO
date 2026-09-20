# VALO TODO

Software renderer feature work, roughly in order of payoff. Optimization comes
after all of this is done.

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
- [ ] **Fragment discard** — a way for the fragment shader to reject the pixel
      (alpha-tested cutouts, foliage).
- [x] **Far-plane reject** — fragments with `z > 1` are skipped. Note: geometry
      placed at exactly `z = 1` (sky quads) gets rounding-dropped; use `0.9999`.
- [ ] **Point coverage / anti-aliased points** — disc coverage per edge pixel,
      blended via alpha. Removes the last of the point-size stepping.
- [ ] **Stencil buffer** — low priority; most classic uses are doable with
      depth tricks or FS masks.
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

## API / ergonomics

- [ ] **Scissor / clip stack** — REQUIRED for UILO. A per-command pixel-space
      scissor rect on `RenderCommand` (Vulkan `VkRect2D` scissor, dynamic state)
      is the primitive: the rasterizer clamps the bbox to it (triangles) / skips
      pixels outside it (lines, points). On top of that, `VALO::pushClip(rect)` /
      `popClip()` keep a stack, intersect the new rect with the current top, and
      stamp the result onto every command `submit()`ed while it's active. Nested
      UI containers are just nested pushes. Empty intersection = command is
      dropped at submit.
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
