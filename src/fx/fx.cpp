#include "core/api.hpp"
#include <cmath>
#include <cstdint>
#include <vector>

// Phase 11: particles (World::particles, cosmetic/local-only) + procedurally synthesized SFX.
// No asset files anywhere — every Wave is built in memory and handed to LoadSoundFromWave.
namespace game::fx {

// ---- particle cap ----
// ponytail: hard cap, no LRU/priority eviction. If a screen full of simultaneous deaths ever
// looks starved, raise the cap or drop oldest first — 400 is comfortably above what a 4-player
// screen produces at once.
static constexpr size_t MAX_PARTICLES = 400;

// ---- cheap deterministic "random": integer hash of an incrementing counter. ----
// ponytail: not statistically rigorous, just needs to *look* varied frame to frame; a real PRNG
// (e.g. std::mt19937) would be overkill for confetti.
static uint32_t g_rngCounter = 0;
static float hash01(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return (float)(x & 0xFFFFFFu) / (float)0x1000000u; // [0,1)
}
static float rand01() { return hash01(g_rngCounter++); }

static void burst(World& w, Vector2 pos, int count, Color color,
                   float speed, float life, float size, float upBias) {
    if (w.particles.size() > MAX_PARTICLES) return; // capped — drop this burst entirely
    for (int i = 0; i < count; ++i) {
        float ang = rand01() * 6.2831853f;
        float spd = speed * (0.5f + 0.5f * rand01());
        Particle pt;
        pt.pos     = pos;
        pt.vel     = { std::cos(ang) * spd, std::sin(ang) * spd - upBias };
        pt.life    = 0;
        pt.maxLife = life * (0.7f + 0.6f * rand01());
        pt.size    = size * (0.7f + 0.6f * rand01());
        pt.color   = color;
        w.particles.push_back(pt);
    }
}

// Ring of particles spawned around `pos` and launched INWARD toward it — the mirror image of
// burst(). Used for EnemyDeath's "void" implosion: things get sucked toward the death point
// instead of flung away from it.
static void implode(World& w, Vector2 pos, int count, Color color,
                     float ringRadius, float speed, float life, float size) {
    if (w.particles.size() > MAX_PARTICLES) return; // capped — drop this burst entirely
    for (int i = 0; i < count; ++i) {
        float ang = rand01() * 6.2831853f;
        float r   = ringRadius * (0.7f + 0.6f * rand01());
        float cx  = std::cos(ang), sy = std::sin(ang);
        float spd = speed * (0.6f + 0.5f * rand01());
        Particle pt;
        pt.pos     = { pos.x + cx * r, pos.y + sy * r };
        pt.vel     = { -cx * spd, -sy * spd };            // inward, toward pos
        pt.life    = 0;
        pt.maxLife = life * (0.7f + 0.6f * rand01());
        pt.size    = size * (0.7f + 0.6f * rand01());
        pt.color   = color;
        w.particles.push_back(pt);
    }
}

// Evenly-spaced ring of particles moving outward at a uniform speed — reads as one coherent
// expanding ring rather than burst()'s scattered circle (burst() randomizes each particle's
// angle/speed independently, which blurs distinct rings together). Stacking a couple of these at
// different speeds/lifetimes gives a concentric-shockwave look (Dash). xStretch widens the ring
// horizontally to read as a *horizontal* shockwave — a dash is always a horizontal move, but
// emit() only carries a position (see api.hpp), not a direction, so left/right is faked with a
// symmetric stretch instead of a true forward/behind fan.
static void ring(World& w, Vector2 pos, int count, Color color,
                  float speed, float life, float size, float xStretch) {
    if (w.particles.size() > MAX_PARTICLES) return; // capped — drop this ring entirely
    float rot = rand01() * 6.2831853f; // rotate so stacked rings don't align particle-for-particle
    for (int i = 0; i < count; ++i) {
        float ang = rot + (6.2831853f * i) / count;
        Particle pt;
        pt.pos     = pos;
        pt.vel     = { std::cos(ang) * speed * xStretch, std::sin(ang) * speed };
        pt.life    = 0;
        pt.maxLife = life * (0.9f + 0.2f * rand01());
        pt.size    = size;
        pt.color   = color;
        w.particles.push_back(pt);
    }
}

void update(World& w, float dt) {
    static constexpr float GRAVITY = 260.0f; // px/s^2, gentle drift — not the platforming gravity
    for (auto& p : w.particles) {
        p.pos.x += p.vel.x * dt;
        p.pos.y += p.vel.y * dt;
        p.vel.y += GRAVITY * dt;
        p.life  += dt;
    }
    std::erase_if(w.particles, [](const Particle& p) { return p.life >= p.maxLife; });
}

void draw(const World& w) {
    for (const auto& p : w.particles) {
        float t     = p.maxLife > 0 ? p.life / p.maxLife : 1.0f;
        float alpha = t >= 1.0f ? 0.0f : 1.0f - t;
        Color c     = p.color;
        c.a         = (unsigned char)(alpha * c.a);
        DrawCircleV(p.pos, p.size, c);
    }
}

// ==================== procedural audio ====================

static bool  g_deviceInited = false; // InitAudioDevice() was called (need matching Close)
static bool  g_audioReady   = false; // device came up — safe to touch Sound/volume APIs
static Sound g_sounds[9]{};          // indexed by (int)Event — 9 events (EnemyHit/Dash/Explosion new)

static Wave wrap_wave(std::vector<short>& samples, unsigned sampleRate) {
    Wave w{};
    w.frameCount = (unsigned)samples.size();
    w.sampleRate = sampleRate;
    w.sampleSize = 16;
    w.channels   = 1;
    w.data       = samples.data(); // LoadSoundFromWave copies out of this before we return
    return w;
}

// Sine/square tone, linear frequency sweep + linear decay envelope.
// ponytail: phase advances by freq(t)*t rather than integrating a true sweep — fine for a
// blip under 0.3s, would drift audibly on a longer sustained sweep.
static std::vector<short> synth_tone(float freqStart, float freqEnd, float durationSec,
                                      unsigned sampleRate, float amp, bool square) {
    unsigned n = (unsigned)(durationSec * sampleRate);
    std::vector<short> s(n);
    for (unsigned i = 0; i < n; ++i) {
        float t    = (float)i / sampleRate;
        float freq = freqStart + (freqEnd - freqStart) * (t / durationSec);
        float ph   = 6.2831853f * freq * t;
        float v    = square ? (std::sin(ph) >= 0 ? 1.0f : -1.0f) : std::sin(ph);
        float env  = 1.0f - (t / durationSec);
        s[i] = (short)(v * amp * env * 32000.0f);
    }
    return s;
}

// White noise burst with linear decay — used for the hit buzz.
static std::vector<short> synth_noise(float durationSec, unsigned sampleRate, float amp) {
    unsigned n = (unsigned)(durationSec * sampleRate);
    std::vector<short> s(n);
    uint32_t seed = 0x9E3779B9u;
    for (unsigned i = 0; i < n; ++i) {
        seed = seed * 1664525u + 1013904223u; // classic LCG
        float r   = ((seed >> 8) & 0xFFFF) / 65535.0f * 2.0f - 1.0f;
        float t   = (float)i / sampleRate;
        float env = 1.0f - (t / durationSec);
        s[i] = (short)(r * amp * env * 32000.0f);
    }
    return s;
}

// Sequence of short sine notes, each with its own decay — used for the pickup arpeggio.
static std::vector<short> synth_arpeggio(std::initializer_list<float> freqs, float noteDur,
                                          unsigned sampleRate, float amp) {
    std::vector<short> s;
    unsigned perNote = (unsigned)(noteDur * sampleRate);
    s.reserve(perNote * freqs.size());
    for (float f : freqs) {
        for (unsigned i = 0; i < perNote; ++i) {
            float t   = (float)i / sampleRate;
            float env = 1.0f - (t / noteDur);
            float v   = std::sin(6.2831853f * f * t);
            s.push_back((short)(v * amp * env * 32000.0f));
        }
    }
    return s;
}

// Elementwise-add two clips (e.g. a low thump + noise for a boom) — clamped to avoid wraparound.
static std::vector<short> mix(const std::vector<short>& a, const std::vector<short>& b) {
    size_t n = a.size() > b.size() ? a.size() : b.size();
    std::vector<short> out(n);
    for (size_t i = 0; i < n; ++i) {
        int v = (i < a.size() ? a[i] : 0) + (i < b.size() ? b[i] : 0);
        out[i] = (short)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
    }
    return out;
}

// Like mix(), but overlays `b` starting `offsetSec` into `a` instead of aligning both clips to
// sample 0 — e.g. landing a bright little tick near the end of a longer sweep for a two-beat
// "collapse, then rebound" sound out of clips synth_tone/synth_noise already build separately.
static std::vector<short> mix_at(const std::vector<short>& a, const std::vector<short>& b,
                                  float offsetSec, unsigned sampleRate) {
    size_t offset = (size_t)(offsetSec * sampleRate);
    size_t n = a.size() > offset + b.size() ? a.size() : offset + b.size();
    std::vector<short> out(n, 0);
    for (size_t i = 0; i < a.size(); ++i) out[i] = a[i];
    for (size_t i = 0; i < b.size(); ++i) {
        int v = out[offset + i] + b[i];
        out[offset + i] = (short)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
    }
    return out;
}

void init_audio() {
    InitAudioDevice();
    g_deviceInited = true;
    g_audioReady   = IsAudioDeviceReady();
    if (!g_audioReady) return; // ponytail: headless/CI box with no device — play() stays a no-op

    constexpr unsigned SR = 22050;
    auto fire   = synth_tone(900, 650, 0.05f, SR, 0.30f, true);
    // EnemyDeath: "void" implosion — a long deep downward sweep + noise whoosh (the enemy getting
    // sucked in), with a small bright upward tick landed near the end (mix_at) for the rebound
    // flash moment, instead of the old plain 0.25s downward blip.
    auto deathSweep = synth_tone(700, 55, 0.35f, SR, 0.42f, false);
    auto deathAir   = synth_noise(0.35f, SR, 0.18f);
    auto deathPop   = synth_tone(1500, 2000, 0.05f, SR, 0.22f, false);
    auto death      = mix_at(mix(deathSweep, deathAir), deathPop, 0.28f, SR);
    auto hit    = synth_noise(0.15f, SR, 0.45f);
    auto pickup = synth_arpeggio({ 523.f, 659.f, 784.f, 1047.f }, 0.05f, SR, 0.35f);
    auto jump   = synth_tone(380, 620, 0.08f, SR, 0.25f, false);
    auto pdeath = synth_tone(750, 160, 0.6f, SR, 0.30f, false); // soft downward wail/whoosh

    // EnemyHit: short percussive crunch — fast down-chirp square blip layered on a tight noise
    // burst. Shorter + brighter than PlayerHit's plain 0.15s noise so the two read as distinct.
    // Louder than before (was 0.40/0.30 amp) so it reads as a crisp "mini hit" over the rest of
    // the mix instead of getting lost.
    auto enemyHit = mix(synth_noise(0.07f, SR, 0.50f), synth_tone(1500, 280, 0.09f, SR, 0.38f, true));
    // Dash: airy whoosh — noise burst with a fast downward sweep riding on top for "air" motion.
    auto dash     = mix(synth_noise(0.12f, SR, 0.28f), synth_tone(1800, 300, 0.12f, SR, 0.18f, false));
    // Explosion: boom — low sine thump (the "body") + noise (the "crack").
    auto explosion = mix(synth_tone(140, 35, 0.3f, SR, 0.55f, false), synth_noise(0.3f, SR, 0.35f));

    g_sounds[(int)Event::Fire]       = LoadSoundFromWave(wrap_wave(fire,   SR));
    g_sounds[(int)Event::EnemyDeath] = LoadSoundFromWave(wrap_wave(death,  SR));
    g_sounds[(int)Event::PlayerHit]  = LoadSoundFromWave(wrap_wave(hit,    SR));
    g_sounds[(int)Event::Pickup]     = LoadSoundFromWave(wrap_wave(pickup, SR));
    g_sounds[(int)Event::Jump]       = LoadSoundFromWave(wrap_wave(jump,   SR));
    g_sounds[(int)Event::PlayerDeath]= LoadSoundFromWave(wrap_wave(pdeath, SR));
    g_sounds[(int)Event::EnemyHit]   = LoadSoundFromWave(wrap_wave(enemyHit, SR));
    g_sounds[(int)Event::Dash]       = LoadSoundFromWave(wrap_wave(dash,     SR));
    g_sounds[(int)Event::Explosion]  = LoadSoundFromWave(wrap_wave(explosion, SR));
    // No UnloadWave calls: the Waves above point at local std::vectors, not RL_MALLOC'd memory —
    // LoadSoundFromWave copies the samples into its own buffer, so the vectors just go out of scope.
}

void shutdown_audio() {
    if (g_audioReady) for (auto& s : g_sounds) UnloadSound(s);
    if (g_deviceInited) CloseAudioDevice();
    g_audioReady = g_deviceInited = false;
}

void set_volume(float v) {
    if (g_audioReady) SetMasterVolume(v);
}

void emit(World& w, Event e, Vector2 pos) {
    switch (e) {
        case Event::Fire:
            burst(w, pos, 5, Color{ 255, 220, 120, 255 }, 100, 0.15f, 2.5f, 0);
            break;
        case Event::EnemyDeath:
            // VOID implosion: a ring rushes INWARD (collapse) — shorter life than the layers
            // below so it visibly converges first — then a bright rebound flash and a dark
            // negative-space core outlast it, so what's left reads as "sucked into a void"
            // rather than a burst flung outward.
            implode(w, pos, 16, Color{ 70, 10, 95, 255 },   28, 260, 0.22f, 3.6f); // collapsing ring
            burst(w,   pos, 10, Color{ 235, 220, 255, 255 }, 130, 0.30f, 4.2f, 0); // bright rebound flash
            burst(w,   pos, 6,  Color{ 12, 0, 20, 255 },      20, 0.42f, 4.5f, 0); // dark void core, lingers longest
            break;
        case Event::PlayerHit:
            burst(w, pos, 14, RAYWHITE,                     175, 0.32f, 3.6f, 12);
            burst(w, pos, 6,  Color{ 255, 130, 130, 220 },  145, 0.30f, 3.0f, 8);
            break;
        case Event::Pickup:
            burst(w, pos, 8, GOLD, 70, 0.55f, 3.0f, 90);
            break;
        case Event::Jump:
            burst(w, pos, 4, Color{ 170, 160, 140, 200 }, 45, 0.25f, 2.5f, 0);
            break;
        case Event::PlayerDeath:
            // Same VOID implosion as an enemy death (collapse -> rebound flash -> dark core)...
            implode(w, pos, 16, Color{ 70, 10, 95, 255 },   28, 260, 0.22f, 3.6f);
            burst(w,   pos, 10, Color{ 235, 220, 255, 255 }, 130, 0.30f, 4.2f, 0);
            burst(w,   pos, 6,  Color{ 12, 0, 20, 255 },      20, 0.42f, 4.5f, 0);
            // ...PLUS the player's own rising, fading pale-blue/white ghost/phantom burst on top.
            burst(w, pos, 16, Color{ 210, 235, 255, 220 }, 40, 0.9f, 4.0f, 130);
            break;
        case Event::EnemyHit:
            // white/yellow sparks — bumped count/speed/size so a hit visibly pops (still smaller
            // than EnemyDeath's burst so death still reads as the bigger event).
            burst(w, pos, 7, RAYWHITE,                    170, 0.20f, 2.6f, 18);
            burst(w, pos, 7, Color{ 255, 240, 140, 255 }, 190, 0.22f, 3.0f, 24);
            break;
        case Event::Dash:
            // Warp-y distortion: two concentric expanding rings at different speeds/lifetimes
            // (so they read as separate rings, not one blob) plus a fast bright core-pop,
            // stretched wider on the horizontal axis since a dash is always a horizontal move —
            // sells a shockwave even without a direction (emit() only carries pos; see api.hpp).
            ring(w,  pos, 14, Color{ 225, 240, 255, 235 }, 260, 0.22f, 3.2f, 1.7f);
            ring(w,  pos, 10, Color{ 150, 195, 255, 180 }, 150, 0.32f, 4.2f, 1.9f);
            burst(w, pos, 8,  Color{ 245, 250, 255, 255 }, 300, 0.14f, 2.4f, 0);
            break;
        case Event::Explosion:
            // big fiery burst — orange core + red stragglers + a white-hot flash center, biased upward
            burst(w, pos, 26, ORANGE,                       250, 0.55f, 7.5f, 75);
            burst(w, pos, 12, Color{ 200, 40, 20, 255 },    170, 0.65f, 6.0f, 45);
            burst(w, pos, 8,  Color{ 255, 250, 210, 255 },   90, 0.25f, 5.5f, 25);
            break;
    }

    if (!g_audioReady) return;
    if (e == Event::Fire || e == Event::EnemyHit) {
        // ponytail: Fire can trigger every tick while held, and rapid-fire pellets can chew
        // through one enemy fast — throttle both with the process's monotonic clock (GetTime(),
        // valid here since audio-ready implies raylib is up) rather than World::time, which
        // resets to 0 on every new_game()/start_game_from_lobby() and would otherwise leave the
        // sound silenced for a while after a second playthrough.
        static double lastFireSoundAt = -1.0, lastHitSoundAt = -1.0;
        double& last = (e == Event::Fire) ? lastFireSoundAt : lastHitSoundAt;
        double now = GetTime();
        if (now - last < 0.05) return;
        last = now;
    }
    PlaySound(g_sounds[(int)e]);
}

} // namespace game::fx
