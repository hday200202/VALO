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
    ~VALO();

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
    void setLayerClearColor(const LayerID layer, const Color& color);

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

    void updateIndexBuffer(
        const IndexBufferID indices,
        const uint32_t* data,
        const size_t count
    );

    void updateVertexBuffer(
        const VertexBufferID vertices,
        const void* data,
        const size_t count,
        const VtxLayout& newLayout = {}
    );

    template <typename V, typename U, typename Vy>
    ProgramID createProgram(
        Vec4f (*vs)(const V&, const U&, Vy&), 
        Color (*fs)(const Vy&, const U&)
    ) { return m_renderer.createProgram(vs, fs); }

    template <typename V, typename U, typename Vy>
    ProgramID createProgram(Vec4f (*vs)(const V&, const U&, Vy&))
    { return m_renderer.createProgram(vs); }

    void destroyTarget(const RenderTargetID target);
    void destroyVertexBuffer(const VertexBufferID buffer);
    void destroyIndexBuffer(const IndexBufferID buffer);
    void destroyProgram(const ProgramID program);
    void destroyTexture(const TextureID texture);

    void  setRenderScale(const float scale);
    float getRenderScale() const;

    Window* getWindow() const { return m_window; }
    
    void setVsync(const bool vsync);
    bool getVsync() const;

    void pushClip(const RenderCommand& shape, const LayerID layer = BASE_LAYER);
    void popClip(const LayerID layer = BASE_LAYER);

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
    std::vector<RenderCommand> m_clipStack;
    Renderer m_renderer;

    Window* m_window = nullptr;
};

}
