# VALO — Vulkan backend plan

Written 2026-09-27, when the software renderer went feature-complete.

Finish line: **the same app code, the same `RenderCommand` state, and a matching
frame on both backends.** The shader source differs by backend and always will -
see below. Everything here is ordered so each phase is independently verifiable
and nothing above `Renderer` ever moves.

---

## Shader contract: two implementations, one spec

**The software and hardware shaders are separate by design, and stay separate.**
A C++ function pointer cannot become SPIR-V, and pretending otherwise (a
dual-compile macro layer, a shared source file) buys nothing here. `createProgram`
is simply overloaded: C++ callbacks for the software backend, GLSL/SPIR-V for the
Vulkan one. The app picks at creation; nothing downstream cares, because a
`ProgramID` is an opaque integer either way.

This is not a compromise. The software renderer was built first and shaped the
way Vulkan wants, so that the interface would already be the right shape when the
backend arrived. The shaders are the one place the two languages genuinely
diverge, and forcing them together would drag that divergence into every uniform
struct and varying declaration.

**The endgame is a translator: write shaders in C++, emit GLSL.** That is what the
parity work was for, and it is why the duplication is not waste - each hand-written
pair is an input/expected-output example for the translator's test corpus, written
before the compiler, which is the right order.

Parity already in place that the translator will not have to special-case:

- built-ins named to mirror GLSL (`valo_Position`, `valo_FragColor`,
  `valo_FragCoord`, `valo_FrontFacing`, `valo_VertexIndex`, `valo_FragDepth`,
  `valo_PointCoord`, `valo_Discard`) - one `#define` each
- reversed-Z as the sole depth convention, so no conversion layer
- sRGB converted at sample, matching a Vulkan `_SRGB` sampler
- uniform **slots** rather than handles, mapping 1:1 onto descriptor bindings

**The cost of separation is drift**, and the only defence is the conformance diff
harness. Pull it forward from phase 6 to phase 3, the moment a textured quad
renders, so it grows with the backend instead of arriving after it. Keep each
shader pair in sibling files with matching names so a divergence is visible side
by side.

---

## What splits, what stays, what is software-only

The question this plan exists to answer. Short version: **the ID + free-list model
is already the abstraction layer**, so far less needs to become polymorphic than
it looks.

**Only `Renderer` becomes virtual.** `SoftwareRenderer : public Renderer` and
`VulkanRenderer : public Renderer`. Every resource is reached exclusively through
an opaque integer ID - verified: `RenderTarget`, `Program`, `VertexBuffer`,
`IndexBuffer` and `UniformBuffer` never appear in `VALO`'s public signatures. So
they do **not** need a class hierarchy. Each backend stores whatever it likes in
its own vectors: `SoftwareRenderer` keeps `vector<RenderTarget>`,
`VulkanRenderer` keeps `vector<VulkanImage>`. The ID is the abstraction, and this
keeps virtual dispatch off anything per-pixel.

| | Verdict |
|---|---|
| `VALO` (layers, clip stack, queue, ordering) | **shared, unchanged** |
| `RenderCommand` + its enums, `Viewport`, `Scissor` | **shared** - pure state description; every field maps to Vulkan state |
| all `*ID` typedefs, `RenderConfig` | **shared** |
| `math/` (`Vec`, `Mat`, `Quat`), `Color` | **shared** |
| `VtxLayout` / `VtxAttrib` | **shared** - describes a layout, both backends consume it |
| `utils/` (glTF, OBJ, `Clock`, `Timestamp`) | **shared** - no rendering |
| `Window` | **shared**, plus one hook: Vulkan needs a `VkSurfaceKHR` from the same SDL window |
| `Renderer` | **splits** - the one abstract interface |
| `RenderTarget`, `Program`, `VertexBuffer`, `IndexBuffer`, `UniformBuffer` | **backend-private storage**, not subclasses - they never cross the ID boundary |
| `DrawFuncs` (three rasterizers, `ScreenTri`, `ClipVert`) | **software-only**, no Vulkan equivalent |
| `Sampling` (`texture()`, thread-local shader env, `boundTargets`, `varyingDerivs`) | **software-only** - on the GPU these are language built-ins |
| `ThreadPool`, `common/Srgb.hpp` tables | **software-only** |

Every operation means the same thing on both sides; Vulkan just does more of the
work on its side of the call. `createTexture` uploads and builds mips either way -
one memcpys and box-filters, the other stages and blits.

---

## Phase 0 — prerequisites, no Vulkan yet (~6 h)

These are API-visible rules. Doing them in software first means the Vulkan
backend inherits correct semantics instead of retrofitting them.

**0a. Deferred destruction - DONE (2026-09-28).** `destroy*` now only enqueues
`(id, m_frame)`; `destroyAllQueued()` runs from `VALO::beginFrame` and releases
anything that has aged `framesInFlight` frames; `shutdown()` forces an
unconditional drain. `m_frame` increments in `show()`, so
`RenderConfig::framesInFlight` finally does something.

The subtle part: **the free list has to be deferred too, not just the object.**
Keeping the resource alive is not enough if the ID is reusable - a draw submitted
against texture 5, then `destroyTexture(5)`, then a `createTexture` that recycles
ID 5, resolves the queued draw to the *new* texture at `endFrame`. Wrong pixels,
no crash. So the ID only reaches the free list at the moment the resource is
actually released.

Five near-identical loops collapsed into one `retireQueue(queue, freeList,
release, force)` template with a per-type `release` lambda. Two bugs worth
remembering: the validity predicates went in inverted (`!= end()` instead of
`== end()`), which rejected every live resource and made every example render
nothing *silently*, because release builds early-return instead of asserting;
and an early draft cleared the queues unconditionally after the age check, which
dropped un-retired entries without ever freeing them.

*Verified:* destroy then create does not recycle the ID; after `framesInFlight`
frames it does; destroy mid-frame survives; shutdown drains clean.

**0a-bis. `boundUniforms` lifetime.** `Renderer::draw` resolves the eight uniform
slots into a *local* array and stores its address in `detail::boundUniforms`
(Renderer.cpp:93), which outlives the call - GCC flags `-Wdangling-pointer=`.
The batched triangle path is safe (`DrawState::uniforms` is an array, so it
copies the eight heap pointers by value and `flushTriBatch` re-sets the
thread-local per state), but points and lines rasterize immediately and the
stale pointer stays live between calls. Fix: make the array a member rather than
a local. ~5 minutes, and worth doing before the split so it is not carried into
two backends.

**0b. Mutation-while-queued rule - NEXT.** `update*Buffer` is a memcpy, so writing after
`submit` and before `endFrame` changes what the queued draw reads. Already true
for uniform buffers. Pick one: document "only between frames", or per-frame
copies for anything marked dynamic. Write the choice into the header comment —
it is a dev-facing contract either way.
*Verify:* a harness that mutates a buffer between submit and endFrame and asserts
the documented behaviour.

**0c. Interface split.** `Renderer` (~48 public declarations) becomes an abstract
base + `SoftwareRenderer`. `VALO`, layers, clip stack, IDs and `RenderCommand`
stay byte-identical. Resource storage moves into the backend (see the table
above) but stays plain types, not subclasses. `createProgram` is the one method
that is *not* on the abstract base - each backend declares its own overload.
*Verify:* all 9 examples build and run untouched. This is the whole point of the
phase — if any example needs an edit, the split is wrong.

---

## Phase 1 — device bring-up, clears only (~5 h)

Instance, physical-device pick, queues, surface, swapchain, frames-in-flight,
sync primitives. `VulkanRenderer::clear()` and `show()` and nothing else.

**Prerequisites, neither present on the Windows machine as of 2026-09-28:**
SwiftShader is not in `ext/` and has never been committed (it lives on the Mac,
untracked), and there is no Vulkan SDK here - `vulkaninfo` exists from the GPU
driver, but `VULKAN_SDK` is unset and there is no `glslangValidator` / `glslc`,
so GLSL cannot be compiled to SPIR-V and validation layers cannot be enabled.
Install the SDK before starting this phase.

On a machine with a real GPU, headless SwiftShader is optional. Validation layers
are not - on from the first line of code, not added later. They are the actual
product of this phase.

*Verify:* a window that clears to a colour, zero validation warnings, clean
resize, clean shutdown.

## Phase 2 — one triangle (~4 h)

Hardcoded vertex buffer, hardcoded SPIR-V, one pipeline, no state, no uniforms.
This is the phase that proves the render-pass and command-buffer plumbing.

*Verify:* a triangle. Then resize the window fifty times and confirm no leak and
no validation warning.

## Phase 3 — resources (~6 h)

Buffers with staging upload, images + samplers, uniform buffers, descriptor sets.
The ID + free-list model carries over directly — that part of the design already
matches Vulkan, which is why it was built that way.

Mipmaps: you already generate the chain on the CPU. Either upload all levels or
`vkCmdBlitImage` on the GPU; uploading is less code and this is not a hot path.

*Verify:* a textured quad, sampled with each wrap mode, matching the software
backend pixel-for-pixel on a flat sample.

## Phase 4 — pipeline cache and state (~5 h)

Key on `(program, the state fields of RenderCommand, vertex layout)`. Every
`RenderCommand` field maps to real Vulkan state:

| RenderCommand | Vulkan |
|---|---|
| `cullmode`, `winding` | rasterization state |
| `blendmode` | colour blend attachment |
| `depthTest/Write/Compare` | depth-stencil state |
| `stencilTest/Op/Ref` | stencil op state |
| `fillmode` | `polygonMode` (needs `fillModeNonSolid`) |
| `viewport`, `scissor` | `vkCmdSetViewport` / `vkCmdSetScissor`, dynamic |
| `instanceCount` | `vkCmdDrawIndexed` instance count |
| `topology` | input assembly |

Reversed-Z is already the only convention, so `depthCompare = Greater` and clear
depth 0 carry straight over — no conversion layer, which is exactly why making it
the sole convention was worth doing.

*Verify:* `stencil.cpp` and `clipstack.cpp`. They exercise stencil ops and the
nested clip stack, which is the state path's hardest case.

## Phase 5 — targets and layers (~4 h)

Render targets → framebuffers + image views. Layers → render passes, one pass per
distinct target in layer order. `textureDepth()` → sampling a depth image, which
needs a layout transition to `DEPTH_STENCIL_READ_ONLY_OPTIMAL`.

*Verify:* `showcase.cpp` — render-to-texture for the mirrored floor, multiple
layers, depth sampling, all in one scene.

## Phase 6 — the finish line (~4 h)

`gltf.cpp`'s scene on both backends, switched by a runtime flag: same app code,
same `RenderCommand` state, one C++ shader and one GLSL shader implementing the
same spec. Then extend the conformance harness started in phase 3 to the full
scene - diff the framebuffers, small per-pixel tolerance for filtering and
precision.

That harness is what keeps the two shader implementations honest afterwards, and
later becomes the acceptance test for the C++ -> GLSL translator.

---

## Ordering rationale

- Phase 0 before any Vulkan, because both semantic rules are API-visible and
  retrofitting them means touching every example twice.
- Clears before triangles before resources: each phase's failures are then
  unambiguous. A triangle that does not appear in phase 2 is a pipeline problem,
  not a descriptor problem, because descriptors do not exist yet.
- The clip stack (phase 4) before render-to-texture (phase 5), because stencil
  state inside one pass is simpler to debug than state across passes.

## Estimate

~34 hours, at the pace this project has actually moved. A week of evenings for
phases 0–2, another for 3–6. The number that matters is phase 0: if the interface
split needs example edits, stop and fix the split before going further.

## Not in scope

MSAA (deferred — `renderScale` already supersamples), compute, MRT, and the
remaining glTF parser gaps. None of them gate the backend.
