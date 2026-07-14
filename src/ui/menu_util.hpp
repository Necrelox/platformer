#pragma once
// Pure, headless-testable helpers — deliberately free of raylib so this logic can be
// asserted without a window/GPU context (see tests/ui_test.cpp, tests/mp_ui_test.cpp).
#include <cstring>

namespace game::ui {

inline int wrap_index(int idx, int delta, int count) {
    return ((idx + delta) % count + count) % count;
}

// Edits an IP-address text buffer (digits + '.' only) in place.
// ch: a char code to append (0 = none this call); backspace: true to drop the last char.
// Returns true if buf was modified. Caller drives it once per pressed char / per backspace.
inline bool ip_edit(char* buf, int cap, int ch, bool backspace) {
    int len = (int)std::strlen(buf);
    if (backspace) {
        if (len == 0) return false;
        buf[len - 1] = '\0';
        return true;
    }
    if (ch == 0) return false;
    bool allowed = (ch >= '0' && ch <= '9') || ch == '.';
    if (!allowed) return false;
    if (len >= cap - 1) return false; // leave room for NUL
    buf[len] = (char)ch;
    buf[len + 1] = '\0';
    return true;
}

} // namespace game::ui
