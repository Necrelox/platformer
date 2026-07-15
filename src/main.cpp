#include "raylib.h"
#include "core/api.hpp"
#include "ui/screen.hpp" // g_viewScale/g_viewOffset + virtual_mouse(): letterbox transform
#include <cmath>
#include <algorithm>

using namespace game;

// Single-pass bloom/glow + vignette over the world render texture. GLSL 330 core; falls back
// to nothing (see postFx guard in main) on GL contexts that can't compile it (e.g. GL 1.1 boxes).
static const char* kBloomFS = R"(
#version 330 core
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;
uniform vec4 colDiffuse;
uniform vec2 dashCenter;   // dash focus, [0,1] UV (top-left origin, matches GetWorldToScreen2D)
uniform float dashAmt;     // 0..1 dash intensity; 0 (default / not dashing) => distortion disabled
out vec4 finalColor;

// Standard soft-knee threshold (Unity/Unreal-style): ramps smoothly from 0 to a full pass
// instead of the old hard smoothstep cliff, so bright edges don't clip into a hard-edged glow.
vec3 brightPass(vec3 c) {
    const float thresh = 0.48;   // lower gate -> more of the scene blooms (the map "lacked light")
    const float knee    = 0.25;
    float b = max(c.r, max(c.g, c.b));
    float soft = clamp(b - thresh + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee + 1e-5);
    float contrib = max(soft, b - thresh);
    return c * (contrib / max(b, 1e-5));
}

void main() {
    vec2 texelSize = 1.0 / vec2(textureSize(texture0, 0));

    // Dash distortion: a radial shockwave ripple + chromatic split around the dashing player.
    // dashAmt==0 (default / not dashing) makes suv == fragTexCoord, i.e. a clean no-op.
    vec2 suv = fragTexCoord;
    vec3 base;
    if (dashAmt > 0.001) {
        vec2 dv   = fragTexCoord - dashCenter;
        float dist = length(dv);
        vec2 dir  = dv / max(dist, 1e-4);
        float wave = sin(dist * 42.0 - dashAmt * 12.0) * exp(-dist * 6.0);
        float falloff = exp(-dist * 4.0);           // concentrate the effect AROUND the dash, not globally
        suv = fragTexCoord + dir * (dashAmt * 0.020 * wave);
        float ca = dashAmt * 0.006 * falloff;       // chromatic aberration, fading out from the dash centre
        base = vec3(texture(texture0, suv + dir * ca).r,
                    texture(texture0, suv).g,
                    texture(texture0, suv - dir * ca).b);
    } else {
        base = texture(texture0, suv).rgb;
    }

    // gaussian-weighted bloom, 9x9 taps around the (possibly warped) suv.
    const int   R = 4;
    const float sigma = 2.2;
    vec3  bloom = vec3(0.0);
    float wsum  = 0.0;
    for (int y = -R; y <= R; y++) {
        for (int x = -R; x <= R; x++) {
            float w = exp(-float(x * x + y * y) / (2.0 * sigma * sigma));
            vec3 c = texture(texture0, suv + vec2(x, y) * texelSize * 1.5).rgb;
            bloom += brightPass(c) * w;
            wsum  += w;
        }
    }
    bloom /= wsum;

    vec3 color = base * 1.06 + bloom * 1.5;   // brighter overall + stronger glow (more "light")

    // gentle color grade: contrast S-curve around mid-grey + a saturation lift for punchier color
    color = clamp((color - 0.5) * 1.10 + 0.5, 0.0, 1.0);
    float luma = dot(color, vec3(0.299, 0.587, 0.114));
    color = mix(vec3(luma), color, 1.20);

    // subtle vignette
    vec2 uv = fragTexCoord - 0.5;
    float vig = clamp(1.0 - dot(uv, uv) * 0.7, 0.0, 1.0);
    color *= vig;

    finalColor = vec4(color, 1.0) * colDiffuse * fragColor;
}
)";

int main() {
    SetConfigFlags(FLAG_WINDOW_RESIZABLE); // so windowed resizing (and fullscreen) can letterbox
    InitWindow(SCREEN_W, SCREEN_H, "Platformer");
    SetExitKey(KEY_NULL); // ESC must NOT close the window — it's a "back"/pause key (handled below)
    SetTargetFPS(144);
    fx::init_audio();

    // Offscreen world buffer + bloom shader, created once. postFx is false (plain direct draw,
    // identical gameplay/visuals minus the glow) whenever either fails to come up — some boxes
    // are GL 1.1 / headless and can't do render textures or GLSL 330.
    RenderTexture2D worldRT = LoadRenderTexture(SCREEN_W, SCREEN_H);
    Shader bloomShader = LoadShaderFromMemory(nullptr, kBloomFS);
    bool postFx = worldRT.id != 0 && bloomShader.id != 0;
    // dash-distortion uniforms (a no-op at dashAmt=0; locations are -1 & SetShaderValue a no-op if
    // the shader ever fails to compile or the compiler strips the uniforms — safe either way).
    int locDashCenter = postFx ? GetShaderLocation(bloomShader, "dashCenter") : -1;
    int locDashAmt    = postFx ? GetShaderLocation(bloomShader, "dashAmt")    : -1;

    // Virtual framebuffer: the ENTIRE frame (world+bloom composite AND every ui:: screen) is
    // drawn into this fixed SCREEN_W x SCREEN_H target, then letterbox-blitted (scaled + centered,
    // black bars) to the real window each frame — see the letterbox block at the end of the loop.
    // letterbox is false only on GL contexts that can't do render textures at all; see the
    // fallback branch below (direct draw, unscaled, never crashes).
    RenderTexture2D virtualRT = LoadRenderTexture(SCREEN_W, SCREEN_H);
    bool letterbox = virtualRT.id != 0;

    const float FIXED = 1.0f / 120.0f; // fixed physics step (framerate-independent)
    float acc = 0;
    Game g;

    while (!WindowShouldClose() && !g.wantQuit) {
        float ft = GetFrameTime();
        if (ft > 0.25f) ft = 0.25f;
        fx::set_volume(g.settings.volume);

        // ---- letterbox transform: recomputed every frame, BEFORE any ui/mouse-aim code reads
        // the cursor, so menu/pause clicks (ui.cpp calls virtual_mouse()) and mouse-aim below map
        // back into virtual 1280x720 space correctly regardless of window size.
        if (letterbox) {
            g_viewScale = std::min(GetScreenWidth() / (float)SCREEN_W, GetScreenHeight() / (float)SCREEN_H);
            g_viewOffset = { (GetScreenWidth()  - SCREEN_W * g_viewScale) * 0.5f,
                             (GetScreenHeight() - SCREEN_H * g_viewScale) * 0.5f };
        } else {
            g_viewScale = 1.0f;
            g_viewOffset = { 0.0f, 0.0f };
        }

        // ---- sample local keyboard ----
        // Movement/fire are HELD state; jumpHeld too. jumpSeq is a monotonic press counter
        // (edge-safe over the network). See the latch note in the sim branch below.
        bool inLeft  = IsKeyDown(KEY_LEFT)  || IsKeyDown(KEY_A);
        bool inRight = IsKeyDown(KEY_RIGHT) || IsKeyDown(KEY_D);
        bool inFire  = IsKeyDown(KEY_F) || IsKeyDown(KEY_J) || IsMouseButtonDown(MOUSE_BUTTON_LEFT);
        g.input.jumpHeld = IsKeyDown(KEY_SPACE) || IsKeyDown(KEY_UP) || IsKeyDown(KEY_W);
        if (IsKeyPressed(KEY_SPACE) || IsKeyPressed(KEY_UP) || IsKeyPressed(KEY_W)) g.input.jumpSeq++;
        // Dash: same edge-safe pattern as jump (monotonic counter, never reset — see InputState::dashSeq).
        if (IsKeyPressed(KEY_LEFT_SHIFT) || IsKeyPressed(KEY_RIGHT_SHIFT) || IsKeyPressed(KEY_K)) g.input.dashSeq++;

        // ESC toggles the in-game pause (both directions), owned here so ui::pause never sees ESC.
        if (IsKeyPressed(KEY_ESCAPE)) {
            if (g.mode == Mode::Playing)      g.mode = Mode::Paused;
            else if (g.mode == Mode::Paused)  g.mode = Mode::Playing;
        }

        // Pump the socket whenever a session is open (lobby or in-game).
        if (net::is_active()) net::poll(g);

        int localSlot = g.net.lobby.localSlot;

        // ---- mouse aim: normalized direction from the local player to the cursor (world space) ----
        // Stored as a direction (not a screen point) so it's camera-independent over the network.
        if ((g.mode == Mode::Playing || g.mode == Mode::Paused) && !g.world.players.empty()) {
            const Player& lp = g.world.players[g.world.localId];
            Vector2 c{ lp.pos.x + lp.size.x * 0.5f, lp.pos.y + lp.size.y * 0.5f };
            Vector2 mw = GetScreenToWorld2D(game::virtual_mouse(), g.world.cam);
            Vector2 d{ mw.x - c.x, mw.y - c.y };
            float len = std::sqrt(d.x * d.x + d.y * d.y);
            if (len > 1.0f) { g.input.aimX = d.x / len; g.input.aimY = d.y / len; }
            else            { g.input.aimX = 0; g.input.aimY = 0; }
        } else {
            g.input.aimX = 0; g.input.aimY = 0;
        }

        if (g.mode == Mode::Playing) {
            if (g.net.role == NetRole::Client) {
                // Client sends its input every frame, so no latch is needed here.
                g.input.left = inLeft; g.input.right = inRight; g.input.fire = inFire;
                g.inputs[localSlot] = g.input;
                net::send_input(g);
                net::sync_client(g, ft);   // ensure() to snapshot genCol + apply interpolated
                follow_camera(g.world, ft);
                fx::update(g.world, ft);
            } else {
                // Solo / Host: authoritative fixed-step sim. Render (144) runs faster than the
                // sim (120), so some frames run ZERO ticks. Latch held keys by OR-ing them in and
                // only clearing after a tick actually consumed them — otherwise a key pressed and
                // released across a zero-tick frame is silently lost ("dropped input" feel).
                g.input.left  |= inLeft;
                g.input.right |= inRight;
                g.input.fire  |= inFire;
                acc += ft;
                bool ticked = false;
                while (acc >= FIXED) {
                    g.inputs[localSlot] = g.input;
                    tick(g, FIXED);
                    acc -= FIXED;
                    ticked = true;
                }
                if (ticked) { g.input.left = g.input.right = g.input.fire = false; }
                if (g.net.role == NetRole::Host) net::broadcast_snapshot(g); // NET throttles internally
            }
        } else if (g.mode == Mode::Paused) {
            // Solo/Host freeze (no tick). A client can't freeze the host, so it keeps syncing.
            g.input.left = g.input.right = g.input.fire = false; // don't queue input while paused
            if (g.net.role == NetRole::Client) {
                g.inputs[localSlot] = g.input;
                net::send_input(g);        // tell the host we've stopped (don't coast on stale input)
                net::sync_client(g, ft);
                follow_camera(g.world, ft);
                fx::update(g.world, ft);
            } else if (g.net.role == NetRole::Host) {
                net::broadcast_snapshot(g);
            }
        }

        // Feed the dash-distortion uniforms (radial warp centred on the dashing local player).
        // No-op when not dashing / no postFx. 0.15f mirrors DASH_TIME in core/game.cpp.
        if (postFx) {
            float dashAmt = 0.0f;
            Vector2 dashCenter{ 0.5f, 0.5f };
            if ((g.mode == Mode::Playing || g.mode == Mode::Paused) && !g.world.players.empty()) {
                const Player& lp = g.world.players[g.world.localId];
                if (lp.dashTimer > 0.0f) {
                    dashAmt = std::fmin(1.0f, lp.dashTimer / 0.15f);
                    Vector2 c{ lp.pos.x + lp.size.x * 0.5f, lp.pos.y + lp.size.y * 0.5f };
                    Vector2 s = GetWorldToScreen2D(c, g.world.cam);
                    dashCenter = { s.x / (float)SCREEN_W, s.y / (float)SCREEN_H };
                }
            }
            SetShaderValue(bloomShader, locDashAmt,    &dashAmt,    SHADER_UNIFORM_FLOAT);
            SetShaderValue(bloomShader, locDashCenter, &dashCenter, SHADER_UNIFORM_VEC2);
        }

        auto drawWorldLayer = [&]() {
            BeginMode2D(g.world.cam);
            render::draw_world(g.world);
            fx::draw(g.world);
            EndMode2D();
        };

        // Texture-mode rendering must happen outside BeginDrawing/EndDrawing.
        if (postFx && (g.mode == Mode::Playing || g.mode == Mode::Paused)) {
            BeginTextureMode(worldRT);
            ClearBackground(Color{ 20, 20, 30, 255 });
            drawWorldLayer();
            EndTextureMode();
        }

        // Composites the world layer to the screen: through the bloom shader when available,
        // otherwise drawn directly (no post-process) — same pixels either way, minus the glow.
        auto compositeWorld = [&]() {
            if (postFx) {
                BeginShaderMode(bloomShader);
                // RenderTexture is y-flipped vs the screen; negative height un-flips it.
                DrawTextureRec(worldRT.texture,
                    Rectangle{ 0, 0, (float)SCREEN_W, -(float)SCREEN_H }, Vector2{ 0, 0 }, WHITE);
                EndShaderMode();
            } else {
                drawWorldLayer();
            }
        };

        auto drawFrame = [&]() {
            switch (g.mode) {
                case Mode::Menu:     ui::menu(g);     break;
                case Mode::Settings: ui::settings(g); break;
                case Mode::Lobby:    ui::lobby(g);    break;
                case Mode::Playing:
                    compositeWorld();
                    ui::hud(g.world);
                    break;
                case Mode::Paused:
                    compositeWorld();
                    ui::hud(g.world);
                    ui::pause(g);   // dim overlay + resume / quit-to-menu
                    break;
                case Mode::GameOver: ui::gameover(g); break;
            }
        };

        if (letterbox) {
            // Whole frame (world+bloom composite + ui) renders into the fixed virtual buffer...
            BeginTextureMode(virtualRT);
            ClearBackground(Color{ 20, 20, 30, 255 });
            drawFrame();
            EndTextureMode();

            // ...then gets blitted scaled + centered onto the real backbuffer, black bars either
            // side. g_viewScale/g_viewOffset (set at the top of the loop) drive both this blit and
            // virtual_mouse(), so the two stay in lockstep.
            BeginDrawing();
            ClearBackground(BLACK);
            DrawTexturePro(virtualRT.texture,
                Rectangle{ 0, 0, (float)SCREEN_W, -(float)SCREEN_H }, // y-flip, see worldRT note above
                Rectangle{ g_viewOffset.x, g_viewOffset.y, SCREEN_W * g_viewScale, SCREEN_H * g_viewScale },
                Vector2{ 0, 0 }, 0.0f, WHITE);
            EndDrawing();
        } else {
            // No render-texture support at all: draw straight to the backbuffer, unscaled. Not
            // letterboxed, but never crashes.
            BeginDrawing();
            ClearBackground(Color{ 20, 20, 30, 255 });
            drawFrame();
            EndDrawing();
        }
    }

    net::shutdown();
    fx::shutdown_audio();
    UnloadShader(bloomShader);
    UnloadRenderTexture(worldRT);
    UnloadRenderTexture(virtualRT);
    CloseWindow();
    return 0;
}
