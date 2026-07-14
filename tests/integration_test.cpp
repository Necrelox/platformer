// Headless integration check: drives the ASSEMBLED simulation (worldgen + physics +
// platforms + combat + items) via core::tick for thousands of steps with synthetic input.
// No InitWindow — tick() never renders, so this runs headless. Proves the 5 subsystems
// cooperate: world extends, player progresses, nothing crashes/soft-locks.
#include "core/api.hpp"
#include <cassert>
#include <cstdio>

using namespace game;

int main() {
    Game g;
    new_game(g, 1);                       // worldgen::reset runs here
    assert(g.mode == Mode::Playing);
    assert(!g.world.players.empty());
    int startCol = g.world.genCol;
    assert(startCol > 0);                 // initial columns generated

    const float DT = 1.0f / 120.0f;
    float startDist = g.world.players[0].distance;

    // Hold right, tap jump every ~0.5s, hold fire. 6000 steps = ~50s of sim.
    for (int i = 0; i < 6000; ++i) {
        g.inputs[0].left  = false;
        g.inputs[0].right = true;
        if (i % 60 == 0) g.inputs[0].jumpSeq++;   // one jump press-edge every 60 steps
        g.inputs[0].jumpHeld = (i % 60 < 12);
        g.inputs[0].fire  = true;
        tick(g, DT);
        // world must keep extending as the player advances
        assert(g.world.genCol >= startCol);
        // players vector never corrupted
        assert(!g.world.players.empty());
    }

    const Player& p = g.world.players[0];
    printf("distance start=%.0f end=%.0f  genCol=%d  enemies=%zu items=%zu platforms=%zu projectiles=%zu  hp=%d alive=%d mode=%d\n",
           startDist, p.distance, g.world.genCol,
           g.world.enemies.size(), g.world.items.size(), g.world.platforms.size(),
           g.world.projectiles.size(), p.hp, (int)p.alive, (int)g.mode);

    // The player should have made forward progress (not stuck at spawn against a wall/softlock).
    assert(p.distance > startDist + 200.0f);
    // World generation kept pace with movement.
    assert(g.world.genCol > startCol);
    // Some content got generated over 50s of travel.
    assert(!g.world.platforms.empty());

    puts("integration_test OK");
    return 0;
}
