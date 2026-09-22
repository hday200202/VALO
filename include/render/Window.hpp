#pragma once

#include <string>
#include <bitset>
#include "common/Math.hpp"

struct SDL_Window;

namespace valo {

// These will directly correspond to SDL_WindowFlags
enum class WinFlags : uint64_t {
    None            = 0,
    Fullscreen      = 0x00000001,
    // skip opengl
    Hidden          = 0x00000008,
    Borderless      = 0x00000010,
    Resize          = 0x00000020,
    Minimized       = 0x00000040,
    Maximized       = 0x00000080,
    HiDPI           = 0x00002000,
    AlwaysOnTop     = 0x00010000,
    // skip metal
    Transparent     = 0x40000000,
};




inline constexpr WinFlags operator|(WinFlags a, WinFlags b) {
    return WinFlags(uint64_t(a) | uint64_t(b));
}


inline constexpr WinFlags operator&(WinFlags a, WinFlags b) {
    return WinFlags(uint64_t(a) & uint64_t(b));
}


inline constexpr WinFlags& operator|=(WinFlags& a, WinFlags b) {
    return a = a | b;
}




// Values are SDL scancodes (physical key positions), so lookups need no translation.
enum class Key : uint16_t {
    Unknown = 0,

    A = 4, B, C, D, E, F, G, H, I, J, K, L, M,
    N, O, P, Q, R, S, T, U, V, W, X, Y, Z,

    Num1 = 30, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9, Num0,

    Return = 40, Escape, Backspace, Tab, Space,
    Minus, Equals, LeftBracket, RightBracket, Backslash,
    Semicolon = 51, Apostrophe, Grave, Comma, Period, Slash, CapsLock,

    F1 = 58, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,

    PrintScreen = 70, ScrollLock, Pause, Insert, Home, PageUp, Delete, End, PageDown,
    Right = 79, Left, Down, Up,

    NumLock = 83, KpDivide, KpMultiply, KpMinus, KpPlus, KpEnter,
    Kp1 = 89, Kp2, Kp3, Kp4, Kp5, Kp6, Kp7, Kp8, Kp9, Kp0, KpPeriod,

    LCtrl = 224, LShift, LAlt, LGui, RCtrl, RShift, RAlt, RGui,

    Count = 512
};

enum class MouseButton : uint8_t { Left = 1, Middle = 2, Right = 3, X1 = 4, X2 = 5, Count = 6 };

class Window {
public:
    Window() = default;
    ~Window();

    Window(const Window&)            = delete;
    Window& operator=(const Window&) = delete;
    Window(Window&& other) noexcept;
    Window& operator=(Window&& other) noexcept;

    Window(
        const std::string& title, 
        const Vec2u& size, 
        const WinFlags flags = WinFlags::Resize | WinFlags::HiDPI,
        const bool respectOSScale = false
    );

    bool isOpen() const;
    void close();

    bool fromSDL(SDL_Window* window);
    SDL_Window* sdlHandle();

    void setSize(const Vec2u& size);
    Vec2u getSize() const;

    Vec2f getCenterGlobal() const;
    Vec2f getCenterLocal() const;

    float getPixelDensity() const;

    void setFullscreen(const bool fullscreen);
    bool isFullscreen() const;

    void setPosition(const Vec2i& position);
    Vec2i getPosition() const;

    void setTitle(const std::string& title);
    void setResizable(const bool resizable);
    void setBordered(const bool bordered);
    void setAlwaysOnTop(const bool alwaysOnTop);

    bool pollEvents();

    void grabMouse();
    void releaseMouse();
    bool isMouseGrabbed() const;

    void grabKeyboard();
    void releaseKeyboard();
    bool isKeyboardGrabbed() const;

    void focus();
    void setFocusable(const bool focusable);
    bool isFocusable() const;

    void show();
    void hide();
    void minimize();
    void maximize();

    void restore();

    bool resized() const { return m_resized; }

    bool isKeyDown(const Key key) const;
    bool wasKeyPressed(const Key key) const  { return m_pressed[size_t(key)]; }
    bool wasKeyReleased(const Key key) const { return m_released[size_t(key)]; }

    bool isMouseDown(const MouseButton button) const;
    bool wasMousePressed(const MouseButton button) const  { return m_mousePressed[size_t(button)]; }
    bool wasMouseReleased(const MouseButton button) const { return m_mouseReleased[size_t(button)]; }

    Vec2f mouseDelta() const    { return m_mouseDelta; }
    Vec2f mousePosition() const;
    void setMousePositionGlobal(const Vec2f& position);
    void setMousePositionLocal(const Vec2f& position);
    float scrollDelta() const   { return m_scrollDelta; }
    float getAspect() const;

private:
    SDL_Window* m_window            = nullptr;
    bool        m_open              = false;
    bool        m_respectOSScale    = false;
    bool        m_resized           = false;
    bool        m_owned             = true;

    Vec2f       m_mouseDelta        = {0.f, 0.f};
    float       m_scrollDelta       = 0.f;

    std::bitset<size_t(Key::Count)>         m_pressed, m_released;
    std::bitset<size_t(MouseButton::Count)> m_mousePressed, m_mouseReleased;

    void cleanup();
    friend class VALO;

    static int s_liveWindows;
};

}