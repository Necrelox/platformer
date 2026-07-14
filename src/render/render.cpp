#include "core/api.hpp"
#include <cmath>

// World-space renderer. Called inside BeginMode2D(cam) by main — never touch the camera or
// input here. Primitives only (no asset files exist).
namespace game::render {

namespace {

constexpr Color GROUND_FILL          = Color{ 60, 120, 60, 255 };
constexpr Color GROUND_TOP           = Color{ 130, 210, 120, 255 }; // lighter exposed-top edge
constexpr Color PLATFORM_MOVING_FILL = Color{ 70, 140, 230, 255 };
constexpr Color PLATFORM_BREAK_FLASH = Color{ 255, 90, 40, 255 };

void draw_tiles(const World& w) {
    float left  = w.cam.target.x - SCREEN_W;
    float right = w.cam.target.x + SCREEN_W;
    int tx0 = (int)std::floor(left  / TILE) - 1;
    int tx1 = (int)std::floor(right / TILE) + 1;
    for (int tx = tx0; tx <= tx1; ++tx) {
        for (int ty = 0; ty < WORLD_H; ++ty) {
            if (!w.map.solid(tx, ty)) continue;
            DrawRectangle(tx * TILE, ty * TILE, TILE, TILE, GROUND_FILL);
            if (!w.map.solid(tx, ty - 1)) // exposed surface -> lighter edge strip
                DrawRectangle(tx * TILE, ty * TILE, TILE, 4, GROUND_TOP);
        }
    }
}

void draw_platforms(const World& w) {
    for (const auto& p : w.platforms) {
        if (!p.active) continue;
        switch (p.kind) {
            case PlatformKind::Static:
                DrawRectangleRec(p.rect, GRAY);
                break;
            case PlatformKind::Moving:
                DrawRectangleRec(p.rect, PLATFORM_MOVING_FILL);
                DrawRectangleLinesEx(p.rect, 2, DARKBLUE);
                break;
            case PlatformKind::Breakable: {
                // ponytail: flash phase derived from world time (deterministic, no extra
                // per-platform timer needed) — good enough for a "cracking" cue.
                bool flash = p.triggered && std::fmod(w.time, 0.2f) < 0.1f;
                DrawRectangleRec(p.rect, flash ? PLATFORM_BREAK_FLASH : ORANGE);
                if (p.triggered) {
                    float x0 = p.rect.x, y0 = p.rect.y;
                    float x1 = p.rect.x + p.rect.width, y1 = p.rect.y + p.rect.height;
                    DrawLine((int)x0, (int)y0, (int)x1, (int)y1, BLACK);
                    DrawLine((int)x1, (int)y0, (int)x0, (int)y1, BLACK);
                }
                break;
            }
        }
    }
}

void draw_item(const Item& it) {
    Vector2 pos = it.pos;
    pos.y += std::sin(it.t * 4.0f) * 4.0f; // vertical bob
    Vector2 c{ pos.x + it.size.x * 0.5f, pos.y + it.size.y * 0.5f };
    float r = it.size.x * 0.35f;
    switch (it.type) {
        case ItemType::Heart:
            DrawCircleV({ c.x - r * 0.5f, c.y - r * 0.3f }, r * 0.6f, RED);
            DrawCircleV({ c.x + r * 0.5f, c.y - r * 0.3f }, r * 0.6f, RED);
            DrawTriangle({ c.x - r, c.y }, { c.x, c.y + r }, { c.x + r, c.y }, RED);
            break;
        case ItemType::RapidFire:
            DrawTriangle({ c.x, c.y - r }, { c.x - r * 0.8f, c.y + r }, { c.x + r * 0.8f, c.y + r }, YELLOW);
            break;
        case ItemType::DoubleShot:
            DrawCircleV({ c.x - r * 0.4f, c.y }, r * 0.6f, PURPLE);
            DrawCircleV({ c.x + r * 0.4f, c.y }, r * 0.6f, PURPLE);
            break;
        case ItemType::Invincibility:
            DrawPoly(c, 6, r, 0, GOLD);
            DrawPolyLines(c, 6, r, 0, WHITE);
            break;
    }
}

void draw_enemy(const Enemy& e) {
    Color body = e.state == EnemyState::Aggro ? Color{ 220, 40, 40, 255 } : MAROON;
    DrawRectangleV(e.pos, e.size, body);
    float eyeY = e.pos.y + e.size.y * 0.3f;
    float eyeX = e.facing == Facing::Right ? e.pos.x + e.size.x * 0.75f : e.pos.x + e.size.x * 0.25f;
    DrawCircleV({ eyeX, eyeY }, 2.5f, WHITE);
}

void draw_projectile(const Projectile& pr) {
    DrawCircleV(pr.pos, 4, pr.fromPlayer ? YELLOW : Color{ 255, 60, 200, 255 });
}

void draw_player(const Player& pl) {
    // ponytail: blink thresholds invuln's own value directly (no extra frame-counter state).
    // Ceiling: assumes invuln only ever counts down at a steady rate (true today via
    // physics::step) — if invuln ever pauses, swap for a GetTime()-based blink instead.
    bool hidden = pl.invuln > 0 && std::fmod(pl.invuln, 0.2f) < 0.1f;
    if (!hidden) {
        DrawRectangleV(pl.pos, pl.size, pl.color);
        float noseY = pl.pos.y + pl.size.y * 0.35f;
        float dir   = pl.facing == Facing::Right ? 1.0f : -1.0f;
        float noseX = pl.facing == Facing::Right ? pl.pos.x + pl.size.x : pl.pos.x;
        DrawTriangle({ noseX, noseY }, { noseX, noseY + pl.size.y * 0.25f },
                     { noseX + dir * 8, noseY + pl.size.y * 0.125f }, BLACK);
    }
    if (pl.invincible)
        DrawRectangleLinesEx({ pl.pos.x - 2, pl.pos.y - 2, pl.size.x + 4, pl.size.y + 4 }, 3, GOLD);
}

} // namespace

void draw_world(const World& w) {
    draw_tiles(w);
    draw_platforms(w);
    for (const auto& it : w.items)       if (it.alive) draw_item(it);
    for (const auto& e  : w.enemies)     if (e.alive)  draw_enemy(e);
    for (const auto& pr : w.projectiles) if (pr.alive) draw_projectile(pr);
    for (const auto& pl : w.players)     if (pl.alive) draw_player(pl);
}

} // namespace game::render
