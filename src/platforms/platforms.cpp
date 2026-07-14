#include "core/api.hpp"
#include <cmath>

// Phase 4 — platform variety: Breakable (trigger/break/respawn) and Moving (ping-pong + rider carry).
// Static needs nothing (core physics treats any active==true platform as solid already).
namespace game::platforms {

// ---- feel knobs ----
static constexpr float BREAK_DELAY   = 0.4f; // s from first contact to break
static constexpr float RESPAWN_DELAY = 3.0f; // s from break to respawn
static constexpr float STAND_TOL     = 4.0f; // px tolerance for "feet at platform top" test

// Shared "is this player standing on this rect" test used by both breakable trigger and
// moving-platform rider carry.
static bool standing_on(const Player& p, Rectangle rect) {
    if (!p.onGround) return false;
    float bottom = p.pos.y + p.size.y;
    if (std::fabs(bottom - rect.y) > STAND_TOL) return false;
    return p.pos.x + p.size.x > rect.x && p.pos.x < rect.x + rect.width;
}

static void update_breakable(const World& w, Platform& plat, float dt) {
    if (!plat.active) {
        plat.respawnTimer -= dt;
        if (plat.respawnTimer <= 0) {
            plat.active       = true;
            plat.triggered    = false;
            plat.breakTimer   = 0;
            plat.respawnTimer = 0;
        }
        return;
    }
    // ponytail: once triggered it commits to breaking even if the player steps off — matches
    // the classic "crumbling platform" feel and keeps this a one-way state machine.
    if (!plat.triggered) {
        for (const auto& p : w.players) {
            if (p.alive && standing_on(p, plat.rect)) {
                plat.triggered  = true;
                plat.breakTimer = BREAK_DELAY;
                break;
            }
        }
    }
    if (plat.triggered) {
        plat.breakTimer -= dt;
        if (plat.breakTimer <= 0) {
            plat.active       = false;
            plat.respawnTimer = RESPAWN_DELAY;
        }
    }
}

static void update_moving(World& w, Platform& plat, float dt) {
    if (plat.speed <= 0) return;
    Rectangle prevRect = plat.rect; // riders are detected against the pre-move position
    Vector2 seg{ plat.b.x - plat.a.x, plat.b.y - plat.a.y };
    float len = std::sqrt(seg.x * seg.x + seg.y * seg.y);
    if (len <= 0.0001f) return; // a == b: nothing to move along

    float dPhase = (plat.speed * dt) / len;
    plat.phase += dPhase * (float)plat.dir;
    if (plat.phase >= 1.0f) { plat.phase = 1.0f; plat.dir = -1; }
    else if (plat.phase <= 0.0f) { plat.phase = 0.0f; plat.dir = 1; }

    Vector2 newPos{ plat.a.x + seg.x * plat.phase, plat.a.y + seg.y * plat.phase };
    Vector2 delta{ newPos.x - prevRect.x, newPos.y - prevRect.y };
    plat.rect.x = newPos.x;
    plat.rect.y = newPos.y;

    // Carry riders standing on the platform's old spot by the same delta (horizontal & vertical).
    // ponytail: direct position nudge, not a swept collision — at these platform speeds (<=
    // MOVE_SPEED-ish) a rider can't tunnel through anything in one frame; physics::step
    // re-resolves against the platform's new rect the same tick anyway. Two known gaps: (1) a
    // level with a platform faster than the player, and (2) a still rider (vel.x==0) nudged
    // horizontally into a solid tile — core's resolve() only corrects along the vel direction,
    // so it won't eject a zero-velocity rider until the platform reverses. Revisit both if a
    // level design actually needs a platform running into walls with idle riders.
    for (auto& p : w.players) {
        if (p.alive && standing_on(p, prevRect)) {
            p.pos.x += delta.x;
            p.pos.y += delta.y;
        }
    }
}

void update(World& w, float dt) {
    for (auto& plat : w.platforms) {
        switch (plat.kind) {
            case PlatformKind::Breakable: update_breakable(w, plat, dt); break;
            case PlatformKind::Moving:    update_moving(w, plat, dt); break;
            case PlatformKind::Static:    break; // solid, no timers, no motion
        }
    }
}

} // namespace game::platforms
