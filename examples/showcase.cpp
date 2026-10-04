// PBR showcase - the minimal example with the shading turned all the way up.
//
// Same three lines of setup as minimal.cpp, but the model is lit by a
// procedural sky and four moving lights through a Cook-Torrance metallic/
// roughness BRDF, stood on a glossy plane that mirrors it, and tone mapped
// with an ACES curve. Tangents are derived at load time so the glTF normal
// maps have a frame to work in.
//
// Usage
//   showcase [--vulkan] [--validation] [path/to/model.gltf]
//
//   --validation turns on the Vulkan validation layers. They are off by default
//   because they dominate the frame: 26 fps with, 120 (vsync capped) without.
//
// Controls
//   left click     grab the mouse to look around; right click or click releases
//   wasd           fly, space / shift up and down, hold ctrl to move faster
//   wheel          exposure
//   tab            snap back to the opening shot and resume the auto orbit
//   r              planar reflection on / off
//   f              floor on / off
//   o              light orbs on / off
//   l              freeze / unfreeze the lights
//   1 2 3 4        render scale 100% / 75% / 50% / 25%
//   escape         quit
//
// Every pixel is shaded on the CPU, so the frame is fragment bound: on a 16
// core machine at ~1MP that is 9 fps at full resolution, which is why it opens
// at 65% internal scale. Press 1 for the beauty shot, 3 to fly around freely.

#include <cstdio>
#include <cstring>
#include "engines/ShowcaseEngine.hpp"

int main(int argc, char** argv) {
    bool useVulkan = false, validation = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--vulkan"))     useVulkan  = true;
        if (!std::strcmp(argv[i], "--validation")) validation = true;
    }

    sc::Engine engine;
    if (!engine.init("VALO - PBR Showcase", {1280, 720},
                     useVulkan ? valo::Backend::Vulkan : valo::Backend::Software,
                     validation)) return 1;

    /*
        addModel fits whatever it is handed: scaled uniformly until its longest
        axis is 4 units, centred on x/z, and stood on y = 0 where the floor and
        its mirror are. Files agree on neither - the models here run from 0.06
        units end to end to 4, and most are centred on the origin rather than
        standing on it, which leaves half the geometry under the mirror where it
        reflects back up through itself and reads as a second copy. Everything
        downstream (camera framing, light rig, contact shadow) keys off the
        fitted size, so nothing needs placing by hand.
    */
    const char* path = "examples/gltf/raptor/scene.gltf";
    for (int i = 1; i < argc; ++i)
        if (argv[i][0] != '-') { path = argv[i]; break; }

    sc::Model& model = engine.addModel(path);
    if (!model.isLoaded()) {
        std::fprintf(stderr, "showcase: could not load '%s'\n", path);
        return 1;
    }

    while (engine.isRunning()) {
        engine.update();
        engine.render();
    }
}
