#include "engines/MinimalEngine.hpp"
#include <cstring>

int main(int argc, char** argv) {
    valo::RenderConfig cfg;

    // the engine picks the matching shader half from this; --vulkan has no
    // textures yet, so everything renders in its material's base colour
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "--vulkan")) cfg.backend = valo::Backend::Vulkan;

    me::Engine engine;
    if (!engine.init(cfg)) return 1;

    engine.setSkyColor("#fff0b4");
    me::Model& model = engine.addModel("examples/gltf/sea_keep_lonely_watcher/scene.gltf");
    // model.setPosition({0.f, -0.6f, -5.f});
    model.setTransform(valo::Mat4f::scale({0.03f, 0.03f, 0.03f}));

    while (engine.isRunning()) {
        engine.update();
        engine.render();
    }
}
