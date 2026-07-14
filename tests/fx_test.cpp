#include "core/api.hpp"
#include <cassert>
#include <cstdio>

// Headless self-check for src/fx/fx.cpp — never calls init_audio()/InitWindow, so emit()/update()
// must work purely on World::particles with no audio device present (play() stays a no-op).
using namespace game;

int main() {
    World w;
    assert(w.particles.empty());

    fx::emit(w, fx::Event::Fire, { 10, 10 });
    assert(!w.particles.empty());
    size_t afterFire = w.particles.size();

    fx::emit(w, fx::Event::EnemyDeath, { 20, 20 });
    fx::emit(w, fx::Event::PlayerHit,  { 30, 30 });
    fx::emit(w, fx::Event::Pickup,     { 40, 40 });
    fx::emit(w, fx::Event::Jump,       { 50, 50 });
    assert(w.particles.size() > afterFire);
    printf("emit: particles grew to %zu\n", w.particles.size());

    // Spam far more bursts than the cap allows — must not grow unbounded.
    for (int i = 0; i < 200; ++i) fx::emit(w, fx::Event::EnemyDeath, { 0, 0 });
    assert(w.particles.size() < 500); // cap is 400; a couple of in-flight bursts may land just over
    printf("cap: particles held at %zu after spam\n", w.particles.size());

    // Advance well past every event's maxLife (<=~0.55s) -> all particles must be erased.
    for (int i = 0; i < 1000; ++i) fx::update(w, 0.01f); // 10s simulated
    assert(w.particles.empty());
    printf("update: particles expired to 0\n");

    printf("fx_test: all checks passed\n");
    return 0;
}
