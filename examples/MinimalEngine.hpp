#pragma once

#include <string>
#include <deque>
#include "../include/VALO.hpp"

namespace me {

using namespace valo;

/*
    yaw/pitch are the authority here: update() rebuilds forward from them every
    frame, so they need real values before the first update and anything that
    wants to aim the camera has to go through setForward() to keep the two in
    step. Leaving them to be assigned only by += in update() read them
    uninitialised on the first mouse grab and threw the view in a random
    direction - hence the defaults on every member below.
*/
struct Camera {
    float pitch = 0.f, yaw = 0.f, roll = 0.f;
    float nearPlane = 0.1f, farPlane = 500.f, fov = 3.14159265f / 2.f;

    Vec3f pos     = {0.f, 0.f,  0.f};
    Vec3f forward = {0.f, 0.f, -1.f};
    Vec3f right   = {1.f, 0.f,  0.f};
    Vec3f up      = {0.f, 1.f,  0.f};

    // inverse of rebuildBasis(), so a caller-supplied direction survives the
    // next mouse move instead of being overwritten by the old yaw/pitch
    void setForward(const Vec3f& dir) {
        const Vec3f f = dir.normalized();
        pitch = std::asin(std::clamp(f.y, -1.f, 1.f));
        yaw   = std::atan2(f.x, -f.z);
        rebuildBasis();
    }

    void update(const Vec2f& mouseDelta, const Vec3f& movement) {
        yaw     += mouseDelta.x * 0.005f;
        pitch   -= mouseDelta.y * 0.005f;
        pitch   = std::clamp(pitch, -1.5f, 1.5f);

        rebuildBasis();

        pos += (forward * movement.z) + (right * movement.x) + (up * movement.y);
    }

    void rebuildBasis() {
        forward = Vec3f{
            std::sin(yaw) * std::cos(pitch), 
            std::sin(pitch), 
            -std::cos(yaw) * std::cos(pitch)
        }.normalized();

        right   = forward.cross(up).normalized();
    }
};

struct Material {
    TextureID baseColorTex          = EMPTY_TEXTURE;
    TextureID emissiveTex           = EMPTY_TEXTURE;
    TextureID metallicRoughnessTex  = EMPTY_TEXTURE;
    TextureID normalTex             = EMPTY_TEXTURE;
    TextureID occlusionTex          = EMPTY_TEXTURE;

    Vec4f     baseColor = {1.f, 1.f, 1.f, 1.f};
    Vec3f     emissive  = {0.f, 0.f, 0.f};
    float     metallic  = 1.f;
};

/*
    Uniforms are memcpy'd into a fixed-size buffer inside RenderCommand, so this
    has to stay a POD - no vectors, no pointers to anything that can move. The
    draw loop walks the nodes and primitives and hands the shader one already
    resolved matrix and one material.
*/
struct Uniforms {
    Mat4f    mvp;
    Material material;
};

struct Varyings { Vec2f uv; };

static inline void vs(const GLTFVertex& in, const Uniforms& u, Varyings& out) {
    out.uv = in.uv;
    valo_Position = u.mvp * Vec4f{ in.pos, 1.f };
}

static inline void fs(const Varyings& in, const Uniforms& u) {
    // texture() indexes the target list unchecked, so never sample EMPTY_TEXTURE
    valo_FragColor = (u.material.baseColorTex == EMPTY_TEXTURE)
        ? u.material.baseColor
        : texture(u.material.baseColorTex, in.uv);
}


class Model {
public:
    Model() = default;

    /*
        The whole glTF file shares one vertex/index buffer, so each primitive is
        just a [firstIndex, indexCount) range into it with its own material.
        One RenderCommand per primitive is what lets each get its own textures.
    */
    void draw(const Mat4f& viewProj) {
        if (!m_valo || !m_loaded) return;

        for (const auto& node : m_nodes) {
            if (node.mesh < 0 || size_t(node.mesh) >= m_meshes.size()) continue;

            const Mat4f mvp = viewProj * m_transform * node.world;

            for (const auto& prim : m_meshes[node.mesh].primitives) {
                RenderCommand cmd;
                cmd.verts       = m_vertices;
                cmd.indices     = m_indices;
                cmd.program     = m_program;
                cmd.firstIndex  = prim.firstIndex;
                cmd.indexCount  = prim.indexCount;
                cmd.blendmode   = BlendMode::Alpha;
                cmd.setUniforms(Uniforms{mvp, materialFor(prim.material)});
                m_valo->submit(cmd);
            }
        }
    }

    bool loadGLTF(const std::string& path) {
        if (!m_valo) return false;

        GLTF gltf = parseGLTF(path);
        if (gltf.vertices.empty() || gltf.indices.empty()) return false;

        m_vertices = m_valo->createVertexBuffer(
            gltf.vertices.data(),
            gltf.vertices.size(),
            GLTFVertex::layout()
        );

        m_indices = m_valo->createIndexBuffer(
            gltf.indices.data(),
            gltf.indices.size()
        );

        m_nodes  = gltf.nodes;
        m_meshes = gltf.meshes;

        // setProgram() before loading wins, otherwise fall back to the built-ins
        if (m_program == DEFAULT_PROGRAM) m_program = m_valo->createProgram(vs, fs);

        m_materials.clear();
        for (auto& m : gltf.materials) {
            Material mat {
                .baseColorTex           = loadTexture(m.baseColorTex),
                .emissiveTex            = loadTexture(m.emissiveTex),
                .metallicRoughnessTex   = loadTexture(m.metallicRoughnessTex),
                .normalTex              = loadTexture(m.normalTex),
                .occlusionTex           = loadTexture(m.occlusionTex),

                .baseColor  = m.baseColor,
                .emissive   = m.emissive,
                .metallic   = m.metallic
            };
            m_materials.push_back(mat);
        }

        return m_loaded = true;
    }

    void setVALO(VALO* valo) { m_valo = valo; }

    /*
        A custom program's shaders still have to take me::Uniforms and
        me::Varyings, since that is what draw() writes into the command.
    */
    void setProgram(const ProgramID program) { m_program = program; }
    ProgramID getProgram() const { return m_program; }

    void setTransform(const Mat4f& transform)   { m_transform = transform; }
    void setPosition(const Vec3f& position)     { m_transform = Mat4f::translation(position); }
    const Mat4f& getTransform() const           { return m_transform; }

    bool isLoaded() const { return m_loaded; }
    const std::vector<Material>& getMaterials() const { return m_materials; }

private:
    TextureID loadTexture(const std::string& path) {
        if (path.empty()) return EMPTY_TEXTURE;
        const TextureID id = m_valo->createTexture(path);
        m_textures.push_back(id);
        return id;
    }

    const Material& materialFor(const int index) const {
        if (index < 0 || size_t(index) >= m_materials.size()) return m_fallback;
        return m_materials[index];
    }

    VertexBufferID                  m_vertices  = EMPTY_BUFFER;
    IndexBufferID                   m_indices   = EMPTY_BUFFER;
    ProgramID                       m_program   = DEFAULT_PROGRAM;

    std::vector<TextureID>          m_textures;
    std::vector<Material>           m_materials;
    std::vector<GLTFNode>           m_nodes;
    std::vector<GLTFMesh>           m_meshes;

    Material                        m_fallback;     // primitives with material == -1
    Mat4f                           m_transform;    // identity until moved

    VALO*                           m_valo = nullptr;
    bool                            m_loaded = false;
};


class Engine {
public:
    Engine() = default;

    bool init(const RenderConfig& cfg = {}) {
        m_running = m_window.create("Minimal Engine", {800, 600}) && m_valo.init(m_window, cfg);
        return m_running;   // the camera defaults itself to the origin looking down -z
    }

    /*
        deque, not vector: addModel hands back a reference the caller keeps in
        order to move the model or swap its program, and a vector would
        invalidate that reference on the next push_back.
    */
    Model& addModel(const std::string& pathToGltf) {
        m_models.emplace_back();
        Model& model = m_models.back();
        model.setVALO(&m_valo);
        model.loadGLTF(pathToGltf);
        return model;
    }

    void update() {
        m_running = m_window.pollEvents();
        if (!m_running) return;
        if (m_window.wasKeyPressed(Key::Escape)) { m_window.close(); m_running = false; }

        const float moveSpeed = 2.f * m_valo.getDeltaTime();
        Vec3f movement = {0.f, 0.f, 0.f};

        if (m_window.isKeyDown(Key::W))         { movement.z =  moveSpeed; }
        if (m_window.isKeyDown(Key::S))         { movement.z = -moveSpeed; }
        if (m_window.isKeyDown(Key::A))         { movement.x = -moveSpeed; }
        if (m_window.isKeyDown(Key::D))         { movement.x =  moveSpeed; }
        if (m_window.isKeyDown(Key::LShift))    { movement.y = -moveSpeed; }
        if (m_window.isKeyDown(Key::Space))     { movement.y =  moveSpeed; }

        bool justGrabbed = false;
        if (m_window.wasMousePressed(MouseButton::Left)) {
            if (m_window.isMouseGrabbed()) m_window.releaseMouse();
            else { m_window.grabMouse(); justGrabbed = true; }
        }

        /*
            The delta has to be read after pollEvents, and thrown away on the
            frame the grab turns on: that frame's motion is the sweep across the
            window on the way to the click, which would snap the view off the
            scene. Relative mode pins the cursor itself, so there is no warp to
            the centre here - warping would only feed its own delta back in.
        */
        if (m_window.isMouseGrabbed())
            m_camera.update(justGrabbed ? Vec2f{0.f, 0.f} : m_window.mouseDelta(), movement);
    }

    void render() {
        if (!m_running) return;

        const Mat4f view = Mat4f::lookAt(m_camera.pos, m_camera.pos + m_camera.forward, m_worldUp);
        const Mat4f proj = Mat4f::perspective(m_camera.fov, m_window.getAspect(), m_camera.nearPlane, m_camera.farPlane);
        const Mat4f viewProj = proj * view;

        m_valo.beginFrame();
        for (auto& m : m_models) m.draw(viewProj);
        m_valo.endFrame();
    }

    void setCameraPosition(const Vec3f& position) { m_camera.pos = position; }
    void setCameraForward(const Vec3f& forward)   { m_camera.setForward(forward); }

    bool isRunning() const { return m_running; }
    VALO& getVALO()     { return m_valo; }
    Window& getWindow() { return m_window; }

private:
    VALO    m_valo;
    Window  m_window;

    Camera  m_camera;

    const Vec3f m_worldUp   = {0.f, 1.f, 0.f};

    bool m_running = false;

    std::deque<Model> m_models;
};

}
