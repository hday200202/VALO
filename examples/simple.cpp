#include <iostream>

#include "engines/Engine.hpp"

int main(int argc, char** argv) {
    Engine engine;
    Backend backend = Backend::Vulkan;

    for (int i = 0; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--software")) backend = Backend::Software;
    }

    engine.init(std::string("Simple | ") + (backend == Backend::Vulkan ? "Vulkan" : "Software"), {800, 600}, backend);
    auto model = engine.createObject("model");

    engine.loadModel(model, "examples/gltf/raptor/scene.gltf");
    auto pModel = engine.getObject(model);
    pModel->setTransform(pModel->getTransform() * Mat4f::translation({0.f, -1.f, -5.f}) * Mat4f::scale({100.f, 100.f, 100.f}));

    while (engine.isRunning()) {
        engine.update();
        engine.render();
    }
}