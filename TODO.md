# VALO TODO

Software renderer feature work, roughly in order of payoff. Optimization comes
after all of this is done.

## Features

- [ ] **Mesh loading** — vendor a single-header loader (tinyobjloader for OBJ,
      cgltf for glTF) the same way stb_image is vendored in `build.sh`. Produce a
      vertex buffer + index buffer from a file. Gate for using the renderer on
      anything real.
- [ ] **Bilinear filtering** — 4-tap sample in `texture()`, weighted by the
      fractional uv.
- [ ] **Mipmaps** — generate the chain at `createTexture`; pick LOD from uv
      derivatives (per-triangle `du/dx`, `dv/dy` computed in the rasterizer).
      Fixes the crawling on far/oblique surfaces.
- [ ] **Sampler state** — wrap modes (repeat / clamp / mirror), filter mode.
      Vulkan puts these in a sampler object; a small `Sampler` struct or extra
      `texture()` params.
- [ ] **sRGB** — decode textures to linear on sample, encode on write to the
      backbuffer. Needed the moment lighting is involved.
- [ ] **Instancing** — `instanceCount` on `RenderCommand` + `valo_InstanceID`
      built-in (same thread_local pattern as `valo_PointSize`) so the vertex
      shader can index per-instance data in uniforms.
- [ ] **Dynamic buffers** — `updateVertexBuffer(id, data, count)` /
      `updateIndexBuffer` instead of destroy+create for per-frame data.
      Maps to a host-visible buffer write in Vulkan.
- [ ] **MRT** — fragment shader outputs N colors, command binds N targets.
      Needed for deferred shading.
- [ ] **Depth as texture** — sample a target's depth buffer from a shader
      (`textureDepth()`), for shadow maps.
- [ ] **Fragment discard** — a way for the fragment shader to reject the pixel
      (alpha-tested cutouts, foliage).
- [ ] **Far-plane reject** — skip fragments with `z > 1` in the depth test.
- [ ] **Point coverage / anti-aliased points** — disc coverage per edge pixel,
      blended via alpha. Removes the last of the point-size stepping.
- [ ] **Stencil buffer** — low priority; most classic uses are doable with
      depth tricks or FS masks.
- [ ] **Other GLSL built-ins** — `valo_FragCoord`, `valo_FrontFacing`,
      `valo_VertexID`, etc. Same pattern as `valo_PointSize`.

## API / ergonomics

- [ ] **2D / 3D quick-config presets** — a 2D preset = pixel-space orthographic
      projection tracking resize, no depth test/write, alpha blend, no cull;
      3D preset = current defaults. Plus a sprite/quad helper (shared unit quad
      + per-draw `{pos, size, uvRect, tint}` uniforms) for UI-heavy use (UILO).
- [ ] **Per-command sort key** within a layer (back-to-front for transparency,
      by program/texture for state coherence).
- [ ] Move remaining header implementations into `.cpp` files once the
      interfaces settle.

## Deferred / probably not

- **MSAA** — expensive in software; `renderScale` already gives supersampling.
- **Compute** — out of scope; this is a graphics library.
- **GLSL translation / dual-compile shaders** — revisit when the Vulkan
  backend exists and the real constraints are known.

## Later: Vulkan backend

- Abstract `Renderer` behind an interface once the software renderer is
  feature-complete; keep the dev-facing API (`createProgram`, `RenderCommand`,
  layers, IDs) identical. Only shader source changes.

## After all of the above: optimization

- Tiled / multithreaded rasterization (per-tile bins, thread pool).
- Incremental edge functions (step, don't recompute per pixel).
- Vertex cache: shade each unique index once per draw.
- SIMD inner loops (interpolation, blending, sampling).
- Depth-test before shading (early-z) is already the order; keep it.
- Profile with a real scene (needs mesh loading first).
