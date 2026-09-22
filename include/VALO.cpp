#include "VALO.hpp"

#include <algorithm>

namespace valo {

VALO::~VALO() { shutdown(); }


bool VALO::init(Window& window, const RenderConfig& cfg) {
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
void VALO::beginFrame() {
    m_deltaTime = m_dtClock.elapsed().asSeconds();
    m_dtClock.reset();
    m_dtClock.start();
    
    if (m_window && m_window->resized()) resize();
    m_clipStack.clear();
    std::vector<RenderTargetID> cleared;

    for (auto& layer : m_layerOrder) {
        const RenderTargetID target = m_layers[layer].target;
        if (std::find(cleared.begin(), cleared.end(), target) != cleared.end()) continue;

        m_renderer.clear(m_layers[layer].clearColor, target);
        cleared.push_back(target);
    }
}


void VALO::submit(
    const RenderCommand& cmd,
    const LayerID layer
) {
    if (m_clipStack.empty()) { m_layers[layer].cmdQueue.push_back(cmd); return; }
    RenderCommand c = cmd;
    c.stencilTest   = StencilTest::Equal;
    c.stencilRef    = uint8_t(m_clipStack.size());
    c.stencilOp     = StencilOp::Keep;
    m_layers[layer].cmdQueue.push_back(c);
}


void VALO::endFrame(const RenderTargetID displayTarget) {
    for (auto& layer : m_layerOrder) {
        auto& currLayer = m_layers[layer];
        for (auto& cmd : currLayer.cmdQueue)
            m_renderer.draw(cmd, currLayer.target);
        currLayer.cmdQueue.clear();
    }

    m_renderer.show(displayTarget);
}


void VALO::setLayerOrder(const std::vector<LayerID>& order) {
    m_layerOrder = order;
}


LayerID VALO::addLayer(
    const RenderTargetID target,
    const Color& clearColor
) {
    m_layers.push_back({{}, target, clearColor});
    LayerID layer = m_layers.size() - 1;
    m_layerOrder.push_back(layer);
    return layer;
}


void VALO::removeLayer(const LayerID layer) {
    m_layers[layer].dead = true;

    auto it = std::find(m_layerOrder.begin(), m_layerOrder.end(), layer);
    if (it != m_layerOrder.end()) m_layerOrder.erase(it);
}


void VALO::setLayerClearColor(const LayerID layer, const Color& color) {
    m_layers[layer].clearColor = clamp(color, 0.f, 1.f);
}

/*
    if no args passed in, target is same size as window.
*/
RenderTargetID VALO::createTarget(
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


VertexBufferID VALO::createVertexBuffer(
    const void* data,
    const size_t count,
    const VtxLayout& layout
) { return m_renderer.createVertexBuffer(data, count, layout); }


IndexBufferID VALO::createIndexBuffer(
    const uint32_t* data,
    const size_t count
) { return m_renderer.createIndexBuffer(data, count); }


TextureID VALO::createTexture(
    const uint32_t w, 
    const uint32_t h,
    uint32_t* data
) { return m_renderer.createTexture(w, h, data); }


void VALO::updateIndexBuffer(
    const IndexBufferID indices,
    const uint32_t* data,
    const size_t count
) { m_renderer.updateIndexBuffer(indices, data, count); }


void VALO::updateVertexBuffer(
    const VertexBufferID vertices,
    const void* data,
    const size_t count,
    const VtxLayout& newLayout
) { m_renderer.updateVertexBuffer(vertices, data, count, newLayout); }


TextureID VALO::createTexture(
    const std::string& path
) { return m_renderer.createTexture(path); }


void VALO::destroyTarget(const RenderTargetID target) 
{ m_renderer.destroyTarget(target); }

void VALO::destroyVertexBuffer(const VertexBufferID buffer) 
{ m_renderer.destroyVertexBuffer(buffer); }

void VALO::destroyIndexBuffer(const IndexBufferID buffer) 
{ m_renderer.destroyIndexBuffer(buffer); }

void VALO::destroyProgram(const ProgramID program) 
{ m_renderer.destroyProgram(program); }

void VALO::destroyTexture(const TextureID texture)
{ m_renderer.destroyTexture(texture); }


void VALO::setRenderScale(const float scale) { 
    m_renderer.setRenderScale(scale);
    if (m_window) resize();
}

float VALO::getRenderScale() const { return m_renderer.getRenderScale(); }


void VALO::resize() {
    if (!m_window) return;
    Vec2u size = m_window->getSize();
    m_renderer.resize(size.x, size.y);
}


void VALO::shutdown() {
    m_renderer.shutdown();
    m_cfg.sdlWindow = nullptr;
    m_window = nullptr;
}

void VALO::setVsync(const bool vsync) {
    m_renderer.setVsync(vsync);
}

bool VALO::getVsync() const {
    return m_renderer.getVsync();
}

void VALO::pushClip(
    const RenderCommand& shape, 
    const LayerID layer
) {
    RenderCommand c = shape;
    c.colorWrite    = false;
    c.depthTest     = false;
    c.depthWrite    = false;
    c.stencilTest   = m_clipStack.empty() ? StencilTest::Always : StencilTest::Equal;
    c.stencilRef    = uint8_t(m_clipStack.size());
    c.stencilOp     = StencilOp::Incr;
    m_layers[layer].cmdQueue.push_back(c);
    m_clipStack.push_back(c);
}

void VALO::popClip(
    const LayerID layer
) {
    if (m_clipStack.empty()) return;
    RenderCommand c = m_clipStack.back();
    c.stencilTest   = StencilTest::Equal;
    c.stencilRef    = uint8_t(m_clipStack.size());
    c.stencilOp     = StencilOp::Decr;
    m_layers[layer].cmdQueue.push_back(c);
    m_clipStack.pop_back();
}

double VALO::getDeltaTime() const {
    return std::min(m_deltaTime, 0.1);
}

}
