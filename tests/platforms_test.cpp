// Self-check for src/platforms/platforms.cpp — headless, no InitWindow.
// Build+run:
//   g++ -std=c++26 -Isrc $(pkg-config --cflags --libs raylib) \
//       tests/platforms_test.cpp src/platforms/platforms.cpp -o /tmp/platforms_test \
//       && /tmp/platforms_test
#include "core/api.hpp"
#include <cassert>
#include <cmath>

using namespace game;

static Player standing_player_at(Rectangle rect) {
    Player p;
    p.alive    = true;
    p.onGround = true;
    p.size     = { TILE * 0.8f, TILE * 1.4f };
    p.pos      = { rect.x + 24.0f, rect.y - p.size.y }; // feet resting on rect top, within x-range
    return p;
}

int main() {
    // ---- Breakable: trigger -> break -> respawn ----
    {
        World w;
        Platform& plat = w.platforms.emplace_back();
        plat.kind = PlatformKind::Breakable;
        plat.rect = { 0, 100, 72, 24 };
        w.players.push_back(standing_player_at(plat.rect));

        assert(plat.active && !plat.triggered);

        platforms::update(w, 0.1f); // first contact frame
        assert(plat.triggered);
        assert(plat.active); // hasn't had ~0.4s yet

        for (int i = 0; i < 10 && plat.active; ++i) platforms::update(w, 0.1f);
        assert(!plat.active);
        assert(plat.respawnTimer > 0.0f);

        for (int i = 0; i < 60 && !plat.active; ++i) platforms::update(w, 0.1f);
        assert(plat.active);
        assert(!plat.triggered);
        assert(plat.breakTimer == 0.0f);
        assert(plat.respawnTimer == 0.0f);
    }

    // ---- Moving: rect oscillates between a and b ----
    {
        World w;
        Platform& plat = w.platforms.emplace_back();
        plat.kind  = PlatformKind::Moving;
        plat.a     = { 0, 200 };
        plat.b     = { 240, 200 };
        plat.rect  = { plat.a.x, plat.a.y, 72, 24 };
        plat.speed = 120.0f;

        float maxX = plat.rect.x, minX = plat.rect.x;
        int   prevDir = plat.dir;
        bool  sawDirFlip = false;
        for (int i = 0; i < 2000; ++i) { // ~33s sim, several round trips at 120px/s over 240px
            platforms::update(w, 1.0f / 60.0f);
            maxX = std::fmax(maxX, plat.rect.x);
            minX = std::fmin(minX, plat.rect.x);
            assert(plat.rect.x >= plat.a.x - 0.01f && plat.rect.x <= plat.b.x + 0.01f);
            if (plat.dir != prevDir) { sawDirFlip = true; prevDir = plat.dir; }
        }
        assert(sawDirFlip);
        assert(maxX > plat.b.x - 1.0f);
        assert(minX < plat.a.x + 1.0f);
    }

    // ---- Moving: carries a rider standing on it ----
    {
        World w;
        Platform& plat = w.platforms.emplace_back();
        plat.kind  = PlatformKind::Moving;
        plat.a     = { 0, 200 };
        plat.b     = { 240, 200 };
        plat.rect  = { plat.a.x, plat.a.y, 72, 24 };
        plat.speed = 120.0f;
        w.players.push_back(standing_player_at(plat.rect));

        float startX = w.players[0].pos.x;
        float rectXBefore = plat.rect.x;
        platforms::update(w, 1.0f / 60.0f);
        float platDelta = plat.rect.x - rectXBefore;
        assert(platDelta > 0.0f); // sanity: platform actually moved this frame
        assert(std::fabs((w.players[0].pos.x - startX) - platDelta) < 0.01f);
    }

    return 0;
}
