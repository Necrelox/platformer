#include "core/api.hpp"
#include <cmath>
#include <random>
#include <algorithm>

// Phase 3 — chunk-based infinite procedural generation.
// A chunk's content depends ONLY on (seed, chunkIndex): no global mutable state, no
// wall-clock, no carry-over from neighboring chunks. Every chunk starts and ends at the
// same baseline ground height, so chunk boundaries always line up regardless of order or
// how many times ensure() is called.
//
// Phase 6 additions: zones (biome id from x, see zone_at), taller/multi-tier verticality
// (hills, a floating-island layer, stacked-platform vertical climbs), spike-tile hazards,
// a "troll" breakable-over-a-pit trap, and the Flyer/Shooter/Charger roster — all layered
// on the same column-cursor loop, all still deterministic from (seed, chunkIndex).
//
// Phase 7 — sky redesign: the mandatory floor (this file's column-cursor loop, gaps/hills/
// flat) stays exactly as traversability-tested, but a SECOND independent pass now scatters
// floating island chains across three height bands over the whole chunk (see scatter_islands
// / build_island_group below) so the sky reads as "full of islands", not "flat ground with a
// rare bump". Coins are gone — items are the only worldgen-owned pickup now.
namespace game::worldgen {

namespace {

constexpr int GROUND_Y      = WORLD_H - 3; // baseline: top row of the 3-tile-thick ground
constexpr int SAFE_COLS     = 13;          // chunk-0 guaranteed-safe columns (0..12)
constexpr int MAX_GAP       = 5;           // tiles — traversal cap (<= 5 per spec)
constexpr int MAX_STEP      = 4;           // tiles — ascent cap (<= 4 per spec, per contiguous climb)
constexpr int ENSURE_MARGIN = 4;           // extra tiles generated past the requested edge
constexpr int ZONE_BAND     = 120;         // tiles per zone band (matches zone_at's own banding)

// Per-zone tuning (8 biomes): 0 surface, 1 cavern, 2 volcanic (LAVA — zone==2 is hazard-hardcoded
// below), 3 sky/cloud, 4 crystal/ice, 5 desert, 6 void/corrupt, 7 toxic/swamp. Each flavors gap
// density, hill frequency, verticality, spike density and enemy density/mix so the map never feels
// samey. IMPORTANT: every table MUST have exactly NUM_ZONES entries — a short initializer silently
// zero-fills (a dead, empty biome), it is NOT a compile error.
constexpr float ZONE_GAP_MUL[NUM_ZONES]   = { 1.00f, 1.15f, 1.35f, 1.30f, 1.50f, 1.25f, 1.60f, 1.40f };
constexpr float ZONE_HILL_MUL[NUM_ZONES]  = { 0.90f, 1.10f, 1.30f, 0.70f, 1.20f, 1.20f, 0.80f, 1.00f };
constexpr float ZONE_VERT_MUL[NUM_ZONES]  = { 0.70f, 1.00f, 1.20f, 1.90f, 1.40f, 1.10f, 1.70f, 1.30f };
constexpr float ZONE_SPIKE_MUL[NUM_ZONES] = { 0.55f, 0.90f, 1.40f, 1.05f, 1.75f, 1.10f, 1.90f, 1.60f };
constexpr float ZONE_ENEMY_MUL[NUM_ZONES] = { 0.85f, 1.05f, 1.25f, 1.15f, 1.55f, 1.20f, 1.70f, 1.45f };

void fill_ground(World& w, int tx, int topRowY) {
    for (int ty = topRowY; ty < WORLD_H; ++ty) w.map.set(tx, ty, T_GROUND);
}

// A one-tile hazard embedded in an otherwise-solid stretch: the walkable surface tile is a
// spike or lava (non-solid), but the row underneath is still solid ground, so it reads as a
// hazard poking out of the floor rather than a hole. Combat's overlap check (not physics
// collision) is what makes stepping into it hurt.
void fill_ground_hazard(World& w, int tx, int topRowY, uint8_t tile) {
    fill_ground(w, tx, topRowY + 1);
    w.map.set(tx, topRowY, tile);
}

// Places 1-2 hazard tiles somewhere in the INTERIOR of a solid span (never the first or last
// column), so there's always a safe landing on both sides — a jump-over, never a wall. Zone 2
// (volcanic) uses lava; every other zone uses spikes.
void maybe_hazard_row(World& w, int gx, int width, float density, int zone, std::mt19937_64& rng, float roll) {
    if (width < 3 || roll >= density) return;
    uint8_t tile = (zone == 2) ? T_LAVA : T_SPIKE;
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    int hazLen = std::min(width - 2, 1 + (int)(unit(rng) * 2.0f));      // 1-2 tiles
    int start  = 1 + (int)(unit(rng) * (float)(width - hazLen - 1));    // interior only
    for (int i = 0; i < hazLen; ++i) fill_ground_hazard(w, gx + start + i, GROUND_Y, tile);
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

// Ground-roster mix: Walker always available; Shooter more common in higher zones; Charger
// (tougher, hp ~4) ONLY appears from zone 2 on ("later zones only"); Jumper (ground hopper)
// only in zones 0-2 (per spec — sky/crystal skip it in favor of Bomber overhead).
EnemyType roll_ground_enemy_type(std::mt19937_64& rng, int zone) {
    float wWalker  = 45.0f;
    float wShooter = 20.0f + 8.0f * (float)zone;
    float wCharger = (zone >= 2) ? 20.0f : 0.0f;
    float wJumper  = (zone <= 2) ? 20.0f : 0.0f;
    std::uniform_real_distribution<float> pick(0.0f, wWalker + wShooter + wCharger + wJumper);
    float r = pick(rng);
    if ((r -= wWalker)  < 0) return EnemyType::Walker;
    if ((r -= wShooter) < 0) return EnemyType::Shooter;
    if ((r -= wCharger) < 0) return EnemyType::Charger;
    return EnemyType::Jumper;
}

// Aerial roster mix (caller already gated zone >= 1): Flyer common everywhere aerial spawns
// happen; Bomber (arcing bomber, zones 1-4) grows more common in later zones.
EnemyType roll_aerial_enemy_type(std::mt19937_64& rng, int zone) {
    float wFlyer  = 60.0f;
    float wBomber = 20.0f + 15.0f * (float)zone;
    std::uniform_real_distribution<float> pick(0.0f, wFlyer + wBomber);
    return (pick(rng) < wFlyer) ? EnemyType::Flyer : EnemyType::Bomber;
}

// Walker/Shooter/Charger all rest on a ground surface (or ledge/plateau/island top — same
// shape of tile). Charger gets hp ~4 (tougher, per spec).
void spawn_ground_enemy(World& w, EnemyType type, float xStart, float xEnd, int surfaceTopY) {
    auto& e = spawn::enemy(w, { (xStart + xEnd) * 0.5f, 0.0f }, type);
    e.pos.y = surfaceTopY * (float)TILE - e.size.y;
    e.patrolMin = xStart;
    e.patrolMax = xEnd;
    if (type == EnemyType::Charger) e.hp = 4;
}

// Flyer/Bomber both spawn IN THE AIR (never resting on a tile) and hover around their spawn y
// (no gravity — combat's AI drives them off baseY).
void spawn_aerial(World& w, EnemyType type, float x, float y) {
    auto& e = spawn::enemy(w, { x, y }, type);
    e.baseY = e.pos.y;
}

// Places a platform bridging a gap: mostly Static, sometimes a vertically-bobbing Moving
// one, and occasionally (later zones, past chunk 0) a "troll" — same bridge-shaped rect, but
// Breakable: looks like a safe path, crumbles ~0.4s after the player steps on it (the timer
// itself is owned by platforms.cpp; worldgen only picks WHICH kind of bridge this is). `py` is
// the platform's world-space top-surface y in px — callers place it relative to whatever two
// surfaces it's linking (floor-gap bridges float above the ground row; island bridges float
// above the island row), so this one helper serves both floor gaps and inter-island spans.
void place_bridge_platform(World& w, int gx, int width, float py, int zone, int chunkIndex, std::mt19937_64& rng) {
    Rectangle rect{ gx * (float)TILE, py, width * (float)TILE * 0.6f, TILE * 0.5f };
    bool troll = chunkIndex > 0 && zone >= 1 && (rng() % 100 < (uint64_t)(8 + zone * 6));
    if (troll) {
        spawn::platform(w, rect, PlatformKind::Breakable);
    } else if (rng() % 100 < 30) {
        auto& pf = spawn::platform(w, rect, PlatformKind::Moving);
        pf.a = { rect.x, rect.y };
        pf.b = { rect.x, rect.y - TILE * 1.5f };
        pf.speed = 40.0f;
    } else {
        spawn::platform(w, rect, PlatformKind::Static);
    }
}

// Multi-tier hill: `tiers` (1-2) chained (ascend <= MAX_STEP, mandatory plateau, repeat),
// then descend back toward baseline. Taller than a single-tier hill without ever exceeding
// MAX_STEP on any one contiguous climb. Returns columns consumed (<= remaining); if the
// budget runs out mid-descent the leftover height just becomes a safe drop at the boundary
// (falling never needs a jump), never a hanging unclimbable ascent.
int build_hill(World& w, int gx, int remaining, int tiers, std::mt19937_64& rng) {
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    int cur = 0, height = 0;
    for (int t = 0; t < tiers && cur < remaining; ++t) {
        int up = std::min(MAX_STEP, 1 + (int)(unit(rng) * (float)MAX_STEP));
        for (int i = 0; i < up && cur < remaining; ++i) { ++height; fill_ground(w, gx + cur, GROUND_Y - height); ++cur; }
        int plateau    = 1 + (int)(unit(rng) * 3.0f);
        int plateauEnd = std::min(remaining, cur + plateau);
        for (; cur < plateauEnd; ++cur) fill_ground(w, gx + cur, GROUND_Y - height);
    }
    while (height > 0 && cur < remaining) { --height; fill_ground(w, gx + cur, GROUND_Y - height); ++cur; }
    return cur;
}

// ---- Floating islands (dense 2D scatter — Terraria/Mario/Silksong sky of platforms) --------
//
// NOT vertical climbs. The whole play space above the floor is diced into a jittered GRID of
// cells (COL_STEP wide x ROW_STEP tall) and most cells drop one small T_GROUND island of varied
// width/thickness at a jittered offset. Grid spacing is within jump/dash reach, so from almost
// any island a neighbour sits within reach in several directions — you wander UP and SIDEWAYS
// freely across a field of platforms (the reference look), while the solid floor below stays the
// mandatory passable path. This pass writes ONLY rows above the floor and confines every write to
// this chunk's own columns, so it can't affect floor traversability, determinism, or idempotency.
constexpr int ISLE_TOP_ROW = 3;             // highest islands sit near the top of the sky
constexpr int ISLE_ROW_STEP = 5;            // ~ single-jump reach apart vertically
constexpr int ISLE_COL_STEP = 6;            // ~ jump/dash reach apart horizontally

// One island: a horizontal T_GROUND strip, `thick` rows deep.
void place_island(World& w, int tx, int ty, int width, int thick) {
    for (int t = 0; t < thick; ++t)
        for (int k = 0; k < width; ++k)
            w.map.set(tx + k, ty + t, T_GROUND);
}

// Fill the sky above the floor with a dense field of small islands (see block comment above).
void scatter_islands(World& w, int chunkStart, int chunkIndex, std::mt19937_64& rng) {
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    const int botRow   = GROUND_Y - 5;                        // leave headroom above the floor lane
    const int firstCol = (chunkIndex == 0) ? SAFE_COLS + 1 : 1; // keep spawn columns clear
    const int lastCol  = CHUNK_W - 2;                          // 1-col margin: writes stay in-chunk

    for (int row = ISLE_TOP_ROW; row <= botRow; row += ISLE_ROW_STEP) {
        int prevRight = -999, prevY = 0;                      // for same-row bridging
        for (int col = firstCol; col <= lastCol; col += ISLE_COL_STEP) {
            int gx0  = chunkStart + col;
            int zone = zone_at(gx0 * (float)TILE);
            float diff = std::min(difficulty_at(gx0 * (float)TILE), 1.0f);

            // Dense: most cells fill; sky/crystal (ZONE_VERT_MUL) and deeper runs a touch denser.
            // Clamped so it never becomes a solid ceiling.
            float pIsland = std::min(0.90f, 0.62f + 0.06f * diff + 0.08f * (ZONE_VERT_MUL[zone] - 1.0f));
            if (unit(rng) >= pIsland) continue;

            int jx = col + (int)(unit(rng) * (ISLE_COL_STEP - 2)); // jitter within the cell
            int jy = row + (int)(unit(rng) * 3.0f) - 1;            // +-1 row jitter
            jy = std::max(ISLE_TOP_ROW, std::min(botRow, jy));
            if (jx < firstCol || jx > lastCol) continue;
            int tx = chunkStart + jx;

            int width = 2 + (int)(unit(rng) * 6.0f);               // 2-7 wide (varied)
            width = std::min(width, lastCol - jx + 1);             // stay in-chunk
            if (width < 2) continue;
            int thick = (unit(rng) < 0.30f) ? 2 : 1;
            place_island(w, tx, jy, width, thick);

            float topX = (tx + width * 0.5f) * (float)TILE;
            if (chunkIndex > 0) {                                   // content on a subset (uncluttered)
                if (unit(rng) < 0.12f)                              // items are rewards, not litter
                    spawn::item(w, { topX, (jy - 1) * (float)TILE }, roll_item(rng, diff));
                if (zone >= 1 && unit(rng) < 0.08f)                 // light island enemy density on top
                    spawn_aerial(w, roll_aerial_enemy_type(rng, zone), topX, (jy + 2) * (float)TILE);
                else if (unit(rng) < 0.07f)
                    spawn_ground_enemy(w, roll_ground_enemy_type(rng, zone),
                                       (float)(tx * TILE), (float)((tx + width) * TILE), jy);
            }

            // Occasionally rope/plank-bridge to the previous same-row island across a jumpable gap.
            int dyRows = (jy > prevY) ? jy - prevY : prevY - jy;
            if (chunkIndex > 0 && prevRight > 0 && tx - prevRight >= 3 && tx - prevRight <= 11
                && dyRows <= 1 && unit(rng) < 0.45f) {
                place_bridge_platform(w, prevRight, tx - prevRight,
                                      (float)std::min(jy, prevY) * (float)TILE, zone, chunkIndex, rng);
            }
            prevRight = tx + width;
            prevY = jy;
        }
    }
}

void generate_chunk(World& w, int chunkIndex) {
    std::mt19937_64 rng(w.seed ^ ((uint64_t)chunkIndex * 0x9E3779B97F4A7C15ull));
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);

    int chunkStart = chunkIndex * CHUNK_W;
    int lc = 0; // local column cursor within this chunk

    if (chunkIndex == 0) {
        // Guaranteed-safe spawn zone: solid flat ground, no gaps/enemies/platforms/items/hazards.
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
        int zone = zone_at(gx * (float)TILE);
        int remaining = CHUNK_W - lc;
        float roll = unit(rng);

        float pGap    = (0.10f + 0.20f * diff) * ZONE_GAP_MUL[zone];
        float pHill   = (0.15f + 0.10f * diff) * ZONE_HILL_MUL[zone];
        // (vertical climbs removed — the floating-island layer now lives entirely in scatter_islands)

        if (!prevGap && roll < pGap && remaining >= 2) {
            int width = std::min({ remaining, MAX_GAP, 2 + (int)(unit(rng) * (MAX_GAP - 1)) });
            float pBridge = std::min(0.9f, 0.5f + 0.1f * (float)zone); // higher zones -> more bridges
            if (width >= 4 && unit(rng) < pBridge)
                place_bridge_platform(w, gx, width, (float)(GROUND_Y - 2) * (float)TILE, zone, chunkIndex, rng);
            // Aerial enemies hover in the air — a pit is a natural spot for one to guard.
            if (chunkIndex > 0 && zone >= 1 && width >= 3 && unit(rng) < 0.10f + 0.08f * (float)zone) {
                float fy = (float)(GROUND_Y - (3 + (int)(unit(rng) * 3.0f))) * (float)TILE;
                spawn_aerial(w, roll_aerial_enemy_type(rng, zone), (gx + width * 0.5f) * (float)TILE, fy);
            }
            lc += width;
            prevGap = true;
            continue;
        }
        bool wasGap = prevGap;
        prevGap = false;

        if (roll < pGap + pHill && remaining >= 3) {
            int tiers = (zone >= 2) ? 2 : ((zone >= 1 && unit(rng) < 0.5f) ? 2 : 1);
            if (zone >= 3 && unit(rng) < 0.4f) tiers = 3; // sky/crystal: occasional extra-tall hill
            int consumed = build_hill(w, gx, remaining, tiers, rng);
            maybe_hazard_row(w, gx, consumed, 0.12f * ZONE_SPIKE_MUL[zone], zone, rng, unit(rng));
            if (chunkIndex > 0 && unit(rng) < (0.15f + 0.20f * diff) * ZONE_ENEMY_MUL[zone]) {
                EnemyType t = roll_ground_enemy_type(rng, zone);
                spawn_ground_enemy(w, t, (float)(gx * TILE), (float)((gx + consumed) * TILE), GROUND_Y);
            }
            lc += consumed;
            continue;
        }

        // Flat (default / remainder-fitting fallback).
        int width = std::max(1, std::min(remaining, 4 + (int)(unit(rng) * 6)));
        // Landing right after a gap: small chance the very first column is a hazard (a "pit
        // bottom"-adjacent hazard) — only when there's still room to land safely beyond it.
        bool spikeLanding = wasGap && zone >= 1 && width >= 3 && unit(rng) < (0.10f + 0.08f * (float)zone);
        for (int i = 0; i < width; ++i) {
            if (i == 0 && spikeLanding) fill_ground_hazard(w, gx + i, GROUND_Y, zone == 2 ? T_LAVA : T_SPIKE);
            else fill_ground(w, gx + i, GROUND_Y);
        }
        if (chunkIndex > 0) {
            maybe_hazard_row(w, gx, width, 0.10f * ZONE_SPIKE_MUL[zone], zone, rng, unit(rng));

            float pEnemy = (0.15f + 0.25f * diff) * ZONE_ENEMY_MUL[zone];
            if (width >= 2 && unit(rng) < pEnemy) {
                EnemyType t = roll_ground_enemy_type(rng, zone);
                spawn_ground_enemy(w, t, (float)(gx * TILE), (float)((gx + width) * TILE), GROUND_Y);
            }
            // Aerial enemies also patrol plain flat stretches in the higher zones.
            if (zone >= 1 && width >= 2 && unit(rng) < (0.08f + 0.06f * (float)zone) * ZONE_ENEMY_MUL[zone]) {
                float fy = (float)(GROUND_Y - (3 + (int)(unit(rng) * 4.0f))) * (float)TILE;
                spawn_aerial(w, roll_aerial_enemy_type(rng, zone), (gx + width * 0.5f) * (float)TILE, fy);
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

    scatter_islands(w, chunkStart, chunkIndex, rng);
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

// Biome id, pure function of x: a band every ZONE_BAND tiles, cycling 0..NUM_ZONES-1 (surface,
// cavern, volcanic, sky/cloud, crystal/ice). Render colors ground per zone; this file uses it
// to vary structure mix / hazard density / enemy roster (higher id = harder, per spec),
// independent of the smooth global difficulty_at(x) ramp.
int zone_at(float x) {
    if (x < 0.0f) x = 0.0f;
    long long band = (long long)(x / ((float)ZONE_BAND * (float)TILE));
    return (int)(band % NUM_ZONES);
}

} // namespace game::worldgen
