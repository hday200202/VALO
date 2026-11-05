#pragma once

#include <string>
#include <bitset>
#include <array>
#include <functional>
#include <vector>
#include "common/Math.hpp"

struct SDL_Window;
struct SDL_Cursor;
union SDL_Event;

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
    Vulkan          = 0x10000000,
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

// Values are SDL_Keymod bits.
enum class KeyMod : uint16_t {
    None    = 0x0000,
    LShift  = 0x0001,
    RShift  = 0x0002,
    LCtrl   = 0x0040,
    RCtrl   = 0x0080,
    LAlt    = 0x0100,
    RAlt    = 0x0200,
    LGui    = 0x0400,
    RGui    = 0x0800,

    Shift   = LShift | RShift,
    Ctrl    = LCtrl  | RCtrl,
    Alt     = LAlt   | RAlt,
    Gui     = LGui   | RGui,
};

inline constexpr KeyMod operator|(KeyMod a, KeyMod b) { return KeyMod(uint16_t(a) | uint16_t(b)); }
inline constexpr KeyMod operator&(KeyMod a, KeyMod b) { return KeyMod(uint16_t(a) & uint16_t(b)); }
inline constexpr bool   any(KeyMod m)                 { return m != KeyMod::None; }

enum class CursorShape : uint8_t {
    Arrow, Hand, Text, ResizeEW, ResizeNS, ResizeNWSE, ResizeNESW, Move, Crosshair, Wait, NotAllowed,
    ResizeStart, ResizeEnd,
    Count
};

enum class SystemTheme : uint8_t { Unknown, Light, Dark };

enum class WindowEventType : uint8_t {
    KeyDown, KeyUp, Text, MouseDown, MouseUp, MouseMove, Wheel, FocusGained, FocusLost, CloseRequested
};

struct WindowEvent {
    WindowEventType type        = WindowEventType::KeyDown;
    uint64_t        timestamp   = 0;                    // nanoseconds
    KeyMod          mods        = KeyMod::None;
    Vec2f           mouse       = {0.f, 0.f};           // pixels, at the time of the event

    Key             key         = Key::Unknown;
    bool            repeat      = false;
    char32_t        symbol      = 0;                    // the layout's character for key, 0 if none

    MouseButton     button      = MouseButton::Left;
    uint8_t         clicks      = 0;

    Vec2f           wheel       = {0.f, 0.f};
    bool            precise     = false;                // not whole notches: a trackpad or high-res wheel

    char            text[32]    = {};                   // UTF-8, for Text
};

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
    Vec2u getMonitorSize() const;

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

    // Return false to defer a native close request (for example, an unsaved document).
    void setCloseRequestHandler(std::function<bool()> handler) { m_onCloseRequest = std::move(handler); }
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
    Vec2f scrollDelta() const   { return m_scrollDelta; }
    float getAspect() const;

    const std::vector<WindowEvent>& events() const { return m_events; }
    KeyMod modState() const;

    void startTextInput();
    void stopTextInput();
    bool isTextInputActive() const;

    std::string getClipboardText() const;
    void setClipboardText(const std::string& text);

    void setCursor(const CursorShape shape);
    std::string cursorDiagnostics() const;
    void setCursorVisible(bool visible);
    bool hasInputFocus() const;

    // called from inside the OS's modal resize loop as well, where the main loop is blocked
    void setResizeCallback(std::function<void()> callback);

    void* nativeHandle() const;

    float getDisplayScale() const;
    float getRefreshRate() const;
    static SystemTheme getSystemTheme();
    static std::string basePath();
    static std::string prefPath(const std::string& org, const std::string& app);

    /*
        The primary display and the machine, with no window needed - an app can
        size its first window from them. The display queries need SDL's video
        subsystem, which the first Window brings up; before that they return the
        fallback named beside each rather than failing.
    */
    static bool  isVideoReady();
    static float primaryDisplayContentScale();    // 1 if unknown
    static Vec2u primaryDisplayBounds();          // window points; 0x0 if unknown
    static float primaryDisplayPixelDensity();    // 1 if unknown
    static float primaryDisplayRefreshRate();     // 0 if unknown
    static int   displayCount();                  // 0 before video is up
    static int   cpuCount();                      // logical cores, 0 if unknown
    static int   systemRamMB();                   // 0 if unknown

    bool create(
        const std::string& title, 
        const Vec2u& size, 
        const WinFlags flags = WinFlags::Resize | WinFlags::HiDPI,
        const bool respectOSScale = false
    );

private:
    SDL_Window* m_window            = nullptr;
    bool        m_open              = false;
    bool        m_respectOSScale    = false;
    bool        m_resized           = false;
    bool        m_owned             = true;
    bool        m_counted           = false;    // holds one of s_liveWindows

    Vec2f       m_mouseDelta        = {0.f, 0.f};
    Vec2f       m_scrollDelta       = {0.f, 0.f};
    Vec2f       m_mouseLast         = {0.f, 0.f};
    KeyMod      m_mods              = KeyMod::None;

    std::vector<WindowEvent>        m_events;
    std::function<void()>           m_onResize;
    std::function<bool()>           m_onCloseRequest;
    bool                            m_watching          = false;
    bool                            m_pollingEvents     = false;
    std::array<SDL_Cursor*, size_t(CursorShape::Count)> m_cursors{};
    CursorShape m_cursorShape = CursorShape::Arrow;

    std::bitset<size_t(Key::Count)>         m_pressed, m_released;
    std::bitset<size_t(MouseButton::Count)> m_mousePressed, m_mouseReleased;

    void cleanup();
    void acquireSDL();
    void releaseSDL();
    void watchResize(const bool on);
    static bool resizeWatch(void* userdata, SDL_Event* event);
    friend class VALO;

    static int s_liveWindows;
};

}
