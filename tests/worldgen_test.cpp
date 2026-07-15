// Self-check for src/world/worldgen.cpp — headless, no InitWindow.
// Build+run:
//   g++ -std=c++26 -Isrc $(pkg-config --cflags --libs raylib) \
//       tests/worldgen_test.cpp src/world/worldgen.cpp src/core/game.cpp -o /tmp/worldgen_test \
//       && /tmp/worldgen_test
#include "core/api.hpp"
#include <cassert>
#include <cstdio>
#include <algorithm>
#include <vector>

using namespace game;

int main() {
    // ---- Determinism: same seed -> identical tiles over the whole generated range ----
    World a, b;
    worldgen::reset(a, 42);
    worldgen::reset(b, 42);
    worldgen::ensure(a, SCREEN_W * 3.0f);
    worldgen::ensure(b, SCREEN_W * 3.0f);
    assert(a.genCol == b.genCol);
    int maxCol = a.genCol;
    for (int tx = 0; tx < maxCol; ++tx)
        for (int ty = 0; ty < WORLD_H; ++ty)
            assert(a.map.at(tx, ty) == b.map.at(tx, ty));

    // ---- Different seed -> tiles differ somewhere past the guaranteed-safe zone ----
    World c;
    worldgen::reset(c, 999);
    worldgen::ensure(c, SCREEN_W * 3.0f);
    bool differs = false;
    for (int tx = 13; tx < maxCol && !differs; ++tx)
        for (int ty = 0; ty < WORLD_H; ++ty)
            if (a.map.at(tx, ty) != c.map.at(tx, ty)) { differs = true; break; }
    assert(differs);

    // ---- Spawn zone (cols 0..8) is solid ground, no gaps ----
    for (int tx = 0; tx <= 8; ++tx)
        for (int ty = WORLD_H - 3; ty < WORLD_H; ++ty)
            assert(a.map.solid(tx, ty));

    // ---- No entities placed inside the safe zone (cols 0..12) ----
    float safeEdge = 13.0f * TILE;
    for (const auto& e : a.enemies)   assert(e.pos.x >= safeEdge);
    for (const auto& it : a.items)    assert(it.pos.x >= safeEdge);
    for (const auto& p : a.platforms) assert(p.rect.x >= safeEdge);

    // ---- Idempotent / incremental: calling ensure repeatedly with growing edges doesn't
    //      regenerate (and thus doesn't change) already-generated columns. ----
    World d;
    worldgen::reset(d, 7);
    int col1 = d.genCol;
    std::vector<uint8_t> snapshot;
    for (int tx = 0; tx < col1; ++tx)
        for (int ty = 0; ty < WORLD_H; ++ty)
            snapshot.push_back(d.map.at(tx, ty));
    worldgen::ensure(d, SCREEN_W * 2.5f);
    int idx = 0;
    for (int tx = 0; tx < col1; ++tx)
        for (int ty = 0; ty < WORLD_H; ++ty)
            assert(d.map.at(tx, ty) == snapshot[idx++]);

    // ---- Traversability: no contiguous pit (whole-column gap) wider than 5 tiles,
    //      anywhere in the generated range, INCLUDING across chunk boundaries. ----
    auto max_pit_run = [](const World& w, int fromCol, int uptoCol) {
        int longest = 0, cur = 0;
        for (int tx = fromCol; tx < uptoCol; ++tx) {
            bool pit = w.map.at(tx, WORLD_H - 1) == 0; // bottom row empty => whole column is a gap
            cur = pit ? cur + 1 : 0;
            longest = std::max(longest, cur);
        }
        return longest;
    };
    assert(max_pit_run(a, 0, maxCol) <= 5);
    assert(max_pit_run(c, 0, maxCol) <= 5);

    // Same check far out where difficulty (and thus gap density) is high, so an adjacent-gap
    // regression actually gets exercised instead of just not happening to occur near the start.
    World hi;
    worldgen::reset(hi, 3);
    worldgen::ensure(hi, 80000.0f); // diff ~0.8, pGap ~0.26 -> dense gaps
    assert(max_pit_run(hi, 0, hi.genCol) <= 5);

    // ---- 5 biomes: zone_at cycles through every zone id over a long enough span ----
    {
        bool seen[NUM_ZONES] = {};
        for (float x = 0.0f; x < 400000.0f; x += (float)TILE) seen[worldgen::zone_at(x)] = true;
        for (int z = 0; z < NUM_ZONES; ++z) assert(seen[z]);
    }

    // ---- New content actually shows up over the large `hi` world: Jumper/Bomber enemies,
    //      and T_LAVA tiles (volcanic biome). ----
    bool sawJumper = false, sawBomber = false;
    for (const auto& e : hi.enemies) {
        if (e.type == EnemyType::Jumper) sawJumper = true;
        if (e.type == EnemyType::Bomber) sawBomber = true;
    }
    assert(sawJumper);
    assert(sawBomber);

    // ---- Floating islands: the sky is full of them now (headline redesign) — count solid
    //      tiles well above the floor across the large `hi` world and expect MANY. ----
    {
        constexpr int GROUND_Y = WORLD_H - 3;
        int aboveFloorSolid = 0;
        for (int tx = 0; tx < hi.genCol; ++tx)
            for (int ty = 0; ty <= GROUND_Y - 4; ++ty)
                if (hi.map.solid(tx, ty)) ++aboveFloorSolid;
        assert(aboveFloorSolid > 1500); // deterministic actual ~4150 for this seed/extent — big margin
    }

    bool sawLava = false;
    for (int tx = 0; tx < hi.genCol && !sawLava; ++tx)
        for (int ty = 0; ty < WORLD_H; ++ty)
            if (hi.map.at(tx, ty) == T_LAVA) { sawLava = true; break; }
    assert(sawLava);

    // ---- difficulty_at: ~0 at start, smooth and monotonically increasing ----
    float prev = worldgen::difficulty_at(0.0f);
    assert(prev < 0.05f);
    for (float x = 1000.0f; x <= 200000.0f; x += 1000.0f) {
        float d2 = worldgen::difficulty_at(x);
        assert(d2 > prev); // strictly increasing
        prev = d2;
    }

    std::printf("worldgen_test OK\n");
    return 0;
}
