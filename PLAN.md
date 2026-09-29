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

**0b. Mutation-while-queued rule - DECIDED (2026-09-28), wording deferred.**
`update*Buffer` is a memcpy, so writing after `submit` and before `endFrame`
changes what the queued draw reads.

**Decision: document "only between frames". No per-frame copies.** Rejected
per-frame copies for dynamic buffers - the thing Vulkan apps normally do -
because it is real work that would constrain the backend before the backend
exists, and it can be added later without an API change. Single-consumer library;
mid-frame mutation is against the architecture anyway.

The comment itself lands in the full documentation pass, not now. What it has to
say, and the part worth not re-deriving:

- `updateTexture` and `updateUniformBuffer` are the cases that genuinely misbehave
  today. Both are read during rasterization, which for batched triangles happens
  at `flush()`, so a mid-frame write reaches a draw queued earlier in the *same*
  frame.
- `updateVertexBuffer` and `updateIndexBuffer` are safe as the software backend
  stands: `drawTriangles` reads them at `draw()` time and copies the results into
  `ScreenTri`, so nothing queued still points at them. They are covered by the
  rule for Vulkan forward-compatibility, not because they are broken.

So the contract is deliberately tighter than the software backend needs, which is
the point - it is the Vulkan semantics that matter.

**0c. Interface split — do this AFTER phase R, not before.** `Renderer` (~48
public declarations) becomes an abstract base + `SoftwareRenderer`. `VALO`,
layers, clip stack, IDs and `RenderCommand` stay byte-identical. Resource storage
moves into the backend (see the table above) but stays plain types, not
subclasses. `createProgram` is the one method that is *not* on the abstract base -
each backend declares its own overload.

The ordering matters: this is the one item in phase 0 that is *better informed by
having written Vulkan*. Designing the abstract interface first means guessing at
what the backend will need from it - and a wrong guess here is expensive, because
it is the seam everything else is built against. After the tutorial you will know
what `VulkanRenderer` actually wants, and the split becomes transcription instead
of speculation.

*Verify:* all 9 examples build and run untouched. This is the whole point of the
phase — if any example needs an edit, the split is wrong.

---

## Vulkan concepts, mapped onto things VALO already has

Read this before phase 1. Vulkan's difficulty is mostly vocabulary - about a
dozen objects you must create in the right order before anything draws. Most of
them correspond to something already in this codebase, and the ones that do not
are the ones worth being careful about.

| Vulkan | What it actually is | VALO analogue |
|---|---|---|
| `VkInstance` | the library handle; where layers and extensions are switched on | the SDL setup in `Renderer::init` |
| `VkPhysicalDevice` | one GPU present on the machine, queried for capabilities | none - there is only the CPU |
| `VkDevice` | your open connection to that GPU; nearly every call takes it | none |
| `VkQueue` | where recorded work is submitted for execution | `ThreadPool` - the thing that actually runs the work |
| `VkCommandBuffer` | a recorded list of draw calls, replayed on submit | `Layer::cmdQueue`, a `vector<RenderCommand>` |
| `VkSwapchain` | the rotating set of images the window presents from | `m_sdlTexture` + `show()` |
| `VkImage` / `VkImageView` | GPU-side pixels, and a typed window onto them | `RenderTarget` |
| `VkFramebuffer` / dynamic rendering | binds actual images as the attachments a draw writes | a `Layer`'s `target` |
| `VkPipeline` | every fixed-function state field **plus** the shaders, baked into one immutable object | `RenderCommand`'s state fields + `Program` |
| `VkDescriptorSet` | the table of buffers and textures the shader reads through | `cmd.uniformBuffers[8]` and the texture IDs a shader samples |
| `VkFence`, `VkSemaphore` | CPU-waits-for-GPU, and GPU-waits-for-GPU | `ThreadPool`'s generation counter and active-worker count |
| staging buffer | CPU-visible scratch memory you copy *through* to reach GPU-only memory | the `assign()` straight into `m_vertexBuffers` |
| **image layout** | what the GPU is currently permitted to do with an image; changed by explicit barriers | **nothing** - a `RenderTarget` is just memory, always readable |

Two entries deserve attention because they have no software counterpart:

**`VkPipeline` is immutable.** In VALO, `RenderCommand` carries its state and the
rasterizer branches on it per draw. Vulkan bakes that state into an object at
creation. Changing `blendmode` means a *different pipeline*, which is why phase 4
is a cache keyed on the state rather than a set of setters.

**Image layouts are the concept with no analogue**, and the one that produces the
most confusing early bugs - an image written as a render target and then sampled
as a texture has to be transitioned in between, and forgetting produces garbage
or a validation error rather than a crash. `showcase.cpp`'s mirrored floor and
`textureDepth()` are exactly this case, which is why they are phase 5 and not
earlier.

---

## Three decisions that materially reduce the work

Take all three. Each removes a category of problem rather than deferring it.

**1. Use dynamic rendering, not render passes.** `VK_KHR_dynamic_rendering` (core
in Vulkan 1.3) lets you begin rendering by naming the attachments inline and
deletes `VkRenderPass`, `VkFramebuffer` and subpass dependencies entirely. Those
three are a large share of the classic tutorial's difficulty and buy nothing on
desktop. Your layer model maps onto it directly: begin rendering, submit the
layer's draws, end rendering.

**2. Use VMA (Vulkan Memory Allocator).** Hand-rolling `vkAllocateMemory` means
memory type selection, heap budgets, alignment and suballocation - days of work
solving a problem that is not yours. VMA is a single vendored header, matching
the way `stb`/`cgltf` are already handled in `build.sh`.

**3. Validation layers on from the first line.** Vulkan silently accepts almost
any misuse and gives undefined results. The layers turn that into an explicit
message naming the struct field. Without them you debug blind; with them most
bugs are a readable sentence. Already vendored - see phase 1's prerequisites.

---

## Phase R — the reference shelf, and the parts the triangle skipped (~8 h)

**Starting point as of 2026-09-28: the triangle is done.** So instance, physical
device, logical device, queues, swapchain, a pipeline, command buffers and the
frame loop have all been typed out once. That is the hard scaffolding, and it is
exactly what makes phases 1 and 2 transcription rather than exploration.

**No one writes Vulkan from memory.** The API has hundreds of entry points and
structs where three of twenty fields matter. Working with references open is the
normal way this is done, so the point of this phase is to have the right ones at
hand, not to memorise anything.

### The shelf

| | For |
|---|---|
| <https://vkguide.dev> | **The main one.** Teaches dynamic rendering + VMA + descriptor abstractions, which is exactly the stack this plan picked. Modern where vulkan-tutorial is dated. |
| <https://vulkan-tutorial.com> | The chapters below, and for anything vkguide moves past too quickly. Render-pass-based, so read it knowing you are skipping that. |
| <https://github.com/SaschaWillems/Vulkan> | One focused, runnable sample per feature. The right first stop for "how do I do X" once the basics are in. |
| <https://docs.vulkan.org/spec/latest/> | Authoritative, searchable. Validation messages link straight into it - follow those links, they are the fastest way to learn a VUID. |
| The validation layers | The primary debugging tool, not a safety net. Read the message, follow the VUID link. |

### The parts the triangle did not cover

The triangle stops right before every genuinely new concept. These four, in order,
are the whole gap between what you have and what phase 3 needs:

1. **Vertex buffers** - and with them staging: CPU-visible scratch memory copied
   into GPU-only memory, because the fast heap is not mappable.
2. **Uniform buffers and descriptor sets** - the binding-table model. This is the
   biggest single idea, and the one your uniform *slots* were already shaped for.
3. **Texture mapping** - images, image views, samplers, and the first real
   **layout transitions**.
4. **Depth buffering** - a depth attachment, which under reversed-Z you already
   understand better than the tutorial explains it.

Work these in the throwaway project, not in VALO, for the same reason as before:
when something does not draw, you want exactly one candidate for what is wrong.
Substitute dynamic rendering for the render-pass chapters and VMA for the
hand-rolled allocator, so the habits match the backend you are about to write.

**Estimate:** ~8 h, and unlike the earlier draft of this document it *is* partly
compressible, because the scaffolding is already familiar. The two that will take
the longest are descriptor sets and layout transitions - they have no software
analogue, so there is nothing in VALO to reason from.

*Verify:* a textured, depth-tested quad with a uniform-buffer transform, running
validation-clean. That is the exact feature set phase 3 has to reproduce behind
the interface, which is why it is the stopping point.

---

## Phase 1 — device bring-up, clears only (~6 h)

**Step-by-step working doc: `VULKAN_SETUP.md`.** Nine steps from the instance to
a clearing window, with the per-step traps and verifications. The rest of this
section is the why; that file is the how.

Instance, physical-device pick, queues, surface, swapchain, frames-in-flight,
sync primitives. `VulkanRenderer::clear()` and `show()` and nothing else.

**Progress 2026-09-29: steps 1-3 done** (instance, debug messenger, surface,
physical device, queue families, logical device, queues) - all verified with
validation on and zero output. Next is the swapchain, then sync, then the frame
loop. Write swapchain creation as its own re-callable function from the start:
resize needs to run it twice, and retrofitting that means unpicking the frame
loop.

**0c is the wall.** The swapchain can live in `VulkanContext`, but
`VulkanRenderer::clear()`/`show()` needs a `Renderer` base class to inherit
from - so the interface split lands before those, while the Vulkan side is
still one file.

The triangle already covered all of this. It is transcription: the same objects
in the same order, now behind your own interface instead of in `main()`.

**Prerequisites: handled, as of 2026-09-28.** `./build.sh` now vendors the whole
LunarG SDK into `ext/vulkan` - loader, validation layers, `glslc`,
`glslangValidator`, headers, and MoltenVK on macOS - and writes
`ext/vulkan/env.sh` to point the loader at the vendored ICD and layers. Nothing
is installed system-wide, and a fresh clone needs only a compiler, cmake and
curl. `ext/vma/vk_mem_alloc.h` comes down with it.

Source `ext/vulkan/env.sh` before running anything that touches Vulkan; without
it the loader searches system paths only and finds neither the ICD nor the
layers.

SwiftShader is still Mac-only and untracked, but it is optional - it is a CPU
reference for the conformance diff, not a requirement for bring-up.

*Verify:* a window that clears to a colour, zero validation warnings, clean
resize, clean shutdown.

**The trap here is resize.** The swapchain becomes invalid when the window
changes size, and `vkAcquireNextImageKHR` reports it with a return code most
people ignore on the first pass. Handle `VK_ERROR_OUT_OF_DATE_KHR` and
`VK_SUBOPTIMAL_KHR` properly now; retrofitting it later means unpicking the frame
loop. `Window::resized()` already exists and `VALO::resize()` already calls
through, so the hook is in place.

## Phase 2 — one triangle (~4 h)

Hardcoded vertex buffer, hardcoded SPIR-V, one pipeline, no state, no uniforms.
This is the phase that proves the command-buffer and submit plumbing.

**Check the Y axis here, at the one point where it is trivial to see.** Vulkan's
clip space has +Y pointing *down*, opposite to OpenGL. `Mat4f::perspective` was
written for Vulkan conventions, but "written for" is not "verified against" -
confirm with an asymmetric triangle whose top and bottom differ, not a symmetric
one that looks identical either way. Getting this wrong shows up much later as
everything being subtly mirrored.

Depth needs no such check: Vulkan's depth range is natively `[0,1]`, and reversed-Z
already puts you there.

*Verify:* a triangle, right way up. Then resize the window fifty times and
confirm no leak and no validation warning.

## Phase 3 — resources (~8 h)

Buffers with staging upload, images + samplers, uniform buffers, descriptor sets.
The ID + free-list model carries over directly - that part of the design already
matches Vulkan, which is why it was built that way.

Descriptor sets are the genuinely new idea: a shader does not take a texture ID,
it reads through a binding slot that you populate ahead of the draw. Your uniform
**slots** already have this shape, which is why slot-binding beat handle-indexing
back when that was decided. Textures need the same treatment - an ID → binding
mapping, built per draw.

Mipmaps: you already generate the chain on the CPU. Either upload all levels or
`vkCmdBlitImage` on the GPU; uploading is less code and this is not a hot path.

**Uniform padding is the trap.** A `Vec3f` is 12 bytes in C++; a `vec3` in a
std140 block is padded to 16. A struct that works in the software backend will
read shifted garbage on the GPU. Either pad the shared structs explicitly, or
enable `VK_EXT_scalar_block_layout` and use `scalar` layout so the rules match C++.
Decide once, write it in the header next to `setUniforms`.

*Verify:* a textured quad, sampled with each wrap mode, matching the software
backend pixel-for-pixel on a flat sample. **Start the conformance diff harness
here** - the shader-contract section explains why it moves up from phase 6.

## Phase 4 — pipeline cache and state (~5 h)

Key on `(program, the state fields of RenderCommand, vertex layout)`. Every
`RenderCommand` field maps to real Vulkan state:

| RenderCommand | Vulkan |
|---|---|
| `cullmode`, `winding` | rasterization state |
| `blendmode` | colour blend attachment |
| `depthTest/Write/Compare` | depth-stencil state |
| `stencilTest/Op/Ref` | stencil op state |
| `fillmode` | `polygonMode` (needs the `fillModeNonSolid` device feature) |
| `viewport`, `scissor` | `vkCmdSetViewport` / `vkCmdSetScissor`, dynamic |
| `instanceCount` | `vkCmdDrawIndexed` instance count |
| `topology` | input assembly |

Viewport and scissor are *dynamic* state - set per draw without a new pipeline,
which is exactly why they went on `RenderCommand` rather than becoming bound
state on `Renderer`. Everything else in that table bakes into the pipeline, so it
belongs in the cache key.

Reversed-Z is already the only convention, so `depthCompare = Greater` and clear
depth 0 carry straight over - no conversion layer, which is exactly why making it
the sole convention was worth doing.

*Verify:* `stencil.cpp` and `clipstack.cpp`. They exercise stencil ops and the
nested clip stack, which is the state path's hardest case.

## Phase 5 — targets and layers (~6 h)

Render targets → images + image views. Layers → one dynamic-rendering block per
distinct target, in layer order. `textureDepth()` → sampling a depth image.

**This is the image-layout phase.** A target written by one layer and sampled by
the next has to be transitioned from `COLOR_ATTACHMENT_OPTIMAL` to
`SHADER_READ_ONLY_OPTIMAL` in between, and a depth image to
`DEPTH_STENCIL_READ_ONLY_OPTIMAL`. Nothing in the software backend prepares you
for this, and the failure mode is a validation error or garbage pixels rather
than a crash. Budget accordingly - the estimate above already does.

*Verify:* `showcase.cpp` - render-to-texture for the mirrored floor, multiple
layers, depth sampling, all in one scene.

## Phase 6 — the finish line (~4 h)

`gltf.cpp`'s scene on both backends, switched by a runtime flag: same app code,
same `RenderCommand` state, one C++ shader and one GLSL shader implementing the
same spec. Then extend the conformance harness started in phase 3 to the full
scene - diff the framebuffers, small per-pixel tolerance for filtering and
precision.

That harness is what keeps the two shader implementations honest afterwards, and
later becomes the acceptance test for the C++ → GLSL translator.

---

## Ordering rationale

- Phase R outside the repo, because working out descriptor sets and designing the
  interface split at the same time means every bug has two possible homes.
- **0a and 0b before phase R; 0c after it.** The two semantic rules are
  API-visible and retrofitting them means touching every example twice, so they
  come first. The interface split is the opposite case: it is the seam everything
  else gets built against, and it is far better designed by someone who has
  already written a Vulkan backend once.
- Clears before triangles before resources: each phase's failures are then
  unambiguous. A triangle that does not appear in phase 2 is a pipeline problem,
  not a descriptor problem, because descriptors do not exist yet.
- The clip stack (phase 4) before render-to-texture (phase 5), because stencil
  state inside one pass is simpler to debug than image layouts across passes.

## Estimate

| | |
|---|---|
| Phase R (references + the 4 skipped chapters, outside the repo) | ~8 h |
| Phases 0–6 (the backend itself) | ~37 h |

This is the third estimate in this document's history and worth recording why.
The first costed the backend as if Vulkan were already familiar. The second
overrode that with a from-scratch 15–25 h learning phase - also wrong, because the
triangle was already done. The real gap is four tutorial chapters, and the
expensive ones are descriptor sets and layout transitions, because they are the
two ideas with no software analogue to reason from.

Everything after phase R moves at this project's normal pace, because by then it
is ordinary work in a codebase you know, with references open.

The number that matters is still phase 0: if the interface split needs example
edits, stop and fix the split before going further.

## Not in scope

MSAA (deferred - `renderScale` already supersamples), compute, MRT, and the
remaining glTF parser gaps. None of them gate the backend.
