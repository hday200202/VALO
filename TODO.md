# VALO TODO

Ordered by what it takes to bring the software renderer to parity with the
eventual Vulkan backend's dev-facing API. Work top to bottom; each section is
a gate for the next.

## State (2026-09-27)

~5.2k lines of library across 28 files, 9 examples, 18/29 tracked items done
(sections 1, 2 and 6), every feature behind a sanitizer-verified harness. The
dev-facing API (Window -> VALO -> create* -> RenderCommand -> submit, typed C++
shader callbacks) is settled and Vulkan-shaped.

**The software renderer is feature-complete for the backend's purposes.** The
only section-1 item left that is actually API is texture formats; the other
three are glTF parser gaps explicitly marked as not gating the backend. Section
3 (interface split + the shader contract decision) is the real gate to Vulkan.

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
- [ ] **glTF embedded images (`.glb`)** *(parser, not API - does not gate the
      backend; `utils/` only)* — `GLTFParser::texPath()` returns `""`
      unless `image->uri` is set, so every `.glb` loads fully untextured and
      silently: verified on a 29-material car, 15/15 images live in
      `buffer_view`, 0/29 materials got a texture. Same for `.gltf` files with
      `data:` URIs, which currently build a nonsense path out of the base64.
      `cgltf_buffer_view_data()` (cgltf.h:871) + `stbi_load_from_memory` is the
      decode; the structural part is that `GLTFMaterial` stores `std::string`
      paths, which can't name an embedded image. Suggest a
      `std::vector<GLTFImage>` on `GLTF` (decoded pixels + w/h) with materials
      holding `int` indices, so file-backed and embedded land in one place.
      ~1 hour.
- [ ] **glTF alpha modes** *(parser + example, not API)* — `GLTFMaterial` drops
      `alpha_mode` and `alpha_cutoff`, which cgltf provides
      (`cgltf.h:572-573`), so an engine cannot tell a cut-out leaf from a
      transparent light shaft and has to pick one behaviour for the whole
      scene. The three modes need different state:

      | mode | blend | depth write | shader |
      |---|---|---|---|
      | OPAQUE | Opaque | on | no alpha test |
      | MASK | Opaque | on | `discard` below cutoff, then force `a = 1` |
      | BLEND | Alpha | off | alpha passes through |

      BLEND draws must also come after every opaque draw, sorted back to front,
      which is why their depth write is off.

      Symptom without it: forcing alpha to 1 gives correct grass but solid
      light shafts; leaving alpha blended gives correct shafts but a halo of
      background colour around every grass blade, because edge texels with
      alpha 0.5-1.0 composite the sky in. Mipmaps make the halo worse, since
      averaging alpha down the chain produces more partial-alpha texels.

      Needs: the two parser fields, both carried into the material uniform, and
      `Model::draw` split into an opaque pass and a sorted blended pass.

- [ ] **Tangents in `GLTFVertex`** *(parser, not API - does not gate the
      backend; `utils/` only)* — it is `{pos, normal, uv}`, so nothing
      using the shared vertex type can do normal mapping. `gltf.cpp` works
      around it by declaring its own `Vertex` with a `Vec4f tangent` and
      computing the basis at load. That belongs in the parser (MikkTSpace-ish
      per-triangle accumulate) so every consumer gets it. ~2 hours.
- [ ] **Texture formats** — at least R8 (font atlases, masks) and RGBA32F
      (HDR targets). `createTexture` gains a format; `texture()` unpacks per
      format. Vulkan has these for free; software needs them to exist so the
      API doesn't change. ~2 hours.

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

## 3. Backend interface extraction

**See `PLAN.md`** for the phased Vulkan plan these items feed into, and for the
shader-contract decision that gates all of it.

### Semantics the software backend gets away with and Vulkan does not

Both of these are API-visible rules, not implementation details, so they have to
be decided before the backend rather than discovered during it. Neither costs
anything in software - the point is that the same dev code stays correct on both.

- [ ] **Deferred destruction.** `destroy*` frees immediately. Vulkan cannot free
      a buffer or image a submitted frame is still reading. The standard answer
      is a destroy queue: `destroy*` pushes `(id, currentFrame)` and the resource
      is actually released once `framesInFlight` frames have retired. Note that
      `RenderConfig::framesInFlight` is declared (Renderer.hpp) and used nowhere
      today, so the frame counter it needs does not exist yet either. In software
      the queue just drains a frame or two late and nothing else changes.

- [ ] **Mutation while queued.** `updateVertexBuffer` / `updateIndexBuffer` /
      `updateUniformBuffer` are a memcpy, so writing one after `submit` and
      before `endFrame` silently changes what the queued draw reads - already
      true for uniform buffers, and the same hazard for the rest. On Vulkan,
      writing memory the GPU is reading is undefined rather than merely
      surprising. Decide between documenting "only between frames", or
      per-frame copies for anything marked dynamic (what Vulkan apps normally
      do). The choice shows up in the dev-facing contract either way.

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
      Estimate: ~1 week.

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
- `bench/` — **untracked, not in git** (never committed; a fresh clone has no
  `examples/bench`). `vk_helmet.cpp` (headless Vulkan) and `valo_helmet.cpp`, same
  scene/shader/resolution. Run the Vulkan one on SwiftShader with
  `VK_ICD_FILENAMES=$PWD/ext/swiftshader/build/Darwin/vk_swiftshader_icd.json`.

## Deferred / probably not

- **MSAA** — expensive in software; `renderScale` already gives supersampling.
- **Compute** — out of scope; this is a graphics library.
