#include "raylib.h"
#include "core/api.hpp"

using namespace game;

int main() {
    InitWindow(SCREEN_W, SCREEN_H, "Platformer");
    SetTargetFPS(144);
    fx::init_audio();

    const float FIXED = 1.0f / 120.0f; // fixed physics step (framerate-independent)
    float acc = 0;
    Game g;

    while (!WindowShouldClose()) {
        float ft = GetFrameTime();
        if (ft > 0.25f) ft = 0.25f;
        fx::set_volume(g.settings.volume);

        // Sample local keyboard into g.input every frame. jumpSeq is a monotonic press counter
        // (edge-safe over the network); movement/fire are held state.
        g.input.left     = IsKeyDown(KEY_LEFT)  || IsKeyDown(KEY_A);
        g.input.right    = IsKeyDown(KEY_RIGHT) || IsKeyDown(KEY_D);
        g.input.jumpHeld = IsKeyDown(KEY_SPACE) || IsKeyDown(KEY_UP) || IsKeyDown(KEY_W);
        if (IsKeyPressed(KEY_SPACE) || IsKeyPressed(KEY_UP) || IsKeyPressed(KEY_W)) g.input.jumpSeq++;
        g.input.fire     = IsKeyDown(KEY_F) || IsKeyDown(KEY_J) || IsMouseButtonDown(MOUSE_BUTTON_LEFT);

        // Pump the socket whenever a session is open (lobby or in-game).
        if (net::is_active()) net::poll(g);

        int localSlot = g.net.lobby.localSlot;

        if (g.mode == Mode::Playing) {
            if (g.net.role == NetRole::Client) {
                // Client: send input, apply interpolated authoritative state, no local sim.
                g.inputs[localSlot] = g.input;
                net::send_input(g);
                net::sync_client(g, ft);   // ensure() to snapshot genCol + apply interpolated
                follow_camera(g.world, ft);
                fx::update(g.world, ft);
            } else {
                // Solo / Host: authoritative fixed-step simulation.
                g.inputs[localSlot] = g.input;
                acc += ft;
                while (acc >= FIXED) { tick(g, FIXED); acc -= FIXED; }
                if (g.net.role == NetRole::Host) net::broadcast_snapshot(g); // NET throttles internally
            }
        }

        BeginDrawing();
        ClearBackground(Color{ 20, 20, 30, 255 });
        switch (g.mode) {
            case Mode::Menu:     ui::menu(g);     break;
            case Mode::Settings: ui::settings(g); break;
            case Mode::Lobby:    ui::lobby(g);    break;
            case Mode::Playing:
                BeginMode2D(g.world.cam);
                render::draw_world(g.world);
                fx::draw(g.world);
                EndMode2D();
                ui::hud(g.world);
                break;
            case Mode::GameOver: ui::gameover(g); break;
        }
        EndDrawing();
    }

    net::shutdown();
    fx::shutdown_audio();
    CloseWindow();
    return 0;
}
