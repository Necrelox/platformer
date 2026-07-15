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
static constexpr float DOUBLE_SPREAD    = 90.f;   // px/s spread between the two shots, perpendicular to aim
static constexpr float MUZZLE_FORWARD   = 0.6f;   // fraction of player width the shot spawns ahead

static constexpr float PATROL_SPEED     = 80.f;   // px/s
static constexpr float AGGRO_SPEED      = 150.f;  // px/s
static constexpr float AGGRO_RADIUS     = TILE * 5.0f;  // px
static constexpr float AGGRO_VERTICAL   = TILE * 2.5f;  // px, "roughly level"
static constexpr float DEFAULT_PATROL_HALF = TILE * 3.0f; // px each side of spawn when worldgen left bounds unset

static constexpr float FLYER_SPEED      = 130.f;  // px/s drift toward target when aggro
static constexpr float FLYER_BOB_AMP    = TILE * 0.6f;
static constexpr float FLYER_BOB_FREQ   = 2.0f;   // rad/s, bob speed (phase driven by e.t)

static constexpr float SHOOTER_FIRE_CD  = 1.4f;   // s between shots
static constexpr float ENEMY_PROJ_SPEED = 420.f;  // px/s

static constexpr float CHARGER_PATROL_SPEED = 50.f;  // px/s, slower than Walker patrol
static constexpr float CHARGER_WINDUP_TIME  = 0.45f; // s held still before the charge
static constexpr float CHARGER_CHARGE_SPEED = 420.f; // px/s, fast horizontal charge

static constexpr float PLAYER_IFRAMES   = 1.0f;   // s of invuln after a hit
static constexpr float KNOCKBACK_X      = 220.f;  // px/s (player's own knockback when hit)
static constexpr float KNOCKBACK_Y      = -300.f; // px/s (negative = up)
// ponytail: separate knobs from the player's KNOCKBACK_X/Y above so tuning "punchy hit on an
// enemy" can't silently retune how hard the player gets shoved when they take damage.
static constexpr float ENEMY_KNOCKBACK_X = 320.f;  // px/s, enemy hit-reaction impulse
static constexpr float ENEMY_KNOCKBACK_Y = -380.f; // px/s (negative = up)
static constexpr float KNOCKBACK_STUN   = 0.16f;  // s an enemy yields to the impulse (all mobs feel it);
                                                   // also how long render's hit-flash shows (e.knockback > 0).
                                                   // Shorter than the old 0.20s (snappier flash) but the
                                                   // higher impulse above still nets MORE travel in that
                                                   // window than before (was 220/-300 for 0.20s = 44/-60px;
                                                   // now 320/-380 for 0.16s = ~51/-61px).

// ground-enemy jumping (Walker/Charger hop over steps/up to a higher target; Jumper hops always)
static constexpr float ENEMY_JUMP_VEL   = -720.f; // px/s hop impulse
static constexpr float GROUND_JUMP_CD   = 1.1f;   // s between Walker/Charger hops
static constexpr float JUMPER_PATROL_SPEED = 55.f;
static constexpr float JUMPER_HOP_CD    = 1.15f;  // s between Jumper hops
static constexpr float JUMPER_HOP_VX    = 190.f;  // px/s forward launch on a hop

// Bomber: aerial, hovers like a Flyer and lobs arcing bombs.
static constexpr float BOMBER_DRIFT     = 95.f;   // px/s horizontal drift to stay above target
static constexpr float BOMBER_HOVER_DY  = TILE * 3.5f; // px it tries to stay above the target
static constexpr float BOMBER_FIRE_CD   = 2.0f;   // s between bombs
static constexpr float BOMBER_BOB_AMP   = TILE * 0.4f;
static constexpr float BOMBER_BOB_FREQ  = 1.6f;
static constexpr float BOMB_GRAVITY     = 1600.f; // px/s^2 on the bomb projectile
static constexpr float BOMB_VX_MAX      = 260.f;  // px/s cap on the toss's horizontal component
static constexpr float BOMB_VY          = -280.f; // px/s initial upward toss
static constexpr float EXPLOSION_RADIUS = TILE * 1.5f;

// Enemy AI eases toward its desired velocity (like core's player `approach`) instead of
// snapping to it — a hard `e.vel.x = dir*SPEED` every tick would erase a knockback impulse
// the same frame it's applied, before it ever moves the enemy.
static constexpr float ENEMY_ACCEL      = 600.f;  // px/s^2

// ponytail: enemies further behind the lead player than this just freeze (no AI tick) instead
// of simulating off-screen forever. Index-aligned vectors are never erased, so this is purely
// a CPU shortcut — enemies ahead of the lead always update normally.
static constexpr float OFFSCREEN_SKIP_DIST = SCREEN_W * 1.5f;

static bool rects_overlap(Vector2 aPos, Vector2 aSize, Vector2 bPos, Vector2 bSize) {
    Rectangle a{ aPos.x, aPos.y, aSize.x, aSize.y };
    Rectangle b{ bPos.x, bPos.y, bSize.x, bSize.y };
    return CheckCollisionRecs(a, b);
}

// ponytail: duplicated from core/game.cpp's file-local `approach` (not exported via api.hpp) —
// same 3-liner, used so enemy AI eases into its target velocity instead of snapping to it.
static float approach(float cur, float target, float step) {
    if (cur < target) return std::fmin(cur + step, target);
    if (cur > target) return std::fmax(cur - step, target);
    return cur;
}

// ---- player firing ----
static void fire_weapon(World& w, Player& p) {
    Vector2 dir;
    float aimLen = std::sqrt(p.aim.x * p.aim.x + p.aim.y * p.aim.y);
    if (aimLen > 0.0001f) {
        dir = { p.aim.x / aimLen, p.aim.y / aimLen };
        p.facing = (p.aim.x >= 0.f) ? Facing::Right : Facing::Left;
    } else {
        float dirSign = (p.facing == Facing::Right) ? 1.f : -1.f;
        dir = { dirSign, 0.f };
    }
    Vector2 perp{ -dir.y, dir.x }; // perpendicular to aim, for double-shot spread

    Vector2 muzzle{ p.pos.x + p.size.x * 0.5f + dir.x * p.size.x * MUZZLE_FORWARD,
                     p.pos.y + p.size.y * 0.4f + dir.y * p.size.x * MUZZLE_FORWARD };

    switch (p.weapon) {
        case Weapon::Pistol:
            spawn::projectile(w, muzzle, { dir.x * PROJ_SPEED, dir.y * PROJ_SPEED }, true, PLAYER_PROJ_DMG);
            p.fireCd = PISTOL_CD;
            break;
        case Weapon::Rapid:
            spawn::projectile(w, muzzle, { dir.x * PROJ_SPEED, dir.y * PROJ_SPEED }, true, PLAYER_PROJ_DMG);
            p.fireCd = RAPID_CD;
            break;
        case Weapon::Double:
            spawn::projectile(w, muzzle, { dir.x * PROJ_SPEED - perp.x * DOUBLE_SPREAD,
                                            dir.y * PROJ_SPEED - perp.y * DOUBLE_SPREAD }, true, PLAYER_PROJ_DMG);
            spawn::projectile(w, muzzle, { dir.x * PROJ_SPEED + perp.x * DOUBLE_SPREAD,
                                            dir.y * PROJ_SPEED + perp.y * DOUBLE_SPREAD }, true, PLAYER_PROJ_DMG);
            p.fireCd = DOUBLE_CD;
            break;
    }
}

// ---- damage / death ----
static void damage_player(World& w, Player& p, float fromX) {
    if (p.dying) return;                       // already playing the death animation
    if (p.invuln > 0 || p.invincible) return;
    p.hp -= 1;
    p.invuln = PLAYER_IFRAMES; // core::physics::step ticks this down; we only set it.
    p.vel.x = (p.pos.x < fromX) ? -KNOCKBACK_X : KNOCKBACK_X;
    p.vel.y = KNOCKBACK_Y;
    w.shake = std::fmax(w.shake, 0.35f);
    fx::emit(w, fx::Event::PlayerHit, { p.pos.x + p.size.x * 0.5f, p.pos.y + p.size.y * 0.5f });
    if (p.hp <= 0) {
        p.hp = 0;
        p.dying      = true;   // core counts deathTimer down, floats the ghost, then alive=false
        p.deathTimer = DEATH_ANIM_TIME;
        fx::emit(w, fx::Event::PlayerDeath, { p.pos.x + p.size.x * 0.5f, p.pos.y + p.size.y * 0.5f });
    }
}

// ---- projectiles ----
static void update_projectiles(World& w, float dt) {
    for (auto& pr : w.projectiles) {
        if (!pr.alive) continue;
        pr.vel.y += pr.gravity * dt;   // 0 for straight shots; >0 makes bombs arc
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
                // knockback, like the player's own knockback-on-damage: push along the
                // projectile's travel direction plus a small upward pop. knockback stun makes
                // EVERY mob type yield to the impulse for a moment (see the guard atop each AI).
                e.vel.x = (pr.vel.x >= 0.f) ? ENEMY_KNOCKBACK_X : -ENEMY_KNOCKBACK_X;
                e.vel.y = ENEMY_KNOCKBACK_Y;
                e.knockback = KNOCKBACK_STUN;
                pr.alive = false;
                Vector2 ec{ e.pos.x + e.size.x * 0.5f, e.pos.y + e.size.y * 0.5f };
                if (e.hp <= 0) {
                    e.alive = false;
                    fx::emit(w, fx::Event::EnemyDeath, ec);
                    w.shake = std::fmax(w.shake, 0.15f);
                } else {
                    fx::emit(w, fx::Event::EnemyHit, ec); // NEW: sound + spark on a non-fatal hit
                }
                break;
            }
        } else {
            for (auto& p : w.players) {
                if (!p.alive || p.dying) continue;
                Rectangle pr_r{ p.pos.x, p.pos.y, p.size.x, p.size.y };
                if (!CheckCollisionPointRec(pr.pos, pr_r)) continue;
                // ponytail: bias fromX to a point "behind" the shot so damage_player's
                // push-away-from-fromX knockback continues in the projectile's travel direction.
                damage_player(w, p, pr.pos.x - pr.vel.x);
                pr.alive = false;
                break;
            }
        }
    }
    // Bombs explode wherever they died THIS tick (fx + shake + AoE), exactly once — they're
    // erased just below, so a bomb can't be processed on a later tick. A player hit directly by
    // the bomb already has i-frames, so the AoE won't double-count them; it catches the others.
    for (auto& pr : w.projectiles) {
        if (!pr.bomb || pr.alive) continue;
        fx::emit(w, fx::Event::Explosion, pr.pos);
        w.shake = std::fmax(w.shake, 0.6f);
        for (auto& p : w.players) {
            if (!p.alive || p.dying) continue;
            Vector2 pc{ p.pos.x + p.size.x * 0.5f, p.pos.y + p.size.y * 0.5f };
            float dx = pc.x - pr.pos.x, dy = pc.y - pr.pos.y;
            if (dx * dx + dy * dy <= EXPLOSION_RADIUS * EXPLOSION_RADIUS)
                damage_player(w, p, pr.pos.x);
        }
    }
    // drop dead projectiles so a sustained firefight doesn't grow the vector forever.
    std::erase_if(w.projectiles, [](const Projectile& pr) { return !pr.alive; });
}

// ---- enemy AI ----

// nearest alive (non-dying) player within AGGRO_RADIUS horizontally and "roughly level" vertically.
static Player* find_target_level(World& w, Vector2 eCenter) {
    Player* target = nullptr;
    float bestDist = AGGRO_RADIUS + 1.f;
    for (auto& p : w.players) {
        if (!p.alive || p.dying) continue;
        Vector2 pCenter{ p.pos.x + p.size.x * 0.5f, p.pos.y + p.size.y * 0.5f };
        float dx = pCenter.x - eCenter.x, dy = pCenter.y - eCenter.y;
        if (std::fabs(dy) > AGGRO_VERTICAL) continue;
        float adx = std::fabs(dx);
        if (adx <= AGGRO_RADIUS && adx < bestDist) { bestDist = adx; target = &p; }
    }
    return target;
}

// nearest alive (non-dying) player within a full-circle radius (for Flyer, which chases in x AND y).
static Player* find_target_radius(World& w, Vector2 eCenter) {
    Player* target = nullptr;
    float bestDist2 = AGGRO_RADIUS * AGGRO_RADIUS;
    for (auto& p : w.players) {
        if (!p.alive || p.dying) continue;
        Vector2 pCenter{ p.pos.x + p.size.x * 0.5f, p.pos.y + p.size.y * 0.5f };
        float dx = pCenter.x - eCenter.x, dy = pCenter.y - eCenter.y;
        float d2 = dx * dx + dy * dy;
        if (d2 <= bestDist2) { bestDist2 = d2; target = &p; }
    }
    return target;
}

// While knocked back, a mob yields to the impulse: no AI velocity override, just carry the hit
// through physics for KNOCKBACK_STUN seconds. Ground types keep gravity; aerial types coast.
// Returns true if it handled this tick (caller returns early). This is what makes knockback
// visible on EVERY mob — including the idle Flyer/Shooter/Bomber that used to erase it.
static bool yield_knockback(World& w, Enemy& e, float dt, bool grounded) {
    if (e.knockback <= 0.f) return false;
    e.knockback -= dt;
    if (grounded) e.vel.y += GRAVITY * dt;
    physics::move_and_collide(w, e.pos, e.vel, e.size, e.onGround, dt);
    return true;
}

// A grounded hop (Walker/Charger clear a step or reach a higher target; Jumper's main move).
static void ground_hop(Enemy& e, float vx, float cooldown) {
    if (!e.onGround || e.jumpCd > 0.f) return;
    e.vel.y   = ENEMY_JUMP_VEL;
    e.vel.x   = vx;
    e.jumpCd  = cooldown;
    e.onGround = false;
}

// Solid tile directly in front at foot height => a step/wall worth hopping.
static bool step_ahead(const World& w, const Enemy& e, float dir) {
    float fx = (dir > 0) ? e.pos.x + e.size.x + 2.f : e.pos.x - 2.f;
    int tx = (int)std::floor(fx / TILE);
    int ty = (int)std::floor((e.pos.y + e.size.y - 4.f) / TILE);
    return w.map.solid(tx, ty);
}

// PATROL-only guard: don't blindly walk off a ledge or into a hazard tile. Aggro chasing still
// hops gaps/steps via ground_hop (untouched) — this only gates the slow Patrol-state walk.
// ponytail: a patrol boxed in by ledges/hazards on both sides will flip facing every tick and
// twitch in place instead of picking a side — harmless (velocity never builds so it just sits),
// add a "trapped" latch only if that ever reads badly in practice.
static bool patrol_should_turn(const World& w, const Enemy& e, float dir) {
    float fx = (dir > 0) ? e.pos.x + e.size.x + 2.f : e.pos.x - 2.f;
    int tx      = (int)std::floor(fx / TILE);
    int footTy  = (int)std::floor((e.pos.y + e.size.y - 4.f) / TILE);
    int belowTy = footTy + 1;
    if (w.map.hazard(tx, footTy)) return true;  // spike/lava wall directly ahead
    if (!w.map.solid(tx, belowTy)) return true; // no solid floor ahead (pit, or a hazard floor)
    return false;
}

static void update_walker(World& w, Enemy& e, float dt) {
    if (yield_knockback(w, e, dt, /*grounded=*/true)) return;
    Vector2 eCenter{ e.pos.x + e.size.x * 0.5f, e.pos.y + e.size.y * 0.5f };
    Player* target = find_target_level(w, eCenter);
    e.state = target ? EnemyState::Aggro : EnemyState::Patrol;

    float dir;
    if (e.state == EnemyState::Aggro) {
        dir = (target->pos.x >= e.pos.x) ? 1.f : -1.f;
        e.facing = dir > 0 ? Facing::Right : Facing::Left;
        e.vel.x = approach(e.vel.x, dir * AGGRO_SPEED, ENEMY_ACCEL * dt);
        // ground enemies can jump now: hop over a step ahead or up toward a higher target
        bool targetAbove = (target->pos.y + target->size.y) < e.pos.y - TILE * 0.5f;
        if (targetAbove || step_ahead(w, e, dir)) ground_hop(e, dir * AGGRO_SPEED * 0.7f, GROUND_JUMP_CD);
    } else {
        if (e.pos.x <= e.patrolMin)      { e.facing = Facing::Right; }
        else if (e.pos.x >= e.patrolMax) { e.facing = Facing::Left; }
        dir = (e.facing == Facing::Right) ? 1.f : -1.f;
        if (e.onGround && patrol_should_turn(w, e, dir)) {
            e.facing = (e.facing == Facing::Right) ? Facing::Left : Facing::Right;
            dir = -dir;
        }
        e.vel.x = approach(e.vel.x, dir * PATROL_SPEED, ENEMY_ACCEL * dt);
    }

    e.vel.y += GRAVITY * dt;
    physics::move_and_collide(w, e.pos, e.vel, e.size, e.onGround, dt);

    // blocked by a wall (not a bound flip): reverse patrol direction next tick
    if (e.state == EnemyState::Patrol && e.vel.x == 0.f) {
        e.facing = (e.facing == Facing::Right) ? Facing::Left : Facing::Right;
    }
}

// Ignores gravity; hovers around baseY with a sine bob while idle, drifts toward the target
// in x AND y once aggroed. Still runs through move_and_collide while chasing so it doesn't fly
// through solid ground.
static void update_flyer(World& w, Enemy& e, float dt) {
    e.t += dt; // bob phase (keep advancing through knockback so the bob doesn't jump on resume)
    if (yield_knockback(w, e, dt, /*grounded=*/false)) return;
    Vector2 eCenter{ e.pos.x + e.size.x * 0.5f, e.pos.y + e.size.y * 0.5f };
    Player* target = find_target_radius(w, eCenter);
    e.state = target ? EnemyState::Aggro : EnemyState::Patrol;

    if (e.state == EnemyState::Aggro) {
        Vector2 tCenter{ target->pos.x + target->size.x * 0.5f, target->pos.y + target->size.y * 0.5f };
        float dx = tCenter.x - eCenter.x, dy = tCenter.y - eCenter.y;
        float dist = std::sqrt(dx * dx + dy * dy);
        Vector2 wantVel = (dist > 1.f) ? Vector2{ dx / dist * FLYER_SPEED, dy / dist * FLYER_SPEED }
                                        : Vector2{ 0, 0 };
        // eased (not snapped) so a knockback impulse from a hit survives into this frame's move.
        e.vel.x = approach(e.vel.x, wantVel.x, ENEMY_ACCEL * dt);
        e.vel.y = approach(e.vel.y, wantVel.y, ENEMY_ACCEL * dt);
        e.facing = dx >= 0 ? Facing::Right : Facing::Left;
        physics::move_and_collide(w, e.pos, e.vel, e.size, e.onGround, dt);
    } else {
        // idle hover: ease toward the bob curve (not a hard snap) so a knockback push visibly
        // decays back instead of teleporting — knockback now reads on idle Flyers too.
        e.vel = { 0, 0 };
        float targetY = e.baseY + std::sin(e.t * FLYER_BOB_FREQ) * FLYER_BOB_AMP;
        e.pos.y += (targetY - e.pos.y) * std::fmin(1.0f, 8.0f * dt);
    }
}

// Mostly stationary; fires an enemy projectile at a target that's in range and roughly in line,
// throttled by fireCd.
static void update_shooter(World& w, Enemy& e, float dt) {
    e.fireCd = std::fmax(0.f, e.fireCd - dt);
    if (yield_knockback(w, e, dt, /*grounded=*/true)) return;
    e.vel.x = approach(e.vel.x, 0.f, ENEMY_ACCEL * dt); // "little/no patrol" -> stationary turret

    Vector2 eCenter{ e.pos.x + e.size.x * 0.5f, e.pos.y + e.size.y * 0.5f };
    // Full-circle radius (not find_target_level's ~2.5-tile vertical band) — on the tall map
    // players are usually above/below, not "roughly level", so the level check barely engaged
    // and shots read as a fixed near-horizontal poke. Aim true at whoever's actually nearest.
    Player* target = find_target_radius(w, eCenter);
    e.state = target ? EnemyState::Aggro : EnemyState::Patrol;

    e.vel.y += GRAVITY * dt;
    physics::move_and_collide(w, e.pos, e.vel, e.size, e.onGround, dt);

    if (target) {
        e.facing = (target->pos.x >= e.pos.x) ? Facing::Right : Facing::Left;
        if (e.fireCd <= 0.f) {
            // aim at the target's center (not just flat) — smarter than a fixed horizontal shot
            Vector2 ec{ e.pos.x + e.size.x * 0.5f, e.pos.y + e.size.y * 0.4f };
            Vector2 tc{ target->pos.x + target->size.x * 0.5f, target->pos.y + target->size.y * 0.5f };
            float dx = tc.x - ec.x, dy = tc.y - ec.y;
            float len = std::sqrt(dx * dx + dy * dy);
            Vector2 dir = (len > 1.f) ? Vector2{ dx / len, dy / len }
                                      : Vector2{ e.facing == Facing::Right ? 1.f : -1.f, 0.f };
            float dirSign = (e.facing == Facing::Right) ? 1.f : -1.f;
            Vector2 muzzle{ ec.x + dirSign * e.size.x * 0.5f, ec.y };
            spawn::projectile(w, muzzle, { dir.x * ENEMY_PROJ_SPEED, dir.y * ENEMY_PROJ_SPEED }, /*fromPlayer=*/false, 1);
            e.fireCd = SHOOTER_FIRE_CD;
        }
    }
}

// Slow patrol; on aggro, a brief wind-up (e.t counts down) then a fast horizontal charge.
static void update_charger(World& w, Enemy& e, float dt) {
    if (yield_knockback(w, e, dt, /*grounded=*/true)) return;
    Vector2 eCenter{ e.pos.x + e.size.x * 0.5f, e.pos.y + e.size.y * 0.5f };
    Player* target = find_target_level(w, eCenter);
    bool wasAggro = (e.state == EnemyState::Aggro);
    e.state = target ? EnemyState::Aggro : EnemyState::Patrol;

    if (e.state == EnemyState::Aggro) {
        if (!wasAggro) e.t = CHARGER_WINDUP_TIME; // just spotted a target: wind up before charging
        e.facing = (target->pos.x >= e.pos.x) ? Facing::Right : Facing::Left;
        float dir = (e.facing == Facing::Right) ? 1.f : -1.f;
        if (e.t > 0.f) {
            e.t -= dt;
            e.vel.x = approach(e.vel.x, 0.f, ENEMY_ACCEL * dt); // winding up: hold still, telegraphs the charge
        } else {
            e.vel.x = approach(e.vel.x, dir * CHARGER_CHARGE_SPEED, ENEMY_ACCEL * dt);
            // hop over a step in the charge lane, or up toward a higher target
            bool targetAbove = (target->pos.y + target->size.y) < e.pos.y - TILE * 0.5f;
            if (targetAbove || step_ahead(w, e, dir)) ground_hop(e, dir * CHARGER_CHARGE_SPEED * 0.6f, GROUND_JUMP_CD);
        }
    } else {
        e.t = 0.f;
        if (e.pos.x <= e.patrolMin)      { e.facing = Facing::Right; }
        else if (e.pos.x >= e.patrolMax) { e.facing = Facing::Left; }
        float dir = (e.facing == Facing::Right) ? 1.f : -1.f;
        if (e.onGround && patrol_should_turn(w, e, dir)) {
            e.facing = (e.facing == Facing::Right) ? Facing::Left : Facing::Right;
            dir = -dir;
        }
        e.vel.x = approach(e.vel.x, dir * CHARGER_PATROL_SPEED, ENEMY_ACCEL * dt);
    }

    e.vel.y += GRAVITY * dt;
    physics::move_and_collide(w, e.pos, e.vel, e.size, e.onGround, dt);

    if (e.state == EnemyState::Patrol && e.vel.x == 0.f) {
        e.facing = (e.facing == Facing::Right) ? Facing::Left : Facing::Right;
    }
}

// Jumper: a ground hopper. Patrols slowly; when it notices a target (in radius, incl. above)
// it crouches and leaps toward it on a cadence. Full knockback like every mob.
static void update_jumper(World& w, Enemy& e, float dt) {
    if (yield_knockback(w, e, dt, /*grounded=*/true)) return;
    Vector2 eCenter{ e.pos.x + e.size.x * 0.5f, e.pos.y + e.size.y * 0.5f };
    Player* target = find_target_radius(w, eCenter); // radius: it will hop up at players above it
    e.state = target ? EnemyState::Aggro : EnemyState::Patrol;

    if (e.state == EnemyState::Aggro) {
        float dir = (target->pos.x >= e.pos.x) ? 1.f : -1.f;
        e.facing = dir > 0 ? Facing::Right : Facing::Left;
        if (e.onGround) {                                  // gather on the ground, then leap
            e.vel.x = approach(e.vel.x, 0.f, ENEMY_ACCEL * dt);
            ground_hop(e, dir * JUMPER_HOP_VX, JUMPER_HOP_CD); // no-op until jumpCd is ready
        } // in air: keep momentum (don't fight the arc)
    } else {
        if (e.pos.x <= e.patrolMin)      e.facing = Facing::Right;
        else if (e.pos.x >= e.patrolMax) e.facing = Facing::Left;
        float dir = (e.facing == Facing::Right) ? 1.f : -1.f;
        if (e.onGround && patrol_should_turn(w, e, dir)) {
            e.facing = (e.facing == Facing::Right) ? Facing::Left : Facing::Right;
            dir = -dir;
        }
        e.vel.x = approach(e.vel.x, dir * JUMPER_PATROL_SPEED, ENEMY_ACCEL * dt);
    }

    e.vel.y += GRAVITY * dt;
    physics::move_and_collide(w, e.pos, e.vel, e.size, e.onGround, dt);
    if (e.state == EnemyState::Patrol && e.vel.x == 0.f)
        e.facing = (e.facing == Facing::Right) ? Facing::Left : Facing::Right;
}

// Bomber: aerial, hovers (no gravity) and lobs arcing bombs. Stays above its target and drops
// gravity-affected bomb projectiles that explode on death (handled in update_projectiles).
static void update_bomber(World& w, Enemy& e, float dt) {
    e.t += dt;
    if (yield_knockback(w, e, dt, /*grounded=*/false)) return;
    e.fireCd = std::fmax(0.f, e.fireCd - dt);
    Vector2 eCenter{ e.pos.x + e.size.x * 0.5f, e.pos.y + e.size.y * 0.5f };
    Player* target = find_target_radius(w, eCenter);
    e.state = target ? EnemyState::Aggro : EnemyState::Patrol;

    if (e.state == EnemyState::Aggro) {
        Vector2 tc{ target->pos.x + target->size.x * 0.5f, target->pos.y + target->size.y * 0.5f };
        float dx = tc.x - eCenter.x;
        float wantVx = (std::fabs(dx) > TILE * 0.5f) ? (dx > 0 ? BOMBER_DRIFT : -BOMBER_DRIFT) : 0.f;
        e.vel.x = approach(e.vel.x, wantVx, ENEMY_ACCEL * dt);
        e.facing = dx >= 0 ? Facing::Right : Facing::Left;
        // hold station ~BOMBER_HOVER_DY above the target (P-controller on altitude, clamped)
        float wantY = target->pos.y - BOMBER_HOVER_DY + std::sin(e.t * BOMBER_BOB_FREQ) * BOMBER_BOB_AMP;
        float desiredVy = std::clamp((wantY - e.pos.y) * 3.0f, -180.f, 180.f);
        e.vel.y = approach(e.vel.y, desiredVy, ENEMY_ACCEL * dt);
        physics::move_and_collide(w, e.pos, e.vel, e.size, e.onGround, dt);

        if (e.fireCd <= 0.f && std::fabs(dx) < AGGRO_RADIUS) {           // lob a bomb
            Vector2 muzzle{ eCenter.x, e.pos.y + e.size.y };
            float vx = std::clamp(dx * 0.8f, -BOMB_VX_MAX, BOMB_VX_MAX);
            Projectile& b = spawn::projectile(w, muzzle, { vx, BOMB_VY }, /*fromPlayer=*/false, 1);
            b.gravity = BOMB_GRAVITY;
            b.bomb    = true;
            e.fireCd  = BOMBER_FIRE_CD;
        }
    } else {
        // idle hover around baseY (eased, like Flyer — so knockback decays back naturally)
        e.vel = { 0, 0 };
        float targetY = e.baseY + std::sin(e.t * BOMBER_BOB_FREQ) * BOMBER_BOB_AMP;
        e.pos.y += (targetY - e.pos.y) * std::fmin(1.0f, 6.0f * dt);
    }
}

static void apply_spike_damage(World& w) {
    for (auto& p : w.players) {
        if (!p.alive || p.dying) continue;
        int tx0 = (int)std::floor(p.pos.x / TILE);
        int tx1 = (int)std::floor((p.pos.x + p.size.x) / TILE);
        int ty0 = (int)std::floor(p.pos.y / TILE);
        int ty1 = (int)std::floor((p.pos.y + p.size.y) / TILE);
        for (int tx = tx0; tx <= tx1; ++tx) {
            bool hit = false;
            for (int ty = ty0; ty <= ty1; ++ty) {
                if (w.map.hazard(tx, ty)) { hit = true; break; }
            }
            if (hit) { damage_player(w, p, (tx + 0.5f) * TILE); break; }
        }
    }
}

static void update_enemies(World& w, float dt) {
    Vector2 lead = lead_pos(w);
    for (auto& e : w.enemies) {
        if (!e.alive) continue;
        // CPU shortcut: enemies far behind the lead player don't need to think this tick.
        if (e.pos.x < lead.x - OFFSCREEN_SKIP_DIST) continue;

        if (e.patrolMin == 0.f && e.patrolMax == 0.f) {
            e.patrolMin = e.pos.x - DEFAULT_PATROL_HALF;
            e.patrolMax = e.pos.x + DEFAULT_PATROL_HALF;
        }
        if (e.jumpCd > 0.f) e.jumpCd -= dt; // ground-hop cooldown, shared by Walker/Charger/Jumper

        switch (e.type) {
            case EnemyType::Walker:  update_walker(w, e, dt);  break;
            case EnemyType::Flyer:   update_flyer(w, e, dt);   break;
            case EnemyType::Shooter: update_shooter(w, e, dt); break;
            case EnemyType::Charger: update_charger(w, e, dt); break;
            case EnemyType::Jumper:  update_jumper(w, e, dt);  break;
            case EnemyType::Bomber:  update_bomber(w, e, dt);  break;
        }

        for (auto& p : w.players) {
            if (!p.alive || p.dying) continue;
            if (rects_overlap(e.pos, e.size, p.pos, p.size)) damage_player(w, p, e.pos.x);
        }
    }
}

void update(World& w, float dt) {
    for (auto& p : w.players) {
        p.fireCd = std::fmax(0.f, p.fireCd - dt);
        if (p.wantFire && p.alive && !p.dying && p.fireCd <= 0) {
            fire_weapon(w, p);
            fx::emit(w, fx::Event::Fire, { p.pos.x + p.size.x * 0.5f, p.pos.y + p.size.y * 0.4f });
        }
    }
    update_projectiles(w, dt);
    update_enemies(w, dt);
    apply_spike_damage(w);
}

} // namespace game::combat
