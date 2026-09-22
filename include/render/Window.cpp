#include "Window.hpp"
#include <SDL3/SDL.h>

namespace valo {

int Window::s_liveWindows = 0;

Window::Window(
    const std::string& title, 
    const Vec2u& size, 
    const WinFlags flags,
    const bool respectOSScale
) : m_respectOSScale(respectOSScale) {
    if (s_liveWindows++ == 0) SDL_Init(SDL_INIT_VIDEO);

    m_window = SDL_CreateWindow(
        title.c_str(), 
        int(size.x), int(size.y), 
        SDL_WindowFlags(uint64_t(flags | WinFlags::Hidden))
    );

    if (!m_window) {
        SDL_Log("Window: %s", SDL_GetError());
        return;
    }

    setSize(size);
    if ((flags & WinFlags::Hidden) == WinFlags::None) show();
    m_open = true;
}

Window::~Window() {
    cleanup();
    if (--s_liveWindows == 0) SDL_Quit();
}

Window::Window(Window&& other) noexcept
    : m_window(other.m_window),
      m_open(other.m_open),
      m_respectOSScale(other.m_respectOSScale),
      m_resized(other.m_resized),
      m_owned(other.m_owned) {
    other.m_window = nullptr;
    other.m_open   = false;
    ++s_liveWindows;
}

Window& Window::operator=(Window&& other) noexcept {
    if (this != &other) {
        cleanup();
        m_window         = other.m_window;
        m_open           = other.m_open;
        m_respectOSScale = other.m_respectOSScale;
        m_resized        = other.m_resized;
        m_owned          = other.m_owned;
        other.m_window   = nullptr;
        other.m_open     = false;
    }
    return *this;
}

bool Window::isOpen() const { return m_open; }
void Window::close() { m_open = false; }

bool Window::fromSDL(SDL_Window* window) {
    if (m_window) cleanup();
    m_window = window;
    m_owned = false;
    m_open = true;
    return m_open;
}

SDL_Window* Window::sdlHandle() { return m_window; }

void Window::setSize(const Vec2u& size) {
    float density = getPixelDensity();
    if (m_respectOSScale)
        SDL_SetWindowSize(m_window, size.x, size.y);
    else
        SDL_SetWindowSize(m_window, size.x / density, size.y / density);
}

Vec2u Window::getSize() const {
    int w, h;
    SDL_GetWindowSizeInPixels(m_window, &w, &h);
    return {
        static_cast<uint32_t>(w),
        static_cast<uint32_t>(h)
    };
}

Vec2f Window::getCenterGlobal() const {
    const auto pos = getPosition();
    const auto size = getSize();
    const float d = getPixelDensity();
    return {
        pos.x + size.x / d / 2.f,
        pos.y + size.y / d / 2.f,
    };
}

Vec2f Window::getCenterLocal() const {
    const auto size = getSize();
    return {
        size.x / 2.f,
        size.y / 2.f
    };
}

float Window::getPixelDensity() const {
    return SDL_GetWindowPixelDensity(m_window);
}

void Window::setFullscreen(const bool fullscreen) {
    SDL_SetWindowFullscreen(m_window, fullscreen);
}

bool Window::isFullscreen() const {
    auto flags = SDL_GetWindowFlags(m_window);
    return flags & SDL_WINDOW_FULLSCREEN;
}

void Window::setPosition(const Vec2i& position) {
    SDL_SetWindowPosition(m_window, position.x, position.y);
}

Vec2i Window::getPosition() const {
    Vec2i pos;
    SDL_GetWindowPosition(m_window, &pos.x, &pos.y);
    return pos;
}

void Window::setTitle(const std::string& title) {
    SDL_SetWindowTitle(m_window, title.c_str());
}

void Window::setResizable(const bool resizable) {
    SDL_SetWindowResizable(m_window, resizable);
}

void Window::setBordered(const bool bordered) {
    SDL_SetWindowBordered(m_window, bordered);
}

void Window::setAlwaysOnTop(const bool alwaysOnTop) {
    SDL_SetWindowAlwaysOnTop(m_window, alwaysOnTop);
}

bool Window::pollEvents() {
    m_resized     = false;
    m_mouseDelta  = {0.f, 0.f};
    m_scrollDelta = 0.f;
    m_pressed.reset();
    m_released.reset();
    m_mousePressed.reset();
    m_mouseReleased.reset();

    const float density = getPixelDensity();

    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                m_open = false;
                break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                m_resized = true;
                break;
            case SDL_EVENT_KEY_DOWN:
                if (!e.key.repeat && e.key.scancode < SDL_SCANCODE_COUNT)
                    m_pressed.set(e.key.scancode);
                break;
            case SDL_EVENT_KEY_UP:
                if (e.key.scancode < SDL_SCANCODE_COUNT)
                    m_released.set(e.key.scancode);
                break;
            case SDL_EVENT_MOUSE_MOTION:
                m_mouseDelta.x += e.motion.xrel * density;
                m_mouseDelta.y += e.motion.yrel * density;
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (e.button.button < size_t(MouseButton::Count))
                    m_mousePressed.set(e.button.button);
                break;
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (e.button.button < size_t(MouseButton::Count))
                    m_mouseReleased.set(e.button.button);
                break;
            case SDL_EVENT_MOUSE_WHEEL:
                m_scrollDelta += e.wheel.y;
                break;
            default:
                break;
        }
    }

    return m_open;
}

bool Window::isKeyDown(const Key key) const {
    const bool* state = SDL_GetKeyboardState(nullptr);
    return state[size_t(key)];
}

bool Window::isMouseDown(const MouseButton button) const {
    return SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_MASK(uint8_t(button));
}

Vec2f Window::mousePosition() const {
    float x, y;
    SDL_GetMouseState(&x, &y);
    const float d = getPixelDensity();
    return { x * d, y * d };
}

void Window::setMousePositionGlobal(const Vec2f& position) {
    SDL_WarpMouseGlobal(position.x, position.y);
}

void Window::setMousePositionLocal(const Vec2f& position) {
    const float d = getPixelDensity();
    SDL_WarpMouseInWindow(m_window, position.x / d, position.y / d);
}

void Window::grabMouse() {
    SDL_SetWindowRelativeMouseMode(m_window, true);
}

void Window::releaseMouse() {
    SDL_SetWindowRelativeMouseMode(m_window, false);
}

bool Window::isMouseGrabbed() const {
    return SDL_GetWindowRelativeMouseMode(m_window);
}

void Window::grabKeyboard() {
    SDL_SetWindowKeyboardGrab(m_window, true);
}

void Window::releaseKeyboard() {
    SDL_SetWindowKeyboardGrab(m_window, false);
}

bool Window::isKeyboardGrabbed() const {
    return SDL_GetWindowKeyboardGrab(m_window);
}

void Window::focus() { SDL_RaiseWindow(m_window); }
void Window::setFocusable(const bool focusable) {
    SDL_SetWindowFocusable(m_window, focusable);
}
bool Window::isFocusable() const {
    return SDL_GetWindowFlags(m_window) & SDL_WINDOW_INPUT_FOCUS;
}


void Window::show() {
    SDL_ShowWindow(m_window);
}

void Window::hide() {
    SDL_HideWindow(m_window);
}

void Window::minimize() {
    SDL_MinimizeWindow(m_window);
}

void Window::maximize() {
    SDL_MaximizeWindow(m_window);
}

void Window::restore() {
    SDL_RestoreWindow(m_window);
}

void Window::cleanup() { 
    if (m_window && m_owned) SDL_DestroyWindow(m_window);
    m_window = nullptr;
    m_open   = false;
}

float Window::getAspect() const {
    auto size = getSize();
    return float(size.x) / float(size.y);
}

}