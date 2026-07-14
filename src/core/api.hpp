#pragma once
// Cross-subsystem call surface. Foundation defines the `core`/`spawn`/`physics` items and
// provides no-op/minimal stubs for every subsystem namespace below; each agent replaces the
// stub in its own directory with the real implementation. Signatures here are the contract —
// do not change them without flagging it (all callers depend on them).
#include "types.hpp"

namespace game {

// ---- core (foundation-owned) ----
void    new_game(Game&, uint64_t seed);   // SOLO: reset world, generate start, spawn 1 player, Playing
void    start_game_from_lobby(Game&);     // MP: reset world from net.lobby.seed, spawn players per slots, Playing
void    tick(Game&, float dt);            // one fixed simulation step while Playing (host/solo)
void    follow_camera(World&, float dt);  // lerp camera to lead player (used by client too)
Vector2 lead_pos(const World&);           // furthest-right player position (camera anchor)
Color   skin_color(int skin);             // 0=red 1=green 2=blue 3=yellow

namespace spawn { // foundation-owned helpers: push into the World vectors and return a ref
    Platform&   platform(World&, Rectangle rect, PlatformKind kind);
    Enemy&      enemy(World&, Vector2 pos, EnemyType type);
    Item&       item(World&, Vector2 pos, ItemType type);
    Projectile& projectile(World&, Vector2 pos, Vector2 vel, bool fromPlayer, int damage);
}

namespace physics { // foundation-owned
    void move_and_collide(const World&, Vector2& pos, Vector2& vel, Vector2 size,
                          bool& onGround, float dt);
    void step(World&, float dt);          // gravity + move_and_collide for all players
}

// ---- subsystem contracts (each implemented in its own dir; foundation ships a stub) ----
namespace worldgen {
    void  reset(World&, uint64_t seed);
    void  ensure(World&, float rightEdgePx);
    float difficulty_at(float x);
}
namespace platforms { void update(World&, float dt); }
namespace combat    { void update(World&, float dt); }   // fires per-player from Player::wantFire
namespace items     { void update(World&, float dt); }
namespace render    { void draw_world(const World&); }
namespace ui {
    void menu(Game&);
    void settings(Game&);
    void lobby(Game&);       // real lobby (host/client): player list, ready, skin, host starts
    void hud(const World&);
    void gameover(Game&);
}

// ---- networking (implemented in src/net/; foundation stubs to no-op so Solo builds) ----
// Host-authoritative, LAN. Host simulates & broadcasts snapshots; clients send input & render.
namespace net {
    bool start_host(uint16_t port, int maxClients); // returns false on failure
    bool start_client(const char* ip, uint16_t port);
    void shutdown();
    bool is_active();          // an ENet session (host or client) is running
    int  peer_count();         // host: connected clients

    // Pump the socket every frame (both Lobby & Playing). Dispatches all incoming packets:
    //  host   -> accept joins (assign slots), handle byes, read client Input into g.inputs[],
    //            react to Hello; may set g.mode.
    //  client -> apply Welcome/LobbyState into g.net.lobby, StartGame -> start_game_from_lobby,
    //            buffer Snapshots for interpolation; may set g.mode.
    void poll(Game&);

    void send_hello(Game&);            // client -> host: name/skin on connect
    void broadcast_lobby(Game&);       // host -> clients: lobby state (reliable)
    void send_start(Game&);            // host -> clients: StartGame(seed, slots) (reliable)
    void send_input(Game&);            // client -> host: g.inputs[localSlot] (reliable-sequenced)
    void broadcast_snapshot(Game&);    // host -> clients: world dynamic state (unreliable)
    void sync_client(Game&, float dt); // client: ensure() to snapshot genCol + apply interpolated state
}

// ---- fx (implemented in src/fx/; foundation stubs to no-op) ----
namespace fx {
    enum class Event { Fire, EnemyDeath, PlayerHit, Pickup, Jump };
    void init_audio();                 // guarded — safe when no audio device is present
    void shutdown_audio();
    void set_volume(float v);          // 0..1 (from Settings)
    void emit(World&, Event, Vector2 pos); // spawn particles + play the event's sfx
    void update(World&, float dt);     // advance/expire particles
    void draw(const World&);           // draw particles (world space, inside BeginMode2D)
}

} // namespace game
