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
static Sound g_sounds[5]{};          // indexed by (int)Event

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

void init_audio() {
    InitAudioDevice();
    g_deviceInited = true;
    g_audioReady   = IsAudioDeviceReady();
    if (!g_audioReady) return; // ponytail: headless/CI box with no device — play() stays a no-op

    constexpr unsigned SR = 22050;
    auto fire   = synth_tone(900, 650, 0.05f, SR, 0.30f, true);
    auto death  = synth_tone(550, 120, 0.25f, SR, 0.40f, false);
    auto hit    = synth_noise(0.15f, SR, 0.45f);
    auto pickup = synth_arpeggio({ 523.f, 659.f, 784.f, 1047.f }, 0.05f, SR, 0.35f);
    auto jump   = synth_tone(380, 620, 0.08f, SR, 0.25f, false);

    g_sounds[(int)Event::Fire]       = LoadSoundFromWave(wrap_wave(fire,   SR));
    g_sounds[(int)Event::EnemyDeath] = LoadSoundFromWave(wrap_wave(death,  SR));
    g_sounds[(int)Event::PlayerHit]  = LoadSoundFromWave(wrap_wave(hit,    SR));
    g_sounds[(int)Event::Pickup]     = LoadSoundFromWave(wrap_wave(pickup, SR));
    g_sounds[(int)Event::Jump]       = LoadSoundFromWave(wrap_wave(jump,   SR));
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
            burst(w, pos, 4, Color{ 255, 220, 120, 255 }, 90, 0.15f, 2.5f, 0);
            break;
        case Event::EnemyDeath:
            burst(w, pos, 14, MAROON, 170, 0.45f, 3.5f, 20);
            break;
        case Event::PlayerHit:
            burst(w, pos, 10, RAYWHITE, 150, 0.30f, 3.0f, 10);
            break;
        case Event::Pickup:
            burst(w, pos, 8, GOLD, 70, 0.55f, 3.0f, 90);
            break;
        case Event::Jump:
            burst(w, pos, 4, Color{ 170, 160, 140, 200 }, 45, 0.25f, 2.5f, 0);
            break;
    }

    if (!g_audioReady) return;
    if (e == Event::Fire) {
        // ponytail: Fire can trigger every tick while held — throttle with the process's
        // monotonic clock (GetTime(), valid here since audio-ready implies raylib is up)
        // rather than World::time, which resets to 0 on every new_game()/start_game_from_lobby()
        // and would otherwise leave Fire silenced for a while after a second playthrough.
        static double lastFireSoundAt = -1.0;
        double now = GetTime();
        if (now - lastFireSoundAt < 0.05) return;
        lastFireSoundAt = now;
    }
    PlaySound(g_sounds[(int)e]);
}

} // namespace game::fx
