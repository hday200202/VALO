#include "Window.hpp"
#include <SDL3/SDL.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>

namespace valo {

int Window::s_liveWindows = 0;

// each Window counts itself at most once, so SDL_Quit runs exactly when the last
// window that started SDL goes away
void Window::acquireSDL() {
    if (m_counted) return;
    if (s_liveWindows++ == 0) SDL_Init(SDL_INIT_VIDEO);
    m_counted = true;
}

void Window::releaseSDL() {
    if (!m_counted) return;
    m_counted = false;
    if (--s_liveWindows == 0) SDL_Quit();
}

namespace {

constexpr uint16_t KEYMOD_BITS = 0x0FC3;

KeyMod keyModOf(const SDL_Keymod mod) { return KeyMod(uint16_t(mod) & KEYMOD_BITS); }

char32_t symbolOf(const SDL_Scancode scancode, const SDL_Keymod mod) {
    const SDL_Keycode key = SDL_GetKeyFromScancode(scancode, mod, true);
    if (key & SDLK_SCANCODE_MASK) return 0;
    if (key < 0x20 || key == 0x7F) return 0;
    return char32_t(key);
}

const SDL_SystemCursor SYSTEM_CURSORS[size_t(CursorShape::Count)] = {
    SDL_SYSTEM_CURSOR_DEFAULT,
    SDL_SYSTEM_CURSOR_POINTER,
    SDL_SYSTEM_CURSOR_TEXT,
    SDL_SYSTEM_CURSOR_EW_RESIZE,
    SDL_SYSTEM_CURSOR_NS_RESIZE,
    SDL_SYSTEM_CURSOR_NWSE_RESIZE,
    SDL_SYSTEM_CURSOR_NESW_RESIZE,
    SDL_SYSTEM_CURSOR_MOVE,
    SDL_SYSTEM_CURSOR_CROSSHAIR,
    SDL_SYSTEM_CURSOR_WAIT,
    SDL_SYSTEM_CURSOR_NOT_ALLOWED,
};

} // namespace

Window::Window(
    const std::string& title, 
    const Vec2u& size, 
    const WinFlags flags,
    const bool respectOSScale
) : m_respectOSScale(respectOSScale) {
    acquireSDL();

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
    releaseSDL();
}

Window::Window(Window&& other) noexcept
    : m_window(other.m_window),
      m_open(other.m_open),
      m_respectOSScale(other.m_respectOSScale),
      m_resized(other.m_resized),
      m_owned(other.m_owned),
      m_counted(other.m_counted),
      m_events(std::move(other.m_events)),
      m_onResize(std::move(other.m_onResize)),
      m_cursors(other.m_cursors) {
    const bool watching = other.m_watching;
    other.watchResize(false);
    other.m_cursors = {};
    other.m_window  = nullptr;
    other.m_open    = false;
    other.m_counted = false;
    watchResize(watching);
}

Window& Window::operator=(Window&& other) noexcept {
    if (this != &other) {
        cleanup();
        const bool watching = other.m_watching;
        other.watchResize(false);

        m_window         = other.m_window;
        m_open           = other.m_open;
        m_respectOSScale = other.m_respectOSScale;
        m_resized        = other.m_resized;
        m_owned          = other.m_owned;
        m_events         = std::move(other.m_events);
        m_onResize       = std::move(other.m_onResize);
        m_cursors        = other.m_cursors;
        other.m_cursors  = {};
        other.m_window   = nullptr;
        other.m_open     = false;

        // two counts collapse into one; this still holds it, so it can't reach zero
        const bool mine = m_counted, theirs = other.m_counted;
        other.m_counted = false;
        m_counted       = mine || theirs;
        if (mine && theirs) --s_liveWindows;

        watchResize(watching);
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
        SDL_SetWindowSize(m_window, int(size.x), int(size.y));
    else
        SDL_SetWindowSize(m_window, int(size.x / density), int(size.y / density));
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

// the display the window is on, in pixels like getSize()
Vec2u Window::getMonitorSize() const {
    const SDL_DisplayID display = SDL_GetDisplayForWindow(m_window);
    const SDL_DisplayMode* mode = display ? SDL_GetCurrentDisplayMode(display) : nullptr;
    if (!mode) return {0, 0};

    return {
        static_cast<uint32_t>(mode->w * mode->pixel_density),
        static_cast<uint32_t>(mode->h * mode->pixel_density)
    };
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
    m_scrollDelta = {0.f, 0.f};
    m_pressed.reset();
    m_released.reset();
    m_mousePressed.reset();
    m_mouseReleased.reset();
    m_events.clear();

    const float density = getPixelDensity();

    auto push = [&](const WindowEventType type, const uint64_t timestamp) -> WindowEvent& {
        WindowEvent& ev = m_events.emplace_back();
        ev.type      = type;
        ev.timestamp = timestamp;
        ev.mods      = m_mods;
        ev.mouse     = m_mouseLast;
        return ev;
    };

    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                m_open = false;
                push(WindowEventType::CloseRequested, e.common.timestamp);
                break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                m_resized = true;
                break;
            case SDL_EVENT_WINDOW_FOCUS_GAINED:
                m_mods = keyModOf(SDL_GetModState());
                push(WindowEventType::FocusGained, e.window.timestamp);
                break;
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                push(WindowEventType::FocusLost, e.window.timestamp);
                break;
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP: {
                const bool down = e.type == SDL_EVENT_KEY_DOWN;
                if (e.key.scancode < SDL_SCANCODE_COUNT) {
                    if (down && !e.key.repeat) m_pressed.set(e.key.scancode);
                    if (!down)                 m_released.set(e.key.scancode);
                }

                // each key event carries the state as of itself; the live state is
                // already past the whole batch by the time anything here runs
                m_mods = keyModOf(e.key.mod);

                WindowEvent& ev = push(down ? WindowEventType::KeyDown : WindowEventType::KeyUp, e.key.timestamp);
                ev.key    = Key(e.key.scancode < SDL_SCANCODE_COUNT ? e.key.scancode : 0);
                ev.repeat = e.key.repeat;
                ev.symbol = symbolOf(e.key.scancode, e.key.mod);
                break;
            }
            case SDL_EVENT_TEXT_INPUT: {
                WindowEvent& ev = push(WindowEventType::Text, e.text.timestamp);
                if (e.text.text) {
                    size_t n = std::min(std::strlen(e.text.text), sizeof(ev.text) - 1);
                    while (n > 0 && (uint8_t(e.text.text[n]) & 0xC0) == 0x80) --n;   // whole code points only
                    std::memcpy(ev.text, e.text.text, n);
                }
                break;
            }
            case SDL_EVENT_MOUSE_MOTION: {
                m_mouseDelta.x += e.motion.xrel * density;
                m_mouseDelta.y += e.motion.yrel * density;
                m_mouseLast = {e.motion.x * density, e.motion.y * density};
                push(WindowEventType::MouseMove, e.motion.timestamp);
                break;
            }
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP: {
                const bool down = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
                if (e.button.button < size_t(MouseButton::Count))
                    (down ? m_mousePressed : m_mouseReleased).set(e.button.button);

                m_mouseLast = {e.button.x * density, e.button.y * density};

                WindowEvent& ev = push(down ? WindowEventType::MouseDown : WindowEventType::MouseUp, e.button.timestamp);
                ev.button = MouseButton(e.button.button);
                ev.clicks = e.button.clicks;
                break;
            }
            case SDL_EVENT_MOUSE_WHEEL: {
                m_scrollDelta.x += e.wheel.x;
                m_scrollDelta.y += e.wheel.y;
                m_mouseLast = {e.wheel.mouse_x * density, e.wheel.mouse_y * density};

                WindowEvent& ev = push(WindowEventType::Wheel, e.wheel.timestamp);
                ev.wheel   = {e.wheel.x, e.wheel.y};
                ev.precise = e.wheel.x != std::floor(e.wheel.x) || e.wheel.y != std::floor(e.wheel.y);
                break;
            }
            default:
                break;
        }
    }

    return m_open;
}

KeyMod Window::modState() const { return keyModOf(SDL_GetModState()); }

void Window::startTextInput()          { SDL_StartTextInput(m_window); }
void Window::stopTextInput()           { SDL_StopTextInput(m_window); }
bool Window::isTextInputActive() const { return SDL_TextInputActive(m_window); }

std::string Window::getClipboardText() const {
    char* text = SDL_GetClipboardText();
    std::string out = text ? text : "";
    SDL_free(text);
    return out;
}

void Window::setClipboardText(const std::string& text) {
    SDL_SetClipboardText(text.c_str());
}

void Window::setCursor(const CursorShape shape) {
    assert(shape < CursorShape::Count && "setCursor(): invalid shape");
    if (shape >= CursorShape::Count) return;

    SDL_Cursor*& cursor = m_cursors[size_t(shape)];
    if (!cursor) cursor = SDL_CreateSystemCursor(SYSTEM_CURSORS[size_t(shape)]);
    if (cursor) SDL_SetCursor(cursor);
}

void Window::setResizeCallback(std::function<void()> callback) {
    m_onResize = std::move(callback);
    watchResize(bool(m_onResize));
}

void Window::watchResize(const bool on) {
    if (on == m_watching) return;

    if (on) SDL_AddEventWatch(&Window::resizeWatch, this);
    else    SDL_RemoveEventWatch(&Window::resizeWatch, this);
    m_watching = on;
}

bool Window::resizeWatch(void* userdata, SDL_Event* e) {
    auto* self = static_cast<Window*>(userdata);
    if (!self->m_window || !self->m_onResize) return true;

    const bool sizeChanged = e->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED;
    const bool redraw      = sizeChanged
                          || e->type == SDL_EVENT_WINDOW_RESIZED
                          || e->type == SDL_EVENT_WINDOW_EXPOSED;

    if (!redraw || e->window.windowID != SDL_GetWindowID(self->m_window)) return true;

    static bool inside = false;
    if (inside) return true;

    inside = true;
    if (sizeChanged) self->m_resized = true;
    self->m_onResize();
    inside = false;

    return true;
}

void* Window::nativeHandle() const {
    const SDL_PropertiesID props = SDL_GetWindowProperties(m_window);
    if (!props) return nullptr;

    if (void* p = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr))      return p;
    if (void* p = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr))    return p;
    if (void* p = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr)) return p;

    const Sint64 x11 = SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
    return reinterpret_cast<void*>(uintptr_t(x11));
}

float Window::getDisplayScale() const {
    return SDL_GetWindowDisplayScale(m_window);
}

float Window::getRefreshRate() const {
    const SDL_DisplayID display = SDL_GetDisplayForWindow(m_window);
    const SDL_DisplayMode* mode = display ? SDL_GetCurrentDisplayMode(display) : nullptr;
    return mode ? mode->refresh_rate : 0.f;
}

SystemTheme Window::getSystemTheme() {
    switch (SDL_GetSystemTheme()) {
        case SDL_SYSTEM_THEME_LIGHT: return SystemTheme::Light;
        case SDL_SYSTEM_THEME_DARK:  return SystemTheme::Dark;
        default:                     return SystemTheme::Unknown;
    }
}

// SDL owns the base path's string; the pref path's is the caller's to free
std::string Window::basePath() {
    const char* path = SDL_GetBasePath();
    return path ? path : "";
}

std::string Window::prefPath(const std::string& org, const std::string& app) {
    char* path = SDL_GetPrefPath(org.c_str(), app.c_str());
    std::string out = path ? path : "";
    SDL_free(path);
    return out;
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
    return !(SDL_GetWindowFlags(m_window) & SDL_WINDOW_NOT_FOCUSABLE);
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
    watchResize(false);

    for (SDL_Cursor*& cursor : m_cursors) {
        if (cursor) SDL_DestroyCursor(cursor);
        cursor = nullptr;
    }

    if (m_window && m_owned) SDL_DestroyWindow(m_window);
    m_window = nullptr;
    m_open   = false;
}

float Window::getAspect() const {
    auto size = getSize();
    return float(size.x) / float(size.y);
}

bool Window::create(
    const std::string& title, 
    const Vec2u& size, 
    const WinFlags flags,
    const bool respectOSScale
) {
    if (m_window && m_owned) SDL_DestroyWindow(m_window);
    m_window = nullptr;
    m_owned  = true;

    acquireSDL();
    m_respectOSScale = respectOSScale;

    m_window = SDL_CreateWindow(
        title.c_str(), 
        int(size.x), int(size.y), 
        SDL_WindowFlags(uint64_t(flags | WinFlags::Hidden))
    );

    if (!m_window) {
        SDL_Log("Window: %s", SDL_GetError());
        return false;
    }

    setSize(size);
    if ((flags & WinFlags::Hidden) == WinFlags::None) show();
    m_open = true;
    return true;
}

}