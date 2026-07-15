#include "core/api.hpp"
#include <cmath>
#include <vector>

namespace game {

// ---- feel knobs (tune in playtest) ----
static constexpr float GRAVITY    = 2200; // px/s^2
static constexpr float MOVE_SPEED = 300;  // px/s
static constexpr float ACCEL      = 3000; // px/s^2 toward target horizontal speed
static constexpr float JUMP_VEL   = -760; // px/s
static constexpr float JUMP_CUT   = 0.45f;// rise velocity retained when jump released early
static constexpr float COYOTE     = 0.08f;// s
static constexpr float MAX_FALL   = 1400; // px/s
static constexpr float VOID_Y     = (WORLD_H + 8) * (float)TILE; // fall past this = death
static constexpr float GHOST_RISE = -170; // px/s upward drift while the death animation plays

// ---- mobility: double jump + dash (new player abilities) ----
static constexpr int   MAX_AIR_JUMPS = 1;    // extra mid-air jumps (1 = a single double jump)
static constexpr float DASH_SPEED    = 1400; // px/s during a dash (well under TILE/FIXED tunnelling limit)
static constexpr float DASH_TIME     = 0.15f;// s the dash lasts (gravity suspended, vel.x pinned)
static constexpr float DASH_CD       = 0.7f; // s cooldown between dashes
// Traversability budget handed to worldgen (derived from the numbers above; keep in sync if tuned):
//   single jump  ~= 5 tiles up / ~8 tiles across   (JUMP_VEL/GRAVITY, MOVE_SPEED)
//   double jump  ~= 9 tiles up                      (second JUMP_VEL impulse)
//   dash         ~= DASH_SPEED*DASH_TIME/TILE ~= 8-9 tiles across, horizontal
// Mandatory floor path must stay crossable by a SINGLE jump (worldgen keeps MAX_GAP<=5, MAX_STEP<=4);
// double jump / dash only ever gate OPTIONAL elevated routes.

static constexpr float SPAWN_Y      = (WORLD_H - 5) * (float)TILE; // just above the baseline ground

// ---- camera shake (juice) ----
static constexpr float SHAKE_DECAY  = 0.5f;  // s for full trauma (1.0) to decay to 0
static constexpr float SHAKE_MAX_PX = 14.0f; // px peak camera offset at full trauma

uint64_t random_seed() {
    // Scramble a high-res clock read so each launch gets a distinct procedural map. Not for
    // anything security-sensitive — just map variety. (splitmix64 finalizer on the time count.)
    uint64_t s = (uint64_t)std::chrono::high_resolution_clock::now().time_since_epoch().count();
    s ^= s >> 30; s *= 0xbf58476d1ce4e5b9ULL;
    s ^= s >> 27; s *= 0x94d049bb133111ebULL;
    s ^= s >> 31;
    return s ? s : 1;
}

static float approach(float cur, float target, float step) {
    if (cur < target) return std::fmin(cur + step, target);
    if (cur > target) return std::fmax(cur - step, target);
    return cur;
}

// ---- spawn helpers ----
namespace spawn {
Platform& platform(World& w, Rectangle rect, PlatformKind kind) {
    Platform p; p.rect = rect; p.kind = kind;
    return w.platforms.emplace_back(p);
}
Enemy& enemy(World& w, Vector2 pos, EnemyType type) {
    Enemy e; e.pos = pos; e.type = type;
    return w.enemies.emplace_back(e);
}
Item& item(World& w, Vector2 pos, ItemType type) {
    Item it; it.pos = pos; it.type = type;
    return w.items.emplace_back(it);
}
Projectile& projectile(World& w, Vector2 pos, Vector2 vel, bool fromPlayer, int damage) {
    Projectile pr; pr.pos = pos; pr.vel = vel; pr.fromPlayer = fromPlayer; pr.damage = damage;
    return w.projectiles.emplace_back(pr);
}
} // namespace spawn

// ---- physics ----
namespace physics {

static void gather_solids(const World& w, Rectangle box, std::vector<Rectangle>& out) {
    int tx0 = (int)std::floor(box.x / TILE) - 1;
    int tx1 = (int)std::floor((box.x + box.width) / TILE) + 1;
    int ty0 = (int)std::floor(box.y / TILE) - 1;
    int ty1 = (int)std::floor((box.y + box.height) / TILE) + 1;
    for (int tx = tx0; tx <= tx1; ++tx)
        for (int ty = ty0; ty <= ty1; ++ty)
            if (w.map.solid(tx, ty))
                out.push_back({ (float)tx * TILE, (float)ty * TILE, (float)TILE, (float)TILE });
    for (const auto& p : w.platforms)
        if (p.active) out.push_back(p.rect);
}

// ponytail: naive per-rect resolution, one axis at a time. Fine at these speeds; if fast
// tunnelling shows up, add swept AABB.
static void resolve(const World& w, Rectangle& box, Vector2& vel, int axis, bool& onGround) {
    // ponytail: reused static scratch — resolve() runs per entity per axis per tick; a fresh
    // vector each call was the one real per-frame allocation hotspot. Single-threaded, non-
    // reentrant, so a static buffer is safe and keeps its capacity across calls.
    static std::vector<Rectangle> solids;
    solids.clear();
    gather_solids(w, box, solids);
    for (const auto& s : solids) {
        if (!CheckCollisionRecs(box, s)) continue;
        if (axis == 0) {
            if (vel.x > 0)      box.x = s.x - box.width;
            else if (vel.x < 0) box.x = s.x + s.width;
            vel.x = 0;
        } else {
            if (vel.y > 0)      { box.y = s.y - box.height; onGround = true; }
            else if (vel.y < 0) box.y = s.y + s.height;
            vel.y = 0;
        }
    }
}

void move_and_collide(const World& w, Vector2& pos, Vector2& vel, Vector2 size,
                      bool& onGround, float dt) {
    onGround = false;
    if (vel.y > MAX_FALL) vel.y = MAX_FALL;
    Rectangle box{ pos.x, pos.y, size.x, size.y };
    box.x += vel.x * dt; resolve(w, box, vel, 0, onGround);
    box.y += vel.y * dt; resolve(w, box, vel, 1, onGround);
    pos = { box.x, box.y };
}

void step(World& w, float dt) {
    for (auto& p : w.players) {
        if (!p.alive) continue;
        if (p.dying) { // ghost: drift by velocity only — no gravity, no collision
            p.pos.x += p.vel.x * dt;
            p.pos.y += p.vel.y * dt;
            continue;
        }
        if (p.dashCd > 0) p.dashCd -= dt;
        if (p.dashTimer > 0) {
            // Active dash: pin horizontal velocity, suspend gravity — a clean flat burst.
            p.dashTimer -= dt;
            p.vel.x = p.dashDir * DASH_SPEED;
            p.vel.y = 0;
        } else {
            p.vel.y += GRAVITY * dt;
        }
        move_and_collide(w, p.pos, p.vel, p.size, p.onGround, dt);
        if (p.onGround) { p.coyote = COYOTE; p.airJumps = 0; }
        else if (p.coyote > 0) p.coyote -= dt;
        if (p.invuln > 0) p.invuln -= dt;
        float front = p.pos.x + p.size.x;
        if (front > p.distance) p.distance = front;
    }
}

} // namespace physics

// ---- core ----
Vector2 lead_pos(const World& w) {
    Vector2 best{ 0, SCREEN_H * 0.3f };
    bool got = false;
    for (const auto& p : w.players) {
        if (!got || p.pos.x > best.x) { best = p.pos; got = true; }
    }
    return best;
}

void follow_camera(World& w, float dt) {
    Vector2 lead = lead_pos(w);
    float k = std::fmin(1.0f, 10.0f * dt);
    w.cam.target.x += (lead.x - w.cam.target.x) * k;
    w.cam.target.y += (lead.y - w.cam.target.y) * k;
    // Screen shake: decay trauma, apply a quadratic-falloff jitter on top of the followed target.
    // Phase from GetTime() (reads 0 headlessly -> zero shake in tests, which is fine).
    if (w.shake > 0) {
        w.shake = std::fmax(0.0f, w.shake - dt / SHAKE_DECAY);
        float mag = w.shake * w.shake * SHAKE_MAX_PX;
        float a   = (float)GetTime() * 47.0f;
        w.cam.target.x += std::sin(a)        * mag;
        w.cam.target.y += std::cos(a * 1.7f) * mag;
    }
}

Color skin_color(int skin) {
    switch (skin) {
        case 0: return RED;
        case 1: return GREEN;
        case 2: return SKYBLUE;
        case 3: return YELLOW;
    }
    return RAYWHITE;
}

void new_game(Game& g, uint64_t seed) {
    g.world = World{};
    g.world.seed = seed ? seed : random_seed();
    worldgen::reset(g.world, g.world.seed);
    Player p;
    p.id = 0;
    p.pos = { TILE * 3.0f, SPAWN_Y };
    p.skin = 0;
    p.color = skin_color(0);
    g.world.players.clear();
    g.world.players.push_back(p);
    g.world.localId = 0;
    g.world.cam.target = p.pos;
    g.net.role = NetRole::Solo;
    // Reset BOTH the per-player intent array AND the local keyboard scratch. g.input carries
    // monotonic jumpSeq/dashSeq press counters that outlive a game; without this reset a stale
    // seq from the previous run trips a phantom jump/dash on the first tick of the new one
    // (the "dashes on respawn without a keypress" bug).
    for (auto& in : g.inputs) in = InputState{};
    g.input = InputState{};
    g.mode = Mode::Playing;
}

void start_game_from_lobby(Game& g) {
    World& w = g.world;
    w = World{};
    w.seed = g.net.lobby.seed;
    worldgen::reset(w, w.seed);
    w.players.clear();
    int slotOrder = 0;
    for (int s = 0; s < MAX_PLAYERS; ++s) {
        if (!g.net.lobby.slots[s].used) continue;
        Player p;
        p.id    = s;
        p.skin  = g.net.lobby.slots[s].skin;
        p.color = skin_color(p.skin);
        p.pos   = { TILE * (3.0f + slotOrder * 1.5f), SPAWN_Y };
        w.players.push_back(p);
        ++slotOrder;
    }
    if (w.players.empty()) { // safety net
        Player p; p.id = g.net.lobby.localSlot; p.color = skin_color(0);
        p.pos = { TILE * 3.0f, SPAWN_Y }; w.players.push_back(p);
    }
    w.localId = 0;
    for (size_t i = 0; i < w.players.size(); ++i)
        if (w.players[i].id == g.net.lobby.localSlot) w.localId = (int)i;
    w.cam.target = w.players[w.localId].pos;
    for (auto& in : g.inputs) in = InputState{}; // clear stale jumpSeq/dashSeq (see new_game)
    g.input = InputState{};
    g.mode = Mode::Playing;
}

void tick(Game& g, float dt) {
    World& w = g.world;
    if (w.players.empty()) return;

    // per-player control intent from Game::inputs
    for (auto& p : w.players) {
        if (!p.alive || p.dying) continue; // dead or mid-death-animation: no control
        int idx = (p.id >= 0 && p.id < MAX_PLAYERS) ? p.id : 0;
        const InputState& in = g.inputs[idx];
        p.aim = { in.aimX, in.aimY }; // normalized mouse aim (combat reads it for firing)

        float target = ((in.right ? 1.0f : 0.0f) - (in.left ? 1.0f : 0.0f)) * MOVE_SPEED;
        p.vel.x = approach(p.vel.x, target, ACCEL * dt);
        if (target > 0)      p.facing = Facing::Right;
        else if (target < 0) p.facing = Facing::Left;

        if (in.jumpSeq != p.lastJumpSeq) {                 // new jump edge
            p.lastJumpSeq = in.jumpSeq;
            if (p.onGround || p.coyote > 0) {              // grounded jump
                p.vel.y = JUMP_VEL; p.coyote = 0; p.onGround = false; p.jumpActive = true;
                fx::emit(w, fx::Event::Jump, { p.pos.x + p.size.x * 0.5f, p.pos.y + p.size.y });
            } else if (p.airJumps < MAX_AIR_JUMPS) {       // double jump (mid-air)
                ++p.airJumps;
                p.vel.y = JUMP_VEL; p.jumpActive = true;
                fx::emit(w, fx::Event::Jump, { p.pos.x + p.size.x * 0.5f, p.pos.y + p.size.y });
            }
        }
        if (in.dashSeq != p.lastDashSeq) {                 // new dash edge
            p.lastDashSeq = in.dashSeq;
            if (p.dashCd <= 0) {
                p.dashCd    = DASH_CD;
                p.dashTimer = DASH_TIME;
                // Dash follows MOVEMENT intent (held left/right), else facing. Mouse aim must NOT
                // decide the dash: aiming one way while running the other (normal while shooting)
                // was pinning every dash toward the cursor — the "dash stuck in one direction" bug.
                p.dashDir = in.right ? 1.f : (in.left ? -1.f
                                       : (p.facing == Facing::Right ? 1.f : -1.f));
                fx::emit(w, fx::Event::Dash, { p.pos.x + p.size.x * 0.5f, p.pos.y + p.size.y * 0.5f });
            }
        }
        if (p.jumpActive) {                                // variable jump height
            if (p.vel.y >= 0)        p.jumpActive = false;             // apex reached
            else if (!in.jumpHeld) { p.vel.y *= JUMP_CUT; p.jumpActive = false; } // released early
        }
        p.wantFire = in.fire;
    }

    worldgen::ensure(w, lead_pos(w).x + SCREEN_W);
    platforms::update(w, dt);
    physics::step(w, dt);
    combat::update(w, dt);
    items::update(w, dt);
    fx::update(w, dt);

    // death handling: fell into the void -> start the ghost animation; then run every dying
    // player's rise + countdown. Combat starts the animation on hp<=0; this owns the rest so a
    // hp-death and a void-death share the same "float up, fade, then really die" sequence.
    for (auto& p : w.players) {
        if (p.alive && !p.dying && p.pos.y > VOID_Y) {
            p.dying = true;
            p.deathTimer = DEATH_ANIM_TIME;
            fx::emit(w, fx::Event::PlayerDeath, { p.pos.x + p.size.x * 0.5f, p.pos.y });
        }
        if (p.dying) {
            p.vel = { p.vel.x * 0.9f, GHOST_RISE }; // ease horizontal, steady upward drift
            p.deathTimer -= dt;
            if (p.deathTimer <= 0) { p.dying = false; p.alive = false; }
        }
    }

    follow_camera(w, dt);
    w.difficulty = worldgen::difficulty_at(lead_pos(w).x);
    w.time += dt;

    bool anyAlive = false;
    for (const auto& p : w.players) anyAlive = anyAlive || p.alive;
    if (!anyAlive) g.mode = Mode::GameOver;
}

} // namespace game
