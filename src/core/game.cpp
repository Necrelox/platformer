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
    std::vector<Rectangle> solids;
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
        p.vel.y += GRAVITY * dt;
        move_and_collide(w, p.pos, p.vel, p.size, p.onGround, dt);
        if (p.onGround) p.coyote = COYOTE;
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
    g.world.seed = seed;
    worldgen::reset(g.world, seed);
    Player p;
    p.id = 0;
    p.pos = { TILE * 3.0f, TILE * 2.0f };
    p.skin = 0;
    p.color = skin_color(0);
    g.world.players.clear();
    g.world.players.push_back(p);
    g.world.localId = 0;
    g.world.cam.target = p.pos;
    g.net.role = NetRole::Solo;
    for (auto& in : g.inputs) in = InputState{};
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
        p.pos   = { TILE * (3.0f + slotOrder * 1.5f), TILE * 2.0f };
        w.players.push_back(p);
        ++slotOrder;
    }
    if (w.players.empty()) { // safety net
        Player p; p.id = g.net.lobby.localSlot; p.color = skin_color(0);
        p.pos = { TILE * 3.0f, TILE * 2.0f }; w.players.push_back(p);
    }
    w.localId = 0;
    for (size_t i = 0; i < w.players.size(); ++i)
        if (w.players[i].id == g.net.lobby.localSlot) w.localId = (int)i;
    w.cam.target = w.players[w.localId].pos;
    g.mode = Mode::Playing;
}

void tick(Game& g, float dt) {
    World& w = g.world;
    if (w.players.empty()) return;

    // per-player control intent from Game::inputs
    for (auto& p : w.players) {
        if (!p.alive) continue;
        int idx = (p.id >= 0 && p.id < MAX_PLAYERS) ? p.id : 0;
        const InputState& in = g.inputs[idx];

        float target = ((in.right ? 1.0f : 0.0f) - (in.left ? 1.0f : 0.0f)) * MOVE_SPEED;
        p.vel.x = approach(p.vel.x, target, ACCEL * dt);
        if (target > 0)      p.facing = Facing::Right;
        else if (target < 0) p.facing = Facing::Left;

        if (in.jumpSeq != p.lastJumpSeq) {                 // new jump edge
            p.lastJumpSeq = in.jumpSeq;
            if (p.onGround || p.coyote > 0) {
                p.vel.y = JUMP_VEL; p.coyote = 0; p.onGround = false; p.jumpActive = true;
                fx::emit(w, fx::Event::Jump, { p.pos.x + p.size.x * 0.5f, p.pos.y + p.size.y });
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

    follow_camera(w, dt);
    w.difficulty = worldgen::difficulty_at(lead_pos(w).x);
    w.time += dt;

    bool anyAlive = false;
    for (const auto& p : w.players) anyAlive = anyAlive || p.alive;
    if (!anyAlive) g.mode = Mode::GameOver;
}

} // namespace game
