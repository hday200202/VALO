#include "engines/MinimalEngine.hpp"

int main() {
    me::Engine engine;
    if (!engine.init()) return 1;

    engine.setSkyColor({242.f / 255, 227.f / 255, 217.f / 255, 1.f});
    me::Model& model = engine.addModel("examples/gltf/sea_keep_lonely_watcher/scene.gltf");
    // model.setPosition({0.f, -0.6f, -5.f});
    model.setTransform(valo::Mat4f::scale({0.5f, 0.5f, 0.5f}));

    while (engine.isRunning()) {
        engine.update();
        engine.render();
    }
}
