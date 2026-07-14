// The multiplayer snapshot design assumes host & client generate IDENTICAL, index-aligned
// enemy/item/platform vectors from the same seed. This test proves that assumption for two
// independent Worlds generated to the same extent (what a host and a client each do locally).
#include "core/api.hpp"
#include <cassert>
#include <cstdio>
#include <cmath>

using namespace game;

static void gen(World& w, uint64_t seed, float toX) {
    worldgen::reset(w, seed);
    worldgen::ensure(w, toX);
}

int main() {
    const uint64_t SEED = 123456789ull;
    const float TO_X = SCREEN_W * 6.0f;

    World a, b;
    gen(a, SEED, TO_X);
    gen(b, SEED, TO_X);

    printf("genCol a=%d b=%d | enemies %zu/%zu items %zu/%zu platforms %zu/%zu\n",
           a.genCol, b.genCol, a.enemies.size(), b.enemies.size(),
           a.items.size(), b.items.size(), a.platforms.size(), b.platforms.size());

    assert(a.genCol == b.genCol);
    assert(a.enemies.size()   == b.enemies.size());
    assert(a.items.size()     == b.items.size());
    assert(a.platforms.size() == b.platforms.size());

    // index-aligned content must match (positions/types) — this is what a snapshot indexes into
    for (size_t i = 0; i < a.enemies.size(); ++i) {
        assert(a.enemies[i].pos.x == b.enemies[i].pos.x);
        assert(a.enemies[i].pos.y == b.enemies[i].pos.y);
        assert(a.enemies[i].type  == b.enemies[i].type);
    }
    for (size_t i = 0; i < a.items.size(); ++i) {
        assert(a.items[i].type == b.items[i].type);
        assert(a.items[i].pos.x == b.items[i].pos.x);
    }
    for (size_t i = 0; i < a.platforms.size(); ++i) {
        assert(a.platforms[i].kind   == b.platforms[i].kind);
        assert(a.platforms[i].rect.x == b.platforms[i].rect.x);
    }

    // a different seed must actually differ (guards against "everything is constant")
    World c; gen(c, SEED + 1, TO_X);
    bool differs = c.platforms.size() != a.platforms.size();
    for (size_t i = 0; i < a.platforms.size() && !differs; ++i)
        if (c.platforms.size() > i && c.platforms[i].rect.x != a.platforms[i].rect.x) differs = true;
    assert(differs);

    puts("determinism_test OK");
    return 0;
}
