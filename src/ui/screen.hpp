#pragma once
// Virtual-resolution letterbox transform (shared contract; not network state).
// The whole frame is rendered into a fixed SCREEN_W x SCREEN_H target, then blitted
// scaled + centered to the real window (fullscreen or resized). main.cpp updates these
// globals once per frame from the current window size; main (mouse aim) and ui (menu/pause
// clicks) map the real cursor back into virtual 1280x720 space via virtual_mouse().
//
// Identity by default (scale 1, no offset) so a plain 1280x720 window — every frame before a
// fullscreen toggle — is pixel-unchanged. Callers may use virtual_mouse() unconditionally.
#include "raylib.h"

namespace game {

inline float   g_viewScale  = 1.0f;        // real px per virtual px (min of the two axis ratios)
inline Vector2 g_viewOffset{ 0.0f, 0.0f }; // letterbox bar offset, real px (centers the image)

// Real cursor position -> virtual (SCREEN_W x SCREEN_H) coordinates.
inline Vector2 virtual_mouse() {
    Vector2 m = GetMousePosition();
    return { (m.x - g_viewOffset.x) / g_viewScale,
             (m.y - g_viewOffset.y) / g_viewScale };
}

} // namespace game
