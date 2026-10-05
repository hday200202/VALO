#pragma once

#include "storage.vert.hpp"

/*
    Four horizontal bands, each one assertion. Green passes, red fails, so the
    result is readable on both backends without a pixel readback path.

      0 (top)    slot 0 holds the float run, and size() sees all of it
      1          summing the first 100 gives what the CPU computed
      2          slot 2 is reachable with 0 and 1 unbound - the case where the
                 Vulkan writeCount and the slot index diverge
      3 (bottom) the struct slot round-trips a 32-byte element
*/
inline void storageFS(const StorageVaryings& in, const StorageUniforms& u) {
    using namespace valo;

    const StorageView<float>    numbers   = storageBuffer<float>(0);
    const StorageView<Particle> particles = storageBuffer<Particle>(2);

    // ndc.y runs -1 at the top to +1 at the bottom
    const int band = int((in.ndc.y * 0.5f + 0.5f) * 4.f);

    bool ok = false;

    if (band <= 0) {
        ok = numbers.size() == u.expectedCount;
    }
    else if (band == 1) {
        float sum = 0.f;
        for (uint32_t i = 0; i < 100u; ++i) sum = sum + numbers[i];

        // each element is i * scale, so the first 100 sum to 4950 * scale
        const float expected = 4950.f * u.scale;
        ok = sum > expected - 0.05f && sum < expected + 0.05f;
    }
    else if (band == 2) {
        ok = particles.size() == 4u;
    }
    else {
        // written as (index, index + 1, 0, 1) with colour (0, 1, 0, 1)
        const Particle p = particles[2];
        ok = p.pos.x > 1.5f && p.pos.x < 2.5f
          && p.pos.y > 2.5f && p.pos.y < 3.5f
          && p.colour.y > 0.5f;
    }

    valo_FragColor = ok ? Color(0.1f, 0.8f, 0.2f, 1.f)
                        : Color(0.9f, 0.1f, 0.1f, 1.f);
}

VALO_FRAGMENT_SHADER(storageFS)
