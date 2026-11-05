#include "Window.hpp"
#include "CursorGlyph.hpp"
#import <Cocoa/Cocoa.h>
#include <sstream>
#include <algorithm>

namespace valo {
void focusMacWindow(void* native) {
    NSWindow* window = (__bridge NSWindow*)native;
    [NSApp activateIgnoringOtherApps:YES];
    [window makeKeyAndOrderFront:nil];
}
namespace {
NSCursor* bracket(bool start) {
    constexpr int size = detail::cursorSize;
    auto* bitmap = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nil
        pixelsWide:size pixelsHigh:size bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES
        isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:size * 4 bitsPerPixel:32];
    auto* data = bitmap.bitmapData;
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            const auto i = (y * size + x) * 4;
            const bool ink = detail::bracketInk(start, x, y);
            data[i] = data[i + 1] = data[i + 2] = ink ? 0 : 255;
            data[i + 3] = detail::bracketOutline(start, x, y) ? 255 : 0;
        }
    auto* image = [[NSImage alloc] initWithSize:NSMakeSize(size, size)];
    [image addRepresentation:bitmap];
    return [[NSCursor alloc] initWithImage:image hotSpot:NSMakePoint(start ? 7 : 16, 12)];
}
NSCursor* invisibleCursor() {
    static NSCursor* cursor = [] {
        auto* bitmap = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nil
            pixelsWide:16 pixelsHigh:16 bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES
            isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:64 bitsPerPixel:32];
        std::fill_n(bitmap.bitmapData, 16 * 64, 0);
        auto* image = [[NSImage alloc] initWithSize:NSMakeSize(16, 16)];
        [image addRepresentation:bitmap];
        return [[NSCursor alloc] initWithImage:image hotSpot:NSZeroPoint];
    }();
    return cursor;
}
NSCursor* cursorFor(CursorShape shape) {
    switch (shape) {
    case CursorShape::Arrow: return NSCursor.arrowCursor;
    case CursorShape::Hand: return NSCursor.pointingHandCursor;
    case CursorShape::Text: return NSCursor.IBeamCursor;
    case CursorShape::ResizeEW: return NSCursor.resizeLeftRightCursor;
    case CursorShape::ResizeNS: return NSCursor.resizeUpDownCursor;
    case CursorShape::Move: return NSCursor.openHandCursor;
    case CursorShape::Crosshair: return NSCursor.crosshairCursor;
    case CursorShape::NotAllowed: return NSCursor.operationNotAllowedCursor;
    case CursorShape::ResizeStart: { static NSCursor* cursor = bracket(true); return cursor; }
    case CursorShape::ResizeEnd: { static NSCursor* cursor = bracket(false); return cursor; }
    default: return nil; // Other shapes retain SDL's native implementation.
    }
}
NSInteger pointerWindow() {
    return [NSWindow windowNumberAtPoint:NSEvent.mouseLocation belowWindowWithWindowNumber:0];
}
}
bool applyMacWindowCursor(void* native, CursorShape shape, bool visible) {
    NSWindow* window = (__bridge NSWindow*)native;
    if (!window.isVisible || pointerWindow() != window.windowNumber)
        return false;
    // Cursor rectangles may be suspended during AppKit tracking. Applying the
    // cursor directly works immediately and also tolerates stale SDL focus.
    // SDL hides by invalidating AppKit cursor rectangles, not by globally
    // hiding NSCursor. Our direct cursor path must honor that visibility too.
    auto* cursor = visible ? cursorFor(shape) : invisibleCursor();
    if (cursor && (!visible || NSCursor.currentCursor != cursor)) [cursor set];
    return true;
}
std::string macWindowCursorDiagnostics(void* native) {
    NSWindow* window = (__bridge NSWindow*)native;
    NSCursor* cursor = NSCursor.currentCursor;
    const auto size = cursor.image.size;
    std::ostringstream out;
    out << "nativeWindow=" << window.windowNumber << " pointerWindow=" << pointerWindow()
        << " cursorRects=" << bool([window areCursorRectsEnabled])
        << " nativeCursor=" << (__bridge void*)cursor
        << " image=" << size.width << 'x' << size.height
        << " imageHash=" << cursor.image.TIFFRepresentation.hash;
    return out.str();
}
} // namespace valo
