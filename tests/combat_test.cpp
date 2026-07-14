#include "core/api.hpp"
#include <cassert>
#include <cstdio>

using namespace game;

static void make_ground(World& w, int ty, int txFrom, int txTo) {
    for (int tx = txFrom; tx <= txTo; ++tx) w.map.set(tx, ty, 1);
}

// (a) enemy in front of the player dies after enough gunfire.
static void test_enemy_dies_to_gunfire() {
    World w;
    w.localId = 0;

    int   groundTy   = 5;
    float groundTopY = groundTy * TILE;
    make_ground(w, groundTy, 0, 40);

    Player p;
    p.facing = Facing::Right;
    p.weapon = Weapon::Rapid; // fast cooldown so the test converges quickly
    p.pos    = { 100.f, groundTopY - p.size.y };
    w.players.push_back(p);

    Enemy e;
    e.pos = { w.players[0].pos.x + 150.f, groundTopY - e.size.y }; // in front, same ground level
    w.enemies.push_back(e);

    InputState in;
    in.fire = true;

    float dt = 1.f / 60.f;
    for (int i = 0; i < 180 && w.enemies[0].alive; ++i)
        combat::update(w, in, dt);

    assert(!w.enemies[0].alive);
    printf("test_enemy_dies_to_gunfire: OK\n");
}

// (b) contact damage applies once, then iframes block a second hit on the very next step.
static void test_player_iframes() {
    World w;
    w.localId = 0;

    int   groundTy   = 5;
    float groundTopY = groundTy * TILE;
    make_ground(w, groundTy, 0, 40);

    Player p;
    p.hp  = 3;
    p.pos = { 200.f, groundTopY - p.size.y };
    w.players.push_back(p);

    Enemy e;
    e.pos = w.players[0].pos; // fully overlapping -> guaranteed contact
    w.enemies.push_back(e);

    InputState in; // no firing needed for this check

    float dt = 1.f / 60.f;
    combat::update(w, in, dt);
    assert(w.players[0].hp == 2);
    assert(w.players[0].invuln > 0.f);

    combat::update(w, in, dt); // immediately again: iframes must hold
    assert(w.players[0].hp == 2);
    assert(w.players[0].alive);

    printf("test_player_iframes: OK\n");
}

int main() {
    test_enemy_dies_to_gunfire();
    test_player_iframes();
    printf("all combat tests passed\n");
    return 0;
}
