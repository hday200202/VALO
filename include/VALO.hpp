#pragma once

#include "render/Window.hpp"
#include "render/Renderer.hpp"

namespace valo {

using LayerID = uint32_t;

class VALO {
public:
    VALO() = default;
    VALO(const VALO&) = delete;
    VALO& operator=(const VALO&) = delete;
    ~VALO() { shutdown(); }

    bool init(Window& window, const RenderConfig& cfg = {});

    void beginFrame();
    void submit(const RenderCommand& cmd, LayerID layer = 0);
    void endFrame(const RenderTargetID displayTarget = Renderer::BACKBUFFER);

    void setLayerOrder(const std::vector<LayerID>& order);
    LayerID addLayer(
        const RenderTargetID target = Renderer::BACKBUFFER,
        const Color& clearColor = {0.f, 0.f, 0.f, 1.f}
    );
    void removeLayer(const LayerID layer);
    void setLayerClearColor(const LayerID layer, const Color& color) {
        Color clearColor = clamp(color, 0.f, 1.f);
        m_layers[layer].clearColor = clearColor;
    }

    // if no args passed in, target is same size as window.
    RenderTargetID createTarget(
        const uint32_t w = 0,
        const uint32_t h = 0
    );

    VertexBufferID createVertexBuffer(
        const void* data,
        const size_t count,
        const VtxLayout& layout
    );

    IndexBufferID createIndexBuffer(
        const uint32_t* data,
        const size_t count
    );

    TextureID createTexture(
        const uint32_t w, 
        const uint32_t h,
        uint32_t* data
    );


    TextureID createTexture(
        const std::string& path
    );

    template <typename V, typename U, typename Vy>
    ProgramID createProgram(
        Vec4f (*vs)(const V&, const U&, Vy&), 
        Color (*fs)(const Vy&, const U&)
    ) { return m_renderer.createProgram(vs, fs); }

    void destroyTarget(const RenderTargetID target);
    void destroyVertexBuffer(const VertexBufferID buffer);
    void destroyIndexBuffer(const IndexBufferID buffer);
    void destroyProgram(const ProgramID program);
    void destroyTexture(const TextureID texture);

    void setRenderScale(const float scale) { 
        m_renderer.setRenderScale(scale);
        if (m_window) resize();
    }
    float getRenderScale() const { return m_renderer.getRenderScale(); }

    void resize();
    void shutdown();

    static const LayerID BASE_LAYER = 0;

private:
    RenderConfig m_cfg;
    
    struct Layer {
        std::vector<RenderCommand> cmdQueue;
        RenderTargetID target = Renderer::BACKBUFFER;
        Color clearColor = {0.f, 0.f, 0.f, 1.f};
        bool dead = false;
    };

    std::vector<Layer> m_layers;
    std::vector<LayerID> m_layerOrder;
    Renderer m_renderer;

    Window* m_window = nullptr;
};



inline bool VALO::init(Window& window, const RenderConfig& cfg) {
    if (!window.isOpen()) return false;

    m_window = &window;
    m_cfg = cfg;
    m_cfg.sdlWindow = window.sdlHandle();

    m_layers.clear();
    m_layerOrder.clear();

    bool initialized = m_renderer.init(m_cfg);
    m_layers.push_back(Layer()); // default layer, renders to backbuffer
    m_layerOrder.push_back(0);

    return initialized;
}


// clears each target once, using the clear color of the first layer (in draw order) that uses it
inline void VALO::beginFrame() {
    if (m_window && m_window->resized()) resize();

    std::vector<RenderTargetID> cleared;

    for (auto& layer : m_layerOrder) {
        const RenderTargetID target = m_layers[layer].target;
        if (std::find(cleared.begin(), cleared.end(), target) != cleared.end()) continue;

        m_renderer.clear(m_layers[layer].clearColor, target);
        cleared.push_back(target);
    }
}


inline void VALO::submit(
    const RenderCommand& cmd,
    const LayerID layer
) { m_layers[layer].cmdQueue.push_back(cmd); }


inline void VALO::endFrame(const RenderTargetID displayTarget) {
    for (auto& layer : m_layerOrder) {
        auto& currLayer = m_layers[layer];
        for (auto& cmd : currLayer.cmdQueue)
            m_renderer.draw(cmd, currLayer.target);
        currLayer.cmdQueue.clear();
    }

    m_renderer.show(displayTarget);
}


inline void VALO::setLayerOrder(const std::vector<LayerID>& order) {
    m_layerOrder = order;
}

inline LayerID VALO::addLayer(
    const RenderTargetID target,
    const Color& clearColor
) {
    m_layers.push_back({{}, target, clearColor});
    LayerID layer = m_layers.size() - 1;
    m_layerOrder.push_back(layer);
    return layer;
}


inline void VALO::removeLayer(const LayerID layer) {
    m_layers[layer].dead = true;

    auto it = std::find(m_layerOrder.begin(), m_layerOrder.end(), layer);
    if (it != m_layerOrder.end()) m_layerOrder.erase(it);
}


inline RenderTargetID VALO::createTarget(
    const uint32_t w, 
    const uint32_t h
) {
    uint32_t width = w; uint32_t height = h;
    bool tracksWindow = false;

    if (w == 0 || h == 0) {
        width = m_renderer.width();
        height = m_renderer.height();
        tracksWindow = true;
    }

    return m_renderer.createTarget(width, height, tracksWindow);
}


inline VertexBufferID VALO::createVertexBuffer(
    const void* data,
    const size_t count,
    const VtxLayout& layout
) { return m_renderer.createVertexBuffer(data, count, layout); }


inline IndexBufferID VALO::createIndexBuffer(
    const uint32_t* data,
    const size_t count
) { return m_renderer.createIndexBuffer(data, count); }


inline TextureID VALO::createTexture(
    const uint32_t w, 
    const uint32_t h,
    uint32_t* data
) { return m_renderer.createTexture(w, h, data); }


inline TextureID VALO::createTexture(
    const std::string& path
) { return m_renderer.createTexture(path); }


inline void VALO::destroyTarget(const RenderTargetID target) 
{ m_renderer.destroyTarget(target); }


inline void VALO::destroyVertexBuffer(const VertexBufferID buffer) 
{ m_renderer.destroyVertexBuffer(buffer); }


inline void VALO::destroyIndexBuffer(const IndexBufferID buffer) 
{ m_renderer.destroyIndexBuffer(buffer); }

inline void VALO::destroyProgram(const ProgramID program) 
{ m_renderer.destroyProgram(program); }

inline void VALO::destroyTexture(const TextureID texture)
{ m_renderer.destroyTexture(texture); }


inline void VALO::resize() {
    if (!m_window) return;
    Vec2u size = m_window->getSize();
    m_renderer.resize(size.x, size.y);
}


inline void VALO::shutdown() {
    m_renderer.shutdown();
    m_cfg.sdlWindow = nullptr;
    m_window = nullptr;
}

}