#pragma once

#include <string>
#include <optional>
#include <numbers>
#include "../include/VALO.hpp"
#include "../shaders/minimal.vert.hpp"
#include "../shaders/minimal.frag.hpp"

using namespace valo;

using ObjectID = uint32_t;

#define MAX_OBJECTS 512

class Object {
public:
    Object() = default;

    const Mat4f& getTransform() const { return m_transform; }
    Object& setTransform(const Mat4f& transform) { m_transform = transform; return *this; }

    const Model& getModel() const { return m_model; }
    Object& setModel(const Model& model) { m_model = std::move(model); return *this; }

    const std::string& getName() const { return m_name; }
    Object& setName(const std::string& name) { m_name = name; return *this; }

private:
    std::string m_name = "";
    Mat4f       m_transform;
    Model       m_model;

    ProgramID   m_program   = DEFAULT_PROGRAM;

    bool        m_valid     = false;

    Object& setValid(const bool valid) { m_valid = valid; return *this; }

    void draw(VALO* valo, const Mat4f& viewProj, bool wireframe = false) {
        if (!m_model.loaded()) return;

        if (m_program == DEFAULT_PROGRAM) 
            m_program = valo->createProgram(
                "examples/shaders/minimal.vert.hpp", 
                "examples/shaders/minimal.frag.hpp"
            );

        for (const ModelDraw& d : m_model.draws) {
            RenderCommand cmd = d.cmd;
            cmd.program  = m_program;
            cmd.fillmode = wireframe ? FillMode::Wireframe : FillMode::Solid;
            cmd.setUniforms(me::Uniforms{
                viewProj * m_transform * d.world,
                m_model.materials[d.material]
            });
            valo->submit(cmd);
        }
    }

    bool load(VALO* valo, const std::string& path) {
        m_model = loadModel(*valo, path);
        if (!m_model.loaded()) return false;
        return true;
    }

    friend class Engine;
};



class Camera {
public:
    Camera() = default;

    Mat4f getView(const Vec3f& up) const { return Mat4f::lookAt(m_pos, m_pos + m_forward, up); }

    Mat4f getProj(const float aspect) const {
        return Mat4f::perspective(m_fov, aspect, m_nearPlane, m_farPlane);
    }

    Mat4f getProjView(const Vec3f& up, const float aspect) const {
        return getProj(aspect) * getView(up);
    }

    Camera& setPosition(const Vec3f& pos) { m_pos = pos; return *this; }
    const Vec3f& getPosition() const { return m_pos; }

    Camera& setForward(const Vec3f& dir) {
        const Vec3f f = dir.normalized();
        m_pitch = std::asin(std::clamp(f.y, -1.f, 1.f));
        m_yaw   = std::atan2(f.x, -f.z);
        rebuildBasis();
        return *this;
    }

    const Vec3f& getForward() const { return m_forward; }
    const Vec3f& getRight()   const { return m_right; }

    Camera& setFov(const float fov) { m_fov = fov; return *this; }
    float getFov() const { return m_fov; }

    Camera& setClipPlanes(const float nearPlane, const float farPlane) {
        m_nearPlane = nearPlane;
        m_farPlane  = farPlane;
        return *this;
    }
    float getNearPlane() const { return m_nearPlane; }
    float getFarPlane()  const { return m_farPlane; }

    void update(const Vec2f& mouseDelta, const Vec3f& movement) {
        m_yaw   += mouseDelta.x * 0.005f;
        m_pitch -= mouseDelta.y * 0.005f;
        m_pitch  = std::clamp(m_pitch, -1.5f, 1.5f);

        rebuildBasis();

        m_pos += (m_forward * movement.z) + (m_right * movement.x) + (m_up * movement.y);
    }

private:
    void rebuildBasis() {
        m_forward = Vec3f{
            std::sin(m_yaw) * std::cos(m_pitch),
            std::sin(m_pitch),
            -std::cos(m_yaw) * std::cos(m_pitch)
        }.normalized();

        m_right = m_forward.cross(m_up).normalized();
    }

    float m_pitch = 0.f, m_yaw = 0.f, m_roll = 0.f;
    float m_nearPlane = 0.1f, m_farPlane = 1000.f, m_fov = std::numbers::pi_v<float> / 2.f;

    Vec3f m_pos     = {0.f, 0.f,  0.f};
    Vec3f m_forward = {0.f, 0.f, -1.f};
    Vec3f m_right   = {1.f, 0.f,  0.f};
    Vec3f m_up      = {0.f, 1.f,  0.f};
};



class Engine {
public:
    Engine() = default;

    bool init(const std::string& title, const Vec2u& size, const Backend backend) {
        WinFlags flags = 
            WinFlags::HiDPI     | 
            WinFlags::Resize    | 
            (backend == Backend::Vulkan ? WinFlags::Vulkan : WinFlags::None);

        RenderConfig cfg;
        cfg.backend = backend;
        cfg.window = &m_window;

        m_running = (m_window.create(title, size, flags) && m_valo.init(m_window, cfg));
        m_valo.setVsync(true);

        return m_running;
    }

    void update() {
        if (!(m_running = m_window.pollEvents())) return;

        float moveSpeed = 2.f * m_valo.getDeltaTime();
        Vec3f movement = {0.f, 0.f, 0.f};

        if (m_window.wasMousePressed(MouseButton::Left)) m_window.isMouseGrabbed() ? m_window.releaseMouse() : m_window.grabMouse();

        if (m_window.isMouseGrabbed()) {
            if (m_window.isKeyDown(Key::W)) movement.z += moveSpeed;
            if (m_window.isKeyDown(Key::S)) movement.z += -moveSpeed;
            if (m_window.isKeyDown(Key::A)) movement.x += -moveSpeed;
            if (m_window.isKeyDown(Key::D)) movement.x += moveSpeed;
            if (m_window.isKeyDown(Key::Space)) movement.y += moveSpeed;
            if (m_window.isKeyDown(Key::LShift)) movement.y += -moveSpeed;
            if (m_window.wasKeyPressed(Key::Tab)) m_wireframe = !m_wireframe;

            if (m_window.wasKeyPressed(Key::V)) m_valo.setVsync(!m_valo.getVsync());

            m_camera.update(m_window.mouseDelta(), movement);
            m_window.setMousePositionLocal(m_window.getCenterLocal());
        }
    }

    void render() {
        if (!m_running) return;

        m_valo.setLayerClearColor(VALO::BASE_LAYER, {1.f, 1.f, 1.f, 1.f});
        m_valo.beginFrame();

        const Mat4f projView = m_camera.getProjView({0.f, 1.f, 0.f}, m_window.getAspect());
        for (ObjectID id = 0; id <= m_lastObjectID; ++id) {
            auto& obj = m_objects[id];
            if (!obj.m_valid || !inFrustum(obj)) continue;
            obj.draw(&m_valo, projView, m_wireframe);
        }

        m_valo.endFrame();
    }

    ObjectID createObject(const std::string& name = "") {
        ObjectID id = 0;
        std::string newName = name;

        if (!m_freeObjects.empty()) {
            id = m_freeObjects.front();
            if (name.empty()) newName = "object_" + std::to_string(id);
            m_freeObjects.pop_front();
            m_objects[id] = Object().setName(newName).setValid(true);
            return id;
        }

        if (name.empty()) newName = "object_" + std::to_string(++m_lastObjectID);
        m_objects[m_lastObjectID] = Object().setName(newName).setValid(true);
        return m_lastObjectID;
    }

    void destroyObject(const ObjectID object) {
        if (object > m_lastObjectID) return;
        m_objects[object] = Object().setValid(false);
        m_freeObjects.push_back(object);
    }

    Object* getObject(const ObjectID object) {
        if (object > m_lastObjectID) return nullptr;
        if (!m_objects[object].m_valid) return nullptr;
        return &m_objects[object];
    }

    bool loadModel(const ObjectID object, const std::string& path) {
        if (object > m_lastObjectID) return false;
        if (!m_objects[object].m_valid) return false;
        return m_objects[object].load(&m_valo, path);
    }

    const bool isRunning() const { return m_running; }

private:
    Window                  m_window;
    VALO                    m_valo;

    Object                  m_objects[MAX_OBJECTS]  = {{}};
    ObjectID                m_lastObjectID          = 0;
    std::deque<ObjectID>    m_freeObjects;

    Camera                  m_camera;

    bool                    m_running               = false;
    bool                    m_wireframe             = false;

    bool inFrustum(const Object& obj) const {
        static bool printed = false;
        const Bounds world = Bounds{obj.m_model.boundsMin, obj.m_model.boundsMax}.transformed(obj.m_transform);

        if (!printed) {
            std::cout << "Bounds: min " << world.min << " | max " << world.max << std::endl;
            printed = true;
        }

        return true; 
    }
};