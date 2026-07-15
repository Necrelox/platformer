#pragma once
// Shared data contract. READ-ONLY for subsystem agents: worldgen / platforms / combat /
// items / render+ui / net / fx all read & write these fields, but the struct/enum definitions
// live here and here only. If you truly need a new field, append it and flag it in your
// report — never restructure this file from a subsystem.
#include "raylib.h"
#include <cstdint>
#include <array>
#include <vector>
#include <unordered_map>
#include <chrono>

namespace game {

// ---- world scale (calibration knobs) ----
inline constexpr int TILE     = 24;    // px per tile
inline constexpr int WORLD_H  = 44;    // tiles tall (fixed vertical extent; infinite horizontally).
                                       // Deep on purpose: real verticality (islands/climbs) builds
                                       // UP from the baseline ground near the bottom (GROUND_Y).
inline constexpr int CHUNK_W  = 32;    // tiles per generated chunk (worldgen convention)
inline constexpr int SCREEN_W = 1280;
inline constexpr int SCREEN_H = 720;

inline constexpr int NUM_ZONES = 8;    // biome count — zone_at() cycles 0..NUM_ZONES-1.
                                       // worldgen & render both size their per-zone tables to this
                                       // (fill EVERY [NUM_ZONES] table when changing — a short
                                       // initializer zero-fills into a dead biome, not a compile error).

inline constexpr int MAX_PLAYERS = 4;  // LAN co-op cap

inline constexpr float DEATH_ANIM_TIME = 1.0f; // s the ghost death animation plays before death

// ---- tile ids: 0 = empty. T_SPIKE / T_LAVA are NON-solid hazards (walk into them = damage). ----
inline constexpr uint8_t T_GROUND = 1;
inline constexpr uint8_t T_SPIKE  = 2;
inline constexpr uint8_t T_LAVA   = 3;   // volcanic-biome hazard: same damage path as spikes.

// ---- tilemap: infinite horizontally, fixed height. ----
struct TileMap {
    std::unordered_map<int, std::array<uint8_t, WORLD_H>> cols; // key = global tile x

    uint8_t at(int tx, int ty) const {
        if (ty < 0 || ty >= WORLD_H) return 0;
        auto it = cols.find(tx);
        return it == cols.end() ? 0 : it->second[ty];
    }
    void set(int tx, int ty, uint8_t v) {
        if (ty < 0 || ty >= WORLD_H) return;
        cols[tx][ty] = v;
    }
    // Solid = any non-empty tile EXCEPT hazards (spikes/lava are things you pass through & get hurt).
    bool solid(int tx, int ty) const { uint8_t v = at(tx, ty); return v != 0 && v != T_SPIKE && v != T_LAVA; }
    bool hazard(int tx, int ty) const { uint8_t v = at(tx, ty); return v == T_SPIKE || v == T_LAVA; }
};

enum class Facing { Left, Right };
enum class Weapon { Pistol, Rapid, Double };

struct Player {
    Vector2 pos{}, vel{};
    Vector2 size{ TILE * 0.8f, TILE * 1.4f };
    bool    onGround = false;
    float   coyote   = 0;      // s of coyote time remaining
    bool    alive    = true;
    Facing  facing   = Facing::Right;
    int     hp = 3, maxHp = 3;
    float   invuln     = 0;    // s of hit-invincibility remaining
    Weapon  weapon     = Weapon::Pistol;
    float   fireCd     = 0;    // s until next allowed shot
    float   powerTimer = 0;    // s left on a temporary power-up (e.g. item invincibility)
    bool    invincible = false;// item-granted invincibility active
    float   distance   = 0;    // furthest x reached (px) — HUD + difficulty
    int     score      = 0;    // coins collected (HUD; synced via PlayerWire so clients see it)
    int     id    = 0;         // player slot [0..MAX_PLAYERS) — indexes Game::inputs
    int     skin  = 0;         // 0=red 1=green 2=blue 3=yellow (see skin_color)
    Color   color = RAYWHITE;  // skin tint (derived from skin)
    // ---- mobility (core/game.cpp owns the logic; host-simulated, not wire-synced — pos/vel are) ----
    int      airJumps  = 0;    // air-jumps used since last grounded (double jump: cap MAX_AIR_JUMPS)
    float    dashCd    = 0;    // s until dash is ready again
    float    dashTimer = 0;    // s left on the active dash (vel is overridden while > 0)
    float    dashDir   = 1;    // +1/-1 horizontal direction of the active dash
    uint32_t lastDashSeq = 0;  // last processed dash edge (see InputState::dashSeq)
    // control intent, set by core::tick from Game::inputs before combat/physics run:
    bool     wantFire    = false;
    uint32_t lastJumpSeq = 0;  // last processed jump edge (see InputState::jumpSeq)
    bool     jumpActive  = false; // rising from a jump & not yet cut (variable jump height)
    Vector2  aim{};            // normalized fire direction ({0,0} = fire toward facing)
    bool     dying   = false;  // playing the ghost death animation (still alive==true meanwhile)
    float    deathTimer = 0;   // s left on the death animation; hits 0 -> alive=false
};

// Append-only (types are deterministic from worldgen, never wire-sent — safe to extend).
// Jumper: ground hopper. Bomber: aerial, lobs arcing bombs (see combat.cpp for behavior spec).
enum class EnemyType  { Walker, Flyer, Shooter, Charger, Jumper, Bomber };
enum class EnemyState { Patrol, Aggro };
struct Enemy {
    Vector2 pos{}, vel{};
    Vector2 size{ TILE * 0.9f, TILE * 0.9f };
    bool       onGround = false;
    int        hp    = 2;
    EnemyType  type  = EnemyType::Walker;
    EnemyState state = EnemyState::Patrol;
    Facing     facing = Facing::Left;
    bool       alive = true;
    float      t = 0;                    // generic AI timer (charge wind-up / flyer bob phase)
    float      patrolMin = 0, patrolMax = 0; // px x-bounds for patrol (worldgen sets)
    float      fireCd = 0;               // Shooter/Bomber: s until next shot/bomb
    float      baseY  = 0;               // Flyer/Bomber: hover center y (worldgen sets = spawn y)
    float      jumpCd = 0;               // Jumper (& jumping ground enemies): s until next hop allowed
    float      knockback = 0;            // s of hit-stun: while > 0, AI yields to the impulse (all mobs
                                         // get knocked back) and render flashes the hit. combat ticks it.
};

struct Projectile {
    Vector2 pos{}, vel{};
    bool    fromPlayer = true;
    bool    alive = true;
    float   life  = 0;   // s alive
    int     damage = 1;
    float   gravity = 0; // px/s^2 applied to vel.y each tick (0 = straight shot; >0 = arcing bomb).
    bool    bomb = false;// true = enemy bomb: explodes (fx + small AoE) when it dies. combat owns this.
};

// Append-only (deterministic from worldgen, never wire-sent). Coin = frequent score scatter,
// NOT part of the power-up loot table (worldgen scatters it separately).
enum class ItemType { Heart, RapidFire, DoubleShot, Invincibility, Coin };
struct Item {
    Vector2  pos{};
    Vector2  size{ TILE * 0.9f, TILE * 0.9f };
    ItemType type = ItemType::Heart;
    bool     alive = true;
    float    t = 0;      // bob animation phase
};

enum class PlatformKind { Static, Breakable, Moving };
struct Platform {
    Rectangle    rect{};                    // current AABB in px
    PlatformKind kind   = PlatformKind::Static;
    bool         active = true;             // false = currently gone (broken / respawning)
    // breakable
    bool  triggered   = false;
    float breakTimer  = 0;                  // s until break after first contact
    float respawnTimer = 0;                 // s until it comes back
    // moving
    Vector2 a{}, b{};                       // path endpoints (px, rect top-left)
    float   speed = 0;                      // px/s along path
    float   phase = 0;                      // 0..1 along path
    int     dir   = 1;                      // +1 / -1
};

// Cosmetic only — NOT network-synced. Safe to erase/reorder (unlike the entity vectors below).
struct Particle {
    Vector2 pos{}, vel{};
    float   life = 0, maxLife = 0.5f;
    float   size = 3;
    Color   color = WHITE;
};

enum class Mode { Menu, Settings, Lobby, Playing, Paused, GameOver };

struct Settings { float volume = 0.7f; bool fullscreen = false; };

// ---- multiplayer ----
enum class NetRole { Solo, Host, Client };

struct PlayerSlot {
    bool used  = false;
    bool ready = false;
    int  skin  = 0;
    char name[16] = {};
};

struct Lobby {
    PlayerSlot slots[MAX_PLAYERS];
    uint64_t   seed      = 1;
    bool       started   = false;
    int        localSlot = 0;   // which slot is the local player
};

struct NetState {
    NetRole role      = NetRole::Solo;
    bool    connected = false;  // client: connected to host / host: session open
    Lobby   lobby;
    char    joinIp[32] = "127.0.0.1";
    int     port       = 45455;
};

struct World {
    uint64_t seed = static_cast<uint64_t>(std::chrono::system_clock::now().time_since_epoch().count());
    TileMap  map;
    std::vector<Player>     players;
    // NET INVARIANT: the next three vectors are APPEND-ONLY and index-aligned across host &
    // client (both generate them identically from the seed). NEVER erase or reorder them — set
    // alive=false / active=false instead. (This is why entity culling-by-index is off the
    // table for multiplayer; only cosmetic `particles` and non-synced `projectiles` may shrink.)
    std::vector<Enemy>      enemies;
    std::vector<Item>       items;
    std::vector<Platform>   platforms;
    std::vector<Projectile> projectiles; // host-authoritative, sent as a full list (not generated)
    std::vector<Particle>   particles;    // cosmetic, local-only
    Camera2D cam{ .offset = { SCREEN_W * 0.5f, SCREEN_H * 0.6f },
                  .target = {}, .rotation = 0, .zoom = 1 };
    float difficulty = 0;   // grows with lead distance
    int   genCol     = 0;   // next global tile column to generate (worldgen owns)
    float time       = 0;
    int   localId    = 0;   // index into players for the local player
    float shake      = 0;   // camera-shake trauma [0..1], local/cosmetic. Anyone bumps it up on a
                            // big event (w.shake = fmaxf(w.shake, X)); follow_camera decays & applies it.
};

struct InputState {
    bool left = false, right = false, jumpHeld = false, fire = false;
    float aimX = 0, aimY = 0;   // normalized aim direction from local mouse ({0,0} = use facing)
    // Monotonic jump-press counter. Incremented once per jump key press-edge. Sent over the
    // wire so the host fires exactly one jump per increment even if packets drop/reorder —
    // never transmit a bare "jump this frame" edge on an unreliable path.
    uint32_t jumpSeq = 0;
    uint32_t dashSeq = 0;   // same edge-safe scheme as jumpSeq, for the dash key.
};

struct Game {
    Mode       mode = Mode::Menu;
    Settings   settings;
    World      world;
    InputState input;                 // local keyboard scratch for the current frame
    InputState inputs[MAX_PLAYERS];    // per-player intent (local + received) consumed by tick
    NetState   net;
    bool       wantQuit = false;      // set by the menu "Quitter"; main loop exits on it
};

} // namespace game
