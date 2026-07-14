#include "core/api.hpp"
#include <cmath>
#include <random>
#include <algorithm>

// Phase 3 — chunk-based infinite procedural generation.
// A chunk's content depends ONLY on (seed, chunkIndex): no global mutable state, no
// wall-clock, no carry-over from neighboring chunks. Every chunk starts and ends at the
// same baseline ground height, so chunk boundaries always line up regardless of order or
// how many times ensure() is called.
namespace game::worldgen {

namespace {

constexpr int GROUND_Y      = WORLD_H - 3; // baseline: top row of the 3-tile-thick ground
constexpr int SAFE_COLS     = 13;          // chunk-0 guaranteed-safe columns (0..12)
constexpr int MAX_GAP       = 5;           // tiles — traversal cap (<= 5 per spec)
constexpr int MAX_STEP      = 4;           // tiles — ascent cap (<= 4 per spec)
constexpr int ENSURE_MARGIN = 4;           // extra tiles generated past the requested edge

void fill_ground(World& w, int tx, int topRowY) {
    for (int ty = topRowY; ty < WORLD_H; ++ty) w.map.set(tx, ty, 1);
}

// Loot table (worldgen-owned): Heart common, RapidFire/DoubleShot mid, Invincibility rare;
// skews toward power-ups as difficulty (0..1+) rises.
ItemType roll_item(std::mt19937_64& rng, float diff) {
    float d = std::min(diff, 1.0f);
    float wHeart  = 55.0f - 20.0f * d;
    float wRapid  = 15.0f + 10.0f * d;
    float wDouble = 15.0f + 10.0f * d;
    float wInvin  = 5.0f  + 10.0f * d;
    std::uniform_real_distribution<float> pick(0.0f, wHeart + wRapid + wDouble + wInvin);
    float r = pick(rng);
    if ((r -= wHeart)  < 0) return ItemType::Heart;
    if ((r -= wRapid)  < 0) return ItemType::RapidFire;
    if ((r -= wDouble) < 0) return ItemType::DoubleShot;
    return ItemType::Invincibility;
}

// Places a platform bridging a gap: mostly Static, sometimes a vertically-bobbing Moving one.
void place_rescue_platform(World& w, int gx, int width, std::mt19937_64& rng) {
    float py = (GROUND_Y - 2) * (float)TILE; // ~2 tiles above ground: within jump reach both sides
    Rectangle rect{ gx * (float)TILE, py, width * (float)TILE * 0.6f, TILE * 0.5f };
    if (rng() % 100 < 30) {
        auto& pf = spawn::platform(w, rect, PlatformKind::Moving);
        pf.a = { rect.x, rect.y };
        pf.b = { rect.x, rect.y - TILE * 1.5f };
        pf.speed = 40.0f;
    } else {
        spawn::platform(w, rect, PlatformKind::Static);
    }
}

void generate_chunk(World& w, int chunkIndex) {
    std::mt19937_64 rng(w.seed ^ ((uint64_t)chunkIndex * 0x9E3779B97F4A7C15ull));
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);

    int chunkStart = chunkIndex * CHUNK_W;
    int lc = 0; // local column cursor within this chunk

    if (chunkIndex == 0) {
        // Guaranteed-safe spawn zone: solid flat ground, no gaps/enemies/platforms/items.
        for (; lc < SAFE_COLS; ++lc) fill_ground(w, chunkStart + lc, GROUND_Y);
    } else {
        // Force the chunk's first column solid so a gap ending the PREVIOUS chunk can never
        // chain with a gap starting this one (that would exceed MAX_GAP across the boundary).
        fill_ground(w, chunkStart, GROUND_Y);
        lc = 1;
    }

    bool prevGap = false; // disallow two gap segments back-to-back (same reason as above)
    while (lc < CHUNK_W) {
        int gx = chunkStart + lc;
        float diff = std::min(difficulty_at(gx * (float)TILE), 1.0f);
        int remaining = CHUNK_W - lc;
        float roll = unit(rng);

        float pGap    = 0.10f + 0.20f * diff;
        float pHill   = 0.15f + 0.10f * diff;
        float pIsland = 0.10f;

        if (!prevGap && roll < pGap && remaining >= 2) {
            int width = std::min({ remaining, MAX_GAP, 2 + (int)(unit(rng) * (MAX_GAP - 1)) });
            if (width >= 4 && unit(rng) < 0.6f) place_rescue_platform(w, gx, width, rng);
            lc += width; // no ground tiles here -> pit
            prevGap = true;
            continue;
        }
        prevGap = false;

        if (roll < pGap + pHill && remaining >= 3) {
            int up      = std::min(MAX_STEP, 1 + (int)(unit(rng) * MAX_STEP));
            int plateau = 1 + (int)(unit(rng) * 3);
            int total   = std::min(remaining, up * 2 + plateau);
            if (total >= 3) {
                int cur = 0, height = 0;
                for (; cur < total && height < up; ++cur) { ++height; fill_ground(w, gx + cur, GROUND_Y - height); }
                int plateauEnd = std::min(total, cur + plateau);
                for (; cur < plateauEnd; ++cur) fill_ground(w, gx + cur, GROUND_Y - height);
                for (; cur < total; ++cur) { height = std::max(0, height - 1); fill_ground(w, gx + cur, GROUND_Y - height); }
                lc += total;
                continue;
            }
        }

        if (roll < pGap + pHill + pIsland && remaining >= 4) {
            int width = std::min(remaining, 4);
            for (int i = 0; i < width; ++i) fill_ground(w, gx + i, GROUND_Y);
            int islandW  = std::min(2, width - 1);
            int islandTy = GROUND_Y - (3 + (int)(unit(rng) * 2)); // floats 3-4 tiles above ground
            for (int i = 0; i < islandW; ++i) w.map.set(gx + 1 + i, islandTy, 1);
            if (chunkIndex > 0 && unit(rng) < 0.5f)
                spawn::item(w, { (gx + 1 + islandW * 0.5f) * TILE, (islandTy - 1) * (float)TILE }, roll_item(rng, diff));
            lc += width;
            continue;
        }

        // Flat (default / remainder-fitting fallback).
        int width = std::max(1, std::min(remaining, 4 + (int)(unit(rng) * 6)));
        for (int i = 0; i < width; ++i) fill_ground(w, gx + i, GROUND_Y);
        if (chunkIndex > 0) {
            if (width >= 2 && unit(rng) < 0.15f + 0.25f * diff) {
                auto& en = spawn::enemy(w, { (gx + width * 0.5f) * TILE, 0 }, EnemyType::Walker);
                en.pos.y = GROUND_Y * (float)TILE - en.size.y; // rest on ground surface
                en.patrolMin = gx * (float)TILE;
                en.patrolMax = (gx + width) * (float)TILE;
            }
            if (width >= 3 && unit(rng) < 0.15f) {
                Rectangle rect{ (gx + width * 0.5f - 1.0f) * TILE, (GROUND_Y - 3) * (float)TILE, TILE * 2.0f, TILE * 0.5f };
                spawn::platform(w, rect, PlatformKind::Breakable);
                if (unit(rng) < 0.7f) spawn::item(w, { rect.x + TILE, rect.y - TILE }, roll_item(rng, diff));
            } else if (width >= 2 && unit(rng) < 0.10f) {
                auto& it = spawn::item(w, { (gx + width * 0.5f) * TILE, 0 }, roll_item(rng, diff));
                it.pos.y = GROUND_Y * (float)TILE - it.size.y;
            }
        }
        lc += width;
    }
}

} // namespace

void reset(World& w, uint64_t seed) {
    w.map = TileMap{};
    w.seed = seed;
    w.genCol = 0;
    ensure(w, SCREEN_W * 1.5f);
}

void ensure(World& w, float rightEdgePx) {
    int upto = (int)std::ceil(rightEdgePx / TILE) + ENSURE_MARGIN;
    while (w.genCol <= upto) {
        int chunkIndex = w.genCol / CHUNK_W;
        generate_chunk(w, chunkIndex);
        w.genCol = (chunkIndex + 1) * CHUNK_W;
    }
}

// ponytail: entities accumulate in the World vectors forever and are never culled behind
// the camera. Fine for a normal run; if runs get very long, add periodic removal of
// off-screen enemies/items/inactive platforms trailing far behind the lead player.
float difficulty_at(float x) {
    float t = x / 20000.0f;      // smooth, monotonic, ~0 at start, asymptotic toward 1
    return t / (1.0f + t);
}

} // namespace game::worldgen
