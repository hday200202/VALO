#include "MinimalEngine.hpp"

int main() {
    me::Engine engine;
    if (!engine.init()) return 1;

    /*
        The car is ~4.7 units long (z +/- 2.36) and sits on y = 0, so with the
        camera at the origin looking down -z it needs to be far enough out that
        its near end clears the near plane, and dropped so the body is centred.
        Left click grabs the mouse to look around, WASD/Space/Shift to fly.
    */
    me::Model& car = engine.addModel("examples/gltf/cuda/scene.gltf");
    car.setPosition({0.f, -0.6f, -5.f});

    while (engine.isRunning()) {
        engine.update();
        engine.render();
    }
}
