#pragma once

#include "render/Window.hpp"
#include "render/Renderer.hpp"
#include "utils/GLTFParser.hpp"
#include "utils/OBJParser.hpp"
#include "utils/Clock.hpp"
#include <limits>

namespace valo {

using LayerID = uint32_t;


/* ========================================================================= //
    VALO - Vulkan Abstracted Layer Orchestrator
    Features:
// ========================================================================= */
class VALO {
public:
    static const LayerID BASE_LAYER = 0;

    /* ---- lifecycle ------------------------------------------------------ */
    VALO() = default;
    VALO(const VALO&) = delete;
    VALO& operator=(const VALO&) = delete;
    ~VALO();

    bool init(Window& window, const RenderConfig& cfg = {});
    void shutdown();

    /* ---- frame ---------------------------------------------------------- */
    void beginFrame();
    void submit(const RenderCommand& cmd, LayerID layer = 0);
    void endFrame(const RenderTargetID displayTarget = Renderer::BACKBUFFER);

    /* ---- layers --------------------------------------------------------- */
    LayerID addLayer(
        const RenderTargetID target = Renderer::BACKBUFFER,
        const Color& clearColor = {0.f, 0.f, 0.f, 1.f},
        const float clearDepth = 0.f
    );
    void removeLayer(const LayerID layer);
    void setLayerOrder(const std::vector<LayerID>& order);
    void setLayerClearColor(const LayerID layer, const Color& color);
    void setLayerClearDepth(const LayerID layer, const float depth);

    /* ---- clip stack ----------------------------------------------------- */
    void pushClip(const RenderCommand& shape, const LayerID layer = BASE_LAYER);
    void popClip(const LayerID layer = BASE_LAYER);

    /* ---- render targets ------------------------------------------------- */
    RenderTargetID createTarget(
        const uint32_t w = 0,
        const uint32_t h = 0,
        const bool srgb = false
    );
    void destroyTarget(const RenderTargetID target);

    /* ---- vertex buffers ------------------------------------------------- */
    VertexBufferID createVertexBuffer(
        const void* data,
        const size_t count,
        const VtxLayout& layout
    );

    void updateVertexBuffer(
        const VertexBufferID vertices,
        const void* data,
        const size_t count,
        const VtxLayout& newLayout = {}
    );

    void destroyVertexBuffer(const VertexBufferID buffer);

    /* ---- index buffers -------------------------------------------------- */
    IndexBufferID createIndexBuffer(
        const uint32_t* data,
        const size_t count
    );

    void updateIndexBuffer(
        const IndexBufferID indices,
        const uint32_t* data,
        const size_t count
    );

    void destroyIndexBuffer(const IndexBufferID buffer);

    /* ---- uniform buffers ------------------------------------------------ */
    UniformBufferID createUniformBuffer(
        const void* data,
        const size_t bytes
    );

    void updateUniformBuffer(
        const UniformBufferID buffer,
        const void* data,
        const size_t bytes
    );

    void destroyUniformBuffer(
        const UniformBufferID buffer
    );

    /* ---- textures ------------------------------------------------------- */
    /*
        srgb = the file/bytes are sRGB-encoded, as almost every colour image is
        (base colour, emissive, anything authored in an image editor). Data
        textures - normal maps, metal/rough, AO, masks - are NOT sRGB and must
        be left false or they come back wrong.
    */
    TextureID createTexture(
        const uint32_t w, 
        const uint32_t h,
        const uint32_t* data,
        const bool srgb = false,
        const bool mips = true
    );

    TextureID createTexture(
        const std::string& path,
        const bool srgb = false,
        const bool mips = true
    );

    void updateTexture(const TextureID texture, const uint32_t* data);
    void updateTexture(
        const TextureID texture, 
        const uint32_t x, const uint32_t y, 
        const uint32_t w, const uint32_t h, 
        const uint32_t* data
    );

    void destroyTexture(const TextureID texture);

    /* ---- programs ------------------------------------------------------- */
    template <typename V, typename U, typename Vy>
    ProgramID createProgram(
        void (*vs)(const V&, const U&, Vy&), 
        void (*fs)(const Vy&, const U&)
    ) { return m_renderer.createProgram(vs, fs); }

    template <typename V, typename U, typename Vy>
    ProgramID createProgram(void (*vs)(const V&, const U&, Vy&))
    { return m_renderer.createProgram(vs); }

    void destroyProgram(const ProgramID program);

    /* ---- state ---------------------------------------------------------- */
    void resize();

    void  setRenderScale(const float scale);
    float getRenderScale() const;

    void setVsync(const bool vsync);
    bool getVsync() const;

    /* ---- queries -------------------------------------------------------- */
    Window* getWindow() const { return m_window; }
    double getDeltaTime() const;

private:
    RenderConfig m_cfg;
    
    struct Layer {
        std::vector<RenderCommand> cmdQueue;
        RenderTargetID target = Renderer::BACKBUFFER;
        Color clearColor = {0.f, 0.f, 0.f, 1.f};
        float clearDepth = 0.f;   // reversed depth: far plane
        bool dead = false;
    };

    std::vector<Layer> m_layers;
    std::vector<LayerID> m_layerOrder;
    std::vector<RenderCommand> m_clipStack;
    Renderer m_renderer;

    Clock m_dtClock;
    double m_deltaTime = 0.5f;

    Window* m_window = nullptr;

    bool validLayer(const LayerID layer) const {
        return layer < m_layers.size() && !m_layers[layer].dead;
    }
};

}
