#pragma once
namespace valo::detail {
inline constexpr int cursorSize = 24;
inline bool bracketInk(bool start, int x, int y) {
    if (!start) x = cursorSize - 1 - x;
    return ((x == 7 || x == 8) && y >= 3 && y <= 20) ||
           (x >= 7 && x <= 15 && (y == 3 || y == 4 || y == 19 || y == 20));
}
inline bool bracketOutline(bool start, int x, int y) {
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx)
            if (bracketInk(start, x + dx, y + dy)) return true;
    return false;
}
} // namespace valo::detail
