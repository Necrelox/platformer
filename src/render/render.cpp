#include "core/api.hpp"
#include <cmath>
#include <vector>

// World-space renderer. Called inside BeginMode2D(cam) by main — never touch the camera or
// input here. Primitives only (no asset files exist).
namespace game::render {

namespace {

constexpr Color PLATFORM_MOVING_FILL = Color{ 70, 140, 230, 255 };
constexpr Color PLATFORM_BREAK_FLASH = Color{ 255, 90, 40, 255 };

// Per-zone ground palette (zone id 0..NUM_ZONES-1 from worldgen::zone_at): fill + lighter
// exposed-top edge. Sized to NUM_ZONES so it tracks the shared constant automatically.
constexpr Color ZONE_GROUND_FILL[NUM_ZONES] = {
    Color{ 60, 120, 60, 255 },    // 0 surface: green
    Color{ 70, 85, 100, 255 },    // 1 cavern: blue-grey
    Color{ 130, 55, 35, 255 },    // 2 volcanic: red-orange (lava biome — zone 2 is hazard-hardcoded in worldgen)
    Color{ 175, 205, 230, 255 },  // 3 sky/cloud: pale airy blue
    Color{ 70, 150, 195, 255 },   // 4 crystal/ice: cyan
    Color{ 200, 165, 95, 255 },   // 5 desert: sand/tan
    Color{ 70, 40, 95, 255 },     // 6 void/corrupt: dark purple
    Color{ 85, 115, 45, 255 },    // 7 toxic/swamp: sickly green
};
constexpr Color ZONE_GROUND_TOP[NUM_ZONES] = {
    Color{ 130, 210, 120, 255 },  // 0 grass
    Color{ 150, 170, 200, 255 },  // 1 pale stone
    Color{ 230, 110, 50, 255 },   // 2 hot orange
    Color{ 255, 255, 255, 255 },  // 3 cloud-white edge
    Color{ 195, 140, 245, 255 },  // 4 violet crystal glint
    Color{ 240, 215, 150, 255 },  // 5 bright sand
    Color{ 185, 100, 235, 255 },  // 6 magenta corruption glow
    Color{ 180, 225, 90, 255 },   // 7 acid-green rim
};
constexpr Color SPIKE_COLOR = Color{ 150, 150, 165, 255 }; // gray/steel hazard

// ---- shared floating-island-sky decor (background, island undersides, ladders/ropes, bridges) ----
// Per-zone BACKGROUND is the biggest "distinct biome" lever: each biome gets its own sky base +
// distant-mountain tint. draw_background cross-fades between adjacent zones near a band boundary so
// the world doesn't snap. ZONE_BAND_PX mirrors worldgen's ZONE_BAND (120 tiles) — same mirror
// pattern as FLOOR_TOP_Y below; keep in sync if worldgen's banding ever changes.
constexpr float ZONE_BAND_PX = 120.0f * TILE;
constexpr Color ZONE_SKY[NUM_ZONES] = {
    Color{ 28, 34, 62, 255 },    // 0 surface: dusk blue
    Color{ 12, 14, 22, 255 },    // 1 cavern: near-black
    Color{ 46, 18, 16, 255 },    // 2 volcanic: dark ember red
    Color{ 96, 146, 205, 255 },  // 3 sky/cloud: bright daytime blue
    Color{ 34, 58, 82, 255 },    // 4 ice: cold steel blue
    Color{ 78, 52, 40, 255 },    // 5 desert: warm dusk
    Color{ 30, 12, 42, 255 },    // 6 void: deep purple
    Color{ 22, 38, 24, 255 },    // 7 toxic: murky green
};
constexpr Color ZONE_MTN[NUM_ZONES] = {
    Color{ 40, 46, 68, 255 },    // 0
    Color{ 22, 24, 34, 255 },    // 1
    Color{ 60, 26, 22, 255 },    // 2
    Color{ 120, 158, 200, 255 }, // 3 (bright, hazy peaks)
    Color{ 52, 78, 104, 255 },   // 4
    Color{ 96, 66, 48, 255 },    // 5
    Color{ 46, 22, 62, 255 },    // 6
    Color{ 34, 52, 34, 255 },    // 7
};
constexpr Color CLOUD_COLOR   = Color{ 210, 215, 230, 150 }; // soft translucent cloud
constexpr Color ROPE_COLOR    = Color{ 90, 60, 35, 255 };
constexpr Color WOOD_PLANK    = Color{ 150, 105, 60, 255 };
constexpr Color WOOD_SEAM     = Color{ 100, 70, 40, 255 };
// Baseline floor top row (spec-shared formula: GROUND_Y = WORLD_H-3). Any solid tile ABOVE this
// row is island/hill/climb geometry, never the bottom floor — used to tell "floating island" from
// "ground" without a frozen-file export.
constexpr int FLOOR_TOP_Y = WORLD_H - 3;

Color darken(Color c, float f) {
    return Color{ (unsigned char)(c.r * f), (unsigned char)(c.g * f), (unsigned char)(c.b * f), c.a };
}

Color lerp_color(Color a, Color b, float t) {
    return Color{ (unsigned char)(a.r + (b.r - a.r) * t), (unsigned char)(a.g + (b.g - a.g) * t),
                  (unsigned char)(a.b + (b.b - a.b) * t), 255 };
}

// Biome background palette at world x, cross-faded over the last 15% of each band into the next
// zone (so the sky/mountains don't snap at a boundary). z0/z1 follow worldgen::zone_at's banding.
void biome_bg(float x, Color& sky, Color& mtn) {
    if (x < 0.0f) x = 0.0f;
    float bandF = x / ZONE_BAND_PX;
    int   band  = (int)bandF;
    float frac  = bandF - (float)band;
    int   z0 = band % NUM_ZONES, z1 = (band + 1) % NUM_ZONES;
    float t  = frac < 0.85f ? 0.0f : (frac - 0.85f) / 0.15f;
    sky = lerp_color(ZONE_SKY[z0], ZONE_SKY[z1], t);
    mtn = lerp_color(ZONE_MTN[z0], ZONE_MTN[z1], t);
}

// Cheap deterministic 2D hash -> stable-but-scattered decoration (ropes/ladders/parallax
// silhouettes) with no stored RNG state. Unsigned throughout: signed int*int would overflow
// (UB) once tile coordinates run into the tens of thousands over a long map.
uint32_t tile_hash(int a, int b) {
    uint32_t h = (uint32_t)a * 374761393u + (uint32_t)b * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

// Cheap parallax backdrop: dark sky wash + a slow mountain silhouette (anchored to the world
// floor line) + a scatter of clouds (anchored to the viewport so they're always in view, not
// just when climbing). Both layers' X offset is a FRACTION of the camera target, so they drift
// slower than the foreground tiles — the classic "distant" depth cue, primitives only.
void draw_background(const World& w) {
    Color sky, mtn;
    biome_bg(w.cam.target.x, sky, mtn); // per-biome sky + mountain tint (cross-faded at boundaries)

    // vertical gradient sky (darker overhead -> biome base near the horizon) for depth
    Rectangle sr{ w.cam.target.x - SCREEN_W, w.cam.target.y - SCREEN_H * 1.5f,
                  SCREEN_W * 2.0f, SCREEN_H * 3.0f };
    DrawRectangleGradientV((int)sr.x, (int)sr.y, (int)sr.width, (int)sr.height, darken(sky, 0.5f), sky);

    float floorY = (float)FLOOR_TOP_Y * TILE;

    constexpr float MOUNTAIN_PARALLAX = 0.20f; // 20% of camera speed -> reads as "far"
    constexpr float MOUNTAIN_CELL     = TILE * 12.0f;
    float mCenter = w.cam.target.x * MOUNTAIN_PARALLAX;
    int mi0 = (int)std::floor((mCenter - SCREEN_W) / MOUNTAIN_CELL) - 1;
    int mi1 = (int)std::floor((mCenter + SCREEN_W) / MOUNTAIN_CELL) + 1;
    for (int i = mi0; i <= mi1; ++i) {
        float wx = i * MOUNTAIN_CELL + w.cam.target.x * (1.0f - MOUNTAIN_PARALLAX);
        uint32_t h = tile_hash(i, 101);
        float peak = floorY - TILE * (7.0f + (float)(h % 100) / 100.0f * 9.0f); // 7-16 tiles tall
        Color tint = darken(mtn, 0.8f + 0.3f * (float)((h >> 8) % 100) / 100.0f);
        DrawTriangle({ wx, floorY }, { wx + MOUNTAIN_CELL, floorY }, { wx + MOUNTAIN_CELL * 0.5f, peak }, tint);
    }

    // Clouds: viewport-anchored in Y (near the top of the visible screen, whatever the
    // player's altitude) so the sky always reads as "full", not just while climbing high.
    constexpr float CLOUD_PARALLAX = 0.35f;
    constexpr float CLOUD_CELL     = TILE * 18.0f;
    float cCenter = w.cam.target.x * CLOUD_PARALLAX;
    int ci0 = (int)std::floor((cCenter - SCREEN_W) / CLOUD_CELL) - 1;
    int ci1 = (int)std::floor((cCenter + SCREEN_W) / CLOUD_CELL) + 1;
    for (int i = ci0; i <= ci1; ++i) {
        uint32_t h = tile_hash(i, 202);
        if (h % 3 != 0) continue; // sparse -- "a few clouds", not a solid deck
        float wx = i * CLOUD_CELL + w.cam.target.x * (1.0f - CLOUD_PARALLAX);
        float wy = w.cam.target.y - SCREEN_H * 0.38f + (float)(h % 220) - 110.0f;
        float cw = TILE * (2.0f + (float)((h >> 6) % 100) / 100.0f * 1.5f);
        DrawEllipse((int)wx, (int)wy, cw, cw * 0.45f, CLOUD_COLOR);
        DrawEllipse((int)(wx + cw * 0.7f), (int)(wy + cw * 0.1f), cw * 0.7f, cw * 0.35f, CLOUD_COLOR);
    }
}

// Wooden-plank bridge look shared by every Platform kind (Static/Moving/Breakable): a tinted
// board with seam lines + a lashed rope along the top edge and short hangers under both ends.
void draw_wood_planks(Rectangle r, Color plankTint) {
    DrawRectangleRec(r, plankTint);
    int nPlanks = (int)(r.width / (TILE * 0.5f));
    if (nPlanks < 1) nPlanks = 1;
    for (int i = 1; i < nPlanks; ++i) {
        float sx = r.x + r.width * (float)i / (float)nPlanks;
        DrawLineEx({ sx, r.y }, { sx, r.y + r.height }, 1.5f, WOOD_SEAM);
    }
    DrawLineEx({ r.x, r.y }, { r.x + r.width, r.y }, 2.0f, ROPE_COLOR);               // top lashing
    DrawLineEx({ r.x, r.y }, { r.x, r.y + r.height * 0.7f }, 1.5f, ROPE_COLOR);       // end hangers
    DrawLineEx({ r.x + r.width, r.y }, { r.x + r.width, r.y + r.height * 0.7f }, 1.5f, ROPE_COLOR);
    Vector2 midTop{ r.x + r.width * 0.5f, r.y + r.height };
    DrawLineEx(midTop, { midTop.x, midTop.y + TILE * 0.5f }, 1.5f, ROPE_COLOR);       // under-span tassel
}

void draw_spike(int tx, int ty) {
    // ponytail: a row of 3 small triangles per tile reads as "spikes" without an asset.
    float x0 = (float)(tx * TILE), y0 = (float)(ty * TILE);
    constexpr int N = 3;
    float sw = (float)TILE / N;
    for (int i = 0; i < N; ++i) {
        float sx = x0 + i * sw;
        DrawTriangle({ sx, y0 + TILE }, { sx + sw, y0 + TILE }, { sx + sw * 0.5f, y0 + TILE * 0.35f }, SPIKE_COLOR);
    }
}

// Lava: animated pulse (shared phase for the whole visible pool keeps it simple + cheap).
// `pulse` is 0..1, computed once per frame by the caller from GetTime().
void draw_lava(int tx, int ty, float pulse) {
    float x0 = (float)(tx * TILE), y0 = (float)(ty * TILE);
    Color base = Color{ (unsigned char)(190 + 40 * pulse), 55, 20, 255 };
    DrawRectangle((int)x0, (int)y0, TILE, TILE, base);
    Color core = Color{ 255, (unsigned char)(110 + 70 * pulse), 30, 255 };
    DrawRectangle((int)x0, (int)(y0 + TILE * 0.35f), TILE, (int)(TILE * 0.35f), core);
}

void draw_lava_glow(Vector2 c, float pulse) {
    float r = TILE * (0.9f + 0.25f * pulse);
    DrawCircleV(c, r, Color{ 255, 110, 40, (unsigned char)(70 + 40 * pulse) });
}

// Collects lava-tile centers (for the grouped additive glow pass) while drawing solid ground.
void draw_tiles(const World& w, std::vector<Vector2>& lavaGlow) {
    float left  = w.cam.target.x - SCREEN_W;
    float right = w.cam.target.x + SCREEN_W;
    int tx0 = (int)std::floor(left  / TILE) - 1;
    int tx1 = (int)std::floor(right / TILE) + 1;
    float pulse = 0.5f + 0.5f * std::sin((float)GetTime() * 3.0f);
    for (int tx = tx0; tx <= tx1; ++tx) {
        int zone = worldgen::zone_at((float)(tx * TILE));
        Color fill = ZONE_GROUND_FILL[zone];
        Color top  = ZONE_GROUND_TOP[zone];
        for (int ty = 0; ty < WORLD_H; ++ty) {
            uint8_t tile = w.map.at(tx, ty);
            if (tile == T_SPIKE) { draw_spike(tx, ty); continue; }
            if (tile == T_LAVA)  {
                draw_lava(tx, ty, pulse);
                lavaGlow.push_back({ tx * TILE + TILE * 0.5f, ty * TILE + TILE * 0.5f });
                continue;
            }
            if (tile == 0) continue;
            DrawRectangle(tx * TILE, ty * TILE, TILE, TILE, fill);
            if (!w.map.solid(tx, ty - 1)) // exposed surface -> lighter edge strip
                DrawRectangle(tx * TILE, ty * TILE, TILE, 4, top);

            // Floating-island underside: an island tile (above the baseline floor) with truly
            // EMPTY space directly below (not just non-solid — a hazard embedded in a hill/floor
            // is "not solid" too, but isn't a floating gap) reads as tapered, hanging dirt/rock.
            if (ty < FLOOR_TOP_Y && w.map.at(tx, ty + 1) == 0) {
                Color under = darken(fill, 0.5f);
                float ux = (float)(tx * TILE), uy = (float)(ty * TILE);
                DrawRectangleRounded({ ux, uy + TILE * 0.5f, (float)TILE, (float)TILE * 0.55f }, 0.6f, 4, under);
                float cx = ux + TILE * 0.5f, by = uy + TILE;
                DrawTriangle({ cx - TILE * 0.18f, by }, { cx + TILE * 0.18f, by }, { cx, by + TILE * 0.3f }, under);

                // Sparse render-only ladders/ropes hanging off islands — pure decoration, no
                // collision; a deterministic per-tile hash keeps them stable and sparse.
                uint32_t h = tile_hash(tx, ty);
                if (h % 9 == 0) {
                    DrawLineEx({ cx, by }, { cx, by + TILE * 2.2f }, 2.0f, ROPE_COLOR);
                    DrawCircleV({ cx, by + TILE * 2.2f }, 3.0f, ROPE_COLOR);
                } else if (h % 23 == 0) {
                    float railL = cx - TILE * 0.22f, railR = cx + TILE * 0.22f;
                    float bottom = by + TILE * 2.6f;
                    DrawLineEx({ railL, by }, { railL, bottom }, 2.0f, ROPE_COLOR);
                    DrawLineEx({ railR, by }, { railR, bottom }, 2.0f, ROPE_COLOR);
                    for (float ry = by + 5.0f; ry < bottom; ry += 8.0f)
                        DrawLineEx({ railL, ry }, { railR, ry }, 1.5f, ROPE_COLOR);
                }
            }
        }
    }
}

void draw_platforms(const World& w) {
    for (const auto& p : w.platforms) {
        if (!p.active) continue;
        switch (p.kind) {
            case PlatformKind::Static:
                draw_wood_planks(p.rect, WOOD_PLANK);
                break;
            case PlatformKind::Moving: {
                // Blend wood + the existing blue "moving" tint so that cue survives the new look.
                Color blended{ (unsigned char)((WOOD_PLANK.r + PLATFORM_MOVING_FILL.r) / 2),
                               (unsigned char)((WOOD_PLANK.g + PLATFORM_MOVING_FILL.g) / 2),
                               (unsigned char)((WOOD_PLANK.b + PLATFORM_MOVING_FILL.b) / 2), 255 };
                draw_wood_planks(p.rect, blended);
                DrawRectangleLinesEx(p.rect, 2, DARKBLUE);
                break;
            }
            case PlatformKind::Breakable: {
                // ponytail: flash phase derived from world time (deterministic, no extra
                // per-platform timer needed) — good enough for a "cracking" cue.
                bool flash = p.triggered && std::fmod(w.time, 0.2f) < 0.1f;
                draw_wood_planks(p.rect, flash ? PLATFORM_BREAK_FLASH : WOOD_PLANK);
                if (p.triggered) {
                    float x0 = p.rect.x, y0 = p.rect.y;
                    float x1 = p.rect.x + p.rect.width, y1 = p.rect.y + p.rect.height;
                    DrawLine((int)x0, (int)y0, (int)x1, (int)y1, BLACK);
                    DrawLine((int)x1, (int)y0, (int)x0, (int)y1, BLACK);
                }
                break;
            }
        }
    }
}

Vector2 item_center(const Item& it) {
    Vector2 pos = it.pos;
    pos.y += std::sin(it.t * 4.0f) * 4.0f; // vertical bob
    return { pos.x + it.size.x * 0.5f, pos.y + it.size.y * 0.5f };
}

void draw_item(const Item& it) {
    Vector2 c = item_center(it);
    float r = it.size.x * 0.35f;
    switch (it.type) {
        case ItemType::Heart:
            DrawCircleV({ c.x - r * 0.5f, c.y - r * 0.3f }, r * 0.6f, RED);
            DrawCircleV({ c.x + r * 0.5f, c.y - r * 0.3f }, r * 0.6f, RED);
            DrawTriangle({ c.x - r, c.y }, { c.x, c.y + r }, { c.x + r, c.y }, RED);
            break;
        case ItemType::RapidFire:
            DrawTriangle({ c.x, c.y - r }, { c.x - r * 0.8f, c.y + r }, { c.x + r * 0.8f, c.y + r }, YELLOW);
            break;
        case ItemType::DoubleShot:
            DrawCircleV({ c.x - r * 0.4f, c.y }, r * 0.6f, PURPLE);
            DrawCircleV({ c.x + r * 0.4f, c.y }, r * 0.6f, PURPLE);
            break;
        case ItemType::Invincibility:
            DrawPoly(c, 6, r, 0, GOLD);
            DrawPolyLines(c, 6, r, 0, WHITE);
            break;
        // ItemType::Coin: coins removed from the game (spec) — enum kept (types.hpp frozen),
        // just no longer drawn. No default: needed, the switch simply has no case for it.
    }
}

constexpr Color AGGRO_TINT        = Color{ 220, 40, 40, 255 };  // shared aggro cue
constexpr Color FLYER_FILL        = Color{ 150, 120, 220, 255 }; // violet, distinct from ground enemies
constexpr Color SHOOTER_FILL      = Color{ 90, 90, 100, 255 };   // gunmetal turret
constexpr Color CHARGER_FILL      = Color{ 120, 55, 30, 255 };   // bulky brown-red
constexpr Color CHARGER_TELEGRAPH = Color{ 255, 200, 40, 255 }; // wind-up flash
constexpr Color JUMPER_FILL       = Color{ 70, 200, 90, 255 };  // frog green
constexpr Color BOMBER_FILL       = Color{ 35, 35, 42, 255 };   // dark sphere

void draw_enemy(const Enemy& e_) {
    // Hit-stun feedback needs to be unmistakable on EVERY enemy type. Rather than threading a
    // scale factor through each type's bespoke shapes below, work on a local, grown-around-
    // center COPY of the enemy: every branch already reads e.pos/e.size, so they all pop larger
    // "for free" with zero per-shape changes.
    Enemy e = e_;
    bool hit = e.knockback > 0.0f;
    if (hit) {
        constexpr float POP = 1.22f; // ~22% bigger while knocked back
        Vector2 grow{ e.size.x * (POP - 1.0f), e.size.y * (POP - 1.0f) };
        e.pos.x -= grow.x * 0.5f; e.pos.y -= grow.y * 0.5f;
        e.size.x += grow.x; e.size.y += grow.y;
    }
    bool aggro = e.state == EnemyState::Aggro;
    // Hit-stun takes over the whole silhouette: a bright flash while the AI yields to knockback.
    auto tint = [&](Color base) { return hit ? WHITE : base; };
    switch (e.type) {
        case EnemyType::Flyer: {
            // floating oval + a pair of wings, no ground-contact "eye" needed
            Color body = tint(aggro ? Color{ 220, 90, 200, 255 } : FLYER_FILL);
            Vector2 c{ e.pos.x + e.size.x * 0.5f, e.pos.y + e.size.y * 0.5f };
            float wingY = c.y - std::sin(e.t * 8.0f) * 3.0f; // gentle flap tied to bob phase
            // Mirroring the right wing's x-offsets flips its winding; raylib backface-culls
            // clockwise triangles by default, so the last two vertices are swapped here to
            // keep this one counter-clockwise too (verified empirically against libraylib).
            DrawTriangle({ c.x - e.size.x * 0.5f, wingY }, { c.x - e.size.x * 0.3f, wingY - 2 },
                         { c.x - e.size.x * 0.95f, wingY - 5 }, WHITE);
            DrawTriangle({ c.x + e.size.x * 0.5f, wingY }, { c.x + e.size.x * 0.95f, wingY - 5 },
                         { c.x + e.size.x * 0.3f, wingY - 2 }, WHITE);
            DrawEllipse((int)c.x, (int)c.y, e.size.x * 0.55f, e.size.y * 0.4f, body);
            float eyeX = e.facing == Facing::Right ? c.x + e.size.x * 0.15f : c.x - e.size.x * 0.15f;
            DrawCircleV({ eyeX, c.y }, 2.0f, BLACK);
            break;
        }
        case EnemyType::Shooter: {
            // turret-ish body + a barrel rectangle poking out toward facing
            Color body = tint(aggro ? AGGRO_TINT : SHOOTER_FILL);
            DrawRectangleV(e.pos, e.size, body);
            DrawCircleV({ e.pos.x + e.size.x * 0.5f, e.pos.y + e.size.y * 0.35f }, e.size.x * 0.32f, DARKGRAY);
            float barrelLen = e.size.x * 0.55f;
            float barrelY   = e.pos.y + e.size.y * 0.35f;
            float barrelX   = e.facing == Facing::Right ? e.pos.x + e.size.x : e.pos.x - barrelLen;
            DrawRectangle((int)barrelX, (int)(barrelY - 2), (int)barrelLen, 4, BLACK);
            break;
        }
        case EnemyType::Charger: {
            // bulkier silhouette; flashes a bright telegraph color while winding up an aggro charge
            bool windUp = aggro && std::fmod(e.t, 0.2f) < 0.1f;
            Color body = tint(windUp ? CHARGER_TELEGRAPH : (aggro ? AGGRO_TINT : CHARGER_FILL));
            Rectangle bulk{ e.pos.x - e.size.x * 0.1f, e.pos.y - e.size.y * 0.1f,
                            e.size.x * 1.2f, e.size.y * 1.1f };
            DrawRectangleRec(bulk, body);
            float eyeY = e.pos.y + e.size.y * 0.3f;
            float eyeX = e.facing == Facing::Right ? e.pos.x + e.size.x * 0.8f : e.pos.x + e.size.x * 0.2f;
            DrawCircleV({ eyeX, eyeY }, 2.5f, WHITE);
            break;
        }
        case EnemyType::Jumper: {
            // frog-ish: squats wide+short on the ground, stretches tall+thin mid-hop.
            Color body = tint(aggro ? AGGRO_TINT : JUMPER_FILL);
            float sx = 1.0f, sy = 1.0f;
            if (e.onGround)        { sx = 1.15f; sy = 0.75f; }      // squashed sitting
            else if (e.vel.y < -50.0f) { sx = 0.85f; sy = 1.25f; }  // stretched going up
            Vector2 sz{ e.size.x * sx, e.size.y * sy };
            Vector2 pos{ e.pos.x + (e.size.x - sz.x) * 0.5f, e.pos.y + (e.size.y - sz.y) }; // bottom stays put
            DrawRectangleRounded({ pos.x, pos.y, sz.x, sz.y }, 0.5f, 6, body);
            float eyeY = pos.y + sz.y * 0.18f;
            float eyeR = e.size.x * 0.2f;
            float eyeXL = pos.x + sz.x * 0.28f, eyeXR = pos.x + sz.x * 0.72f;
            DrawCircleV({ eyeXL, eyeY }, eyeR, WHITE);
            DrawCircleV({ eyeXR, eyeY }, eyeR, WHITE);
            DrawCircleV({ eyeXL, eyeY }, eyeR * 0.5f, BLACK);
            DrawCircleV({ eyeXR, eyeY }, eyeR * 0.5f, BLACK);
            break;
        }
        case EnemyType::Bomber: {
            // dark sphere with a lit, blinking fuse — reads as "about to explode".
            Vector2 c{ e.pos.x + e.size.x * 0.5f, e.pos.y + e.size.y * 0.5f };
            float r = e.size.x * 0.45f;
            Color body = tint(aggro ? Color{ 90, 25, 25, 255 } : BOMBER_FILL);
            DrawCircleV(c, r, body);
            DrawCircleLines((int)c.x, (int)c.y, r, Color{ 10, 10, 10, 255 });
            Vector2 fuseBase{ c.x, c.y - r };
            Vector2 fuseTip{ c.x + 3, c.y - r - 8 };
            DrawLineEx(fuseBase, fuseTip, 2.0f, Color{ 90, 60, 30, 255 });
            bool lit = std::fmod(e.t, 0.3f) < 0.15f;
            DrawCircleV(fuseTip, lit ? 3.5f : 2.0f, lit ? Color{ 255, 220, 80, 255 } : Color{ 200, 120, 20, 255 });
            break;
        }
        case EnemyType::Walker:
        default: {
            Color body = tint(aggro ? AGGRO_TINT : MAROON);
            DrawRectangleV(e.pos, e.size, body);
            float eyeY = e.pos.y + e.size.y * 0.3f;
            float eyeX = e.facing == Facing::Right ? e.pos.x + e.size.x * 0.75f : e.pos.x + e.size.x * 0.25f;
            DrawCircleV({ eyeX, eyeY }, 2.5f, WHITE);
            break;
        }
    }
    if (hit) // bright outline on top of the flash+pop so a hit reads unmistakably, any type
        DrawRectangleLinesEx({ e.pos.x - 3, e.pos.y - 3, e.size.x + 6, e.size.y + 6 }, 3.0f,
                              Color{ 255, 255, 160, 255 });
}

void draw_enemy_glow(const Enemy& e) {
    bool aggro  = e.state == EnemyState::Aggro;
    bool bomber = e.type == EnemyType::Bomber;
    if (!aggro && !bomber) return;
    Vector2 c{ e.pos.x + e.size.x * 0.5f, e.pos.y + e.size.y * 0.5f };
    Color glow = bomber ? Color{ 255, 80, 40, 90 } : Color{ 255, 60, 60, 70 };
    float r = e.size.x * (bomber ? 1.05f : 0.85f);
    DrawCircleV(c, r, glow);
}

// Player shots warm yellow, enemy shots magenta, bombs red-hot (larger glow).
Color projectile_color(const Projectile& pr) {
    if (pr.bomb) return Color{ 255, 90, 30, 255 };
    return pr.fromPlayer ? Color{ 255, 220, 80, 255 } : Color{ 255, 60, 200, 255 };
}

void draw_projectile(const Projectile& pr) {
    Color col = projectile_color(pr);
    float r = pr.bomb ? 6.0f : 4.0f;
    // fading streak behind the projectile, opposite its velocity ("trace sur les bullets")
    float speed = std::sqrt(pr.vel.x * pr.vel.x + pr.vel.y * pr.vel.y);
    if (speed > 1.0f) {
        Vector2 dir{ -pr.vel.x / speed, -pr.vel.y / speed };
        constexpr int N = 4;
        for (int i = 1; i <= N; ++i) {
            float t = (float)i / N;
            Vector2 p{ pr.pos.x + dir.x * t * r * 3.0f, pr.pos.y + dir.y * t * r * 3.0f };
            DrawCircleV(p, r * (1.0f - t * 0.6f), Fade(col, (1.0f - t) * 0.5f));
        }
    }
    DrawCircleV(pr.pos, r, col);
}

void draw_projectile_glow(const Projectile& pr) {
    Color col = projectile_color(pr);
    float r = pr.bomb ? 16.0f : 9.0f;
    DrawCircleV(pr.pos, r, Fade(col, pr.bomb ? 0.5f : 0.35f));
}

void draw_player(const Player& pl) {
    if (pl.dying) {
        // ponytail: classic sheet-ghost — a pale rounded silhouette that rises and fades out
        // over deathTimer -> 0. No normal body/nose while dying (per contract).
        float t = pl.deathTimer / DEATH_ANIM_TIME; // 1 -> 0
        float rise = (1.0f - t) * pl.size.y;       // floats upward as it fades
        Vector2 top{ pl.pos.x, pl.pos.y - rise };
        Rectangle body{ top.x, top.y, pl.size.x, pl.size.y * 0.85f };
        Color ghost = Fade(Color{ 225, 240, 255, 255 }, t * 0.8f);
        DrawRectangleRounded(body, 0.7f, 8, ghost);
        Vector2 c{ top.x + pl.size.x * 0.5f, top.y + pl.size.y * 0.35f };
        Color eye = Fade(BLACK, t * 0.8f);
        DrawCircleV({ c.x - pl.size.x * 0.18f, c.y }, 2.5f, eye);
        DrawCircleV({ c.x + pl.size.x * 0.18f, c.y }, 2.5f, eye);
        return;
    }
    if (pl.dashTimer > 0.0f) {
        // motion-streak afterimages trailing behind the dash direction
        for (int i = 1; i <= 3; ++i) {
            float off = -pl.dashDir * i * pl.size.x * 0.55f;
            Rectangle r{ pl.pos.x + off, pl.pos.y, pl.size.x, pl.size.y };
            DrawRectangleRec(r, Fade(pl.color, 0.22f * (4 - i) / 3.0f));
        }
    }
    // ponytail: blink thresholds invuln's own value directly (no extra frame-counter state).
    // Ceiling: assumes invuln only ever counts down at a steady rate (true today via
    // physics::step) — if invuln ever pauses, swap for a GetTime()-based blink instead.
    bool hidden = pl.invuln > 0 && std::fmod(pl.invuln, 0.2f) < 0.1f;
    if (!hidden) {
        DrawRectangleV(pl.pos, pl.size, pl.color);
        float noseY = pl.pos.y + pl.size.y * 0.35f;
        float dir   = pl.facing == Facing::Right ? 1.0f : -1.0f;
        float noseX = pl.facing == Facing::Right ? pl.pos.x + pl.size.x : pl.pos.x;
        DrawTriangle({ noseX, noseY }, { noseX, noseY + pl.size.y * 0.25f },
                     { noseX + dir * 8, noseY + pl.size.y * 0.125f }, BLACK);
    }
    if (pl.invincible)
        DrawRectangleLinesEx({ pl.pos.x - 2, pl.pos.y - 2, pl.size.x + 4, pl.size.y + 4 }, 3, GOLD);
}

void draw_dash_glow(const Player& pl) {
    if (pl.dashTimer <= 0.0f) return;
    Vector2 c{ pl.pos.x + pl.size.x * 0.5f - pl.dashDir * pl.size.x * 0.6f, pl.pos.y + pl.size.y * 0.5f };
    DrawCircleV(c, pl.size.x * 0.9f, Fade(pl.color, 0.35f));
}

} // namespace

void draw_world(const World& w) {
    draw_background(w);
    std::vector<Vector2> lavaGlow;
    draw_tiles(w, lavaGlow);
    draw_platforms(w);
    for (const auto& it : w.items)       if (it.alive) draw_item(it);
    for (const auto& e  : w.enemies)     if (e.alive)  draw_enemy(e);
    for (const auto& pr : w.projectiles) if (pr.alive) draw_projectile(pr);
    for (const auto& pl : w.players)     if (pl.alive) draw_player(pl);

    // Grouped additive pass: every glow/light/trail effect draws inside one blend-mode toggle
    // ("shaders lumiere sur les tirs") instead of flipping BLEND_ADDITIVE per shape.
    float pulse = 0.5f + 0.5f * std::sin((float)GetTime() * 3.0f);
    BeginBlendMode(BLEND_ADDITIVE);
    for (const auto& c : lavaGlow) draw_lava_glow(c, pulse);
    for (const auto& e  : w.enemies)     if (e.alive)  draw_enemy_glow(e);
    for (const auto& pr : w.projectiles) if (pr.alive) draw_projectile_glow(pr);
    for (const auto& pl : w.players)     if (pl.alive) draw_dash_glow(pl);
    EndBlendMode();
}

} // namespace game::render
