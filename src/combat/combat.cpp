#include "core/api.hpp"
#include <algorithm>
#include <cmath>

// Phase 5 — player gun, projectiles, enemy AI, damage/death.
namespace game::combat {

// ---- feel knobs ----
// ponytail: game.cpp keeps its own GRAVITY as a file-local constant (not exported via api.hpp),
// so enemies need their own copy here. Keep this number in sync with core/game.cpp's GRAVITY.
static constexpr float GRAVITY          = 2200.f; // px/s^2, must match core/game.cpp

static constexpr float PROJ_SPEED       = 700.f;  // px/s
static constexpr float PROJ_LIFE        = 1.5f;   // s
static constexpr int   PLAYER_PROJ_DMG  = 1;
static constexpr float PISTOL_CD        = 0.35f;  // s
static constexpr float RAPID_CD         = 0.12f;  // s
static constexpr float DOUBLE_CD        = 0.35f;  // s
static constexpr float DOUBLE_SPREAD    = 90.f;   // px/s vertical spread between the two shots
static constexpr float MUZZLE_FORWARD   = 0.6f;   // fraction of player width the shot spawns ahead

static constexpr float PATROL_SPEED     = 80.f;   // px/s
static constexpr float AGGRO_SPEED      = 150.f;  // px/s
static constexpr float AGGRO_RADIUS     = TILE * 5.0f;  // px
static constexpr float AGGRO_VERTICAL   = TILE * 2.5f;  // px, "roughly level"
static constexpr float DEFAULT_PATROL_HALF = TILE * 3.0f; // px each side of spawn when worldgen left bounds unset

static constexpr float PLAYER_IFRAMES   = 1.0f;   // s of invuln after a hit
static constexpr float KNOCKBACK_X      = 220.f;  // px/s
static constexpr float KNOCKBACK_Y      = -300.f; // px/s (negative = up)

static bool rects_overlap(Vector2 aPos, Vector2 aSize, Vector2 bPos, Vector2 bSize) {
    Rectangle a{ aPos.x, aPos.y, aSize.x, aSize.y };
    Rectangle b{ bPos.x, bPos.y, bSize.x, bSize.y };
    return CheckCollisionRecs(a, b);
}

// ---- player firing ----
static void fire_weapon(World& w, Player& p) {
    float dirSign = (p.facing == Facing::Right) ? 1.f : -1.f;
    Vector2 muzzle{ p.pos.x + p.size.x * 0.5f + dirSign * p.size.x * MUZZLE_FORWARD,
                     p.pos.y + p.size.y * 0.4f };

    switch (p.weapon) {
        case Weapon::Pistol:
            spawn::projectile(w, muzzle, { dirSign * PROJ_SPEED, 0 }, true, PLAYER_PROJ_DMG);
            p.fireCd = PISTOL_CD;
            break;
        case Weapon::Rapid:
            spawn::projectile(w, muzzle, { dirSign * PROJ_SPEED, 0 }, true, PLAYER_PROJ_DMG);
            p.fireCd = RAPID_CD;
            break;
        case Weapon::Double:
            spawn::projectile(w, muzzle, { dirSign * PROJ_SPEED, -DOUBLE_SPREAD }, true, PLAYER_PROJ_DMG);
            spawn::projectile(w, muzzle, { dirSign * PROJ_SPEED,  DOUBLE_SPREAD }, true, PLAYER_PROJ_DMG);
            p.fireCd = DOUBLE_CD;
            break;
    }
}

// ---- projectiles ----
static void update_projectiles(World& w, float dt) {
    for (auto& pr : w.projectiles) {
        if (!pr.alive) continue;
        pr.pos.x += pr.vel.x * dt;
        pr.pos.y += pr.vel.y * dt;
        pr.life  += dt;
        if (pr.life > PROJ_LIFE) { pr.alive = false; continue; }

        int tx = (int)std::floor(pr.pos.x / TILE);
        int ty = (int)std::floor(pr.pos.y / TILE);
        if (w.map.solid(tx, ty)) { pr.alive = false; continue; }

        bool hitPlatform = false;
        for (const auto& plat : w.platforms) {
            if (!plat.active) continue;
            if (CheckCollisionPointRec(pr.pos, plat.rect)) { hitPlatform = true; break; }
        }
        if (hitPlatform) { pr.alive = false; continue; }

        if (pr.fromPlayer) {
            for (auto& e : w.enemies) {
                if (!e.alive) continue;
                Rectangle er{ e.pos.x, e.pos.y, e.size.x, e.size.y };
                if (!CheckCollisionPointRec(pr.pos, er)) continue;
                e.hp -= pr.damage;
                pr.alive = false;
                if (e.hp <= 0) {
                    e.alive = false;
                    fx::emit(w, fx::Event::EnemyDeath, { e.pos.x + e.size.x * 0.5f, e.pos.y + e.size.y * 0.5f });
                }
                break;
            }
        }
        // ponytail: no enemy-fired projectiles exist yet (only Walkers, melee-only), so the
        // fromPlayer==false / hits-player path is unused. Add when an enemy type shoots back.
    }
    // drop dead projectiles so a sustained firefight doesn't grow the vector forever.
    std::erase_if(w.projectiles, [](const Projectile& pr) { return !pr.alive; });
}

// ---- enemy AI ----
static void damage_player(World& w, Player& p, float fromX) {
    if (p.invuln > 0 || p.invincible) return;
    p.hp -= 1;
    p.invuln = PLAYER_IFRAMES; // core::physics::step ticks this down; we only set it.
    p.vel.x = (p.pos.x < fromX) ? -KNOCKBACK_X : KNOCKBACK_X;
    p.vel.y = KNOCKBACK_Y;
    fx::emit(w, fx::Event::PlayerHit, { p.pos.x + p.size.x * 0.5f, p.pos.y + p.size.y * 0.5f });
    if (p.hp <= 0) { p.hp = 0; p.alive = false; }
}

static void update_enemies(World& w, float dt) {
    for (auto& e : w.enemies) {
        if (!e.alive) continue;
        if (e.type != EnemyType::Walker) continue; // only enemy type that exists today

        if (e.patrolMin == 0.f && e.patrolMax == 0.f) {
            e.patrolMin = e.pos.x - DEFAULT_PATROL_HALF;
            e.patrolMax = e.pos.x + DEFAULT_PATROL_HALF;
        }

        // aggro check: nearest alive player within radius and roughly level
        Player* target = nullptr;
        float bestDist = AGGRO_RADIUS + 1.f;
        Vector2 eCenter{ e.pos.x + e.size.x * 0.5f, e.pos.y + e.size.y * 0.5f };
        for (auto& p : w.players) {
            if (!p.alive) continue;
            Vector2 pCenter{ p.pos.x + p.size.x * 0.5f, p.pos.y + p.size.y * 0.5f };
            float dx = pCenter.x - eCenter.x, dy = pCenter.y - eCenter.y;
            if (std::fabs(dy) > AGGRO_VERTICAL) continue;
            float adx = std::fabs(dx);
            if (adx <= AGGRO_RADIUS && adx < bestDist) { bestDist = adx; target = &p; }
        }
        e.state = target ? EnemyState::Aggro : EnemyState::Patrol;

        float dir;
        if (e.state == EnemyState::Aggro) {
            dir = (target->pos.x >= e.pos.x) ? 1.f : -1.f;
            e.facing = dir > 0 ? Facing::Right : Facing::Left;
            e.vel.x = dir * AGGRO_SPEED;
        } else {
            if (e.pos.x <= e.patrolMin)      { e.facing = Facing::Right; }
            else if (e.pos.x >= e.patrolMax) { e.facing = Facing::Left; }
            dir = (e.facing == Facing::Right) ? 1.f : -1.f;
            e.vel.x = dir * PATROL_SPEED;
        }

        e.vel.y += GRAVITY * dt;
        physics::move_and_collide(w, e.pos, e.vel, e.size, e.onGround, dt);

        // blocked by a wall (not a bound flip): reverse patrol direction next tick
        if (e.state == EnemyState::Patrol && e.vel.x == 0.f) {
            e.facing = (e.facing == Facing::Right) ? Facing::Left : Facing::Right;
        }

        for (auto& p : w.players) {
            if (!p.alive) continue;
            if (rects_overlap(e.pos, e.size, p.pos, p.size)) damage_player(w, p, e.pos.x);
        }
    }
}

void update(World& w, float dt) {
    for (auto& p : w.players) {
        p.fireCd = std::fmax(0.f, p.fireCd - dt);
        if (p.wantFire && p.alive && p.fireCd <= 0) {
            fire_weapon(w, p);
            fx::emit(w, fx::Event::Fire, { p.pos.x + p.size.x * 0.5f, p.pos.y + p.size.y * 0.4f });
        }
    }
    update_projectiles(w, dt);
    update_enemies(w, dt);
}

} // namespace game::combat
