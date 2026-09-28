#include "engines/MinimalEngine.hpp"

int main() {
    me::Engine engine;
    if (!engine.init()) return 1;

    engine.setSkyColor("#fff0b4");
    me::Model& model = engine.addModel("examples/gltf/tiny_isometric_room/scene.gltf");
    // model.setPosition({0.f, -0.6f, -5.f});
    model.setTransform(valo::Mat4f::scale({0.03f, 0.03f, 0.03f}));

    while (engine.isRunning()) {
        engine.update();
        engine.render();
    }
}
