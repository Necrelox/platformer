// Windowed smoke test: exercises the REAL draw path against a fully-populated world, with a
// real GL context. Distinct from integration_test (which never renders). Exit 0 = every draw
// path (render::draw_world + all ui:: screens + BeginMode2D) survived real GL calls on real
// data. Does NOT verify it looks right — that needs a human at the keyboard.
#include "raylib.h"
#include "core/api.hpp"
#include <cassert>
#include <cstdio>

using namespace game;

static void frame(Game& g) {
    BeginDrawing();
    ClearBackground(Color{ 20, 20, 30, 255 });
    switch (g.mode) {
        case Mode::Menu:     ui::menu(g);     break;
        case Mode::Settings: ui::settings(g); break;
        case Mode::Lobby:    ui::lobby(g);    break;
        case Mode::Playing:
            BeginMode2D(g.world.cam);
            render::draw_world(g.world);
            EndMode2D();
            ui::hud(g.world);
            break;
        case Mode::GameOver: ui::gameover(g); break;
    }
    EndDrawing();
}

int main() {
    SetTraceLogLevel(LOG_WARNING);
    InitWindow(SCREEN_W, SCREEN_H, "render smoke");

    Game g;
    new_game(g, 1);
    // Populate the world so every entity type exists on screen while drawing.
    for (int i = 0; i < 3000; ++i) {
        g.inputs[0].right = true;
        if (i % 60 == 0) g.inputs[0].jumpSeq++;
        g.inputs[0].jumpHeld = (i % 60 < 12); g.inputs[0].fire = true;
        tick(g, 1.0f / 120.0f);
    }
    assert(!g.world.platforms.empty());
    printf("populated: enemies=%zu items=%zu platforms=%zu projectiles=%zu\n",
           g.world.enemies.size(), g.world.items.size(),
           g.world.platforms.size(), g.world.projectiles.size());

    // Real Playing frames through render::draw_world + ui::hud.
    for (int i = 0; i < 120 && !WindowShouldClose(); ++i) { tick(g, 1.0f / 120.0f); frame(g); }

    // Force each UI screen's draw path (no input -> they just render).
    Mode screens[] = { Mode::Menu, Mode::Settings, Mode::Lobby, Mode::GameOver };
    for (Mode m : screens) { g.mode = m; for (int i = 0; i < 10 && !WindowShouldClose(); ++i) frame(g); }

    CloseWindow();
    puts("render_smoke OK");
    return 0;
}
