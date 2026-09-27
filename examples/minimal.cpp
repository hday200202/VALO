#include "engines/MinimalEngine.hpp"

int main() {
    me::Engine engine;
    if (!engine.init()) return 1;

    engine.setSkyColor("#fff0b4");
    me::Model& model = engine.addModel("examples/gltf/trees/scene.gltf");
    // model.setPosition({0.f, -0.6f, -5.f});
    model.setTransform(valo::Mat4f::scale({0.1f, 0.1f, 0.1f}));

    while (engine.isRunning()) {
        engine.update();
        engine.render();
    }
}
