# VALO — Vulkan backend plan

Written 2026-09-27, when the software renderer went feature-complete.

Finish line, from TODO section 4: **`gltf.cpp` runs unchanged on both backends.**
Everything below is ordered so that each phase is independently verifiable and
nothing above `Renderer` ever moves.

---

## The one decision: the shader contract

Everything else in this plan is backend-independent. This is not, and it has to
be settled first because it decides whether "unchanged" in the finish line is
achievable at all.

Today a program is two C++ function pointers. A function pointer cannot become
SPIR-V, so something has to give.

| | Dev writes | Software path | Vulkan path | Cost |
|---|---|---|---|---|
| **A. Both by hand** | C++ *and* GLSL per shader | unchanged | glslang → SPIR-V | every shader twice, they drift; `gltf.cpp` is no longer "unchanged" |
| **B. Dual-compile header** | one file, compiles as C++ *and* GLSL | unchanged | glslang → SPIR-V | a macro layer, and two real friction points (below) |
| **C. SPIR-V only** | GLSL | SPIR-V interpreter | native | deletes the dual-source problem and the typed C++ shader API with it; the interpreter is its own multi-week project |

**B is what your own finish line implies** — it is the only one where the example
file does not change. Its two sharp edges, both known in advance:

1. **Uniform layout.** `Vec3f` is 12 bytes in C++; a `vec3` in a std140 block is
   padded to 16. Shared uniform structs need explicit padding, or std430 plus
   `VK_EXT_scalar_block_layout`. Not hard, but it silently corrupts data if missed.
2. **Texture binding.** Your `texture(TextureID, uv)` takes a `uint32_t` ID.
   GLSL needs a bound `sampler2D`. Needs an ID → descriptor-slot mapping, which
   is a real design step, not a macro.

Neither is a blocker; both are cheaper to face now than mid-backend.

**Recommendation: B, but prove it on one shader before committing.** Take
`minimal.cpp`'s shader, make it compile both ways, and run it through glslang.
An afternoon, and it either works or it tells you to fall back to A.

---

## Phase 0 — prerequisites, no Vulkan yet (~6 h)

These are API-visible rules. Doing them in software first means the Vulkan
backend inherits correct semantics instead of retrofitting them.

**0a. Deferred destruction.** `destroy*` currently frees immediately; Vulkan
cannot free what a submitted frame is still reading. `destroy*` pushes
`(id, frameIndex)`; the resource is released once `framesInFlight` frames retire.
`RenderConfig::framesInFlight` is already declared (Renderer.hpp:33) and used
nowhere, so the frame counter it needs does not exist yet either — add it here.
In software the queue just drains a frame or two late.
*Verify:* destroy a texture mid-frame, keep sampling it, no crash, no ASan report.

**0b. Mutation-while-queued rule.** `update*Buffer` is a memcpy, so writing after
`submit` and before `endFrame` changes what the queued draw reads. Already true
for uniform buffers. Pick one: document "only between frames", or per-frame
copies for anything marked dynamic. Write the choice into the header comment —
it is a dev-facing contract either way.
*Verify:* a harness that mutates a buffer between submit and endFrame and asserts
the documented behaviour.

**0c. Interface split.** `Renderer` (36 public methods) becomes an abstract
`IRenderer` + `SoftwareRenderer`. `VALO`, layers, clip stack, IDs and
`RenderCommand` stay byte-identical. `RenderTarget` becomes backend-owned.
*Verify:* all 9 examples build and run untouched. This is the whole point of the
phase — if any example needs an edit, the split is wrong.

---

## Phase 1 — device bring-up, clears only (~5 h)

Instance, physical-device pick, queues, surface, swapchain, frames-in-flight,
sync primitives. `VulkanRenderer::clear()` and `show()` and nothing else.

Do it **headless first** against SwiftShader (already vendored for the bench):
faster iteration, and validation-layer output is the actual product of this phase.
Validation layers on from the first line of code, not added later.

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

`gltf.cpp` unchanged on both backends, switched by a runtime flag. Then a
backend-agnostic conformance harness: render the same scene on both, diff the
framebuffers, allow a small per-pixel tolerance for filtering and precision.

That diff harness is the thing that keeps the two backends honest afterward.

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
